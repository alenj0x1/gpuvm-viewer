#include "bridge.h"

#include <QJsonDocument>
#include <QByteArray>
#include <QThread>
#include <QCoreApplication>
#include <QMetaObject>

#include <SDL.h>

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace GpuVM {

namespace {

// Pipe names GpuVM creates: \\.\pipe\gpuvm-viewer-<32 hex>. Anything else is refused, so the argument can't
// point the viewer at another program's pipe.
bool isValidPipeName(const QString& name)
{
    static const QString prefix = QStringLiteral("\\\\.\\pipe\\gpuvm-viewer-");
    if (!name.startsWith(prefix) || name.length() != prefix.length() + 32) {
        return false;
    }
    for (QChar c : name.mid(prefix.length())) {
        if (!c.isDigit() && !(c >= 'a' && c <= 'f')) {
            return false;
        }
    }
    return true;
}

// Commands the app may send. Each is checked again where it is handled.
bool isKnownCommand(const QString& type)
{
    static const char* known[] = { "inset", "capture", "fullscreen", "stats", "quit", "focus" };
    for (const char* k : known) {
        if (type == QLatin1String(k)) {
            return true;
        }
    }
    return false;
}

constexpr int kMaxLine = 16 * 1024;

}

Bridge& Bridge::instance()
{
    static Bridge bridge;
    return bridge;
}

bool Bridge::parseArguments(int& argc, char* argv[])
{
    QString tokenVar;
    int out = 1;
    for (int i = 1; i < argc; i++) {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if ((arg == "--gpuvm-pipe" || arg == "--gpuvm-token-env") && i + 1 < argc) {
            QString value = QString::fromLocal8Bit(argv[++i]);
            if (arg == "--gpuvm-pipe") {
                m_PipeName = value;
            }
            else {
                tokenVar = value;
            }
            continue;
        }
        argv[out++] = argv[i];
    }
    argc = out;
    argv[argc] = nullptr;

    if (m_PipeName.isEmpty()) {
        return tokenVar.isEmpty();
    }
    if (!isValidPipeName(m_PipeName) || !tokenVar.startsWith("GPUVM_")) {
        m_PipeName.clear();
        return false;
    }
    m_Token = qEnvironmentVariable(tokenVar.toUtf8().constData());
    // The token is only for this handshake: children of the viewer don't inherit it.
    qunsetenv(tokenVar.toUtf8().constData());
    if (m_Token.length() < 32) {
        m_PipeName.clear();
        return false;
    }
    return true;
}

bool Bridge::start()
{
#ifdef Q_OS_WIN32
    if (!enabled()) {
        return false;
    }
    std::wstring name = m_PipeName.toStdWString();
    // SECURITY_IDENTIFICATION: the app can't act as this user through the pipe beyond what it already is.
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 50 && pipe == INVALID_HANDLE_VALUE; attempt++) {
        // Overlapped: reads and writes on one synchronous handle would wait for each other.
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(name.c_str(), 100);
            }
            else {
                Sleep(100);
            }
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "GpuVM: can't connect to the app (%lu)", GetLastError());
        return false;
    }

    // The app creates the pipe (first instance only, this user, local only) before it starts the viewer, so
    // nobody else can be listening on this random name.
    m_Pipe = pipe;
    m_WriteLock = SDL_CreateMutex();
    m_WriteReady = SDL_CreateCond();

    QThread* writer = QThread::create([this] { writeLoop(); });
    writer->setObjectName("GpuVM bridge writer");
    writer->start();

    QJsonObject hello;
    hello["token"] = m_Token;
    hello["pid"] = (qint64)GetCurrentProcessId();
    send("hello", hello);
    m_Token.fill('0');
    m_Token.clear();

    QThread* reader = QThread::create([this] { readLoop(); });
    reader->setObjectName("GpuVM bridge");
    reader->start();
    return true;
#else
    return false;
#endif
}

void Bridge::stop()
{
#ifdef Q_OS_WIN32
    if (m_Pipe == nullptr) {
        return;
    }
    // Let the writer send what's queued (the "ended" message) before the reader is cancelled.
    SDL_LockMutex((SDL_mutex*)m_WriteLock);
    m_Stopping = true;
    SDL_CondSignal((SDL_cond*)m_WriteReady);
    SDL_UnlockMutex((SDL_mutex*)m_WriteLock);
#endif
}

void Bridge::send(const QString& type, const QJsonObject& fields)
{
#ifdef Q_OS_WIN32
    if (m_Pipe == nullptr) {
        return;
    }
    QJsonObject message = fields;
    message["type"] = type;
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    line.append('\n');

    // Queued: callers include the video receive thread, which must never wait on the app.
    SDL_LockMutex((SDL_mutex*)m_WriteLock);
    if (m_Outgoing.size() < 64 || type != QLatin1String("stats")) {
        m_Outgoing.append(line);
        SDL_CondSignal((SDL_cond*)m_WriteReady);
    }
    SDL_UnlockMutex((SDL_mutex*)m_WriteLock);
#else
    Q_UNUSED(type);
    Q_UNUSED(fields);
#endif
}

void Bridge::allowAppForeground()
{
#ifdef Q_OS_WIN32
    ULONG serverPid = 0;
    if (m_Pipe != nullptr && GetNamedPipeServerProcessId((HANDLE)m_Pipe, &serverPid) && serverPid != 0) {
        AllowSetForegroundWindow(serverPid);
    }
#endif
}

void Bridge::sessionStarted()
{
    m_InSession = true;
}

void Bridge::sessionEnded()
{
    m_InSession = false;
}

void Bridge::writeLoop()
{
#ifdef Q_OS_WIN32
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (;;) {
        SDL_LockMutex((SDL_mutex*)m_WriteLock);
        while (m_Outgoing.isEmpty() && !m_Stopping) {
            SDL_CondWait((SDL_cond*)m_WriteReady, (SDL_mutex*)m_WriteLock);
        }
        if (m_Outgoing.isEmpty()) {
            SDL_UnlockMutex((SDL_mutex*)m_WriteLock);
            break;
        }
        QByteArray line = m_Outgoing.takeFirst();
        SDL_UnlockMutex((SDL_mutex*)m_WriteLock);

        DWORD written = 0;
        ResetEvent(ov.hEvent);
        if (!WriteFile((HANDLE)m_Pipe, line.constData(), (DWORD)line.size(), nullptr, &ov) &&
                GetLastError() != ERROR_IO_PENDING) {
            break;
        }
        if (!GetOverlappedResult((HANDLE)m_Pipe, &ov, &written, TRUE)) {
            break;
        }
    }
    CloseHandle(ov.hEvent);
    // Nothing more will be written: stop the reader too.
    m_Stopping = true;
    CancelIoEx((HANDLE)m_Pipe, nullptr);
#endif
}

void Bridge::readLoop()
{
#ifdef Q_OS_WIN32
    QByteArray buffer;
    char chunk[4096];
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (!m_Stopping) {
        DWORD read = 0;
        ResetEvent(ov.hEvent);
        if (!ReadFile((HANDLE)m_Pipe, chunk, sizeof(chunk), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
            break;
        }
        if (!GetOverlappedResult((HANDLE)m_Pipe, &ov, &read, TRUE) || read == 0) {
            break;
        }
        buffer.append(chunk, (int)read);
        if (buffer.size() > kMaxLine && buffer.indexOf('\n') < 0) {
            // A line this long is never something GpuVM sends: drop the connection.
            break;
        }
        int newline;
        while ((newline = buffer.indexOf('\n')) >= 0) {
            QByteArray line = buffer.left(newline);
            buffer.remove(0, newline + 1);
            QJsonParseError error;
            QJsonDocument doc = QJsonDocument::fromJson(line, &error);
            if (error.error == QJsonParseError::NoError && doc.isObject()) {
                dispatch(doc.object());
            }
        }
    }

    CloseHandle(ov.hEvent);

    // The app went away: the viewer has no reason to keep streaming on its own (its window would be left
    // without the panel and the admin prompts), so it closes the session cleanly.
    if (!m_Stopping) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GpuVM: the app closed the connection");
        QJsonObject quit;
        quit["type"] = "quit";
        dispatch(quit);
    }
#endif
}

void Bridge::dispatch(const QJsonObject& message)
{
    QString type = message.value("type").toString();
    if (!isKnownCommand(type)) {
        return;
    }
    if (!m_InSession) {
        // Before the stream window exists (connecting) only "quit" means something: leave at once.
        if (type == "quit") {
            QMetaObject::invokeMethod(qApp, [] { QCoreApplication::exit(0); }, Qt::QueuedConnection);
        }
        return;
    }
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_GPUVM_COMMAND;
    event.user.data1 = new QJsonObject(message);
    if (SDL_PushEvent(&event) <= 0) {
        delete (QJsonObject*)event.user.data1;
    }
}

}
