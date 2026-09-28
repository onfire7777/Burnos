#include "livelog.h"
#include "settings.h"
#include "sttengine.h"
#include "sttpaths.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMediaDevices>
#include <QProcess>
#include <QTimer>
#include <QtMath>

namespace {
// What the recogniser wants: 16 kHz, mono, signed 16 bit.
constexpr int kSampleRate = 16000;
constexpr int kBytesPerSample = 2;
constexpr qint64 kBytesPerSecond = qint64(kSampleRate) * kBytesPerSample;

// How much quiet ends a phrase is a setting now, because it depends entirely
// on how the speaker paces a sentence. See BurnOS::Settings::hangoverMs.
// Always kept in front of a phrase, because loudness is only noticed once the
// first syllable has already gone past.
constexpr int kPrerollMs = 320;
// Less voiced audio than this was a cough, a door or a keyboard.
constexpr int kMinSpeechMs = 350;
// How long it has to STAY loud before it counts as somebody talking. A door, a
// cough or a keystroke is loud for an instant; a voice is not. This is what
// keeps quiet room noise from arming the orb, rather than simply setting the
// threshold so high that quiet speech is missed with it. The pre-roll is
// longer than this, so nothing said during the attack is lost.
constexpr int kAttackMs = 140;
// Nobody asks a question for half a minute. This stops a noisy room from
// recording for ever.
constexpr int kMaxPhraseMs = 30000;
// The floor under the adaptive threshold is a setting now - see
// BurnOS::Settings::micOpenThreshold. It was briefly hardcoded at 0.030 and the
// user had to shout: the ATTACK below is what rejects noise, so this does not
// have to be high and should not be.

qint64 msToBytes(int ms)
{
    return kBytesPerSecond * ms / 1000;
}

int bytesToMs(qint64 bytes)
{
    return static_cast<int>(bytes * 1000 / kBytesPerSecond);
}
} // namespace

AudioLevelTap::AudioLevelTap(QObject *parent)
    : QIODevice(parent)
{
    open(QIODevice::WriteOnly);
}

qint64 AudioLevelTap::readData(char *, qint64)
{
    return -1; // write only
}

qint64 AudioLevelTap::writeData(const char *data, qint64 size)
{
    const qint16 *samples = reinterpret_cast<const qint16 *>(data);
    const int count = static_cast<int>(size / 2);

    int peak = 0;
    qreal square = 0.0;
    for (int i = 0; i < count; ++i) {
        const qreal s = samples[i];
        square += s * s;
        const int v = qAbs(static_cast<int>(samples[i]));
        if (v > peak)
            peak = v;
    }

    if (count > 0) {
        // Deliberately hot. A raw linear level barely moves for normal speech,
        // so the curve is bent hard to make quiet talking still show.
        const qreal norm = peak / 32768.0;
        emit levelChanged(qBound(0.0, qPow(norm, 0.45) * 1.8, 1.0));

        // The detector gets RMS instead, which one keyboard tap cannot fake
        // the way it fakes a peak.
        emit audioChunk(QByteArray(data, static_cast<qsizetype>(size)),
                        qSqrt(square / count) / 32768.0);
    }

    return size;
}

SttEngine::SttEngine(QObject *parent)
    : QObject(parent)
{
}

SttEngine::~SttEngine() = default;

QString SttEngine::cliPath() const
{
    // The CLI sits in its own folder so Windows resolves its DLLs from there.
    return QCoreApplication::applicationDirPath()
           + QStringLiteral("/parakeet-streaming/parakeet-cli.exe");
}

bool SttEngine::startLive()
{
    if (m_live)
        return true;

    if (!BurnOS::Stt::modelPresent()) {
        emit failed(tr("No model at %1")
                        .arg(QDir::toNativeSeparators(BurnOS::Stt::modelPath())));
        return false;
    }

    if (!QFileInfo::exists(cliPath())) {
        emit failed(tr("parakeet-cli.exe is missing from the parakeet-streaming folder."));
        return false;
    }

    if (!openMicrophone())
        return false;

    m_live = true;
    BurnOS::liveLog(QStringLiteral("live mode ARMED (orb blue), hangover %1 ms, preroll %2 ms")
                        .arg(BurnOS::Settings::hangoverMs())
                        .arg(kPrerollMs));

    if (qEnvironmentVariableIsSet("BURNOS_VAD_SELFTEST"))
        QTimer::singleShot(0, this, &SttEngine::selfTest);

    return true;
}

void SttEngine::stopLive()
{
    if (!m_live)
        return;

    m_live = false;
    closeMicrophone();
    m_speech.clear();
    m_preroll.clear();
    m_queue.clear();
    if (m_capturing) {
        m_capturing = false;
        emit speechEnded();
    }

    BurnOS::liveLog(QStringLiteral("live mode DISARMED (orb grey)"));
}

void SttEngine::pauseListening()
{
    if (!m_source)
        return;

    closeMicrophone();
    m_speech.clear();
    m_preroll.clear();
    m_queue.clear();
    m_silenceBytes = 0;
    m_voicedBytes = 0;
    m_attackBytes = 0;
    BurnOS::liveLog(QStringLiteral("listening paused while we answer"));
}

bool SttEngine::resumeListening()
{
    if (!m_live)
        return false;
    if (m_source)
        return true;

    if (!openMicrophone())
        return false;

    BurnOS::liveLog(QStringLiteral("listening again"));
    return true;
}

bool SttEngine::openMicrophone()
{
    closeMicrophone();

    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        emit failed(tr("No microphone is available."));
        return false;
    }

    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(format)) {
        emit failed(tr("%1 will not record 16 kHz mono 16 bit.")
                        .arg(device.description()));
        return false;
    }

    m_tap = new AudioLevelTap(this);
    connect(m_tap, &AudioLevelTap::levelChanged,
            this, &SttEngine::levelChanged);
    connect(m_tap, &AudioLevelTap::audioChunk,
            this, &SttEngine::onAudio);

    m_source = new QAudioSource(device, format, this);
    m_source->start(m_tap);

    // A device that claims the format and then delivers nothing must not look
    // like an armed, listening orb, so its own state is watched too.
    connect(m_source, &QAudioSource::stateChanged, this, [this]() {
        if (!m_source)
            return;
        if (m_source->error() != QAudio::NoError) {
            BurnOS::liveLog(QStringLiteral("microphone error %1, state %2")
                                .arg(static_cast<int>(m_source->error()))
                                .arg(static_cast<int>(m_source->state())));
            emit failed(tr("The microphone stopped: error %1.")
                            .arg(static_cast<int>(m_source->error())));
        }
    });

    m_speech.clear();
    m_preroll.clear();
    m_capturing = false;
    m_silenceBytes = 0;
    m_voicedBytes = 0;
    m_attackBytes = 0;
    m_idleBytes = 0;
    m_beats = 0;
    m_floor = 0.004;

    BurnOS::liveLog(QStringLiteral("microphone open: %1").arg(device.description()));
    return true;
}

void SttEngine::closeMicrophone()
{
    if (m_source) {
        m_source->stop();
        m_source->deleteLater();
        m_source = nullptr;
    }
    if (m_tap) {
        m_tap->disconnect(this);
        m_tap->deleteLater();
        m_tap = nullptr;
    }
    emit levelChanged(0.0);
}

void SttEngine::onAudio(const QByteArray &pcm, qreal rms)
{
    if (!m_live || !m_source)
        return;

    // The thresholds ride on top of whatever the room is doing, with a floor
    // under them for a room that is genuinely silent.
    // Read every time, so a change in the settings dialog applies to the very
    // next syllable. The adaptive part still rides on whatever the room is
    // doing; the setting is only the floor under it.
    const qreal quietest = BurnOS::Settings::micOpenThreshold();
    const qreal openAt = qMax(quietest, m_floor * 4.5);
    const qreal closeAt = qMax(quietest * 0.55, m_floor * 2.5);

    if (!m_capturing) {
        // A short tail is always kept, so a phrase never starts clipped. It is
        // longer than the attack below, so the run up is inside it either way.
        m_preroll.append(pcm);
        const qint64 keep = msToBytes(kPrerollMs);
        if (m_preroll.size() > keep)
            m_preroll.remove(0, static_cast<qsizetype>(m_preroll.size() - keep));

        if (rms < openAt) {
            // Learn the room, but only out of audio nobody is talking over.
            m_floor = m_floor * 0.95 + rms * 0.05;
            m_attackBytes = 0;

            // The first few seconds of every open microphone are traced, so a
            // run can be proved to have measured the room at all. After that it
            // goes quiet, because a log growing all day proves nothing.
            m_idleBytes += pcm.size();
            if (m_beats < 5 && m_idleBytes >= msToBytes(1000)) {
                m_idleBytes = 0;
                ++m_beats;
                BurnOS::liveLog(QStringLiteral("listening: rms %1, floor %2, opens at %3")
                                    .arg(rms, 0, 'f', 4)
                                    .arg(m_floor, 0, 'f', 4)
                                    .arg(openAt, 0, 'f', 4));
            }
            return;
        }

        // Loud enough - but a door, a cough or a keystroke is loud for an
        // instant too. It has to STAY loud before anyone is deemed to be
        // talking, which throws out a spike without having to set the bar so
        // high that quiet speech goes with it.
        m_attackBytes += pcm.size();
        if (m_attackBytes < msToBytes(kAttackMs))
            return;

        m_capturing = true;
        m_speech = m_preroll; // the pre-roll already holds the whole attack
        m_preroll.clear();
        m_voicedBytes = m_attackBytes;
        m_silenceBytes = 0;
        m_attackBytes = 0;

        BurnOS::liveLog(QStringLiteral("speech STARTED, rms %1 over %2 (floor %3)")
                            .arg(rms, 0, 'f', 4)
                            .arg(openAt, 0, 'f', 4)
                            .arg(m_floor, 0, 'f', 4));
        emit speechStarted();
        return;
    }

    m_speech.append(pcm);
    if (rms >= closeAt) {
        m_voicedBytes += pcm.size();
        m_silenceBytes = 0;
    } else {
        m_silenceBytes += pcm.size();
    }

    // Read every time, so a change in the settings dialog applies to the very
    // next phrase without restarting anything.
    const int hangoverMs = BurnOS::Settings::hangoverMs();
    const bool quietEnough = m_silenceBytes >= msToBytes(hangoverMs);
    const bool tooLong = m_speech.size() >= msToBytes(kMaxPhraseMs);
    if (!quietEnough && !tooLong)
        return;

    const QByteArray phrase = m_speech;
    const int voicedMs = bytesToMs(m_voicedBytes);
    m_speech.clear();
    m_capturing = false;
    m_silenceBytes = 0;
    m_voicedBytes = 0;
    emit speechEnded();

    if (voicedMs < kMinSpeechMs) {
        BurnOS::liveLog(QStringLiteral("discarded: only %1 ms of voice").arg(voicedMs));
        return;
    }

    BurnOS::liveLog(QStringLiteral("speech ENDED after %1 ms of quiet, %2 ms of voice%3")
                        .arg(tooLong ? 0 : hangoverMs)
                        .arg(voicedMs)
                        .arg(tooLong ? QStringLiteral(" (length cap)") : QString()));

    // What follows tears the microphone down, and we are inside its own write
    // call, so it happens on the next turn of the event loop instead.
    // The microphone deliberately STAYS OPEN. The user is allowed to carry on
    // talking while we work, and whatever they add gets folded into the same
    // question - MainWindow does that part. Still deferred by one turn of the
    // event loop, because we are inside the audio device's own write call.
    QTimer::singleShot(0, this, [this, phrase]() { transcribe(phrase); });
}

void SttEngine::selfTest()
{
    // 20 ms buffers, the size a real microphone hands over, because every
    // decision in the detector is counted in bytes rather than in clock time.
    const int chunk = 640;
    QByteArray loud(chunk, Qt::Uninitialized);
    QByteArray quiet(chunk, '\0');
    qint16 *s = reinterpret_cast<qint16 *>(loud.data());
    for (int i = 0; i < chunk / 2; ++i)
        s[i] = static_cast<qint16>(8000.0 * qSin(2.0 * M_PI * 220.0 * i / kSampleRate));

    BurnOS::liveLog(QStringLiteral("SELF TEST: 900 ms of tone at rms 0.17, then silence"));
    for (int i = 0; i < 45 && m_live; ++i)
        onAudio(loud, 0.17);
    for (int i = 0; i < 50 && m_live; ++i)
        onAudio(quiet, 0.0);
    BurnOS::liveLog(QStringLiteral("SELF TEST: fed 1900 ms in total"));
}

void SttEngine::transcribe(const QByteArray &pcm)
{
    if (m_pending) {
        // One recogniser at a time. Now that the microphone stays open while we
        // work, a second phrase can arrive before the first has been read, and
        // it waits its turn rather than overwriting the wav that is still open.
        m_queue.append(pcm);
        BurnOS::liveLog(QStringLiteral("queued %1 ms behind the recogniser")
                            .arg(bytesToMs(pcm.size())));
        return;
    }

    m_wavPath = QDir::temp().filePath(QStringLiteral("burnos-capture.wav"));

    QString error;
    if (!writeWav(pcm, m_wavPath, &error)) {
        BurnOS::liveLog(QStringLiteral("wav failed: %1").arg(error));
        emit failed(error);
        drainQueue();
        return;
    }

    if (!m_proc) {
        m_proc = new QProcess(this);
        connect(m_proc, &QProcess::finished, this,
                [this](int code, QProcess::ExitStatus) {
                    m_pending = false;
                    const QString out =
                        QString::fromUtf8(m_proc->readAllStandardOutput()).trimmed();
                    if (code != 0) {
                        const QString err =
                            QString::fromUtf8(m_proc->readAllStandardError()).trimmed();
                        BurnOS::liveLog(QStringLiteral("parakeet-cli exit %1").arg(code));
                        emit failed(tr("parakeet-cli exited with %1. %2")
                                        .arg(code)
                                        .arg(err.left(200)));
                        drainQueue();
                        return;
                    }
                    BurnOS::liveLog(QStringLiteral("transcribed: %1")
                                        .arg(out.isEmpty() ? QStringLiteral("(nothing)") : out));
                    emit transcribed(out);
                    drainQueue();
                });
    }

    m_pending = true;

    BurnOS::liveLog(QStringLiteral("transcribing %1 ms of audio").arg(bytesToMs(pcm.size())));

    // -ng because there is no GPU here and probing for one costs seconds.
    // -np so stdout carries the transcription and nothing else.
    m_proc->setWorkingDirectory(QFileInfo(cliPath()).absolutePath());
    m_proc->start(cliPath(), {QStringLiteral("-m"), BurnOS::Stt::modelPath(),
                              QStringLiteral("-ng"),
                              QStringLiteral("-np"),
                              QStringLiteral("-f"), m_wavPath});
}

void SttEngine::drainQueue()
{
    if (m_pending || m_queue.isEmpty())
        return;
    transcribe(m_queue.takeFirst());
}

bool SttEngine::writeWav(const QByteArray &pcm, const QString &wavPath,
                         QString *error) const
{
    if (pcm.size() < kSampleRate / 4 * kBytesPerSample) {
        if (error)
            *error = tr("Too short to transcribe.");
        return false;
    }

    QFile wav(wavPath);
    if (!wav.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = tr("Cannot write the WAV file.");
        return false;
    }

    const quint32 dataSize = static_cast<quint32>(pcm.size());
    QDataStream s(&wav);
    s.setByteOrder(QDataStream::LittleEndian);

    s.writeRawData("RIFF", 4);
    s << quint32(36 + dataSize);
    s.writeRawData("WAVE", 4);
    s.writeRawData("fmt ", 4);
    s << quint32(16) << quint16(1) << quint16(1) << quint32(kSampleRate)
      << quint32(kSampleRate * kBytesPerSample) << quint16(kBytesPerSample)
      << quint16(16);
    s.writeRawData("data", 4);
    s << dataSize;
    s.writeRawData(pcm.constData(), static_cast<int>(pcm.size()));

    wav.close();
    return true;
}
