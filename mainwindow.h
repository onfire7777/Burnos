#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "computeruse.h"

#include <QMainWindow>
#include <QRect>
#include <QStringList>
#include <QVariantMap>

class ComposioClient;
class ModelInstaller;
class OpenRouterClient;
class QTimer;
class OrbWidget;
class SttEngine;
class TtsEngine;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    // Kept only so every change of window position goes through one place. The
    // answer travels with the orb by itself now, because it is inside it.
    void moveEvent(QMoveEvent *event) override;
    // Answers the Windows hit test, so only the orb is solid and the empty
    // space around the caption lets clicks through to the desktop behind.
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
    // Where the orb is on screen, which is this window's frame exactly.
    // Read the stored orb size and size the orb AND the window from it. Called
    // once at startup and again every time the settings dialog closes.
    // px is the size the settings slider is showing right now, a moment ahead
    // of what has been written down. Anything else passes nothing and gets the
    // stored size.
    void applyOrbSize(int px = -1);
    // Put the orb back where it was last left, clamped onto a screen that
    // still exists. Called once, at startup, after the size is known.
    void restoreOrbPosition();
    // Put a failure where the answer goes - inside the orb - and in the log.
    void showNotice(const QString &reason);
    // Feeds the next scripted question in, as if it had just been transcribed.
    // Only ever used by the BURNOS_ASK_SELFTEST rehearsal.
    void askNextRehearsal();

    // Everything said goes straight to the reasoning model, with a description
    // of what is on screen alongside it. The model either answers or asks for
    // the computer_use tool; there is no classifier in front of it.
    void routeSpoken(const QString &said);
    // One tool call from the model. `function` says which tool it asked for:
    // computer_use, which happens here and now, or app_action, which is a
    // network call and finishes later on a ComposioClient signal.
    void onToolRequested(const QString &function, const QVariantMap &args);
    // Search for, run, or sign in to one of the user's online apps. Returns at
    // once - the turn carries on from the handlers wired up in the constructor,
    // because Composio answers over the network.
    void runAppAction(const QVariantMap &args);
    // Re-read one window's controls, make it the window actions apply to, and
    // describe the list in the words the model is given. This is the ONLY place
    // that list is worded, so what it is told at the start of a turn and what it
    // is told after the screen changes cannot drift apart.
    QString describeControls(const DesktopWindow &target);
    // Carries the action out and returns, in plain words, what actually
    // happened - which is handed straight back to the model to say out loud.
    // png comes back filled when the action produced a picture for the model.
    QString runComputerUse(const QVariantMap &args, QByteArray *png);
    // End a turn: the orb stops claiming to be thinking and the microphone is
    // reopened. Every path out of a turn, success or failure, goes through here.
    void listenAgain();
    // Muting. STOPS EVERYTHING, not just the microphone - see the definition,
    // there are six separate things running by the middle of a turn and every
    // one of them has to be told.
    void forceStop();

private:
    OrbWidget *m_orb = nullptr;

    // What was on screen when the decision was asked for. Jev answers with a
    // key out of this list, so it has to survive until the answer comes back.
    DesktopWindow m_target;
    QList<DesktopControl> m_offered;

    // The window the last screenshot was of, and how much it was shrunk to fit.
    // The model answers in the PICTURE's pixels, so this is what turns a point
    // it names back into a point on the real window.
    quint64 m_shotWindow = 0;
    qreal m_shotScale = 1.0;

    // Tool calls used by the turn in flight. Capped, because a model that
    // cannot get a command to work will otherwise keep trying for ever - one
    // question burned fifteen calls before this existed.
    int m_toolCalls = 0;
    // Set by forceStop(). A tool action can be half way through a wait when the
    // orb is muted, and its result must be thrown away rather than handed back
    // to the model, which would start the turn up again under a grey orb.
    bool m_abort = false;
    // Collapses a whole drag into one write of the orb's position.
    QTimer *m_savePos = nullptr;
    SttEngine *m_stt = nullptr;
    OpenRouterClient *m_ai = nullptr;
    TtsEngine *m_tts = nullptr;
    // Everything beyond this machine: Gmail, Slack, Notion and the rest,
    // whichever of them the user connected in Manage integrations.
    ComposioClient *m_composio = nullptr;
    ModelInstaller *m_setup = nullptr;
    bool m_settingsOpen = false;
    // Questions still to be asked by the rehearsal, in order.
    QStringList m_rehearsal;
    // What has been asked but not yet answered OUT LOUD. If the user carries
    // on talking before the voice starts, the new words are added to this and
    // the whole thought is asked again, so a pause for breath cannot split one
    // question into two unrelated ones.
    QString m_unanswered;
};

#endif // MAINWINDOW_H
