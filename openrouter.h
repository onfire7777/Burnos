#ifndef OPENROUTER_H
#define OPENROUTER_H

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

// THE ONE WAY BURNOS TALKS TO A MODEL: send a conversation, get one message
// back.
//
// Since v0.4 this knows nothing about conversations, tools or turns. The talker
// and every background task keep their own messages and hand them to send();
// what lives here is everything they share - the key, the reasoning effort, the
// timeouts, the log line, and a model that turns out not to accept pictures.
// Several requests can be in flight at once: the talker's, and one for each
// task.
//
// SINCE v0.5 EACH REQUEST NAMES ITS OWN MODEL. The talker asks a fast one so
// the conversation keeps its pace, and every task asks the working model,
// which may be a slower and far stronger one - both at the same moment, over
// the same connection, neither waiting on the other. A request that names no
// model goes to the talking model, which is all there was before.
class OpenRouterClient : public QObject
{
    Q_OBJECT

public:
    struct Request
    {
        QJsonArray messages;
        QJsonArray tools;    // none offered when empty
        QString who;         // "talk 3", "task 2" - names it in the log
        QString model;       // empty is the talking model from Settings
        int timeoutMs = 30000;
        int maxTokens = 0;   // 0 is the answer length from Settings
        // Ask OpenRouter for whichever provider of the model answers FIRST.
        // The talker wants this: the same model has taken anywhere from two to
        // twenty eight seconds to answer, depending on who served it.
        bool fastest = false;
    };

    struct Reply
    {
        bool ok = false;
        QString error;       // worded for a person, when not ok
        QJsonObject message; // the first choice's message, when ok
        int reasoningTokens = 0;
        qint64 ms = 0;
    };

    using Done = std::function<void(const Reply &)>;

    explicit OpenRouterClient(QObject *parent = nullptr);

    // One chat completion. `done` is called exactly once - never from inside
    // send() itself, and never at all for a request cancelled first. The id is
    // for cancel().
    quint64 send(const Request &request, Done done);
    void cancel(quint64 id);

    // Whether this model may be sent a picture. It stops being true the first
    // time OpenRouter refuses one for it - for that model only, so a working
    // model that sees is never blinded by a talking model that does not.
    bool canSee(const QString &model) const;

    // What a model that cannot see is told in place of a screenshot.
    static QString blindNote();

private:
    void post(quint64 id, Request request, Done done, bool retried);
    static QJsonArray dropImages(const QJsonArray &messages, bool *dropped);

    QNetworkAccessManager *m_net = nullptr;
    // Every request not yet answered. A null reply is a failure already decided
    // and waiting one turn of the event loop to be delivered.
    QHash<quint64, QNetworkReply *> m_live;
    quint64 m_nextId = 0;

    // A MODEL THAT CANNOT SEE MUST NOT KILL THE TURN. OpenRouter refuses the
    // whole request with a 404 - "No endpoints found that support image input"
    // - when the chosen model has no endpoint that takes a picture. The first
    // refusal is answered by asking again without the picture, and the model's
    // name is kept here so it is never sent another one. A set, because the
    // talking and the working model are judged separately.
    QSet<QString> m_blind;
};

#endif // OPENROUTER_H
