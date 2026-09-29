#include "livelog.h"
#include "settings.h"
#include "ttsengine.h"
#include "sttpaths.h"

#include <QAudioDevice>
#include <QAudioOutput>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QtEndian>

#include <cmath>

namespace {
// Which of the eleven voices in kokoro-en-v0_19 speaks, and how loudly, are
// settings now; only the thread count is fixed here.
constexpr int kThreads = 4;
}

TtsEngine::TtsEngine(QObject *parent)
    : QObject(parent)
{
}

TtsEngine::~TtsEngine() = default;

QString TtsEngine::cliPath() const
{
    // Its own folder, so Windows finds onnxruntime.dll next to it.
    return QCoreApplication::applicationDirPath()
           + QStringLiteral("/kokoro-tts/sherpa-onnx-offline-tts.exe");
}

bool TtsEngine::speak(const QString &text)
{
    const QString spoken = text.trimmed();
    if (spoken.isEmpty()) {
        emit failed(tr("Nothing to say."));
        return false;
    }

    if (m_busy) {
        stop(); // a newer answer always wins over one still being spoken
    }

    // MADE ALREADY: said at once, with no synthesis to wait for.
    const QString cached = cachePath(spoken);
    if (QFileInfo::exists(cached)) {
        m_text = spoken;
        m_busy = true;
        emit started();
        BurnOS::liveLog(QStringLiteral("kokoro skipped: '%1' was made already")
                            .arg(spoken));
        play(cached);
        return true;
    }

    if (!QFileInfo::exists(cliPath())) {
        emit failed(tr("sherpa-onnx-offline-tts.exe is missing from the kokoro-tts folder."));
        return false;
    }

    if (!BurnOS::Tts::ready()) {
        emit failed(tr("The Kokoro model is not in %1")
                        .arg(QDir::toNativeSeparators(BurnOS::Tts::modelDir())));
        return false;
    }

    m_text = spoken;

    // A fresh file each time: the player keeps a handle on the previous one.
    m_wavPath = QDir::temp().filePath(
        QStringLiteral("burnos-tts-%1.wav").arg(++m_counter));

    if (!m_proc) {
        m_proc = new QProcess(this);
        connect(m_proc, &QProcess::finished, this,
                [this](int code, QProcess::ExitStatus) {
                    if (m_cancelled) {
                        BurnOS::liveLog(QStringLiteral("kokoro cancelled"));
                        return;
                    }
                    if (code != 0) {
                        m_busy = false;
                        const QString err =
                            QString::fromUtf8(m_proc->readAllStandardError())
                                .simplified()
                                .left(200);
                        BurnOS::liveLog(QStringLiteral("kokoro exit %1: %2")
                                            .arg(code)
                                            .arg(err));
                        emit failed(tr("Kokoro exited with %1. %2").arg(code).arg(err));
                        return;
                    }
                    BurnOS::liveLog(QStringLiteral("kokoro wrote %1 bytes of wav")
                                        .arg(QFileInfo(m_wavPath).size()));
                    play(m_wavPath);
                });
    }

    const QStringList args{
        QStringLiteral("--kokoro-model=") + BurnOS::Tts::modelPath(),
        QStringLiteral("--kokoro-voices=") + BurnOS::Tts::voicesPath(),
        QStringLiteral("--kokoro-tokens=") + BurnOS::Tts::tokensPath(),
        QStringLiteral("--kokoro-data-dir=") + BurnOS::Tts::espeakDataDir(),
        QStringLiteral("--num-threads=%1").arg(kThreads),
        QStringLiteral("--sid=%1").arg(BurnOS::Settings::ttsVoice()),
        QStringLiteral("--output-filename=") + m_wavPath,
        spoken,
    };

    BurnOS::liveLog(QStringLiteral("kokoro synthesising %1 chars with voice %2")
                        .arg(spoken.size())
                        .arg(BurnOS::Settings::ttsVoice()));

    m_busy = true;
    emit started();
    m_proc->setWorkingDirectory(QFileInfo(cliPath()).absolutePath());
    m_proc->start(cliPath(), args);
    return true;
}

void TtsEngine::play(const QString &wavPath)
{
    if (!m_player) {
        m_player = new QMediaPlayer(this);
        m_audio = new QAudioOutput(this);
        m_player->setAudioOutput(m_audio);

        connect(m_player, &QMediaPlayer::playbackStateChanged, this,
                [this](QMediaPlayer::PlaybackState state) {
                    if (state == QMediaPlayer::StoppedState) {
                        stopMeter();
                        if (m_busy) {
                            m_busy = false;
                            BurnOS::liveLog(QStringLiteral("playback finished"));
                            emit finished();
                        }
                    }
                });

        connect(m_player, &QMediaPlayer::errorOccurred, this,
                [this](QMediaPlayer::Error, const QString &message) {
                    stopMeter();
                    m_busy = false;
                    BurnOS::liveLog(QStringLiteral("playback ERROR: %1").arg(message));
                    emit failed(tr("Playback failed: %1").arg(message));
                });

        connect(m_player, &QMediaPlayer::positionChanged, this,
                [this](qint64 position) {
                    // The player only says where it is every few dozen
                    // milliseconds. Remember when it last said, and the meter
                    // fills in the gap with a clock.
                    m_lastPosMs = position;
                    m_sincePos.restart();
                    const qint64 total = m_player->duration();
                    if (total > 0)
                        emit progress(qreal(position) / qreal(total));
                });

        // The loudness meter: the orb wants a level every frame, and a voice
        // changes far too quickly for the player's own position reports.
        m_meter = new QTimer(this);
        m_meter->setInterval(16);
        connect(m_meter, &QTimer::timeout, this, [this]() {
            if (m_envelope.isEmpty())
                return;
            qint64 posMs = m_lastPosMs;
            if (m_sincePos.isValid())
                posMs += m_sincePos.elapsed();
            const int i = int(posMs / 10);
            emit level(i >= 0 && i < m_envelope.size() ? m_envelope.at(i)
                                                       : qreal(0.0));
        });
    }

    loadEnvelope(wavPath);
    m_lastPosMs = 0;
    m_sincePos.restart();
    m_meter->start();

    // Read at playback, so a change in the settings dialog applies to the next
    // answer without restarting anything.
    m_audio->setVolume(qreal(BurnOS::Settings::ttsVolume()) / 100.0);
    m_player->setSource(QUrl::fromLocalFile(wavPath));
    emit speaking(m_text);
    m_player->play();

    BurnOS::liveLog(QStringLiteral("playing at %1 percent through %2")
                        .arg(BurnOS::Settings::ttsVolume())
                        .arg(m_audio->device().description()));
}

void TtsEngine::stopMeter()
{
    if (m_meter && m_meter->isActive()) {
        m_meter->stop();
        // A last zero, so the orb is not left holding whatever the voice was
        // doing on its final frame.
        emit level(0.0);
    }
}

void TtsEngine::loadEnvelope(const QString &wavPath)
{
    // Plain RIFF WAVE, 16 bit PCM, which is the only thing sherpa-onnx writes.
    // Walked chunk by chunk rather than assuming a 44 byte header, because a
    // LIST chunk in front of the data is common and would otherwise be read as
    // very loud noise.
    m_envelope.clear();

    QFile f(wavPath);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray bytes = f.readAll();
    if (bytes.size() < 12 || bytes.left(4) != "RIFF" || bytes.mid(8, 4) != "WAVE")
        return;

    const uchar *d = reinterpret_cast<const uchar *>(bytes.constData());
    int channels = 1;
    int rate = 24000;
    int bits = 16;
    int dataStart = -1;
    int dataLen = 0;

    int pos = 12;
    while (pos + 8 <= bytes.size()) {
        const QByteArray id = bytes.mid(pos, 4);
        const quint32 len = qFromLittleEndian<quint32>(d + pos + 4);
        pos += 8;
        if (id == "fmt " && len >= 16 && pos + 16 <= bytes.size()) {
            channels = qFromLittleEndian<quint16>(d + pos + 2);
            rate = int(qFromLittleEndian<quint32>(d + pos + 4));
            bits = qFromLittleEndian<quint16>(d + pos + 14);
        } else if (id == "data") {
            dataStart = pos;
            dataLen = int(qMin<qint64>(len, bytes.size() - pos));
            break;
        }
        pos += int(len + (len & 1));
    }
    if (dataStart < 0 || bits != 16 || channels < 1 || rate <= 0)
        return;

    // RMS over every 10 ms, first channel only - the voice is mono anyway.
    const int frames = dataLen / (2 * channels);
    const int perBin = qMax(1, rate / 100);
    float peak = 0.0f;
    for (int start = 0; start < frames; start += perBin) {
        const int end = qMin(frames, start + perBin);
        double acc = 0.0;
        for (int i = start; i < end; ++i) {
            const double v = qFromLittleEndian<qint16>(
                                 d + dataStart + qint64(i) * channels * 2)
                             / 32768.0;
            acc += v * v;
        }
        const float rms = float(std::sqrt(acc / qMax(1, end - start)));
        m_envelope.append(rms);
        peak = qMax(peak, rms);
    }

    // Normalised to the loudest moment and lifted with a gentle curve, so the
    // quieter syllables still move the orb and the room noise floor does not.
    if (peak > 0.0f) {
        for (float &v : m_envelope) {
            const float x = qMax(0.0f, v / peak - 0.05f) / 0.95f;
            v = std::pow(x, 0.65f);
        }
    }

    BurnOS::liveLog(QStringLiteral("voice envelope: %1 bins of 10 ms, %2 Hz")
                        .arg(m_envelope.size())
                        .arg(rate));
}

void TtsEngine::stop()
{
    stopMeter();
    // Both flags go down FIRST. Killing the process fires its finished signal
    // synchronously inside waitForFinished, and stopping the player fires a
    // state change; neither is a failure or a completed answer here.
    m_cancelled = true;
    m_busy = false;

    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
        m_proc->waitForFinished(2000);
    }
    if (m_player)
        m_player->stop();

    m_cancelled = false;
}

void TtsEngine::prepare(const QStringList &phrases)
{
    for (const QString &phrase : phrases) {
        if (!QFileInfo::exists(cachePath(phrase)) && !m_toWarm.contains(phrase))
            m_toWarm.append(phrase);
    }
    if (!m_warm || m_warm->state() == QProcess::NotRunning)
        warmNext();
}

void TtsEngine::warmNext()
{
    while (!m_toWarm.isEmpty() && QFileInfo::exists(cachePath(m_toWarm.first())))
        m_toWarm.removeFirst();
    if (m_toWarm.isEmpty() || !QFileInfo::exists(cliPath()) || !BurnOS::Tts::ready())
        return;

    const QString phrase = m_toWarm.takeFirst();
    m_warmTarget = cachePath(phrase);
    QDir().mkpath(QFileInfo(m_warmTarget).absolutePath());

    if (!m_warm) {
        m_warm = new QProcess(this);
        connect(m_warm, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            // Written under another name and renamed only once complete, so a
            // half written file can never be played as a reply.
            const QString part = m_warmTarget + QStringLiteral(".part.wav");
            if (code == 0 && QFileInfo(part).size() > 44) {
                QFile::remove(m_warmTarget);
                QFile::rename(part, m_warmTarget);
                BurnOS::liveLog(QStringLiteral("voice cache: ready %1")
                                    .arg(QFileInfo(m_warmTarget).fileName()));
            } else {
                QFile::remove(part);
                BurnOS::liveLog(QStringLiteral("voice cache: kokoro exit %1").arg(code));
            }
            warmNext();
        });
    }

    const QStringList args{
        QStringLiteral("--kokoro-model=") + BurnOS::Tts::modelPath(),
        QStringLiteral("--kokoro-voices=") + BurnOS::Tts::voicesPath(),
        QStringLiteral("--kokoro-tokens=") + BurnOS::Tts::tokensPath(),
        QStringLiteral("--kokoro-data-dir=") + BurnOS::Tts::espeakDataDir(),
        QStringLiteral("--num-threads=2"),
        QStringLiteral("--sid=%1").arg(BurnOS::Settings::ttsVoice()),
        QStringLiteral("--output-filename=") + m_warmTarget + QStringLiteral(".part.wav"),
        phrase,
    };
    BurnOS::liveLog(QStringLiteral("voice cache: making '%1' with voice %2")
                        .arg(phrase)
                        .arg(BurnOS::Settings::ttsVoice()));
    m_warm->setWorkingDirectory(QFileInfo(cliPath()).absolutePath());
    m_warm->start(cliPath(), args);
}

QString TtsEngine::cachePath(const QString &text) const
{
    // Case and punctuation do not change how a short reply sounds, so "On it!"
    // plays the "On it." already made.
    QString key;
    for (const QChar c : text.toLower()) {
        if (c.isLetterOrNumber() || c.isSpace())
            key.append(c);
    }
    key = key.simplified();
    const QByteArray id = QCryptographicHash::hash(
                              QStringLiteral("%1|%2")
                                  .arg(BurnOS::Settings::ttsVoice())
                                  .arg(key)
                                  .toUtf8(),
                              QCryptographicHash::Sha1)
                              .toHex()
                              .left(20);
    return QDir::temp().filePath(QStringLiteral("burnos-voice-cache/%1.wav")
                                     .arg(QString::fromLatin1(id)));
}
