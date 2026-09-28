#include "livelog.h"
#include "openrouter.h"
#include "settings.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QUrl>

namespace {

constexpr auto kUrl = "https://openrouter.ai/api/v1/chat/completions";

// How much of the conversation travels with every question. Spoken turns are
// short so this is generous, but an orb left running for a week must not send a
// week of talk each time it is asked something.
constexpr int kMaxMessages = 24;   // twelve exchanges
constexpr int kMaxChars    = 24000;

// The answer is SPOKEN, and the voice is synthesised locally at roughly real
// time, so every extra word is another second the user waits before hearing
// anything. Brevity is not taste here, it is the difference between a
// conversation and a lecture.
constexpr auto kSystemPrompt =
    "You are Burnos, a voice assistant. The user is talking to you out loud and "
    "you are answering out loud, back and forth, like a conversation between two "
    "people in a room.\n"
    "\n"
    "Your reply is read aloud by a text to speech voice at about the speed of "
    "ordinary speech, so length costs the user real time. Answer in ONE sentence "
    "wherever one will do, and never more than two. The only exception is when "
    "they have actually asked for something that cannot be said briefly.\n"
    "\n"
    "Say the answer first and stop. No preamble, no repeating the question back, "
    "no offering further help, no sign off. If you do not know, say so in a few "
    "words.\n"
    "\n"
    "YOU CAN OPERATE THIS WINDOWS MACHINE, through the computer_use tool. Use it "
    "whenever the user is telling you to DO something rather than asking you "
    "something: opening a program, running a command, clicking something on "
    "screen, pressing a key, typing into a field, or asking what is currently "
    "open. Work out what they meant - \"open my editor\" or \"bring up the "
    "browser\" means starting a program - and open it with the launch action, "
    "giving the program's NAME. Do not write a command line to open something: "
    "many apps on this machine have no command line at all and launch is what "
    "finds them. Never tell the user you have no access to their computer. "
    "Never claim to have done something you have not actually done through the "
    "tool.\n"
    "\n"
    "AFTER ANYTHING THAT CHANGES THE SCREEN, the list of controls you were "
    "given is out of date. launch hands you the new window's controls itself; "
    "after a click that opens a page or a dialog, call list_controls before you "
    "try to click or type into what appeared.\n"
    "\n"
    "CHECK YOUR OWN WORK WITH YOUR EYES BEFORE YOU REPORT IT. After any action "
    "meant to change something - launch, click, click_at, type_text, press_key "
    "- take a screenshot and LOOK at it before you say anything to the user. A "
    "tool result saying the click was sent does NOT mean the program opened, "
    "the dialog appeared, the field took the text, or that what the user "
    "actually wanted has happened. Only say it is done once you have SEEN it "
    "done. If the screenshot does not show what you expected, put it right - "
    "try another control, look again, or take a different route - rather than "
    "reporting success; a program can take a few seconds to appear, so if it "
    "looks empty or half drawn, take one more screenshot before deciding it "
    "failed. If it truly did not work, say plainly what you saw instead. The "
    "screenshot is for YOU, not for the user: never describe it unless they "
    "asked what is on screen.\n"
    "\n"
    "When a command prints something the user asked for, tell them the ANSWER, "
    "not the raw output - they are listening, not reading a terminal.\n"
    "\n"
    "When a system message lists what is on screen, those are the controls you "
    "can click or type into by name, and you must give back the key exactly as "
    "it appears there, such as c7.\n"
    "\n"
    "YOU CAN ALSO SEE. If the list does not hold what the user meant, or they "
    "ask what is on screen, take a screenshot and look at it. Having looked, "
    "click a point in it with click_at, giving x and y in THAT PICTURE's own "
    "pixels measured from its top left corner. Do not guess at a point you have "
    "not seen, and do not describe the whole screen unless you were asked to - "
    "look, act, then say one short sentence about what you did.\n"
    "\n"
    "This is a continuing conversation and you remember what has already been "
    "said in it. Refer back to it naturally, work out what 'it', 'that' and "
    "'they' mean from what came before, and never ask the user to repeat "
    "something they have already told you.\n"
    "\n"
    "Never use markdown, bullet points, headings, numbered lists, code blocks, "
    "emoji or decorative symbols: every character you write is pronounced "
    "literally. Write numbers, units and abbreviations the way they should be "
    "said out loud.";

// The app_action tool: everything beyond this machine. Offered ONLY when the
// user has actually connected something, because a tool for an empty account
// can do nothing but waste a turn failing.
//
// TWO VERBS, and the order matters. Composio has tens of thousands of tools and
// their slugs cannot be guessed, so the model searches for one in plain words,
// gets back real slugs with the arguments each needs, and only then runs one.
QJsonObject appActionTool()
{
    const QJsonObject parameters{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("action"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("find_tools"), QStringLiteral("run_tool")}},
                {QStringLiteral("description"),
                 QStringLiteral("find_tools searches the connected apps for "
                                "something that can do what the user asked and "
                                "gives you back real tool slugs with the "
                                "arguments each one needs. run_tool carries one "
                                "of them out. ALWAYS find_tools first: tool "
                                "slugs cannot be guessed and an invented one "
                                "simply fails.")}}},
            {QStringLiteral("query"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"),
                 QStringLiteral("For find_tools. What you want to do, in a few "
                                "plain words: send an email, post a message to "
                                "a channel, create a calendar event, open an "
                                "issue.")}}},
            {QStringLiteral("tool"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"),
                 QStringLiteral("For run_tool. The slug exactly as find_tools "
                                "gave it, such as GMAIL_SEND_EMAIL. Never one "
                                "you have not been given.")}}},
            {QStringLiteral("arguments"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("additionalProperties"), true},
                {QStringLiteral("description"),
                 QStringLiteral("For run_tool. The tool's own arguments, using "
                                "the names find_tools listed. Fill in every one "
                                "it said was needed; if you are missing "
                                "something only the user knows, such as who the "
                                "message is for, ask them instead of "
                                "guessing.")}}},
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("action")}},
    };

    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("app_action")},
            {QStringLiteral("description"),
             QStringLiteral("Do something in one of the user's connected apps - "
                            "send an email, post to a chat channel, add a "
                            "calendar event, write a page, open an issue, look "
                            "something up in one of their accounts.")},
            {QStringLiteral("parameters"), parameters},
        }},
    };
}

// The tools offered this turn. computer_use always; app_action as well once at
// least one app is connected. A single function with an action rather than one
// function per verb, because the model picks an action far more reliably than it
// picks between a dozen near identical function names.
QJsonArray tools(const QStringList &apps)
{
    const QJsonObject parameters{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("action"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("launch"), QStringLiteral("run_command"),
                    QStringLiteral("list_windows"), QStringLiteral("list_controls"),
                    QStringLiteral("screenshot"), QStringLiteral("click"),
                    QStringLiteral("click_at"), QStringLiteral("press_key"),
                    QStringLiteral("type_text")}},
                {QStringLiteral("description"),
                 QStringLiteral("launch OPENS A PROGRAM BY ITS NAME and is the only "
                                "way you should open one - it finds Store apps, which "
                                "have no command line at all, and it tells you the new "
                                "window's controls once it is up. run_command runs a "
                                "Windows command line and gives you back what it "
                                "printed; use it for commands, NOT for opening "
                                "programs. list_windows says what is open. "
                                "list_controls re-reads what a window offers, which you "
                                "need after anything that changes the screen. "
                                "screenshot SHOWS YOU a window so you can see it for "
                                "yourself. click presses a named control. click_at "
                                "clicks a point you picked out of a screenshot. "
                                "press_key presses one key. type_text types into a "
                                "field.")}}},
            {QStringLiteral("name"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"),
                 QStringLiteral("For launch. What the program is called, as a person "
                                "would say it: Microsoft Store, Firefox, Notepad, "
                                "Spotify, Calculator. Work out the real name from what "
                                "the user said - 'my browser' is whichever browser is "
                                "installed - and give that, not a command line.")}}},
            {QStringLiteral("window"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"),
                 QStringLiteral("For screenshot and list_controls. Part of the title of "
                                "the window meant. Leave it out for whatever is in "
                                "front.")}}},
            {QStringLiteral("x"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("description"),
                 QStringLiteral("For click_at. How far ACROSS the screenshot the point "
                                "is, in that picture's own pixels, from its left edge.")}}},
            {QStringLiteral("y"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("description"),
                 QStringLiteral("For click_at. How far DOWN the screenshot the point is, "
                                "in that picture's own pixels, from its top edge.")}}},
            {QStringLiteral("command"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"),
                 QStringLiteral("For run_command. The command line, exactly as it would "
                                "be typed at a Windows prompt: dir, tasklist, ipconfig, "
                                "and so on. DO NOT use this to open a program - use the "
                                "launch action for that, because a Store app has no "
                                "command line to start and this will simply fail.")}}},
            {QStringLiteral("control"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"),
                 QStringLiteral("For click and type_text. The key of the control from "
                                "the on screen list, exactly as shown, such as c7.")}}},
            {QStringLiteral("key"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("enter"), QStringLiteral("escape"),
                    QStringLiteral("tab"), QStringLiteral("space"),
                    QStringLiteral("backspace"), QStringLiteral("delete"),
                    QStringLiteral("up"), QStringLiteral("down"),
                    QStringLiteral("left"), QStringLiteral("right"),
                    QStringLiteral("home"), QStringLiteral("end"),
                    QStringLiteral("pageup"), QStringLiteral("pagedown"),
                    QStringLiteral("f5")}},
                {QStringLiteral("description"), QStringLiteral("For press_key.")}}},
            {QStringLiteral("text"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("description"),
                 QStringLiteral("For type_text. Exactly the words to type, and "
                                "nothing else - no leading \"type\" and no full stop "
                                "unless the user meant one.")}}},
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("action")}},
    };

    QJsonArray list;
    list.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("computer_use")},
            {QStringLiteral("description"),
             QStringLiteral("Operate the Windows machine Burnos is running on.")},
            {QStringLiteral("parameters"), parameters},
        }},
    });

    if (!apps.isEmpty())
        list.append(appActionTool());

    return list;
}

} // namespace

OpenRouterClient::OpenRouterClient(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

bool OpenRouterClient::ask(const QString &prompt, const QString &screenContext)
{
    if (prompt.trimmed().isEmpty()) {
        emit failed(tr("Nothing was said."));
        return false;
    }

    if (m_busy) {
        BurnOS::liveLog(QStringLiteral("ask REFUSED: still waiting on the previous answer"));
        emit failed(tr("Still waiting on the previous answer."));
        return false;
    }

    m_pending = prompt.trimmed();
    m_context = screenContext;
    m_awaitingTool = false;
    m_callId.clear();

    m_turn = QJsonArray();
    m_turn.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                              {QStringLiteral("content"), m_pending}});

    return post();
}

void OpenRouterClient::finishTool(const QString &result, const QByteArray &png)
{
    if (!m_awaitingTool)
        return;

    m_awaitingTool = false;
    m_turn.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("tool")},
        {QStringLiteral("tool_call_id"), m_callId},
        {QStringLiteral("content"), result},
    });
    m_callId.clear();

    if (!png.isEmpty()) {
        // A tool message cannot carry an image, so the picture arrives straight
        // afterwards as a user message with a data URL in it. Images are never
        // folded into the history: only the question and the spoken answer are,
        // so a conversation cannot grow into megabytes of screenshots.
        QJsonArray parts;
        parts.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), QStringLiteral("Here is that screenshot.")}});
        parts.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("image_url")},
            {QStringLiteral("image_url"), QJsonObject{
                {QStringLiteral("url"), QStringLiteral("data:image/png;base64,")
                                            + QString::fromLatin1(png.toBase64())}}}});
        m_turn.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                  {QStringLiteral("content"), parts}});
    }

    BurnOS::liveLog(QStringLiteral("tool result handed back: %1%2")
                        .arg(result.left(120))
                        .arg(png.isEmpty()
                                 ? QString()
                                 : QStringLiteral(" (+ %1 KB screenshot)")
                                       .arg(png.size() / 1024)));
    post();
}

bool OpenRouterClient::post()
{
    const QString key = BurnOS::Settings::openRouterKey();
    if (key.isEmpty()) {
        BurnOS::liveLog(QStringLiteral("ask REFUSED: no OpenRouter key stored"));
        emit failed(tr("No OpenRouter key stored. Right click the orb and open Settings to add one."));
        endTurn();
        return false;
    }

    // Which apps the user has connected, read back at the moment it is used
    // like every other setting. Empty means the app_action tool is not offered
    // at all, so the model cannot promise something it has no way to do.
    const QStringList apps = BurnOS::Settings::hasComposioKey()
                                 ? BurnOS::Settings::composioApps()
                                 : QStringList();

    QJsonArray messages;
    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                {QStringLiteral("content"),
                                 QString::fromLatin1(kSystemPrompt)}});

    // NAMING THE APPS MATTERS. Told merely that it has apps, the model invents a
    // tool slug and the call fails; told that Gmail and Slack are connected, it
    // searches for the right one first. This is a system message rather than
    // part of the prompt above because the list is whatever the user set up in
    // Manage integrations and it changes without a rebuild.
    if (!apps.isEmpty()) {
        messages.append(QJsonObject{
            {QStringLiteral("role"), QStringLiteral("system")},
            {QStringLiteral("content"),
             tr("THESE APPS ARE CONNECTED and you can act in them through the "
                "app_action tool: %1. Nothing else is connected, so never offer "
                "to use an app that is not on this list - say it has to be "
                "connected in Settings, under Manage integrations. Call "
                "find_tools first and then run_tool with a slug that search "
                "actually gave you back; never invent a tool name. Say what you "
                "did in one short sentence, not the raw result.")
                 .arg(apps.join(QStringLiteral(", ")))}});
    }

    if (!m_context.isEmpty()) {
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                    {QStringLiteral("content"), m_context}});
    }
    for (const QJsonValue &turn : std::as_const(m_history))
        messages.append(turn);
    for (const QJsonValue &turn : std::as_const(m_turn))
        messages.append(turn);

    const QJsonObject body{
        {QStringLiteral("model"), BurnOS::Settings::openRouterModel()},
        {QStringLiteral("messages"), messages},
        // A reasoning model spends tokens thinking before it writes anything,
        // so a small budget comes back with an empty content field.
        {QStringLiteral("max_tokens"), BurnOS::Settings::replyTokens()},
        {QStringLiteral("tools"), tools(apps)},
    };

    QNetworkRequest request{QUrl(QString::fromLatin1(kUrl))};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    request.setRawHeader("X-Title", "Burnos");
    request.setTransferTimeout(60000);

    BurnOS::liveLog(QStringLiteral("asking %1, budget %2 tokens, %3 turn(s) of context: %4")
                        .arg(BurnOS::Settings::openRouterModel())
                        .arg(BurnOS::Settings::replyTokens())
                        .arg(turnsRemembered())
                        .arg(m_pending.left(120)));

    QElapsedTimer clock;
    clock.start();

    m_busy = true;
    QNetworkReply *reply =
        m_net->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_reply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, clock]() {
        reply->deleteLater();
        m_reply = nullptr;
        m_busy = false;

        const QByteArray raw = reply->readAll();

        if (reply->error() != QNetworkReply::NoError) {
            const QString detail = QString::fromUtf8(raw).simplified().left(300);
            BurnOS::liveLog(QStringLiteral("openrouter FAILED after %1 ms: %2")
                                .arg(clock.elapsed())
                                .arg(reply->errorString()));
            emit failed(detail.isEmpty() ? reply->errorString()
                                         : tr("%1 - %2").arg(reply->errorString(), detail));
            endTurn();
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(raw).object();
        const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty()) {
            BurnOS::liveLog(QStringLiteral("openrouter returned no choices after %1 ms")
                                .arg(clock.elapsed()));
            emit failed(tr("No choices in the reply."));
            endTurn();
            return;
        }

        const QJsonObject message = choices.first().toObject()
                                        .value(QStringLiteral("message")).toObject();

        // A tool call means the turn is only half done.
        const QJsonArray calls = message.value(QStringLiteral("tool_calls")).toArray();
        if (!calls.isEmpty()) {
            // The assistant message goes back verbatim next time: the API wants
            // the tool result attached to the call that asked for it.
            m_turn.append(message);

            const QJsonObject call = calls.first().toObject();
            m_callId = call.value(QStringLiteral("id")).toString();
            const QJsonObject fn = call.value(QStringLiteral("function")).toObject();
            const QVariantMap args =
                QJsonDocument::fromJson(fn.value(QStringLiteral("arguments"))
                                            .toString().toUtf8())
                    .object().toVariantMap();

            m_awaitingTool = true;
            BurnOS::liveLog(QStringLiteral("model called %1 in %2 ms: %3")
                                .arg(fn.value(QStringLiteral("name")).toString())
                                .arg(clock.elapsed())
                                .arg(fn.value(QStringLiteral("arguments")).toString().left(160)));
            emit toolRequested(fn.value(QStringLiteral("name")).toString(), args);
            return;
        }

        QString text = message.value(QStringLiteral("content")).toString().trimmed();

        // Some reasoning models leave content empty and put the answer in
        // reasoning instead. Better to speak that than to say nothing.
        if (text.isEmpty())
            text = message.value(QStringLiteral("reasoning")).toString().trimmed();

        if (text.isEmpty()) {
            BurnOS::liveLog(QStringLiteral("openrouter answered EMPTY after %1 ms")
                                .arg(clock.elapsed()));
            emit failed(tr("Empty answer. Raw: %1")
                            .arg(QString::fromUtf8(raw).simplified().left(400)));
            endTurn();
            return;
        }

        // Only now is the turn worth keeping - and only the question and the
        // answer, never the tool plumbing, which would be stale next time.
        remember(QStringLiteral("user"), m_pending);
        remember(QStringLiteral("assistant"), text);
        endTurn();
        trimHistory();

        BurnOS::liveLog(QStringLiteral("openrouter replied in %1 ms, %2 chars, %3 turn(s) kept: %4")
                            .arg(clock.elapsed())
                            .arg(text.size())
                            .arg(turnsRemembered())
                            .arg(text.simplified().left(160)));
        emit replied(text);
    });

    return true;
}

void OpenRouterClient::endTurn()
{
    m_turn = QJsonArray();
    m_pending.clear();
    m_context.clear();
    m_callId.clear();
    m_awaitingTool = false;
}

void OpenRouterClient::remember(const QString &role, const QString &text)
{
    if (text.trimmed().isEmpty())
        return;
    m_history.append(QJsonObject{{QStringLiteral("role"), role},
                                 {QStringLiteral("content"), text.trimmed()}});
}

void OpenRouterClient::trimHistory()
{
    auto characters = [this]() {
        int total = 0;
        for (const QJsonValue &turn : std::as_const(m_history)) {
            total += turn.toObject().value(QStringLiteral("content"))
                         .toString().size();
        }
        return total;
    };

    // Dropped two at a time, so what is left always starts on a question.
    while (m_history.size() > kMaxMessages
           || (m_history.size() > 2 && characters() > kMaxChars)) {
        m_history.removeFirst();
        m_history.removeFirst();
    }
}

void OpenRouterClient::cancel()
{
    if (m_reply) {
        BurnOS::liveLog(QStringLiteral("openrouter request abandoned"));
        // Disconnected first, so aborting cannot fire the finished handler and
        // report a failure for something we threw away on purpose.
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_busy = false;
    endTurn();
}

void OpenRouterClient::clearHistory()
{
    m_history = QJsonArray();
    endTurn();
    BurnOS::liveLog(QStringLiteral("context cleared"));
}
