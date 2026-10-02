#ifndef MCP_H
#define MCP_H

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;
class QProcess;
class QTimer;

namespace BurnOS {

// ONE MODEL CONTEXT PROTOCOL SERVER, spoken to in JSON-RPC 2.0 over one of two
// transports. That is the whole of how Burnos reaches a capability that lives
// in another program - the user's long-term memory (Mnemosyne) first of all -
// without linking a line of it in. The server does its work in its own
// process; Burnos only asks.
//
//  - STDIO: a child process Burnos starts and speaks to on its stdin and
//    stdout, one message per line. Its mcp.json entry has a "command".
//  - HTTP: a server already listening on a URL - MCP Streamable HTTP, every
//    message a POST, the answer as JSON or as an event stream. Its entry has
//    a "url". The memory server is hosted this way on localhost.
//
// Given BOTH, the url is tried first; when nothing answers there, the command
// is started - it must serve that url - and the url is tried again until it
// does. Somebody else's server at the url is used as found and never stopped.
// A process Burnos started is Burnos's to stop: on Windows it goes into a job
// object, so the whole tree (a Python launcher and the interpreters under it)
// dies with Burnos, not just the launcher.
class McpServer : public QObject
{
    Q_OBJECT

public:
    struct Tool
    {
        QString name;
        QString description;
        QJsonObject schema;
    };
    // `result` is the tools/call result as the server sent it; `error` says why
    // there is none.
    using Done = std::function<void(bool ok, const QJsonObject &result, const QString &error)>;

    McpServer(const QString &name, const QString &url, const QString &command,
              const QStringList &args, const QHash<QString, QString> &env,
              const QString &cwd, QObject *parent = nullptr);
    ~McpServer() override;

    QString name() const { return m_name; }
    // Where it is reached over HTTP; empty for a stdio server.
    QString url() const { return m_url; }
    bool isHttp() const { return !m_url.isEmpty(); }
    // The process serving it now is one Burnos started, not somebody else's.
    bool ownsProcess() const { return m_proc != nullptr; }
    // Answered initialize and tools/list: its tools are known and callable.
    bool isReady() const { return m_ready; }
    const QList<Tool> &tools() const { return m_tools; }
    bool hasTool(const QString &tool) const;
    // Whether that tool's schema names this argument - how a client tells a
    // newer server from an older one without asking its version.
    bool takes(const QString &tool, const QString &argument) const;
    // The build that answered initialize: "<name> <version>", the version
    // carrying its commit where the server gives one. Empty until the
    // handshake lands. This is what Burnos tells the user when asked which
    // memory it is running, instead of guessing.
    QString build() const { return m_build; }

    void start();
    // One tools/call. `done` is called exactly once, and never from inside
    // this call.
    void call(const QString &tool, const QJsonObject &arguments, Done done,
              int timeoutMs = 30000);

    // What a tool answered, as words for a model: its text parts, or its
    // structured content when it gave no text.
    static QString textOf(const QJsonObject &result);

signals:
    void readyChanged(bool ready);

private:
    using Reply = std::function<void(const QJsonObject &result, const QString &error)>;
    struct Pending
    {
        Reply then;
        QTimer *timer = nullptr;
        // The POST carrying it, when the transport is HTTP.
        QPointer<QNetworkReply> post;
    };

    void launch();
    void adopt();
    void handshake();
    void stalled(const QString &why);
    void request(const QString &method, const QJsonObject &params, int timeoutMs, Reply then);
    void send(const QJsonObject &message);
    void post(const QJsonObject &message, int id, int timeoutMs);
    void onPosted(QNetworkReply *reply, int id);
    void dispatch(const QJsonObject &message);
    void answer(int id, const QJsonObject &result, const QString &error);
    void onOutput();
    void onExit();
    void failAll(const QString &why);

    QString m_name;
    QString m_url;
    QString m_command;
    QStringList m_args;
    QHash<QString, QString> m_env;
    QString m_cwd;

    QProcess *m_proc = nullptr;
    // The Windows job object every process Burnos started here belongs to.
    void *m_job = nullptr;
    QNetworkAccessManager *m_net = nullptr;
    // The Mcp-Session-Id a stateful HTTP server hands out; a stateless one
    // (Mnemosyne's default) gives none.
    QByteArray m_sessionId;
    // HTTP only: the next try at a url that does not answer.
    QTimer *m_retry = nullptr;
    int m_attempts = 0;
    QByteArray m_buffer;
    int m_nextId = 0;
    QHash<int, Pending> m_pending;
    QList<Tool> m_tools;
    // What serverInfo said at the handshake, kept for build().
    QString m_build;
    bool m_ready = false;
    bool m_stopping = false;
    // Died too often to be started again; an HTTP url is still watched.
    bool m_givenUp = false;
    // When it last died, so a server that keeps dying is left alone rather
    // than restarted for ever.
    QList<qint64> m_deaths;
};

// EVERY SERVER THE USER LISTED, in %APPDATA%\BurnOS\mcp.json, in the same
// "mcpServers" layout Claude Desktop reads, plus "url" for one reached over
// HTTP:
//
//   { "mcpServers": { "memory": {
//       "url": "http://127.0.0.1:8765/mcp",
//       "command": "...\\mnemosyne\\.venv\\Scripts\\mneme-mcp.exe",
//       "args": ["--sdk-streamable-http", "--backend", "sqlite", ...] } } }
//
// With no mcp.json at all, the memory server is looked for in the mnemosyne
// folder beside Burnos.exe (see mnemosyneDir) and listed under that same
// layout. There is no settings screen for it on purpose. Burnos can edit that
// file itself, by voice, with its own file tools.
class McpClient : public QObject
{
    Q_OBJECT

public:
    explicit McpClient(QObject *parent = nullptr);

    static QString configPath();
    // The mnemosyne folder beside Burnos.exe: the memory server's executable
    // (mneme-mcp.exe, or .venv\Scripts\mneme-mcp.exe inside a Python venv).
    static QString mnemosyneDir();
    // The server executable in that folder, or empty when there is none.
    static QString mnemosyneExe();
    // Mnemosyne's canonical SQLite store, %LOCALAPPDATA%\BurnOS\memory: the
    // ONE store every server reads, whoever starts it. A second copy kept
    // anywhere else would quietly split the user's memories in two.
    static QString memoryStore();
    // The secret Burnos and the memory server sign session tokens with, in
    // %LOCALAPPDATA%\BurnOS\memory.secret - made on first use, and handed to
    // a server Burnos starts as MNEMOSYNE_MCP_SESSION_SECRET. Mnemosyne's
    // working memory (the short-term memory) answers nobody without one. A
    // server started by hand must be given the same secret.
    static QString sessionSecret();
    // A signed Mnemosyne session token: base64url claims, a dot, and their
    // HMAC-SHA256 under that secret - exactly what the server verifies.
    static QString sessionToken(const QString &tenant, const QString &user,
                                const QString &agent, const QString &session,
                                int ttlSeconds);
    // Reads the list and starts every server on it.
    void load();

    McpServer *server(const QString &name) const;
    QStringList readyNames() const;

    // The user's long-term memory: the first ready server that offers capture
    // and search, which is what Mnemosyne offers. Null when there is none.
    McpServer *memory() const;

    // For a task's find step: the tools of the ready servers whose name or
    // description holds the query's words, with the arguments each takes.
    QString describeTools(const QString &serverName, const QString &query) const;

signals:
    void memoryChanged(bool ready);

private:
    QList<McpServer *> m_servers;
};

} // namespace BurnOS

#endif // MCP_H
