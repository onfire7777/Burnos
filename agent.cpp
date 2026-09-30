#include "agent.h"

#include "composio.h"
#include "livelog.h"
#include "mcp.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLocale>
#include <QRegularExpression>
#include <QThreadPool>
#include <QTimer>

namespace BurnOS {

namespace {

// ---------------------------------------------------------------------------
// LIMITS
// ---------------------------------------------------------------------------

// The talker: one request, one sentence back. Thirty seconds is many times
// what that needs; past it the user is told rather than left on Thinking.
constexpr int kTalkTimeoutMs = 30000;

// What was said travels with every question: twelve exchanges or 24000
// characters, whichever comes first. An orb left running for a week must not
// send a week of talk with every sentence.
constexpr int kMaxMessages = 24;
constexpr int kMaxChars = 24000;

// A task is generous, because it works in the background and nobody waits on
// it. A turn in v0.3 got eight tool calls and ninety seconds, because the orb
// sat on Thinking through every one of them.
constexpr int kMaxLiveTasks = 3;
constexpr int kTaskMaxCalls = 60;
constexpr qint64 kTaskMaxMs = 20 * 60 * 1000;
// Three minutes of silence, not ninety seconds: since v0.5 a task may run on a
// substantive reasoning model, which thinks at medium effort and then writes
// up to the token budget below before a single byte comes back.
constexpr int kTaskRequestMs = 180000;
// Room for what a tool call has to carry: a paragraph to type now, a whole
// file later on.
constexpr int kTaskMaxTokens = 16000;
// Only the newest pictures stay in a task's conversation. Each one is a few
// hundred kilobytes of base64 and the model only ever needs the latest.
constexpr int kKeepShots = 3;
// One tool's answer is cut to the first number. Once the answers add up past
// the second, the older ones are cut down, so a long task cannot outgrow the
// model's context.
constexpr int kMaxToolText = 12000;
constexpr int kMaxToolTotal = 120000;
// A finished task stays in the talker's report this long, so "how did that
// go?" still has an answer.
constexpr qint64 kKeepFinishedMs = 10 * 60 * 1000;

// THE USER'S LONG-TERM MEMORY lives in Mnemosyne, over MCP. One desktop, one
// person: a single tenant and user, fixed here so every memory lands in the
// same place. The lookup before each answer gets this long before the answer
// goes ahead without it.
constexpr auto kMemoryTenant = "burnos";
constexpr auto kMemoryUser = "owner";
constexpr int kMemoryLookupMs = 3000;
constexpr double kMemoryMinScore = 0.3;

const char kMemoryPrompt[] =
    "LONG-TERM MEMORY. You have a memory that lasts beyond this conversation "
    "and beyond restarts - but ONLY what the remember tool stores. When the user "
    "asks you to remember something you MUST call remember with it written as "
    "one plain sentence about them - 'The user's favourite code editor is Qt "
    "Creator.' - and say 'Got it.'; saying 'Got it' without calling remember "
    "stores nothing at all. NEVER say you remember anything that is not in the "
    "memory notes you were given or in what recall found. When they "
    "ask what you remember, or ask something only your memory could answer "
    "and the memory notes you were given do not already hold it, call recall "
    "with what to look for, and if that finds nothing, say you do not know. "
    "When they ask you to forget something, call "
    "forget_memory with its memory id from the notes; if the notes do not show "
    "it, recall it first. Store ONLY what they ask you to remember - never the "
    "conversation itself, and never passwords, keys or card numbers. Use what "
    "you remember naturally, including in the briefs you write for tasks: 'my "
    "editor' is the editor they told you about.";

// ---------------------------------------------------------------------------
// THE TALKER'S BRIEF. It talks and it delegates; it never operates anything.
// ---------------------------------------------------------------------------
const char kTalkPrompt[] =
    "You are Burnos, a voice assistant living on the user's Windows PC. The "
    "user talks to you out loud and you answer out loud, back and forth, like "
    "two people in a room.\n"
    "\n"
    "Your reply is read aloud by a text to speech voice, so every word costs "
    "the user time. Answer in ONE short sentence wherever one will do, and "
    "never more than two unless they asked for something that cannot be said "
    "briefly. Say the answer first and stop: no preamble, no repeating the "
    "question, no offering more help, no sign off. If you do not know, say so "
    "in a few words.\n"
    "\n"
    "YOU NEVER OPERATE THE COMPUTER YOURSELF - background tasks do. Whenever "
    "the user wants something DONE - opening a program or a website, "
    "clicking, typing, reading what is on screen, running a command, looking "
    "something up online, sending a message, using one of their apps, writing "
    "or changing files or code - call start_task. A task works in the "
    "background while you carry on talking, so the user can ask you other "
    "things at the same time, and its result is read out when it finishes. "
    "Never tell the user you cannot use their computer, and never say "
    "something is done when you have only started it.\n"
    "\n"
    "Write a task's instruction as a complete brief for a capable assistant "
    "who has NOT heard this conversation: exactly what to do, with every "
    "detail it needs - names, what 'it' or 'that' refers to, where to save "
    "things, which window, site or account. Give it a title of a few words. "
    "In a brief, text to type, numbers, names and addresses are written "
    "EXACTLY as they are written down - 0.8, not zero point eight - because "
    "the task types what you give it.\n"
    "\n"
    "In the SAME reply as any tool call, say a short acknowledgement out loud. "
    "'On it.' is best; add a few words only when they confirm something the "
    "user needs to hear, such as which file or which person.\n"
    "\n"
    "If the user adds to or changes something a task is still working on, "
    "call steer_task with the change instead of starting another task. If "
    "they plainly ask for it to be stopped or cancelled, call stop_task. An "
    "exclamation, a sigh or a remark about something else - 'oh no', 'hmm', "
    "'wait' - never stops work: carry on, or ask in a few words. If a task is "
    "waiting for an answer from the user and they give it, pass the answer on "
    "with steer_task.\n"
    "\n"
    "When they ask how something is going, answer from the task list you are "
    "given, in one sentence. Everything else - questions, chat, facts, advice, "
    "arithmetic, the time - answer yourself, straight away, without a task.\n"
    "\n"
    "This is a continuing conversation and you remember what has been said in "
    "it, including what the tasks reported. Work out what 'it', 'that' and "
    "'they' mean from what came before, and never ask the user to repeat "
    "something they have already told you.\n"
    "\n"
    "Never use markdown, bullet points, headings, numbered lists, code, emoji "
    "or symbols: every character you write is pronounced. Write numbers, units "
    "and abbreviations the way they are said out loud.";

// ---------------------------------------------------------------------------
// A TASK'S BRIEF. It works; it never chats.
// ---------------------------------------------------------------------------
const char kTaskPrompt[] =
    "You are carrying out ONE task on the user's Windows PC, in the background, "
    "for Burnos, their voice assistant. Nobody reads this conversation: the "
    "user may be talking to Burnos about something else while you work, and "
    "they cannot answer you here.\n"
    "\n"
    "Do the whole task with your tools. When it is finished - or it cannot be "
    "done - reply WITHOUT calling a tool, in ONE or TWO short sentences that "
    "will be read aloud to the user: what you did, what you found, or what "
    "went wrong. When they asked for something to be looked up or read out, "
    "the answer itself is the reply. Report the outcome, not your steps. "
    "Plain spoken words only - no markdown, lists, symbols or emoji - and "
    "numbers the way they are said.\n"
    "\n"
    "If you truly need something only the user knows - which account, what to "
    "call a file, which of two things they meant - call ask_user with one "
    "short question, and carry on once the answer comes back. Never guess "
    "anything that matters, and never ask about something you can find out "
    "yourself.\n"
    "\n"
    "YOU OPERATE THIS MACHINE through the computer_use tool. Open a program "
    "with the launch action, giving the program's NAME, never a command line: "
    "many apps here have none, and launch is what finds them. Work out what "
    "was meant - 'my editor' or 'the browser' is whichever one is installed. "
    "Never claim to have done anything you have not actually done through a "
    "tool.\n"
    "\n"
    "TO OPEN A WEBSITE, USE open_url AND NOTHING ELSE. Give it the address - "
    "github.com, gmail.com - and Windows hands it to the browser the user "
    "already has open, on the profile they are signed in on. NEVER open a "
    "website by launching a browser and typing the address: that can start a "
    "fresh window on a profile they have never signed in on. Only if they "
    "named a particular browser should you launch that one and type the "
    "address in.\n"
    "\n"
    "MUSIC: to play a song, an artist, an album or a playlist, use computer_use "
    "play_music with text set to what to play and NOTHING ELSE - no launching "
    "Spotify, no searching or clicking in it. It searches Spotify on this PC, "
    "presses play on the best match and checks that it really is playing, so "
    "its answer is the proof and needs no screenshot; say what is playing in "
    "your report. If it fails, play it another way: with app_action when "
    "Spotify is connected in the user's online apps, otherwise on YouTube - "
    "open_url youtube.com/results?search_query= with the words, then click the "
    "first video. To pause, resume or skip, "
    "press_keys 'playpause', 'nexttrack' or 'prevtrack', and for the volume "
    "'volumeup', 'volumedown' or 'mute': they reach the player wherever it is.\n"
    "\n"
    "AFTER ANYTHING THAT CHANGES THE SCREEN, the list of controls you were "
    "given is out of date. launch hands you the new window's controls itself; "
    "after a click that opens a page or a dialog, call list_controls before "
    "you click or type into what appeared.\n"
    "\n"
    "CHECK YOUR OWN WORK WITH YOUR EYES. After any action meant to change "
    "something - launch, click, click_at, type_text, press_keys - take a "
    "screenshot and LOOK at it before you report anything. A tool result "
    "saying a click was sent does not mean the program opened, the dialog "
    "appeared or the field took the text. Only report something as done once "
    "you have SEEN it done. If the screenshot does not show what you expected, "
    "put it right - another control, another look, another route. A program "
    "can take a few seconds to appear, so if it looks empty or half drawn, "
    "look once more before deciding it failed. If it truly did not work, say "
    "plainly what you saw.\n"
    "\n"
    "When a system message lists what is on screen, those are the controls "
    "you can click or type into, and you must give back the key exactly as it "
    "appears there, such as c7. If the list does not hold what you need, take "
    "a screenshot and click a point in it with click_at, giving x and y in "
    "THAT PICTURE's own pixels from its top left corner. Never click a point "
    "you have not seen.\n"
    "\n"
    "THE KEYBOARD AND THE MOUSE. You work with your OWN pointer and keyboard, in "
    "the background: your clicks and keys go straight to the window, so they do "
    "not move the user's mouse or take their keyboard, and they can carry on "
    "with something else while you work. press_keys takes any shortcut - "
    "'ctrl+s', 'ctrl+l', 'alt+tab', 'win+r'. type_text with no control types "
    "wherever the window's cursor is. Every screenshot outlines the controls "
    "with their keys: click by key wherever there is one, and by point only "
    "where there is not. hold 'ctrl' or 'shift' on a click to add to a "
    "selection. If a click or some text will not take, add real true to use the "
    "physical mouse and keyboard for that one action, as a last resort. "
    "read_text gives you a page's or a document's words far more reliably than "
    "reading them off a picture.\n"
    "\n"
    "CODE AND FILES go through the files tool, never through an editor's "
    "window: read before you change anything, edit with the exact text, write "
    "whole new files. Builds, tests, git and scripts go through the shell tool, "
    "in the project's own folder, with the time a long build needs. Read what "
    "it printed; if it failed, fix the cause and run it again, until it works "
    "or you are sure it cannot. Where the user will want to see the result, "
    "open it for them.\n"
    "\n"
    "When a command prints something the user wanted, report the ANSWER, not "
    "the raw output.";

QJsonObject msg(const QString &role, const QString &text)
{
    return QJsonObject{{QStringLiteral("role"), role},
                       {QStringLiteral("content"), text}};
}

QString nowWords()
{
    return QLocale(QLocale::English)
        .toString(QDateTime::currentDateTime(),
                  QStringLiteral("h:mm AP 'on' dddd d MMMM yyyy"));
}

QString ago(qint64 ms)
{
    const qint64 seconds = qMax<qint64>(1, ms / 1000);
    if (seconds < 60)
        return QStringLiteral("%1 seconds").arg(seconds);
    const qint64 minutes = seconds / 60;
    if (minutes < 60) {
        return minutes == 1 ? QStringLiteral("a minute")
                            : QStringLiteral("%1 minutes").arg(minutes);
    }
    return QStringLiteral("%1 hours").arg(minutes / 60);
}

// "Remember that my editor is Qt Creator" -> "my editor is Qt Creator". Empty
// when the words are not an instruction to remember - a question about
// memory ("do you remember...?") is not one.
QString rememberCommand(const QString &said)
{
    QString s = said.simplified();
    while (!s.isEmpty() && !s.at(0).isLetter())
        s.remove(0, 1);
    if (s.endsWith(QLatin1Char('?')))
        return {};
    static const char *const kLeads[] = {
        "please remember that ", "please remember ", "remember that ", "remember ",
        "can you remember that ", "could you remember that ", "don't forget that ",
        "do not forget that ", "make a note that ", "keep in mind that ",
    };
    for (const char *lead : kLeads) {
        const QString prefix = QString::fromLatin1(lead);
        if (s.startsWith(prefix, Qt::CaseInsensitive)) {
            QString fact = s.mid(prefix.size()).trimmed();
            while (fact.endsWith(QLatin1Char('.')) || fact.endsWith(QLatin1Char('!')))
                fact.chop(1);
            return fact.size() >= 3 ? fact : QString();
        }
    }
    return {};
}

// Whether the words plainly ask for work to be stopped. stop_task is the one
// tool that throws work away, so it is only honoured when they do: a lighter
// talking model once heard "Oh no." - somebody reacting to something else in
// the room - and stopped a task a second into its work.
bool meansStop(const QString &said)
{
    static const QRegularExpression kStop(
        QStringLiteral("\\b(stop|cancel|abort|halt|kill|quit|enough|scrap|pause|"
                       "end (it|that|this|the)|never ?mind|forget (it|about|that)|"
                       "drop (it|that)|leave it|call it off|don'?t bother|do not bother|"
                       "don'?t do|do not do|don'?t need|do not need|no longer need)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return kStop.match(said).hasMatch();
}

// A tool's arguments: an object where the model behaved, a string of JSON
// where it did not. BOTH are taken - a step thrown away over quoting is a step
// the model has to spend again.
QJsonObject argumentsOf(const QVariant &raw)
{
    return raw.typeId() == QMetaType::QString
               ? QJsonDocument::fromJson(raw.toString().toUtf8()).object()
               : QJsonObject::fromVariantMap(raw.toMap());
}

// Given as a number where the model behaved and as "2" where it did not.
int taskNumber(const QJsonValue &value)
{
    return value.isString() ? value.toString().toInt() : value.toInt();
}

QJsonObject functionTool(const QString &name, const QString &description,
                         const QJsonObject &parameters)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("description"), description},
            {QStringLiteral("parameters"), parameters},
        }},
    };
}

QJsonObject textParam(const QString &description)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                       {QStringLiteral("description"), description}};
}

QJsonObject numberParam(const QString &description)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                       {QStringLiteral("description"), description}};
}

// The talker's tools. It can start work, change work and stop work - and, when
// the memory is up, remember, recall and forget. Nothing else: everything that
// touches the machine happens inside a task.
QJsonArray talkTools(bool memory)
{
    QJsonArray list;
    if (memory) {
        list.append(functionTool(
            QStringLiteral("remember"),
            QStringLiteral("Keep something the user asked you to remember, for good."),
            QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"fact", textParam(QStringLiteral(
                         "One plain sentence about the user, such as The user's "
                         "favourite code editor is Qt Creator."))}}},
                {"required", QJsonArray{"fact"}},
            }));
        list.append(functionTool(
            QStringLiteral("recall"),
            QStringLiteral("Look something up in your long-term memory of the user."),
            QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"query", textParam(QStringLiteral("What to look for, in a few words."))}}},
                {"required", QJsonArray{"query"}},
            }));
        list.append(functionTool(
            QStringLiteral("forget_memory"),
            QStringLiteral("Forget one thing in your long-term memory, by its memory id."),
            QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"id", textParam(QStringLiteral("The memory id from the notes."))}}},
                {"required", QJsonArray{"id"}},
            }));
    }
    list.append(functionTool(
        QStringLiteral("start_task"),
        QStringLiteral("Start a background task that does something on the "
                       "user's computer or in their apps, while you carry on "
                       "talking with them."),
        QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"title", textParam(QStringLiteral(
                     "A few words naming the job, such as Open GitHub or Write "
                     "the poem."))},
                {"instruction", textParam(QStringLiteral(
                     "Everything the task needs, as a complete brief for "
                     "someone who has not heard this conversation: what to do, "
                     "and every name, file, site, window or account involved."))},
            }},
            {"required", QJsonArray{"title", "instruction"}},
        }));
    list.append(functionTool(
        QStringLiteral("steer_task"),
        QStringLiteral("Add to or change what a running task is doing, or give "
                       "it the user's answer to the question it asked."),
        QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"task", numberParam(QStringLiteral(
                     "The task's number, from the task list."))},
                {"message", textParam(QStringLiteral(
                     "What to add or change, or the user's answer, in full."))},
            }},
            {"required", QJsonArray{"task", "message"}},
        }));
    list.append(functionTool(
        QStringLiteral("stop_task"),
        QStringLiteral("Stop a running task, throwing its work away. ONLY when "
                       "the user plainly asks for it to be stopped or cancelled - "
                       "never on an exclamation or an unclear remark."),
        QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"task", numberParam(QStringLiteral(
                     "The task's number. Leave it out to stop every task."))},
            }},
        }));
    return list;
}

// One function with an action, rather than a function per verb, because a
// model picks an action far more reliably than it picks between a dozen near
// identical function names.
QJsonObject computerUseTool()
{
    const QJsonObject properties{
        {"action", QJsonObject{
            {"type", "string"},
            {"enum", QJsonArray{"launch", "open_url", "play_music", "list_windows", "list_controls",
                                "screenshot", "click", "click_at", "type_text",
                                "press_keys", "scroll", "drag", "move_mouse",
                                "read_text", "window", "wait_for", "clipboard"}},
            {"description",
             "launch OPENS A PROGRAM BY ITS NAME and is the only way you should "
             "open one - it finds Store apps, which have no command line at all, "
             "and it tells you the new window's controls once it is up. "
             "open_url OPENS A WEB PAGE in the browser the user already has, on "
             "the profile they are signed in on, and is the only way you should "
             "open a website. play_music PLAYS MUSIC: text is what to play and it "
             "searches Spotify on this PC, presses play on the best match and "
             "checks that it really is playing. list_windows says what is open. "
             "list_controls re-reads what a window offers - filter narrows it to "
             "the controls named with a word. screenshot SHOWS YOU a window - or "
             "the whole screen with screen true - with every control outlined and "
             "its key drawn on it; given x, y, to_x and to_y it ZOOMS IN on that "
             "part of your last picture, for text too small to read. click "
             "presses a named control (button right "
             "or double true for those). click_at clicks a point you picked out "
             "of the last screenshot. type_text types into a named control, or "
             "with no control wherever the window's cursor is. press_keys "
             "presses any keys or chords in order: 'ctrl+s', 'alt+tab', 'win+r', "
             "'ctrl+shift+esc', 'ctrl+a delete', 'f5', 'enter'. scroll turns the "
             "wheel over a point, a control or the window. drag drags from x,y "
             "to to_x,to_y. move_mouse hovers. read_text gives you the words in "
             "a window or control - a web page, a document, a dialog - without "
             "a picture. window focuses, maximizes, minimizes, restores or "
             "closes a window. wait_for waits until a window or some text "
             "appears, or for some seconds. clipboard reads it, or sets it to "
             "text. Every click, keystroke, drag and scroll uses the agent's OWN "
             "pointer and keyboard in the background, so the user keeps their real "
             "mouse and keyboard for other things; add real true to force the "
             "physical devices when something will not take a background action."}}},
        {"name", textParam(QStringLiteral(
             "For launch. What the program is called, as a person would say "
             "it: Microsoft Store, Firefox, Notepad, Spotify, Calculator. Work "
             "out the real name from what was asked - 'my browser' is whichever "
             "browser is installed - and give that, not a command line."))},
        {"url", textParam(QStringLiteral(
             "For open_url. The web address itself, such as github.com or "
             "https://mail.google.com. Give the site, not a search for it."))},
        {"window", textParam(QStringLiteral(
             "Part of the title of the window meant, for screenshot, "
             "list_controls, type_text, press_keys, read_text, window and "
             "wait_for. Leave it out for the window you are working in."))},
        {"screen", QJsonObject{{"type", "boolean"},
                               {"description", "For screenshot: true for the whole "
                                               "monitor your window is on, as the "
                                               "user sees it - dialogs, menus and "
                                               "tooltips too."}}},
        {"x", numberParam(QStringLiteral(
             "For click_at, drag, scroll and move_mouse. How far ACROSS the last "
             "screenshot the point is, in that picture's own pixels."))},
        {"y", numberParam(QStringLiteral(
             "How far DOWN the last screenshot the point is, in that picture's "
             "own pixels."))},
        {"to_x", numberParam(QStringLiteral("For drag: where to drop, across. For a "
                                           "screenshot zoom: the far corner."))},
        {"to_y", numberParam(QStringLiteral("For drag: where to drop, down. For a "
                                           "screenshot zoom: the far corner."))},
        {"button", QJsonObject{{"type", "string"},
                               {"enum", QJsonArray{"left", "right", "middle"}},
                               {"description", "For click and click_at. Left if "
                                               "left out."}}},
        {"double", QJsonObject{{"type", "boolean"},
                               {"description", "For click and click_at: a double "
                                               "click."}}},
        {"hold", QJsonObject{{"type", "string"},
                             {"description", "For click and click_at: a modifier to "
                                             "hold down during the click - 'ctrl' or "
                                             "'shift', for adding to a selection."}}},
        {"real", QJsonObject{{"type", "boolean"},
                             {"description", "Force the PHYSICAL mouse and keyboard "
                                             "for this one action, borrowing the "
                                             "user's own pointer. A last resort, only "
                                             "when a click or some text plainly did "
                                             "not land the normal way."}}},
        {"control", textParam(QStringLiteral(
             "The key of a control from the list or the screenshot, exactly as "
             "shown, such as c7 - for click, type_text, scroll, move_mouse and "
             "read_text."))},
        {"keys", textParam(QStringLiteral(
             "For press_keys. Keys joined with + are pressed together, and "
             "groups separated by spaces go one after another: 'ctrl+l', "
             "'ctrl+a delete', 'alt+f4', 'win+r', 'shift+tab', 'pagedown'. Any "
             "single character, f1 to f24, enter, escape, tab, space, "
             "backspace, delete, insert, home, end, pageup, pagedown, arrows, "
             "ctrl, alt, shift, win, volumeup, volumedown, mute, playpause, "
             "nexttrack, prevtrack."))},
        {"filter", textParam(QStringLiteral(
             "For list_controls: only the controls whose name holds these words, "
             "such as play or save - how to find one control among hundreds in "
             "a big app or a web page."))},
        {"text", textParam(QStringLiteral(
             "For play_music: what to play as the user said it - a song, an "
             "artist, an album or a playlist, with the artist if they named one - "
             "never the words play or Spotify. "
             "For type_text: exactly the words to type - a new line is Enter. "
             "For wait_for: the words to wait for. For clipboard: what to put "
             "on it; leave it out to read the clipboard instead."))},
        {"direction", QJsonObject{{"type", "string"},
                                  {"enum", QJsonArray{"up", "down", "left", "right"}},
                                  {"description", "For scroll."}}},
        {"amount", numberParam(QStringLiteral(
             "For scroll: how many notches of the wheel, 5 if left out."))},
        {"how", QJsonObject{{"type", "string"},
                            {"enum", QJsonArray{"focus", "maximize", "minimize",
                                                "restore", "close"}},
                            {"description", "For window."}}},
        {"seconds", numberParam(QStringLiteral(
             "For wait_for: the longest to wait, up to 60; 10 if left out."))},
    };
    return functionTool(QStringLiteral("computer_use"),
                        QStringLiteral("Operate the Windows machine Burnos is "
                                       "running on."),
                        QJsonObject{{"type", "object"},
                                    {"properties", properties},
                                    {"required", QJsonArray{"action"}}});
}

// Everything beyond this machine. THREE VERBS, and the order matters: Composio
// has tens of thousands of tools and their slugs cannot be guessed, so the
// model searches in plain words, gets back real slugs with the arguments each
// needs, and only then runs one.
QJsonObject appActionTool()
{
    const QJsonObject properties{
        {"action", QJsonObject{
            {"type", "string"},
            {"enum", QJsonArray{"find_tools", "run_tool", "connect_app"}},
            {"description",
             "find_tools searches the connected apps for something that can do "
             "what the user asked and gives you back real tool slugs with the "
             "arguments each one needs. run_tool carries one of them out. "
             "ALWAYS find_tools first: tool slugs cannot be guessed and an "
             "invented one simply fails. connect_app opens the sign in page for "
             "an app find_tools reported as NOT CONNECTED."}}},
        {"query", textParam(QStringLiteral(
             "For find_tools. What you want to do, as one full sentence naming "
             "the app: send an email with Gmail, post a message to a channel, "
             "create a calendar event, open an issue."))},
        {"app", textParam(QStringLiteral(
             "For connect_app. The app's slug exactly as find_tools named it, "
             "such as gmail or googlecalendar."))},
        {"tool", textParam(QStringLiteral(
             "For run_tool. The slug exactly as find_tools gave it, such as "
             "GMAIL_SEND_EMAIL. Never one you have not been given."))},
        {"arguments", QJsonObject{
            {"type", "object"},
            {"additionalProperties", true},
            {"description",
             "For run_tool. The tool's own arguments, using the names "
             "find_tools listed. Fill in every one it said was needed; if you "
             "are missing something only the user knows, such as who the "
             "message is for, ask them with ask_user instead of guessing."}}},
    };
    return functionTool(QStringLiteral("app_action"),
                        QStringLiteral("Do something in one of the user's online "
                                       "apps - send an email, post to a chat "
                                       "channel, add a calendar event, write a "
                                       "page, open an issue, look something up "
                                       "in one of their accounts or on the web."),
                        QJsonObject{{"type", "object"},
                                    {"properties", properties},
                                    {"required", QJsonArray{"action"}}});
}

// CODING. Files are read and changed directly, never by typing into an editor's
// window: it is exact, it is fast, and nothing on screen can get in the way.
QJsonObject filesTool()
{
    const QJsonObject properties{
        {"action", QJsonObject{
            {"type", "string"},
            {"enum", QJsonArray{"read", "write", "edit", "list", "find", "search"}},
            {"description",
             "read a text file with numbered lines - offset and limit for a long "
             "one. write a whole file, creating its folders as needed. edit a "
             "file by replacing an exact piece of it: old is copied exactly from "
             "what read showed, without the line numbers, and must be unique "
             "unless replace_all. list a folder. find files by name under a "
             "folder. search file contents under a folder with a regular "
             "expression."}}},
        {"path", textParam(QStringLiteral(
             "The file or folder, as a full Windows path. A relative path is "
             "under the user's home folder."))},
        {"offset", numberParam(QStringLiteral(
             "For read: the first line wanted, counting from 1."))},
        {"limit", numberParam(QStringLiteral(
             "For read: how many lines, 400 if left out, at most 2000."))},
        {"content", textParam(QStringLiteral("For write: the whole new content."))},
        {"old", textParam(QStringLiteral("For edit: the exact text to replace."))},
        {"new", textParam(QStringLiteral("For edit: what replaces it."))},
        {"replace_all", QJsonObject{{"type", "boolean"},
                                    {"description", "For edit: every place old appears."}}},
        {"pattern", textParam(QStringLiteral(
             "For find: a file name pattern such as *.cpp or CMakeLists.txt. "
             "For search: a regular expression."))},
        {"glob", textParam(QStringLiteral(
             "For search: only files whose names match this, such as *.h."))},
        {"ignore_case", QJsonObject{{"type", "boolean"},
                                    {"description", "For search."}}},
    };
    return functionTool(QStringLiteral("files"),
                        QStringLiteral("Read, write and change files and look "
                                       "through folders on this machine - code, "
                                       "notes, configuration, anything in text."),
                        QJsonObject{{"type", "object"},
                                    {"properties", properties},
                                    {"required", QJsonArray{"action", "path"}}});
}

QJsonObject shellTool()
{
    const QJsonObject properties{
        {"command", textParam(QStringLiteral(
             "The command line, as typed at a Windows prompt: a build, tests, "
             "git, python, node, powershell -c \"...\", dir. Not for opening "
             "programs - computer_use launch does that."))},
        {"cwd", textParam(QStringLiteral(
             "The folder to run it in; the user's home folder if left out."))},
        {"timeout", numberParam(QStringLiteral(
             "Seconds before it is stopped: 120 if left out, at most 900. Give a "
             "long build the time it needs."))},
        {"background", QJsonObject{{"type", "boolean"},
                                   {"description", "Start it and let it keep "
                                                   "running - a server, a watcher, "
                                                   "a program - instead of waiting "
                                                   "for it to finish."}}},
    };
    return functionTool(QStringLiteral("shell"),
                        QStringLiteral("Run a command line on this machine and get "
                                       "back its exit code and what it printed. "
                                       "Nothing else waits while it runs."),
                        QJsonObject{{"type", "object"},
                                    {"properties", properties},
                                    {"required", QJsonArray{"command"}}});
}

QJsonObject askUserTool()
{
    return functionTool(
        QStringLiteral("ask_user"),
        QStringLiteral("Ask the user one short question out loud and wait for "
                       "the answer. Only for something only they know."),
        QJsonObject{{"type", "object"},
                    {"properties", QJsonObject{
                        {"question", textParam(QStringLiteral(
                             "One short question, worded to be spoken."))}}},
                    {"required", QJsonArray{"question"}}});
}

// The user's MCP servers, found and then called - two steps, because a server
// such as Mnemosyne has dozens of tools and their arguments have to be seen
// before they can be used.
QJsonObject mcpTool()
{
    const QJsonObject properties{
        {"action", QJsonObject{
            {"type", "string"},
            {"enum", QJsonArray{"list_tools", "call_tool"}},
            {"description",
             "list_tools shows what the servers offer - narrowed by server and "
             "by words in query - with the arguments each tool takes. call_tool "
             "runs one of them."}}},
        {"server", textParam(QStringLiteral("The server's name, such as memory."))},
        {"query", textParam(QStringLiteral("For list_tools: words to look for."))},
        {"tool", textParam(QStringLiteral("For call_tool: the tool's name exactly as listed."))},
        {"arguments", QJsonObject{
            {"type", "object"},
            {"additionalProperties", true},
            {"description", "For call_tool: the tool's arguments by the names listed."}}},
    };
    return functionTool(QStringLiteral("mcp"),
                        QStringLiteral("Use the user's MCP servers - their long-term "
                                       "memory, and anything else they have "
                                       "connected to Burnos."),
                        QJsonObject{{"type", "object"},
                                    {"properties", properties},
                                    {"required", QJsonArray{"action"}}});
}

QJsonArray taskTools(bool online, bool mcp)
{
    QJsonArray list{computerUseTool(), filesTool(), shellTool(), askUserTool()};
    if (online)
        list.append(appActionTool());
    if (mcp)
        list.append(mcpTool());
    return list;
}

// NAMING THE APPS MATTERS. Told merely that it has apps, the model invents a
// tool slug and the call fails; told that Gmail and Slack are connected, it
// searches for the right one first.
QString appsNote()
{
    const QStringList apps = Settings::composioApps();
    return QObject::tr(
               "YOU CAN ACT IN THE USER'S ONLINE APPS through the app_action tool: "
               "email, calendars, chat, documents, code hosting, web search and "
               "about fifteen hundred more. Connected right now: %1. WHICH TOOL "
               "TO REACH FOR, AND IT MATTERS: anything that lives in one of "
               "their ACCOUNTS - an email, a calendar entry, a chat message, a "
               "document, a repository, an issue, a look up on the web - goes "
               "through app_action, even when a browser happens to be open, "
               "because it is faster, it cannot misclick, and it does not depend "
               "on what is on screen. Anything that lives on THIS MACHINE - "
               "opening a program, a file, a window or a setting, reading what "
               "is on screen, clicking something - goes through computer_use, "
               "and simply putting a web page in front of the user to look at is "
               "computer_use with open_url. Where a job needs both, do the "
               "account part with app_action first. Apps that need no sign in, "
               "such as web search, news and Hacker News, work straight away. "
               "Always call find_tools first, describing the task and the app in "
               "a full sentence, then run_tool with a slug that search actually "
               "gave you back; never invent a tool name. When find_tools says an "
               "app is NOT CONNECTED, call connect_app with that app: it opens "
               "the sign in page in the user's browser, and your report tells "
               "them to finish signing in there and ask again. Never send them to "
               "Settings for something connect_app can do.")
        .arg(apps.isEmpty() ? QObject::tr("none yet") : apps.join(QStringLiteral(", ")));
}

// What a task is doing, in a few plain words, for the talker to relay when
// the user asks how it is going.
QString describeStep(const QString &function, const QVariantMap &args)
{
    const QString action = args.value(QStringLiteral("action")).toString();
    if (function == QLatin1String("ask_user"))
        return QStringLiteral("asking the user a question");
    if (function == QLatin1String("mcp")) {
        return QStringLiteral("using %1")
            .arg(args.value(QStringLiteral("server")).toString().isEmpty()
                     ? QStringLiteral("the user's MCP servers")
                     : args.value(QStringLiteral("server")).toString());
    }
    if (function == QLatin1String("files")) {
        const QString path = args.value(QStringLiteral("path")).toString();
        if (action == QLatin1String("read"))
            return QStringLiteral("reading %1").arg(path);
        if (action == QLatin1String("write"))
            return QStringLiteral("writing %1").arg(path);
        if (action == QLatin1String("edit"))
            return QStringLiteral("changing %1").arg(path);
        return QStringLiteral("looking through %1").arg(path);
    }
    if (function == QLatin1String("shell")) {
        return QStringLiteral("running %1")
            .arg(args.value(QStringLiteral("command")).toString().left(80));
    }
    if (action == QLatin1String("press_keys"))
        return QStringLiteral("pressing %1").arg(args.value(QStringLiteral("keys")).toString());
    if (action == QLatin1String("scroll"))
        return QStringLiteral("scrolling");
    if (action == QLatin1String("drag"))
        return QStringLiteral("dragging");
    if (action == QLatin1String("read_text"))
        return QStringLiteral("reading what is on screen");
    if (action == QLatin1String("window"))
        return QStringLiteral("arranging windows");
    if (action == QLatin1String("wait_for"))
        return QStringLiteral("waiting for something to appear");
    if (action == QLatin1String("clipboard"))
        return QStringLiteral("using the clipboard");
    if (function == QLatin1String("app_action")) {
        if (action == QLatin1String("run_tool")) {
            return QStringLiteral("running %1 in the user's apps")
                .arg(args.value(QStringLiteral("tool")).toString());
        }
        if (action == QLatin1String("connect_app")) {
            return QStringLiteral("opening the sign in for %1")
                .arg(args.value(QStringLiteral("app")).toString());
        }
        return QStringLiteral("looking for the right tool in the user's apps");
    }
    if (action == QLatin1String("launch"))
        return QStringLiteral("opening %1").arg(args.value(QStringLiteral("name")).toString());
    if (action == QLatin1String("open_url"))
        return QStringLiteral("opening %1").arg(args.value(QStringLiteral("url")).toString());
    if (action == QLatin1String("play_music"))
        return QStringLiteral("playing %1").arg(args.value(QStringLiteral("text")).toString());
    if (action == QLatin1String("screenshot"))
        return QStringLiteral("looking at the screen");
    if (action == QLatin1String("list_windows") || action == QLatin1String("list_controls"))
        return QStringLiteral("checking what is on screen");
    if (action == QLatin1String("click") || action == QLatin1String("click_at"))
        return QStringLiteral("clicking");
    if (action == QLatin1String("press_key"))
        return QStringLiteral("pressing %1").arg(args.value(QStringLiteral("key")).toString());
    if (action == QLatin1String("type_text"))
        return QStringLiteral("typing");
    return QStringLiteral("working");
}

} // namespace

// ===========================================================================
// TASK
// ===========================================================================

Task::Task(int id, const QString &title, const QString &instruction, Agent *agent)
    : QObject(agent)
    , m_id(id)
    , m_title(title)
    , m_instruction(instruction)
    , m_agent(agent)
    , m_cancelled(std::make_shared<std::atomic_bool>(false))
{
    m_clock.start();
}

qint64 Task::runningMs() const
{
    return m_clock.isValid() ? m_clock.elapsed() : 0;
}

qint64 Task::endedAgoMs() const
{
    return m_ended.isValid() ? m_ended.elapsed() : 0;
}

void Task::start()
{
    liveLog(QStringLiteral("task %1 STARTED '%2': %3")
                .arg(m_id)
                .arg(m_title, m_instruction.simplified().left(400)));
    m_step = QStringLiteral("looking at what is on screen");

    // The window in front and what can be clicked in it, looked at ON THE
    // HANDS before the model is asked anything, so its first move can name a
    // control instead of spending a round trip on finding one.
    Computer::onHands(
        this,
        []() {
            DeskResult r;
            r.text = Computer::describeControls(Computer::foreground(), r.state);
            return r;
        },
        [this](const DeskResult &r) {
            if (!isLive())
                return;
            m_desk = r.state;
            m_messages.append(msg(QStringLiteral("system"),
                                  QString::fromLatin1(kTaskPrompt)));
            if (Settings::hasComposioKey())
                m_messages.append(msg(QStringLiteral("system"), appsNote()));
            const QStringList servers = m_agent->mcp()->readyNames();
            if (!servers.isEmpty()) {
                m_messages.append(msg(
                    QStringLiteral("system"),
                    tr("THE USER'S MCP SERVERS, ALREADY CONNECTED, through the mcp "
                       "tool: %1. Never look for their configuration files or "
                       "processes - call mcp with list_tools to see what they "
                       "offer, then call_tool. The one called memory is the user's "
                       "long-term memory (Mnemosyne): pass tenant_id '%2' and "
                       "user_id '%3' to every tool of it.")
                        .arg(servers.join(QStringLiteral(", ")),
                             QString::fromLatin1(kMemoryTenant),
                             QString::fromLatin1(kMemoryUser))));
            }
            m_messages.append(msg(QStringLiteral("system"),
                                  tr("The time now is %1. The user's home folder "
                                     "is %2. The window in front right now. %3")
                                      .arg(nowWords(),
                                           QDir::toNativeSeparators(QDir::homePath()),
                                           r.text)));
            m_messages.append(msg(QStringLiteral("user"), m_instruction));
            ask();
        });
}

QString Task::model() const
{
    // Read from Settings on every request, like everything else, so a working
    // model chosen while a task runs is the one its very next step asks.
    return m_onTalkModel ? Settings::openRouterModel() : Settings::taskModel();
}

void Task::ask()
{
    if (!isLive())
        return;

    // THE BUDGET. Past it the model is told to stop and report - once, with no
    // tools on offer - so the user still hears what was done rather than
    // nothing at all.
    if (!m_wrappingUp && (m_toolCalls >= kTaskMaxCalls || runningMs() >= kTaskMaxMs)) {
        m_wrappingUp = true;
        liveLog(QStringLiteral("task %1: out of budget after %2 step(s) and %3 s, "
                               "asking for a report")
                    .arg(m_id)
                    .arg(m_toolCalls)
                    .arg(runningMs() / 1000));
        m_messages.append(msg(QStringLiteral("user"),
                              tr("Stop here: this task has used all the time and "
                                 "steps it is allowed. Do not call any tool. Reply "
                                 "with one or two sentences for the user saying "
                                 "what you managed and what is still left to do.")));
    }

    // WHAT THE USER ADDED while the last step ran. It goes in here, after every
    // tool result the model is owed, because a user message in among those
    // would be refused.
    for (const QString &line : std::as_const(m_steering)) {
        m_messages.append(msg(QStringLiteral("user"),
                              tr("The user adds, while you work: %1").arg(line)));
    }
    m_steering.clear();

    pruneContext();

    OpenRouterClient::Request request;
    request.messages = m_messages;
    if (!m_wrappingUp)
        request.tools = taskTools(Settings::hasComposioKey(),
                                  !m_agent->mcp()->readyNames().isEmpty());
    request.who = QStringLiteral("task %1").arg(m_id);
    // THE WORKING MODEL, never the talking one's fastest-provider rule: a task
    // wants the best answer from whichever provider OpenRouter judges best,
    // and nobody is waiting on its first word.
    request.model = model();
    request.timeoutMs = kTaskRequestMs;
    request.maxTokens = kTaskMaxTokens;

    const QPointer<Task> self(this);
    m_request = m_agent->ai()->send(request, [self](const OpenRouterClient::Reply &reply) {
        if (self)
            self->onAnswer(reply);
    });
}

void Task::onAnswer(const OpenRouterClient::Reply &reply)
{
    m_request = 0;
    if (!isLive())
        return;

    if (!reply.ok) {
        // ONE MORE TRY. A timeout or a provider hiccup should not throw away a
        // task that was going well; a second failure in a row is real.
        if (m_retries < 1) {
            ++m_retries;
            liveLog(QStringLiteral("task %1: the request failed (%2), trying once more")
                        .arg(m_id)
                        .arg(reply.error.left(160)));
            const QPointer<Task> self(this);
            QTimer::singleShot(1500, this, [self]() {
                if (self && self->isLive() && !self->m_request)
                    self->ask();
            });
            return;
        }

        // THE WORKING MODEL IS DOWN, NOT THE TASK. When a separate working
        // model fails twice running - overloaded, withdrawn, out of credit at
        // its provider - the task carries on with the talking model rather
        // than dying with the work half done. Once moved it stays moved:
        // flipping back and forth would change minds in the middle of a job.
        const QString failed = model();
        const QString talking = Settings::openRouterModel();
        if (!m_onTalkModel && failed != talking) {
            m_onTalkModel = true;
            m_retries = 0;
            liveLog(QStringLiteral("task %1: %2 failed twice (%3), carrying on with "
                                   "the talking model %4")
                        .arg(m_id)
                        .arg(failed, reply.error.left(160), talking));
            const QPointer<Task> self(this);
            QTimer::singleShot(0, this, [self]() {
                if (self && self->isLive() && !self->m_request)
                    self->ask();
            });
            return;
        }
        end(State::Failed, tr("I could not finish %1, because the model stopped "
                              "answering.").arg(m_title));
        return;
    }
    m_retries = 0;

    const QJsonObject message = reply.message;
    const QJsonArray calls = message.value(QStringLiteral("tool_calls")).toArray();
    if (!calls.isEmpty() && !m_wrappingUp) {
        // Back verbatim, because every tool result has to answer a call in it.
        m_messages.append(message);
        m_calls = calls;
        runNextCall();
        return;
    }

    const QString text = message.value(QStringLiteral("content")).toString().trimmed();
    if (text.isEmpty()) {
        if (!m_nudged) {
            m_nudged = true;
            liveLog(QStringLiteral("task %1: an empty answer, asking for the report")
                        .arg(m_id));
            m_messages.append(msg(QStringLiteral("user"),
                                  tr("You gave no answer. If the task is finished, "
                                     "reply now with the result for the user in "
                                     "one or two short sentences; if it is not, "
                                     "carry on with it.")));
            ask();
            return;
        }
        end(State::Failed, tr("I worked on %1 but could not put the result into "
                              "words.").arg(m_title));
        return;
    }
    end(State::Done, text);
}

void Task::runNextCall()
{
    if (!isLive())
        return;

    if (m_calls.isEmpty()) {
        // Every call in that answer has its result. The pictures go in now,
        // after all of them, and the model is asked what next.
        flushShots();
        ask();
        return;
    }

    const QJsonObject call = m_calls.first().toObject();
    m_calls.removeFirst();
    m_callId = call.value(QStringLiteral("id")).toString();
    const QJsonObject fn = call.value(QStringLiteral("function")).toObject();
    const QString name = fn.value(QStringLiteral("name")).toString();
    const QString rawArgs = fn.value(QStringLiteral("arguments")).toString();

    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(rawArgs.toUtf8(), &parse);
    const QVariantMap args = doc.object().toVariantMap();

    ++m_toolCalls;
    m_step = describeStep(name, args);
    liveLog(QStringLiteral("task %1 step %2: %3 %4")
                .arg(m_id)
                .arg(m_toolCalls)
                .arg(name, rawArgs.simplified().left(200)));

    if (!rawArgs.trimmed().isEmpty() && parse.error != QJsonParseError::NoError) {
        answerCall(tr("Failed: those arguments were not valid JSON (%1). Send the "
                      "call again with valid JSON.").arg(parse.errorString()));
        return;
    }

    if (name == QLatin1String("computer_use")) {
        // ON THE HANDS: one thread, one action at a time, whichever task asks.
        const DeskState desk = m_desk;
        const std::shared_ptr<std::atomic_bool> cancelled = m_cancelled;
        Computer::onHands(
            this,
            [args, desk, cancelled]() { return Computer::perform(args, desk, *cancelled); },
            [this](const DeskResult &r) {
                if (!isLive())
                    return;
                m_desk = r.state;
                liveLog(QStringLiteral("task %1: %2")
                            .arg(m_id)
                            .arg(r.text.simplified().left(200)));
                answerCall(r.text, r.png);
            });
        return;
    }

    if (name == QLatin1String("files")) {
        // ON A POOL THREAD: a search through a big project takes a moment, and
        // nothing else - the talker, the other tasks - should wait on it.
        const QPointer<Task> self(this);
        const int id = m_id;
        QThreadPool::globalInstance()->start([self, args, id]() {
            const QString result = Computer::files(args);
            QMetaObject::invokeMethod(
                qApp,
                [self, result, id]() {
                    liveLog(QStringLiteral("task %1: %2").arg(id).arg(result.simplified().left(200)));
                    if (self && self->isLive())
                        self->answerCall(result);
                },
                Qt::QueuedConnection);
        });
        return;
    }

    if (name == QLatin1String("shell")) {
        Computer::runShell(this, args, m_cancelled, [this](const QString &result) {
            if (!isLive())
                return;
            liveLog(QStringLiteral("task %1: %2").arg(m_id).arg(result.simplified().left(200)));
            answerCall(result);
        });
        return;
    }

    if (name == QLatin1String("app_action")) {
        runAppAction(args);
        return;
    }

    if (name == QLatin1String("mcp")) {
        runMcp(args);
        return;
    }

    if (name == QLatin1String("ask_user")) {
        const QString question = args.value(QStringLiteral("question")).toString().trimmed();
        if (question.isEmpty()) {
            answerCall(tr("Failed: ask_user needs a question."));
            return;
        }
        // The call stays open until the answer arrives through steer().
        m_state = State::WaitingForUser;
        m_question = question;
        m_step = QStringLiteral("waiting for the user's answer");
        liveLog(QStringLiteral("task %1 ASKS: %2").arg(m_id).arg(question));
        emit asked(m_id, question);
        return;
    }

    answerCall(tr("Failed: there is no tool called %1.").arg(name));
}

void Task::runAppAction(const QVariantMap &args)
{
    const QString action = args.value(QStringLiteral("action")).toString();

    if (!ComposioClient::isConfigured()) {
        answerCall(tr("Online apps are not set up: there is no Composio key. Say "
                      "so in your report - the user can add one in Settings, "
                      "under Manage integrations."));
        return;
    }

    // The answer comes back over the network, to THIS task only, and is handed
    // to the model as the result of this call.
    const QPointer<Task> self(this);
    const int id = m_id;
    const auto back = [self, id](bool ok, const QString &summary) {
        liveLog(QStringLiteral("task %1: app action %2: %3")
                    .arg(id)
                    .arg(ok ? QStringLiteral("answered") : QStringLiteral("failed"),
                         summary.simplified().left(200)));
        if (!self || !self->isLive())
            return;
        self->answerCall(summary.isEmpty()
                             ? tr("Nothing any of the user's apps can reach does that.")
                             : summary);
    };

    ComposioClient *apps = m_agent->composio();

    if (action == QLatin1String("find_tools")) {
        const QString query = args.value(QStringLiteral("query")).toString().trimmed();
        if (query.isEmpty()) {
            answerCall(tr("find_tools needs a query saying in a sentence what you "
                          "are trying to do and in which app."));
            return;
        }
        apps->searchTools(query, back);
        return;
    }

    if (action == QLatin1String("run_tool")) {
        const QString tool = args.value(QStringLiteral("tool")).toString().trimmed();
        if (tool.isEmpty()) {
            answerCall(tr("run_tool needs the slug of a tool find_tools gave you."));
            return;
        }
        apps->runTool(tool, argumentsOf(args.value(QStringLiteral("arguments"))), back);
        return;
    }

    if (action == QLatin1String("connect_app")) {
        const QString app = args.value(QStringLiteral("app")).toString().trimmed().toLower();
        if (app.isEmpty()) {
            answerCall(tr("connect_app needs app: the slug of the app to connect, "
                          "as find_tools named it, such as gmail."));
            return;
        }
        apps->connectApp(app, back);
        return;
    }

    answerCall(tr("'%1' is not something app_action does. Use find_tools to look "
                  "for a tool, run_tool to carry it out, or connect_app to sign in "
                  "to an app.").arg(action));
}

void Task::runMcp(const QVariantMap &args)
{
    McpClient *mcp = m_agent->mcp();
    const QString action = args.value(QStringLiteral("action")).toString();
    const QString serverName = args.value(QStringLiteral("server")).toString().trimmed();

    if (action == QLatin1String("list_tools")) {
        answerCall(mcp->describeTools(serverName, args.value(QStringLiteral("query")).toString()));
        return;
    }

    if (action == QLatin1String("call_tool")) {
        McpServer *server = mcp->server(serverName);
        if (!server || !server->isReady()) {
            answerCall(tr("Failed: there is no ready MCP server called '%1'. Ready: %2.")
                           .arg(serverName,
                                mcp->readyNames().isEmpty()
                                    ? QStringLiteral("none")
                                    : mcp->readyNames().join(QStringLiteral(", "))));
            return;
        }
        const QString tool = args.value(QStringLiteral("tool")).toString().trimmed();
        if (!server->hasTool(tool)) {
            answerCall(tr("Failed: %1 has no tool called '%2'. Use list_tools first.")
                           .arg(serverName, tool));
            return;
        }
        const QJsonObject arguments = argumentsOf(args.value(QStringLiteral("arguments")));

        const QPointer<Task> self(this);
        const int id = m_id;
        server->call(tool, arguments,
                     [self, id, tool](bool ok, const QJsonObject &result, const QString &error) {
            const QString text = ok ? McpServer::textOf(result)
                                    : tr("Failed: %1").arg(error.isEmpty()
                                                              ? McpServer::textOf(result)
                                                              : error);
            liveLog(QStringLiteral("task %1: mcp %2 %3")
                        .arg(id)
                        .arg(tool, ok ? QStringLiteral("answered") : QStringLiteral("failed")));
            if (self && self->isLive())
                self->answerCall(text.isEmpty() ? tr("It ran and said nothing.") : text);
        }, 60000);
        return;
    }

    answerCall(tr("Failed: mcp needs action list_tools or call_tool."));
}

void Task::answerCall(const QString &result, const QByteArray &png)
{
    if (!isLive() || m_callId.isEmpty())
        return;

    QString text = result;
    if (text.size() > kMaxToolText)
        text = text.left(kMaxToolText) + QStringLiteral(" ... (cut short, there was more)");

    m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")},
                                  {QStringLiteral("tool_call_id"), m_callId},
                                  {QStringLiteral("content"), text}});
    m_callId.clear();
    if (!png.isEmpty())
        m_shots.append(png);

    runNextCall();
}

void Task::flushShots()
{
    // A tool message cannot carry an image, so each picture follows the tool
    // results as a user message holding a data URL - the shape the API does
    // accept. A model that cannot see is told so in words instead.
    for (const QByteArray &png : std::as_const(m_shots)) {
        if (!m_agent->ai()->canSee(model())) {
            m_messages.append(msg(QStringLiteral("user"), OpenRouterClient::blindNote()));
            continue;
        }
        QJsonArray parts;
        parts.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                 {QStringLiteral("text"),
                                  QStringLiteral("Here is that screenshot.")}});
        parts.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("image_url")},
            {QStringLiteral("image_url"),
             QJsonObject{{QStringLiteral("url"),
                          QStringLiteral("data:image/png;base64,")
                              + QString::fromLatin1(png.toBase64())}}}});
        m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                      {QStringLiteral("content"), parts}});
    }
    m_shots.clear();
}

void Task::pruneContext()
{
    // Walked from the newest backwards: the latest pictures and the latest
    // tool output are what the model needs, and the rest is cut down to a line
    // saying it was there.
    int shots = 0;
    int toolChars = 0;
    for (int i = int(m_messages.size()) - 1; i >= 0; --i) {
        QJsonObject message = m_messages.at(i).toObject();
        const QString role = message.value(QStringLiteral("role")).toString();
        if (role == QLatin1String("user") && message.value(QStringLiteral("content")).isArray()) {
            if (++shots > kKeepShots) {
                m_messages[i] = msg(QStringLiteral("user"),
                                    QStringLiteral("(An earlier screenshot, taken "
                                                   "out to save space.)"));
            }
        } else if (role == QLatin1String("tool")) {
            const QString content = message.value(QStringLiteral("content")).toString();
            toolChars += int(content.size());
            if (toolChars > kMaxToolTotal && content.size() > 300) {
                message.insert(QStringLiteral("content"),
                               content.left(300)
                                   + QStringLiteral(" ... (the rest of this older "
                                                    "result was taken out to save "
                                                    "space)"));
                m_messages[i] = message;
            }
        }
    }
}

void Task::steer(const QString &message)
{
    if (!isLive())
        return;
    liveLog(QStringLiteral("task %1 STEERED: %2").arg(m_id).arg(message.left(300)));

    // The answer to its own question: the call it made is answered with it and
    // the task carries on from exactly where it stopped.
    if (m_state == State::WaitingForUser && !m_callId.isEmpty()) {
        m_state = State::Running;
        m_question.clear();
        answerCall(tr("The user answered: %1").arg(message));
        return;
    }

    m_steering.append(message);

    // The model is working out its next step right now, without this. Ask again
    // with it, rather than let it take a step the user has just changed their
    // mind about.
    if (m_request && m_calls.isEmpty() && m_callId.isEmpty()) {
        m_agent->ai()->cancel(m_request);
        m_request = 0;
        ask();
    }
}

void Task::stop()
{
    if (!isLive())
        return;
    if (m_request) {
        m_agent->ai()->cancel(m_request);
        m_request = 0;
    }
    end(State::Stopped, tr("Stopped."));
}

void Task::end(State state, const QString &outcome)
{
    if (!isLive())
        return;
    m_state = state;
    m_outcome = outcome.trimmed();
    m_step.clear();
    m_question.clear();
    m_calls = QJsonArray();
    m_callId.clear();
    // Anything of this task still waiting or sleeping on the hands lets go now.
    m_cancelled->store(true);
    m_ended.start();

    static const char *const kWords[] = {"RUNNING", "WAITING", "DONE", "FAILED", "STOPPED"};
    liveLog(QStringLiteral("task %1 %2 after %3 s and %4 step(s): %5")
                .arg(m_id)
                .arg(QString::fromLatin1(kWords[int(state)]))
                .arg(runningMs() / 1000)
                .arg(m_toolCalls)
                .arg(m_outcome.simplified().left(300)));
    emit finished(m_id);
}

// ===========================================================================
// AGENT
// ===========================================================================

Agent::Agent(OpenRouterClient *ai, ComposioClient *composio, QObject *parent)
    : QObject(parent)
    , m_ai(ai)
    , m_composio(composio)
    , m_mcp(new McpClient(this))
{
    m_mcp->load();
    connect(m_mcp, &McpClient::memoryChanged, this, [](bool ready) {
        liveLog(QStringLiteral("memory is %1").arg(ready ? QStringLiteral("READY")
                                                          : QStringLiteral("not available")));
    });
}

int Agent::liveTasks() const
{
    int live = 0;
    for (const QPointer<Task> &t : m_tasks) {
        if (t && t->isLive())
            ++live;
    }
    return live;
}

Task *Agent::task(int id) const
{
    for (const QPointer<Task> &t : m_tasks) {
        if (t && t->id() == id)
            return t;
    }
    return nullptr;
}

void Agent::hear(const QString &text)
{
    QString said = text.simplified();
    if (said.isEmpty())
        return;

    if (!m_asking.isEmpty()) {
        // STILL TALKING, and the talker has not answered yet. Nothing has been
        // done about the first half, so that request is dropped and the whole
        // thought asked as one - a pause for breath must not split one question
        // into two unrelated ones.
        liveLog(QStringLiteral("talk: still talking - folded into the question in flight"));
        if (m_request)
            m_ai->cancel(m_request);
        m_request = 0;
        said = m_asking + QLatin1Char(' ') + said;
    } else if (m_unspokenTurn) {
        // Answered, but not out loud yet, and the user has carried on. What
        // was queued to be said is stale either way. If that answer only
        // talked, it is taken back altogether and the whole thought asked
        // again; if it started something, the work stands and this is simply
        // the next turn.
        emit retract(m_unspokenTurn);
        if (m_unspokenOnlyTalk) {
            liveLog(QStringLiteral("talk: still talking - took back the unspoken answer"));
            forget(m_unspokenAsked, m_unspokenAnswer);
            said = m_unspokenAsked + QLatin1Char(' ') + said;
        }
        m_unspokenTurn = 0;
    }

    talk(said);
}

void Agent::talk(const QString &said)
{
    pruneTasks();
    m_asking = said;
    const int turn = ++m_turn;
    emit thinkingChanged(true);

    // MEMORY FIRST, when there is one. Whatever the user once asked to be
    // remembered that bears on what they just said goes to the talker with it,
    // so "open my editor" can mean the editor they told it about last week.
    // Local and quick - and skipped if the memory does not answer in time.
    McpServer *memory = m_mcp->memory();
    if (!memory) {
        sendTalk(turn, said, QString());
        return;
    }
    memory->call(QStringLiteral("search"),
                 QJsonObject{{"tenant_id", kMemoryTenant},
                             {"user_id", kMemoryUser},
                             {"query", said}},
                 [this, turn, said](bool ok, const QJsonObject &result, const QString &error) {
        if (turn != m_turn || m_asking.isEmpty())
            return; // a newer turn took over, or everything was stopped
        QString notes;
        if (ok) {
            notes = memoryNotes(result);
            if (!notes.isEmpty()) {
                liveLog(QStringLiteral("talk %1: memory brought %2 note(s)")
                            .arg(turn)
                            .arg(notes.count(QLatin1Char('\n')) + 1));
            }
        } else {
            liveLog(QStringLiteral("talk %1: memory lookup failed (%2), going on without it")
                        .arg(turn)
                        .arg(error));
        }
        sendTalk(turn, said, notes);
    }, kMemoryLookupMs);
}

void Agent::sendTalk(int turn, const QString &said, const QString &memories)
{
    const bool remembering = m_mcp->memory() != nullptr;

    QJsonArray messages;
    messages.append(msg(QStringLiteral("system"), QString::fromLatin1(kTalkPrompt)));
    if (remembering)
        messages.append(msg(QStringLiteral("system"), QString::fromLatin1(kMemoryPrompt)));
    messages.append(msg(QStringLiteral("system"), statusReport()));
    if (!memories.isEmpty()) {
        messages.append(msg(QStringLiteral("system"),
                            tr("FROM YOUR LONG-TERM MEMORY - things the user once asked "
                               "you to remember that may bear on this. Use them where "
                               "they help, and never read the memory ids out:\n%1")
                                .arg(memories)));
    } else if (remembering) {
        // Said outright, because a model left to fill the gap will invent a
        // memory rather than admit to none - it once named a favourite editor
        // it had never been told.
        messages.append(msg(QStringLiteral("system"),
                            tr("Your long-term memory holds nothing that matches what "
                               "was just said. If the question needs a memory, call "
                               "recall - never answer from a guess.")));
    }
    for (const QJsonValue &line : std::as_const(m_history))
        messages.append(line);
    messages.append(msg(QStringLiteral("user"), said));
    m_talkSent = messages;

    liveLog(QStringLiteral("talk %1: heard '%2' - %3 task(s) running, %4 line(s) of history")
                .arg(turn)
                .arg(said.left(300))
                .arg(liveTasks())
                .arg(m_history.size()));
    askTalker(turn, messages, QStringLiteral("talk %1").arg(turn));
}

void Agent::askTalker(int turn, const QJsonArray &messages, const QString &who)
{
    OpenRouterClient::Request request;
    request.messages = messages;
    // Offered on a recall's second round too: some providers refuse a history
    // holding tool calls without them. A second recall in a turn is not run.
    request.tools = talkTools(m_mcp->memory() != nullptr);
    request.who = who;
    request.timeoutMs = kTalkTimeoutMs;
    request.fastest = true;
    m_request = m_ai->send(request, [this, turn](const OpenRouterClient::Reply &reply) {
        onTalkAnswer(turn, reply);
    });
}

void Agent::onTalkAnswer(int turn, const OpenRouterClient::Reply &reply)
{
    m_request = 0;
    if (turn != m_turn)
        return; // a newer turn took over
    const QString asked = m_asking;

    if (!reply.ok) {
        m_asking.clear();
        liveLog(QStringLiteral("talk %1 FAILED: %2").arg(turn).arg(reply.error));
        emit failed(reply.error);
        emit thinkingChanged(false);
        return;
    }

    const QJsonObject message = reply.message;
    QString text = message.value(QStringLiteral("content")).toString().trimmed();
    const QJsonArray calls = message.value(QStringLiteral("tool_calls")).toArray();

    // The tools run HERE, at once: starting, steering or stopping a task takes
    // no time at all, so there is no second round trip to the model. Its own
    // words are said; where it gave none, what the tool reported is.
    bool acted = false;
    QString ack;
    QString refusal;
    QStringList did;
    QString recallQuery;
    QString recallCallId;
    QHash<QString, QString> results;
    for (const QJsonValue &value : calls) {
        const QJsonObject fn = value.toObject().value(QStringLiteral("function")).toObject();
        const QString name = fn.value(QStringLiteral("name")).toString();
        const QJsonObject args = QJsonDocument::fromJson(
                                     fn.value(QStringLiteral("arguments")).toString().toUtf8())
                                     .object();
        const QString callId = value.toObject().value(QStringLiteral("id")).toString();
        did.append(name);
        // RECALL waits for the memory's answer, below. Everything else runs
        // now. A second recall in the same turn is not run at all.
        if (name == QLatin1String("recall")) {
            if (m_recalledTurn != turn) {
                recallQuery = args.value(QStringLiteral("query")).toString().trimmed();
                recallCallId = callId;
            }
            continue;
        }
        // THE SAFETY NET for stop_task: work carries on unless the words
        // plainly asked for it to stop, and the user hears how to stop it.
        if (name == QLatin1String("stop_task") && !meansStop(asked)) {
            liveLog(QStringLiteral("talk %1: stop_task REFUSED - '%2' does not ask "
                                   "for anything to stop")
                        .arg(turn)
                        .arg(asked.left(160)));
            const QString kept = tr("I've kept it going. Say stop if you want it stopped.");
            results.insert(callId, kept);
            if (refusal.isEmpty())
                refusal = kept;
            continue;
        }
        bool ok = false;
        const QString said = runTalkTool(name, args, &ok);
        results.insert(callId, said);
        if (ok) {
            acted = true;
            if (ack.isEmpty())
                ack = said;
        } else if (refusal.isEmpty()) {
            refusal = said;
        }
    }

    if (!recallQuery.isEmpty()) {
        m_recalledTurn = turn;
        recall(turn, message, results, recallCallId, recallQuery);
        return; // still thinking: the answer comes after the memory's
    }

    // THE SAFETY NET for "remember that ...": a model that says "Got it" and
    // never calls remember has stored nothing, and the user would only find
    // out a week later. When the words were plainly an instruction to
    // remember and no remember was called, the user's own words are kept.
    if (!did.contains(QStringLiteral("remember")) && m_mcp->memory()) {
        const QString fact = rememberCommand(asked);
        if (!fact.isEmpty()) {
            liveLog(QStringLiteral("memory: the talker did not call remember - keeping "
                                   "the user's own words"));
            bool ok = false;
            runTalkTool(QStringLiteral("remember"),
                        QJsonObject{{QStringLiteral("fact"),
                                     QStringLiteral("The user asked to be remembered: %1").arg(fact)}},
                        &ok);
            if (ok) {
                acted = true;
                if (text.isEmpty())
                    text = tr("Got it.");
            }
        }
    }

    // A tool that could not do what was asked says so, whatever the model
    // wrote - "On it" over a task that never started would be a lie.
    if (!refusal.isEmpty())
        text = acted ? QString((text.isEmpty() ? ack : text) + QLatin1Char(' ') + refusal)
                     : refusal;
    if (text.isEmpty())
        text = ack;
    // Some reasoning models leave content empty and put a plain answer in
    // reasoning instead. Better to say that than nothing.
    if (text.isEmpty() && calls.isEmpty())
        text = message.value(QStringLiteral("reasoning")).toString().trimmed();

    if (text.isEmpty()) {
        m_asking.clear();
        liveLog(QStringLiteral("talk %1: EMPTY answer after %2 ms").arg(turn).arg(reply.ms));
        emit failed(tr("The model gave an empty answer."));
        emit thinkingChanged(false);
        return;
    }

    m_asking.clear();
    remember(QStringLiteral("user"), asked);
    remember(QStringLiteral("assistant"), text);
    trimHistory();

    m_unspokenTurn = turn;
    m_unspokenOnlyTalk = !acted;
    m_unspokenAsked = asked;
    m_unspokenAnswer = text;

    liveLog(QStringLiteral("talk %1 answered in %2 ms, %3 reasoning tokens%4: %5")
                .arg(turn)
                .arg(reply.ms)
                .arg(reply.reasoningTokens)
                .arg(did.isEmpty() ? QString()
                                   : QStringLiteral(" [%1]").arg(did.join(QStringLiteral(", "))))
                .arg(text.simplified().left(200)));

    // Said BEFORE thinking ends, so whatever listens for the end of thinking
    // already finds the answer waiting to be spoken.
    emit say(text, turn);
    emit thinkingChanged(false);
}

void Agent::recall(int turn, const QJsonObject &assistant,
                   const QHash<QString, QString> &results, const QString &callId,
                   const QString &query)
{
    // What the memory found goes back as that call's result, every other call
    // gets what it did, and the talker is asked once more for what to say.
    const auto answer = [this, turn, assistant, results, callId](const QString &found) {
        if (turn != m_turn || m_asking.isEmpty())
            return;
        QJsonArray messages = m_talkSent;
        messages.append(assistant);
        for (const QJsonValue &value : assistant.value(QStringLiteral("tool_calls")).toArray()) {
            const QString id = value.toObject().value(QStringLiteral("id")).toString();
            messages.append(QJsonObject{
                {QStringLiteral("role"), QStringLiteral("tool")},
                {QStringLiteral("tool_call_id"), id},
                {QStringLiteral("content"), id == callId ? found
                                                         : results.value(id, QStringLiteral("Done."))}});
        }
        askTalker(turn, messages, QStringLiteral("talk %1 recall").arg(turn));
    };

    McpServer *memory = m_mcp->memory();
    if (!memory) {
        answer(tr("The memory is not available right now."));
        return;
    }
    liveLog(QStringLiteral("talk %1: recalling '%2'").arg(turn).arg(query));
    memory->call(QStringLiteral("search"),
                 QJsonObject{{"tenant_id", kMemoryTenant},
                             {"user_id", kMemoryUser},
                             {"query", query}},
                 [this, answer](bool ok, const QJsonObject &result, const QString &error) {
        if (!ok) {
            answer(tr("The memory could not be searched: %1").arg(error));
            return;
        }
        const QString notes = memoryNotes(result);
        liveLog(QStringLiteral("recall found %1 note(s)")
                    .arg(notes.isEmpty() ? 0 : notes.count(QLatin1Char('\n')) + 1));
        answer(notes.isEmpty() ? tr("Nothing in the memory matches that.")
                               : tr("From the memory:\n%1").arg(notes));
    }, 5000);
}

QString Agent::memoryNotes(const QJsonObject &result)
{
    // Mnemosyne answers with structured content: whether it abstained, and
    // its hits, each with an id and the text that was remembered.
    QJsonObject found = result.value(QStringLiteral("structuredContent")).toObject();
    if (found.isEmpty())
        found = QJsonDocument::fromJson(McpServer::textOf(result).toUtf8()).object();
    if (found.value(QStringLiteral("abstained")).toBool())
        return {};

    QStringList lines;
    for (const QJsonValue &value : found.value(QStringLiteral("hits")).toArray()) {
        const QJsonObject hit = value.toObject();
        const QString text = hit.value(QStringLiteral("text")).toString().simplified();
        const QString cid = hit.value(QStringLiteral("id")).toString();
        if (text.isEmpty() || cid.isEmpty())
            continue;
        if (hit.value(QStringLiteral("score")).toDouble() < kMemoryMinScore)
            continue;
        const QString shortId = cid.left(12);
        m_memoryIds.insert(shortId, cid);
        lines.append(QStringLiteral("- [memory %1] %2").arg(shortId, text.left(400)));
        if (lines.size() >= 5)
            break;
    }
    return lines.join(QLatin1Char('\n'));
}

QString Agent::runTalkTool(const QString &name, const QJsonObject &args, bool *acted)
{
    *acted = false;

    // REMEMBER and FORGET go to the memory and the talker does not wait: they
    // take a moment, and if one fails the voice says so afterwards.
    if (name == QLatin1String("remember")) {
        McpServer *memory = m_mcp->memory();
        const QString fact = args.value(QStringLiteral("fact")).toString().simplified();
        if (!memory)
            return tr("My memory is not available right now.");
        if (fact.isEmpty())
            return tr("I didn't catch what to remember.");
        memory->call(QStringLiteral("capture"),
                     QJsonObject{{"tenant_id", kMemoryTenant},
                                 {"user_id", kMemoryUser},
                                 {"actor", "burnos"},
                                 {"source_type", "conversation"},
                                 {"content", fact}},
                     [this, fact](bool ok, const QJsonObject &result, const QString &error) {
            if (ok) {
                liveLog(QStringLiteral("memory: REMEMBERED '%1' as %2")
                            .arg(fact.left(200),
                                 result.value(QStringLiteral("structuredContent")).toObject()
                                     .value(QStringLiteral("cid")).toString().left(12)));
                return;
            }
            const QString why = error.isEmpty() ? McpServer::textOf(result).left(160) : error;
            liveLog(QStringLiteral("memory: could NOT remember '%1': %2").arg(fact.left(200), why));
            emit say(tr("I could not save that to my memory: %1").arg(why), 0);
        });
        *acted = true;
        return tr("Got it.");
    }

    if (name == QLatin1String("forget_memory")) {
        McpServer *memory = m_mcp->memory();
        const QString id = args.value(QStringLiteral("id")).toString().trimmed();
        const QString cid = m_memoryIds.value(id.left(12),
                                              id.size() == 64 ? id : QString());
        if (!memory)
            return tr("My memory is not available right now.");
        if (cid.isEmpty())
            return tr("I couldn't tell which memory that was.");
        memory->call(QStringLiteral("forget"),
                     QJsonObject{{"tenant_id", kMemoryTenant}, {"cid", cid}},
                     [this, cid](bool ok, const QJsonObject &result, const QString &error) {
            if (ok) {
                liveLog(QStringLiteral("memory: FORGOT %1").arg(cid.left(12)));
                return;
            }
            const QString why = error.isEmpty() ? McpServer::textOf(result).left(160) : error;
            liveLog(QStringLiteral("memory: could NOT forget %1: %2").arg(cid.left(12), why));
            emit say(tr("I could not forget that: %1").arg(why), 0);
        });
        *acted = true;
        return tr("Forgotten.");
    }

    if (name == QLatin1String("start_task")) {
        const QString title = args.value(QStringLiteral("title")).toString().simplified();
        const QString instruction = args.value(QStringLiteral("instruction")).toString().trimmed();
        if (instruction.isEmpty())
            return tr("I didn't catch what to do.");
        if (liveTasks() >= kMaxLiveTasks) {
            return tr("I'm already doing three things, so that has to wait until "
                      "one of them finishes.");
        }
        auto *t = new Task(++m_nextTask, title.isEmpty() ? instruction.left(40) : title,
                           instruction, this);
        connect(t, &Task::finished, this, &Agent::onTaskFinished);
        connect(t, &Task::asked, this, &Agent::onTaskAsked);
        m_tasks.append(t);
        t->start();
        updateWorking();
        *acted = true;
        return tr("On it.");
    }

    if (name == QLatin1String("steer_task")) {
        Task *t = task(taskNumber(args.value(QStringLiteral("task"))));
        const QString message = args.value(QStringLiteral("message")).toString().trimmed();
        if (!t || !t->isLive())
            return tr("That one has already finished.");
        if (message.isEmpty())
            return tr("I didn't catch the change.");
        t->steer(message);
        *acted = true;
        return tr("Okay.");
    }

    if (name == QLatin1String("stop_task")) {
        const int id = taskNumber(args.value(QStringLiteral("task")));
        int stopped = 0;
        for (const QPointer<Task> &t : std::as_const(m_tasks)) {
            if (t && t->isLive() && (id <= 0 || t->id() == id)) {
                t->stop();
                ++stopped;
            }
        }
        updateWorking();
        *acted = true;
        return stopped ? tr("Stopped.") : tr("Nothing was running.");
    }

    return tr("I can't do that.");
}

QString Agent::statusReport() const
{
    QStringList lines;
    lines.append(tr("The time now is %1. The user's home folder is %2, and their "
                    "Desktop is %2\\Desktop.")
                     .arg(nowWords(), QDir::toNativeSeparators(QDir::homePath())));

    const DesktopWindow front = Computer::foreground();
    if (!front.title.isEmpty())
        lines.append(tr("The window in front is '%1'.").arg(front.title));

    // So "which model is doing that?" has a true answer, and the talker never
    // takes credit for work a stronger model is doing beside it.
    const QString talking = Settings::openRouterModel();
    const QString working = Settings::taskModel();
    lines.append(working == talking
                     ? tr("You are running on %1, and background tasks run on the "
                          "same model.").arg(talking)
                     : tr("You are running on %1, chosen to keep the conversation "
                          "fast. Background tasks think separately, at the same "
                          "time, on %2.").arg(talking, working));

    const QStringList servers = m_mcp->readyNames();
    if (!servers.isEmpty()) {
        lines.append(tr("Tasks can use the user's MCP servers directly through their "
                        "mcp tool, already connected: %1 - memory is the user's "
                        "long-term memory.")
                         .arg(servers.join(QStringLiteral(", "))));
    }

    QStringList report;
    for (const QPointer<Task> &t : m_tasks) {
        if (!t)
            continue;
        const QString name = tr("Task %1, '%2'").arg(t->id()).arg(t->title());
        switch (t->state()) {
        case Task::State::Running:
            report.append(tr("%1: running for %2, %3 step(s) so far, now %4.")
                              .arg(name, ago(t->runningMs()))
                              .arg(t->steps())
                              .arg(t->step().isEmpty() ? tr("thinking") : t->step()));
            break;
        case Task::State::WaitingForUser:
            report.append(tr("%1: waiting for the user to answer: %2")
                              .arg(name, t->question()));
            break;
        case Task::State::Done:
            report.append(tr("%1: finished %2 ago and reported: %3")
                              .arg(name, ago(t->endedAgoMs()), t->outcome()));
            break;
        case Task::State::Failed:
            report.append(tr("%1: failed %2 ago: %3")
                              .arg(name, ago(t->endedAgoMs()), t->outcome()));
            break;
        case Task::State::Stopped:
            report.append(tr("%1: stopped by the user %2 ago.")
                              .arg(name, ago(t->endedAgoMs())));
            break;
        }
    }
    if (report.isEmpty())
        lines.append(tr("No background tasks."));
    else
        lines.append(tr("Background tasks:") + QLatin1Char('\n') + report.join(QLatin1Char('\n')));
    return lines.join(QLatin1Char('\n'));
}

void Agent::onTaskFinished(int id)
{
    updateWorking();
    Task *t = task(id);
    if (!t || t->state() == Task::State::Stopped)
        return; // stopped on request: the talker has already said so

    const QString news = t->outcome();
    if (news.isEmpty())
        return;
    // What the voice is about to say becomes part of the conversation, so
    // "open the first one" can refer to it.
    remember(QStringLiteral("assistant"), news);
    trimHistory();
    emit say(news, 0);
}

void Agent::onTaskAsked(int, const QString &question)
{
    remember(QStringLiteral("assistant"), question);
    trimHistory();
    emit say(question, 0);
}

void Agent::spoken(int turn)
{
    if (turn == m_unspokenTurn)
        m_unspokenTurn = 0;
}

void Agent::stopAll()
{
    if (m_request) {
        m_ai->cancel(m_request);
        m_request = 0;
        m_asking.clear();
    }
    m_unspokenTurn = 0;
    for (const QPointer<Task> &t : std::as_const(m_tasks)) {
        if (t && t->isLive())
            t->stop();
    }
    updateWorking();
    emit thinkingChanged(false);
}

void Agent::remember(const QString &role, const QString &text)
{
    if (text.trimmed().isEmpty())
        return;
    m_history.append(msg(role, text.trimmed()));
}

void Agent::forget(const QString &asked, const QString &answer)
{
    // The newest exchange that matches, and only if it is still whole.
    for (int i = int(m_history.size()) - 1; i > 0; --i) {
        const QJsonObject a = m_history.at(i).toObject();
        const QJsonObject q = m_history.at(i - 1).toObject();
        if (a.value(QStringLiteral("role")).toString() == QLatin1String("assistant")
            && a.value(QStringLiteral("content")).toString() == answer.trimmed()
            && q.value(QStringLiteral("role")).toString() == QLatin1String("user")
            && q.value(QStringLiteral("content")).toString() == asked.trimmed()) {
            m_history.removeAt(i);
            m_history.removeAt(i - 1);
            return;
        }
    }
}

void Agent::trimHistory()
{
    const auto characters = [this]() {
        int total = 0;
        for (const QJsonValue &line : std::as_const(m_history))
            total += int(line.toObject().value(QStringLiteral("content")).toString().size());
        return total;
    };

    while (m_history.size() > kMaxMessages
           || (m_history.size() > 2 && characters() > kMaxChars))
        m_history.removeFirst();

    // It must begin on something the user said.
    while (!m_history.isEmpty()
           && m_history.first().toObject().value(QStringLiteral("role")).toString()
                  != QLatin1String("user"))
        m_history.removeFirst();
}

void Agent::pruneTasks()
{
    for (int i = int(m_tasks.size()) - 1; i >= 0; --i) {
        Task *t = m_tasks.at(i);
        if (!t) {
            m_tasks.removeAt(i);
        } else if (!t->isLive() && t->endedAgoMs() > kKeepFinishedMs) {
            m_tasks.removeAt(i);
            t->deleteLater();
        }
    }
}

void Agent::updateWorking()
{
    const bool working = liveTasks() > 0;
    if (working == m_working)
        return;
    m_working = working;
    emit workingChanged(working);
}

} // namespace BurnOS
