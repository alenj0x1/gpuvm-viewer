#include "sessionhooks.h"
#include "bridge.h"

#include "streaming/session.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <atomic>

#include <SDL_syswm.h>

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
