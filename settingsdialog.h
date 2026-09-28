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

// The one place every stored preference can be changed: the OpenRouter key and
// model, how long an answer may be, how long a pause ends a sentence, which
// Kokoro voice says it and how loudly, and whether the orb stays above every
// other window.
//
// Nothing is applied by signal: every setting is read back at the moment it is
// used, so saving here is enough and no engine has to be restarted.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);

private:
    void load();
    void save();

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
};

#endif // SETTINGSDIALOG_H
