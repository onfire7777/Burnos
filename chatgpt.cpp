#include "chatgpt.h"
#include "livelog.h"
#include "settings.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace BurnOS::ChatGpt {

namespace {

constexpr auto kClient = "app_EMoamEEZ73f0CkXaXp7hrann"; // OpenAI's public Codex sign-in client
constexpr auto kRedirect = "http://localhost:1455/auth/callback";
constexpr auto kTokenUrl = "https://auth.openai.com/oauth/token";

QByteArray randomUrlSafe(int bytes)
{
    QByteArray raw(bytes, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(raw.data()), bytes / 4);
    return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QJsonObject jwtClaims(const QString &jwt)
{
    return QJsonDocument::fromJson(QByteArray::fromBase64(jwt.section(QLatin1Char('.'), 1, 1).toLatin1(),
                                                          QByteArray::Base64UrlEncoding))
        .object();
}

QJsonObject login()
{
    return QJsonDocument::fromJson(Settings::chatGptLogin()).object();
}

// Keeps what a token answer gave, filling in the account and expiry from the JWTs.
QString keep(const QJsonObject &answer)
{
    QJsonObject l = login();
    for (const char *k : {"access_token", "refresh_token", "id_token"}) {
        const QString v = answer.value(QLatin1String(k)).toString();
        if (!v.isEmpty())
            l.insert(QLatin1String(k), v);
    }
    const QJsonObject auth = jwtClaims(l.value(QStringLiteral("id_token")).toString())
                                 .value(QStringLiteral("https://api.openai.com/auth")).toObject();
    if (!auth.value(QStringLiteral("chatgpt_account_id")).toString().isEmpty())
        l.insert(QStringLiteral("account"), auth.value(QStringLiteral("chatgpt_account_id")).toString());
    if (!auth.value(QStringLiteral("chatgpt_plan_type")).toString().isEmpty())
        l.insert(QStringLiteral("plan"), auth.value(QStringLiteral("chatgpt_plan_type")).toString());
    const QString email = jwtClaims(l.value(QStringLiteral("id_token")).toString()).value(QStringLiteral("email")).toString();
    if (!email.isEmpty())
        l.insert(QStringLiteral("email"), email);
    const qint64 exp = jwtClaims(l.value(QStringLiteral("access_token")).toString()).value(QStringLiteral("exp")).toInteger();
    const qint64 in = answer.value(QStringLiteral("expires_in")).toInteger();
    l.insert(QStringLiteral("expires"), exp ? exp : QDateTime::currentSecsSinceEpoch() + (in ? in : 3600));
    if (l.value(QStringLiteral("access_token")).toString().isEmpty() || l.value(QStringLiteral("account")).toString().isEmpty())
        return QStringLiteral("OpenAI's answer held no usable sign-in.");
    Settings::setChatGptLogin(QJsonDocument(l).toJson(QJsonDocument::Compact));
    return {};
}

void postToken(QNetworkAccessManager *net, QObject *context, const QUrlQuery &form,
               std::function<void(const QString &error)> then)
{
    QNetworkRequest req{QUrl(QString::fromLatin1(kTokenUrl))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    req.setTransferTimeout(30000);
    QNetworkReply *reply = net->post(req, form.toString(QUrl::FullyEncoded).toUtf8());
    QObject::connect(reply, &QNetworkReply::finished, context, [reply, then]() {
        reply->deleteLater();
        const QByteArray raw = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            then(QStringLiteral("OpenAI refused the sign-in: %1").arg(QString::fromUtf8(raw).simplified().left(200)));
            return;
        }
        then(keep(QJsonDocument::fromJson(raw).object()));
    });
}

} // namespace

bool signedIn()
{
    return !login().value(QStringLiteral("refresh_token")).toString().isEmpty();
}

QString who()
{
    const QJsonObject l = login();
    return QStringLiteral("%1 (%2)").arg(l.value(QStringLiteral("email")).toString(),
                                         l.value(QStringLiteral("plan")).toString());
}

void signOut()
{
    Settings::setChatGptLogin({});
}

void signIn(QObject *context, Done done)
{
    auto *server = new QTcpServer(context);
    if (!server->listen(QHostAddress::LocalHost, 1455)) {
        server->deleteLater();
        QTimer::singleShot(0, context, [done]() {
            done(false, QObject::tr("Port 1455 is in use - another sign-in (perhaps codex login) is open. "
                                    "Finish or close it and try again."));
        });
        return;
    }
    const QByteArray verifier = randomUrlSafe(32);
    const QByteArray challenge = QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const QString state = QString::fromLatin1(randomUrlSafe(16));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    q.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(kClient));
    q.addQueryItem(QStringLiteral("redirect_uri"), QString::fromLatin1(kRedirect));
    q.addQueryItem(QStringLiteral("scope"), QStringLiteral("openid profile email offline_access"));
    q.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challenge));
    q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    q.addQueryItem(QStringLiteral("id_token_add_organizations"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("codex_cli_simplified_flow"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("state"), state);
    QUrl url(QStringLiteral("https://auth.openai.com/oauth/authorize"));
    url.setQuery(q);

    auto *net = new QNetworkAccessManager(server);
    auto finished = std::make_shared<bool>(false);
    const auto finish = [server, done, finished](bool ok, const QString &message) {
        if (*finished)
            return;
        *finished = true;
        server->close();
        server->deleteLater();
        liveLog(QStringLiteral("chatgpt sign-in: %1").arg(ok ? QStringLiteral("done") : message));
        done(ok, message);
    };
    // Five minutes to log in, then the port is let go.
    QTimer::singleShot(300000, server, [finish]() { finish(false, QObject::tr("The sign-in was not finished in time.")); });
    QObject::connect(server, &QTcpServer::newConnection, server, [server, net, verifier, state, finish]() {
        QTcpSocket *sock = server->nextPendingConnection();
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, net, server, verifier, state, finish]() {
            const QByteArray line = sock->readLine();
            const QUrlQuery got(QUrl(QString::fromLatin1(line.split(' ').value(1))).query());
            const bool ours = got.queryItemValue(QStringLiteral("state")) == state
                              && got.hasQueryItem(QStringLiteral("code"));
            sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<p>");
            sock->write(ours ? "Signed in to Burnos. You can close this tab." : "Sign-in failed - try again in Burnos.");
            sock->disconnectFromHost();
            if (!ours)
                return; // a favicon or a stale tab: keep waiting
            QUrlQuery form;
            form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
            form.addQueryItem(QStringLiteral("code"), got.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded));
            form.addQueryItem(QStringLiteral("redirect_uri"), QString::fromLatin1(kRedirect));
            form.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(kClient));
            form.addQueryItem(QStringLiteral("code_verifier"), QString::fromLatin1(verifier));
            postToken(net, server, form, [finish](const QString &error) {
                finish(error.isEmpty(), error.isEmpty() ? who() : error);
            });
        });
    });
    QDesktopServices::openUrl(url);
}

void withAccess(QNetworkAccessManager *net, QObject *context, Got got, bool forceRefresh)
{
    const QJsonObject l = login();
    const QString refresh = l.value(QStringLiteral("refresh_token")).toString();
    if (refresh.isEmpty()) {
        QTimer::singleShot(0, context, [got]() {
            got({}, {}, QObject::tr("Not signed in to ChatGPT. Open Settings and choose Sign in with ChatGPT."));
        });
        return;
    }
    // Good for at least five more minutes: use it as it is.
    if (!forceRefresh && l.value(QStringLiteral("expires")).toInteger() > QDateTime::currentSecsSinceEpoch() + 300) {
        const QString access = l.value(QStringLiteral("access_token")).toString();
        const QString account = l.value(QStringLiteral("account")).toString();
        QTimer::singleShot(0, context, [got, access, account]() { got(access, account, {}); });
        return;
    }
    // shortcut: two requests that both find the token stale both refresh; harmless, one extra call.
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    form.addQueryItem(QStringLiteral("refresh_token"), refresh);
    form.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(kClient));
    form.addQueryItem(QStringLiteral("scope"), QStringLiteral("openid profile email"));
    postToken(net, context, form, [got](const QString &error) {
        const QJsonObject now = login();
        got(now.value(QStringLiteral("access_token")).toString(), now.value(QStringLiteral("account")).toString(),
            error.isEmpty() ? QString() : QObject::tr("The ChatGPT sign-in has expired - sign in again in Settings. %1").arg(error));
    });
}

void models(QNetworkAccessManager *net, QObject *context, Models done)
{
    QPointer<QObject> ctx(context);
    withAccess(net, context, [net, ctx, done](const QString &access, const QString &account, const QString &error) {
        if (!ctx)
            return;
        if (!error.isEmpty()) {
            done({}, error);
            return;
        }
        // shortcut: the version Codex sends; the list may change shape with later versions.
        QNetworkRequest req{QUrl(QStringLiteral("https://chatgpt.com/backend-api/codex/models?client_version=0.153.4"))};
        req.setRawHeader("Authorization", "Bearer " + access.toUtf8());
        req.setRawHeader("chatgpt-account-id", account.toUtf8());
        req.setRawHeader("originator", "codex_cli_rs");
        req.setTransferTimeout(20000);
        QNetworkReply *reply = net->get(req);
        QObject::connect(reply, &QNetworkReply::finished, ctx, [reply, done]() {
            reply->deleteLater();
            const QByteArray raw = reply->readAll();
            if (reply->error() != QNetworkReply::NoError) {
                done({}, QObject::tr("OpenAI did not answer for this sign-in: %1").arg(reply->errorString()));
                return;
            }
            QList<QPair<QString, QString>> list;
            for (const QJsonValue &v : QJsonDocument::fromJson(raw).object().value(QStringLiteral("models")).toArray()) {
                const QJsonObject m = v.toObject();
                if (m.value(QStringLiteral("visibility")).toString() != QLatin1String("list"))
                    continue;
                list.append({m.value(QStringLiteral("slug")).toString(),
                             QStringLiteral("%1 - %2").arg(m.value(QStringLiteral("display_name")).toString(),
                                                          m.value(QStringLiteral("description")).toString())});
            }
            done(list, list.isEmpty() ? QObject::tr("OpenAI listed no models for this sign-in.") : QString());
        });
    });
}

} // namespace BurnOS::ChatGpt
