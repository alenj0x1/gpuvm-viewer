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

    // Connects and starts the reader thread. Commands are posted to SDL's queue while a stream is running and
    // kept until then otherwise.
    bool start();
    void stop();

    // Viewer -> app.
    void send(const QString& type, const QJsonObject& fields = QJsonObject());

    // Lets the app (and only the app) bring one of its windows to the front next, e.g. the panel.
    void allowAppForeground();

    // Called once per session from the SDL thread.
    void sessionStarted();
    void sessionEnded();

private:
    Bridge() = default;
    void readLoop();
    void writeLoop();
    void dispatch(const QJsonObject& message);

    QString m_PipeName;
    bool m_HasDisplay = false;
    bool m_Quiet = false;
    quintptr m_EmbedApp = 0;
    SDL_Rect m_EmbedRect = {};
    int m_DisplayX = 0;
    int m_DisplayY = 0;
    QString m_Token;
    void* m_Pipe = nullptr;  // HANDLE
    void* m_WriteLock = nullptr;  // SDL_mutex*
    void* m_WriteReady = nullptr;  // SDL_cond*
    QList<QByteArray> m_Outgoing;
    std::atomic<bool> m_InSession{false};
    std::atomic<bool> m_Stopping{false};
};

}
