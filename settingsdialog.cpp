#include "integrationsdialog.h"
#include "settings.h"
#include "settingsdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// kokoro-en-v0_19 ships eleven speakers, numbered 0 to 10.
constexpr int kVoiceCount = 11;

// How long after the last change the numbers are written down. Long enough
// that a whole slider drag is one registry write instead of a hundred, short
// enough that nobody can close the dialog in the gap - and closing it writes
// them anyway.
constexpr int kSaveDelayMs = 400;

// The models worth offering, in the order they are shown. Each earned its
// place by actually being used for a while, which is why the list is five long
// and not fifty: a combo of every model on OpenRouter is a worse choice than no
// choice at all.
//
// The combo is EDITABLE, so this is a shortlist rather than a limit - any
// OpenRouter slug can be typed in. The default is whichever settings.cpp says,
// not whichever is first here.
struct ModelChoice
{
    const char *slug;
    const char *note;
};

constexpr ModelChoice kModels[] = {
    {"xiaomi/mimo-v2.6-flash",
     "Fastest to start talking. The default, and the right answer for ordinary "
     "conversation."},
    {"minimax/minimax-m2.7",
     "Reasons the hardest. Best when it has to work out which control to click "
     "or which tool to run."},
    {"openai/gpt-6-luna",
     "Phrases an answer the most naturally out loud."},
    {"deepseek/deepseek-v4.1-flash",
     "Quick and cheap. Fine for questions, weaker at driving the machine."},
    {"typesafe/jev-router",
     "Picks a model per question by itself. Clever, but it thinks before it "
     "chooses, so the orb stays quiet for longer."},
};

// Same palette as the orb's context menu, or the dialog would look like it
// belongs to another program.
constexpr auto kStyle =
    "QDialog { background-color: #12161f; }"
    "QLabel { color: #d8e6f5; }"
    "QCheckBox { color: #d8e6f5; }"
    "QLineEdit, QComboBox, QSpinBox { background-color: #0c1018; color: #d8e6f5;"
    " border: 1px solid #2c3648; border-radius: 3px; padding: 4px; }"
    "QComboBox QAbstractItemView { background-color: #12161f; color: #d8e6f5;"
    " selection-background-color: #2a3c5a; }"
    "QSlider::groove:horizontal { height: 4px; background: #2c3648;"
    " border-radius: 2px; }"
    "QSlider::handle:horizontal { width: 12px; margin: -5px 0;"
    " border-radius: 6px; background: #5b8dd9; }"
    "QPushButton { background-color: #1b2331; color: #d8e6f5;"
    " border: 1px solid #2c3648; border-radius: 3px; padding: 5px 18px; }"
    "QPushButton:hover { background-color: #2a3c5a; }";

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Burnos Settings"));
    setModal(true);
    setMinimumWidth(470);
    setStyleSheet(QString::fromLatin1(kStyle));

    // The orb is a stay on top window; a dialog without the same hint would
    // open behind it and look like nothing happened.
    setWindowFlag(Qt::WindowStaysOnTopHint, true);

    m_key = new QLineEdit(this);
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText(QStringLiteral("sk-or-v1-..."));
    m_key->setToolTip(tr("Saved when you press Enter or click away, not on "
                         "every keystroke - half a key is no use to anyone. "
                         "Stored encrypted for this Windows user only."));

    m_showKey = new QCheckBox(tr("Show"), this);
    connect(m_showKey, &QCheckBox::toggled, this, [this](bool on) {
        m_key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });

    auto *keyRow = new QHBoxLayout;
    keyRow->setContentsMargins(0, 0, 0, 0);
    keyRow->addWidget(m_key, 1);
    keyRow->addWidget(m_showKey);

    // Editable, so the shortlist is a convenience and not a cage: any
    // OpenRouter slug can be typed straight in. Each preset carries its own
    // tooltip saying what it is good at, because the slug alone tells a user
    // nothing about which one to pick.
    m_model = new QComboBox(this);
    m_model->setEditable(true);
    m_model->setInsertPolicy(QComboBox::NoInsert);
    for (const ModelChoice &choice : kModels) {
        m_model->addItem(QString::fromLatin1(choice.slug));
        m_model->setItemData(m_model->count() - 1,
                             tr(choice.note), Qt::ToolTipRole);
    }
    m_model->setToolTip(tr("Which model answers. Every one of these works; they "
                           "trade speed against how well they reason. Hover a "
                           "choice to see what it is good at, or type any other "
                           "OpenRouter model id. Picking one takes effect at "
                           "once; a typed one when you press Enter or click "
                           "away."));

    m_tokens = new QSpinBox(this);
    m_tokens->setRange(256, 4000);
    m_tokens->setSingleStep(100);
    m_tokens->setSuffix(tr(" tokens"));
    // Without this the box reports every digit as it is typed, so 1200 on the
    // way to 2000 is briefly 2, then 20, then 200 - and each one would be
    // saved and clamped to the minimum.
    m_tokens->setKeyboardTracking(false);
    m_tokens->setToolTip(tr("A reasoning model spends part of this budget "
                            "thinking, so a small value can come back empty."));

    // A dialog of its own, opened from here. It holds a second credential and a
    // searchable catalogue of about fifteen hundred apps, neither of which fits
    // in a form of eight rows. Like this page, everything in it takes effect as
    // you go.
    m_integrations = new QPushButton(tr("Manage integrations..."), this);
    m_integrations->setToolTip(tr("Connect Gmail, Slack, Notion, GitHub and "
                                  "about fifteen hundred other apps, so you can "
                                  "ask for things in them out loud."));

    connect(m_integrations, &QPushButton::clicked, this, [this]() {
        IntegrationsDialog dialog(this);
        dialog.exec();
    });

    // The button keeps its own width rather than stretching across the row, so
    // it reads as a way out of this dialog and not as another field.
    auto *integrationsRow = new QHBoxLayout;
    integrationsRow->setContentsMargins(0, 0, 0, 0);
    integrationsRow->addWidget(m_integrations);
    integrationsRow->addStretch(1);

    m_sensitivity = new QSlider(Qt::Horizontal, this);
    m_sensitivity->setRange(0, 100);
    m_sensitivity->setSingleStep(5);
    m_sensitivity->setPageStep(10);
    m_sensitivity->setToolTip(tr("How readily it starts listening. Raise it if "
                                 "you have to speak up to be heard; lower it "
                                 "only if a noisy room keeps setting it off."));

    m_sensitivityValue = new QLabel(this);
    m_sensitivityValue->setMinimumWidth(44);
    connect(m_sensitivity, &QSlider::valueChanged, this, [this](int value) {
        m_sensitivityValue->setText(tr("%1 %").arg(value));
    });

    auto *sensitivityRow = new QHBoxLayout;
    sensitivityRow->setContentsMargins(0, 0, 0, 0);
    sensitivityRow->addWidget(m_sensitivity, 1);
    sensitivityRow->addWidget(m_sensitivityValue);

    // In fiftieths of a second, because nobody thinks in single milliseconds
    // and the difference between 1000 and 1010 ms is not audible anyway.
    m_hangover = new QSlider(Qt::Horizontal, this);
    m_hangover->setRange(300, 3000);
    m_hangover->setSingleStep(50);
    m_hangover->setPageStep(100);
    m_hangover->setToolTip(tr("How long you have to stop talking before it "
                              "decides you have finished. Raise it if it keeps "
                              "answering while you are still thinking."));

    m_hangoverValue = new QLabel(this);
    m_hangoverValue->setMinimumWidth(44);
    connect(m_hangover, &QSlider::valueChanged, this, [this](int ms) {
        m_hangoverValue->setText(tr("%1 s").arg(ms / 1000.0, 0, 'f', 2));
    });

    auto *hangoverRow = new QHBoxLayout;
    hangoverRow->setContentsMargins(0, 0, 0, 0);
    hangoverRow->addWidget(m_hangover, 1);
    hangoverRow->addWidget(m_hangoverValue);

    m_voice = new QComboBox(this);
    for (int i = 0; i < kVoiceCount; ++i) {
        m_voice->addItem(i == 0 ? tr("Voice 1 (default)")
                                : tr("Voice %1").arg(i + 1));
    }
    m_voice->setToolTip(tr("Which of the eleven Kokoro speakers answers. The "
                           "next thing said uses it."));

    m_volume = new QSlider(Qt::Horizontal, this);
    m_volume->setRange(0, 100);
    m_volume->setSingleStep(5);
    m_volume->setPageStep(10);

    m_volumeValue = new QLabel(this);
    m_volumeValue->setMinimumWidth(44);
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        m_volumeValue->setText(tr("%1 %").arg(value));
    });

    auto *volumeRow = new QHBoxLayout;
    volumeRow->setContentsMargins(0, 0, 0, 0);
    volumeRow->addWidget(m_volume, 1);
    volumeRow->addWidget(m_volumeValue);

    // How big the orb is on the desktop. The window is exactly this, so it is
    // also how big a target there is to click and drag.
    m_orbSize = new QSlider(Qt::Horizontal, this);
    m_orbSize->setRange(140, 320);
    m_orbSize->setSingleStep(10);
    m_orbSize->setPageStep(20);
    m_orbSize->setToolTip(tr("How big the orb sits on the desktop. A bigger orb "
                             "is easier to hit and shows the words it is "
                             "saying in larger type. It resizes as you drag."));

    m_orbSizeValue = new QLabel(this);
    m_orbSizeValue->setMinimumWidth(44);
    connect(m_orbSize, &QSlider::valueChanged, this, [this](int value) {
        m_orbSizeValue->setText(tr("%1 px").arg(value));
    });

    auto *orbSizeRow = new QHBoxLayout;
    orbSizeRow->setContentsMargins(0, 0, 0, 0);
    orbSizeRow->addWidget(m_orbSize, 1);
    orbSizeRow->addWidget(m_orbSizeValue);

    m_onTop = new QCheckBox(tr("Keep the orb above other windows"), this);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);
    form->addRow(tr("OpenRouter key"), keyRow);
    form->addRow(tr("Model"), m_model);
    form->addRow(tr("Answer length"), m_tokens);
    form->addRow(tr("Integrations"), integrationsRow);
    form->addRow(tr("Microphone sensitivity"), sensitivityRow);
    form->addRow(tr("Pause before it answers"), hangoverRow);
    form->addRow(tr("Voice"), m_voice);
    form->addRow(tr("Speech volume"), volumeRow);
    form->addRow(tr("Orb size"), orbSizeRow);
    form->addRow(QString(), m_onTop);

    // A note where the Save button used to be, because a form with no Save
    // button looks like a form you have not finished with.
    auto *note = new QLabel(tr("Every change here takes effect straight away "
                               "and is remembered."), this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: #8fa3bd;"));

    // Close, not Save and Cancel: there is nothing left to save by the time you
    // reach it, and nothing to cancel either - a Cancel that silently undid the
    // size you just watched the orb take would be a lie.
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 0, 0);
    footer->addWidget(note, 1);
    footer->addWidget(buttons);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 14);
    root->setSpacing(14);
    root->addLayout(form);
    root->addStretch(1);
    root->addLayout(footer);

    m_save = new QTimer(this);
    m_save->setSingleShot(true);
    m_save->setInterval(kSaveDelayMs);
    connect(m_save, &QTimer::timeout, this, &SettingsDialog::saveValues);

    // LAST, and in this order. Filling the form in moves every widget, which is
    // indistinguishable from a person moving it, so nothing is listening yet.
    load();
    wireLive();

    // Whichever way the dialog goes away - Close, Escape, or the cross in the
    // corner - the last few hundred milliseconds of changes and both typed
    // fields are committed. finished() is the one signal all three end at.
    connect(this, &QDialog::finished, this, [this](int) {
        m_save->stop();
        saveValues();
        saveCredentials();
    });
}

void SettingsDialog::load()
{
    // Decrypted only to be shown, and only behind a password field: the key is
    // never kept in a member anywhere else.
    m_key->setText(BurnOS::Settings::openRouterKey());

    // setCurrentText on an editable combo selects a matching preset where there
    // is one and simply types the text in where there is not, which is exactly
    // right for a model somebody pasted in by hand.
    m_model->setCurrentText(BurnOS::Settings::openRouterModel());

    m_tokens->setValue(BurnOS::Settings::replyTokens());

    const int sensitivity = BurnOS::Settings::micSensitivity();
    m_sensitivity->setValue(sensitivity);
    m_sensitivityValue->setText(tr("%1 %").arg(sensitivity));

    const int hangover = BurnOS::Settings::hangoverMs();
    m_hangover->setValue(hangover);
    m_hangoverValue->setText(tr("%1 s").arg(hangover / 1000.0, 0, 'f', 2));

    m_voice->setCurrentIndex(BurnOS::Settings::ttsVoice());

    const int volume = BurnOS::Settings::ttsVolume();
    m_volume->setValue(volume);
    m_volumeValue->setText(tr("%1 %").arg(volume));

    const int orbSize = BurnOS::Settings::orbSize();
    m_orbSize->setValue(orbSize);
    m_orbSizeValue->setText(tr("%1 px").arg(orbSize));

    m_onTop->setChecked(BurnOS::Settings::alwaysOnTop());
}

void SettingsDialog::wireLive()
{
    // The sliders and the spin box. Each is read back at the moment it is used
    // by whichever engine wants it, so writing one down is the whole of
    // applying it - there is nothing to notify.
    connect(m_sensitivity, &QSlider::valueChanged, this, &SettingsDialog::saveSoon);
    connect(m_hangover, &QSlider::valueChanged, this, &SettingsDialog::saveSoon);
    connect(m_volume, &QSlider::valueChanged, this, &SettingsDialog::saveSoon);
    connect(m_tokens, &QSpinBox::valueChanged, this, &SettingsDialog::saveSoon);
    connect(m_voice, &QComboBox::currentIndexChanged, this, &SettingsDialog::saveSoon);

    // The orb resizes NOW, under the slider, and the value is written down a
    // moment later. The size travels with the signal because the registry has
    // not caught up yet and must not be asked to on every frame.
    connect(m_orbSize, &QSlider::valueChanged, this, [this](int px) {
        emit orbSizeChanged(px);
        saveSoon();
    });

    // Written down BEFORE it is emitted: putting a window flag back reads the
    // stored value, so the store has to be right first. One tick is one write,
    // so there is nothing here to collapse.
    connect(m_onTop, &QCheckBox::toggled, this, [this](bool on) {
        BurnOS::Settings::setAlwaysOnTop(on);
        emit alwaysOnTopChanged();
    });

    // The two typed fields. A key or a model id is only worth storing once it
    // is whole, so these wait for Enter, for the focus to leave, or for the
    // dialog to close. Picking a model off the list is already whole.
    connect(m_key, &QLineEdit::editingFinished, this,
            &SettingsDialog::saveCredentials);
    if (QLineEdit *edit = m_model->lineEdit()) {
        connect(edit, &QLineEdit::editingFinished, this,
                &SettingsDialog::saveCredentials);
    }
    connect(m_model, &QComboBox::activated, this,
            [this](int) { saveCredentials(); });
}

void SettingsDialog::saveSoon()
{
    m_save->start();
}

void SettingsDialog::saveValues()
{
    BurnOS::Settings::setReplyTokens(m_tokens->value());
    BurnOS::Settings::setMicSensitivity(m_sensitivity->value());
    BurnOS::Settings::setHangoverMs(m_hangover->value());
    BurnOS::Settings::setTtsVoice(m_voice->currentIndex());
    BurnOS::Settings::setTtsVolume(m_volume->value());
    BurnOS::Settings::setOrbSize(m_orbSize->value());
}

void SettingsDialog::saveCredentials()
{
    // An emptied field removes the key rather than storing an empty one, which
    // is exactly what setOpenRouterKey already does.
    BurnOS::Settings::setOpenRouterKey(m_key->text());
    BurnOS::Settings::setOpenRouterModel(m_model->currentText());
}
