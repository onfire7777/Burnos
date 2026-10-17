#include "mcpauth.h"

#include "livelog.h"
#include "settings.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace BurnOS::McpAuth {

namespace {

// Registered once per authorization server, so it must never change.
constexpr quint16 kPort = 47823;

QString redirectUri()
{
    return QStringLiteral("http://127.0.0.1:%1/callback").arg(kPort);
}

// Tokens are kept per server, the registration per issuer (the spec binds a
// client to the authorization server that issued it).
QString keyFor(const QString &kind, const QString &id)
{
    return QStringLiteral("mcp-oauth/%1-%2").arg(
        kind, QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex().left(16)));
}

QJsonObject load(const QString &key)
{
    return QJsonDocument::fromJson(Settings::secret(key)).object();
}

void save(const QString &key, const QJsonObject &value)
{
    Settings::setSecret(key, QJsonDocument(value).toJson(QJsonDocument::Compact));
}

QByteArray randomUrlSafe(int bytes)
{
    QByteArray raw(bytes, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(raw.data()), bytes / 4);
    return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QString origin(const QUrl &url)
{
    return url.scheme() + QStringLiteral("://") + url.authority();
}

QString pathOf(const QUrl &url)
{
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

using Json = std::function<void(const QJsonObject &json, const QString &error)>;
using Form = QList<QPair<QString, QString>>;

void finish(QNetworkReply *reply, const Json &then)
{
    reply->deleteLater();
    const QByteArray body = reply->readAll();
    const QJsonObject json = QJsonDocument::fromJson(body).object();
    if (reply->error() != QNetworkReply::NoError || json.isEmpty()) {
        then(json, QStringLiteral("HTTP %1: %2")
                       .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                       .arg(QString::fromUtf8(body.left(200)).simplified()));
        return;
    }
    then(json, {});
}

// The first of `urls` that answers with a JSON object.
void getFirst(QNetworkAccessManager *net, QObject *context, QStringList urls, Json then)
{
    if (urls.isEmpty()) {
        QTimer::singleShot(0, context, [then]() { then({}, QStringLiteral("no metadata found")); });
        return;
    }
    QNetworkRequest req{QUrl(urls.takeFirst())};
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("MCP-Protocol-Version", "2026-07-28");
    req.setTransferTimeout(15000);
    QNetworkReply *reply = net->get(req);
    QObject::connect(reply, &QNetworkReply::finished, context, [net, context, urls, then, reply]() {
        finish(reply, [net, context, urls, then](const QJsonObject &json, const QString &error) {
            if (error.isEmpty())
                then(json, {});
            else
                getFirst(net, context, urls, then);
        });
    });
}

void post(QNetworkAccessManager *net, QObject *context, const QString &url, const QByteArray &type,
          const QByteArray &body, Json then)
{
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, type);
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(30000);
    QNetworkReply *reply = net->post(req, body);
    QObject::connect(reply, &QNetworkReply::finished, context, [reply, then]() { finish(reply, then); });
}

void postForm(QNetworkAccessManager *net, QObject *context, const QString &url, const Form &form, Json then)
{
    // Every value percent-encoded: a '+' left bare in a code reads back as a space.
    QByteArrayList pairs;
    for (const auto &[k, v] : form)
        pairs.append(QUrl::toPercentEncoding(k) + '=' + QUrl::toPercentEncoding(v));
    post(net, context, url, "application/x-www-form-urlencoded", pairs.join('&'), then);
}

// Where RFC 8414 and OpenID Connect put an issuer's metadata.
QStringList metadataUrls(const QString &issuer)
{
    const QUrl u(issuer);
    return {origin(u) + QStringLiteral("/.well-known/oauth-authorization-server") + pathOf(u),
            origin(u) + QStringLiteral("/.well-known/openid-configuration") + pathOf(u),
            origin(u) + pathOf(u) + QStringLiteral("/.well-known/openid-configuration")};
}

void keepTokens(const QString &serverUrl, QJsonObject entry, const QJsonObject &answer)
{
    for (const char *k : {"access_token", "refresh_token"}) {
        const QString v = answer.value(QLatin1String(k)).toString();
        if (!v.isEmpty())
            entry.insert(QLatin1String(k), v);
    }
    const qint64 in = answer.value(QStringLiteral("expires_in")).toInteger();
    entry.insert(QStringLiteral("expires"), in > 0 ? QDateTime::currentSecsSinceEpoch() + in : 0);
    save(keyFor(QStringLiteral("token"), serverUrl), entry);
}

using Client = std::function<void(const QString &id, const QString &secret, const QString &error)>;

// BurnOS's registration at this issuer: kept, or made now by dynamic client
// registration. shortcut: no Client ID Metadata Document (it needs a public
// https URL BurnOS does not host); a server that only takes those cannot be signed in to yet.
void withClient(QNetworkAccessManager *net, QObject *context, const QString &issuer, const QJsonObject &as,
                Client then)
{
    const QString key = keyFor(QStringLiteral("client"), issuer);
    const QJsonObject kept = load(key);
    if (!kept.value(QStringLiteral("client_id")).toString().isEmpty()) {
        QTimer::singleShot(0, context, [then, kept]() {
            then(kept.value(QStringLiteral("client_id")).toString(), kept.value(QStringLiteral("client_secret")).toString(), {});
        });
        return;
    }
    const QString endpoint = as.value(QStringLiteral("registration_endpoint")).toString();
    if (endpoint.isEmpty()) {
        QTimer::singleShot(0, context, [then]() {
            then({}, {}, QObject::tr("its sign-in server does not let BurnOS register itself"));
        });
        return;
    }
    const QJsonObject request{
        {QStringLiteral("client_name"), QStringLiteral("BurnOS")},
        {QStringLiteral("redirect_uris"), QJsonArray{redirectUri()}},
        {QStringLiteral("grant_types"), QJsonArray{QStringLiteral("authorization_code"), QStringLiteral("refresh_token")}},
        {QStringLiteral("response_types"), QJsonArray{QStringLiteral("code")}},
        {QStringLiteral("token_endpoint_auth_method"), QStringLiteral("none")},
        {QStringLiteral("application_type"), QStringLiteral("native")},
    };
    post(net, context, endpoint, "application/json", QJsonDocument(request).toJson(QJsonDocument::Compact),
         [key, then](const QJsonObject &json, const QString &error) {
        const QString id = json.value(QStringLiteral("client_id")).toString();
        if (!error.isEmpty() || id.isEmpty()) {
            then({}, {}, QObject::tr("registering BurnOS failed (%1)").arg(error));
            return;
        }
        save(key, json);
        then(id, json.value(QStringLiteral("client_secret")).toString(), {});
    });
}

void authorize(QNetworkAccessManager *net, QObject *context, const QString &serverUrl, const QString &issuer,
               const QJsonObject &as, const QString &resource, const QString &scope, const QString &clientId,
               const QString &clientSecret, Done done)
{
    auto *listener = new QTcpServer(context);
    if (!listener->listen(QHostAddress::LocalHost, kPort)) {
        listener->deleteLater();
        QTimer::singleShot(0, context, [done]() {
            done(false, QObject::tr("Port %1 is in use - another sign-in is open. Finish it and try again.").arg(kPort));
        });
        return;
    }
    const QByteArray verifier = randomUrlSafe(32);
    const QString state = QString::fromLatin1(randomUrlSafe(16));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    q.addQueryItem(QStringLiteral("client_id"), clientId);
    q.addQueryItem(QStringLiteral("redirect_uri"), redirectUri());
    q.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(
        QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
    q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    q.addQueryItem(QStringLiteral("state"), state);
    q.addQueryItem(QStringLiteral("resource"), resource);
    if (!scope.isEmpty())
        q.addQueryItem(QStringLiteral("scope"), scope);
    QUrl url(as.value(QStringLiteral("authorization_endpoint")).toString());
    url.setQuery(q);

    const QJsonObject entry{{QStringLiteral("issuer"), issuer},
                            {QStringLiteral("client_id"), clientId},
                            {QStringLiteral("client_secret"), clientSecret},
                            {QStringLiteral("token_endpoint"), as.value(QStringLiteral("token_endpoint")).toString()},
                            {QStringLiteral("resource"), resource}};
    auto over = std::make_shared<bool>(false);
    const auto end = [listener, done, over, serverUrl](bool ok, const QString &message) {
        if (*over)
            return;
        *over = true;
        listener->close();
        listener->deleteLater();
        liveLog(QStringLiteral("mcp sign-in %1: %2").arg(serverUrl, ok ? QStringLiteral("done") : message));
        done(ok, message);
    };
    QTimer::singleShot(300000, listener, [end]() { end(false, QObject::tr("The sign-in was not finished in time.")); });
    QObject::connect(listener, &QTcpServer::newConnection, listener, [=]() {
        QTcpSocket *sock = listener->nextPendingConnection();
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [=]() {
            const QUrlQuery got(QUrl(QString::fromLatin1(sock->readLine().split(' ').value(1))).query());
            const QString iss = got.queryItemValue(QStringLiteral("iss"), QUrl::FullyDecoded);
            // RFC 9207: an issuer the answer names must be the one asked.
            const bool ours = got.queryItemValue(QStringLiteral("state")) == state
                              && got.hasQueryItem(QStringLiteral("code")) && (iss.isEmpty() || iss == issuer);
            sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<p>");
            sock->write(ours ? "Signed in to BurnOS. You can close this tab." : "Sign-in failed - try again in BurnOS.");
            sock->disconnectFromHost();
            if (!ours)
                return; // a favicon or a stale tab: keep waiting
            Form form{{QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
                      {QStringLiteral("code"), got.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded)},
                      {QStringLiteral("redirect_uri"), redirectUri()},
                      {QStringLiteral("client_id"), clientId},
                      {QStringLiteral("code_verifier"), QString::fromLatin1(verifier)},
                      {QStringLiteral("resource"), resource}};
            if (!clientSecret.isEmpty())
                form.append({QStringLiteral("client_secret"), clientSecret});
            postForm(net, listener, entry.value(QStringLiteral("token_endpoint")).toString(), form,
                     [=](const QJsonObject &json, const QString &error) {
                if (!error.isEmpty() || json.value(QStringLiteral("access_token")).toString().isEmpty()) {
                    end(false, QObject::tr("The sign-in server refused: %1").arg(error));
                    return;
                }
                keepTokens(serverUrl, entry, json);
                end(true, QObject::tr("Signed in."));
            });
        });
    });
    QDesktopServices::openUrl(url);
}

} // namespace

QString token(const QString &serverUrl)
{
    const QJsonObject t = load(keyFor(QStringLiteral("token"), serverUrl));
    const qint64 expires = t.value(QStringLiteral("expires")).toInteger();
    if (expires && expires < QDateTime::currentSecsSinceEpoch() + 60)
        return {};
    return t.value(QStringLiteral("access_token")).toString();
}

bool canRefresh(const QString &serverUrl)
{
    return !load(keyFor(QStringLiteral("token"), serverUrl)).value(QStringLiteral("refresh_token")).toString().isEmpty();
}

void signIn(QNetworkAccessManager *net, QObject *context, const QString &serverUrl, const QString &challenge,
            Done done)
{
    const QUrl server(serverUrl);
    QStringList where;
    const QRegularExpressionMatch named = QRegularExpression(QStringLiteral("resource_metadata=\"([^\"]+)\"")).match(challenge);
    if (named.hasMatch())
        where << named.captured(1);
    where << origin(server) + QStringLiteral("/.well-known/oauth-protected-resource") + pathOf(server)
          << origin(server) + QStringLiteral("/.well-known/oauth-protected-resource");
    const QString asked = QRegularExpression(QStringLiteral("scope=\"([^\"]+)\"")).match(challenge).captured(1);
    getFirst(net, context, where, [=](const QJsonObject &resource, const QString &error) {
        const QJsonArray servers = resource.value(QStringLiteral("authorization_servers")).toArray();
        const QString issuer = servers.isEmpty() ? QString() : servers.at(0).toString();
        if (!error.isEmpty() || issuer.isEmpty()) {
            done(false, QObject::tr("%1 does not say where to sign in (%2).").arg(serverUrl, error));
            return;
        }
        QString scope = asked;
        if (scope.isEmpty()) {
            for (const QJsonValue &s : resource.value(QStringLiteral("scopes_supported")).toArray())
                scope += (scope.isEmpty() ? QString() : QStringLiteral(" ")) + s.toString();
        }
        const QString res = resource.value(QStringLiteral("resource")).toString(serverUrl);
        getFirst(net, context, metadataUrls(issuer), [=](const QJsonObject &as, const QString &error) {
            if (!error.isEmpty() || as.value(QStringLiteral("authorization_endpoint")).toString().isEmpty()) {
                done(false, QObject::tr("The sign-in server %1 did not describe itself (%2).").arg(issuer, error));
                return;
            }
            withClient(net, context, issuer, as, [=](const QString &id, const QString &secret, const QString &error) {
                if (!error.isEmpty()) {
                    done(false, QObject::tr("Cannot sign in to %1: %2.").arg(serverUrl, error));
                    return;
                }
                authorize(net, context, serverUrl, issuer, as, res, scope, id, secret, done);
            });
        });
    });
}

void refresh(QNetworkAccessManager *net, QObject *context, const QString &serverUrl, Done done)
{
    const QJsonObject entry = load(keyFor(QStringLiteral("token"), serverUrl));
    Form form{{QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
              {QStringLiteral("refresh_token"), entry.value(QStringLiteral("refresh_token")).toString()},
              {QStringLiteral("client_id"), entry.value(QStringLiteral("client_id")).toString()},
              {QStringLiteral("resource"), entry.value(QStringLiteral("resource")).toString()}};
    if (!entry.value(QStringLiteral("client_secret")).toString().isEmpty())
        form.append({QStringLiteral("client_secret"), entry.value(QStringLiteral("client_secret")).toString()});
    postForm(net, context, entry.value(QStringLiteral("token_endpoint")).toString(), form,
             [serverUrl, entry, done](const QJsonObject &json, const QString &error) {
        if (!error.isEmpty() || json.value(QStringLiteral("access_token")).toString().isEmpty()) {
            // A refresh token that no longer works is dropped, so the user is asked to sign in again.
            QJsonObject left = entry;
            left.remove(QStringLiteral("refresh_token"));
            left.remove(QStringLiteral("access_token"));
            save(keyFor(QStringLiteral("token"), serverUrl), left);
            done(false, error);
            return;
        }
        keepTokens(serverUrl, entry, json);
        done(true, {});
    });
}

} // namespace BurnOS::McpAuth
