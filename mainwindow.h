#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "keywordspotter.h"

#include <QElapsedTimer>
#include <QList>
#include <QMainWindow>
#include <QString>
#include <QStringList>

class ComposioClient;
class ModelInstaller;
class OpenRouterClient;
class OrbWidget;
class QTimer;
class SttEngine;
class TtsEngine;

namespace BurnOS {
class Agent;
}

// The orb's window, and the wiring between the ears, the brain, the voice and
// the orb. Since v0.4 nothing here operates the computer or asks a model
// anything - the Agent does both - and what this window decides is WHEN
// something gets said.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    // Kept only so every change of window position goes through one place.
    void moveEvent(QMoveEvent *event) override;
    // Answers the Windows hit test, so only the orb is solid and the space
    // around it lets clicks through to the desktop behind.
    bool nativeEvent(const QByteArray &eventType, void *message,
                     qintptr *result) override;

private:
    // Opened from the orb's context menu, or by saying "open app settings".
    // Modal, so only one can ever be up, and listening pauses while it is.
    void openSettings();
    void applyAlwaysOnTop();
    // Arm live mode without a click. Used at startup, and again once the first
    // run download finishes, so the orb is listening the moment it is usable.
    void armLiveMode();
    // px is the size the settings slider is showing right now, a moment ahead
    // of what has been written down. Anything else passes nothing and gets the
    // stored size.
    void applyOrbSize(int px = -1);
    // Put the orb back where it was last left, clamped onto a screen that
    // still exists. Called once, at startup, after the size is known.
    void restoreOrbPosition();
    // ONCE OPENROUTER HAS SAID WHAT EACH MODEL CAN DO: a model that cannot see
    // the screen or call tools cannot do this job at all, so Burnos moves itself
    // to one that can instead of failing every turn.
    void checkModelIsUpToTheJob();
    // Put a failure where the answer goes - on the orb - and in the log.
    void showNotice(const QString &reason);

    // THE VOICE HAS ONE QUEUE. The talker's answers and the tasks' news both
    // come here and are said one at a time: never over the user while they are
    // talking, an answer before any news, and news only once no answer is
    // being worked out.
    void queueSpeech(const QString &text, int turn);
    // The user carried on talking before this turn's answer was said.
    void retractSpeech(int turn);
    void speakNext();
    // Nothing is being said any more, however it ended: the voice state is
    // cleared and the orb stops looking like it is talking.
    void voiceGone();
    // "Thinking" on the orb: a phrase is being transcribed, or the talker is
    // working out what to say.
    void refreshThinking();
    // The voice has finished: reopen the microphone, if the orb is still armed.
    void listenAgain();
    // Opens the microphone for live mode. If it will not open, the orb goes
    // back to grey and remembers it is grey for want of a microphone.
    bool openMicrophone();
    // THE ORB IS ARMED, and listens the way Settings says: to everything, or -
    // in wake word mode - to nothing but its wake word.
    bool listen();

    // WAKE WORD MODE, as a smart speaker works. WAITING, the keyword spotter is
    // all that hears the microphone and nothing said is answered. The wake word
    // wakes Burnos: it is then listening exactly as it does with the mode off,
    // answers what was asked, and STAYS AWAKE for a few seconds after it has
    // finished - long enough to be answered back, or asked the next thing,
    // without the wake word - and then waits again. It also wakes by itself to
    // say what a task has to report. Mute, unmute and the other spoken
    // commands work in either state and need no wake word.
    bool waitForWakeWord();
    void wakeUp(bool afterWakeWord);
    // Something was just asked, answered or said: the few seconds start again.
    void stayAwake();
    // Those seconds are up: back to waiting, unless something is still going on.
    void sleepIfIdle();
    // True only for a mute by the user - the one grey orb that holds the voice
    // back. Grey for want of a microphone still speaks, so answers are kept.
    bool voiceHeld() const;
    // ONE CLICK MUTES, AND ONLY MUTES: the voice goes quiet and nothing said
    // is listened to any more - the microphone's sound goes to the keyword
    // spotter alone, for "unmute" and the other spoken commands and NOT for
    // the wake word - while the talker and every task carry on. What they
    // have to say is kept, and said after the unmute.
    void mute();
    void unmuted();
    // THE KEYWORD SPOTTER HEARD SOMETHING, muted or waiting: "unmute" does
    // what the click that unmutes does, "mute" what the click that mutes does,
    // the wake word wakes a waiting Burnos, and the other spoken commands do
    // what they do when Burnos is listening.
    void keywordHeard(BurnOS::Wake::Command command);
    // Unmutes as a click would.
    void unmuteByVoice();
    // A DOUBLE CLICK STOPS EVERYTHING - microphone, talker, every task, the
    // voice and whatever was waiting to be said. It is not a mute: the orb goes
    // red for two seconds and is then listening, or muted, as it was before.
    void forceStop();

    // WHAT WAS SAID, from the microphone or the rehearsal: a spoken command
    // for this window, words to be typed in transcription mode, or a line for
    // the Agent.
    void heard(const QString &text);
    // TRANSCRIPTION MODE: what is said is typed where the caret is, as a
    // dictation tool would, and is neither answered nor remembered. Turned on
    // and off by saying so. The voice keeps what it has to say until it is off.
    void setDictating(bool on);
    // Puts `text` into whatever window has the keyboard.
    void typeOut(const QString &text);
    // THE TRANSCRIPTION KEY, from Settings: while it is held, what is said is
    // typed - in either mode, muted or not - and the mode itself does not
    // change. Registered with Windows as a hot key, so it works from any window.
    void applyTranscriptionKey();
    void keyDown();
    void keyUp();
    // Transcription is starting: the voice stops at once, and what it was
    // saying is said again, from the start, when transcription is over.
    void holdVoice();
    // Reads the last transcription aloud: the last hold of the key, or all
    // that was typed since transcription was last started by voice.
    void readBack();

    // The BURNOS_ASK_SELFTEST rehearsal: the next scripted line, and the check
    // that lets an untimed one go once everything said so far has been
    // answered out loud.
    void rehearse();
    void rehearseWhenIdle();

    OrbWidget *m_orb = nullptr;
    // Collapses a whole drag into one write of the orb's position.
    QTimer *m_savePos = nullptr;
    SttEngine *m_stt = nullptr;
    TtsEngine *m_tts = nullptr;
    OpenRouterClient *m_ai = nullptr;
    // Everything beyond this machine: Gmail, Slack, Notion and the rest.
    ComposioClient *m_composio = nullptr;
    BurnOS::Agent *m_agent = nullptr;
    ModelInstaller *m_setup = nullptr;
    bool m_settingsOpen = false;
    bool m_transcribing = false;
    // A phrase was still inside the recogniser when everything was stopped:
    // what it turns out to say is dropped, not answered.
    bool m_dropHeard = false;
    // Running since the wake word woke Burnos: the phrase that follows may
    // begin with the wake word itself, or with the end of what came before it.
    QElapsedTimer m_wokeAt;
    bool m_waiting = false;    // wake word mode: waiting for the wake word
    QTimer *m_sleep = nullptr; // awake: counts the seconds until it waits again
    // Those seconds ran out while somebody was mid sentence: it waits again as
    // soon as that turns out to be nobody it answers.
    bool m_sleepDue = false;
    bool m_dictating = false; // transcription mode is on
    bool m_keyHeld = false;   // the transcription key is down
    // That key's virtual key while it is registered with Windows, else 0, and
    // the timer that sees it come up again: a hot key only ever says "down".
    int m_keyVk = 0;
    int m_keyMods = 0;       // the MOD_ flags that go with it
    QString m_lastDictation; // what readBack() reads
    QTimer *m_keyWatch = nullptr;

    struct Speech
    {
        QString text;
        int turn = 0; // the talker turn it answers, 0 for news from a task
    };
    QList<Speech> m_speech;      // waiting to be said, in order
    Speech m_voice;              // being synthesised or said right now
    bool m_voiceBusy = false;    // m_voice is in the voice engine
    bool m_voicePlaying = false; // and it is audible
    // Grey because no microphone would open (none plugged in, or a remote
    // session without one) - not because the user muted.
    bool m_micMissing = false;
    // Looks at the queue again while something is in the way.
    QTimer *m_speakRetry = nullptr;

    // Lines still to be said by the rehearsal, in order.
    QStringList m_rehearsal;
    bool m_rehearsalWaiting = false;
};

#endif // MAINWINDOW_H
