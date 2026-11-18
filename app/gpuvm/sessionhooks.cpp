#include "sessionhooks.h"
#include "bridge.h"

#include "streaming/session.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <atomic>

#include <SDL_syswm.h>
#ifdef Q_OS_WIN32
#include <windows.h>
#include <dwmapi.h>
#include <ole2.h>
#pragma comment(lib, "ole32.lib")
#endif

namespace GpuVM {

namespace {

std::atomic<bool> s_FirstFrameSent{false};
std::atomic<bool> s_EndedSent{false};
std::atomic<bool> s_Failed{false};

}

void attachSession(Session* session)
{
    Bridge& bridge = Bridge::instance();
    if (!bridge.enabled()) {
        return;
    }
    s_FirstFrameSent = false;
    s_EndedSent = false;
    s_Failed = false;
    bridge.sessionAttached();

    // Stages and errors come from the connection thread; they are only queued for the app.
    QObject::connect(session, &Session::stageStarting, [](QString stage) {
        QJsonObject f;
        f["stage"] = stage;
        Bridge::instance().send("stage", f);
    });
    QObject::connect(session, &Session::stageFailed, [](QString stage, int errorCode, QString failingPorts) {
        s_Failed = true;
        QJsonObject f;
        f["stage"] = stage;
        f["code"] = errorCode;
        f["ports"] = failingPorts;
        Bridge::instance().send("stage-failed", f);
    });
    QObject::connect(session, &Session::connectionStarted, [] {
        Bridge::instance().send("connected");
    });
    QObject::connect(session, &Session::displayLaunchError, [](QString text) {
        s_Failed = true;
        QJsonObject f;
        f["message"] = text;
        Bridge::instance().send("error", f);
    });
    // On the main thread (qApp's), once the session is fully cleaned up. The Qt window stays hidden when GpuVM
    // drives the viewer, so nothing would ever close upstream's error dialogs: the app shows the error and the
    // viewer exits. A quit the app asked for is a normal end even if it cut a connection short.
    QObject::connect(session, &Session::readyForDeletion, qApp, [] {
        bool failed = s_Failed && !Bridge::instance().quitRequested();
        reportEnded(failed ? "error" : "closed");
        Bridge::instance().stop();
        QCoreApplication::exit(failed ? 1 : 0);
    }, Qt::QueuedConnection);
}

#ifdef Q_OS_WIN32
// Files dragged over the picture: the viewer only tells the app that a drag is over it ("drag", no fields) and
// never accepts the drop or reads what is dragged. The app then shows its own window over the picture, which
// takes the drop with the real paths from Windows (lab 043). So a viewer taken over by its streaming host can't
// name files of this PC for the app to copy: at most it can make the app show its drop area.
class DragNotifier final : public IDropTarget
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    // A single static object that lives as long as the process: no reference counting needed.
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL, DWORD* effect) override
    {
        Bridge::instance().send("drag");
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override
    {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL, DWORD* effect) override
    {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
};

static void notifyDrags(HWND hwnd)
{
    static DragNotifier s_Notifier;
    static HWND s_Registered = nullptr;
    if (hwnd == s_Registered) {
        return;
    }
    // Qt already initialized OLE on this (the main) thread; a second call only adds a reference.
    OleInitialize(nullptr);
    if (SUCCEEDED(RegisterDragDrop(hwnd, &s_Notifier))) {
        s_Registered = hwnd;
    }
}
#endif

void reportWindow(SDL_Window* window)
{
    Bridge& bridge = Bridge::instance();
    if (!bridge.enabled() || window == nullptr) {
        return;
    }
    QJsonObject f;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
        // As a number: HWNDs are 32-bit values even on 64-bit Windows, so a double holds them exactly.
        f["hwnd"] = (double)(quintptr)info.info.win.window;
#ifdef Q_OS_WIN32
        notifyDrags(info.info.win.window);
#endif
    }
    f["fullScreen"] = (SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
    Bridge::instance().send("window", f);
}

void reportFirstFrame()
{
    if (!Bridge::instance().enabled() || s_FirstFrameSent.exchange(true)) {
        return;
    }
    Bridge::instance().send("first-frame");
}

// The window hidden by cloakUntilNextFrame (s_Hidden), and the same once the old renderer is gone, so only a frame of
// the new one lifts it (s_Cloaked).
static std::atomic<quintptr> s_Hidden{0};
static std::atomic<quintptr> s_Cloaked{0};

void uncloakOnNextFrame()
{
    s_Cloaked = s_Hidden.load();
}

static void uncloak()
{
#ifdef Q_OS_WIN32
    s_Cloaked = 0;
    quintptr hwnd = s_Hidden.exchange(0);
    if (hwnd != 0) {
        BOOL off = FALSE;
        DwmSetWindowAttribute((HWND)hwnd, DWMWA_CLOAK, &off, sizeof(off));
    }
#endif
}

void cloakUntilNextFrame(SDL_Window* window)
{
#ifdef Q_OS_WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!Bridge::instance().enabled() || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
        return;
    }
    BOOL on = TRUE;
    if (FAILED(DwmSetWindowAttribute(info.info.win.window, DWMWA_CLOAK, &on, sizeof(on)))) {
        return;
    }
    s_Hidden = (quintptr)info.info.win.window;
    // Never left hidden: a frame that doesn't come (no IDR yet) lifts it after 1.5 s anyway.
    SDL_AddTimer(1500, [](Uint32, void*) -> Uint32 { uncloak(); return 0; }, nullptr);
#else
    Q_UNUSED(window);
#endif
}

void framePresented()
{
    if (s_Cloaked.load() != 0) {
        uncloak();
    }
    reportFirstFrame();
}

void reportStats(double receivedFps, double renderedFps, double networkDropPercent, double decodeMs,
                 double renderMs, double hostLatencyMs, double networkLatencyMs)
{
    if (!Bridge::instance().enabled()) {
        return;
    }
    QJsonObject f;
    f["receivedFps"] = receivedFps;
    f["renderedFps"] = renderedFps;
    f["networkDropPercent"] = networkDropPercent;
    f["decodeMs"] = decodeMs;
    f["renderMs"] = renderMs;
    f["hostLatencyMs"] = hostLatencyMs;
    f["networkLatencyMs"] = networkLatencyMs;
    Bridge::instance().send("stats", f);
}

void reportEnded(const char* reason)
{
    if (!Bridge::instance().enabled() || s_EndedSent.exchange(true)) {
        return;
    }
    QJsonObject f;
    f["reason"] = QString::fromUtf8(reason);
    Bridge::instance().send("ended", f);
}

}
