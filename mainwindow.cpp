#include "agent.h"
#include "composio.h"
#include "installer.h"
#include "livelog.h"
#include "mainwindow.h"
#include "openrouter.h"
#include "orbwidget.h"
#include "settings.h"
#include "settingsdialog.h"
#include "sttengine.h"
#include "ttsengine.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QScreen>
#include <QStringList>
#include <QTimer>

// Last, and only for the hit test below: windows.h defines macros that quarrel
// with Qt's own headers if it is allowed to go first.
#include <windows.h>
#include <windowsx.h>

namespace {
// "Open app settings" and the handful of other ways the recogniser writes it
// down. Punctuation is dropped, because parakeet ends most phrases with a full
// stop, and the length is capped so a sentence that merely mentions settings
// is still a question for the model.
bool isSettingsCommand(const QString &text)
{
    QString said;
    for (const QChar c : text.toLower()) {
        if (c.isLetterOrNumber() || c.isSpace())
            said.append(c);
    }
    said = said.simplified();
    if (said.isEmpty() || said.size() > 40)
        return false;
    static const QStringList exact{
        QStringLiteral("settings"),
        QStringLiteral("app settings"),
    };
    if (exact.contains(said))
        return true;
    static const QStringList phrases{
        QStringLiteral("open settings"),
        QStringLiteral("open the settings"),
        QStringLiteral("open app settings"),
        QStringLiteral("open app setting"),
        QStringLiteral("open the app settings"),
        QStringLiteral("open application settings"),
        QStringLiteral("open settings menu"),
        QStringLiteral("open the settings menu"),
        QStringLiteral("show settings"),
        QStringLiteral("show the settings"),
        QStringLiteral("show app settings"),
        QStringLiteral("open preferences"),
        QStringLiteral("show preferences"),
    };
    for (const QString &phrase : phrases) {
        // A leading "burnos" or "hey" is common, so the tail is what counts.
        if (said == phrase || said.endsWith(QLatin1Char(' ') + phrase))
            return true;
    }
    return false;
}

// The replies the talker falls back on when a model calls a tool without a
// word of its own. Made once and kept on disk, so they play the instant they
// are needed instead of after two seconds of Kokoro.
QStringList instantReplies()
{
    return {QStringLiteral("On it."), QStringLiteral("Okay."),
            QStringLiteral("Stopped."), QStringLiteral("Got it."),
            QStringLiteral("Forgotten.")};
}

// A rehearsal line of the form "@4 what time is it" is said four seconds after
// the line before it, whether or not that one has been answered - which is how
// talking while it works is rehearsed. Returns the delay and leaves only the
// words; -1 for a line that is not timed.
int rehearsalDelayMs(QString *line)
{
    if (!line->startsWith(QLatin1Char('@')))
        return -1;
    const int space = int(line->indexOf(QLatin1Char(' ')));
    if (space < 2)
        return -1;
    bool ok = false;
    const double seconds = line->mid(1, space - 1).toDouble(&ok);
    if (!ok)
        return -1;
    *line = line->mid(space + 1).trimmed();
    return int(seconds * 1000.0);
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_orb(new OrbWidget(this))
{
    setWindowTitle(QStringLiteral("Burnos"));
    // Set on the window as well as on the application, so alt-tab and the task
    // bar show the orb even before the executable's own resource is read.
    setWindowIcon(OrbWidget::orbIcon());

    // Written once at startup, because almost every puzzling report turns out
    // to be a setting rather than a fault, and the log is the only channel this
    // program has. The key itself is never logged, only whether there is one.
    BurnOS::liveLog(QStringLiteral("Burnos %1").arg(QCoreApplication::applicationVersion()));
    BurnOS::liveLog(QStringLiteral("settings: %1 tokens, hangover %2 ms, sensitivity %3 "
                                   "(speech opens at %4), voice %5, volume %6, key %7")
                        .arg(BurnOS::Settings::replyTokens())
                        .arg(BurnOS::Settings::hangoverMs())
                        .arg(BurnOS::Settings::micSensitivity())
                        .arg(BurnOS::Settings::vadThreshold(), 0, 'f', 2)
                        .arg(BurnOS::Settings::ttsVoice())
                        .arg(BurnOS::Settings::ttsVolume())
                        .arg(BurnOS::Settings::hasOpenRouterKey() ? QStringLiteral("stored")
                                                                  : QStringLiteral("MISSING")));
    BurnOS::liveLog(QStringLiteral("models: talking %1, working %2")
                        .arg(BurnOS::Settings::openRouterModel(),
                             BurnOS::Settings::hasTaskModel()
                                 ? BurnOS::Settings::taskModel()
                                 : QStringLiteral("the same (no separate working model)")));

    // No frame, no title bar, no background: only the orb is ever painted,
    // and it stays above every other window.
    Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint;
    if (BurnOS::Settings::alwaysOnTop())
        flags |= Qt::WindowStaysOnTopHint;
    setWindowFlags(flags);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setStyleSheet(QStringLiteral("QMainWindow { background: transparent; }"));

    setCentralWidget(m_orb);

    // How big the orb is, and therefore how big the window is, is a stored
    // preference. It all lives in one place because it has to be redone the
    // moment the settings dialog changes it.
    applyOrbSize();
    restoreOrbPosition();

    // THE PARTS: ears, voice, the model, the user's online apps, and the Agent
    // that thinks with all of them. This window wires them together and
    // decides when something is said; it operates nothing itself.
    m_stt = new SttEngine(this);
    m_tts = new TtsEngine(this);
    m_ai = new OpenRouterClient(this);
    m_composio = new ComposioClient(this);
    m_agent = new BurnOS::Agent(m_ai, m_composio, this);

    // Whatever is waiting to be said is looked at again this often while
    // something is in the way: the user mid sentence, an answer still being
    // worked out.
    m_speakRetry = new QTimer(this);
    m_speakRetry->setSingleShot(true);
    m_speakRetry->setInterval(400);
    connect(m_speakRetry, &QTimer::timeout, this, &MainWindow::speakNext);

    // Ask Composio once at startup which apps are actually connected, rather
    // than trusting a cached list written who knows when - a connection revoked
    // from the other end would otherwise still be offered to the model.
    if (ComposioClient::isConfigured())
        m_composio->refreshConnections();

    // --- THE ORB ------------------------------------------------------------
    // ONE CLICK MUTES OR UNMUTES, and interrupts nothing that is being worked
    // on. A DOUBLE CLICK STOPS EVERYTHING. The first click of a double click
    // has already muted or unmuted by the time the second lands, and the stop
    // puts the orb back as it was before that click - a stop is not a mute -
    // and turns it red for two seconds.
    connect(m_orb, &OrbWidget::doubleClicked, this, [this]() {
        forceStop();
        // A STOP IS NOT A MUTE: an orb that was listening goes on listening.
        if (m_orb->isActive() && !m_stt->startLive())
            m_orb->setActive(false);
    });
    connect(m_orb, &OrbWidget::clicked, this, [this]() {
        if (m_orb->isActive()) {
            if (!m_stt->startLive())
                m_orb->setActive(false);
            unmuted();
        } else {
            mute();
        }
    });

    // --- THE EARS -----------------------------------------------------------
    // The core warps with the microphone - but not while the orb is talking,
    // when the microphone now stays open and mostly hears the orb itself.
    connect(m_stt, &SttEngine::levelChanged, this, [this](qreal level) {
        m_orb->setLevel(m_voicePlaying ? 0.0 : level);
    });
    // TALKING OVER THE VOICE: the user started speaking, clearly louder than
    // the orb's own echo. The voice stops at once and the phrase is theirs.
    connect(m_stt, &SttEngine::bargeIn, this, [this]() {
        BurnOS::liveLog(QStringLiteral("voice: stopped - the user is talking over it"));
        m_tts->stop();
        voiceGone();
    });
    // The orb brightens while a phrase is being kept, so the detector's opinion
    // is visible without any text.
    connect(m_stt, &SttEngine::speechStarted, this, [this]() {
        m_orb->setCapturing(true);
    });
    connect(m_stt, &SttEngine::speechEnded, this, [this]() {
        m_orb->setCapturing(false);
        m_transcribing = true;
        refreshThinking();
    });
    // The transcription is never shown. It goes to the Agent - unless it is a
    // spoken command for this window, which needs no model at all.
    connect(m_stt, &SttEngine::transcribed, this, [this](const QString &text) {
        m_transcribing = false;
        refreshThinking();
        if (m_dropHeard) {
            // Said before everything was stopped, read after it: not answered.
            m_dropHeard = false;
            BurnOS::liveLog(QStringLiteral("stopped: dropped what was still being "
                                           "transcribed"));
            return;
        }
        if (text.isEmpty()) {
            speakNext();
            return;
        }
        if (isSettingsCommand(text)) {
            openSettings(); // resumes listening itself once it closes
            return;
        }
        m_agent->hear(text);
    });
    // The microphone itself failing is the one case that disarms, because
    // there is nothing left to listen with.
    connect(m_stt, &SttEngine::failed, this, [this](const QString &reason) {
        // Nothing is being read any more, so there is nothing left to drop.
        if (!m_stt->isReading())
            m_dropHeard = false;
        m_transcribing = false;
        if (!m_stt->isLive())
            m_orb->setActive(false);
        m_orb->setCapturing(false);
        listenAgain();
        showNotice(reason);
    });

    // --- THE BRAIN ----------------------------------------------------------
    connect(m_agent, &BurnOS::Agent::say, this, &MainWindow::queueSpeech);
    connect(m_agent, &BurnOS::Agent::retract, this, &MainWindow::retractSpeech);
    connect(m_agent, &BurnOS::Agent::thinkingChanged, this, [this]() {
        refreshThinking();
        speakNext();
        rehearseWhenIdle();
    });
    connect(m_agent, &BurnOS::Agent::workingChanged, m_orb, &OrbWidget::setWorking);
    connect(m_agent, &BurnOS::Agent::failed, this, [this](const QString &reason) {
        showNotice(reason);
        rehearseWhenIdle();
    });

    // --- THE VOICE ----------------------------------------------------------
    connect(m_tts, &TtsEngine::speaking, this, [this](const QString &text) {
        m_orb->setPreparing(false);
        m_voicePlaying = true;
        // The microphone STAYS OPEN while the voice plays: it learns how loud
        // the voice comes back and lets the user talk over it.
        m_stt->setOverVoice(true);
        BurnOS::liveLog(QStringLiteral("orb deep blue: speaking %1 chars")
                            .arg(text.size()));
        m_orb->setSpeaking(true);
        // Said out loud, so that turn is closed and can no longer be taken back.
        if (m_voice.turn > 0)
            m_agent->spoken(m_voice.turn);
    });
    // The real loudness of the voice, every frame, so the orb breathes with it.
    connect(m_tts, &TtsEngine::level, m_orb, &OrbWidget::setSpeechLevel);
    connect(m_tts, &TtsEngine::finished, this, [this]() {
        voiceGone();
        listenAgain();
        speakNext();
        rehearseWhenIdle();
    });
    // A failed voice must not kill live mode: say why, then listen again.
    connect(m_tts, &TtsEngine::failed, this, [this](const QString &reason) {
        voiceGone();
        listenAgain();
        showNotice(reason);
        speakNext();
        rehearseWhenIdle();
    });

    // WHAT A MODEL CAN DO IS A FACT, NOT A GUESS. OpenRouter's own catalogue is
    // fetched once at startup, and if the model in Settings turns out not to be
    // able to see the screen or call tools, Burnos moves itself to one that can
    // rather than failing every task that checks its own work.
    connect(BurnOS::ModelCatalogue::instance(), &BurnOS::ModelCatalogue::loaded,
            this, &MainWindow::checkModelIsUpToTheJob);
    BurnOS::ModelCatalogue::instance()->load();

    // First run: the models are hundreds of megabytes and are not shipped, so
    // fetch whatever is missing before the orb becomes usable.
    m_setup = new ModelInstaller(this);
    connect(m_setup, &ModelInstaller::progress, m_orb, &OrbWidget::setStatus);
    connect(m_setup, &ModelInstaller::finished, m_orb, &OrbWidget::endInitializing);
    connect(m_setup, &ModelInstaller::failed, this, [this](const QString &reason) {
        m_orb->endInitializing();
        showNotice(reason);
    });
    // Nothing should have to be clicked to be heard: the orb comes up already
    // armed and blue. If the models are still missing it arms the moment the
    // download lands instead.
    connect(m_setup, &ModelInstaller::finished, this, &MainWindow::armLiveMode);

    if (!ModelInstaller::everythingPresent()) {
        m_orb->beginInitializing();
        m_setup->start();
    } else {
        // Deferred by one turn of the event loop, so the window is on screen
        // before the microphone is opened.
        QTimer::singleShot(0, this, &MainWindow::armLiveMode);
    }

    // Proof of the whole path without a microphone: each line is heard exactly
    // as a transcription would be, so the talker, the tasks, the voice and the
    // orb are all the real ones. Lines are separated by a vertical bar. A plain
    // line waits until everything before it has been answered out loud; a line
    // starting "@4 " goes four seconds after the one before, answered or not,
    // which is how talking while it works is proved. Armed only by the
    // environment, never in a normal run.
    m_rehearsal = qEnvironmentVariable("BURNOS_ASK_SELFTEST")
                      .split(QLatin1Char('|'), Qt::SkipEmptyParts);
    for (QString &line : m_rehearsal)
        line = line.trimmed();
    m_rehearsal.removeAll(QString());
    if (!m_rehearsal.isEmpty()) {
        const int delay = rehearsalDelayMs(&m_rehearsal.first());
        QTimer::singleShot(delay >= 0 ? delay : 1500, this, &MainWindow::rehearse);
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::armLiveMode()
{
    // The short replies are made once and kept, so they play at once. Asked
    // for here because this is the moment the voice model is known to be on
    // disk; anything already made is simply skipped.
    m_tts->prepare(instantReplies());

    if (m_stt->isLive())
        return;

    // The orb is told first, so the colour and the hint change on the same
    // frame the microphone opens; a failure puts it straight back to grey.
    m_orb->setActive(true);
    if (!m_stt->startLive())
        m_orb->setActive(false);
}

void MainWindow::restoreOrbPosition()
{
    // Where it was last left, if it has ever been moved. Anything off the edge
    // of every screen that still exists is pulled back on, so unplugging the
    // monitor it was parked on cannot lose the orb.
    QPoint topLeft;
    if (BurnOS::Settings::hasOrbPosition()) {
        topLeft = BurnOS::Settings::orbPosition();
    } else if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        topLeft = QPoint(area.center().x() - width() / 2,
                         area.center().y() - height() / 2);
    }

    const QRect box(topLeft, size());
    const QScreen *screen = QGuiApplication::screenAt(box.center());
    if (!screen)
        screen = QGuiApplication::screenAt(topLeft);
    if (!screen)
        screen = QGuiApplication::primaryScreen();

    if (screen) {
        const QRect area = screen->availableGeometry();
        topLeft.setX(qBound(area.left(), topLeft.x(), area.right() - width()));
        topLeft.setY(qBound(area.top(), topLeft.y(), area.bottom() - height()));
    }

    move(topLeft);
    BurnOS::liveLog(QStringLiteral("orb position: %1,%2")
                        .arg(topLeft.x())
                        .arg(topLeft.y()));
}

void MainWindow::applyOrbSize(int px)
{
    // THE WINDOW IS THE ORB, and it never changes size.
    // WHILE THE SLIDER IS MOVING the size arrives here directly, because the
    // registry is deliberately a moment behind: a drag would otherwise write
    // to it on every frame. Every other caller passes nothing and gets the
    // stored size.
    const int size = px > 0 ? px : BurnOS::Settings::orbSize();

    // Grown and shrunk about the same CENTRE, so changing the size in the
    // dialog does not throw the orb across the desktop.
    const QPoint centre = geometry().center();

    m_orb->setOrbDiameter(size);
    setFixedSize(size, size);

    QPoint topLeft(centre.x() - size / 2, centre.y() - size / 2);
    if (const QScreen *screen = QGuiApplication::screenAt(centre)) {
        const QRect area = screen->availableGeometry();
        topLeft.setX(qBound(area.left(), topLeft.x(), area.right() - size));
        topLeft.setY(qBound(area.top(), topLeft.y(), area.bottom() - size));
    }
    move(topLeft);

    // Not on every frame of a drag: that is a hundred lines of log for one
    // decision. The size settled on is logged when the dialog closes and this
    // runs once more from the stored value.
    if (px <= 0)
        BurnOS::liveLog(QStringLiteral("orb size: %1 px").arg(size));
}

void MainWindow::checkModelIsUpToTheJob()
{
    BurnOS::ModelCatalogue *catalogue = BurnOS::ModelCatalogue::instance();

    // The default is chosen to be capable, so it is the safe place to land. If
    // even that has lost a capability since, the first model on the catalogue
    // that can do the job will do: anything is better than one that is going to
    // fail every turn.
    const auto replacement = [catalogue]() -> QString {
        const QString fallback = BurnOS::Settings::defaultOpenRouterModel();
        if (catalogue->fitToRun(fallback))
            return fallback;
        const QStringList able = catalogue->capable();
        return able.isEmpty() ? QString() : able.first();
    };
    const auto missing = [catalogue](const QString &slug, bool mustSee) {
        const BurnOS::ModelFacts facts = catalogue->facts(slug);
        QStringList what;
        if (mustSee && !facts.seesImages)
            what.append(tr("see the screen"));
        if (!facts.callsTools)
            what.append(tr("call its tools"));
        return what.join(QStringLiteral(" or "));
    };

    // THE WORKING MODEL FIRST. Every task needs it to see and to call tools,
    // and whether the talking model must see as well depends on whether there
    // is a separate working model at all.
    if (BurnOS::Settings::hasTaskModel()) {
        const QString working = BurnOS::Settings::taskModel();
        const QString to = replacement();
        if (!catalogue->fitToRun(working) && !to.isEmpty()) {
            BurnOS::Settings::setTaskModel(to);
            BurnOS::liveLog(QStringLiteral("MODEL CHANGED: the working model %1 cannot "
                                           "%2, so tasks moved to %3. Settings says why.")
                                .arg(working, missing(working, true), to));
        }
    }

    // THE TALKING MODEL: it must always call tools, and must see the screen
    // too when it is also the one doing the work.
    const QString talking = BurnOS::Settings::openRouterModel();
    const bool alsoWorks = !BurnOS::Settings::hasTaskModel();
    if (alsoWorks ? catalogue->fitToRun(talking) : catalogue->fitToTalk(talking))
        return;
    const QString to = replacement();
    if (to.isEmpty())
        return;
    BurnOS::Settings::setOpenRouterModel(to);
    BurnOS::liveLog(QStringLiteral("MODEL CHANGED: the talking model %1 cannot %2, "
                                   "so Burnos moved to %3. Settings says why.")
                        .arg(talking, missing(talking, alsoWorks), to));
}

void MainWindow::showNotice(const QString &reason)
{
    // Errors go where the answer goes, and into the log. A tooltip over a
    // frameless, translucent, always on top window is not reliably visible, and
    // an invisible error cannot be told apart from no answer at all.
    BurnOS::liveLog(QStringLiteral("ERROR on screen: %1").arg(reason));
    m_orb->showNotice(reason);
}

void MainWindow::moveEvent(QMoveEvent *event)
{
    QMainWindow::moveEvent(event);

    // Remember where it was left. NOT written here: a drag fires this on every
    // mouse move, and the registry is not a place to write sixty times a
    // second. The timer collapses a whole drag into one write, half a second
    // after the orb comes to rest.
    if (!m_savePos) {
        m_savePos = new QTimer(this);
        m_savePos->setSingleShot(true);
        connect(m_savePos, &QTimer::timeout, this, [this]() {
            BurnOS::Settings::setOrbPosition(pos());
        });
    }
    m_savePos->start(500);
}

void MainWindow::queueSpeech(const QString &text, int turn)
{
    // THE VOICE SAYS WORDS, NOT FORMATTING. Every prompt asks for plain
    // speech, and a stronger working model still writes `fizz.py` in code
    // quotes or **bold** now and then. The marks come out here, at the one
    // door every spoken line comes through, whichever model wrote it.
    QString words = text;
    words.remove(QLatin1Char('`'));
    words.remove(QLatin1Char('*'));
    words = words.trimmed();
    if (words.isEmpty())
        return;
    m_speech.append(Speech{words, turn});
    BurnOS::liveLog(QStringLiteral("voice: queued %1: %2")
                        .arg(turn > 0 ? QStringLiteral("the answer to turn %1").arg(turn)
                                      : QStringLiteral("news from a task"),
                             words.simplified().left(160)));
    speakNext();
}

void MainWindow::retractSpeech(int turn)
{
    const qsizetype before = m_speech.size();
    m_speech.removeIf([turn](const Speech &s) { return s.turn == turn; });

    // Still being synthesised and not yet heard: stopped. Once it is audible
    // it cannot be taken back, and the Agent is told the moment it is.
    bool stopped = false;
    if (m_voiceBusy && !m_voicePlaying && m_voice.turn == turn) {
        m_tts->stop();
        m_voice = Speech();
        m_voiceBusy = false;
        m_orb->setPreparing(false);
        stopped = true;
    }
    if (stopped || before != m_speech.size()) {
        BurnOS::liveLog(QStringLiteral("voice: took back the unsaid answer to turn %1")
                            .arg(turn));
    }
}

void MainWindow::speakNext()
{
    if (m_voiceBusy || m_speech.isEmpty() || m_settingsOpen)
        return;

    // MUTED MEANS QUIET, NOT FORGOTTEN. The work goes on behind a muted orb, so
    // what it has to say - a task that finished, a question it needs answered -
    // waits here and is said after the click that unmutes.
    if (!m_orb->isActive()) {
        m_speakRetry->stop();
        return;
    }

    // Never over the user. Whatever is waiting goes once they have finished.
    if (m_stt->isCapturing()) {
        m_speakRetry->start();
        return;
    }

    // The talker's answer to what was just said goes before any news from a
    // task, and news waits while an answer is still being worked out, so it
    // cannot cut in front of it.
    int pick = -1;
    for (int i = 0; i < m_speech.size(); ++i) {
        if (m_speech.at(i).turn > 0) {
            pick = i;
            break;
        }
    }
    if (pick < 0) {
        if (m_transcribing || m_agent->isThinking()) {
            m_speakRetry->start();
            return;
        }
        pick = 0;
    }

    m_voice = m_speech.takeAt(pick);
    m_voiceBusy = true;
    m_orb->setPreparing(true);

    // A voice that cannot start says so through its failed signal, which
    // resets all of this and moves on to whatever is next - so there is
    // nothing to undo here, only the words to keep.
    const QString words = m_voice.text;
    BurnOS::liveLog(QStringLiteral("voice: saying %1").arg(words.simplified().left(160)));
    if (!m_tts->speak(words))
        BurnOS::liveLog(QStringLiteral("UNSPOKEN: %1").arg(words));
}

void MainWindow::voiceGone()
{
    m_stt->setOverVoice(false);
    m_voice = Speech();
    m_voiceBusy = false;
    m_voicePlaying = false;
    m_orb->stopSpeaking();
}

void MainWindow::refreshThinking()
{
    // Not behind a muted orb: the word there is "Muted", and the arcs round
    // the rim already say that work is going on.
    m_orb->setThinking(m_orb->isActive() && (m_transcribing || m_agent->isThinking()));
}

void MainWindow::listenAgain()
{
    refreshThinking();
    if (!m_voiceBusy)
        m_orb->setPreparing(false);

    // MUTED MEANS MUTED. Anything that finishes after the orb was clicked off
    // must not quietly reopen the microphone behind a grey orb - and nothing
    // reopens it while our own voice is audible or the settings are up.
    if (!m_orb->isActive() || m_voicePlaying || m_settingsOpen)
        return;

    m_stt->resumeListening();
}

void MainWindow::mute()
{
    // ONE CLICK: QUIET, AND NOTHING ELSE. The microphone shuts and the voice
    // stops mid word, but the talker goes on answering and every task goes on
    // working - a mute must never cost the user the thing they asked for.
    m_speakRetry->stop();

    // An answer still being made into speech has been heard by nobody, so it
    // goes back to the front of the queue and is said after the unmute. One
    // that was already audible is simply cut off: it was said.
    if (m_voiceBusy && !m_voicePlaying)
        m_speech.prepend(m_voice);

    m_stt->stopLive();
    m_tts->stop();
    voiceGone();
    m_orb->setPreparing(false);

    // A phrase the recogniser already has is still read and still answered:
    // it was said before the click. One cut off by the click is gone, and must
    // not leave "transcribing" standing with nothing to end it.
    m_transcribing = m_stt->isReading();
    refreshThinking();

    BurnOS::liveLog(QStringLiteral("muted: microphone shut and voice quiet - the talker and "
                                   "%1 task(s) carry on, %2 line(s) wait to be said")
                        .arg(m_agent->liveTasks())
                        .arg(m_speech.size()));
}

void MainWindow::unmuted()
{
    // Back on. Whatever went on behind the muted orb shows again, and what was
    // kept is said - but only once a double click has had its chance. This
    // click may be the first half of one, and a stop must not be preceded by
    // half a sentence.
    refreshThinking();
    QTimer::singleShot(QApplication::doubleClickInterval(), this, &MainWindow::speakNext);
    BurnOS::liveLog(QStringLiteral("unmuted: %1 line(s) kept while muted")
                        .arg(m_speech.size()));
}

void MainWindow::forceStop()
{
    // THIS STOPS EVERYTHING, AT ONCE. The queue goes first, so nothing that
    // stops below can start the voice again on its way out.
    m_speech.clear();
    m_speakRetry->stop();
    m_rehearsal.clear();
    m_rehearsalWaiting = false;

    // A phrase still inside the recogniser comes out after this. It is dropped.
    m_dropHeard = m_stt->isReading();
    m_stt->stopLive();
    m_tts->stop();
    voiceGone();
    m_transcribing = false;
    m_agent->stopAll();

    // THE HINT HAS TO BE PUT BACK BY HAND. Listening, Thinking and Preparing
    // are all states the orb can be sitting in when it is muted, and none of
    // them clear themselves.
    m_orb->setCapturing(false);
    m_orb->setThinking(false);
    m_orb->setPreparing(false);
    m_orb->setWorking(false);
    m_orb->showStopped();

    BurnOS::liveLog(QStringLiteral("stopped: microphone, talker, tasks, voice and queue "
                                   "all stopped"));
}

void MainWindow::openSettings()
{
    if (m_settingsOpen)
        return;
    m_settingsOpen = true;

    // Nothing should be heard while a key is being typed, and the orb must not
    // be talking over the dialog either. Live mode is only paused here, never
    // disarmed, so the orb stays blue and picks up again afterwards. Anything
    // still waiting to be said waits for the dialog to close; background tasks
    // carry on.
    const bool wasLive = m_stt->isLive();
    m_stt->pauseListening();
    m_tts->stop();
    voiceGone();
    m_orb->setPreparing(false);

    SettingsDialog dialog(this);

    // LIVE, while it is open. There is no Save button on that dialog: every
    // setting is written down as it is changed, and the two that live on this
    // window rather than in an engine arrive here as they change.
    connect(&dialog, &SettingsDialog::orbSizeChanged,
            this, [this](int px) { applyOrbSize(px); });
    connect(&dialog, &SettingsDialog::alwaysOnTopChanged, this, [this, &dialog]() {
        applyAlwaysOnTop();
        // Putting the flag back raises the orb, which would otherwise come up
        // over the very dialog that asked for it.
        if (dialog.isVisible()) {
            dialog.raise();
            dialog.activateWindow();
        }
    });

    dialog.exec();

    applyAlwaysOnTop();
    applyOrbSize();
    // An app may have been connected or dropped in Manage integrations, and
    // what the tasks are told they can reach comes from that list.
    if (ComposioClient::isConfigured())
        m_composio->refreshConnections();
    // The voice may have changed, and the short replies are kept per voice.
    m_tts->prepare(instantReplies());
    m_settingsOpen = false;

    if (wasLive)
        listenAgain();
    speakNext();
}

void MainWindow::applyAlwaysOnTop()
{
    const bool onTop = BurnOS::Settings::alwaysOnTop();
    if (windowFlags().testFlag(Qt::WindowStaysOnTopHint) == onTop)
        return;

    // Changing a window flag hides the window on Windows, so it has to be put
    // back up, and a frameless translucent one has to be raised as well.
    setWindowFlag(Qt::WindowStaysOnTopHint, onTop);
    show();
    raise();
}

void MainWindow::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral(
        "QMenu { background-color: #12161f; color: #d8e6f5; border: 1px solid #2c3648; padding: 4px; }"
        "QMenu::item { padding: 6px 24px 6px 16px; }"
        "QMenu::item:selected { background-color: #2a3c5a; }"));

    QAction *settingsAction = menu.addAction(tr("Settings..."));
    menu.addSeparator();
    QAction *exitAction = menu.addAction(tr("Exit"));

    // Acted on after the menu has closed rather than through a connect, so the
    // modal dialog is never opened from inside the menu's own event loop.
    QAction *chosen = menu.exec(event->globalPos());
    event->accept();

    if (chosen == settingsAction)
        openSettings();
    else if (chosen == exitAction)
        qApp->quit();
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message,
                             qintptr *result)
{
    // Everything but the orb itself is painted transparent and must not be
    // furniture: without this the desktop carries an invisible box that eats
    // every click near the orb. HTTRANSPARENT hands the click to whatever is
    // behind instead.
    if (eventType == "windows_generic_MSG") {
        MSG *msg = static_cast<MSG *>(message);
        if (msg && msg->message == WM_NCHITTEST) {
            RECT frame{};
            if (GetWindowRect(msg->hwnd, &frame)) {
                // lParam is in physical pixels and relative to the desktop, so
                // subtract the window origin first and only then unscale. That
                // keeps it right under display scaling without having to know
                // where the screen itself begins.
                const qreal scale = devicePixelRatioF();
                const QPointF local(
                    (GET_X_LPARAM(msg->lParam) - frame.left) / scale,
                    (GET_Y_LPARAM(msg->lParam) - frame.top) / scale);
                if (!m_orb->hitsOrb(local)) {
                    *result = HTTRANSPARENT;
                    return true;
                }
            }
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::rehearse()
{
    if (m_rehearsal.isEmpty())
        return;

    const QString line = m_rehearsal.takeFirst();
    BurnOS::liveLog(QStringLiteral("SELF TEST: heard as if spoken: %1").arg(line));
    // Through the SAME door a real sentence comes in by.
    m_agent->hear(line);

    if (m_rehearsal.isEmpty())
        return;
    const int delay = rehearsalDelayMs(&m_rehearsal.first());
    if (delay >= 0)
        QTimer::singleShot(delay, this, &MainWindow::rehearse);
    else
        m_rehearsalWaiting = true;
}

void MainWindow::rehearseWhenIdle()
{
    // The next untimed line goes once everything said so far has been answered
    // out loud. Work still going on in the background does not count: talking
    // over it is the whole point.
    if (!m_rehearsalWaiting || m_rehearsal.isEmpty())
        return;
    if (m_agent->isThinking() || m_voiceBusy || !m_speech.isEmpty())
        return;
    m_rehearsalWaiting = false;
    QTimer::singleShot(1200, this, &MainWindow::rehearse);
}
