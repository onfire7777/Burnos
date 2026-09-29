#include "mcp.h"

#include "livelog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

namespace BurnOS {

namespace {

// A Python server spends a few seconds importing before it answers anything.
constexpr int kStartTimeoutMs = 30000;
constexpr auto kProtocolVersion = "2024-11-05";
// Three deaths in five minutes and a server is left alone.
constexpr int kMaxDeaths = 3;
constexpr qint64 kDeathWindowMs = 5 * 60 * 1000;

} // namespace

// ===========================================================================
// ONE SERVER
// ===========================================================================

McpServer::McpServer(const QString &name, const QString &command, const QStringList &args,
                     const QHash<QString, QString> &env, const QString &cwd, QObject *parent)
    : QObject(parent)
    , m_name(name)
    , m_command(command)
    , m_args(args)
    , m_env(env)
    , m_cwd(cwd)
{
}

McpServer::~McpServer()
{
    m_stopping = true;
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(1000);
    }
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
    if (m_proc)
        return;
    m_ready = false;
    m_buffer.clear();
    m_tools.clear();

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

    connect(m_proc, &QProcess::started, this, &McpServer::handshake);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &McpServer::onOutput);
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

void McpServer::handshake()
{
    const QJsonObject hello{
        {QStringLiteral("protocolVersion"), QString::fromLatin1(kProtocolVersion)},
        {QStringLiteral("capabilities"), QJsonObject()},
        {QStringLiteral("clientInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Burnos")},
                                                    {QStringLiteral("version"), QStringLiteral("0.4")}}},
    };
    request(QStringLiteral("initialize"), hello, kStartTimeoutMs,
            [this](const QJsonObject &, const QString &error) {
        if (!error.isEmpty()) {
            liveLog(QStringLiteral("mcp %1: no answer to initialize (%2)").arg(m_name, error));
            return;
        }
        send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("method"), QStringLiteral("notifications/initialized")}});
        request(QStringLiteral("tools/list"), QJsonObject(), kStartTimeoutMs,
                [this](const QJsonObject &result, const QString &error) {
            if (!error.isEmpty()) {
                liveLog(QStringLiteral("mcp %1: no tool list (%2)").arg(m_name, error));
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
            liveLog(QStringLiteral("mcp %1: ready, %2 tool(s)").arg(m_name).arg(m_tools.size()));
            emit readyChanged(true);
        });
    });
}

void McpServer::request(const QString &method, const QJsonObject &params, int timeoutMs,
                        Reply then)
{
    if (!m_proc || m_proc->state() != QProcess::Running) {
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
        late.then({}, QStringLiteral("no answer in time"));
    });
    pending.timer->start(timeoutMs);
    m_pending.insert(id, pending);
    send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), id},
                     {QStringLiteral("method"), method},
                     {QStringLiteral("params"), params}});
}

void McpServer::send(const QJsonObject &message)
{
    if (m_proc)
        m_proc->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
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
        const QJsonObject message = QJsonDocument::fromJson(line).object();
        if (message.isEmpty())
            continue;

        // Something the SERVER asks of us. Burnos offers nothing back - no
        // roots, no sampling - and says so rather than leave it waiting.
        if (message.contains(QStringLiteral("method"))) {
            if (message.contains(QStringLiteral("id"))) {
                send(QJsonObject{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                    {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32601},
                                                          {QStringLiteral("message"),
                                                           QStringLiteral("not supported")}}}});
            }
            continue;
        }

        const int id = message.value(QStringLiteral("id")).toInt();
        auto it = m_pending.find(id);
        if (it == m_pending.end())
            continue;
        const Pending answered = it.value();
        m_pending.erase(it);
        answered.timer->stop();
        answered.timer->deleteLater();
        if (message.contains(QStringLiteral("error"))) {
            answered.then({}, message.value(QStringLiteral("error")).toObject()
                                  .value(QStringLiteral("message")).toString());
        } else {
            answered.then(message.value(QStringLiteral("result")).toObject(), QString());
        }
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
    // ever is a process storm, and the log says it was given up on.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_deaths.append(now);
    while (!m_deaths.isEmpty() && now - m_deaths.first() > kDeathWindowMs)
        m_deaths.removeFirst();
    if (m_deaths.size() > kMaxDeaths) {
        liveLog(QStringLiteral("mcp %1: stopped for good after dying %2 times in five minutes")
                    .arg(m_name).arg(m_deaths.size()));
        return;
    }
    liveLog(QStringLiteral("mcp %1: stopped - starting it again").arg(m_name));
    QTimer::singleShot(2000, this, &McpServer::start);
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
    return QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA"))
           + QStringLiteral("/BurnOS/mcp.json");
}

void McpClient::load()
{
    const QString path = configPath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        liveLog(QStringLiteral("mcp: no servers listed - %1 is not there")
                    .arg(QDir::toNativeSeparators(path)));
        return;
    }
    QJsonParseError parse{};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll(), &parse).object();
    if (parse.error != QJsonParseError::NoError) {
        liveLog(QStringLiteral("mcp: %1 is not valid JSON (%2)")
                    .arg(QDir::toNativeSeparators(path), parse.errorString()));
        return;
    }

    const QJsonObject listed = root.value(QStringLiteral("mcpServers")).toObject();
    for (auto it = listed.begin(); it != listed.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        if (entry.value(QStringLiteral("disabled")).toBool())
            continue;
        const QString command = entry.value(QStringLiteral("command")).toString();
        if (command.isEmpty()) {
            liveLog(QStringLiteral("mcp: %1 has no command, left out").arg(it.key()));
            continue;
        }
        QStringList args;
        for (const QJsonValue &arg : entry.value(QStringLiteral("args")).toArray())
            args.append(arg.toString());
        QHash<QString, QString> env;
        const QJsonObject vars = entry.value(QStringLiteral("env")).toObject();
        for (auto v = vars.begin(); v != vars.end(); ++v)
            env.insert(v.key(), v.value().toString());

        auto *server = new McpServer(it.key(), command, args, env,
                                     entry.value(QStringLiteral("cwd")).toString(), this);
        connect(server, &McpServer::readyChanged, this,
                [this]() { emit memoryChanged(memory() != nullptr); });
        m_servers.append(server);
        server->start();
    }
    liveLog(QStringLiteral("mcp: %1 server(s) listed in %2")
                .arg(m_servers.size()).arg(QDir::toNativeSeparators(path)));
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
