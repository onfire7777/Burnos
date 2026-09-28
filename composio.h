#ifndef COMPOSIO_H
#define COMPOSIO_H

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

// One app Composio can reach: Gmail, Slack, Notion, GitHub and about fifteen
// hundred more. `slug` is the id every other call takes; `name` is the only
// part a person should ever be shown.
struct ComposioApp
{
    QString slug;
    QString name;
    QString description;

    // Usable with no sign in at all - a handful of public data toolkits are.
    bool noAuth = false;
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

// One thing an app can actually do: SLACK_SEND_MESSAGE, GMAIL_SEND_EMAIL.
// This is what the model is shown and what it names to run something.
struct ComposioTool
{
    QString slug;
    QString name;
    QString description;
    QString appSlug;
    QStringList required;    // arguments the call will not work without
    QJsonObject properties;  // the input schema's properties, for the model

    // One line for the model. Short on purpose: ten of these travel together.
    QString describe() const;
};

// Everything Burnos does with Composio, which is the whole of its reach into
// other people's services.
//
// WHY COMPOSIO AT ALL: the orb can already operate this machine, but that only
// covers what has a window on it. Sending an email, posting to Slack, filing a
// Notion page or opening a GitHub issue are all HTTP APIs with their own OAuth
// dance, and writing fifteen hundred of those is not a project. Composio holds
// the credentials, refreshes the tokens and exposes every app as a flat list of
// named tools, so the orb needs exactly two verbs: find a tool, run a tool.
//
// EVERY CALL IS ASYNCHRONOUS and answers on a signal. Nothing here blocks, and
// nothing here is cached beyond the life of one request: the key is read out of
// settings at the moment it is used, exactly like the OpenRouter key.
class ComposioClient : public QObject
{
    Q_OBJECT

public:
    explicit ComposioClient(QObject *parent = nullptr);

    // True when a key is stored. No key means the integration tool is not even
    // offered to the model, rather than offered and then failing.
    static bool isConfigured();

    // The app catalogue. An empty query is the popular ones, in usage order,
    // which is what the dialog shows before anybody types.
    void searchApps(const QString &query, int limit = 40);

    // What this install has already connected. Answers on appsReady() too, with
    // only the connected apps in it, so the dialog can show them without a
    // second lookup.
    void refreshConnections();

    // Start the sign in for one app. Composio needs an auth config first, so
    // this reuses one if the project already has it and creates a managed one
    // if it does not, then opens the hosted sign in page in the user's browser.
    // The connection finishes OUT OF BAND, in that browser - which is why the
    // dialog polls afterwards rather than waiting on a reply.
    void connectApp(const QString &slug);

    // Revoke and forget one connection. The account itself is untouched; what
    // goes away is Composio's permission to act on it.
    void disconnectApp(const QString &connectionId);

    // Tools matching a plain language query, restricted to the apps given -
    // which are the connected ones, because a tool for an app nobody has signed
    // into can only fail.
    void searchTools(const QString &query, const QStringList &appSlugs,
                     int limit = 8);

    // Run one tool. `arguments` is the object the tool's own schema asks for.
    void runTool(const QString &toolSlug, const QJsonObject &arguments);

    bool isBusy() const { return m_inFlight > 0; }

signals:
    // The catalogue, or the connected subset. `connectedOnly` says which.
    void appsReady(const QList<ComposioApp> &apps, bool connectedOnly);
    // The browser has been sent to the sign in page for this app.
    void linkOpened(const QString &slug);
    void disconnected(const QString &connectionId);
    void toolsReady(const QList<ComposioTool> &tools);
    // What running the tool did, already worded for the model to say out loud.
    void toolFinished(bool ok, const QString &summary);
    void failed(const QString &reason);

private:
    // One request against the v3 API, with the key and the JSON headers on it.
    // Returns nullptr and emits failed() when there is no key stored.
    QNetworkReply *send(const QByteArray &verb, const QString &path,
                        const QJsonObject &body = QJsonObject());

    // The reply's JSON, or nothing at all plus a failed() when the request
    // itself went wrong. Every handler starts with this.
    bool readJson(QNetworkReply *reply, QJsonObject *out, const QString &what);

    // Step two and three of connectApp(), split out because the auth config
    // either already exists or has to be made first.
    void createAuthConfig(const QString &slug);
    void createLink(const QString &slug, const QString &authConfigId);

    QNetworkAccessManager *m_net = nullptr;
    int m_inFlight = 0;
};

#endif // COMPOSIO_H
