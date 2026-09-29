#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

// The one place every stored preference can be changed: the OpenRouter key and
// model, how long an answer may be, how long a pause ends a sentence, which
// Kokoro voice says it and how loudly, and whether the orb stays above every
// other window.
//
// THERE IS NO SAVE BUTTON. Every control takes effect as you move it and is
// written down by itself, because a settings page with a Save button is one
// where half the work is remembering to press it - and where a slider you
// dragged to hear the difference did nothing until you did. Only the two typed
// fields wait: a key or a model id stored on every keystroke would be stored
// half finished, so those commit when you finish typing, leave the field, or
// close the dialog.
//
// Most settings need no applying at all: every engine reads its own back at the
// moment it uses it, so writing one down IS applying it. The two that live on
// the window rather than in an engine - how big the orb is and whether it stays
// on top - are pushed out on the signals below as they change, so the orb
// changes under the slider rather than after the dialog closes.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);

signals:
    // As the slider moves. The size travels WITH the signal rather than being
    // read back from settings, because the write is deliberately a moment
    // behind: a drag must not write to the registry on every frame.
    void orbSizeChanged(int px);
    // As the box is ticked. This one is written down before it is emitted, so
    // the window can simply read it back like everything else.
    void alwaysOnTopChanged();

private:
    void load();
    // Connected AFTER load(), or filling the form in would look exactly like a
    // person changing it and write every value straight back.
    void wireLive();

    // The numbers and the choices: everything that cannot be typed wrong half
    // way through. Written a short moment after the last change, which collapses
    // a whole slider drag into one registry write.
    void saveValues();
    void saveSoon();
    // The two typed fields, committed only when they are finished with.
    void saveCredentials();

    QLineEdit *m_key = nullptr;
    QCheckBox *m_showKey = nullptr;
    QComboBox *m_model = nullptr;
    QSpinBox *m_tokens = nullptr;
    // Integrations are a page of their own, not a row here: the catalogue runs
    // to fifteen hundred apps and needs a search box. This is the way in.
    QPushButton *m_integrations = nullptr;
    QSlider *m_sensitivity = nullptr;
    QLabel *m_sensitivityValue = nullptr;
    QSlider *m_hangover = nullptr;
    QLabel *m_hangoverValue = nullptr;
    QComboBox *m_voice = nullptr;
    QSlider *m_volume = nullptr;
    QLabel *m_volumeValue = nullptr;
    QSlider *m_orbSize = nullptr;
    QLabel *m_orbSizeValue = nullptr;
    QCheckBox *m_onTop = nullptr;

    QTimer *m_save = nullptr;
};

#endif // SETTINGSDIALOG_H
