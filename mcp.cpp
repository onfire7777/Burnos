#include "mcp.h"

#include "livelog.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHostAddress>
#include <QMessageAuthenticationCode>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace BurnOS {

namespace {

// A Python server spends a few seconds importing before it answers anything.
constexpr int kStartTimeoutMs = 30000;
constexpr auto kProtocolVersion = "2024-11-05";
// Three deaths in five minutes and a server is left alone.
constexpr int kMaxDeaths = 3;
constexpr qint64 kDeathWindowMs = 5 * 60 * 1000;
// How soon a url that does not answer is tried again: every second while a
// process Burnos started is still coming up, every five seconds when it is
// somebody else's to host.
constexpr int kRetryOwnMs = 1000;
constexpr int kRetryHostedMs = 5000;
// After the first miss only every twelfth is logged - once a minute, not a
// line every five seconds for as long as the server is down.
constexpr int kLogEvery = 12;

// Where the memory server listens when Burnos hosts it itself: Mnemosyne's
// own default port, so a server the user starts by hand from the same folder
// is found at the same place.
constexpr auto kMemoryHost = "127.0.0.1";
constexpr auto kMemoryPort = "8765";

} // namespace

// ===========================================================================
// ONE SERVER
// ===========================================================================

McpServer::McpServer(const QString &name, const QString &url, const QString &command,
                     const QStringList &args, const QHash<QString, QString> &env,
                     const QString &cwd, QObject *parent)
    : QObject(parent)
    , m_name(name)
    , m_url(url)
    , m_command(command)
    , m_args(args)
    , m_env(env)
    , m_cwd(cwd)
{
    if (isHttp()) {
        m_net = new QNetworkAccessManager(this);
        // A localhost server is never behind the system proxy, and asking
        // Windows about one costs four seconds on the first request.
        const QString host = QUrl(m_url).host();
        if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0
            || QHostAddress(host).isLoopback())
            m_net->setProxy(QNetworkProxy::NoProxy);
        m_retry = new QTimer(this);
        m_retry->setSingleShot(true);
        connect(m_retry, &QTimer::timeout, this, &McpServer::handshake);
    }
}

McpServer::~McpServer()
{
    m_stopping = true;
    if (m_retry)
        m_retry->stop();
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(1000);
    }
#ifdef Q_OS_WIN
    // Closing the job takes every descendant with it - the interpreters a
    // launcher started, which kill() above never reaches.
    if (m_job)
        CloseHandle(static_cast<HANDLE>(m_job));
#endif
}

bool McpServer::hasTool(const QString &tool) const
{
    for (const Tool &t : m_tools) {
        if (t.name == tool)
            return true;
    }
    return false;
}

void McpServer::start()
{
    if (m_stopping)
        return;
    if (isHttp()) {
        // The url first. Nothing there -> stalled() starts the command, if
        // there is one, and keeps asking.
        m_retry->stop();
        m_ready = false;
        m_tools.clear();
        m_sessionId.clear();
        m_attempts = 0;
        handshake();
        return;
    }
    launch();
}

void McpServer::launch()
{
    if (m_proc)
        return;
    if (!isHttp()) {
        m_ready = false;
        m_buffer.clear();
        m_tools.clear();
    }

    m_proc = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // A Python server reads and writes the console's code page otherwise, and
    // what goes over the pipe is UTF-8 JSON. Harmless for anything else.
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    for (auto it = m_env.cbegin(); it != m_env.cend(); ++it)
        env.insert(it.key(), it.value());
    m_proc->setProcessEnvironment(env);
    if (!m_cwd.isEmpty())
        m_proc->setWorkingDirectory(m_cwd);

    // Into the job before it has had time to start anything of its own.
    connect(m_proc, &QProcess::started, this, &McpServer::adopt);
    if (isHttp()) {
        // Its stdout is not protocol here, only chatter for the log.
        connect(m_proc, &QProcess::readyReadStandardOutput, this, [this]() {
            const QString said = QString::fromUtf8(m_proc->readAllStandardOutput()).simplified();
            if (!said.isEmpty())
                liveLog(QStringLiteral("mcp %1 says: %2").arg(m_name, said.left(300)));
        });
    } else {
        connect(m_proc, &QProcess::started, this, &McpServer::handshake);
        connect(m_proc, &QProcess::readyReadStandardOutput, this, &McpServer::onOutput);
    }
    // Standard error is the server's own log, never protocol.
    connect(m_proc, &QProcess::readyReadStandardError, this, [this]() {
        const QString said = QString::fromUtf8(m_proc->readAllStandardError()).simplified();
        if (!said.isEmpty())
            liveLog(QStringLiteral("mcp %1 says: %2").arg(m_name, said.left(300)));
    });
    connect(m_proc, &QProcess::finished, this, &McpServer::onExit);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            liveLog(QStringLiteral("mcp %1: would not start: %2")
                        .arg(m_name, m_proc ? m_proc->errorString() : QString()));
            onExit();
        }
    });

    liveLog(QStringLiteral("mcp %1: starting %2").arg(m_name, QDir::toNativeSeparators(m_command)));
    m_proc->start(m_command, m_args);
}

void McpServer::adopt()
{
#ifdef Q_OS_WIN
    if (!m_proc)
        return;
    if (!m_job) {
        m_job = CreateJobObjectW(nullptr, nullptr);
        if (m_job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(static_cast<HANDLE>(m_job), JobObjectExtendedLimitInformation,
                                    &limits, sizeof(limits));
        }
    }
    if (!m_job)
        return;
    HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                 static_cast<DWORD>(m_proc->processId()));
    if (!process)
        return;
    if (!AssignProcessToJobObject(static_cast<HANDLE>(m_job), process)) {
        liveLog(QStringLiteral("mcp %1: could not adopt process %2 (error %3) - what it "
                               "starts may outlive Burnos")
                    .arg(m_name).arg(m_proc->processId()).arg(GetLastError()));
    }
    CloseHandle(process);
#endif
}

void McpServer::handshake()
{
    if (m_stopping)
        return;
    const QJsonObject hello{
        {QStringLiteral("protocolVersion"), QString::fromLatin1(kProtocolVersion)},
        {QStringLiteral("capabilities"), QJsonObject()},
        {QStringLiteral("clientInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Burnos")},
                                                    {QStringLiteral("version"), QStringLiteral("0.4")}}},
    };
    request(QStringLiteral("initialize"), hello, kStartTimeoutMs,
            [this](const QJsonObject &, const QString &error) {
        if (!error.isEmpty()) {
            stalled(QStringLiteral("no answer to initialize (%1)").arg(error));
            return;
        }
        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("method"), QStringLiteral("notifications/initialized")}});
        request(QStringLiteral("tools/list"), QJsonObject(), kStartTimeoutMs,
                [this](const QJsonObject &result, const QString &error) {
            if (!error.isEmpty()) {
                stalled(QStringLiteral("no tool list (%1)").arg(error));
                return;
            }
            m_tools.clear();
            for (const QJsonValue &value : result.value(QStringLiteral("tools")).toArray()) {
                const QJsonObject t = value.toObject();
                m_tools.append({t.value(QStringLiteral("name")).toString(),
                                t.value(QStringLiteral("description")).toString(),
                                t.value(QStringLiteral("inputSchema")).toObject()});
            }
            m_ready = true;
            m_attempts = 0;
            QString where;
            if (isHttp()) {
                where = QStringLiteral(" at %1 (%2)")
                            .arg(m_url, m_proc ? QStringLiteral("started by Burnos")
                                               : QStringLiteral("already hosted"));
            }
            liveLog(QStringLiteral("mcp %1: ready, %2 tool(s)%3").arg(m_name).arg(m_tools.size()).arg(where));
            emit readyChanged(true);
        });
    });
}

// The handshake got no answer. Over stdio the process is alive but mute, and
// there is nothing to do but say so - when it dies, onExit starts it again.
// Over HTTP nothing is listening yet: start the command if it is Burnos's to
// start, and ask again in a moment either way.
void McpServer::stalled(const QString &why)
{
    if (m_stopping)
        return;
    if (!isHttp()) {
        liveLog(QStringLiteral("mcp %1: %2").arg(m_name, why));
        return;
    }
    ++m_attempts;
    if (!m_command.isEmpty() && !m_proc && !m_givenUp) {
        liveLog(QStringLiteral("mcp %1: nothing answers at %2 - starting it").arg(m_name, m_url));
        launch();
    } else if (m_attempts == 1 || m_attempts % kLogEvery == 0) {
        liveLog(QStringLiteral("mcp %1: %2 - trying %3 again").arg(m_name, why, m_url));
    }
    m_retry->start(m_proc ? kRetryOwnMs : kRetryHostedMs);
}

void McpServer::request(const QString &method, const QJsonObject &params, int timeoutMs,
                        Reply then)
{
    if (!isHttp() && (!m_proc || m_proc->state() != QProcess::Running)) {
        QTimer::singleShot(0, this, [then]() { then({}, QStringLiteral("the server is not running")); });
        return;
    }
    const int id = ++m_nextId;
    Pending pending;
    pending.then = std::move(then);
    pending.timer = new QTimer(this);
    pending.timer->setSingleShot(true);
    connect(pending.timer, &QTimer::timeout, this, [this, id]() {
        auto it = m_pending.find(id);
        if (it == m_pending.end())
            return;
        const Pending late = it.value();
        m_pending.erase(it);
        late.timer->deleteLater();
        // The POST, if it is still open, finishes cancelled and is ignored.
        if (late.post)
            late.post->abort();
        late.then({}, QStringLiteral("no answer in time"));
    });
    pending.timer->start(timeoutMs);
    m_pending.insert(id, pending);
    const QJsonObject message{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                              {QStringLiteral("id"), id},
                              {QStringLiteral("method"), method},
                              {QStringLiteral("params"), params}};
    if (isHttp())
        post(message, id, timeoutMs);
    else
        send(message);
}

// A message that wants no answer: a notification, or an error back to the
// server. Over HTTP it is still a POST, answered with 202 and nothing else.
void McpServer::send(const QJsonObject &message)
{
    if (isHttp()) {
        post(message, -1, 10000);
        return;
    }
    if (m_proc)
        m_proc->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void McpServer::post(const QJsonObject &message, int id, int timeoutMs)
{
    QNetworkRequest req{QUrl(m_url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // Both, or a Streamable HTTP server refuses with 406: it may answer either way.
    req.setRawHeader("Accept", "application/json, text/event-stream");
    req.setRawHeader("MCP-Protocol-Version", kProtocolVersion);
    if (!m_sessionId.isEmpty())
        req.setRawHeader("Mcp-Session-Id", m_sessionId);
    // The pending timer answers first; this only makes sure the socket is let go.
    req.setTransferTimeout(timeoutMs + 1000);
    QNetworkReply *reply = m_net->post(req, QJsonDocument(message).toJson(QJsonDocument::Compact));
    if (id >= 0) {
        auto it = m_pending.find(id);
        if (it != m_pending.end())
            it->post = reply;
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, id]() {
        reply->deleteLater();
        onPosted(reply, id);
    });
}

void McpServer::onPosted(QNetworkReply *reply, int id)
{
    if (m_stopping)
        return;
    // Our own timeout cancelling it: already answered.
    if (reply->error() == QNetworkReply::OperationCanceledError)
        return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0) {
        // Nothing at the other end - refused, reset, gone. Everything in
        // flight fails; the handshake's error path decides what happens next,
        // and a server that WAS up is asked for again in a moment.
        const bool wasReady = m_ready;
        m_ready = false;
        m_sessionId.clear();
        if (wasReady) {
            liveLog(QStringLiteral("mcp %1: lost %2 (%3)").arg(m_name, m_url, reply->errorString()));
            emit readyChanged(false);
        }
        failAll(reply->errorString());
        if (wasReady) {
            m_attempts = 0;
            m_retry->start(m_proc ? kRetryOwnMs : kRetryHostedMs);
        }
        return;
    }
    const QByteArray session = reply->rawHeader("Mcp-Session-Id");
    if (!session.isEmpty())
        m_sessionId = session;
    const QByteArray body = reply->readAll();
    // A notification: 202 Accepted and nothing to route.
    if (id < 0)
        return;
    if (status >= 400) {
        answer(id, {}, QStringLiteral("HTTP %1: %2")
                           .arg(status).arg(QString::fromUtf8(body.left(200)).simplified()));
        return;
    }
    const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    if (type.contains(QLatin1String("text/event-stream"))) {
        // Server-sent events: the "data:" lines of each event, a blank line
        // between events, every event one JSON-RPC message.
        QByteArray data;
        for (const QByteArray &raw : body.split('\n')) {
            const QByteArray line = raw.trimmed();
            if (line.isEmpty()) {
                if (!data.isEmpty())
                    dispatch(QJsonDocument::fromJson(data).object());
                data.clear();
                continue;
            }
            if (line.startsWith("data:")) {
                if (!data.isEmpty())
                    data += '\n';
                data += line.mid(5).trimmed();
            }
        }
        if (!data.isEmpty())
            dispatch(QJsonDocument::fromJson(data).object());
    } else {
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isArray()) {
            for (const QJsonValue &value : doc.array())
                dispatch(value.toObject());
        } else {
            dispatch(doc.object());
        }
    }
    // Answered with nothing that carried our id: say so now rather than
    // leave it to the timer.
    if (m_pending.contains(id))
        answer(id, {}, QStringLiteral("HTTP %1 with no JSON-RPC answer in it").arg(status));
}

// One JSON-RPC message from the server, whichever way it came.
void McpServer::dispatch(const QJsonObject &message)
{
    if (message.isEmpty())
        return;
    // Something the SERVER asks of us. Burnos offers nothing back - no
    // roots, no sampling. Over stdio it is told so rather than left waiting;
    // over HTTP the ask rides inside a response stream and is simply dropped.
    if (message.contains(QStringLiteral("method"))) {
        if (!isHttp() && message.contains(QStringLiteral("id"))) {
            send(QJsonObject{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32601},
                                                      {QStringLiteral("message"),
                                                       QStringLiteral("not supported")}}}});
        }
        return;
    }
    const int id = message.value(QStringLiteral("id")).toInt(-1);
    if (message.contains(QStringLiteral("error"))) {
        answer(id, {}, message.value(QStringLiteral("error")).toObject()
                           .value(QStringLiteral("message")).toString());
    } else {
        answer(id, message.value(QStringLiteral("result")).toObject(), QString());
    }
}

void McpServer::answer(int id, const QJsonObject &result, const QString &error)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;
    const Pending answered = it.value();
    m_pending.erase(it);
    answered.timer->stop();
    answered.timer->deleteLater();
    answered.then(result, error);
}

void McpServer::onOutput()
{
    if (!m_proc)
        return;
    m_buffer.append(m_proc->readAllStandardOutput());
    qsizetype newline = 0;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newline).trimmed();
        m_buffer.remove(0, newline + 1);
        if (line.isEmpty())
            continue;
        dispatch(QJsonDocument::fromJson(line).object());
    }
}

void McpServer::failAll(const QString &why)
{
    const QHash<int, Pending> waiting = m_pending;
    m_pending.clear();
    for (const Pending &p : waiting) {
        p.timer->stop();
        p.timer->deleteLater();
        p.then({}, why);
    }
}

void McpServer::onExit()
{
    if (!m_proc)
        return;
    const bool wasReady = m_ready;
    m_ready = false;
    m_proc->disconnect(this);
    m_proc->deleteLater();
    m_proc = nullptr;
    failAll(QStringLiteral("%1 stopped").arg(m_name));
    if (wasReady)
        emit readyChanged(false);
    if (m_stopping)
        return;

    // STARTED AGAIN, unless it keeps dying: a broken server restarted for
    // ever is a process storm, and the log says it was given up on. An HTTP
    // url is still watched, in case somebody else hosts it.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_deaths.append(now);
    while (!m_deaths.isEmpty() && now - m_deaths.first() > kDeathWindowMs)
        m_deaths.removeFirst();
    if (m_deaths.size() > kMaxDeaths) {
        m_givenUp = true;
        liveLog(QStringLiteral("mcp %1: stopped for good after dying %2 times in five minutes%3")
                    .arg(m_name).arg(m_deaths.size())
                    .arg(isHttp() ? QStringLiteral(" - still watching %1").arg(m_url) : QString()));
        if (isHttp())
            m_retry->start(kRetryHostedMs);
        return;
    }
    liveLog(QStringLiteral("mcp %1: stopped - starting it again").arg(m_name));
    if (isHttp()) {
        // The url first, as always: the command only when nothing answers.
        m_attempts = 0;
        m_retry->start(2000);
    } else {
        QTimer::singleShot(2000, this, &McpServer::start);
    }
}

void McpServer::call(const QString &tool, const QJsonObject &arguments, Done done, int timeoutMs)
{
    if (!m_ready) {
        const QString why = QStringLiteral("%1 is not ready").arg(m_name);
        QTimer::singleShot(0, this, [done, why]() { done(false, {}, why); });
        return;
    }
    request(QStringLiteral("tools/call"),
            QJsonObject{{QStringLiteral("name"), tool}, {QStringLiteral("arguments"), arguments}},
            timeoutMs, [done](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty()) {
            done(false, {}, error);
            return;
        }
        done(!result.value(QStringLiteral("isError")).toBool(), result, QString());
    });
}

QString McpServer::textOf(const QJsonObject &result)
{
    QStringList parts;
    for (const QJsonValue &value : result.value(QStringLiteral("content")).toArray()) {
        const QJsonObject part = value.toObject();
        if (part.value(QStringLiteral("type")).toString() == QLatin1String("text"))
            parts.append(part.value(QStringLiteral("text")).toString());
    }
    if (parts.isEmpty() && result.contains(QStringLiteral("structuredContent"))) {
        parts.append(QString::fromUtf8(QJsonDocument(result.value(QStringLiteral("structuredContent"))
                                                         .toObject())
                                           .toJson(QJsonDocument::Compact)));
    }
    return parts.join(QLatin1Char('\n'));
}

// ===========================================================================
// EVERY SERVER
// ===========================================================================

McpClient::McpClient(QObject *parent)
    : QObject(parent)
{
}

QString McpClient::configPath()
{
    // BURNOS_MCP_CONFIG names another list: a test run talks to a twin of
    // the memory server on a scratch store, never to the user's own.
    const QString other = qEnvironmentVariable("BURNOS_MCP_CONFIG");
    if (!other.isEmpty())
        return QDir::fromNativeSeparators(other);
    return QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA"))
           + QStringLiteral("/BurnOS/mcp.json");
}

QString McpClient::mnemosyneDir()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/mnemosyne");
}

QString McpClient::mnemosyneExe()
{
    const QString dir = mnemosyneDir();
    // A standalone build first, then the launcher inside a Python venv.
    for (const QString &candidate : {dir + QStringLiteral("/mneme-mcp.exe"),
                                     dir + QStringLiteral("/.venv/Scripts/mneme-mcp.exe")}) {
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return QString();
}

QString McpClient::memoryStore()
{
    return QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"))
           + QStringLiteral("/BurnOS/memory");
}

QString McpClient::sessionSecret()
{
    static QString cached;
    if (!cached.isEmpty())
        return cached;

    const QString path = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"))
                         + QStringLiteral("/BurnOS/memory.secret");
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        cached = QString::fromUtf8(file.readAll()).trimmed();
        file.close();
        if (!cached.isEmpty())
            return cached;
    }

    // None yet: 256 bits from the system's generator, kept as hex.
    quint32 words[8];
    QRandomGenerator::system()->fillRange(words);
    cached = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char *>(words), int(sizeof words)).toHex());
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(cached.toUtf8());
        file.close();
        liveLog(QStringLiteral("mcp: made the memory session secret at %1")
                    .arg(QDir::toNativeSeparators(path)));
    } else {
        liveLog(QStringLiteral("mcp: could not write %1 (%2) - the secret lasts this run only")
                    .arg(QDir::toNativeSeparators(path), file.errorString()));
    }
    return cached;
}

QString McpClient::sessionToken(const QString &tenant, const QString &user,
                                const QString &agent, const QString &session,
                                int ttlSeconds)
{
    const QJsonObject claims{{QStringLiteral("tenant_id"), tenant},
                             {QStringLiteral("user_id"), user},
                             {QStringLiteral("role"), QStringLiteral("agent")},
                             {QStringLiteral("source_trust_tier"), 0},
                             {QStringLiteral("agent_id"), agent},
                             {QStringLiteral("session_id"), session},
                             {QStringLiteral("exp"),
                              QDateTime::currentSecsSinceEpoch() + ttlSeconds}};
    const QByteArray payload = QJsonDocument(claims)
                                   .toJson(QJsonDocument::Compact)
                                   .toBase64(QByteArray::Base64UrlEncoding
                                             | QByteArray::OmitTrailingEquals);
    const QByteArray signature = QMessageAuthenticationCode::hash(payload, sessionSecret().toUtf8(),
                                                                  QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding
                                               | QByteArray::OmitTrailingEquals);
    return QString::fromLatin1(payload + '.' + signature);
}

namespace {

// The memory server as the mnemosyne folder beside Burnos.exe would list it:
// Mnemosyne over Streamable HTTP on localhost, reading the canonical store
// (memoryStore) - the same one a server hosted by hand reads, so it makes no
// difference to the memories who started it. Nothing, when the folder holds
// no server.
QJsonObject builtInMemory()
{
    const QString exe = McpClient::mnemosyneExe();
    if (exe.isEmpty())
        return {};
    const QString host = QString::fromLatin1(kMemoryHost);
    const QString port = QString::fromLatin1(kMemoryPort);
    const QString dir = QDir::toNativeSeparators(McpClient::mnemosyneDir());
    return QJsonObject{
        {QStringLiteral("url"), QStringLiteral("http://%1:%2/mcp").arg(host, port)},
        {QStringLiteral("command"), QDir::toNativeSeparators(exe)},
        {QStringLiteral("args"), QJsonArray{QStringLiteral("--sdk-streamable-http"),
                                            QStringLiteral("--http-host"), host,
                                            QStringLiteral("--http-port"), port,
                                            QStringLiteral("--backend"), QStringLiteral("sqlite"),
                                            QStringLiteral("--store"), QDir::toNativeSeparators(McpClient::memoryStore())}},
        // So its working memory answers the session tokens Burnos signs.
        {QStringLiteral("env"), QJsonObject{{QStringLiteral("MNEMOSYNE_MCP_SESSION_SECRET"),
                                             McpClient::sessionSecret()}}},
        {QStringLiteral("cwd"), dir}};
}

} // namespace

void McpClient::load()
{
    const QString path = configPath();
    QJsonObject listed;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonParseError parse{};
        const QJsonObject root = QJsonDocument::fromJson(file.readAll(), &parse).object();
        if (parse.error != QJsonParseError::NoError) {
            liveLog(QStringLiteral("mcp: %1 is not valid JSON (%2)")
                        .arg(QDir::toNativeSeparators(path), parse.errorString()));
            return;
        }
        listed = root.value(QStringLiteral("mcpServers")).toObject();
    } else {
        // Nothing listed: the memory server in the folder beside Burnos.exe,
        // if it is there.
        const QJsonObject memory = builtInMemory();
        if (memory.isEmpty()) {
            liveLog(QStringLiteral("mcp: no servers listed - %1 is not there and %2 holds no server")
                        .arg(QDir::toNativeSeparators(path), QDir::toNativeSeparators(mnemosyneDir())));
            return;
        }
        liveLog(QStringLiteral("mcp: %1 is not there - memory is the server in %2")
                    .arg(QDir::toNativeSeparators(path), QDir::toNativeSeparators(mnemosyneDir())));
        listed.insert(QStringLiteral("memory"), memory);
    }

    for (auto it = listed.begin(); it != listed.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        if (entry.value(QStringLiteral("disabled")).toBool())
            continue;
        const QString url = entry.value(QStringLiteral("url")).toString();
        const QString command = entry.value(QStringLiteral("command")).toString();
        if (url.isEmpty() && command.isEmpty()) {
            liveLog(QStringLiteral("mcp: %1 has neither url nor command, left out").arg(it.key()));
            continue;
        }
        QStringList args;
        for (const QJsonValue &arg : entry.value(QStringLiteral("args")).toArray())
            args.append(arg.toString());
        QHash<QString, QString> env;
        const QJsonObject vars = entry.value(QStringLiteral("env")).toObject();
        for (auto v = vars.begin(); v != vars.end(); ++v)
            env.insert(v.key(), v.value().toString());
        // The memory server, however it was listed, gets the session secret
        // Burnos signs with - or its working memory answers nobody.
        const bool memoryServer =
            it.key().compare(QLatin1String("memory"), Qt::CaseInsensitive) == 0
            || command.contains(QLatin1String("mneme-mcp"), Qt::CaseInsensitive);
        if (memoryServer && !env.contains(QStringLiteral("MNEMOSYNE_MCP_SESSION_SECRET")))
            env.insert(QStringLiteral("MNEMOSYNE_MCP_SESSION_SECRET"), sessionSecret());

        auto *server = new McpServer(it.key(), url, command, args, env,
                                     entry.value(QStringLiteral("cwd")).toString(), this);
        connect(server, &McpServer::readyChanged, this,
                [this]() { emit memoryChanged(memory() != nullptr); });
        m_servers.append(server);
        server->start();
    }
    liveLog(QStringLiteral("mcp: %1 server(s) listed").arg(m_servers.size()));
}

McpServer *McpClient::server(const QString &name) const
{
    for (McpServer *s : m_servers) {
        if (s->name().compare(name, Qt::CaseInsensitive) == 0)
            return s;
    }
    return nullptr;
}

QStringList McpClient::readyNames() const
{
    QStringList names;
    for (McpServer *s : m_servers) {
        if (s->isReady())
            names.append(s->name());
    }
    return names;
}

McpServer *McpClient::memory() const
{
    for (McpServer *s : m_servers) {
        if (s->isReady() && s->hasTool(QStringLiteral("capture"))
            && s->hasTool(QStringLiteral("search")))
            return s;
    }
    return nullptr;
}

QString McpClient::describeTools(const QString &serverName, const QString &query) const
{
    const QStringList words = query.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList lines;
    int total = 0;
    for (McpServer *s : m_servers) {
        if (!s->isReady())
            continue;
        if (!serverName.isEmpty() && s->name().compare(serverName, Qt::CaseInsensitive) != 0)
            continue;
        for (const McpServer::Tool &t : s->tools()) {
            const QString haystack = (t.name + QLatin1Char(' ') + t.description).toLower();
            bool matches = words.isEmpty();
            for (const QString &w : words) {
                if (haystack.contains(w)) {
                    matches = true;
                    break;
                }
            }
            if (!matches)
                continue;
            ++total;
            if (lines.size() >= 25)
                continue;
            // The arguments, the required ones marked, so a call can be
            // written without guessing.
            const QJsonObject props = t.schema.value(QStringLiteral("properties")).toObject();
            const QJsonArray required = t.schema.value(QStringLiteral("required")).toArray();
            QStringList args;
            for (auto p = props.begin(); p != props.end(); ++p) {
                const bool needed = required.contains(p.key());
                args.append(needed ? p.key() + QStringLiteral(" (required)") : p.key());
            }
            lines.append(QStringLiteral("%1 / %2: %3 Arguments: %4.")
                             .arg(s->name(), t.name, t.description.simplified().left(200),
                                  args.isEmpty() ? QStringLiteral("none")
                                                 : args.join(QStringLiteral(", "))));
        }
    }
    if (lines.isEmpty())
        return QStringLiteral("No tool on a ready server matches that. Ready servers: %1.")
            .arg(readyNames().isEmpty() ? QStringLiteral("none") : readyNames().join(QStringLiteral(", ")));
    QString out = lines.join(QLatin1Char('\n'));
    if (total > lines.size())
        out += QStringLiteral("\n... and %1 more; search with more specific words.").arg(total - lines.size());
    return out;
}

} // namespace BurnOS
