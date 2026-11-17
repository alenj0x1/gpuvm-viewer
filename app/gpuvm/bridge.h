// GpuVM viewer: link between this streaming window and the GpuVM app.
//
// The GpuVM app starts the viewer with --gpuvm-pipe <name> --gpuvm-token-env <var>. The app is the pipe
// server: it creates the pipe (current user only, local only, first instance) and accepts only the process it
// launched, so the viewer just connects as a client and proves itself with the token taken from its
// environment (never from the command line). Messages are JSON, one per line, from a fixed list in both
// directions; anything else is ignored.
//
// Without --gpuvm-pipe the viewer behaves like upstream Moonlight and none of this runs.

#pragma once

#include <QString>
#include <QJsonObject>
#include <QList>
#include <QByteArray>
#include <atomic>

#include <SDL.h>

class QThread;

namespace GpuVM {

// SDL user event code for a command from the app (data1 = new QJsonObject, owned by the receiver).
constexpr int SDL_CODE_GPUVM_COMMAND = 0x4750;  // 'GP'

class Bridge
{
public:
    static Bridge& instance();

    // Parses the GpuVM arguments; returns false if they're malformed. Removes them from argv.
    bool parseArguments(int& argc, char* argv[]);

    // A point on the display the stream window should open on (--gpuvm-display x,y), if GpuVM gave one.
    bool displayPoint(int& x, int& y) const { x = m_DisplayX; y = m_DisplayY; return m_HasDisplay; }

    // Inside the GpuVM app's window (--gpuvm-embed hwnd,x,y,w,h): a borderless window over the app's content
    // area, waiting just below the app until the app reveals it with the first picture.
    bool embed(quintptr& app, SDL_Rect& rect) const { app = m_EmbedApp; rect = m_EmbedRect; return m_EmbedApp != 0; }
    // The app's content area moved or changed size.
    void setEmbedRect(const SDL_Rect& rect) { m_EmbedRect = rect; }

    // GpuVM pairs the viewer with --gpuvm-quiet: no window, the result is the exit code.
    bool quiet() const { return m_Quiet || enabled(); }

    // True when the viewer was started by GpuVM.
    bool enabled() const { return !m_PipeName.isEmpty(); }

    // Where the viewer keeps its settings, pairing identity, caches and logs when GpuVM runs it (stream and
    // pair): %USERPROFILE%\AppData\LocalLow\GpuVM\Viewer, the one place a Low integrity process may write.
    static QString dataDirectory();

    // Points QSettings (an INI file there), Path's logs and caches and TEMP at dataDirectory(). Call before any
    // QSettings is created. Moves what earlier viewers kept in HKCU\Software\GpuVM\Viewer once.
    static void prepareStorage();

    // Connects and starts the reader and writer threads.
    bool start();
    // Sends what is queued (up to ~500 ms), then closes the pipe and waits for both threads. Main thread;
    // safe to call more than once. Call it before the process exits.
    void stop();

    // Viewer -> app. Queued; never blocks.
    void send(const QString& type, const QJsonObject& fields = QJsonObject());

    // Lets the app (and only the app) bring one of its windows to the front next, e.g. the panel.
    void allowAppForeground();

    // True once the app asked the viewer to quit (sticky: a quit is never lost).
    bool quitRequested() const { return m_QuitRequested; }

    // Session lifetime, from the main thread. Commands that arrive before the stream window exists (place, inset,
    // capture...) are kept and replayed when it does; a quit before that interrupts the connection.
    void sessionAttached();
    void sessionStarted();
    void sessionEnded();

private:
    Bridge() = default;
    void readLoop();
    void writeLoop();
    void dispatch(const QJsonObject& message);
    void pushCommand(const QJsonObject& message);
    void quitBeforeSession();

    enum class Phase { NoSession, Attached, Running, Ended };

    struct Outgoing {
        QString type;
        QByteArray line;
    };

    QString m_PipeName;
    bool m_HasDisplay = false;
    bool m_Quiet = false;
    quintptr m_EmbedApp = 0;
    SDL_Rect m_EmbedRect = {};
    int m_DisplayX = 0;
    int m_DisplayY = 0;
    QString m_Token;
    void* m_Pipe = nullptr;  // HANDLE
    QThread* m_Reader = nullptr;
    QThread* m_Writer = nullptr;

    // Outgoing queue (m_WriteLock).
    SDL_mutex* m_WriteLock = nullptr;
    SDL_cond* m_WriteReady = nullptr;
    QList<Outgoing> m_Outgoing;
    bool m_WriterDead = false;

    // Incoming commands (m_CommandLock).
    SDL_mutex* m_CommandLock = nullptr;
    Phase m_Phase = Phase::NoSession;
    QList<QJsonObject> m_Pending;

    std::atomic<bool> m_StopRequested{false};
    std::atomic<bool> m_QuitRequested{false};
};

}
