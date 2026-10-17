#ifndef MCPAUTH_H
#define MCPAUTH_H

#include <QString>

#include <functional>

class QNetworkAccessManager;
class QObject;

// SIGNING IN TO A REMOTE MCP SERVER, as the MCP authorization spec has it
// (2026-07-28): the server's protected resource metadata names its
// authorization server, BurnOS registers itself there (dynamic client
// registration, kept per issuer), and the user signs in in their browser
// (authorization code with PKCE, back to a loopback port). The tokens are kept
// encrypted for this Windows user (Settings::secret), per server.
namespace BurnOS::McpAuth {

using Done = std::function<void(bool ok, const QString &message)>;

// The access token kept for this server, or empty. Never refreshes.
QString token(const QString &serverUrl);
// Whether a refresh token is kept, so refresh() can be tried.
bool canRefresh(const QString &serverUrl);
// The full sign-in. `challenge` is the WWW-Authenticate header of the 401 that
// asked for it (it may name the metadata and the scope), or empty.
void signIn(QNetworkAccessManager *net, QObject *context, const QString &serverUrl,
            const QString &challenge, Done done);
// A new access token from the kept refresh token.
void refresh(QNetworkAccessManager *net, QObject *context, const QString &serverUrl, Done done);

} // namespace BurnOS::McpAuth

#endif // MCPAUTH_H
