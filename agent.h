#ifndef AGENT_H
#define AGENT_H

#include "computeruse.h"
#include "openrouter.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>

class ComposioClient;

namespace BurnOS {

class Agent;
class McpClient;
class WebBrowser;

// ONE PIECE OF BACKGROUND WORK - "open GitHub", "write a poem in Notepad".
//
// A task is a small agent of its own: its own conversation with the model,
// starting from the brief the talker wrote for it, then its tool calls and
// their results, round and round until the job is done. It runs while the
// user carries on talking about anything else, and it never speaks for itself:
// its result, or a question, goes up to the Agent, which decides when the
// voice says it.
class Task : public QObject
{
    Q_OBJECT

public:
    enum class State { Running, WaitingForUser, Done, Failed, Stopped };

    Task(int id, const QString &title, const QString &instruction, Agent *agent);

    int id() const { return m_id; }
    QString title() const { return m_title; }
    State state() const { return m_state; }
    bool isLive() const
    {
        return m_state == State::Running || m_state == State::WaitingForUser;
    }
    // What it is doing right now, in a few plain words, for the talker's report.
    QString step() const { return m_step; }
    // What it asked the user and is waiting on.
    QString question() const { return m_question; }
    // Its result once it has ended, worded to be said out loud.
    QString outcome() const { return m_outcome; }
    int steps() const { return m_toolCalls; }
    // The model it thinks with: the working model, or the talking one once the
    // working model has failed it twice running.
    QString model() const;
    qint64 runningMs() const;
    qint64 endedAgoMs() const;

    void start();
    // More, or different, from the user while it works - or their answer to
    // its question. Relayed by the talker.
    void steer(const QString &message);
    void stop();

signals:
    void finished(int id);
    void asked(int id, const QString &question);

private:
    void ask();
    void onAnswer(const OpenRouterClient::Reply &reply);
    void runNextCall();
    void runAppAction(const QVariantMap &args);
    void runMcp(const QVariantMap &args);
    void runWeb(const QVariantMap &args);
    void answerCall(const QString &result, const QByteArray &png = QByteArray());
    void flushShots();
    void pruneContext();
    void end(State state, const QString &outcome);

    const int m_id;
    const QString m_title;
    const QString m_instruction;
    Agent *const m_agent;

    State m_state = State::Running;
    QString m_step;
    QString m_question;
    QString m_outcome;
    QElapsedTimer m_clock;
    QElapsedTimer m_ended;
    int m_toolCalls = 0;
    int m_retries = 0;
    // Set, for good, when a separate working model failed twice in a row and
    // the task moved to the talking model to finish rather than die.
    bool m_onTalkModel = false;
    bool m_nudged = false;
    bool m_wrappingUp = false;

    // The whole conversation of this task: system notes, the brief, and every
    // tool call and result since.
    QJsonArray m_messages;
    // The tool calls from the model's last answer still to be run, in order,
    // and the id of the one running now.
    QJsonArray m_calls;
    QString m_callId;
    // Pictures taken by those calls. They join the conversation only once
    // every call has its result - a user message between two tool results is
    // refused.
    QList<QByteArray> m_shots;
    // What the user added while a step was running, sent with the next request.
    QStringList m_steering;
    quint64 m_request = 0;
    // Which window it is working in, the controls it was last shown and its
    // last screenshot. Carried from one action to the next, and to nobody else.
    DeskState m_desk;
    // Set when it ends, and read by any action still running on the hands, so
    // a stopped task lets go of the mouse at once.
    std::shared_ptr<std::atomic_bool> m_cancelled;
};

// THE BRAIN: the talker in front, the tasks behind it.
//
// Everything the user says goes to the TALKER, one short and fast model call
// that knows the conversation, the time, the window in front and what every
// task is doing. It answers in a sentence, and when something needs DOING it
// starts a task and goes on talking. Talking to Burnos while it works
// therefore never interrupts the work. An interruption cancels speech, never
// work.
class Agent : public QObject
{
    Q_OBJECT

public:
    Agent(OpenRouterClient *ai, ComposioClient *composio, QObject *parent = nullptr);

    // Something the user said, transcribed.
    void hear(const QString &text);
    // The voice has started saying what the talker answered on this turn, so
    // the turn is closed and cannot be taken back any more.
    void spoken(int turn);
    // Stopped, by a double click on the orb: the talker and every task stop,
    // now. A single click only mutes, and reaches nothing in here.
    void stopAll();

    // From the moment something is heard until the talker's answer is ready:
    // the memory lookup before it, the request, and a recall after it.
    bool isThinking() const { return !m_asking.isEmpty(); }
    int liveTasks() const;

    OpenRouterClient *ai() const { return m_ai; }
    ComposioClient *composio() const { return m_composio; }
    McpClient *mcp() const { return m_mcp; }
    // The hidden browser the tasks look things up in.
    WebBrowser *web() const { return m_web; }

    // FOR THE PROBES (--talk-probe, --context-probe), which run this very
    // class with no microphone and no voice.
    // What the talker would be sent for these words, without asking it.
    void context(const QString &said, std::function<void(const QJsonArray &)> done);
    // How big the last request to the talker was, part by part, and what the
    // provider counted for it.
    QJsonObject lastStats() const { return m_stats; }
    // Nothing is on its way to the memory.
    bool isQuiet() const { return m_writes == 0 && m_held.isEmpty(); }
    // A probe starts no tasks: a test must never touch the desktop.
    void setProbe(bool on) { m_probe = on; }
    // Except the one a probe script asks for by name, with "@task": started
    // directly, with no talker in front of it, to try what a task does with
    // an instruction - its research in the hidden browser, above all.
    void startTask(const QString &title, const QString &instruction);

signals:
    // Say this out loud. `turn` is the talker turn it answers, or 0 for news
    // from a task.
    void say(const QString &text, int turn);
    // Drop whatever was queued to be said for this turn: the user carried on
    // talking before it was said.
    void retract(int turn);
    // The talker was asked, in whatever words, to start typing what is said
    // ("start"), to stop ("stop"), or to read the last of it back ("read_back").
    void transcriptionAsked(const QString &action);
    void thinkingChanged(bool thinking);
    void workingChanged(bool working);
    void failed(const QString &reason);

private:
    void talk(const QString &said);
    // THE WORKING SET: what the memory gave for one answer, and all of what
    // stands in for a chat. Built afresh each time and thrown away after.
    struct Recalled
    {
        QString profile;   // what the user asked to be kept in mind, as lines
        QString notes;     // what bears on the words just said, as lines
        QJsonArray recent; // the newest lines of the conversation, as messages
        int unsaved = 0;   // how many of those are not in the memory yet
    };
    // Asks the memory, and calls `then` exactly once with what it gave - at
    // once, with only the lines not saved yet, when there is no memory. `who`
    // names it in the log.
    void lookup(const QString &who, const QString &said,
                std::function<void(const Recalled &)> then);
    // The whole request for the talker: instructions, status, the working
    // set, the new words. Sizes every part into m_stats.
    QJsonArray compose(const QString &said, const Recalled &recalled);
    void sendTalk(int turn, const QString &said, const Recalled &recalled);
    // One question to the talking model - fast provider, and the talker's
    // tools unless `tools` is false - answered on onTalkAnswer.
    void askTalker(int turn, const QJsonArray &messages, const QString &who, bool tools = true);
    void onTalkAnswer(int turn, const OpenRouterClient::Reply &reply);
    // The talker asked its memory something: look it up and ask the talker
    // once more with what was found, for the answer to say.
    void recall(int turn, const QJsonObject &assistant, const QHash<QString, QString> &results,
                const QString &callId, const QString &query);
    // What a memory search found, as lines for the talker, each with a short
    // id it can hand back to forget or replace one - without the question
    // itself and without what `tail` already shows word for word.
    QString memoryNotes(const QJsonObject &found, const QString &query, const QString &said,
                        const QJsonArray &tail, int maxLines, int maxChars);
    // The user's profile as the memory returned it, as lines with short ids:
    // standing instructions first, then what bears on `said`, then the rest.
    QString profileLines(const QJsonObject &found, const QString &said);
    // One write to the memory that nobody waits for.
    void write(const QString &tool, const QJsonObject &arguments, const QString &done,
               const QString &sorry);
    // A stored line into working memory, citing its ledger record.
    void seed(const QString &role, const QString &line, const QDateTime &at, const QString &cid,
              int triesLeft);
    void saved(const QString &role, const QString &line, const QDateTime &at);
    // Carries out one of the talker's three tools and says, in a few words,
    // what came of it. `acted` is false when nothing could be done.
    QString runTalkTool(const QString &name, const QJsonObject &args, bool *acted);
    QString statusReport() const;
    void onTaskFinished(int id);
    void onTaskAsked(int id, const QString &question);
    // Into the memory - every word, the moment it is said. `role` is user or
    // assistant, `at` when it was said. It goes to the ledger (long-term) and
    // into working memory (short-term), the second citing the first.
    void store(const QString &role, const QString &text,
               const QDateTime &at = QDateTime::currentDateTimeUtc());
    // Stores what was said while the memory was not up.
    void flushBacklog();
    // Stores what the user said this turn, held back while the memory was
    // read for it.
    void release();
    void pruneTasks();
    void updateWorking();
    Task *task(int id) const;

    OpenRouterClient *const m_ai;
    ComposioClient *const m_composio;
    // The user's MCP servers, their long-term memory among them.
    McpClient *m_mcp = nullptr;
    // A browser of Burnos's own, with no window: started the first time a
    // task needs the web, one tab a task.
    WebBrowser *m_web = nullptr;
    // Short ids the talker has been shown - of remembered lines and of profile
    // lines - and the full ids behind them.
    QHash<QString, QString> m_memoryIds;
    QHash<QString, QString> m_profileIds;
    // What the profile says, word for word, and what was last remembered: the
    // talker now and then asks for the same thing to be kept twice, a turn
    // apart, and it is kept once.
    QStringList m_profileSaid;
    QStringList m_remembered;
    // The ledger record of the last thing the user said: the evidence for
    // whatever the talker is asked to remember about it.
    QString m_lastUserCid;
    // What the talker was first sent this turn, and that plus the lookups it
    // has made since. It may look twice in a turn; what it found is kept for
    // the one time it is asked again because it gave no words.
    QJsonArray m_talkBase;
    QJsonArray m_talkSent;
    int m_recallTurn = 0;
    int m_recalls = 0;
    QString m_recallFound;
    int m_nudgedTurn = 0;
    // The last request's size and cost, a count of the writes to the memory
    // still in flight, and whether this is a probe.
    QJsonObject m_stats;
    int m_writes = 0;
    bool m_probe = false;
    // This turn's answer is transcription starting or being read back: the
    // talker's own words are not said.
    bool m_silentTurn = false;

    // THERE IS NO CONVERSATION KEPT HERE. What was said lives in the memory
    // and is read back from it before every answer. These two are the
    // memory's own queue and nothing more.
    struct Unstored
    {
        QString role;
        QString text;
        QDateTime at;
    };
    // What could not be stored because the memory was not up: stored, in the
    // order it was said, the moment it is.
    QList<Unstored> m_backlog;
    // What has been said and is not confirmed in working memory yet - on its
    // way, or waiting in the backlog. Shown with the newest lines until it
    // is, at most a dozen, and dropped from here the moment it is saved.
    QList<Unstored> m_unsaved;
    // The user's words of this turn, held for the instant the memory is read
    // for them - see hear() - and stored the moment it has been.
    QList<Unstored> m_held;

    QList<QPointer<Task>> m_tasks;
    int m_nextTask = 0;
    bool m_working = false;

    int m_turn = 0;
    quint64 m_request = 0; // the talker's request in flight, 0 when none
    QString m_asking;      // what that request is about

    // The last answer that has not been said out loud yet. If the user keeps
    // talking before it is, it is taken back; and if it only TALKED - started
    // nothing - the whole thought is asked again as one, which is what stops a
    // pause for breath splitting one question into two.
    int m_unspokenTurn = 0;
    bool m_unspokenOnlyTalk = false;
    QString m_unspokenAsked;
    QString m_unspokenAnswer;
};

// THE PROBE behind --talk-probe and --context-probe: the real conversation
// path - hear, the memory, the model, the answer - with no microphone, no
// voice and no window, so a conversation can be scripted and checked. One line
// of `script` is one thing said. "@wait 2" waits two seconds, "@context words"
// records what would be sent for those words without asking the model, and
// "@nomemory" does not wait for the memory to come up, and "@rapid" says each
// line the instant the last was answered instead of waiting, as a person
// would, until the reply is in the memory. What was answered, how
// long it took and how big each request was go to `outPath` as JSON. Runs the
// application's event loop and returns its exit code.
int runProbe(const QStringList &script, const QString &outPath);

} // namespace BurnOS

#endif // AGENT_H
