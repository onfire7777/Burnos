#include "livelog.h"
#include "openrouter.h"
#include "settings.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {

constexpr auto kUrl = "https://openrouter.ai/api/v1/chat/completions";

// MEDIUM, ALWAYS, whichever model is chosen and whoever is asking. The user's
// call on 2026-09-28, and deliberately NOT a setting, so it cannot drift per
// model or per install. Left out, each model decides for itself: MiMo 2.6
// Flash spent twenty seconds reasoning before one tool call, then did it again
// after every tool result.
constexpr auto kReasoningEffort = "medium";

// WHAT THE MODEL IS TOLD INSTEAD OF A PICTURE when it cannot be sent one.
// Some models on OpenRouter have no endpoint that accepts an image at all, and
// the whole request is refused with a 404 rather than the picture simply being
// ignored - which killed work that had otherwise gone perfectly.
constexpr auto kBlindNote =
    "The screenshot was taken, but the model you are running as cannot look at "
    "pictures, so there is nothing here for you to see. Do not ask for another "
    "one. Judge what happened from the window title and from list_controls "
    "instead, and if you still cannot tell, say plainly that you could not "
    "check rather than claiming it worked.";

} // namespace

OpenRouterClient::OpenRouterClient(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

QString OpenRouterClient::blindNote()
{
    return QString::fromLatin1(kBlindNote);
}

bool OpenRouterClient::canSee(const QString &model) const
{
    // Blindness is recorded against the model's NAME, so choosing a different
    // model in Settings gets its eyes back without restarting the orb.
    return !m_blind.contains(model);
}

quint64 OpenRouterClient::send(const Request &request, Done done)
{
    const quint64 id = ++m_nextId;
    post(id, request, std::move(done), false);
    return id;
}

void OpenRouterClient::cancel(quint64 id)
{
    // Out of the table first, so nothing already on its way can deliver an
    // answer for a request dropped on purpose.
    const bool known = m_live.contains(id);
    QNetworkReply *reply = m_live.take(id);
    if (!known)
        return;
    if (reply) {
        // Disconnected before aborting, so the abort cannot fire the finished
        // handler and report a failure for something thrown away deliberately.
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    BurnOS::liveLog(QStringLiteral("request %1 abandoned").arg(id));
}

void OpenRouterClient::post(quint64 id, Request request, Done done, bool retried)
{
    // A failure known before anything is sent is still delivered on the next
    // turn of the event loop, never from inside send(): the caller has not
    // even got its id back yet.
    const auto failSoon = [this, id, done](const QString &why) {
        m_live.insert(id, nullptr);
        QTimer::singleShot(0, this, [this, id, done, why]() {
            if (!m_live.contains(id))
                return; // cancelled in the meantime
            m_live.remove(id);
            Reply reply;
            reply.error = why;
            done(reply);
        });
    };

    const QString key = BurnOS::Settings::openRouterKey();
    if (key.isEmpty()) {
        BurnOS::liveLog(QStringLiteral("%1: REFUSED, no OpenRouter key stored")
                            .arg(request.who));
        failSoon(tr("No OpenRouter key stored. Right click the orb and open "
                    "Settings to add one."));
        return;
    }

    // Read here, at the moment of asking, so a model changed in Settings is the
    // one the very next request goes to.
    const QString model = request.model.isEmpty() ? BurnOS::Settings::openRouterModel()
                                                  : request.model;

    // A MODEL THAT HAS ALREADY REFUSED A PICTURE never gets another. The
    // refusal is a 404 on the whole request, not a shrug.
    if (!canSee(model)) {
        bool dropped = false;
        request.messages = dropImages(request.messages, &dropped);
    }

    QJsonObject body{
        {QStringLiteral("model"), model},
        {QStringLiteral("messages"), request.messages},
        // A reasoning model spends tokens thinking before it writes anything,
        // so a small budget comes back with an empty content field.
        {QStringLiteral("max_tokens"),
         request.maxTokens > 0 ? request.maxTokens : BurnOS::Settings::replyTokens()},
        {QStringLiteral("reasoning"),
         QJsonObject{{QStringLiteral("effort"), QString::fromLatin1(kReasoningEffort)}}},
        // The full count of what was sent and written, cached tokens
        // included - without this OpenRouter leaves the details out.
        {QStringLiteral("usage"), QJsonObject{{QStringLiteral("include"), true}}},
    };
    if (!request.tools.isEmpty())
        body.insert(QStringLiteral("tools"), request.tools);
    if (request.fastest) {
        body.insert(QStringLiteral("provider"),
                    QJsonObject{{QStringLiteral("sort"), QStringLiteral("latency")}});
    }

    QNetworkRequest net{QUrl(QString::fromLatin1(kUrl))};
    net.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    net.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    net.setRawHeader("X-Title", "Burnos");
    net.setTransferTimeout(request.timeoutMs);

    BurnOS::liveLog(QStringLiteral("%1: asking %2, reasoning %3, %4 message(s), "
                                   "%5 tool(s)%6%7")
                        .arg(request.who, model, QString::fromLatin1(kReasoningEffort))
                        .arg(request.messages.size())
                        .arg(request.tools.size())
                        .arg(request.fastest ? QStringLiteral(", fastest provider")
                                             : QString())
                        .arg(retried ? QStringLiteral(", again without pictures")
                                     : QString()));

    QElapsedTimer clock;
    clock.start();
    QNetworkReply *reply =
        m_net->post(net, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_live.insert(id, reply);

    connect(reply, &QNetworkReply::finished, this,
            [this, id, reply, clock, request, done, retried, model]() {
        reply->deleteLater();
        if (m_live.value(id) != reply)
            return; // cancelled, or already asked again
        m_live.remove(id);

        const QByteArray raw = reply->readAll();
        Reply out;
        out.ms = clock.elapsed();

        if (reply->error() != QNetworkReply::NoError) {
            const QString detail = QString::fromUtf8(raw).simplified().left(300);
            BurnOS::liveLog(QStringLiteral("%1: FAILED after %2 ms: %3 %4")
                                .arg(request.who)
                                .arg(out.ms)
                                .arg(reply->errorString(), detail.left(200)));

            // A timeout surfaces as a bare "Operation canceled", which means
            // nothing to someone who only asked a question.
            if (reply->error() == QNetworkReply::OperationCanceledError) {
                out.error = tr("The model took too long to answer, so I stopped waiting.");
                done(out);
                return;
            }

            // A BLIND MODEL MUST NOT KILL THE WORK. Take the pictures back
            // out, ask again, and never send this model another.
            if (!retried && detail.contains(QStringLiteral("support image input"))) {
                bool dropped = false;
                Request again = request;
                again.messages = dropImages(request.messages, &dropped);
                if (dropped) {
                    m_blind.insert(model);
                    BurnOS::liveLog(QStringLiteral("%1 cannot see images: asking again "
                                                   "without the screenshot, and none "
                                                   "will be sent to it again")
                                        .arg(model));
                    post(id, again, done, true);
                    return;
                }
            }

            out.error = detail.isEmpty()
                            ? reply->errorString()
                            : tr("%1 - %2").arg(reply->errorString(), detail);
            done(out);
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(raw).object();

        // Logged with every answer, so a slow turn can be told apart from a
        // slow network: thousands of these means the model thought at length.
        const QJsonObject usage = root.value(QStringLiteral("usage")).toObject();
        out.reasoningTokens = usage.value(QStringLiteral("completion_tokens_details")).toObject()
                                  .value(QStringLiteral("reasoning_tokens")).toInt();
        out.promptTokens = usage.value(QStringLiteral("prompt_tokens")).toInt();
        out.cachedTokens = usage.value(QStringLiteral("prompt_tokens_details")).toObject()
                               .value(QStringLiteral("cached_tokens")).toInt();
        out.completionTokens = usage.value(QStringLiteral("completion_tokens")).toInt();

        const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty()) {
            const QString why = root.value(QStringLiteral("error")).toObject()
                                    .value(QStringLiteral("message")).toString();
            BurnOS::liveLog(QStringLiteral("%1: no choices after %2 ms %3")
                                .arg(request.who)
                                .arg(out.ms)
                                .arg(why));
            out.error = why.isEmpty() ? tr("The model sent back nothing.") : why;
            done(out);
            return;
        }

        out.message = choices.first().toObject().value(QStringLiteral("message")).toObject();
        out.ok = true;
        // Who served it is logged too: the same model is quick from one
        // provider and slow from the next, and only one of them may cache.
        BurnOS::liveLog(QStringLiteral("%1: answered in %2 ms by %7, %3 reasoning tokens, "
                                       "%4 tokens in (%5 cached), %6 out")
                            .arg(request.who)
                            .arg(out.ms)
                            .arg(out.reasoningTokens)
                            .arg(out.promptTokens)
                            .arg(out.cachedTokens)
                            .arg(out.completionTokens)
                            .arg(root.value(QStringLiteral("provider")).toString()));
        done(out);
    });
}

QJsonArray OpenRouterClient::dropImages(const QJsonArray &messages, bool *dropped)
{
    *dropped = false;
    QJsonArray kept;
    for (const QJsonValue &value : messages) {
        const QJsonObject message = value.toObject();
        // A picture is the only thing ever sent as content PARTS rather than as
        // plain text, so that alone identifies one without unpacking it.
        if (message.value(QStringLiteral("role")).toString() == QLatin1String("user")
            && message.value(QStringLiteral("content")).isArray()) {
            *dropped = true;
            kept.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                    {QStringLiteral("content"), blindNote()}});
            continue;
        }
        kept.append(message);
    }
    return kept;
}

// ===========================================================================
// THE CATALOGUE
// ===========================================================================

namespace BurnOS {

ModelCatalogue *ModelCatalogue::instance()
{
    static ModelCatalogue catalogue;
    return &catalogue;
}

ModelCatalogue::ModelCatalogue(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

void ModelCatalogue::load()
{
    if (m_loaded || m_loading)
        return;
    m_loading = true;

    // Public and keyless, so it can be asked before a key is typed in. Fifteen
    // seconds is plenty for four hundred models on a slow line, and short
    // enough that a dead network does not leave Settings "asking" for ever.
    QNetworkRequest request{QUrl(QStringLiteral("https://openrouter.ai/api/v1/models"))};
    request.setRawHeader("X-Title", "Burnos");
    request.setTransferTimeout(15000);

    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_loading = false;
        if (reply->error() != QNetworkReply::NoError) {
            // Nothing known, so nothing refused.
            liveLog(QStringLiteral("model catalogue unavailable: %1").arg(reply->errorString()));
            return;
        }

        const QJsonArray data = QJsonDocument::fromJson(reply->readAll())
                                    .object()
                                    .value(QStringLiteral("data"))
                                    .toArray();
        int able = 0;
        int talkers = 0;
        for (const QJsonValue &value : data) {
            const QJsonObject model = value.toObject();
            ModelFacts f;
            f.slug = model.value(QStringLiteral("id")).toString();
            if (f.slug.isEmpty())
                continue;
            f.name = model.value(QStringLiteral("name")).toString();
            f.seesImages = model.value(QStringLiteral("architecture")).toObject()
                               .value(QStringLiteral("input_modalities")).toArray()
                               .contains(QStringLiteral("image"));
            f.callsTools = model.value(QStringLiteral("supported_parameters")).toArray()
                               .contains(QStringLiteral("tools"));
            able += f.canDoTheJob();
            talkers += f.canTalk();
            m_facts.insert(f.slug, f);
        }

        m_loaded = !m_facts.isEmpty();
        liveLog(QStringLiteral("model catalogue: %1 models, %2 can both see the "
                               "screen and call tools, %3 can at least talk")
                    .arg(m_facts.size())
                    .arg(able)
                    .arg(talkers));
        if (m_loaded)
            emit loaded();
    });
}

bool ModelCatalogue::knows(const QString &slug) const
{
    return m_facts.contains(slug.trimmed());
}

ModelFacts ModelCatalogue::facts(const QString &slug) const
{
    return m_facts.value(slug.trimmed());
}

bool ModelCatalogue::fitToRun(const QString &slug) const
{
    return !knows(slug) || facts(slug).canDoTheJob();
}

bool ModelCatalogue::fitToTalk(const QString &slug) const
{
    return !knows(slug) || facts(slug).canTalk();
}

QStringList ModelCatalogue::capable() const
{
    QStringList slugs;
    for (auto it = m_facts.cbegin(); it != m_facts.cend(); ++it) {
        if (it.value().canDoTheJob())
            slugs.append(it.key());
    }
    slugs.sort();
    return slugs;
}

} // namespace BurnOS
