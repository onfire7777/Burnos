#ifndef OPENROUTER_H
#define OPENROUTER_H

#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QVariantMap>

class QNetworkAccessManager;
class QNetworkReply;

// Chat completions against OpenRouter, carrying the conversation so far AND the
// ability to operate this machine.
//
// THE MODEL DECIDES FOR ITSELF whether a sentence needs a tool. There used to
// be a separate decisions model in front of it choosing between talk and five
// fixed actions, and the trouble with that is it had no reasoning: it could
// only pick from lists written out in advance, so it could not work out that
// "open my editor" means Qt Creator, or which of forty controls is the one the
// user meant. Intelligence was the missing piece, so the reasoning model now
// does the whole job in one conversation.
//
// The turn is a small loop: ask -> the model may answer, or ask for the tool ->
// the caller runs it and hands back what happened -> the model says it out
// loud. Only the question and the final answer are kept in the history.
class OpenRouterClient : public QObject
{
    Q_OBJECT

public:
    explicit OpenRouterClient(QObject *parent = nullptr);

    // screenContext describes what is on screen right now. It rides along as a
    // system message for THIS TURN ONLY - it is never remembered, because a
    // stale list of controls is worse than none.
    bool ask(const QString &prompt, const QString &screenContext = QString());

    // Hand back what the tool actually did. The turn carries on from there and
    // replied() follows with what to say out loud.
    //
    // A PNG may ride along - that is how the model gets EYES. The tool role
    // cannot carry an image, so the picture follows the tool result as a user
    // message, which is the shape the API does accept.
    void finishTool(const QString &result, const QByteArray &png = QByteArray());

    bool isBusy() const { return m_busy; }
    bool isAwaitingTool() const { return m_awaitingTool; }

    // Abandon the turn in flight. Nothing is emitted and nothing is remembered.
    void cancel();

    void clearHistory();
    int turnsRemembered() const { return int(m_history.size() / 2); }

signals:
    void replied(const QString &text);
    // The model wants a tool. `function` says which - computer_use for this
    // machine, app_action for one of the user's connected apps - and `args` is
    // that call's arguments, parsed. Two tools share one signal because a turn
    // is the same shape either way: run it, hand back what happened, let the
    // model say it out loud.
    void toolRequested(const QString &function, const QVariantMap &args);
    void failed(const QString &reason);

private:
    bool post();
    void remember(const QString &role, const QString &text);
    void trimHistory();
    void endTurn();

    QNetworkAccessManager *m_net = nullptr;
    QNetworkReply *m_reply = nullptr;
    bool m_busy = false;
    bool m_awaitingTool = false;

    // Alternating user and assistant messages, oldest first. Whole exchanges
    // are dropped in pairs when it grows too long, so it never begins on an
    // answer with no question above it.
    QJsonArray m_history;

    // The messages belonging to the turn in flight: the question, and then the
    // model's tool call and the result if there was one. Folded into the
    // history, in its short form, only once an answer comes back.
    QJsonArray m_turn;
    QString m_pending;
    QString m_context;
    QString m_callId;
};

#endif // OPENROUTER_H
