#ifndef CHATGPT_H
#define CHATGPT_H

#include <QList>
#include <QPair>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class QObject;

namespace BurnOS::ChatGpt {

// SIGN IN WITH CHATGPT: the same OAuth sign-in OpenAI's Codex uses, so a
// chatgpt:<model> answers on the user's ChatGPT subscription. The browser opens
// OpenAI's own page; the code comes back to 127.0.0.1:1455 and is swapped for
// tokens, kept encrypted in Settings and refreshed before they expire. Tolerated
// by OpenAI rather than an official API, so it may change on their side.
using Done = std::function<void(bool ok, const QString &message)>;
void signIn(QObject *context, Done done);
void signOut();
bool signedIn();
// The email and plan the sign-in belongs to, for Settings to show.
QString who();

// A current access token and account id, refreshed first when needed. `got`
// is called once, never from inside this call.
using Got = std::function<void(const QString &access, const QString &account, const QString &error)>;
void withAccess(QNetworkAccessManager *net, QObject *context, Got got, bool forceRefresh = false);

// The models this sign-in may use, as OpenAI lists them for it (hidden ones
// left out): "slug" -> "Display name - description". Asking also proves the
// sign-in works: `error` says why when it does not.
using Models = std::function<void(const QList<QPair<QString, QString>> &models, const QString &error)>;
void models(QNetworkAccessManager *net, QObject *context, Models done);

} // namespace BurnOS::ChatGpt

#endif // CHATGPT_H
