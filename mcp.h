#ifndef MCP_H
#define MCP_H

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;
class QTimer;

namespace BurnOS {

// ONE MODEL CONTEXT PROTOCOL SERVER, run as a child process and spoken to over
// its stdin and stdout: JSON-RPC 2.0, one message per line. That is the whole
// of how Burnos reaches a capability that lives in another program - the
// user's long-term memory (Mnemosyne) first of all - without linking a line of
// it in. The server does its work in its own process; Burnos only asks.
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

    McpServer(const QString &name, const QString &command, const QStringList &args,
              const QHash<QString, QString> &env, const QString &cwd,
              QObject *parent = nullptr);
    ~McpServer() override;

    QString name() const { return m_name; }
    // Answered initialize and tools/list: its tools are known and callable.
    bool isReady() const { return m_ready; }
    const QList<Tool> &tools() const { return m_tools; }
    bool hasTool(const QString &tool) const;

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
    };

    void handshake();
    void request(const QString &method, const QJsonObject &params, int timeoutMs, Reply then);
    void send(const QJsonObject &message);
    void onOutput();
    void onExit();
    void failAll(const QString &why);

    QString m_name;
    QString m_command;
    QStringList m_args;
    QHash<QString, QString> m_env;
    QString m_cwd;

    QProcess *m_proc = nullptr;
    QByteArray m_buffer;
    int m_nextId = 0;
    QHash<int, Pending> m_pending;
    QList<Tool> m_tools;
    bool m_ready = false;
    bool m_stopping = false;
    // When it last died, so a server that keeps dying is left alone rather
    // than restarted for ever.
    QList<qint64> m_deaths;
};

// EVERY SERVER THE USER LISTED, in %APPDATA%\BurnOS\mcp.json, in the same
// "mcpServers" layout Claude Desktop reads:
//
//   { "mcpServers": { "memory": { "command": "...mneme-mcp.exe",
//                                 "args": ["--backend", "sqlite", ...] } } }
//
// There is no settings screen for it on purpose. Burnos can edit that file
// itself, by voice, with its own file tools.
class McpClient : public QObject
{
    Q_OBJECT

public:
    explicit McpClient(QObject *parent = nullptr);

    static QString configPath();
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
