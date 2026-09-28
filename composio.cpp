#include "composio.h"
#include "livelog.h"
#include "settings.h"

#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

namespace {

// v3 is the stable REST surface. The key rides in x-api-key, not in an
// Authorization header, which is the one thing easy to get wrong here.
constexpr auto kBase = "https://backend.composio.dev/api/v3";

// A tool result can be a whole mailbox page. The model only needs enough of it
// to tell the user what happened, and everything past this is a slower answer
// for no gain.
constexpr int kMaxResultChars = 2400;

// Manual tool execution has to name a toolkit version or Composio cannot tell
// which definition of the tool was meant. "latest" is the right answer for an
// app that is asking a question a person just spoke.
constexpr auto kVersions = "latest";

ComposioApp readApp(const QJsonObject &o)
{
    ComposioApp app;
    app.slug = o.value(QStringLiteral("slug")).toString();
    app.name = o.value(QStringLiteral("name")).toString();
    if (app.name.isEmpty())
        app.name = app.slug;

    const QJsonObject meta = o.value(QStringLiteral("meta")).toObject();
    app.description = meta.value(QStringLiteral("description")).toString();

    app.noAuth = o.value(QStringLiteral("no_auth")).toBool()
                 || meta.value(QStringLiteral("isNoAuth")).toBool();

    const QJsonArray managed =
        o.value(QStringLiteral("composio_managed_auth_schemes")).toArray();
    app.managedAuth = !managed.isEmpty();

    return app;
}

} // namespace

QString ComposioTool::describe() const
{
    QString line = slug;
    if (!description.isEmpty())
        line += QStringLiteral(" - ") + description.simplified().left(180);
    if (!required.isEmpty()) {
        line += QStringLiteral(" [needs: ") + required.join(QStringLiteral(", "))
                + QLatin1Char(']');
    }
    return line;
}

ComposioClient::ComposioClient(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

bool ComposioClient::isConfigured()
{
    return BurnOS::Settings::hasComposioKey();
}

QNetworkReply *ComposioClient::send(const QByteArray &verb, const QString &path,
                                    const QJsonObject &body)
{
    const QString key = BurnOS::Settings::composioKey();
    if (key.isEmpty()) {
        BurnOS::liveLog(QStringLiteral("composio REFUSED: no key stored"));
        emit failed(tr("No Composio key stored. Open Settings, then Manage "
                       "integrations, and paste one in."));
        return nullptr;
    }

    QNetworkRequest request{QUrl(QString::fromLatin1(kBase) + path)};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("x-api-key", key.toUtf8());
    request.setRawHeader("Accept", "application/json");
    // Generous, because a tool call goes on to hit somebody else's API: sending
    // an email through Composio is two network hops, not one.
    request.setTransferTimeout(45000);

    const QByteArray payload =
        body.isEmpty() ? QByteArray()
                       : QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply *reply = nullptr;
    if (verb == "GET")
        reply = m_net->get(request);
    else if (verb == "POST")
        reply = m_net->post(request, payload);
    else
        reply = m_net->sendCustomRequest(request, verb, payload);

    ++m_inFlight;
    connect(reply, &QNetworkReply::finished, this, [this]() {
        if (m_inFlight > 0)
            --m_inFlight;
    });

    BurnOS::liveLog(QStringLiteral("composio %1 %2")
                        .arg(QString::fromLatin1(verb), path));
    return reply;
}

bool ComposioClient::readJson(QNetworkReply *reply, QJsonObject *out,
                              const QString &what)
{
    reply->deleteLater();
    const QByteArray raw = reply->readAll();
    const QJsonObject root = QJsonDocument::fromJson(raw).object();

    if (reply->error() != QNetworkReply::NoError) {
        // Composio words its own errors well, so the body is worth far more
        // than Qt's "Error transferring ... server replied: Bad Request".
        QString detail = root.value(QStringLiteral("message")).toString();
        if (detail.isEmpty())
            detail = root.value(QStringLiteral("error")).toString();
        if (detail.isEmpty())
            detail = QString::fromUtf8(raw).simplified().left(240);

        BurnOS::liveLog(QStringLiteral("composio %1 FAILED: %2")
                            .arg(what, detail.isEmpty() ? reply->errorString()
                                                        : detail));
        emit failed(detail.isEmpty() ? reply->errorString() : detail);
        return false;
    }

    *out = root;
    return true;
}

void ComposioClient::searchApps(const QString &query, int limit)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 200)));
    // With nothing typed, the most used apps first is a far better first screen
    // than the alphabet, which opens on things nobody has heard of.
    q.addQueryItem(QStringLiteral("sort_by"),
                   query.trimmed().isEmpty() ? QStringLiteral("usage")
                                             : QStringLiteral("usage"));
    if (!query.trimmed().isEmpty())
        q.addQueryItem(QStringLiteral("search"), query.trimmed());

    QNetworkReply *reply =
        send("GET", QStringLiteral("/toolkits?") + q.toString(QUrl::FullyEncoded));
    if (!reply)
        return;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        QJsonObject root;
        if (!readJson(reply, &root, QStringLiteral("app search")))
            return;

        QList<ComposioApp> apps;
        const QJsonArray items = root.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &v : items) {
            const ComposioApp app = readApp(v.toObject());
            if (!app.slug.isEmpty())
                apps.append(app);
        }

        BurnOS::liveLog(QStringLiteral("composio: %1 app(s) found")
                            .arg(apps.size()));
        emit appsReady(apps, false);
    });
}

void ComposioClient::refreshConnections()
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("user_ids"), BurnOS::Settings::composioUserId());
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("100"));

    QNetworkReply *reply = send("GET", QStringLiteral("/connected_accounts?")
                                           + q.toString(QUrl::FullyEncoded));
    if (!reply)
        return;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        QJsonObject root;
        if (!readJson(reply, &root, QStringLiteral("connections")))
            return;

        QList<ComposioApp> apps;
        QStringList connectedSlugs;
        const QJsonArray items = root.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &v : items) {
            const QJsonObject o = v.toObject();
            ComposioApp app;
            app.slug = o.value(QStringLiteral("toolkit")).toObject()
                           .value(QStringLiteral("slug")).toString();
            if (app.slug.isEmpty())
                continue;
            app.name = app.slug;
            app.connectionId = o.value(QStringLiteral("id")).toString();
            app.connectionStatus = o.value(QStringLiteral("status")).toString();
            app.managedAuth = true;
            apps.append(app);
            if (app.isConnected() && !connectedSlugs.contains(app.slug))
                connectedSlugs.append(app.slug);
        }

        // Written down so the model can be told what it may reach without a
        // network round trip at the top of every question.
        BurnOS::Settings::setComposioApps(connectedSlugs);

        BurnOS::liveLog(QStringLiteral("composio: %1 connection(s), %2 active")
                            .arg(apps.size()).arg(connectedSlugs.size()));
        emit appsReady(apps, true);
    });
}

void ComposioClient::connectApp(const QString &slug)
{
    if (slug.isEmpty())
        return;

    // An auth config is the project's credentials for one app. The project may
    // well already have one - made here earlier, or in Composio's dashboard -
    // and making a second would work but litter the account, so look first.
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("toolkit_slug"), slug);
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("10"));

    QNetworkReply *reply = send("GET", QStringLiteral("/auth_configs?")
                                           + q.toString(QUrl::FullyEncoded));
    if (!reply)
        return;

    connect(reply, &QNetworkReply::finished, this, [this, reply, slug]() {
        QJsonObject root;
        if (!readJson(reply, &root, QStringLiteral("auth configs")))
            return;

        const QJsonArray items = root.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &v : items) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("is_disabled")).toBool())
                continue;
            const QString id = o.value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) {
                createLink(slug, id);
                return;
            }
        }

        createAuthConfig(slug);
    });
}

void ComposioClient::createAuthConfig(const QString &slug)
{
    // use_composio_managed_auth is what makes this a one click affair: Composio
    // supplies the OAuth client, so the user signs in and nothing else is asked
    // of them. Apps without a managed scheme need the user's own client id and
    // that belongs in Composio's dashboard, not here - the request simply fails
    // and the message says so.
    const QJsonObject body{
        {QStringLiteral("toolkit"), QJsonObject{{QStringLiteral("slug"), slug}}},
        {QStringLiteral("auth_config"), QJsonObject{
            {QStringLiteral("type"), QStringLiteral("use_composio_managed_auth")}}},
    };

    QNetworkReply *reply = send("POST", QStringLiteral("/auth_configs"), body);
    if (!reply)
        return;

    connect(reply, &QNetworkReply::finished, this, [this, reply, slug]() {
        QJsonObject root;
        if (!readJson(reply, &root, QStringLiteral("create auth config")))
            return;

        const QString id = root.value(QStringLiteral("auth_config")).toObject()
                               .value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            emit failed(tr("%1 cannot be connected from here: it needs its own "
                           "credentials, which have to be set up in the Composio "
                           "dashboard first.").arg(slug));
            return;
        }
        createLink(slug, id);
    });
}

void ComposioClient::createLink(const QString &slug, const QString &authConfigId)
{
    const QJsonObject body{
        {QStringLiteral("auth_config_id"), authConfigId},
        {QStringLiteral("user_id"), BurnOS::Settings::composioUserId()},
    };

    QNetworkReply *reply =
        send("POST", QStringLiteral("/connected_accounts/link"), body);
    if (!reply)
        return;

    connect(reply, &QNetworkReply::finished, this, [this, reply, slug]() {
        QJsonObject root;
        if (!readJson(reply, &root, QStringLiteral("create link")))
            return;

        const QString url = root.value(QStringLiteral("redirect_url")).toString();
        if (url.isEmpty()) {
            emit failed(tr("Composio did not give a sign in link for %1.").arg(slug));
            return;
        }

        // The sign in happens in the user's own browser, where their password
        // manager and their existing sessions already are. Nothing about it
        // comes back to us: the connection appears on the next refresh.
        QDesktopServices::openUrl(QUrl(url));
        BurnOS::liveLog(QStringLiteral("composio: sign in opened for %1").arg(slug));
        emit linkOpened(slug);
    });
}

void ComposioClient::disconnectApp(const QString &connectionId)
{
    if (connectionId.isEmpty())
        return;

    QNetworkReply *reply = send("DELETE", QStringLiteral("/connected_accounts/")
                                              + connectionId);
    if (!reply)
        return;

    connect(reply, &QNetworkReply::finished, this, [this, reply, connectionId]() {
        QJsonObject root;
        if (!readJson(reply, &root, QStringLiteral("disconnect")))
            return;
        BurnOS::liveLog(QStringLiteral("composio: %1 disconnected").arg(connectionId));
        emit disconnected(connectionId);
    });
}

void ComposioClient::searchTools(const QString &query, const QStringList &appSlugs,
                                 int limit)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 20)));
    q.addQueryItem(QStringLiteral("toolkit_versions"), QString::fromLatin1(kVersions));
    if (!query.trimmed().isEmpty())
        q.addQueryItem(QStringLiteral("search"), query.trimmed());
    if (!appSlugs.isEmpty()) {
        q.addQueryItem(QStringLiteral("toolkit_slugs"),
                       appSlugs.join(QLatin1Char(',')));
    }

    QNetworkReply *reply =
        send("GET", QStringLiteral("/tools?") + q.toString(QUrl::FullyEncoded));
    if (!reply)
        return;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        QJsonObject root;
        if (!readJson(reply, &root, QStringLiteral("tool search")))
            return;

        QList<ComposioTool> tools;
        const QJsonArray items = root.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &v : items) {
            const QJsonObject o = v.toObject();
            ComposioTool tool;
            tool.slug = o.value(QStringLiteral("slug")).toString();
            if (tool.slug.isEmpty())
                continue;
            tool.name = o.value(QStringLiteral("name")).toString();
            tool.description = o.value(QStringLiteral("description")).toString();
            tool.appSlug = o.value(QStringLiteral("toolkit")).toObject()
                               .value(QStringLiteral("slug")).toString();

            const QJsonObject schema =
                o.value(QStringLiteral("input_parameters")).toObject();
            tool.properties = schema.value(QStringLiteral("properties")).toObject();
            for (const QJsonValue &r : schema.value(QStringLiteral("required")).toArray())
                tool.required.append(r.toString());

            tools.append(tool);
        }

        BurnOS::liveLog(QStringLiteral("composio: %1 tool(s) found")
                            .arg(tools.size()));
        emit toolsReady(tools);
    });
}

void ComposioClient::runTool(const QString &toolSlug, const QJsonObject &arguments)
{
    if (toolSlug.isEmpty()) {
        emit toolFinished(false, tr("No tool was named."));
        return;
    }

    const QJsonObject body{
        {QStringLiteral("user_id"), BurnOS::Settings::composioUserId()},
        {QStringLiteral("arguments"), arguments},
    };

    QNetworkReply *reply = send("POST", QStringLiteral("/tools/execute/") + toolSlug,
                                body);
    if (!reply) {
        emit toolFinished(false, tr("No Composio key is stored, so nothing was run."));
        return;
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply, toolSlug]() {
        reply->deleteLater();
        const QByteArray raw = reply->readAll();
        const QJsonObject root = QJsonDocument::fromJson(raw).object();

        // A tool that ran and was refused by the far end is NOT a transport
        // error, and either way the model is told the same way: what happened,
        // in words it can say out loud.
        const bool transportOk = reply->error() == QNetworkReply::NoError;
        const bool successful = root.value(QStringLiteral("successful")).toBool();

        if (!transportOk || !successful) {
            QString detail = root.value(QStringLiteral("error")).toString();
            if (detail.isEmpty())
                detail = root.value(QStringLiteral("message")).toString();
            if (detail.isEmpty())
                detail = QString::fromUtf8(raw).simplified().left(300);
            if (detail.isEmpty())
                detail = reply->errorString();

            BurnOS::liveLog(QStringLiteral("composio run %1 FAILED: %2")
                                .arg(toolSlug, detail));
            emit toolFinished(false, tr("%1 did not work: %2")
                                         .arg(toolSlug, detail.left(kMaxResultChars)));
            return;
        }

        QString data = QString::fromUtf8(
            QJsonDocument(root.value(QStringLiteral("data")).toObject())
                .toJson(QJsonDocument::Compact));
        if (data == QStringLiteral("{}"))
            data.clear();
        if (data.size() > kMaxResultChars) {
            data = data.left(kMaxResultChars)
                   + QStringLiteral(" ...(cut short, there was more)");
        }

        BurnOS::liveLog(QStringLiteral("composio run %1 succeeded, %2 chars back")
                            .arg(toolSlug).arg(data.size()));
        emit toolFinished(true, data.isEmpty()
                                    ? tr("%1 ran and reported success.").arg(toolSlug)
                                    : tr("%1 ran. It returned: %2")
                                          .arg(toolSlug, data));
    });
}
