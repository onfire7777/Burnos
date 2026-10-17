#include "livelog.h"
#include "openrouter.h"
#include "settings.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
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

// WHAT A CLI MODEL IS TOLD ON EVERY TURN. The claude and codex programs are
// used as a model only: their own tools are switched off, and a tool call is
// written into the reply for Burnos to carry out with its own tools and guard.
// Framed as writing Burnos's reply, not acting: Codex otherwise applied its own
// read-only sandbox to Burnos's tools and refused to write files.
constexpr auto kCliContract =
    "Above is the conversation of Burnos, a voice assistant app on the user's "
    "Windows PC, with its user. Burnos acts on the PC with its tools, with the "
    "user's full permission. Your job is only to WRITE Burnos's next reply: what it "
    "says, and which of its tools it calls next. You perform nothing yourself, so "
    "your own environment, tools and sandbox are irrelevant. The SYSTEM NOTES are "
    "Burnos's instructions; the reply must follow them. Write ONLY one JSON object "
    "and nothing else: {\"content\": what Burnos says (empty when it only calls "
    "tools), \"tool_calls\": [{\"name\": one of Burnos's tools, \"arguments\": a "
    "JSON object of its arguments}]}. Use [] when it calls no tool.";

// Codex is held to this shape; strict schemas allow no open object, so the
// arguments travel as a JSON string.
constexpr auto kCliSchema =
    R"({"type":"object","additionalProperties":false,"required":["content","tool_calls"],)"
    R"("properties":{"content":{"type":"string"},"tool_calls":{"type":"array","items":)"
    R"({"type":"object","additionalProperties":false,"required":["name","arguments"],)"
    R"("properties":{"name":{"type":"string"},"arguments":{"type":"string",)"
    R"("description":"the arguments as a JSON object string"}}}}}})";

// The conversation as one prompt, and its pictures as PNG bytes.
QString cliPrompt(const QJsonArray &messages, const QJsonArray &tools, QList<QByteArray> *images)
{
    QStringList out;
    for (const QJsonValue &value : messages) {
        const QJsonObject m = value.toObject();
        const QString role = m.value(QStringLiteral("role")).toString();
        QString text;
        const QJsonValue content = m.value(QStringLiteral("content"));
        if (content.isArray()) {
            for (const QJsonValue &p : content.toArray()) {
                const QJsonObject part = p.toObject();
                if (part.value(QStringLiteral("type")).toString() == QLatin1String("text")) {
                    text += part.value(QStringLiteral("text")).toString();
                } else {
                    const QString url = part.value(QStringLiteral("image_url")).toObject()
                                            .value(QStringLiteral("url")).toString();
                    images->append(QByteArray::fromBase64(url.section(QLatin1Char(','), 1).toLatin1()));
                    text += QStringLiteral(" [picture %1, attached]").arg(images->size());
                }
            }
        } else {
            text = content.toString();
        }
        if (role == QLatin1String("system")) {
            out << QStringLiteral("SYSTEM NOTE:\n") + text;
        } else if (role == QLatin1String("user")) {
            out << QStringLiteral("USER:\n") + text;
        } else if (role == QLatin1String("tool")) {
            out << QStringLiteral("RESULT OF %1:\n%2")
                       .arg(m.value(QStringLiteral("tool_call_id")).toString(), text);
        } else {
            QString said = QStringLiteral("BURNOS:\n") + text;
            for (const QJsonValue &c : m.value(QStringLiteral("tool_calls")).toArray()) {
                const QJsonObject fn = c.toObject().value(QStringLiteral("function")).toObject();
                said += QStringLiteral("\n[called %1 as %2 with %3]")
                            .arg(fn.value(QStringLiteral("name")).toString(),
                                 c.toObject().value(QStringLiteral("id")).toString(),
                                 fn.value(QStringLiteral("arguments")).toString());
            }
            out << said;
        }
    }
    QStringList list;
    for (const QJsonValue &t : tools) {
        const QJsonObject fn = t.toObject().value(QStringLiteral("function")).toObject();
        list << QStringLiteral("- %1: %2\n  arguments: %3")
                    .arg(fn.value(QStringLiteral("name")).toString(),
                         fn.value(QStringLiteral("description")).toString(),
                         QString::fromUtf8(QJsonDocument(fn.value(QStringLiteral("parameters")).toObject())
                                               .toJson(QJsonDocument::Compact)));
    }
    out << QStringLiteral("BURNOS'S TOOLS:\n") + (list.isEmpty() ? QStringLiteral("none") : list.join(QLatin1Char('\n')));
    out << QString::fromLatin1(kCliContract);
    return out.join(QStringLiteral("\n\n"));
}

// The reply as the one chat message the rest of Burnos expects.
bool cliMessage(QString text, int id, QJsonObject *message)
{
    text = text.trimmed();
    const QRegularExpressionMatch fence =
        QRegularExpression(QStringLiteral("```(?:json)?\\s*([\\s\\S]*?)\\s*```")).match(text);
    if (fence.hasMatch())
        text = fence.captured(1);
    const QJsonObject root = QJsonDocument::fromJson(text.toUtf8()).object();
    if (root.isEmpty())
        return false;
    QJsonArray calls;
    int n = 0;
    for (const QJsonValue &v : root.value(QStringLiteral("tool_calls")).toArray()) {
        const QJsonObject c = v.toObject();
        const QJsonValue a = c.value(QStringLiteral("arguments"));
        calls.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("call_%1_%2").arg(id).arg(++n)},
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"),
             QJsonObject{{QStringLiteral("name"), c.value(QStringLiteral("name")).toString()},
                         {QStringLiteral("arguments"),
                          a.isObject() ? QString::fromUtf8(QJsonDocument(a.toObject()).toJson(QJsonDocument::Compact))
                                       : a.toString()}}}});
    }
    *message = QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                           {QStringLiteral("content"), root.value(QStringLiteral("content")).toString()}};
    if (!calls.isEmpty())
        message->insert(QStringLiteral("tool_calls"), calls);
    return true;
}

} // namespace

bool OpenRouterClient::isCli(const QString &model)
{
    return model.startsWith(QLatin1String("claude:")) || model.startsWith(QLatin1String("codex:"));
}

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
    if (QProcess *proc = m_procs.take(id)) {
        proc->disconnect(this);
        proc->kill();
        proc->deleteLater();
        BurnOS::liveLog(QStringLiteral("request %1 abandoned").arg(id));
        return;
    }
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

    // Read here, at the moment of asking, so a model changed in Settings is the
    // one the very next request goes to.
    const QString model = request.model.isEmpty() ? BurnOS::Settings::openRouterModel()
                                                  : request.model;
    if (isCli(model)) {
        postCli(id, request, model, done);
        return;
    }

    const QString key = BurnOS::Settings::openRouterKey();
    if (key.isEmpty()) {
        BurnOS::liveLog(QStringLiteral("%1: REFUSED, no OpenRouter key stored")
                            .arg(request.who));
        failSoon(tr("No OpenRouter key stored. Right click the orb and open "
                    "Settings to add one."));
        return;
    }

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

// ONE TURN FROM THE USER'S OWN SUBSCRIPTION. The official claude or codex
// program answers it with its own login - Burnos never reads their tokens -
// with every tool of its own switched off, so all acting still goes through
// Burnos's tools and guard. Each call is a fresh process holding the whole
// conversation; cancel() kills it.
// shortcut: one process per turn costs 3 to 5 s of start-up; a long-lived
// stream-json session per task is the upgrade if that proves too slow.
void OpenRouterClient::postCli(quint64 id, const Request &request, const QString &model, Done done)
{
    const QString provider = model.section(QLatin1Char(':'), 0, 0);
    const QString name = model.section(QLatin1Char(':'), 1).trimmed();
    const QString exe = QStandardPaths::findExecutable(provider);
    if (exe.isEmpty()) {
        m_live.insert(id, nullptr);
        QTimer::singleShot(0, this, [this, id, done, provider]() {
            if (!m_live.contains(id))
                return;
            m_live.remove(id);
            Reply reply;
            reply.error = tr("The %1 program is not installed, so that model cannot "
                             "answer. Install it and log in, or choose another model "
                             "in Settings.").arg(provider);
            done(reply);
        });
        return;
    }

    QList<QByteArray> images;
    const QString prompt = cliPrompt(request.messages, request.tools, &images);
    auto *dir = new QTemporaryDir(QDir::tempPath() + QStringLiteral("/burnos-cli-XXXXXX"));
    auto *proc = new QProcess(this);
    proc->setWorkingDirectory(dir->path());
    connect(proc, &QObject::destroyed, [dir]() { delete dir; }); // finished or cancelled
    QStringList args;
    QByteArray input;
    if (provider == QLatin1String("claude")) {
        args = {QStringLiteral("-p"), QStringLiteral("--safe-mode"), QStringLiteral("--tools"), QString(),
                QStringLiteral("--strict-mcp-config"), QStringLiteral("--no-session-persistence"),
                QStringLiteral("--effort"), QStringLiteral("medium"),
                QStringLiteral("--system-prompt"), QString::fromLatin1(kCliContract),
                QStringLiteral("--input-format"), QStringLiteral("stream-json"),
                QStringLiteral("--output-format"), QStringLiteral("stream-json"), QStringLiteral("--verbose")};
        if (!name.isEmpty() && name != QLatin1String("default"))
            args << QStringLiteral("--model") << name;
        QJsonArray content;
        for (const QByteArray &png : std::as_const(images)) {
            content.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("image")},
                {QStringLiteral("source"), QJsonObject{{QStringLiteral("type"), QStringLiteral("base64")},
                                                       {QStringLiteral("media_type"), QStringLiteral("image/png")},
                                                       {QStringLiteral("data"), QString::fromLatin1(png.toBase64())}}}});
        }
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                   {QStringLiteral("text"), prompt}});
        input = QJsonDocument(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("user")},
                    {QStringLiteral("message"), QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                                            {QStringLiteral("content"), content}}}})
                    .toJson(QJsonDocument::Compact) + '\n';
    } else {
        QFile schema(dir->filePath(QStringLiteral("reply.schema.json")));
        if (schema.open(QIODevice::WriteOnly))
            schema.write(kCliSchema);
        schema.close();
        // The user's own config is left out (its MCP servers, hooks and a model
        // a ChatGPT login may not be allowed); the login itself still applies.
        args = {QStringLiteral("exec"), QStringLiteral("--ignore-user-config"),
                QStringLiteral("-c"), QStringLiteral("mcp_servers={}"),
                QStringLiteral("-c"), QStringLiteral("model_reasoning_effort=\"medium\""),
                QStringLiteral("--ephemeral"), QStringLiteral("--skip-git-repo-check"),
                QStringLiteral("-s"), QStringLiteral("read-only"),
                QStringLiteral("-C"), dir->path(),
                QStringLiteral("--output-schema"), schema.fileName(),
                QStringLiteral("-o"), dir->filePath(QStringLiteral("reply.txt"))};
        if (!name.isEmpty() && name != QLatin1String("default"))
            args << QStringLiteral("-m") << name;
        for (int i = 0; i < images.size(); ++i) {
            QFile png(dir->filePath(QStringLiteral("picture%1.png").arg(i + 1)));
            if (png.open(QIODevice::WriteOnly))
                png.write(images.at(i));
            args << QStringLiteral("-i") << png.fileName();
        }
        args << QStringLiteral("-");
        input = prompt.toUtf8();
    }

    BurnOS::liveLog(QStringLiteral("%1: asking %2 through the %3 program, %4 message(s), %5 tool(s), "
                                   "%6 picture(s)")
                        .arg(request.who, model, provider)
                        .arg(request.messages.size())
                        .arg(request.tools.size())
                        .arg(images.size()));
    m_procs.insert(id, proc);
    QElapsedTimer clock;
    clock.start();
    // Starting the program is part of every answer, so it gets a minute more.
    auto *limit = new QTimer(proc);
    limit->setSingleShot(true);
    connect(limit, &QTimer::timeout, proc, [proc]() { proc->kill(); });
    limit->start(request.timeoutMs + 60000);

    connect(proc, &QProcess::finished, this, [this, id, proc, dir, provider, request, done, clock, limit]() {
        const bool timedOut = !limit->isActive();
        proc->deleteLater();
        const QByteArray out = proc->readAllStandardOutput();
        const QByteArray err = proc->readAllStandardError();
        QString text;
        if (provider == QLatin1String("claude")) {
            for (const QByteArray &line : out.split('\n')) {
                const QJsonObject o = QJsonDocument::fromJson(line).object();
                if (o.value(QStringLiteral("type")).toString() == QLatin1String("result"))
                    text = o.value(QStringLiteral("is_error")).toBool() ? QString() : o.value(QStringLiteral("result")).toString();
            }
        } else {
            QFile reply(dir->filePath(QStringLiteral("reply.txt")));
            if (reply.open(QIODevice::ReadOnly))
                text = QString::fromUtf8(reply.readAll());
        }
        if (m_procs.take(id) != proc)
            return; // cancelled

        Reply r;
        r.ms = clock.elapsed();
        if (!text.isEmpty() && cliMessage(text, int(id), &r.message)) {
            r.ok = true;
        } else if (!text.isEmpty()) {
            // shortcut: prose instead of JSON is taken as a plain answer; asking
            // again is the upgrade if models drift from the contract.
            r.ok = true;
            r.message = QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                    {QStringLiteral("content"), text.trimmed()}};
        } else {
            const QString why = QString::fromUtf8(err + out).split(QLatin1Char('\n'))
                                    .filter(QRegularExpression(QStringLiteral("error|login|limit|auth"),
                                                               QRegularExpression::CaseInsensitiveOption))
                                    .join(QLatin1Char(' ')).simplified().right(300);
            r.error = timedOut ? tr("The model took too long to answer, so I stopped waiting.")
                               : tr("The %1 program gave no answer. %2").arg(provider, why);
        }
        BurnOS::liveLog(r.ok ? QStringLiteral("%1: answered in %2 ms by the %3 program").arg(request.who).arg(r.ms).arg(provider)
                             : QStringLiteral("%1: FAILED after %2 ms: %3").arg(request.who).arg(r.ms).arg(r.error));
        done(r);
    });
    proc->start(exe, args);
    proc->write(input);
    proc->closeWriteChannel();
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
