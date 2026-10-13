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

// The TALKING model: every answer the orb says out loud, and every decision to
// hand work off to a task. Falls back to the shipped default when nothing has
// been chosen. Stored where the one and only model always was, so an install
// from before v0.5 goes on talking on exactly what it chose.
QString openRouterModel();
void setOpenRouterModel(const QString &model);
QString defaultOpenRouterModel();

// The WORKING model: what every background task thinks with - driving the
// desktop, writing code, anything handed off with start_task - at the same
// time as the talking model carries on the conversation. Nothing stored means
// the same model as the talking, which is how every version before 0.5 ran,
// so taskModel() always names a real model and hasTaskModel() says whether it
// is a separate one.
QString taskModel();
bool hasTaskModel();
void setTaskModel(const QString &model); // empty goes back to "same as talking"

// Token budget for one answer. Clamped, because a reasoning model spends part
// of it thinking and a tiny budget comes back empty.
int replyTokens();
void setReplyTokens(int tokens);

// How readily the microphone opens, 0 to 100, where 100 picks up the quietest
// speech. It does NOT need to be low to reject noise: Silero barely scores a
// keyboard or a door at all.
int micSensitivity();
void setMicSensitivity(int percent);

// The same setting, as the speech probability Silero has to reach before
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

// THE TRANSCRIPTION KEY: while it is held, what is said is typed where the
// caret is instead of being answered. Kept as the words Settings shows
// ("Ctrl+Alt+Space") and as what Windows is asked to register - its MOD_ flags
// in the high word, the virtual key in the low. A code of zero means no key.
QString transcriptionKey();
quint32 transcriptionKeyCode();
void setTranscriptionKey(const QString &shown, quint32 code);

// THE WAKE WORD, for wake word mode: what follows it in the same breath is a
// request. "Hey Burnos" until another is chosen; an empty one goes back to
// that.
QString wakeWord();
QString defaultWakeWord();
void setWakeWord(const QString &phrase);
// WAKE WORD MODE, off unless switched on: Burnos answers only what is said
// after the wake word, as a smart speaker does. Until it hears it, only the
// wake word and a spoken command are acted on. With it off the wake word plays no part at all
// and Burnos answers whatever it hears, as it always has.
bool wakeMode();
void setWakeMode(bool on);

// THE OWNER'S VOICE, as taught in Settings: the voiceprint - empty until then -
// and how loud their speech is in this microphone. With a voiceprint and the
// check on, a phrase that does not sound like it is neither transcribed nor
// answered nor remembered. Only these numbers are kept, never a recording.
QList<float> voicePrint();
double voiceLevel();
void setVoicePrint(const QList<float> &print, double level);
bool voiceCheckOn();
void setVoiceCheckOn(bool on);
// How alike a phrase has to be to the voiceprint, 0 to 100, and the same as
// the number the check compares with: 0.30 at the most forgiving, 0.50 in the
// middle, 0.70 at the strictest. One person scores about 0.6 and up against themselves, two
// different people about 0.3 and down.
int voiceStrictness();
void setVoiceStrictness(int percent);
double voiceThreshold();

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
