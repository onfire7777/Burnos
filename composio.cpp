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

// REST v3.1 is Composio's current API; v3 is frozen and kept only for older
// integrations. The key rides in x-api-key, not in an Authorization header,
// which is the one thing easy to get wrong here.
constexpr auto kBase = "https://backend.composio.dev/api/v3.1";

// A tool result can be a whole mailbox page. The model only needs enough of it
// to tell the user what happened, and everything past this is a slower answer
// for no gain.
constexpr int kMaxResultChars = 2400;

// The same reasoning for a search. A raw session search runs to twenty-odd
// kilobytes of schemas, plans and guidance; what the model is handed is the
// few tools that can do the job, their arguments, and which apps still need a
// sign in - worded, and capped.
constexpr int kMaxSearchChars   = 3600;
constexpr int kMaxRelatedTools  = 4;
constexpr int kMaxArgumentChars = 900;

// Composio's own toolkit. Its tools are the session itself - search, execute,
// manage connections - so it is part of every session and has nothing to sign
// in to. Asking Composio to connect it is exactly the error that started this.
constexpr auto kBuiltInToolkit = "composio";

// The two failures worth telling apart from any other: an app that has no sign
// in, and a tool whose app is not connected yet.
constexpr auto kSlugNoAuth       = "ToolRouterV2_ToolkitsIsNoAuth";
constexpr auto kSlugNotConnected = "ToolRouterV2_NoActiveConnection";

ComposioApp readApp(const QJsonObject &o)
{
    ComposioApp app;
    app.slug = o.value(QStringLiteral("slug")).toString();
    app.name = o.value(QStringLiteral("name")).toString();
    if (app.name.isEmpty())
        app.name = app.slug;

    const QJsonObject meta = o.value(QStringLiteral("meta")).toObject();
    app.description = meta.value(QStringLiteral("description")).toString();

    // The same fact is spelled three ways across the catalogue and session
    // endpoints, and shows a fourth way as NO_AUTH being the only scheme.
    app.noAuth = o.value(QStringLiteral("no_auth")).toBool()
                 || o.value(QStringLiteral("is_no_auth")).toBool()
                 || meta.value(QStringLiteral("isNoAuth")).toBool();
    const QJsonArray schemes = o.value(QStringLiteral("auth_schemes")).toArray();
    if (!schemes.isEmpty()) {
        bool onlyNoAuth = true;
        for (const QJsonValue &scheme : schemes) {
            if (scheme.toString() != QStringLiteral("NO_AUTH"))
                onlyNoAuth = false;
        }
        app.noAuth = app.noAuth || onlyNoAuth;
    }

    app.builtIn = app.slug == QLatin1String(kBuiltInToolkit);

    const QJsonArray managed =
        o.value(QStringLiteral("composio_managed_auth_schemes")).toArray();
    app.managedAuth = !managed.isEmpty();

    return app;
}

QString shortened(const QString &text, int max)
{
    const QString flat = text.simplified();
    return flat.size() > max ? flat.left(max - 3) + QStringLiteral("...") : flat;
}

// One tool's arguments on a line the model can act on: the needed ones first,
// each with its type and a few words of what it is.
QString describeArguments(const QJsonObject &schema)
{
    const QJsonObject properties =
        schema.value(QStringLiteral("properties")).toObject();
    QStringList needed;
    for (const QJsonValue &name : schema.value(QStringLiteral("required")).toArray())
        needed.append(name.toString());

    QStringList parts;
    auto describe = [&properties, &parts](const QString &name, bool isNeeded) {
        const QJsonObject property = properties.value(name).toObject();
        QString type = property.value(QStringLiteral("type")).toString();
        // A choice of types comes as anyOf or oneOf. Shown with no type at all,
        // the model once sent a sentence where a boolean was wanted.
        if (type.isEmpty()) {
            QStringList choices;
            for (const QString &key : {QStringLiteral("anyOf"), QStringLiteral("oneOf")}) {
                for (const QJsonValue &choice : property.value(key).toArray()) {
                    const QString t = choice.toObject().value(QStringLiteral("type")).toString();
                    if (!t.isEmpty() && t != QStringLiteral("null") && !choices.contains(t))
                        choices.append(t);
                }
            }
            type = choices.join(QStringLiteral(" or "));
        }
        QString part = name;
        if (!type.isEmpty() && isNeeded)
            part += QStringLiteral(" (") + type + QStringLiteral(", needed)");
        else if (!type.isEmpty())
            part += QStringLiteral(" (") + type + QLatin1Char(')');
        else if (isNeeded)
            part += QStringLiteral(" (needed)");
        const QString what =
            shortened(property.value(QStringLiteral("description")).toString(), 90);
        if (!what.isEmpty())
            part += QStringLiteral(": ") + what;
        parts.append(part);
    };

    for (const QString &name : std::as_const(needed))
        describe(name, true);
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        if (!needed.contains(it.key()))
            describe(it.key(), false);
    }

    return shortened(parts.join(QStringLiteral("; ")), kMaxArgumentChars);
}

// A session search answer cut down to what the model needs to act on it.
// Empty when nothing at all matched.
QString summariseSearch(const QJsonObject &root)
{
    const QJsonObject schemas = root.value(QStringLiteral("tool_schemas")).toObject();

    QStringList primary;
    QStringList related;
    QStringList steps;
    QStringList pitfalls;
    for (const QJsonValue &v : root.value(QStringLiteral("results")).toArray()) {
        const QJsonObject result = v.toObject();
        for (const QJsonValue &slug :
             result.value(QStringLiteral("primary_tool_slugs")).toArray()) {
            if (!primary.contains(slug.toString()))
                primary.append(slug.toString());
        }
        for (const QJsonValue &slug :
             result.value(QStringLiteral("related_tool_slugs")).toArray()) {
            if (!related.contains(slug.toString()))
                related.append(slug.toString());
        }
        for (const QJsonValue &step :
             result.value(QStringLiteral("recommended_plan_steps")).toArray())
            steps.append(shortened(step.toString(), 200));
        for (const QJsonValue &pitfall :
             result.value(QStringLiteral("known_pitfalls")).toArray())
            pitfalls.append(shortened(pitfall.toString(), 220));
    }
    related.removeIf([&primary](const QString &slug) {
        return slug.isEmpty() || primary.contains(slug);
    });
    primary.removeAll(QString());

    if (primary.isEmpty() && related.isEmpty())
        return QString();

    auto toolLine = [&schemas](const QString &slug, int descriptionChars) {
        const QJsonObject schema = schemas.value(slug).toObject();
        QString line = QStringLiteral("- ") + slug;
        const QString what = shortened(
            schema.value(QStringLiteral("description")).toString(), descriptionChars);
        if (!what.isEmpty())
            line += QStringLiteral(": ") + what;
        const QJsonObject input = schema.value(QStringLiteral("input_schema")).toObject();
        if (!input.isEmpty()) {
            const QString arguments = describeArguments(input);
            line += QStringLiteral("\n  Arguments: ")
                    + (arguments.isEmpty() ? QStringLiteral("none") : arguments);
        }
        return line;
    };

    QStringList lines;
    lines.append(QStringLiteral("Tools that can do it. Call run_tool with ONE slug "
                                "exactly as written and every argument marked "
                                "needed:"));
    for (const QString &slug : std::as_const(primary))
        lines.append(toolLine(slug, 200));
    if (!related.isEmpty()) {
        lines.append(QStringLiteral("Related:"));
        for (int i = 0; i < related.size() && i < kMaxRelatedTools; ++i)
            lines.append(toolLine(related.at(i), 120));
    }

    // Whether an app can run right now decides whether its tools can run at
    // all, so it is said plainly, together with what to do about it.
    QStringList ready;
    QStringList missing;
    for (const QJsonValue &v :
         root.value(QStringLiteral("toolkit_connection_statuses")).toArray()) {
        const QJsonObject status = v.toObject();
        const QString toolkit = status.value(QStringLiteral("toolkit")).toString();
        if (toolkit.isEmpty())
            continue;
        if (status.value(QStringLiteral("has_active_connection")).toBool())
            ready.append(toolkit);
        else
            missing.append(toolkit);
    }
    if (!ready.isEmpty()) {
        lines.append(QStringLiteral("Ready to use now: ")
                     + ready.join(QStringLiteral(", ")) + QLatin1Char('.'));
    }
    if (!missing.isEmpty()) {
        lines.append(QStringLiteral("NOT CONNECTED: ") + missing.join(QStringLiteral(", "))
                     + QStringLiteral(". Tools of an app that is not connected "
                                      "cannot run. Call app_action with action "
                                      "connect_app and app set to that slug - it "
                                      "opens the sign in page in the user's "
                                      "browser - then tell the user to finish "
                                      "signing in there and ask again."));
    }
    if (!steps.isEmpty())
        lines.append(QStringLiteral("Suggested steps: ") + steps.mid(0, 4).join(QLatin1Char(' ')));
    if (!pitfalls.isEmpty())
        lines.append(QStringLiteral("Watch out: ") + pitfalls.mid(0, 2).join(QLatin1Char(' ')));

    QString text = lines.join(QLatin1Char('\n'));
    if (text.size() > kMaxSearchChars)
        text = text.left(kMaxSearchChars) + QStringLiteral(" ...(cut short)");
    return text;
}

} // namespace

ComposioClient::ComposioClient(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

bool ComposioClient::isConfigured()
{
    return BurnOS::Settings::hasComposioKey();
}

void ComposioClient::call(const QByteArray &verb, const QString &path,
                          const QJsonObject &body, const QString &what,
                          Handler done)
{
    const QString key = BurnOS::Settings::composioKey();
    if (key.isEmpty()) {
        BurnOS::liveLog(QStringLiteral("composio REFUSED: no key stored"));
        Reply refused;
        refused.error = tr("No Composio key stored. Open Settings, then Manage "
                           "integrations, and paste one in.");
        done(refused);
        return;
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
    BurnOS::liveLog(QStringLiteral("composio %1 %2")
                        .arg(QString::fromLatin1(verb), path));

    connect(reply, &QNetworkReply::finished, this, [this, reply, what, done]() {
        if (m_inFlight > 0)
            --m_inFlight;
        reply->deleteLater();

        const QByteArray raw = reply->readAll();
        Reply result;
        result.status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.root = QJsonDocument::fromJson(raw).object();

        if (reply->error() == QNetworkReply::NoError) {
            result.ok = true;
            done(result);
            return;
        }

        // v3.1 reports a failure as an object - message, slug, and a request
        // id Composio support can trace. The whole thing printed raw, as the
        // dialog once did, helps nobody.
        QString requestId;
        const QJsonValue error = result.root.value(QStringLiteral("error"));
        if (error.isObject()) {
            const QJsonObject e = error.toObject();
            result.error = e.value(QStringLiteral("message")).toString();
            result.errorSlug = e.value(QStringLiteral("slug")).toString();
            requestId = e.value(QStringLiteral("request_id")).toString();
        } else {
            result.error = error.toString();
        }
        if (result.error.isEmpty())
            result.error = result.root.value(QStringLiteral("message")).toString();
        if (result.error.isEmpty()
            && reply->error() == QNetworkReply::OperationCanceledError)
            result.error = tr("Composio did not answer in time.");
        if (result.error.isEmpty())
            result.error = QString::fromUtf8(raw).simplified().left(240);
        if (result.error.isEmpty())
            result.error = reply->errorString();

        BurnOS::liveLog(QStringLiteral("composio %1 FAILED, HTTP %2 %3%4: %5")
                            .arg(what, QString::number(result.status),
                                 result.errorSlug,
                                 requestId.isEmpty()
                                     ? QString()
                                     : QStringLiteral(", request ") + requestId,
                                 result.error));
        done(result);
    });
}

void ComposioClient::withSession(SessionHandler then)
{
    const QString stored = BurnOS::Settings::composioSessionId();
    if (!stored.isEmpty()) {
        then(stored, QString());
        return;
    }

    m_waitingForSession.append(then);
    if (m_creatingSession)
        return;
    m_creatingSession = true;

    // Connection management and the workbench are both switched off: Burnos
    // connects apps itself through the link endpoint, and it has no use for a
    // remote sandbox when every result ends up spoken rather than processed.
    const QJsonObject body{
        {QStringLiteral("user_id"), BurnOS::Settings::composioUserId()},
        {QStringLiteral("manage_connections"),
         QJsonObject{{QStringLiteral("enable"), false}}},
        {QStringLiteral("workbench"),
         QJsonObject{{QStringLiteral("enable"), false}}},
    };

    call("POST", QStringLiteral("/tool_router/session"), body,
         QStringLiteral("create session"), [this](const Reply &r) {
             m_creatingSession = false;

             QString id;
             QString error;
             if (r.ok) {
                 id = r.root.value(QStringLiteral("session_id")).toString();
                 if (id.isEmpty()) {
                     error = tr("Composio made a session but did not say which.");
                 } else {
                     BurnOS::Settings::setComposioSessionId(id);
                     BurnOS::liveLog(QStringLiteral("composio: session %1 created")
                                         .arg(id));
                 }
             } else {
                 error = r.error;
             }

             const QList<SessionHandler> waiting = m_waitingForSession;
             m_waitingForSession.clear();
             for (const SessionHandler &handler : waiting)
                 handler(id, error);
         });
}

void ComposioClient::callInSession(const QByteArray &verb, const QString &subPath,
                                   const QJsonObject &body, const QString &what,
                                   Handler done, bool retried)
{
    withSession([this, verb, subPath, body, what, done, retried](
                    const QString &id, const QString &error) {
        if (id.isEmpty()) {
            Reply failure;
            failure.error = error.isEmpty() ? tr("There is no Composio session.")
                                            : error;
            done(failure);
            return;
        }

        const QString path = QStringLiteral("/tool_router/session/") + id + subPath;
        call(verb, path, body, what,
             [this, verb, subPath, body, what, done, retried, id](const Reply &r) {
                 if (r.ok || retried || (r.status != 404 && r.status != 403)) {
                     done(r);
                     return;
                 }

                 // Sessions never expire, but one can be deleted in the Composio
                 // dashboard. A 404 can also be about the app rather than the
                 // session, so look before throwing the session away.
                 call("GET", QStringLiteral("/tool_router/session/") + id,
                      QJsonObject(), QStringLiteral("check session"),
                      [this, verb, subPath, body, what, done, id, r](const Reply &check) {
                          if (check.ok
                              || (check.status != 404 && check.status != 403)) {
                              done(r);
                              return;
                          }
                          BurnOS::liveLog(QStringLiteral("composio: session %1 is "
                                                         "gone, making a new one")
                                              .arg(id));
                          if (BurnOS::Settings::composioSessionId() == id)
                              BurnOS::Settings::setComposioSessionId(QString());
                          callInSession(verb, subPath, body, what, done, true);
                      });
             });
    });
}

void ComposioClient::searchApps(const QString &query, int limit)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 200)));
    // The most used first, which is a far better first screen than the
    // alphabet: that opens on things nobody has heard of.
    q.addQueryItem(QStringLiteral("sort_by"), QStringLiteral("usage"));
    if (!query.trimmed().isEmpty())
        q.addQueryItem(QStringLiteral("search"), query.trimmed());

    call("GET", QStringLiteral("/toolkits?") + q.toString(QUrl::FullyEncoded),
         QJsonObject(), QStringLiteral("app search"), [this](const Reply &r) {
             if (!r.ok) {
                 emit failed(r.error);
                 return;
             }

             QList<ComposioApp> apps;
             const QJsonArray items = r.root.value(QStringLiteral("items")).toArray();
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

    call("GET", QStringLiteral("/connected_accounts?") + q.toString(QUrl::FullyEncoded),
         QJsonObject(), QStringLiteral("connections"), [this](const Reply &r) {
             if (!r.ok) {
                 emit failed(r.error);
                 return;
             }

             QList<ComposioApp> apps;
             QStringList connectedSlugs;
             const QJsonArray items = r.root.value(QStringLiteral("items")).toArray();
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

             // Written down so the model can be told what it may reach without
             // a network round trip at the top of every question.
             BurnOS::Settings::setComposioApps(connectedSlugs);

             BurnOS::liveLog(QStringLiteral("composio: %1 connection(s), %2 active")
                                 .arg(apps.size())
                                 .arg(connectedSlugs.size()));
             emit appsReady(apps, true);
         });
}

void ComposioClient::connectApp(const QString &slug)
{
    if (slug.isEmpty())
        return;

    // Known without asking: this one is the session itself.
    if (slug == QLatin1String(kBuiltInToolkit)) {
        emit noSignInNeeded(slug);
        return;
    }

    // The session's link endpoint does what used to take three calls: it finds
    // or makes the managed auth config and hands back a hosted sign in page.
    callInSession("POST", QStringLiteral("/link"),
                  QJsonObject{{QStringLiteral("toolkit"), slug}},
                  QStringLiteral("sign in link"), [this, slug](const Reply &r) {
                      if (!r.ok) {
                          if (r.errorSlug == QLatin1String(kSlugNoAuth)) {
                              emit noSignInNeeded(slug);
                          } else if (r.status == 404) {
                              emit failed(tr("%1 has no ready made sign in on "
                                             "Composio. Its credentials have to be "
                                             "set up once in the Composio dashboard, "
                                             "under Auth Configs, and then Connect "
                                             "works here.").arg(slug));
                          } else {
                              emit failed(r.error);
                          }
                          return;
                      }

                      const QString url =
                          r.root.value(QStringLiteral("redirect_url")).toString();
                      if (url.isEmpty()) {
                          emit failed(tr("Composio did not give a sign in link for "
                                         "%1.").arg(slug));
                          return;
                      }

                      // The sign in happens in the user's own browser, where
                      // their password manager and existing sessions already
                      // are. Nothing of it comes back here: the connection
                      // simply appears on the next refresh.
                      QDesktopServices::openUrl(QUrl(url));
                      BurnOS::liveLog(QStringLiteral("composio: sign in opened for %1")
                                          .arg(slug));
                      emit linkOpened(slug);
                  });
}

void ComposioClient::disconnectApp(const QString &connectionId)
{
    if (connectionId.isEmpty())
        return;

    call("DELETE", QStringLiteral("/connected_accounts/") + connectionId,
         QJsonObject(), QStringLiteral("disconnect"),
         [this, connectionId](const Reply &r) {
             if (!r.ok) {
                 emit failed(r.error);
                 return;
             }
             BurnOS::liveLog(QStringLiteral("composio: %1 disconnected")
                                 .arg(connectionId));
             emit disconnected(connectionId);
         });
}

void ComposioClient::searchTools(const QString &useCase)
{
    const QString query = useCase.trimmed().left(1000);
    if (query.isEmpty()) {
        emit toolsFound(false, tr("The search needs a sentence saying what to do."));
        return;
    }

    const QJsonObject body{
        {QStringLiteral("queries"),
         QJsonArray{QJsonObject{{QStringLiteral("use_case"), query}}}},
    };

    callInSession("POST", QStringLiteral("/search"), body,
                  QStringLiteral("tool search"), [this](const Reply &r) {
                      if (!r.ok) {
                          emit toolsFound(false, tr("The tool search failed: %1")
                                                     .arg(r.error));
                          return;
                      }

                      const QString summary = summariseSearch(r.root);
                      const QString error = r.root.value(QStringLiteral("error")).toString();
                      if (summary.isEmpty() && !error.isEmpty()) {
                          emit toolsFound(false, tr("The tool search failed: %1")
                                                     .arg(error));
                          return;
                      }

                      BurnOS::liveLog(QStringLiteral("composio: search answered, "
                                                     "%1 chars for the model")
                                          .arg(summary.size()));
                      emit toolsFound(true, summary);
                  });
}

void ComposioClient::runTool(const QString &toolSlug, const QJsonObject &arguments)
{
    if (toolSlug.isEmpty()) {
        emit toolFinished(false, tr("No tool was named."));
        return;
    }

    const QJsonObject body{
        {QStringLiteral("tool_slug"), toolSlug},
        {QStringLiteral("arguments"), arguments},
    };

    callInSession(
        "POST", QStringLiteral("/execute"), body, QStringLiteral("run ") + toolSlug,
        [this, toolSlug](const Reply &r) {
            // A tool that ran and was refused by the far end is NOT a transport
            // error, and either way the model is told the same way: what
            // happened, in words it can say out loud.
            if (!r.ok) {
                if (r.errorSlug == QLatin1String(kSlugNotConnected)) {
                    emit toolFinished(false,
                                      tr("%1 cannot run yet: %2 Call app_action with "
                                         "action connect_app for that app - it opens "
                                         "the sign in page in the user's browser - "
                                         "and tell the user to finish signing in "
                                         "there, then ask again.")
                                          .arg(toolSlug, r.error));
                } else {
                    emit toolFinished(false, tr("%1 did not work: %2")
                                                 .arg(toolSlug,
                                                      r.error.left(kMaxResultChars)));
                }
                return;
            }

            const QString logId = r.root.value(QStringLiteral("log_id")).toString();
            const QString error = r.root.value(QStringLiteral("error")).toString();
            if (!error.isEmpty()) {
                BurnOS::liveLog(QStringLiteral("composio run %1 FAILED, log %2: %3")
                                    .arg(toolSlug, logId, error));
                emit toolFinished(false, tr("%1 did not work: %2")
                                             .arg(toolSlug, error.left(kMaxResultChars)));
                return;
            }

            const QJsonValue value = r.root.value(QStringLiteral("data"));
            QString data;
            if (value.isObject()) {
                data = QString::fromUtf8(QJsonDocument(value.toObject())
                                             .toJson(QJsonDocument::Compact));
            } else if (value.isArray()) {
                data = QString::fromUtf8(QJsonDocument(value.toArray())
                                             .toJson(QJsonDocument::Compact));
            } else if (!value.isNull() && !value.isUndefined()) {
                data = value.toVariant().toString();
            }
            if (data == QStringLiteral("{}") || data == QStringLiteral("[]"))
                data.clear();
            if (data.size() > kMaxResultChars) {
                data = data.left(kMaxResultChars)
                       + QStringLiteral(" ...(cut short, there was more)");
            }

            BurnOS::liveLog(QStringLiteral("composio run %1 succeeded, log %2, "
                                           "%3 chars back")
                                .arg(toolSlug, logId, QString::number(data.size())));
            emit toolFinished(true, data.isEmpty()
                                        ? tr("%1 ran and reported success.").arg(toolSlug)
                                        : tr("%1 ran. It returned: %2").arg(toolSlug, data));
        });
}
