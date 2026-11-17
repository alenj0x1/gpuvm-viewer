#include "bridge.h"
#include "sessionhooks.h"

#include "streaming/session.h"
#include "path.h"

#include <QJsonDocument>
#include <QSettings>
#include <QFile>
#include <QByteArray>
#include <QThread>
#include <QStringList>
#include <QCoreApplication>
#include <QMetaObject>
#include <QDir>
#include <QStandardPaths>

#include <SDL.h>

#include <cstdlib>

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// Token calls (lowerIntegrity), kept here so upstream's app.pro needs no change.
#pragma comment(lib, "advapi32.lib")
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
    static const char* known[] = { "inset", "capture", "fullscreen", "stats", "quit", "focus", "place", "reveal", "show", "hide" };
    for (const char* k : known) {
        if (type == QLatin1String(k)) {
            return true;
        }
    }
    return false;
}

// Commands that only set a state: before the stream window exists, a newer one replaces the older.
bool isStateCommand(const QString& type)
{
    return type == QLatin1String("place") || type == QLatin1String("inset") || type == QLatin1String("capture") ||
           type == QLatin1String("stats") || type == QLatin1String("focus");
}

// Messages to the app that only report the latest state: dropped first when the app reads too slowly.
bool isDroppable(const QString& type)
{
    return type == QLatin1String("stats") || type == QLatin1String("window");
}

constexpr int kMaxLine = 16 * 1024;
constexpr int kMaxPending = 32;
constexpr int kMaxStatsQueued = 64;
constexpr int kMaxQueued = 256;
constexpr int kHardMaxQueued = 1024;
constexpr unsigned long kDrainMs = 500;

int SDLCALL dropCommandEvent(void*, SDL_Event* event)
{
    if (event->type == SDL_USEREVENT && event->user.code == SDL_CODE_GPUVM_COMMAND) {
        delete (QJsonObject*)event->user.data1;
        return 0;
    }
    return 1;
}

}

Bridge& Bridge::instance()
{
    static Bridge bridge;
    return bridge;
}

QString Bridge::dataDirectory()
{
#ifdef Q_OS_WIN32
    // Same folder the GpuVM app prepares (and points TEMP at) for the viewer.
    QString profile = qEnvironmentVariable("USERPROFILE");
    if (profile.isEmpty()) {
        profile = QDir::homePath();
    }
    return QDir::cleanPath(profile + "/AppData/LocalLow/GpuVM/Viewer");
#else
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#endif
}

bool Bridge::lowerIntegrity()
{
#ifdef Q_OS_WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &token)) {
        return false;
    }
    bool ok = false;
    BYTE current[64];
    DWORD size = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, current, sizeof(current), &size)) {
        PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(current)->Label.Sid;
        DWORD level = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
        if (level <= SECURITY_MANDATORY_LOW_RID) {
            ok = true;
        }
        else {
            // S-1-16-4096. A process may always lower its own level (never raise it).
            SID_IDENTIFIER_AUTHORITY authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
            PSID low = nullptr;
            if (AllocateAndInitializeSid(&authority, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &low)) {
                TOKEN_MANDATORY_LABEL label = {};
                label.Label.Sid = low;
                label.Label.Attributes = SE_GROUP_INTEGRITY;
                ok = SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(low)) != FALSE;
                FreeSid(low);
            }
        }
    }
    CloseHandle(token);
    return ok;
#else
    return true;
#endif
}

void Bridge::prepareStorage()
{
    QString dir = dataDirectory();
    QString temp = dir + "/Temp";
    QDir().mkpath(temp);
#ifdef Q_OS_WIN32
    // Anything that writes temporary files (the app already points TEMP here when it starts a stream).
    std::wstring nativeTemp = QDir::toNativeSeparators(temp).toStdWString();
    _wputenv_s(L"TEMP", nativeTemp.c_str());
    _wputenv_s(L"TMP", nativeTemp.c_str());
    // Compiled QML is never cached on disk: the QML always comes from the install folder, which only administrators can write.
    _wputenv_s(L"QML_DISABLE_DISK_CACHE", L"1");
#endif

    // <dir>\GpuVM\Viewer.ini (organization\application), for the pairing run (Medium) and the streams (Low) alike.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, dir);
    Path::initializeIn(dir);

#ifdef Q_OS_WIN32
    // Earlier viewers kept settings and the pairing identity in the registry, which a Low process can read but
    // not write: copied over once so the machines stay paired.
    {
        QSettings settings;
        if (!QFile::exists(settings.fileName())) {
            QSettings old("HKEY_CURRENT_USER\\Software\\GpuVM\\Viewer", QSettings::NativeFormat);
            const QStringList keys = old.allKeys();
            for (const QString& key : keys) {
                settings.setValue(key, old.value(key));
            }
            settings.sync();
        }
    }
#endif

    // One log per run (and a dump per crash) is written there: keep the newest few.
    QDir logs(Path::getLogDir());
    const QFileInfoList files = logs.entryInfoList({ "Moonlight-*.log", "Moonlight-*.dmp" }, QDir::Files, QDir::Time);
    for (int i = 20; i < files.size(); i++) {
        QFile::remove(files[i].absoluteFilePath());
    }
}

bool Bridge::parseArguments(int& argc, char* argv[])
{
    QString tokenVar;
    int out = 1;
    for (int i = 1; i < argc; i++) {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--gpuvm-embed" && i + 1 < argc) {
            QStringList parts = QString::fromLocal8Bit(argv[++i]).split(',');
            bool ok = parts.size() == 5;
            qulonglong hwnd = ok ? parts[0].toULongLong(&ok) : 0;
            int v[4] = {};
            for (int k = 0; ok && k < 4; k++) {
                v[k] = parts[k + 1].toInt(&ok);
            }
            if (ok && hwnd != 0 && v[2] > 0 && v[3] > 0) {
                m_EmbedApp = (quintptr)hwnd;
                m_EmbedRect = { v[0], v[1], v[2], v[3] };
            }
            continue;
        }
        if (arg == "--gpuvm-quiet") {
            m_Quiet = true;
            continue;
        }
        if (arg == "--gpuvm-display" && i + 1 < argc) {
            QStringList parts = QString::fromLocal8Bit(argv[++i]).split(',');
            bool okX = false, okY = false;
            if (parts.size() == 2) {
                m_DisplayX = parts[0].toInt(&okX);
                m_DisplayY = parts[1].toInt(&okY);
            }
            m_HasDisplay = okX && okY;
            continue;
        }
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
    m_CommandLock = SDL_CreateMutex();

    m_Writer = QThread::create([this] { writeLoop(); });
    m_Writer->setObjectName("GpuVM bridge writer");
    m_Writer->start();

    QJsonObject hello;
    hello["token"] = m_Token;
    hello["pid"] = (qint64)GetCurrentProcessId();
    send("hello", hello);
    m_Token.fill('0');
    m_Token.clear();

    m_Reader = QThread::create([this] { readLoop(); });
    m_Reader->setObjectName("GpuVM bridge");
    m_Reader->start();
    return true;
#else
    return false;
#endif
}

void Bridge::stop()
{
#ifdef Q_OS_WIN32
    if (m_Pipe == nullptr || m_StopRequested.exchange(true)) {
        return;
    }
    HANDLE pipe = (HANDLE)m_Pipe;

    // The writer sends what's queued (the "ended" message) and stops; a write the app never reads is cancelled.
    SDL_LockMutex(m_WriteLock);
    SDL_CondSignal(m_WriteReady);
    SDL_UnlockMutex(m_WriteLock);
    if (!m_Writer->wait(kDrainMs)) {
        do {
            CancelIoEx(pipe, nullptr);
        } while (!m_Writer->wait(50));
    }

    // The reader may be about to start a read when the first cancel lands: cancel until it is gone.
    do {
        CancelIoEx(pipe, nullptr);
    } while (!m_Reader->wait(50));

    SDL_LockMutex(m_WriteLock);
    m_WriterDead = true;
    m_Outgoing.clear();
    m_Pipe = nullptr;
    SDL_UnlockMutex(m_WriteLock);
    CloseHandle(pipe);

    delete m_Writer;
    m_Writer = nullptr;
    delete m_Reader;
    m_Reader = nullptr;
#endif
}

void Bridge::send(const QString& type, const QJsonObject& fields)
{
#ifdef Q_OS_WIN32
    if (m_WriteLock == nullptr) {
        return;
    }
    QJsonObject message = fields;
    message["type"] = type;
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    line.append('\n');

    // Queued: callers include the video receive thread, which must never wait on the app.
    SDL_LockMutex(m_WriteLock);
    if (m_Pipe == nullptr || m_WriterDead) {
        SDL_UnlockMutex(m_WriteLock);
        return;
    }
    // Only the latest window state matters: one queued "window" at most.
    if (type == QLatin1String("window")) {
        for (Outgoing& queued : m_Outgoing) {
            if (queued.type == type) {
                queued.line = line;
                SDL_UnlockMutex(m_WriteLock);
                return;
            }
        }
    }
    bool keep = true;
    if (type == QLatin1String("stats")) {
        keep = m_Outgoing.size() < kMaxStatsQueued;
    }
    else if (m_Outgoing.size() >= kMaxQueued) {
        // The app reads too slowly: make room by dropping the oldest state report.
        bool dropped = false;
        for (int i = 0; i < m_Outgoing.size(); i++) {
            if (isDroppable(m_Outgoing[i].type)) {
                m_Outgoing.removeAt(i);
                dropped = true;
                break;
            }
        }
        keep = dropped || m_Outgoing.size() < kHardMaxQueued;
    }
    if (keep) {
        m_Outgoing.append({ type, line });
        SDL_CondSignal(m_WriteReady);
    }
    SDL_UnlockMutex(m_WriteLock);
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

void Bridge::sessionAttached()
{
    if (m_CommandLock == nullptr) {
        return;
    }
    SDL_LockMutex(m_CommandLock);
    if (m_Phase == Phase::NoSession) {
        m_Phase = Phase::Attached;
    }
    SDL_UnlockMutex(m_CommandLock);
}

void Bridge::sessionStarted()
{
    if (m_CommandLock == nullptr) {
        return;
    }
    // Under the lock, so commands arriving now queue up behind the replayed ones.
    SDL_LockMutex(m_CommandLock);
    m_Phase = Phase::Running;
    for (const QJsonObject& command : std::as_const(m_Pending)) {
        pushCommand(command);
    }
    m_Pending.clear();
    if (m_QuitRequested) {
        // The app asked to quit while connecting and the connection got through anyway: end it at once.
        QJsonObject quit;
        quit["type"] = "quit";
        pushCommand(quit);
    }
    SDL_UnlockMutex(m_CommandLock);
}

void Bridge::sessionEnded()
{
    if (m_CommandLock == nullptr) {
        return;
    }
    SDL_LockMutex(m_CommandLock);
    m_Phase = Phase::Ended;
    m_Pending.clear();
    SDL_UnlockMutex(m_CommandLock);

    // Nothing is pushed any more: free the commands still in SDL's queue.
    SDL_FilterEvents(dropCommandEvent, nullptr);
}

void Bridge::writeLoop()
{
#ifdef Q_OS_WIN32
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool failed = false;
    for (;;) {
        SDL_LockMutex(m_WriteLock);
        while (m_Outgoing.isEmpty() && !m_StopRequested) {
            SDL_CondWait(m_WriteReady, m_WriteLock);
        }
        if (m_Outgoing.isEmpty()) {
            SDL_UnlockMutex(m_WriteLock);
            break;
        }
        QByteArray line = m_Outgoing.takeFirst().line;
        SDL_UnlockMutex(m_WriteLock);

        DWORD written = 0;
        ResetEvent(ov.hEvent);
        if (!WriteFile((HANDLE)m_Pipe, line.constData(), (DWORD)line.size(), nullptr, &ov) &&
                GetLastError() != ERROR_IO_PENDING) {
            failed = true;
            break;
        }
        if (!GetOverlappedResult((HANDLE)m_Pipe, &ov, &written, TRUE)) {
            failed = true;
            break;
        }
    }
    CloseHandle(ov.hEvent);

    // Nothing more will be written: later messages are dropped instead of piling up.
    SDL_LockMutex(m_WriteLock);
    m_WriterDead = true;
    m_Outgoing.clear();
    SDL_UnlockMutex(m_WriteLock);

    if (failed && !m_StopRequested) {
        // The app can't be reached: stop the reader too, which ends the session (see readLoop).
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GpuVM: can't write to the app (%lu)", GetLastError());
        CancelIoEx((HANDLE)m_Pipe, nullptr);
    }
#endif
}

void Bridge::readLoop()
{
#ifdef Q_OS_WIN32
    QByteArray buffer;
    char chunk[4096];
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (!m_StopRequested) {
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

    // The app went away (or can't be written to): the viewer has no reason to keep streaming on its own (its
    // window would be left without the panel and the admin prompts), so it closes the session cleanly. Only
    // stop() ends this loop on purpose.
    if (!m_StopRequested) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GpuVM: the app closed the connection");
        QJsonObject quit;
        quit["type"] = "quit";
        dispatch(quit);
    }
#endif
}

void Bridge::pushCommand(const QJsonObject& message)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_GPUVM_COMMAND;
    event.user.data1 = new QJsonObject(message);
    if (SDL_PushEvent(&event) <= 0) {
        delete (QJsonObject*)event.user.data1;
    }
}

// Reader thread.
void Bridge::dispatch(const QJsonObject& message)
{
    QString type = message.value("type").toString();
    if (!isKnownCommand(type)) {
        return;
    }
    SDL_LockMutex(m_CommandLock);
    if (type == QLatin1String("quit")) {
        bool first = !m_QuitRequested.exchange(true);
        if (m_Phase == Phase::Running) {
            pushCommand(message);
        }
        else if (first && m_Phase != Phase::Ended && qApp != nullptr) {
            QMetaObject::invokeMethod(qApp, [this] { quitBeforeSession(); }, Qt::QueuedConnection);
        }
    }
    else if (m_Phase == Phase::Running) {
        pushCommand(message);
    }
    else if (m_Phase != Phase::Ended) {
        // Before the stream window exists: kept and replayed when it does.
        bool replaced = false;
        if (isStateCommand(type)) {
            for (QJsonObject& pending : m_Pending) {
                if (pending.value("type").toString() == type) {
                    pending = message;
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced && m_Pending.size() < kMaxPending) {
            m_Pending.append(message);
        }
    }
    SDL_UnlockMutex(m_CommandLock);
}

// Main thread: a quit that arrived before the stream window existed.
void Bridge::quitBeforeSession()
{
    SDL_LockMutex(m_CommandLock);
    Phase phase = m_Phase;
    SDL_UnlockMutex(m_CommandLock);

    switch (phase) {
    case Phase::NoSession:
        // Still looking for the machine or its app: nothing runs on the host yet.
        reportEnded("closed");
        stop();
        QCoreApplication::exit(0);
        break;
    case Phase::Attached:
        // Connecting: stop it. If the connection gets through anyway, sessionStarted() ends it at once; either way
        // the session's end reports "ended" and exits.
        if (Session::get() != nullptr) {
            Session::get()->interrupt();
        }
        break;
    case Phase::Running:
    case Phase::Ended:
        // The session loop got the quit / the viewer is already on its way out.
        break;
    }
}

}
