#ifndef COMPOSIO_H
#define COMPOSIO_H

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;

// One app Composio can reach: Gmail, Slack, Notion, GitHub and about fifteen
// hundred more. `slug` is the id every other call takes; `name` is the only
// part a person should ever be shown.
struct ComposioApp
{
    QString slug;
    QString name;
    QString description;

    // Usable with no sign in at all: web search, Hacker News, weather and the
    // like. There is nothing to connect, so there is no Connect for them - the
    // session runs their tools as they are.
    bool noAuth = false;
    // Composio's own toolkit, whose tools ARE the session: search, execute,
    // manage connections. Part of every session and never connected.
    bool builtIn = false;
    // Composio owns the OAuth app for this toolkit, so connecting needs nothing
    // from the user but a sign in. Where this is false the user has to bring
    // their own client id or an API key, which is a job for Composio's own
    // dashboard rather than a 470 pixel dialog, and the row says so.
    bool managedAuth = false;

    // Filled in from the connected accounts list, not from the app list.
    QString connectionId;
    QString connectionStatus;   // ACTIVE, INITIATED, INITIALIZING, FAILED, or empty

    bool isConnected() const
    {
        return connectionStatus == QStringLiteral("ACTIVE");
    }
    // Signed in somewhere in a browser but not finished yet.
    bool isPending() const
    {
        return !connectionStatus.isEmpty() && !isConnected()
               && connectionStatus != QStringLiteral("FAILED")
               && connectionStatus != QStringLiteral("EXPIRED");
    }
};

// Everything Burnos does with Composio, which is the whole of its reach into
// other people's services.
//
// WHY COMPOSIO AT ALL: the orb can already operate this machine, but that only
// covers what has a window on it. Sending an email, posting to Slack, filing a
// Notion page or opening a GitHub issue are all HTTP APIs with their own OAuth
// dance, and writing fifteen hundred of those is not a project. Composio holds
// the credentials, refreshes the tokens and runs the tools.
//
// ONE SESSION PER INSTALL, which is Composio's current model (REST v3.1). The
// session is made once for this install's user, kept in settings and reused
// for good - Composio keeps sessions on the server and never expires them - and
// tool search, tool runs and sign in links all go through it. Burnos is its own
// harness: the OpenRouter model plans, and the session supplies the catalogue,
// the credentials and the execution underneath it. It is also what makes an
// app with no sign in work at all: the old direct path tried to build a sign in
// for one and Composio refused with Auth_Config_NoAuthApp.
//
// EVERY CALL IS ASYNCHRONOUS and answers on a signal. Nothing here blocks, and
// the key is read out of settings at the moment it is used, exactly like the
// OpenRouter key.
class ComposioClient : public QObject
{
    Q_OBJECT

public:
    explicit ComposioClient(QObject *parent = nullptr);

    // THE ANSWER TO ONE CALLER, already worded for the model. Several tasks can
    // use the apps at the same moment, and each has to get back the answer to
    // its OWN question. connectApp alone may be called without one, by the
    // integrations dialog, which listens to the signals instead.
    using Answer = std::function<void(bool ok, const QString &summary)>;

    // True when a key is stored. No key means the app tool is not even offered
    // to the model, rather than offered and then failing.
    static bool isConfigured();

    // The app catalogue. An empty query is the popular ones, in usage order,
    // which is what the dialog shows before anybody types.
    void searchApps(const QString &query, int limit = 40);

    // What this install has connected. Answers on appsReady() with only the
    // connected apps in it, and writes their slugs into settings so the model
    // can be told without a network round trip at the top of every question.
    void refreshConnections();

    // Open the sign in page for one app in the user's own browser, through the
    // session's link endpoint. The connection finishes OUT OF BAND, in that
    // browser, which is why the dialog polls afterwards rather than waiting on
    // a reply. An app that needs no sign in answers on noSignInNeeded().
    void connectApp(const QString &slug, Answer answer = {});

    // Revoke and forget one connection. The account itself is untouched; what
    // goes away is Composio's permission to act on it.
    void disconnectApp(const QString &connectionId);

    // Search every app for tools that can do something, described in a full
    // sentence. The answer is already worded for the model: the tools, their
    // arguments, and which of the apps involved still need a sign in.
    void searchTools(const QString &useCase, Answer answer);

    // Run one tool in the session. `arguments` is the object its schema asks for.
    void runTool(const QString &toolSlug, const QJsonObject &arguments, Answer answer);

signals:
    // The catalogue, or the connected subset. `connectedOnly` says which.
    void appsReady(const QList<ComposioApp> &apps, bool connectedOnly);
    // The browser has been sent to the sign in page for this app.
    void linkOpened(const QString &slug);
    // The app needs no sign in, so there was nothing to connect.
    void noSignInNeeded(const QString &slug);
    void disconnected(const QString &connectionId);
    void failed(const QString &reason);

private:
    struct Reply
    {
        bool ok = false;
        int status = 0;          // HTTP status, 0 when nothing came back
        QJsonObject root;
        QString error;           // Composio's own words, when it gave any
        QString errorSlug;       // such as ToolRouterV2_NoActiveConnection
    };
    using Handler = std::function<void(const Reply &)>;
    using SessionHandler =
        std::function<void(const QString &sessionId, const QString &error)>;

    // One request against the v3.1 API, with the key and the JSON headers on
    // it. `what` names it in the log. `done` is always called, exactly once.
    void call(const QByteArray &verb, const QString &path, const QJsonObject &body,
              const QString &what, Handler done);

    // The session id, made first if this install has none yet. Several callers
    // asking at once share the one creation request.
    void withSession(SessionHandler then);

    // A request under /tool_router/session/<id>. A session deleted on the
    // Composio side is made again once and the request retried.
    void callInSession(const QByteArray &verb, const QString &subPath,
                       const QJsonObject &body, const QString &what,
                       Handler done, bool retried = false);

    QNetworkAccessManager *m_net = nullptr;
    bool m_creatingSession = false;
    QList<SessionHandler> m_waitingForSession;
};

#endif // COMPOSIO_H
