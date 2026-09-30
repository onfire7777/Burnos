#ifndef AGENT_H
#define AGENT_H

#include "computeruse.h"
#include "openrouter.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <atomic>
#include <memory>

class ComposioClient;

namespace BurnOS {

class Agent;
class McpClient;

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
    // Muted: the talker and every task stop, now.
    void stopAll();

    // From the moment something is heard until the talker's answer is ready:
    // the memory lookup before it, the request, and a recall after it.
    bool isThinking() const { return !m_asking.isEmpty(); }
    int liveTasks() const;

    OpenRouterClient *ai() const { return m_ai; }
    ComposioClient *composio() const { return m_composio; }
    McpClient *mcp() const { return m_mcp; }

signals:
    // Say this out loud. `turn` is the talker turn it answers, or 0 for news
    // from a task.
    void say(const QString &text, int turn);
    // Drop whatever was queued to be said for this turn: the user carried on
    // talking before it was said.
    void retract(int turn);
    void thinkingChanged(bool thinking);
    void workingChanged(bool working);
    void failed(const QString &reason);

private:
    void talk(const QString &said);
    void sendTalk(int turn, const QString &said, const QString &memories,
                  const QJsonArray &recent);
    // One question to the talking model - fast provider, the talker's tools -
    // answered on onTalkAnswer.
    void askTalker(int turn, const QJsonArray &messages, const QString &who);
    void onTalkAnswer(int turn, const OpenRouterClient::Reply &reply);
    // The talker asked its memory something: look it up and ask the talker
    // once more with what was found, for the answer to say.
    void recall(int turn, const QJsonObject &assistant, const QHash<QString, QString> &results,
                const QString &callId, const QString &query);
    // What a memory search found, as lines for the talker, each with a short
    // id it can hand back to forget one.
    QString memoryNotes(const QJsonObject &result);
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
    void pruneTasks();
    void updateWorking();
    Task *task(int id) const;

    OpenRouterClient *const m_ai;
    ComposioClient *const m_composio;
    // The user's MCP servers, their long-term memory among them.
    McpClient *m_mcp = nullptr;
    // Short memory ids the talker has been shown, and the full ids behind them.
    QHash<QString, QString> m_memoryIds;
    // What the talker was last sent, for a recall's second round, and the turn
    // that has already had one - a second recall in a turn is refused.
    QJsonArray m_talkSent;
    int m_recalledTurn = 0;

    // THERE IS NO CONVERSATION KEPT HERE. What was said lives in the memory
    // and is read back from it before every answer. This is only what could
    // not be stored yet because the memory was not up - stored, in the order
    // it was said, the moment it is - and it is never shown to a model.
    struct Unstored
    {
        QString role;
        QString text;
        QDateTime at;
    };
    QList<Unstored> m_backlog;

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

} // namespace BurnOS

#endif // AGENT_H
