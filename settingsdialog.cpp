#include "composio.h"
#include "keywordspotter.h"
#include "livelog.h"
#include "openrouter.h"
#include "settings.h"
#include "settingsdialog.h"
#include "sttengine.h"
#include "voicecheck.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QElapsedTimer>
#include <QMediaDevices>
#include <QProgressBar>

#include <cmath>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QKeyEvent>
#include <QKeySequence>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
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
// place by actually being used or timed, which is why the list is seven long
// and not fifty: a combo of every model on OpenRouter is a worse choice than no
// choice at all. BOTH model boxes offer this same list, because a model's
// strengths decide which job it suits, not which box it sits in.
//
// Timed on 2026-09-29 with _research/model-bench.ps1, reasoning medium, three
// runs each. To answer or hand off: Gemini 3.5 Flash Lite 0.6-0.9 s, GPT-6
// Luna about 1.1 s, MiMo 2.6 Flash 1.3-3 s with outliers past 9 s. One working
// step with a screenshot: Grok 4.7 1.4 s, Claude Sonnet 5.5 2.8 s, Claude Opus
// 5.5 3.7 s. Every one of them handed off and acted correctly.
//
// EVERY ONE OF THESE IS CHECKED against OpenRouter's own catalogue as the
// dialog opens, and any that turns out not to see the screen or not to call
// tools is taken straight back off the list. This list used to hold minimax
// m2.7, recommended here for driving the machine, and it takes TEXT ONLY: every
// turn that took a screenshot to check its own work came back as a 404 and the
// word Error on the orb. jev-router was on it too, and advertises no tool
// support at all. A shortlist written by hand cannot know these things, so it
// is no longer trusted to.
//
// The combo is EDITABLE, so this is a shortlist rather than a cage - any
// OpenRouter slug can be typed in, PROVIDED IT CAN DO THE JOB. The default is
// whichever settings.cpp says, not whichever is first here.
struct ModelChoice
{
    const char *slug;
    const char *note;
};

constexpr ModelChoice kModels[] = {
    {"xiaomi/mimo-v2.6-flash",
     "The default. Quick and cheap, and able to do the work as well as the "
     "talking."},
    {"google/gemini-3.5-flash-lite",
     "The fastest to answer in testing, well under a second. A talking model "
     "first; it can see, so it can do light work too."},
    {"openai/gpt-6-luna",
     "Phrases an answer the most naturally out loud, in about a second."},
    {"anthropic/claude-sonnet-5.5",
     "Reasons hard and drives the machine well. The natural working model."},
    {"anthropic/claude-opus-5.5",
     "Reasons the hardest of all. The working model for big jobs - slower and "
     "dearer, which working in the background hides."},
    {"x-ai/grok-4.7",
     "Reads a screenshot quickly and acts on what it saw. A fast working "
     "model."},
    {"deepseek/deepseek-v4.1-flash",
     "Quick and cheap. Fine for questions, weaker at driving the machine."},
};

// The first entry on the working model's list. Chosen, nothing is stored and
// every task runs on the talking model - exactly how Burnos ran before v0.5.
constexpr auto kSameAsTalking = "Same as the talking model";

// The three colours a line under a model box can be: all is well, the
// catalogue has never heard of it, and it cannot do the job.
constexpr auto kNotePlain = "#8fa3bd";
constexpr auto kNoteWarn = "#d8b46a";
constexpr auto kNoteRefuse = "#e58a8a";

// Same palette as the orb's context menu, for both dialogs, or they would look
// like they belong to another program.
constexpr auto kStyle =
    "QDialog { background-color: #12161f; }"
    "QLabel { color: #d8e6f5; }"
    "QCheckBox { color: #d8e6f5; }"
    "QLineEdit, QComboBox, QSpinBox { background-color: #0c1018; color: #d8e6f5;"
    " border: 1px solid #2c3648; border-radius: 3px; padding: 4px; }"
    "QComboBox QAbstractItemView { background-color: #12161f; color: #d8e6f5;"
    " selection-background-color: #2a3c5a; }"
    "QListWidget { background-color: #0c1018; color: #d8e6f5;"
    " border: 1px solid #2c3648; border-radius: 3px; padding: 2px; outline: none; }"
    "QListWidget::item { padding: 5px 6px; border-radius: 3px; }"
    "QListWidget::item:selected { background-color: #2a3c5a; }"
    "QSlider::groove:horizontal { height: 4px; background: #2c3648;"
    " border-radius: 2px; }"
    "QSlider::handle:horizontal { width: 12px; margin: -5px 0;"
    " border-radius: 6px; background: #5b8dd9; }"
    "QPushButton { background-color: #1b2331; color: #d8e6f5;"
    " border: 1px solid #2c3648; border-radius: 3px; padding: 5px 18px; }"
    "QPushButton:hover:enabled { background-color: #2a3c5a; }"
    "QPushButton:disabled { color: #566274; border-color: #1e2634; }";

} // namespace

// MANAGE INTEGRATIONS: the Composio key, a searchable catalogue of about
// fifteen hundred apps, and Connect or Disconnect one at a time. Opened only
// from Settings, so it lives here. The sign in happens in the user's own
// browser and finishes out of band, so after sending them there the dialog
// keeps asking Composio whether the connection has appeared yet.
class IntegrationsDialog : public QDialog
{
public:
    explicit IntegrationsDialog(QWidget *parent = nullptr);

private:
    // Saved the moment it stops being edited: nothing else here works until
    // it is stored.
    void keyEdited();
    void search();
    void repaintList();
    void connectSelected();
    void disconnectSelected();
    void selectionChanged();
    void say(const QString &message, bool isError = false);
    // What a person should see for an app known only by its slug, because the
    // connections endpoint hands back slugs and nothing else.
    QString displayName(const QString &slug) const;

    ComposioClient *m_composio = nullptr;
    QLineEdit *m_key = nullptr;
    QCheckBox *m_showKey = nullptr;
    QLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    QPushButton *m_connect = nullptr;
    QPushButton *m_disconnect = nullptr;
    QPushButton *m_refresh = nullptr;
    QLabel *m_status = nullptr;

    QTimer *m_typing = nullptr; // a typed word is one lookup, not one per key
    QTimer *m_poll = nullptr;   // asks whether a sign in finished; gives up
    int m_polls = 0;
    QString m_awaiting;         // slug we are waiting to see appear

    // The catalogue as last searched and every connection this install has,
    // from two endpoints; the list on screen is the two merged.
    QList<ComposioApp> m_catalogue;
    QHash<QString, ComposioApp> m_connected; // by slug
    QHash<QString, QString> m_names;         // slug to proper name
    // So "nothing matched" is only said about a search that came back empty.
    bool m_catalogueLoaded = false;
};

// THE TRANSCRIPTION KEY, set by pressing it: click the field, then the key or
// the combination. It is kept as Windows will be asked to register it, which
// is why this is not a QKeySequenceEdit - its keys are Qt's and would have to
// be translated back. Backspace or Delete takes the key away again. Saved the
// moment it is pressed, like everything else on this page.
class KeyField : public QLineEdit
{
public:
    explicit KeyField(QWidget *parent = nullptr)
        : QLineEdit(parent)
    {
        setReadOnly(true);
        setPlaceholderText(QObject::tr("None - click here, then press a key"));
        setText(BurnOS::Settings::transcriptionKey());
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        const int key = event->key();
        const Qt::KeyboardModifiers held =
            event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier
                                  | Qt::MetaModifier);
        // Escape still closes the page and Enter still does nothing: neither
        // can be the key. A modifier alone is on its way to a combination.
        if (key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter
            || key == Qt::Key_Tab || key == Qt::Key_Backtab) {
            QLineEdit::keyPressEvent(event);
            return;
        }
        if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt
            || key == Qt::Key_Meta || key == Qt::Key_AltGr || key == Qt::Key_unknown)
            return;
        if (held == Qt::NoModifier && (key == Qt::Key_Backspace || key == Qt::Key_Delete)) {
            clear();
            BurnOS::Settings::setTranscriptionKey(QString(), 0);
            return;
        }
        // Any one key will do, alone or with Ctrl, Shift, Alt or Win. Alone, it
        // is Burnos's while Burnos runs: it no longer types anywhere else.
        const quint32 vk = event->nativeVirtualKey() & 0xFFFF;
        if (!vk)
            return;
        // Windows' own flags: MOD_ALT 1, MOD_CONTROL 2, MOD_SHIFT 4, MOD_WIN 8.
        quint32 mods = 0;
        if (held & Qt::AltModifier)
            mods |= 1;
        if (held & Qt::ControlModifier)
            mods |= 2;
        if (held & Qt::ShiftModifier)
            mods |= 4;
        if (held & Qt::MetaModifier)
            mods |= 8;
        const QString shown =
            QKeySequence(QKeyCombination(held, Qt::Key(key))).toString(QKeySequence::NativeText);
        setText(shown);
        BurnOS::Settings::setTranscriptionKey(shown, (mods << 16) | vk);
    }
};

// TEACHING BURNOS A VOICE. The person reads a few lines for 25 seconds; the
// pauses are taken out, and what is left becomes the voiceprint every later
// phrase is held against, with how loud their speech is in this microphone.
// Only those are kept, never the recording. The two halves of the reading have
// to sound like ONE person, or a room that talked over it would be learned as
// the voice.
class VoiceLesson : public QDialog
{
public:
    explicit VoiceLesson(QWidget *parent = nullptr);

private:
    void begin();
    void finish();
    void fail(const QString &why);

    QLabel *m_status = nullptr;
    QProgressBar *m_bar = nullptr;
    QPushButton *m_start = nullptr;
    QAudioSource *m_source = nullptr;
    QTimer *m_clock = nullptr;
    QElapsedTimer m_since;
    QByteArray m_pcm;
    // THE CHECK, without a microphone: BURNOS_VOICE_LESSON_WAV names a 16 kHz
    // mono WAV that is taken for the reading, and the dialog starts and closes
    // by itself. Never in a normal run.
    const QString m_rehearsal = qEnvironmentVariable("BURNOS_VOICE_LESSON_WAV");
};

namespace {
constexpr int kLessonMs = 25000;
// At least this much of the reading has to be speech.
constexpr int kLessonSpeechMs = 8000;
// How alike its two halves have to be to count as one voice.
constexpr float kOneVoice = 0.5f;
} // namespace

VoiceLesson::VoiceLesson(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Teach Burnos my voice"));
    setModal(true);
    setMinimumWidth(470);
    setStyleSheet(QString::fromLatin1(kStyle));
    setWindowFlag(Qt::WindowStaysOnTopHint, true);

    auto *how = new QLabel(tr("Press Start and read the lines below aloud, in your normal "
                              "voice and at your normal distance from the microphone. It "
                              "takes 25 seconds. Keep the room quiet while you read: no "
                              "television, nobody else talking."), this);
    how->setWordWrap(true);
    how->setStyleSheet(QStringLiteral("color: #8fa3bd;"));

    auto *lines = new QLabel(
        tr("Hello Burnos, this is my voice. I am reading these lines so that you can "
           "learn how I sound. When I speak to you, I want you to listen to me, and "
           "not to the television or to anyone else in the room. I talk like this when "
           "I ask you a question, when I give you something to do, and when I am just "
           "thinking out loud. If I pause for a moment, wait for me, and then carry on "
           "with what I asked. Thank you for listening."), this);
    lines->setWordWrap(true);
    lines->setStyleSheet(QStringLiteral("font-size: 14px; padding: 8px;"));

    m_bar = new QProgressBar(this);
    m_bar->setRange(0, kLessonMs);
    m_bar->setTextVisible(false);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    m_start = new QPushButton(tr("Start"), this);
    m_start->setAutoDefault(false);
    auto *close = new QPushButton(tr("Close"), this);
    close->setAutoDefault(false);
    connect(m_start, &QPushButton::clicked, this, &VoiceLesson::begin);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_status, 1);
    buttons->addWidget(m_start);
    buttons->addWidget(close);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 14);
    root->setSpacing(12);
    root->addWidget(how);
    root->addWidget(lines);
    root->addWidget(m_bar);
    root->addLayout(buttons);

    m_clock = new QTimer(this);
    m_clock->setInterval(100);
    connect(m_clock, &QTimer::timeout, this, [this]() {
        m_bar->setValue(int(qMin<qint64>(m_since.elapsed(), kLessonMs)));
        if (m_since.elapsed() >= kLessonMs)
            finish();
    });

    if (!m_rehearsal.isEmpty())
        QTimer::singleShot(0, this, &VoiceLesson::begin);
}

void VoiceLesson::begin()
{
    m_start->setEnabled(false);
    m_pcm.clear();
    if (!m_rehearsal.isEmpty()) {
        QString error;
        m_pcm = SttEngine::pcmFromWav(m_rehearsal, &error);
        finish();
        return;
    }

    QAudioFormat format;
    format.setSampleRate(16000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull() || !device.isFormatSupported(format)) {
        fail(tr("No microphone is available."));
        return;
    }
    m_source = new QAudioSource(device, format, this);
    QIODevice *in = m_source->start();
    if (!in) {
        fail(tr("The microphone would not open."));
        return;
    }
    connect(in, &QIODevice::readyRead, this, [this, in]() { m_pcm.append(in->readAll()); });
    m_status->setText(tr("Listening. Read the lines now."));
    m_since.start();
    m_clock->start();
}

void VoiceLesson::finish()
{
    m_clock->stop();
    if (m_source) {
        m_source->stop();
        m_source->deleteLater();
        m_source = nullptr;
    }
    m_bar->setValue(kLessonMs);
    m_status->setText(tr("Working it out..."));

    const BurnOS::Voice::Spoken spoken = BurnOS::Voice::spoken(m_pcm);
    BurnOS::liveLog(QStringLiteral("voice check: the reading was %1 s long, %2 s of it speech")
                        .arg(m_pcm.size() / 32000.0, 0, 'f', 1)
                        .arg(spoken.pcm.size() / 32000.0, 0, 'f', 1));
    m_pcm.clear(); // the recording itself is not kept
    const int spokenMs = int(spoken.pcm.size() / 32); // 16 kHz, 16 bit: 32 bytes a millisecond
    if (spokenMs < kLessonSpeechMs) {
        fail(tr("I only heard %1 seconds of speech. Try again, a little closer to the "
                "microphone.").arg(spokenMs / 1000));
        return;
    }

    // Two voiceprints, one of each half, so the whole reading is used and the
    // halves can be held against each other.
    const qsizetype half = (spoken.pcm.size() / 2) & ~qsizetype(1);
    const QByteArray second = spoken.pcm.mid(half);
    const double level = double(spoken.level);
    BurnOS::Voice::print(this, spoken.pcm.left(half),
                         [this, second, level, spokenMs](const BurnOS::Voice::Print &a) {
        BurnOS::Voice::print(this, second,
                             [this, a, level, spokenMs](const BurnOS::Voice::Print &b) {
            if (a.isEmpty() || b.isEmpty()) {
                fail(tr("The voice model could not be run. The log says why."));
                return;
            }
            const float same = BurnOS::Voice::alike(a, b);
            if (same < kOneVoice) {
                fail(tr("The two halves of that did not sound like one voice. Try again "
                        "with the room quiet."));
                return;
            }
            BurnOS::Voice::Print both(a.size());
            double square = 0.0;
            for (qsizetype i = 0; i < a.size(); ++i) {
                both[i] = a[i] + b[i];
                square += double(both[i]) * double(both[i]);
            }
            const float norm = square > 0.0 ? float(std::sqrt(square)) : 1.0f;
            for (float &value : both)
                value /= norm;
            BurnOS::Settings::setVoicePrint(both, level);
            BurnOS::Settings::setVoiceCheckOn(true);
            BurnOS::liveLog(QStringLiteral("voice check: a voice was TAUGHT from %1 s of speech - "
                                           "its halves alike %2, loudness %3")
                                .arg(spokenMs / 1000.0, 0, 'f', 1)
                                .arg(double(same), 0, 'f', 2)
                                .arg(level, 0, 'f', 4));
            m_status->setText(tr("Done. Burnos now answers only this voice."));
            m_start->setText(tr("Teach again"));
            m_start->setEnabled(true);
            if (!m_rehearsal.isEmpty())
                accept();
        });
    });
}

void VoiceLesson::fail(const QString &why)
{
    BurnOS::liveLog(QStringLiteral("voice check: teaching FAILED - %1").arg(why));
    m_status->setText(why);
    m_start->setEnabled(true);
    if (!m_rehearsal.isEmpty())
        reject();
}

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
    m_model->setToolTip(tr("Which model listens and answers you. Pick a fast one: "
                           "every second it thinks is a second of silence. Hover "
                           "a choice to see what it is good at, or type any other "
                           "OpenRouter model id. Picking one takes effect at "
                           "once; a typed one when you press Enter or click "
                           "away."));

    // A LINE UNDER THE BOX SAYING WHETHER THIS MODEL CAN ACTUALLY DO THE JOB.
    // Burnos operates the machine with a tool call and checks its own work by
    // looking at a screenshot, and a model that cannot do both fails every such
    // turn - the only sign of which used to be the word Error on the orb.
    m_modelNote = new QLabel(this);
    m_modelNote->setWordWrap(true);
    m_modelNote->setStyleSheet(QStringLiteral("color: #8fa3bd;"));

    // THE WORKING MODEL. Every background task thinks with this one while the
    // talking model above carries on the conversation, both at once: a fast
    // model to talk, a substantive one to do the work. The first entry stores
    // nothing and runs tasks on the talking model, the way every version
    // before 0.5 did, which is why it is the default.
    m_taskModel = new QComboBox(this);
    m_taskModel->setEditable(true);
    m_taskModel->setInsertPolicy(QComboBox::NoInsert);
    m_taskModel->addItem(tr(kSameAsTalking));
    m_taskModel->setItemData(0, tr("One model does everything. Simplest, and the "
                                   "cheapest when that model is a fast one."),
                             Qt::ToolTipRole);
    for (const ModelChoice &choice : kModels) {
        m_taskModel->addItem(QString::fromLatin1(choice.slug));
        m_taskModel->setItemData(m_taskModel->count() - 1,
                                 tr(choice.note), Qt::ToolTipRole);
    }
    m_taskModel->setToolTip(tr("Which model does the work you hand off - driving "
                               "the desktop, writing code, anything that takes "
                               "more than a sentence. It works in the background "
                               "at the same time as the conversation, so a "
                               "slower, stronger model here never slows the "
                               "talking down."));

    m_taskModelNote = new QLabel(this);
    m_taskModelNote->setWordWrap(true);
    m_taskModelNote->setStyleSheet(QStringLiteral("color: #8fa3bd;"));

    connect(BurnOS::ModelCatalogue::instance(), &BurnOS::ModelCatalogue::loaded,
            this, &SettingsDialog::applyCatalogue);
    BurnOS::ModelCatalogue::instance()->load();

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

    // NOT A DEFAULT BUTTON. A dialog hands Enter to its first push button, and
    // this was it: pressing Enter to commit a typed model id opened the
    // integrations page instead. Enter belongs to the field being typed in.
    m_integrations->setAutoDefault(false);

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
    form->addRow(tr("Talking model"), m_model);
    form->addRow(QString(), m_modelNote);
    form->addRow(tr("Working model"), m_taskModel);
    form->addRow(QString(), m_taskModelNote);
    form->addRow(tr("Answer length"), m_tokens);
    form->addRow(tr("Integrations"), integrationsRow);
    form->addRow(tr("Microphone sensitivity"), sensitivityRow);
    form->addRow(tr("Pause before it answers"), hangoverRow);
    auto *dictateKey = new KeyField(this);
    dictateKey->setToolTip(tr("Hold this key and speak: the words are typed where "
                              "the cursor is, in any program, instead of being "
                              "answered. Any single key works, or a key with "
                              "Ctrl, Shift, Alt or Win. A key on its own stops "
                              "typing in other programs while Burnos runs. "
                              "Backspace removes it."));
    auto *dictateNote = new QLabel(tr("Hold it and speak, and the words are typed where the "
                                      "cursor is. Or ask Burnos to start transcribing."), this);
    dictateNote->setWordWrap(true);
    dictateNote->setStyleSheet(QStringLiteral("color: #8fa3bd;"));
    form->addRow(tr("Transcription key"), dictateKey);
    form->addRow(QString(), dictateNote);

    // WAKE WORD MODE, and the wake word. With the mode on, Burnos answers only
    // what is said after the wake word; until it hears it, a small keyword
    // model is all that listens. The wake word has to be something that model
    // can spell - plain letters - so one it cannot is refused here, and the
    // word before it stays. What changes here is taken up when the dialog
    // closes.
    auto *wake = new QLineEdit(this);
    wake->setMaxLength(40);
    wake->setText(BurnOS::Settings::wakeWord());
    wake->setPlaceholderText(BurnOS::Settings::defaultWakeWord());
    wake->setEnabled(BurnOS::Wake::available());
    wake->setToolTip(tr("What you say to wake Burnos in wake word mode; what you say "
                        "straight after it is your request. Two words work "
                        "better than one. Empty goes back to \"%1\".")
                         .arg(BurnOS::Settings::defaultWakeWord()));
    auto *wakeMode = new QCheckBox(tr("Answer only after the wake word"), this);
    wakeMode->setChecked(BurnOS::Settings::wakeMode());
    wakeMode->setEnabled(BurnOS::Wake::available());
    wakeMode->setToolTip(tr("Wake word mode: Burnos waits, answering nothing, until it "
                            "hears the wake word. It then answers what you ask and "
                            "listens for a few seconds more before it waits again. "
                            "\"Mute\" and \"unmute\" work by voice with or without it."));
    auto *wakeNote = new QLabel(this);
    wakeNote->setWordWrap(true);
    wakeNote->setStyleSheet(QStringLiteral("color: #8fa3bd;"));
    const auto showWake = [wakeNote, wakeMode](bool refused) {
        wakeNote->setText(
            !BurnOS::Wake::available()
                ? tr("The wake word model is not here yet. Restart Burnos.")
                : refused ? tr("That cannot be a wake word: plain letters only, four or "
                               "more. The one before it was kept.")
                          : wakeMode->isChecked()
                                ? tr("Burnos waits for \"%1\" and answers what you say after "
                                     "it. Until then nothing it hears is kept.")
                                      .arg(BurnOS::Settings::wakeWord())
                                : tr("Off: Burnos answers whatever it hears, and \"%1\" is "
                                     "not listened for.")
                                      .arg(BurnOS::Settings::wakeWord()));
    };
    showWake(false);
    connect(wakeMode, &QCheckBox::toggled, this, [showWake](bool on) {
        BurnOS::Settings::setWakeMode(on);
        showWake(false);
    });
    connect(wake, &QLineEdit::editingFinished, this, [wake, showWake]() {
        const QString typed = wake->text().simplified();
        if (typed.compare(BurnOS::Settings::wakeWord(), Qt::CaseInsensitive) == 0)
            return;
        const bool refused = !typed.isEmpty() && !BurnOS::Wake::canSpell(typed);
        if (!refused)
            BurnOS::Settings::setWakeWord(typed);
        wake->setText(BurnOS::Settings::wakeWord());
        showWake(refused);
    });
    form->addRow(tr("Wake word"), wake);
    form->addRow(QString(), wakeMode);
    form->addRow(QString(), wakeNote);

    // MY VOICE. Once taught, Burnos answers only phrases that sound like it:
    // a television or somebody else in the room is neither answered nor
    // remembered. The check can be switched off again without forgetting the
    // voice, and how alike a phrase has to be is the slider.
    auto *teach = new QPushButton(this);
    teach->setAutoDefault(false);
    auto *voiceState = new QLabel(this);
    voiceState->setStyleSheet(QStringLiteral("color: #8fa3bd;"));
    auto *onlyMe = new QCheckBox(tr("Answer only my voice"), this);
    onlyMe->setToolTip(tr("Other voices - a television, other people in the room - "
                          "are ignored: not answered and not remembered. The "
                          "transcription key is never checked."));
    auto *strict = new QSlider(Qt::Horizontal, this);
    strict->setRange(0, 100);
    strict->setSingleStep(5);
    strict->setPageStep(10);
    strict->setToolTip(tr("How closely a phrase has to match your voice. Raise it if "
                          "other voices still get through; lower it if Burnos "
                          "ignores you."));
    auto *strictValue = new QLabel(this);
    strictValue->setMinimumWidth(44);
    const auto showVoice = [teach, voiceState, onlyMe, strict]() {
        const bool able = BurnOS::Voice::available();
        const bool taught = !BurnOS::Settings::voicePrint().isEmpty();
        teach->setEnabled(able);
        teach->setText(taught ? tr("Teach it again...") : tr("Teach Burnos my voice..."));
        voiceState->setText(!able ? tr("The voice model is not here yet. Restart Burnos.")
                                  : taught ? tr("Burnos knows your voice.")
                                           : tr("Not taught yet: every voice is answered."));
        onlyMe->setEnabled(able && taught);
        strict->setEnabled(able && taught);
    };
    onlyMe->setChecked(BurnOS::Settings::voiceCheckOn());
    strict->setValue(BurnOS::Settings::voiceStrictness());
    strictValue->setText(tr("%1 %").arg(strict->value()));
    showVoice();
    connect(teach, &QPushButton::clicked, this, [this, onlyMe, showVoice]() {
        VoiceLesson lesson(this);
        lesson.exec();
        onlyMe->setChecked(BurnOS::Settings::voiceCheckOn());
        showVoice();
    });
    connect(onlyMe, &QCheckBox::toggled, this,
            [](bool on) { BurnOS::Settings::setVoiceCheckOn(on); });
    // Written when the slider is let go, not on every step of a drag.
    connect(strict, &QSlider::valueChanged, this, [strict, strictValue](int value) {
        strictValue->setText(tr("%1 %").arg(value));
        if (!strict->isSliderDown())
            BurnOS::Settings::setVoiceStrictness(value);
    });
    connect(strict, &QSlider::sliderReleased, this,
            [strict]() { BurnOS::Settings::setVoiceStrictness(strict->value()); });
    auto *teachRow = new QHBoxLayout;
    teachRow->setContentsMargins(0, 0, 0, 0);
    teachRow->addWidget(teach);
    teachRow->addWidget(voiceState, 1);
    auto *strictRow = new QHBoxLayout;
    strictRow->setContentsMargins(0, 0, 0, 0);
    strictRow->addWidget(strict, 1);
    strictRow->addWidget(strictValue);
    form->addRow(tr("My voice"), teachRow);
    form->addRow(QString(), onlyMe);
    form->addRow(tr("How strict"), strictRow);

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
    // Nor this one, for the same reason: Enter commits a field, it does not
    // close the page. Escape and the button itself still do.
    if (QPushButton *close = buttons->button(QDialogButtonBox::Close))
        close->setAutoDefault(false);
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
    m_taskModel->setCurrentText(BurnOS::Settings::hasTaskModel()
                                    ? BurnOS::Settings::taskModel()
                                    : tr(kSameAsTalking));

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
    if (QLineEdit *edit = m_taskModel->lineEdit()) {
        connect(edit, &QLineEdit::editingFinished, this,
                &SettingsDialog::saveCredentials);
    }
    connect(m_taskModel, &QComboBox::activated, this,
            [this](int) { saveCredentials(); });

    // The line underneath follows the box as it changes, so the answer to "can
    // this one do the job" is there BEFORE the choice is committed rather than
    // after it has broken a turn.
    connect(m_model, &QComboBox::currentTextChanged, this,
            [this](const QString &) { refreshModelNote(); });
    connect(m_taskModel, &QComboBox::currentTextChanged, this,
            [this](const QString &) { refreshModelNote(); });
    refreshModelNote();
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

    // A MODEL THAT CANNOT DO THE JOB IS NOT STORED AT ALL. Sending a screenshot
    // to a model with no image endpoint does not degrade gracefully: OpenRouter
    // throws the entire request away, so choosing one here would quietly break
    // every turn that checks its own work. The field goes back to what was
    // stored and the line underneath says why.
    //
    // TWO MODELS since v0.5, judged TOGETHER: what the talking model has to be
    // able to do depends on whether it does the work as well.
    BurnOS::ModelCatalogue *catalogue = BurnOS::ModelCatalogue::instance();
    const QString talk = m_model->currentText().trimmed();
    const QString work = chosenTaskModel();
    const QString keptTalk = BurnOS::Settings::openRouterModel();
    const QString keptWork = BurnOS::Settings::hasTaskModel()
                                 ? BurnOS::Settings::taskModel()
                                 : QString();

    // NOTHING CHANGED, SO NOTHING TO SAY. One Enter commits a field twice -
    // once for the line edit, and once more when the combo finds the text it
    // was put back to on its own list - and that second pass must not wipe out
    // the red line explaining a refusal the first one has just written.
    const QString talkNow = talk.isEmpty() ? BurnOS::Settings::defaultOpenRouterModel()
                                           : talk;
    if (talkNow == keptTalk && work == keptWork)
        return;

    QString workRefused;
    if (!work.isEmpty() && !catalogue->fitToRun(work)) {
        workRefused = tr("%1 cannot both see the screen and use tools, which "
                         "every task needs, so it was not saved.").arg(work);
    }
    const QString workAfter = workRefused.isEmpty() ? work : keptWork;

    // Only a model that has just been CHANGED is judged here, so the one that
    // was already stored is never refused for a clash the other just caused.
    const bool talkChanged = talkNow != keptTalk;
    QString talkRefused;
    if (talkChanged && !talk.isEmpty() && !catalogue->fitToTalk(talk)) {
        talkRefused = tr("%1 cannot call tools, so it could neither start work "
                         "nor remember anything. It was not saved.").arg(talk);
    } else if (talkChanged && !talk.isEmpty() && workAfter.isEmpty()
               && !catalogue->fitToRun(talk)) {
        talkRefused = tr("%1 cannot see the screen, so it can only talk while a "
                         "separate working model does the work - choose that "
                         "first. It was not saved.").arg(talk);
    }
    const QString talkAfter = !talkRefused.isEmpty() ? keptTalk
                              : talk.isEmpty() ? BurnOS::Settings::defaultOpenRouterModel()
                                               : talk;

    // THE LAST WAY TO BREAK IT: going back to one model while the talking one
    // is a model that cannot see.
    if (workRefused.isEmpty() && work.isEmpty() && !keptWork.isEmpty()
        && !catalogue->fitToRun(talkAfter)) {
        workRefused = tr("%1 cannot see the screen, so it cannot do the work as "
                         "well as the talking.").arg(talkAfter);
    }

    // A refused box is put back with its signals off, or currentTextChanged
    // would overwrite the explanation below with a cheerful line about what
    // is actually stored.
    if (workRefused.isEmpty()) {
        BurnOS::Settings::setTaskModel(work);
    } else {
        BurnOS::liveLog(QStringLiteral("settings REFUSED working model %1: %2")
                            .arg(work.isEmpty() ? QStringLiteral("same as talking") : work,
                                 workRefused));
        const QSignalBlocker quiet(m_taskModel);
        m_taskModel->setCurrentText(keptWork.isEmpty() ? tr(kSameAsTalking) : keptWork);
    }
    if (talkRefused.isEmpty()) {
        BurnOS::Settings::setOpenRouterModel(talk);
    } else {
        BurnOS::liveLog(QStringLiteral("settings REFUSED talking model %1: %2")
                            .arg(talk, talkRefused));
        const QSignalBlocker quiet(m_model);
        m_model->setCurrentText(keptTalk);
    }

    // Both lines describe what is now stored; a refusal then takes over its
    // own line, in red, saying why and what is still in use.
    refreshModelNote();
    const QString red = QStringLiteral("color: %1;").arg(QLatin1String(kNoteRefuse));
    if (!workRefused.isEmpty()) {
        m_taskModelNote->setStyleSheet(red);
        m_taskModelNote->setText(tr("%1 Still using %2.")
                                     .arg(workRefused,
                                          keptWork.isEmpty() ? tr("the talking model")
                                                             : keptWork));
    }
    if (!talkRefused.isEmpty()) {
        m_modelNote->setStyleSheet(red);
        m_modelNote->setText(tr("%1 Still using %2.").arg(talkRefused, keptTalk));
    }
}

QString SettingsDialog::chosenTaskModel() const
{
    const QString text = m_taskModel->currentText().trimmed();
    return text == tr(kSameAsTalking) ? QString() : text;
}

void SettingsDialog::applyCatalogue()
{
    BurnOS::ModelCatalogue *catalogue = BurnOS::ModelCatalogue::instance();
    if (!catalogue->isLoaded()) {
        refreshModelNote();
        return;
    }

    // A SHORTLIST IS ONLY EVER A GUESS ABOUT WHAT A MODEL CAN DO. Once the real
    // catalogue is in, anything on the list that cannot see the screen or call
    // tools comes off it rather than sitting there for somebody to pick. The
    // one currently in use is left alone even if it fails - taking it out from
    // under the user while they are looking at it explains nothing; the line
    // underneath does that instead.
    //
    // Each list by its own bar: the talking one needs tools, the working one
    // tools and eyes. The working list's first entry, "same as talking", is
    // not a model and always stays.
    const auto prune = [catalogue](QComboBox *box, bool working) {
        const QString chosen = box->currentText();
        const QSignalBlocker quiet(box);
        for (int i = box->count() - 1; i >= 0; --i) {
            const QString slug = box->itemText(i);
            if (slug == chosen || (working && i == 0))
                continue;
            if (!(working ? catalogue->fitToRun(slug) : catalogue->fitToTalk(slug)))
                box->removeItem(i);
        }
        box->setCurrentText(chosen);
    };
    prune(m_model, false);
    prune(m_taskModel, true);
    refreshModelNote();
}

void SettingsDialog::refreshModelNote()
{
    BurnOS::ModelCatalogue *catalogue = BurnOS::ModelCatalogue::instance();
    const auto say = [](QLabel *note, const char *colour, const QString &text) {
        note->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(colour)));
        note->setText(text);
    };
    const auto unknown = [](const QString &slug) {
        return tr("OpenRouter has no model called %1. Check the spelling, or "
                  "pick one from the list.").arg(slug);
    };

    const QString talk = m_model->currentText().trimmed();
    const QString work = chosenTaskModel();
    // One model doing both, so the talking one needs all a task does.
    const bool alsoWorks = work.isEmpty();

    // "Same as talking" needs no catalogue to explain.
    if (alsoWorks) {
        say(m_taskModelNote, kNotePlain,
            tr("Tasks run on the talking model, the way Burnos always has. "
               "Choose a stronger model here and the hard work is done by it, "
               "in the background, while the talking stays fast."));
    }

    if (!catalogue->isLoaded()) {
        say(m_modelNote, kNotePlain, tr("Asking OpenRouter what this model can do..."));
        if (!alsoWorks)
            say(m_taskModelNote, kNotePlain, tr("Asking OpenRouter what this model can do..."));
        return;
    }

    // THE TALKING MODEL: tools always, and eyes only when it does the work too.
    if (!talk.isEmpty() && !catalogue->knows(talk)) {
        say(m_modelNote, kNoteWarn, unknown(talk));
    } else if (!talk.isEmpty()) {
        const BurnOS::ModelFacts facts = catalogue->facts(talk);
        const QString label = facts.name.isEmpty() ? talk : facts.name;
        if (!facts.callsTools) {
            say(m_modelNote, kNoteRefuse,
                tr("%1 cannot call tools, so it could neither start work nor "
                   "remember anything. Burnos will not talk on it.").arg(label));
        } else if (!facts.seesImages && alsoWorks) {
            say(m_modelNote, kNoteRefuse,
                tr("%1 cannot see the screen, so it can only talk while a "
                   "separate working model does the work. Choose one below.")
                    .arg(label));
        } else if (!facts.seesImages) {
            say(m_modelNote, kNotePlain,
                tr("%1 talks and calls its tools. It never has to see: the "
                   "working model below does everything on screen.").arg(label));
        } else if (alsoWorks) {
            say(m_modelNote, kNotePlain,
                tr("%1 can see the screen and use its tools, so it can talk and "
                   "do the work as well.").arg(label));
        } else {
            say(m_modelNote, kNotePlain,
                tr("%1 can see the screen and use its tools. It does the "
                   "talking; the working model below does the work at the "
                   "same time.").arg(label));
        }
    }

    // THE WORKING MODEL: tools and eyes, because every task may need both.
    if (alsoWorks)
        return;
    if (!catalogue->knows(work)) {
        say(m_taskModelNote, kNoteWarn, unknown(work));
        return;
    }
    const BurnOS::ModelFacts facts = catalogue->facts(work);
    const QString label = facts.name.isEmpty() ? work : facts.name;
    if (facts.canDoTheJob()) {
        say(m_taskModelNote, kNotePlain,
            tr("%1 can see the screen and use its tools. Every task thinks with "
               "it in the background while the talking model keeps the "
               "conversation going.").arg(label));
        return;
    }
    QStringList missing;
    if (!facts.seesImages)
        missing.append(tr("cannot see the screen, so it cannot check its own work"));
    if (!facts.callsTools)
        missing.append(tr("cannot call tools, so it cannot operate this machine"));
    say(m_taskModelNote, kNoteRefuse,
        tr("%1 %2. No task will run on it.").arg(label, missing.join(tr(", and it "))));
}

// ===========================================================================
// MANAGE INTEGRATIONS
// ===========================================================================

namespace {

// How long to keep asking whether a sign in finished, and how often. Two
// minutes is long enough for a password manager, a sign in and a consent
// screen; past that the user has plainly wandered off and Refresh is there.
constexpr int kPollMs = 4000;
constexpr int kMaxPolls = 30;

// Which role on a list item holds what. The slug is what every Composio call
// takes; the connection id is what disconnecting needs.
constexpr int kSlugRole = Qt::UserRole;
constexpr int kConnectionRole = Qt::UserRole + 1;
constexpr int kConnectedRole = Qt::UserRole + 2;
// Set for an app with nothing to sign in to. It has no Connect, because it
// already works: the session runs its tools as they are.
constexpr int kNoAuthRole = Qt::UserRole + 3;

} // namespace

IntegrationsDialog::IntegrationsDialog(QWidget *parent)
    : QDialog(parent)
    , m_composio(new ComposioClient(this))
{
    setWindowTitle(tr("Manage Integrations"));
    setModal(true);
    setMinimumSize(540, 560);
    setStyleSheet(QString::fromLatin1(kStyle));

    // The orb stays on top, so a dialog without the same hint opens behind it
    // and looks like nothing happened.
    setWindowFlag(Qt::WindowStaysOnTopHint, true);

    auto *blurb = new QLabel(
        tr("Connect the apps you want Burnos to use. Signing in happens in your "
           "browser and Burnos never sees the password - Composio holds the "
           "connection and refreshes it. Ask for things out loud afterwards: "
           "\"email Sarah the notes\", \"post that in Slack\". Apps that need "
           "no sign in work straight away, and asking for one that is not "
           "connected yet opens its sign in for you."),
        this);
    blurb->setWordWrap(true);
    blurb->setStyleSheet(QStringLiteral("color: #8fa3bd;"));

    m_key = new QLineEdit(this);
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText(QStringLiteral("ak_..."));
    m_key->setToolTip(tr("Your Composio project API key, from Settings then "
                         "Project Settings on composio.dev. Stored encrypted "
                         "for this Windows user only."));
    connect(m_key, &QLineEdit::editingFinished, this, &IntegrationsDialog::keyEdited);

    m_showKey = new QCheckBox(tr("Show"), this);
    connect(m_showKey, &QCheckBox::toggled, this, [this](bool on) {
        m_key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });

    auto *keyRow = new QHBoxLayout;
    keyRow->setContentsMargins(0, 0, 0, 0);
    keyRow->addWidget(m_key, 1);
    keyRow->addWidget(m_showKey);

    auto *keyForm = new QFormLayout;
    keyForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    keyForm->setHorizontalSpacing(14);
    keyForm->addRow(tr("Composio key"), keyRow);

    m_search = new QLineEdit(this);
    m_search->setClearButtonEnabled(true);
    m_search->setPlaceholderText(tr("Search apps - Gmail, Slack, Notion, "
                                    "Calendar, GitHub..."));

    m_typing = new QTimer(this);
    m_typing->setSingleShot(true);
    m_typing->setInterval(350);
    connect(m_typing, &QTimer::timeout, this, &IntegrationsDialog::search);
    connect(m_search, &QLineEdit::textChanged, this, [this]() { m_typing->start(); });
    // Enter searches at once rather than waiting out the debounce.
    connect(m_search, &QLineEdit::returnPressed, this, [this]() {
        m_typing->stop();
        search();
    });

    m_list = new QListWidget(this);
    m_list->setAlternatingRowColors(false);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_list, &QListWidget::currentItemChanged,
            this, &IntegrationsDialog::selectionChanged);
    // Double clicking a row is the obvious way to connect it.
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item && !item->data(kConnectedRole).toBool())
            connectSelected();
    });

    m_connect = new QPushButton(tr("Connect"), this);
    m_connect->setEnabled(false);
    connect(m_connect, &QPushButton::clicked, this, &IntegrationsDialog::connectSelected);

    m_disconnect = new QPushButton(tr("Disconnect"), this);
    m_disconnect->setEnabled(false);
    connect(m_disconnect, &QPushButton::clicked,
            this, &IntegrationsDialog::disconnectSelected);

    m_refresh = new QPushButton(tr("Refresh"), this);
    connect(m_refresh, &QPushButton::clicked, this, [this]() {
        m_composio->refreshConnections();
        search();
    });

    // Enter belongs to the field being typed in, never to a button: with a
    // row selected it used to press Connect from inside the key field.
    for (QPushButton *button : {m_connect, m_disconnect, m_refresh})
        button->setAutoDefault(false);

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->addWidget(m_connect);
    actions->addWidget(m_disconnect);
    actions->addWidget(m_refresh);
    actions->addStretch(1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(32);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(10);
    root->addWidget(blurb);
    root->addLayout(keyForm);
    root->addWidget(m_search);
    root->addWidget(m_list, 1);
    root->addLayout(actions);
    root->addWidget(m_status);
    root->addWidget(buttons);

    // Everything below is one of the two lists coming back, merged and redrawn.
    connect(m_composio, &ComposioClient::appsReady, this,
            [this](const QList<ComposioApp> &apps, bool connectedOnly) {
                if (connectedOnly) {
                    m_connected.clear();
                    for (const ComposioApp &app : apps) {
                        if (!app.connectionStatus.isEmpty())
                            m_connected.insert(app.slug, app);
                    }
                    // The sign in we sent them off to has landed.
                    if (!m_awaiting.isEmpty()
                        && m_connected.value(m_awaiting).isConnected()) {
                        say(tr("%1 is connected.").arg(displayName(m_awaiting)));
                        m_awaiting.clear();
                        if (m_poll)
                            m_poll->stop();
                    }
                } else {
                    m_catalogue = apps;
                    m_catalogueLoaded = true;
                    for (const ComposioApp &app : apps) {
                        if (!app.name.isEmpty())
                            m_names.insert(app.slug, app.name);
                    }
                }
                repaintList();
            });

    connect(m_composio, &ComposioClient::linkOpened, this, [this](const QString &slug) {
        m_awaiting = slug;
        m_polls = 0;
        say(tr("Sign in to %1 in the browser window that just opened. This will "
               "pick it up as soon as you have.").arg(displayName(slug)));
        if (!m_poll) {
            m_poll = new QTimer(this);
            m_poll->setInterval(kPollMs);
            connect(m_poll, &QTimer::timeout, this, [this]() {
                if (++m_polls > kMaxPolls || m_awaiting.isEmpty()) {
                    m_poll->stop();
                    if (!m_awaiting.isEmpty()) {
                        say(tr("Still not connected. Finish the sign in and press "
                               "Refresh."));
                        m_awaiting.clear();
                    }
                    return;
                }
                m_composio->refreshConnections();
            });
        }
        m_poll->start();
    });

    connect(m_composio, &ComposioClient::disconnected, this, [this](const QString &) {
        say(tr("Disconnected."));
        m_composio->refreshConnections();
    });

    connect(m_composio, &ComposioClient::noSignInNeeded, this, [this](const QString &slug) {
        say(tr("%1 needs no sign in - Burnos can already use it. Just ask for it "
               "out loud.").arg(displayName(slug)));
    });

    connect(m_composio, &ComposioClient::failed, this, [this](const QString &reason) {
        say(reason, true);
    });

    // The key field is filled in behind a password mask, exactly as the
    // OpenRouter one is: shown to be corrected, never kept in a member.
    m_key->setText(BurnOS::Settings::composioKey());

    if (ComposioClient::isConfigured()) {
        m_composio->refreshConnections();
        search();
    } else {
        say(tr("Paste a Composio key above to see the apps you can connect. The "
               "free plan covers a hundred thousand actions a month."));
    }
}

void IntegrationsDialog::keyEdited()
{
    const QString typed = m_key->text().trimmed();
    if (typed == BurnOS::Settings::composioKey())
        return;

    BurnOS::Settings::setComposioKey(typed);
    if (typed.isEmpty()) {
        m_catalogue.clear();
        m_connected.clear();
        BurnOS::Settings::setComposioApps(QStringList());
        repaintList();
        say(tr("Key removed. Burnos will not reach any connected app now."));
        return;
    }

    say(tr("Key saved. Loading apps."));
    m_composio->refreshConnections();
    search();
}

void IntegrationsDialog::search()
{
    if (!ComposioClient::isConfigured())
        return;
    m_composio->searchApps(m_search->text(), 60);
}

void IntegrationsDialog::repaintList()
{
    const QString keep = m_list->currentItem()
                             ? m_list->currentItem()->data(kSlugRole).toString()
                             : QString();

    m_list->clear();

    // Connected first, always. They are the few rows that matter out of sixty,
    // and hunting for Gmail in an alphabet of apps nobody signed into is how a
    // settings page becomes something the user avoids opening.
    QStringList shown;
    auto addRow = [this, &shown](const ComposioApp &app, bool connected) {
        if (app.slug.isEmpty() || shown.contains(app.slug))
            return;
        shown.append(app.slug);

        const ComposioApp link = m_connected.value(app.slug);
        QString label = displayName(app.slug);
        if (connected)
            label += tr("   - connected");
        else if (link.isPending())
            label += tr("   - waiting for sign in");
        else if (app.builtIn)
            label += tr("   - built in, always on");
        else if (app.noAuth)
            label += tr("   - ready, no sign in needed");
        else if (!app.managedAuth)
            label += tr("   - needs its own credentials");

        auto *item = new QListWidgetItem(label, m_list);
        item->setData(kSlugRole, app.slug);
        item->setData(kConnectionRole, link.connectionId);
        item->setData(kConnectedRole, connected);
        item->setData(kNoAuthRole, app.noAuth || app.builtIn);
        if (!app.description.isEmpty())
            item->setToolTip(app.description);
        if (connected)
            item->setForeground(QColor(QStringLiteral("#7fd3a0")));
    };

    // A connected app is shown whatever the search says, because taking it away
    // has to stay possible without guessing its name back.
    for (auto it = m_connected.constBegin(); it != m_connected.constEnd(); ++it) {
        if (!it.value().isConnected())
            continue;
        ComposioApp app = it.value();
        app.name = displayName(app.slug);
        addRow(app, true);
    }

    for (const ComposioApp &app : std::as_const(m_catalogue))
        addRow(app, m_connected.value(app.slug).isConnected());

    // Only once a search has really come back empty. The connections answer
    // usually lands first, on a list with nothing in it yet, and the message
    // from that moment used to sit on under a full catalogue.
    const QString nothing =
        tr("Nothing matched. Try the app's own name - Gmail rather than email.");
    if (m_list->count() == 0 && m_catalogueLoaded && ComposioClient::isConfigured())
        say(nothing);
    else if (m_status->text() == nothing)
        say(QString());

    if (!keep.isEmpty()) {
        for (int i = 0; i < m_list->count(); ++i) {
            if (m_list->item(i)->data(kSlugRole).toString() == keep) {
                m_list->setCurrentRow(i);
                break;
            }
        }
    }
    selectionChanged();
}

void IntegrationsDialog::selectionChanged()
{
    QListWidgetItem *item = m_list->currentItem();
    const bool connected = item && item->data(kConnectedRole).toBool();
    const bool noAuth = item && item->data(kNoAuthRole).toBool();
    m_connect->setEnabled(item && !connected && !noAuth);
    m_disconnect->setEnabled(connected
                             && !item->data(kConnectionRole).toString().isEmpty());
}

void IntegrationsDialog::connectSelected()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    const QString slug = item->data(kSlugRole).toString();
    // Nothing to sign in to: say so instead of asking Composio for a sign in it
    // would refuse, which is the error this row used to show.
    if (item->data(kNoAuthRole).toBool()) {
        say(tr("%1 needs no sign in - Burnos can already use it. Just ask for it "
               "out loud.").arg(displayName(slug)));
        return;
    }
    say(tr("Opening the sign in for %1.").arg(displayName(slug)));
    m_composio->connectApp(slug);
}

void IntegrationsDialog::disconnectSelected()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    const QString id = item->data(kConnectionRole).toString();
    if (id.isEmpty())
        return;
    say(tr("Disconnecting %1.").arg(displayName(item->data(kSlugRole).toString())));
    m_composio->disconnectApp(id);
}

void IntegrationsDialog::say(const QString &message, bool isError)
{
    m_status->setStyleSheet(isError ? QStringLiteral("color: #e58a8a;")
                                    : QStringLiteral("color: #8fa3bd;"));
    m_status->setText(message);
}

QString IntegrationsDialog::displayName(const QString &slug) const
{
    const QString known = m_names.value(slug);
    if (!known.isEmpty())
        return known;

    // The connections endpoint gives slugs and nothing else, so "google_calendar"
    // has to be made presentable without asking anybody.
    QString pretty = slug;
    pretty.replace(QLatin1Char('_'), QLatin1Char(' '));
    QStringList words = pretty.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (QString &word : words) {
        if (!word.isEmpty())
            word[0] = word.at(0).toUpper();
    }
    return words.join(QLatin1Char(' '));
}
