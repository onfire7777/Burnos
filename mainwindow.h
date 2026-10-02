#ifndef MAINWINDOW_H
#define MAINWINDOW_H

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
    // ONE CLICK MUTES, AND ONLY MUTES: the microphone shuts and the voice goes
    // quiet, while the talker and every task carry on. What they have to say
    // is kept, and said after the click that unmutes.
    void mute();
    void unmuted();
    // A DOUBLE CLICK STOPS EVERYTHING - microphone, talker, every task, the
    // voice and whatever was waiting to be said. It is not a mute: the orb goes
    // red for two seconds and is then listening, or muted, as it was before.
    void forceStop();

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

    struct Speech
    {
        QString text;
        int turn = 0; // the talker turn it answers, 0 for news from a task
    };
    QList<Speech> m_speech;      // waiting to be said, in order
    Speech m_voice;              // being synthesised or said right now
    bool m_voiceBusy = false;    // m_voice is in the voice engine
    bool m_voicePlaying = false; // and it is audible
    // Looks at the queue again while something is in the way.
    QTimer *m_speakRetry = nullptr;

    // Lines still to be said by the rehearsal, in order.
    QStringList m_rehearsal;
    bool m_rehearsalWaiting = false;
};

#endif // MAINWINDOW_H
