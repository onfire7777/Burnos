#ifndef SETTINGS_H
#define SETTINGS_H

#include <QPoint>
#include <QString>
#include <QStringList>

namespace BurnOS {
namespace Settings {

// The OpenRouter API key, or an empty string when none is stored.
// Decrypted on demand; never cached in a member anywhere.
QString openRouterKey();

// Stores the key, encrypted for the current Windows user. Passing an empty
// string removes it entirely rather than storing an empty value.
void setOpenRouterKey(const QString &key);

// True when a non empty key is stored. Cheaper than decrypting to check.
bool hasOpenRouterKey();

// The OpenRouter model id used for every turn, falling back to the shipped
// default when nothing has been chosen.
QString openRouterModel();
void setOpenRouterModel(const QString &model);
QString defaultOpenRouterModel();

// Token budget for one answer. Clamped, because a reasoning model spends part
// of it thinking and a tiny budget comes back empty.
int replyTokens();
void setReplyTokens(int tokens);

// How readily the microphone opens, 0 to 100, where 100 picks up the quietest
// speech. It does NOT need to be low to reject noise: a spike is thrown out by
// the attack window in sttengine.cpp, which is a far better filter than simply
// refusing to hear anything quiet.
int micSensitivity();
void setMicSensitivity(int percent);

// The same setting as the RMS the gate actually opens at, 0.010 to 0.050.
// Only used when the Silero weights cannot be loaded.
double micOpenThreshold();

// The same setting again, as the speech probability Silero has to reach before
// a phrase starts: 0.85 at the least sensitive, 0.35 at the most.
double vadThreshold();

// How long a pause has to last before a phrase counts as finished, in
// milliseconds. Too short and it answers before the speaker has finished
// thinking; too long and every reply feels sluggish. Clamped, because zero
// would end a phrase between two words.
int hangoverMs();
void setHangoverMs(int ms);

// Which of the eleven kokoro-en-v0_19 speakers answers, 0 to 10.
int ttsVoice();
void setTtsVoice(int voice);

// Playback volume of the spoken answer, 0 to 100.
int ttsVolume();
void setTtsVolume(int percent);

// How big the orb is on the desktop, in pixels across. The window is exactly
// this, because the orb never changes size: an answer is said one word at a
// time in the middle of it. Clamped, because below about 140 a word of any
// length stops fitting and above 320 the orb is furniture rather than a light.
int orbSize();
void setOrbSize(int px);

// Where the orb was last left on screen, its top left corner. Restored on the
// next start, clamped to a screen that still exists - a position saved on a
// monitor that has since been unplugged must not strand the orb off screen.
bool hasOrbPosition();
QPoint orbPosition();
void setOrbPosition(const QPoint &topLeft);

// Whether the orb stays above every other window.
bool alwaysOnTop();
void setAlwaysOnTop(bool onTop);

// The Composio project API key, which is what lets the orb reach Gmail, Slack,
// Notion, GitHub and the rest. Stored exactly like the OpenRouter key -
// encrypted for this Windows user with DPAPI - because it is every bit as much
// a credential: it can act on every account connected under it.
QString composioKey();
void setComposioKey(const QString &key);
bool hasComposioKey();

// Who Composio thinks is asking. Composio groups connected accounts by a user
// id of our choosing, and this install is one user, so it is generated once on
// first use and then never changes. It must NOT be the Windows account name:
// renaming the account would orphan every connection made before the rename.
QString composioUserId();

// The apps connected in the integrations dialog, by Composio slug. This is a
// CACHE of what the service already knows, written whenever that dialog
// refreshes, so the model can be told what it may reach without a network
// round trip at the top of every question.
QStringList composioApps();
void setComposioApps(const QStringList &slugs);

// The Composio session this install works in. One per user, made on first use
// and then reused for good, because Composio keeps sessions on the server and
// never expires them; every tool search, tool run and sign in link goes
// through it. Forgotten whenever the key changes, since a session belongs to
// the project the key came from.
QString composioSessionId();
void setComposioSessionId(const QString &id);

} // namespace Settings
} // namespace BurnOS

#endif // SETTINGS_H
