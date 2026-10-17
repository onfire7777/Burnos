#include "mcp.h"

#include "livelog.h"
#include "mcpauth.h"
#include "settings.h"

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
// The newest version, spoken statelessly, and the one asked for of a server
// that still wants the initialize handshake.
constexpr auto kLatestVersion = "2026-07-28";
constexpr auto kLegacyVersion = "2025-11-25";
// How long a call waits on the user answering a server's question.
constexpr int kAskTimeoutMs = 15 * 60 * 1000;
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

// The memory server, whatever mcp.json called it.
bool isMemoryServer(const QString &name, const QString &command)
{
    return name.compare(QLatin1String("memory"), Qt::CaseInsensitive) == 0
           || command.contains(QLatin1String("mneme-mcp"), Qt::CaseInsensitive);
}

// MEMORIES FOUND BY MEANING. A memory server Burnos starts embeds every line
// it stores, and every question, with qwen3-embedding-8b over OpenRouter - on
// the key from Settings, the one the talking model already uses. It has no
// setting of its own: with a key it is on, without one the memory searches by
// words as it did before 0.17. The model and the instruction are the ones the
// LongMemEval run was made with; the provider named first is the faster of the
// two measured. Every line and every question goes to OpenRouter for this.
QHash<QString, QString> embeddingEnv()
{
    const QString key = Settings::openRouterKey();
    if (key.isEmpty())
        return {};
    const QString index = QFileInfo(McpClient::memoryStore()).absolutePath()
                          + QStringLiteral("/embedding-index.sqlite");
    return {
        {QStringLiteral("OPENROUTER_API_KEY"), key},
        {QStringLiteral("MNEMOSYNE_PASSAGE_EMBEDDING_API_KEY_ENV"), QStringLiteral("OPENROUTER_API_KEY")},
        {QStringLiteral("MNEMOSYNE_PASSAGE_INDEX"), QDir::toNativeSeparators(index)},
        {QStringLiteral("MNEMOSYNE_PASSAGE_EMBEDDING_URL"),
         QStringLiteral("https://openrouter.ai/api/v1/embeddings")},
        {QStringLiteral("MNEMOSYNE_PASSAGE_EMBEDDING_MODEL"), QStringLiteral("qwen/qwen3-embedding-8b")},
        {QStringLiteral("MNEMOSYNE_PASSAGE_EMBEDDING_DIMS"), QStringLiteral("1024")},
        {QStringLiteral("MNEMOSYNE_PASSAGE_EMBEDDING_BODY"),
         QStringLiteral("{\"provider\":{\"order\":[\"DeepInfra\",\"Nebius\"]}}")},
        {QStringLiteral("MNEMOSYNE_PASSAGE_QUERY_TASK"),
         QStringLiteral("Given a question a user asks about their past conversations with an "
                        "assistant, retrieve the conversation that contains the answer")},
        // Burnos waits three seconds for its memory each turn. A question whose
        // embedding has not come back by now is searched by words instead.
        {QStringLiteral("MNEMOSYNE_PASSAGE_QUERY_TIMEOUT"), QStringLiteral("1.5")},
    };
}

QJsonObject clientInfo()
{
    return {{QStringLiteral("name"), QStringLiteral("BurnOS")},
            {QStringLiteral("version"), QCoreApplication::applicationVersion()}};
}

// What BurnOS does for a server: ask the user (elicitation, a form or a page
// to open). Roots and sampling are deprecated and not offered.
QJsonObject clientCapabilities()
{
    return {{QStringLiteral("elicitation"),
             QJsonObject{{QStringLiteral("form"), QJsonObject()}, {QStringLiteral("url"), QJsonObject()}}}};
}

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
        m_local = host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0
                  || QHostAddress(host).isLoopback();
        if (m_local)
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

bool McpServer::takes(const QString &tool, const QString &argument) const
{
    for (const Tool &t : m_tools) {
        if (t.name == tool)
            return t.schema.value(QStringLiteral("properties")).toObject().contains(argument);
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

void McpServer::startAgain()
{
    if (!m_proc)
        return;
    // Asked for, not a crash: it must not count towards giving the server up.
    m_deaths.clear();
    m_proc->kill();
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
    // Read now, not when the list was loaded: a memory server started again
    // after the key changed embeds on the new one. An mcp.json that sets up a
    // passage index of its own is left exactly as written.
    if (isMemoryServer(m_name, m_command) && !m_env.contains(QStringLiteral("MNEMOSYNE_PASSAGE_INDEX"))) {
        const QHash<QString, QString> meaning = embeddingEnv();
        for (auto it = meaning.cbegin(); it != meaning.cend(); ++it)
            env.insert(it.key(), it.value());
    }
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

// Over HTTP the newest way is asked first: a 2026-07-28 server answers
// server/discover and needs no handshake, an older one refuses it at once. A
// stdio server is asked the old way first, because an older one may not answer
// anything before initialize at all.
void McpServer::handshake()
{
    if (m_stopping)
        return;
    if (isHttp())
        discover(true);
    else
        initialize(true);
}

void McpServer::discover(bool fallBack)
{
    m_stateless = true;
    m_version = QString::fromLatin1(kLatestVersion);
    request(QStringLiteral("server/discover"), QJsonObject(), kStartTimeoutMs,
            [this, fallBack](const QJsonObject &found, const QString &error) {
        if (m_needsSignIn || m_refreshing)
            return;
        if (error.isEmpty()
            && found.value(QStringLiteral("supportedVersions")).toArray().contains(QString::fromLatin1(kLatestVersion))) {
            const QJsonObject info = found.value(QStringLiteral("_meta")).toObject()
                                         .value(QStringLiteral("io.modelcontextprotocol/serverInfo")).toObject();
            m_build = (info.value(QStringLiteral("name")).toString() + QLatin1Char(' ')
                       + info.value(QStringLiteral("version")).toString()).trimmed();
            listTools();
            return;
        }
        if (fallBack && !m_down)
            initialize(false);
        else
            stalled(QStringLiteral("no answer to server/discover (%1)").arg(error));
    });
}

void McpServer::initialize(bool fallBack)
{
    m_stateless = false;
    m_version = QString::fromLatin1(kLegacyVersion);
    const QJsonObject hello{
        {QStringLiteral("protocolVersion"), m_version},
        {QStringLiteral("capabilities"), clientCapabilities()},
        {QStringLiteral("clientInfo"), clientInfo()},
    };
    request(QStringLiteral("initialize"), hello, kStartTimeoutMs,
            [this, fallBack](const QJsonObject &answer, const QString &error) {
        if (m_needsSignIn || m_refreshing)
            return;
        if (!error.isEmpty()) {
            // A stdio server that knows no initialize is a newer one.
            if (fallBack && !m_down)
                discover(false);
            else
                stalled(QStringLiteral("no answer to initialize (%1)").arg(error));
            return;
        }
        // The version the server chose, which every later request names.
        m_version = answer.value(QStringLiteral("protocolVersion")).toString(m_version);
        // Which build answered - name and version, the version carrying its
        // commit where the server gives one - so a stale install shows in
        // the log.
        const QJsonObject info = answer.value(QStringLiteral("serverInfo")).toObject();
        m_build = (info.value(QStringLiteral("name")).toString() + QLatin1Char(' ')
                   + info.value(QStringLiteral("version")).toString()).trimmed();
        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("method"), QStringLiteral("notifications/initialized")}});
        listTools();
    });
}

void McpServer::listTools(std::function<void()> then)
{
    // shortcut: the first page only (no nextCursor); every server met so far lists its tools in one.
    request(QStringLiteral("tools/list"), QJsonObject(), kStartTimeoutMs,
            [this, then](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty()) {
            if (m_ready)
                liveLog(QStringLiteral("mcp %1: no fresh tool list (%2)").arg(m_name, error));
            else
                stalled(QStringLiteral("no tool list (%1)").arg(error));
            if (then)
                then();
            return;
        }
        m_tools.clear();
        for (const QJsonValue &value : result.value(QStringLiteral("tools")).toArray()) {
            const QJsonObject t = value.toObject();
            m_tools.append({t.value(QStringLiteral("name")).toString(),
                            t.value(QStringLiteral("description")).toString(),
                            t.value(QStringLiteral("inputSchema")).toObject()});
        }
        const bool wasReady = m_ready;
        m_ready = true;
        m_attempts = 0;
        QString where;
        if (isHttp()) {
            where = QStringLiteral(" at %1 (%2)")
                        .arg(m_url, m_proc ? QStringLiteral("started by Burnos")
                                           : QStringLiteral("already hosted"));
        }
        liveLog(QStringLiteral("mcp %1: %2, %3 tool(s)%4%5 - protocol %6")
                    .arg(m_name, wasReady ? QStringLiteral("tools listed again") : QStringLiteral("ready"))
                    .arg(m_tools.size())
                    .arg(where, m_build.isEmpty() ? QString() : QStringLiteral(" - ") + m_build, m_version));
        if (!wasReady)
            emit readyChanged(true);
        if (then)
            then();
    });
}

void McpServer::rpc(const QString &method, const QJsonObject &params, Reply then, int timeoutMs)
{
    if (!m_ready) {
        const QString why = QStringLiteral("%1 is not ready").arg(m_name);
        QTimer::singleShot(0, this, [then, why]() { then({}, why); });
        return;
    }
    request(method, params, timeoutMs, std::move(then));
}

void McpServer::signIn(std::function<void(bool ok, const QString &message)> done)
{
    if (!isHttp() || m_local) {
        QTimer::singleShot(0, this, [done]() { done(false, tr("Only a remote server is signed in to.")); });
        return;
    }
    McpAuth::signIn(m_net, this, m_url, m_challenge, [this, done](bool ok, const QString &message) {
        if (ok) {
            m_needsSignIn = false;
            m_attempts = 0;
            handshake();
        }
        done(ok, message);
    });
}

// A 401 from a remote server: a kept refresh token is tried once, quietly;
// with none, or when it no longer works, the user has to sign in.
void McpServer::unauthorized(QNetworkReply *reply)
{
    m_challenge = QString::fromLatin1(reply->rawHeader("WWW-Authenticate"));
    if (m_refreshing || m_needsSignIn)
        return;
    if (!McpAuth::canRefresh(m_url)) {
        m_needsSignIn = true;
        liveLog(QStringLiteral("mcp %1: needs signing in (ask BurnOS to sign in to %1)").arg(m_name));
        return;
    }
    m_refreshing = true;
    McpAuth::refresh(m_net, this, m_url, [this](bool ok, const QString &error) {
        m_refreshing = false;
        if (!ok) {
            m_needsSignIn = true;
            liveLog(QStringLiteral("mcp %1: the sign-in has run out (%2) - needs signing in again").arg(m_name, error));
            return;
        }
        if (!m_ready)
            handshake();
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
        for (auto a = m_asked.begin(); a != m_asked.end();)
            a = a->second == id ? m_asked.erase(a) : std::next(a);
        // A call parked on a question to the user has nobody left to tell.
        if (late.then)
            late.then({}, QStringLiteral("no answer in time"));
    });
    pending.timer->start(timeoutMs);
    pending.method = method;
    m_pending.insert(id, pending);
    QJsonObject withMeta = params;
    if (m_stateless) {
        // 2026-07-28 has no handshake: every request says who is asking, in
        // which version, and what it can do.
        QJsonObject meta = params.value(QStringLiteral("_meta")).toObject();
        meta.insert(QStringLiteral("io.modelcontextprotocol/protocolVersion"), m_version);
        meta.insert(QStringLiteral("io.modelcontextprotocol/clientCapabilities"), clientCapabilities());
        meta.insert(QStringLiteral("io.modelcontextprotocol/clientInfo"), clientInfo());
        withMeta.insert(QStringLiteral("_meta"), meta);
    }
    const QJsonObject message{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                              {QStringLiteral("id"), id},
                              {QStringLiteral("method"), method},
                              {QStringLiteral("params"), withMeta}};
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
    req.setRawHeader("MCP-Protocol-Version", m_version.toLatin1());
    // 2026-07-28 wants the method, and what it names, in the headers too.
    const QString method = message.value(QStringLiteral("method")).toString();
    if (!method.isEmpty()) {
        req.setRawHeader("Mcp-Method", method.toUtf8());
        const QJsonObject p = message.value(QStringLiteral("params")).toObject();
        const QString named = p.value(QStringLiteral("name")).toString(p.value(QStringLiteral("uri")).toString());
        // shortcut: no Base64 sentinel for a name that is not plain ASCII; tool names are.
        if (!named.isEmpty() && (method == QLatin1String("tools/call") || method == QLatin1String("prompts/get")
                                 || method == QLatin1String("resources/read")))
            req.setRawHeader("Mcp-Name", named.toUtf8());
    }
    if (!m_sessionId.isEmpty())
        req.setRawHeader("Mcp-Session-Id", m_sessionId);
    if (!m_local) {
        const QString bearer = McpAuth::token(m_url);
        if (!bearer.isEmpty())
            req.setRawHeader("Authorization", "Bearer " + bearer.toUtf8());
    }
    // The pending timer answers first; this only makes sure the socket is let go.
    req.setTransferTimeout(timeoutMs + 1000);
    QNetworkReply *reply = m_net->post(req, QJsonDocument(message).toJson(QJsonDocument::Compact));
    if (id >= 0) {
        auto it = m_pending.find(id);
        if (it != m_pending.end())
            it->post = reply;
    }
    // An event stream is read as it comes: an older server may ask the user
    // something in the middle of it and wait for the answer before it ends.
    auto buffer = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, id, buffer]() {
        if (!reply->header(QNetworkRequest::ContentTypeHeader).toString().contains(QLatin1String("text/event-stream")))
            return;
        buffer->append(reply->readAll());
        events(*buffer, id, false);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, buffer]() {
        reply->deleteLater();
        onPosted(reply, id, *buffer);
    });
}

// Server-sent events: the "data:" lines of each event, a blank line between
// events, every event one JSON-RPC message. What is left is kept for the next
// read, unless this is the last.
void McpServer::events(QByteArray &buffer, int via, bool final)
{
    buffer.replace("\r\n", "\n");
    while (!buffer.isEmpty()) {
        qsizetype end = buffer.indexOf("\n\n");
        if (end < 0 && !final)
            return;
        const QByteArray event = end < 0 ? buffer : buffer.left(end);
        buffer.remove(0, end < 0 ? buffer.size() : end + 2);
        QByteArray data;
        for (const QByteArray &raw : event.split('\n')) {
            const QByteArray line = raw.trimmed();
            if (line.startsWith("data:")) {
                if (!data.isEmpty())
                    data += '\n';
                data += line.mid(5).trimmed();
            }
        }
        if (!data.isEmpty())
            dispatch(QJsonDocument::fromJson(data).object(), via);
    }
}

void McpServer::onPosted(QNetworkReply *reply, int id, QByteArray &buffer)
{
    if (m_stopping)
        return;
    // Our own timeout cancelling it: already answered.
    if (reply->error() == QNetworkReply::OperationCanceledError)
        return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    m_down = status == 0;
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
    if (status == 401 && !m_local)
        unauthorized(reply);
    if (status >= 400) {
        answer(id, {}, status == 401 && m_needsSignIn
                           ? tr("%1 needs signing in: call mcp with action sign_in and server %1.").arg(m_name)
                           : QStringLiteral("HTTP %1: %2")
                                 .arg(status).arg(QString::fromUtf8(body.left(200)).simplified()));
        return;
    }
    const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    if (type.contains(QLatin1String("text/event-stream"))) {
        buffer.append(body);
        events(buffer, id, true);
    } else {
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isArray()) {
            for (const QJsonValue &value : doc.array())
                dispatch(value.toObject(), id);
        } else {
            dispatch(doc.object(), id);
        }
    }
    // Answered with nothing that carried our id: say so now rather than
    // leave it to the timer.
    if (m_pending.contains(id))
        answer(id, {}, QStringLiteral("HTTP %1 with no JSON-RPC answer in it").arg(status));
}

// One JSON-RPC message from the server, whichever way it came.
void McpServer::dispatch(const QJsonObject &message, int via)
{
    if (message.isEmpty())
        return;
    if (message.contains(QStringLiteral("method"))) {
        if (message.contains(QStringLiteral("id")))
            serverAsks(message, via);
        else if (message.value(QStringLiteral("method")).toString() == QLatin1String("notifications/tools/list_changed"))
            listTools();
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

// Something an older server (before 2026-07-28) asks of BurnOS in the middle
// of a call. A question for the user (elicitation/create) is handed to the
// task that made the call as an input_required result - the shape a newer
// server gives it - and the call stays open, parked, until the task answers it
// through call() with inputResponses. Everything else that is still part of
// the protocol is answered here; the rest is refused, never left waiting.
void McpServer::serverAsks(const QJsonObject &message, int via)
{
    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonValue id = message.value(QStringLiteral("id"));
    const auto reply = [this, id](const QString &key, const QJsonValue &value) {
        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}, {key, value}});
    };
    if (method == QLatin1String("ping")) {
        reply(QStringLiteral("result"), QJsonObject());
        return;
    }
    if (method == QLatin1String("roots/list")) {
        reply(QStringLiteral("result"), QJsonObject{{QStringLiteral("roots"), QJsonArray()}});
        return;
    }
    if (method == QLatin1String("elicitation/create")) {
        // Over stdio nothing says which call it belongs to: the latest tool call.
        if (!m_pending.contains(via)) {
            for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
                if (it->method == QLatin1String("tools/call") && it->then && it.key() > via)
                    via = it.key();
            }
        }
        auto it = m_pending.find(via);
        if (it != m_pending.end() && it->then) {
            const QString key = QStringLiteral("server-question-%1").arg(id.toVariant().toString());
            m_asked.insert(key, {id, via});
            const Reply waiting = std::move(it->then);
            it->then = nullptr;
            it->timer->start(kAskTimeoutMs);
            waiting(QJsonObject{{QStringLiteral("resultType"), QStringLiteral("input_required")},
                                {QStringLiteral("inputRequests"), QJsonObject{{key, QJsonObject{
                                    {QStringLiteral("method"), method},
                                    {QStringLiteral("params"), message.value(QStringLiteral("params"))}}}}}},
                    QString());
            return;
        }
    }
    reply(QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32601},
                                               {QStringLiteral("message"), QStringLiteral("not supported")}});
}

void McpServer::answer(int id, const QJsonObject &result, const QString &error)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end() || !it->then)
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
    m_asked.clear();
    for (const Pending &p : waiting) {
        p.timer->stop();
        p.timer->deleteLater();
        if (p.then)
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

void McpServer::call(const QString &tool, const QJsonObject &arguments, Done done, int timeoutMs,
                     const QJsonObject &more)
{
    if (!m_ready) {
        const QString why = QStringLiteral("%1 is not ready").arg(m_name);
        QTimer::singleShot(0, this, [done, why]() { done(false, {}, why); });
        return;
    }
    const Reply finished = [done](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty()) {
            done(false, {}, error);
            return;
        }
        done(!result.value(QStringLiteral("isError")).toBool(), result, QString());
    };
    // The answers to an older server's question go back to it as responses,
    // and the call it parked carries on: no new tools/call.
    const QJsonObject answers = more.value(QStringLiteral("inputResponses")).toObject();
    int resumed = -1;
    for (auto it = answers.begin(); it != answers.end(); ++it) {
        const auto asked = m_asked.find(it.key());
        if (asked == m_asked.end())
            continue;
        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), asked->first}, {QStringLiteral("result"), it.value()}});
        resumed = asked->second;
        m_asked.erase(asked);
    }
    if (resumed >= 0) {
        auto parked = m_pending.find(resumed);
        if (parked == m_pending.end()) {
            QTimer::singleShot(0, this, [done]() {
                done(false, {}, tr("The server stopped waiting for that answer - call the tool again."));
            });
            return;
        }
        parked->then = finished;
        parked->timer->start(timeoutMs);
        return;
    }
    QJsonObject params = more;
    params.insert(QStringLiteral("name"), tool);
    params.insert(QStringLiteral("arguments"), arguments);
    request(QStringLiteral("tools/call"), params, timeoutMs, finished);
}

QString McpServer::textOf(const QJsonObject &result)
{
    QStringList parts;
    for (const QJsonValue &value : result.value(QStringLiteral("content")).toArray()) {
        const QJsonObject part = value.toObject();
        if (part.value(QStringLiteral("type")).toString() == QLatin1String("text"))
            parts.append(part.value(QStringLiteral("text")).toString());
    }
    // Structured content may be any JSON value since 2026-07-28, not only an object.
    const QJsonValue structured = result.value(QStringLiteral("structuredContent"));
    if (parts.isEmpty() && !structured.isUndefined()) {
        parts.append(structured.isObject() || structured.isArray()
                         ? QString::fromUtf8(QJsonDocument::fromVariant(structured.toVariant())
                                                 .toJson(QJsonDocument::Compact))
                         : structured.toVariant().toString());
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
        const bool memoryServer = isMemoryServer(it.key(), command);
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

QStringList McpClient::signInNames() const
{
    QStringList names;
    for (McpServer *s : m_servers) {
        if (s->needsSignIn())
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
