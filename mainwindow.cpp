#include "composio.h"
#include "computeruse.h"
#include "installer.h"
#include "livelog.h"
#include "mainwindow.h"
#include "modelcatalogue.h"
#include "openrouter.h"
#include "orbwidget.h"
#include "settings.h"
#include "settingsdialog.h"
#include "sttengine.h"
#include "ttsengine.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMenu>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QStringList>
#include <QThread>
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
// Which program the user asked for, out of a sentence like "okay, open Firefox
// again". This is what lets it open ANYTHING on the machine: Jev cannot invent
// text, so a list it picks from could only ever hold what somebody thought to
// put there. The name has to come from the words themselves.
QString appNameFrom(const QString &said)
{
    static const QStringList triggers{
        QStringLiteral("open up "), QStringLiteral("bring up "),
        QStringLiteral("open "),    QStringLiteral("launch "),
        QStringLiteral("start "),   QStringLiteral("run "),
    };

    const QString lower = said.toLower();
    int at = -1;
    int skip = 0;
    for (const QString &trigger : triggers) {
        const int found = lower.indexOf(trigger);
        if (found >= 0 && (at < 0 || found < at)) {
            at = found;
            skip = trigger.size();
        }
    }
    if (at < 0)
        return {};

    QString name = said.mid(at + skip);

    // Anything past a sentence break belongs to the next thought.
    const int stop = name.indexOf(QRegularExpression(QStringLiteral("[.,;!?]")));
    if (stop >= 0)
        name = name.left(stop);
    name = name.simplified();

    // Filler people hang on the end of a request. Only ever stripped when there
    // is a real name in front of it.
    static const QStringList tail{
        QStringLiteral("again"),   QStringLiteral("please"),
        QStringLiteral("now"),     QStringLiteral("for me"),
        QStringLiteral("app"),     QStringLiteral("application"),
        QStringLiteral("program"), QStringLiteral("window"),
    };
    bool stripped = true;
    while (stripped) {
        stripped = false;
        for (const QString &word : tail) {
            const QString suffix = QLatin1Char(' ') + word;
            if (name.endsWith(suffix, Qt::CaseInsensitive)) {
                name.chop(suffix.size());
                name = name.simplified();
                stripped = true;
            }
        }
    }

    // A leading "the" is never part of a program's name.
    if (name.startsWith(QStringLiteral("the "), Qt::CaseInsensitive))
        name = name.mid(4).simplified();

    return name;
}

// What the user actually wants typed, out of a sentence like "type hello there"
// or "search for cheap flights". Jev cannot invent text, so this has to be
// pulled out of the words themselves.
QString textToType(const QString &said)
{
    static const QStringList triggers{
        QStringLiteral("search for "), QStringLiteral("fill in "),
        QStringLiteral("put in "),     QStringLiteral("type in "),
        QStringLiteral("type "),       QStringLiteral("write "),
        QStringLiteral("enter "),
    };

    const QString lower = said.toLower();
    int at = -1;
    int skip = 0;
    for (const QString &trigger : triggers) {
        const int found = lower.indexOf(trigger);
        if (found >= 0 && (at < 0 || found < at)) {
            at = found;
            skip = trigger.size();
        }
    }
    if (at < 0)
        return {};

    QString out = said.mid(at + skip).trimmed();
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char('!'))
           || out.endsWith(QLatin1Char('?')))
        out.chop(1);
    return out.trimmed();
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
    BurnOS::liveLog(QStringLiteral("settings: model %1, %2 tokens, hangover %3 ms, "
                                   "sensitivity %7 (opens at %8), "
                                   "voice %4, volume %5, key %6")
                        .arg(BurnOS::Settings::openRouterModel())
                        .arg(BurnOS::Settings::replyTokens())
                        .arg(BurnOS::Settings::hangoverMs())
                        .arg(BurnOS::Settings::ttsVoice())
                        .arg(BurnOS::Settings::ttsVolume())
                        .arg(BurnOS::Settings::hasOpenRouterKey()
                                 ? QStringLiteral("stored")
                                 : QStringLiteral("MISSING"))
                        .arg(BurnOS::Settings::micSensitivity())
                        .arg(BurnOS::Settings::micOpenThreshold(), 0, 'f', 4));

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
    // preference now. It all lives in one place because it has to be redone the
    // moment the settings dialog changes it.
    applyOrbSize();
    restoreOrbPosition();

    // Click once to arm live mode: the microphone stays open, speech starts the
    // recording by itself and 650 ms of quiet ends it. Click again to disarm.
    m_stt = new SttEngine(this);
    m_ai = new OpenRouterClient(this);
    m_tts = new TtsEngine(this);

    connect(m_ai, &OpenRouterClient::toolRequested,
            this, &MainWindow::onToolRequested);

    // EVERYTHING BEYOND THIS MACHINE goes through Composio. computer_use can
    // only touch what has a window on this desktop; sending an email or posting
    // to a channel is somebody else's HTTP API behind somebody else's OAuth, and
    // Composio is what holds those connections and refreshes their tokens.
    //
    // It answers over the network, so the half of a turn that uses an app
    // finishes in these three handlers rather than inside onToolRequested().
    // Each one checks isAwaitingTool() first: this same object also serves the
    // startup refresh below, whose failures belong in the log and nowhere near
    // the conversation.
    m_composio = new ComposioClient(this);

    connect(m_composio, &ComposioClient::toolsFound, this,
            [this](bool ok, const QString &summary) {
                if (m_abort || !m_ai->isAwaitingTool())
                    return;
                if (ok && summary.isEmpty()) {
                    m_ai->finishTool(tr("Nothing Composio can reach does that. "
                                        "Tell the user so in one short sentence."));
                    return;
                }
                BurnOS::liveLog(QStringLiteral("app action search %1: %2")
                                    .arg(ok ? QStringLiteral("answered")
                                            : QStringLiteral("failed"),
                                         summary.simplified().left(160)));
                m_ai->finishTool(summary);
            });

    // A sign in the model asked for. The page is open in the browser now, and
    // the turn ends there: the sign in itself finishes in the browser, and the
    // next question finds the app connected.
    connect(m_composio, &ComposioClient::linkOpened, this, [this](const QString &slug) {
        if (m_abort || !m_ai->isAwaitingTool())
            return;
        m_ai->finishTool(tr("The sign in page for %1 is open in the user's web "
                            "browser now. Tell them in one short sentence to "
                            "finish signing in there and then ask again. Do not "
                            "call any more tools for this question.").arg(slug));
    });

    connect(m_composio, &ComposioClient::noSignInNeeded, this, [this](const QString &slug) {
        if (m_abort || !m_ai->isAwaitingTool())
            return;
        m_ai->finishTool(tr("%1 needs no sign in and can be used right away. Carry "
                            "on with find_tools and run_tool.").arg(slug));
    });

    connect(m_composio, &ComposioClient::toolFinished, this,
            [this](bool ok, const QString &summary) {
                if (m_abort || !m_ai->isAwaitingTool())
                    return;
                BurnOS::liveLog(QStringLiteral("app action %1: %2")
                                    .arg(ok ? QStringLiteral("succeeded")
                                            : QStringLiteral("failed"),
                                         summary.simplified().left(160)));
                m_ai->finishTool(summary);
            });

    connect(m_composio, &ComposioClient::failed, this, [this](const QString &reason) {
        if (m_abort || !m_ai->isAwaitingTool())
            return;
        m_ai->finishTool(tr("That could not be done: %1").arg(reason));
    });

    // Ask Composio once at startup which apps are actually connected, rather
    // than trusting a cached list written who knows when - a connection revoked
    // from the other end would otherwise still be offered to the model. The
    // answer is written back to settings by the client itself.
    if (ComposioClient::isConfigured())
        m_composio->refreshConnections();

    connect(m_orb, &OrbWidget::clicked, this, [this]() {
        if (m_orb->isActive()) {
            if (!m_stt->startLive())
                m_orb->setActive(false);
        } else {
            forceStop();
        }
    });

    connect(m_stt, &SttEngine::levelChanged, m_orb, &OrbWidget::setLevel);

    // The orb brightens while a phrase is being kept, so the detector's opinion
    // is visible without any text.
    connect(m_stt, &SttEngine::speechStarted, this, [this]() {
        m_orb->setCapturing(true);
    });
    connect(m_stt, &SttEngine::speechEnded, this, [this]() {
        m_orb->setCapturing(false);
        // From here until we are listening again the microphone is shut, so the
        // orb must stop saying it is listening.
        m_orb->setThinking(true);
    });

    // The transcription is never shown: only the answer is, and only as it is
    // being spoken.
    // Live mode survives a turn: only a click disarms it. Nothing is listened
    // to while we are answering, or our own voice would become the next
    // question, so the microphone is reopened once the answer is finished.
    connect(m_stt, &SttEngine::transcribed, this, [this](const QString &text) {
        if (text.isEmpty()) {
            listenAgain();
            return;
        }
        // A spoken command is handled here and never reaches the model: saying
        // "open app settings" should open the dialog, not ask an AI about it.
        if (isSettingsCommand(text)) {
            openSettings(); // resumes listening itself once it closes
            return;
        }

        // Still talking. Whatever was asked a moment ago has not been answered
        // out loud yet, so abandon that turn and ask the whole thought at once
        // rather than letting a pause for breath split it in two.
        if (!m_unanswered.isEmpty()) {
            BurnOS::liveLog(QStringLiteral("still talking - carrying the unanswered question forward"));
            m_ai->cancel();
            m_tts->stop();
            m_orb->stopSpeaking();
            m_unanswered += QLatin1Char(' ');
        }
        m_unanswered += text;

        m_orb->setPreparing(false);
        m_orb->setThinking(true);
        routeSpoken(m_unanswered);
    });

    connect(m_ai, &OpenRouterClient::replied, this, [this](const QString &text) {
        // On screen immediately. Synthesising a long answer takes Kokoro the
        // better part of half a minute, and waiting for the voice before
        // showing anything is what made a working reply look like no reply.
        // The words are NOT put on screen here. They appear when the voice
        // actually starts saying them, so the bubble and the speech are always
        // the same thing rather than the text running a long way ahead of it.
        // Kokoro's wait gets a word of its own on the orb instead.
        m_orb->setPreparing(true);
        if (!m_tts->speak(text)) {
            // Nothing is going to say it. The orb shows no text at all, so the
            // answer goes to the log in full rather than being lost, and the
            // orb carries the failure word.
            BurnOS::liveLog(QStringLiteral("UNSPOKEN answer: %1").arg(text));
            listenAgain();
            m_orb->showNotice(text, 12000);
        }
    });

    connect(m_tts, &TtsEngine::speaking, this, [this](const QString &text) {
        // The voice has started. NOW the words appear, revealed in step with it.
        m_orb->setPreparing(false);
        // Answered out loud, so the turn is closed and can no longer be added
        // to. The microphone shuts here, and only here, because up to this
        // moment the user was free to keep talking - but our own voice must
        // never become the next question.
        m_unanswered.clear();
        // The microphone shuts here, and only here, because up to this moment
        // the user was free to keep talking - but our own voice must never
        // become the next question, and there is no echo cancellation.
        m_stt->pauseListening();
        // The orb goes DEEP BLUE and breathes with the voice. No words: the
        // answer is spoken, and the screen only says that it is being spoken.
        BurnOS::liveLog(QStringLiteral("orb deep blue: speaking %1 chars")
                            .arg(text.size()));
        m_orb->setSpeaking(true);
    });
    // The real loudness of the voice, every frame, so the orb breathes with it.
    connect(m_tts, &TtsEngine::level, m_orb, &OrbWidget::setSpeechLevel);
    connect(m_tts, &TtsEngine::finished, this, [this]() {
        m_orb->stopSpeaking();
        listenAgain();
        // A scripted rehearsal carries straight on to its next question.
        if (!m_rehearsal.isEmpty())
            QTimer::singleShot(1200, this, &MainWindow::askNextRehearsal);
    });

    // A failed turn must not kill live mode: say why, then listen again.
    // listenAgain first, then the notice: putting them the other way round
    // cleared the bubble's hold on the hint the instant it was taken.
    connect(m_tts, &TtsEngine::failed, this, [this](const QString &reason) {
        m_unanswered.clear();
        listenAgain();
        showNotice(reason);
    });

    connect(m_ai, &OpenRouterClient::failed, this, [this](const QString &reason) {
        m_unanswered.clear();
        listenAgain();
        showNotice(reason);
    });

    // The microphone itself failing is the one case that does disarm, because
    // there is nothing left to listen with.
    connect(m_stt, &SttEngine::failed, this, [this](const QString &reason) {
        if (!m_stt->isLive())
            m_orb->setActive(false);
        m_orb->setCapturing(false);
        listenAgain();
        showNotice(reason);
    });

    // WHAT A MODEL CAN DO IS A FACT, NOT A GUESS. OpenRouter's own catalogue is
    // fetched once at startup, and if the model in Settings turns out not to be
    // able to see the screen or call tools, Burnos moves itself to one that can
    // rather than failing every turn that checks its own work.
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

    // Proof of the whole answer path without a microphone: the text is injected
    // exactly where a transcription would have arrived, so the model, the voice
    // and the bubble are all the real ones. SEVERAL questions can be separated
    // with a vertical bar and are asked one after another, which is how the
    // conversation memory is tested - the second question is free to refer back
    // to the first. Armed only by the environment, never in a normal run.
    m_rehearsal = qEnvironmentVariable("BURNOS_ASK_SELFTEST")
                      .split(QLatin1Char('|'), Qt::SkipEmptyParts);
    for (QString &line : m_rehearsal)
        line = line.trimmed();
    if (!m_rehearsal.isEmpty())
        QTimer::singleShot(1500, this, &MainWindow::askNextRehearsal);
}

MainWindow::~MainWindow() = default;

void MainWindow::armLiveMode()
{
    if (m_stt->isLive())
        return;

    // The orb is told first, so the colour and the hint change on the same
    // frame the microphone opens; a failure puts it straight back to grey.
    m_abort = false;
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
    // THE WINDOW IS THE ORB, and it never changes size. The answer is said one
    // word at a time in the middle of the orb, where the hint words go, so an
    // answer needs no more room than the orb already has.
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
    const QString chosen = BurnOS::Settings::openRouterModel();
    if (catalogue->fitToRun(chosen))
        return;

    const BurnOS::ModelFacts facts = catalogue->facts(chosen);
    QStringList missing;
    if (!facts.seesImages)
        missing.append(tr("see the screen"));
    if (!facts.callsTools)
        missing.append(tr("call its tools"));

    // The default is chosen to be capable, so it is the safe place to land. If
    // even that has lost a capability since, the first model on the catalogue
    // that can do the job will do: anything is better than one that is going to
    // fail every turn.
    QString replacement = BurnOS::Settings::defaultOpenRouterModel();
    if (!catalogue->fitToRun(replacement)) {
        const QStringList able = catalogue->capable();
        if (able.isEmpty())
            return;
        replacement = able.first();
    }

    BurnOS::Settings::setOpenRouterModel(replacement);
    BurnOS::liveLog(QStringLiteral("MODEL CHANGED: %1 cannot %2, so Burnos moved "
                                   "to %3. Settings says why.")
                        .arg(chosen,
                             missing.join(QStringLiteral(" or ")),
                             replacement));
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

void MainWindow::askNextRehearsal()
{
    if (m_rehearsal.isEmpty())
        return;

    const QString question = m_rehearsal.takeFirst();
    BurnOS::liveLog(QStringLiteral("SELF TEST: asking as if transcribed: %1")
                        .arg(question));
    // Exactly as a real turn does, or the orb hears its own answer come back
    // out of the speakers and the rehearsal argues with itself.
    m_stt->pauseListening();
    m_orb->setThinking(true);
    // Through the SAME router a real sentence goes through, so the rehearsal
    // exercises the computer use skill as well as the chat path.
    m_unanswered = question;
    routeSpoken(question);
}

void MainWindow::routeSpoken(const QString &said)
{
    // What is on screen travels WITH the question, as a system message for
    // this turn only, so the model can name a control instead of guessing at
    // one. It is never remembered: a stale list of controls is worse than none.
    m_abort = false;
    m_toolCalls = 0;
    const QString context = tr("The window in front right now. %1")
                                .arg(describeControls(BurnOS::Computer::foreground()));

    BurnOS::liveLog(QStringLiteral("routing: front window \"%1\", %2 control(s) offered")
                        .arg(m_target.title.isEmpty() ? QStringLiteral("(none)")
                                                      : m_target.title)
                        .arg(m_offered.size()));

    if (!m_ai->ask(said, context)) {
        m_unanswered.clear();
        listenAgain();
    }
}

void MainWindow::onToolRequested(const QString &function, const QVariantMap &args)
{
    // The model asked for a tool, so run it and hand back plainly what
    // happened - including when it failed. It is the model that then says it
    // out loud, so it can phrase a refusal properly instead of us guessing.
    if (m_abort)
        return;

    constexpr int kMaxToolCalls = 8;
    if (++m_toolCalls > kMaxToolCalls) {
        BurnOS::liveLog(QStringLiteral("computer use: STOPPED after %1 calls")
                            .arg(kMaxToolCalls));
        m_ai->finishTool(tr("Stop. You have tried %1 times and it is not working. "
                            "Do not call the tool again for this question - tell "
                            "the user briefly what went wrong.")
                             .arg(kMaxToolCalls));
        return;
    }

    // An app action goes out over the network and answers on a signal, so the
    // turn carries on from the handlers in the constructor. The call cap above
    // still applies to it, which is what stops a model that cannot get a tool's
    // arguments right from trying all afternoon.
    if (function == QStringLiteral("app_action")) {
        runAppAction(args);
        return;
    }

    QByteArray png;
    const QString result = runComputerUse(args, &png);

    // Muted while that was running. The result is thrown away rather than
    // handed back, because handing it back restarts the turn.
    if (m_abort) {
        BurnOS::liveLog(QStringLiteral("computer use: result dropped, muted part "
                                       "way through the action"));
        return;
    }

    BurnOS::liveLog(QStringLiteral("computer use: %1").arg(result));
    m_ai->finishTool(result, png);
}

void MainWindow::runAppAction(const QVariantMap &args)
{
    const QString action = args.value(QStringLiteral("action")).toString();

    if (!ComposioClient::isConfigured()) {
        m_ai->finishTool(tr("Online apps are not set up: there is no Composio "
                            "key. Tell the user to add one in Settings, under "
                            "Manage integrations."));
        return;
    }

    // EVERY APP IS SEARCHED, not only the connected ones. The session says in
    // its answer which apps are connected, an app with no sign in runs as it
    // is, and one that needs a sign in gets connect_app - which is how "email
    // Sarah" can work on the first day without a trip to Settings first.
    if (action == QStringLiteral("find_tools")) {
        const QString query = args.value(QStringLiteral("query")).toString().trimmed();
        if (query.isEmpty()) {
            m_ai->finishTool(tr("find_tools needs a query saying in a sentence "
                                "what you are trying to do and in which app."));
            return;
        }
        BurnOS::liveLog(QStringLiteral("app action: searching for \"%1\"").arg(query));
        m_composio->searchTools(query);
        return;
    }

    if (action == QStringLiteral("run_tool")) {
        const QString tool = args.value(QStringLiteral("tool")).toString().trimmed();
        if (tool.isEmpty()) {
            m_ai->finishTool(tr("run_tool needs the slug of a tool find_tools gave "
                                "you."));
            return;
        }

        // The arguments arrive as an object where the model behaved and as a
        // string of JSON where it did not. BOTH are accepted: a turn thrown away
        // over quoting is a turn the user has to speak all over again.
        const QVariant raw = args.value(QStringLiteral("arguments"));
        QJsonObject arguments;
        if (raw.typeId() == QMetaType::QString) {
            arguments = QJsonDocument::fromJson(raw.toString().toUtf8()).object();
        } else {
            arguments = QJsonObject::fromVariantMap(raw.toMap());
        }

        BurnOS::liveLog(QStringLiteral("app action: running %1 with %2 argument(s)")
                            .arg(tool).arg(arguments.size()));
        m_composio->runTool(tool, arguments);
        return;
    }

    if (action == QStringLiteral("connect_app")) {
        const QString app =
            args.value(QStringLiteral("app")).toString().trimmed().toLower();
        if (app.isEmpty()) {
            m_ai->finishTool(tr("connect_app needs app: the slug of the app to "
                                "connect, as find_tools named it, such as gmail."));
            return;
        }
        BurnOS::liveLog(QStringLiteral("app action: opening the sign in for %1").arg(app));
        m_composio->connectApp(app);
        return;
    }

    m_ai->finishTool(tr("\"%1\" is not something app_action does. Use find_tools "
                        "to look for a tool, run_tool to carry it out, or "
                        "connect_app to sign in to an app.")
                         .arg(action));
}
QString MainWindow::describeControls(const DesktopWindow &target)
{
    m_target = target;
    m_offered = BurnOS::Computer::controls(target.handle);

    if (target.isNull() || target.title.isEmpty()) {
        return tr("There is no window in front, so there is nothing to click or "
                  "type into.");
    }
    if (m_offered.isEmpty()) {
        return tr("\"%1\" offers nothing that can be clicked or typed into. Take a "
                  "screenshot of it and use click_at instead.")
            .arg(target.title);
    }

    QStringList lines;
    for (const DesktopControl &c : std::as_const(m_offered))
        lines.append(QStringLiteral("%1: %2").arg(c.key, c.describe()));
    return tr("\"%1\" has these controls, and the key to give back for each one:\n%2")
        .arg(target.title, lines.join(QLatin1Char('\n')));
}

QString MainWindow::runComputerUse(const QVariantMap &args, QByteArray *png)
{
    const QString action = args.value(QStringLiteral("action")).toString();
    QString error;

    auto namedControl = [this, &args]() -> DesktopControl {
        const QString wanted =
            args.value(QStringLiteral("control")).toString().trimmed();
        for (const DesktopControl &c : std::as_const(m_offered)) {
            if (c.key.compare(wanted, Qt::CaseInsensitive) == 0)
                return c;
        }
        return {};
    };

    // THE LIST OF CONTROLS IS FROM THE START OF THE TURN, and a turn that opens
    // a program has moved the screen on underneath it. When the model names a
    // key that is no longer there, re-read what is in front and hand the NEW
    // list back instead of aiming the action at whatever the old key pointed
    // to. This is what "it will not type into the Store" was, half the time:
    // the Store had only just opened and every key still described the window
    // that was in front before it.
    auto listAgain = [this]() -> QString {
        return tr("Failed: that control is not on screen any more - the screen has "
                  "changed since the list you were given. %1")
            .arg(describeControls(BurnOS::Computer::foreground()));
    };

    if (action == QLatin1String("launch")) {
        const QString name = args.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty())
            return tr("Failed: no program was named.");

        // What is already open, so the new window can be told from the rest.
        QSet<quint64> before;
        for (const DesktopWindow &w : BurnOS::Computer::windows())
            before.insert(w.handle);

        QString opened;
        if (!BurnOS::Computer::launchAnything(name, &opened, &error))
            return tr("Failed: %1").arg(error);
        if (opened.isEmpty())
            opened = name;

        // WAIT FOR THE WINDOW. The model's next move is nearly always to type
        // into what it just opened, and a program takes a second or two to put
        // a window up - so handing back now would hand back the controls of
        // whatever was in front BEFORE, which is half of why typing into a
        // freshly opened Store app never worked. Events keep being served
        // through the wait or the orb freezes while it happens.
        DesktopWindow fresh;
        QElapsedTimer clock;
        clock.start();
        while (fresh.isNull() && !m_abort && clock.elapsed() < 9000) {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
            QThread::msleep(150);
            for (const DesktopWindow &w : BurnOS::Computer::windows()) {
                if (!before.contains(w.handle)) {
                    fresh = w;
                    break;
                }
            }
        }

        if (fresh.isNull()) {
            return tr("%1 was started but it has not put a window up yet. Use "
                      "list_windows in a moment to see whether it came up.")
                .arg(opened);
        }

        // A XAML app reports almost nothing for its first second on screen, so
        // reading its controls the instant the window exists finds an empty
        // shell. Let it finish drawing itself.
        QThread::msleep(900);
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
        return tr("Opened %1. %2").arg(opened, describeControls(fresh));
    }

    if (action == QLatin1String("open_url")) {
        const QString url = args.value(QStringLiteral("url")).toString().trimmed();
        QString opened;
        if (!BurnOS::Computer::openUrl(url, &opened, &error))
            return tr("Failed: %1").arg(error);

        // A page takes a moment to come up and the model's next move is to look
        // at what it opened, so the controls handed back have to be the NEW
        // ones. Events keep being served through the wait, or the orb freezes
        // while it happens.
        QElapsedTimer clock;
        clock.start();
        while (!m_abort && clock.elapsed() < 2600) {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
            QThread::msleep(150);
        }

        const DesktopWindow front = BurnOS::Computer::foreground();
        if (front.isNull())
            return tr("Opened %1 in the user's own browser.").arg(opened);
        return tr("Opened %1 in the browser the user already had open, so they "
                  "are signed in there as themselves. %2")
            .arg(opened, describeControls(front));
    }

    if (action == QLatin1String("run_command")) {
        const QString command =
            args.value(QStringLiteral("command")).toString().trimmed();
        QString printed;
        if (BurnOS::Computer::runCommand(command, &printed, &error))
            return printed;
        return tr("Failed: %1").arg(error);
    }

    if (action == QLatin1String("list_windows")) {
        const QList<DesktopWindow> open = BurnOS::Computer::windows();
        if (open.isEmpty())
            return tr("Nothing is open.");
        QStringList titles;
        for (const DesktopWindow &w : open)
            titles.append(w.title);
        return tr("%1 windows are open: %2")
            .arg(open.size())
            .arg(titles.join(QStringLiteral("; ")));
    }

    if (action == QLatin1String("screenshot")) {
        const QString wanted =
            args.value(QStringLiteral("window")).toString().trimmed();
        const DesktopWindow target =
            wanted.isEmpty() ? BurnOS::Computer::foreground()
                             : BurnOS::Computer::findWindow(wanted);
        if (target.isNull())
            return tr("Failed: I could not find that window.");

        const WindowShot shot = BurnOS::Computer::capture(target.handle);
        if (shot.isNull())
            return tr("Failed: that window would not be captured.");

        m_shotWindow = target.handle;
        m_shotScale = shot.scale;
        if (png)
            *png = shot.png;

        return tr("Screenshot of \"%1\", %2 by %3 pixels.")
            .arg(target.title)
            .arg(shot.width)
            .arg(shot.height);
    }

    if (action == QLatin1String("click_at")) {
        if (m_shotWindow == 0) {
            return tr("Failed: take a screenshot first, so there is a picture to "
                      "pick a point out of.");
        }
        const int pictureX = args.value(QStringLiteral("x")).toInt();
        const int pictureY = args.value(QStringLiteral("y")).toInt();

        // Back out of the picture's pixels into the window's own.
        const int windowX = int(pictureX / m_shotScale);
        const int windowY = int(pictureY / m_shotScale);

        if (BurnOS::Computer::clickAt(m_shotWindow, windowX, windowY, &error))
            return tr("Clicked at %1, %2 in that screenshot.").arg(pictureX).arg(pictureY);
        return tr("Failed: %1").arg(error);
    }

    if (action == QLatin1String("list_controls")) {
        const QString wanted =
            args.value(QStringLiteral("window")).toString().trimmed();
        const DesktopWindow target =
            wanted.isEmpty() ? BurnOS::Computer::foreground()
                             : BurnOS::Computer::findWindow(wanted);
        if (target.isNull())
            return tr("Failed: I could not find that window.");
        return describeControls(target);
    }

    if (action == QLatin1String("click")) {
        const DesktopControl target = namedControl();
        if (target.key.isEmpty())
            return listAgain();
        if (BurnOS::Computer::click(m_target.handle, target, &error)) {
            return tr("Clicked %1.")
                .arg(target.name.isEmpty() ? target.type : target.name);
        }
        return tr("Failed: %1").arg(error);
    }

    if (action == QLatin1String("press_key")) {
        const QString key =
            args.value(QStringLiteral("key")).toString().trimmed().toLower();
        if (BurnOS::Computer::pressKey(key, &error))
            return tr("Pressed %1.").arg(key);
        return tr("Failed: %1").arg(error);
    }

    if (action == QLatin1String("type_text")) {
        const QString text = args.value(QStringLiteral("text")).toString();
        const DesktopControl target = namedControl();
        if (text.isEmpty())
            return tr("Failed: no text was given.");
        if (target.key.isEmpty())
            return listAgain();
        if (BurnOS::Computer::fill(m_target.handle, target, text, &error))
            return tr("Typed it in.");
        return tr("Failed: %1").arg(error);
    }

    return tr("Failed: %1 is not something I can do.").arg(action);
}



void MainWindow::listenAgain()
{
    m_orb->setThinking(false);
    m_orb->setPreparing(false);

    // MUTED MEANS MUTED. A turn that finishes after the orb was clicked off -
    // a reply that lands, a voice that stops, a tool that gives up - must not
    // quietly reopen the microphone behind a grey orb.
    if (!m_orb->isActive())
        return;

    m_stt->resumeListening();
}

void MainWindow::forceStop()
{
    // MUTING STOPS EVERYTHING, AT ONCE, and "everything" is longer than it
    // looks: by the middle of a turn there is a microphone open, a request in
    // flight, possibly a tool action part way through a wait, a Kokoro child
    // process synthesising, a voice playing, words on screen and a hint on the
    // orb. Leaving any one of them running is what made a muted orb carry on
    // talking seconds later.
    m_abort = true;

    m_stt->stopLive();
    m_tts->stop();
    m_ai->cancel();

    m_unanswered.clear();
    m_toolCalls = 0;
    // A scripted rehearsal is stopped too - mute is mute, even in a self test.
    m_rehearsal.clear();

    // THE HINT HAS TO BE PUT BACK BY HAND. Listening, Thinking and Preparing
    // are all states the orb can be sitting in when it is muted, and none of
    // them clear themselves. The words go last, because dropping them is what
    // gives the orb its hint back and that has to happen after the rest.
    m_orb->setCapturing(false);
    m_orb->setThinking(false);
    m_orb->setPreparing(false);
    m_orb->stopSpeaking();

    BurnOS::liveLog(QStringLiteral("muted: microphone, request, voice and caption "
                                   "all stopped"));
}

void MainWindow::openSettings()
{
    if (m_settingsOpen)
        return;
    m_settingsOpen = true;

    // Nothing should be heard while a key is being typed, and the orb must not
    // be talking over the dialog either. Live mode is only paused here, never
    // disarmed, so the orb stays blue and picks up again afterwards.
    const bool wasLive = m_stt->isLive();
    m_stt->pauseListening();
    m_tts->stop();
    m_orb->stopSpeaking();

    SettingsDialog dialog(this);

    // LIVE, while it is open. There is no Save button on that dialog: every
    // setting is written down as it is changed, and the two that live on this
    // window rather than in an engine arrive here as they change, so the orb
    // grows under the slider instead of after the dialog closes.
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

    // Both are already as the user left them; these are the reconciliation,
    // and applyOrbSize with no size is what finally logs the one it settled on.
    applyAlwaysOnTop();
    applyOrbSize();
    // An app may have been connected or dropped in Manage integrations, and
    // what the model is told it can reach comes from that list.
    if (ComposioClient::isConfigured())
        m_composio->refreshConnections();
    m_settingsOpen = false;

    if (wasLive)
        listenAgain();
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
    // This window is 460 pixels wide and grows downwards, because the answer is
    // shown whole and word wrapped under the orb. All of that is painted, none
    // of it is furniture: without this the desktop carries a large invisible
    // box that eats every click near the orb and can be dragged from anywhere.
    // HTTRANSPARENT hands the click to whatever is behind instead.
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
