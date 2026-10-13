#include "installer.h"
#include "livelog.h"
#include "settings.h"
#include "ttsengine.h"

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

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThreadPool>

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

// Last, and only for loading the voice's library from its own folder.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace {

// THE VOICE, KEPT LOADED. sherpa-onnx-offline-tts.exe reads the 330 MB Kokoro
// model in again for every answer - a second before the first syllable is even
// worked on. The same engine is in sherpa-onnx-c-api.dll beside it: loaded
// once when Burnos opens and kept until it closes (measured: "Okay." in 0.4 s
// instead of 1.5). Found by name at run time; if the library is not there, or
// is the build without a voice, every answer goes through the program as
// before. BURNOS_TTS_CLI forces that, for comparing the two.
//
// The structures are sherpa-onnx v1.13.8's own (c-api.h), field for field.
struct VitsConfig { const char *model, *lexicon, *tokens, *data_dir; float noise_scale, noise_scale_w, length_scale; const char *dict_dir; };
struct MatchaConfig { const char *acoustic_model, *vocoder, *lexicon, *tokens, *data_dir; float noise_scale, length_scale; const char *dict_dir; };
struct KokoroConfig { const char *model, *voices, *tokens, *data_dir; float length_scale; const char *dict_dir, *lexicon, *lang; };
struct KittenConfig { const char *model, *voices, *tokens, *data_dir; float length_scale; };
struct ZipvoiceConfig { const char *tokens, *encoder, *decoder, *vocoder, *data_dir, *lexicon; float feat_scale, t_shift, target_rms, guidance_scale; };
struct PocketConfig { const char *lm_flow, *lm_main, *encoder, *decoder, *text_conditioner, *vocab_json, *token_scores_json; qint32 voice_embedding_cache_capacity; };
struct SupertonicConfig { const char *duration_predictor, *text_encoder, *vector_estimator, *vocoder, *tts_json, *unicode_indexer, *voice_style; };
struct TtsModelConfig { VitsConfig vits; qint32 num_threads; qint32 debug; const char *provider; MatchaConfig matcha; KokoroConfig kokoro; KittenConfig kitten; ZipvoiceConfig zipvoice; PocketConfig pocket; SupertonicConfig supertonic; };
struct TtsConfig { TtsModelConfig model; const char *rule_fsts; qint32 max_num_sentences; const char *rule_fars; float silence_scale; };
struct GeneratedAudio { const float *samples; qint32 n; qint32 sample_rate; };

struct LoadedVoice
{
    QMutex lock;
    bool tried = false;
    std::atomic_bool failed{false};
    QString error;
    const void *tts = nullptr;
    const GeneratedAudio *(*generate)(const void *, const char *, qint32, float,
                                      qint32 (*)(const float *, qint32, float, void *), void *) = nullptr;
    void (*destroy)(const GeneratedAudio *) = nullptr;

    static QString library()
    {
        return QCoreApplication::applicationDirPath()
               + QStringLiteral("/kokoro-tts/sherpa-onnx-c-api.dll");
    }

    bool usable() const
    {
        return !failed && !qEnvironmentVariableIsSet("BURNOS_TTS_CLI")
               && QFileInfo::exists(library());
    }

    // Between sentences: 0 stops a voice nobody wants any more.
    static qint32 carryOn(const float *, qint32, float, void *cancelled)
    {
        return static_cast<std::atomic_bool *>(cancelled)->load() ? 0 : 1;
    }

    // Called with the lock held.
    bool load()
    {
        if (tried)
            return tts != nullptr;
        tried = true;
        const QString path = QDir::toNativeSeparators(library());
        HMODULE lib = LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
                                     LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!lib) {
            error = QStringLiteral("%1 would not load (error %2)").arg(path).arg(GetLastError());
            return false;
        }
        const void *(*create)(const TtsConfig *) = nullptr;
        create = reinterpret_cast<decltype(create)>(
            reinterpret_cast<void *>(GetProcAddress(lib, "SherpaOnnxCreateOfflineTts")));
        generate = reinterpret_cast<decltype(generate)>(reinterpret_cast<void *>(
            GetProcAddress(lib, "SherpaOnnxOfflineTtsGenerateWithProgressCallbackWithArg")));
        destroy = reinterpret_cast<decltype(destroy)>(reinterpret_cast<void *>(
            GetProcAddress(lib, "SherpaOnnxDestroyOfflineTtsGeneratedAudio")));
        if (!create || !generate || !destroy) {
            error = QStringLiteral("%1 is the build without a voice").arg(path);
            return false;
        }
        const QString dir = QDir::toNativeSeparators(BurnOS::Models::kokoroDir());
        const QByteArray model = (dir + QStringLiteral("\\model.onnx")).toLocal8Bit();
        const QByteArray voices = (dir + QStringLiteral("\\voices.bin")).toLocal8Bit();
        const QByteArray tokens = (dir + QStringLiteral("\\tokens.txt")).toLocal8Bit();
        const QByteArray data = (dir + QStringLiteral("\\espeak-ng-data")).toLocal8Bit();
        TtsConfig config;
        std::memset(&config, 0, sizeof config);
        config.model.kokoro.model = model.constData();
        config.model.kokoro.voices = voices.constData();
        config.model.kokoro.tokens = tokens.constData();
        config.model.kokoro.data_dir = data.constData();
        config.model.num_threads = 4;
        config.model.provider = "cpu";
        config.max_num_sentences = 1;
        tts = create(&config);
        if (!tts)
            error = QStringLiteral("the Kokoro model would not load");
        return tts != nullptr;
    }

    // One answer into a 16 bit WAV, on whichever thread calls. False when it
    // was cancelled, or when it has to go through the program instead - and
    // then `failed` is set and every later answer does too.
    bool say(const QString &text, int voice, const QString &wavPath, std::atomic_bool *cancelled)
    {
        QMutexLocker locker(&lock);
        if (!load()) {
            if (!failed.exchange(true))
                BurnOS::liveLog(QStringLiteral("voice: not kept loaded - %1. Each answer starts "
                                               "the Kokoro program instead.").arg(error));
            return false;
        }
        if (cancelled->load())
            return false;
        const GeneratedAudio *audio =
            generate(tts, text.toUtf8().constData(), voice, 1.0f, carryOn, cancelled);
        if (cancelled->load()) {
            if (audio)
                destroy(audio);
            return false;
        }
        if (!audio || audio->n <= 0) {
            if (audio)
                destroy(audio);
            failed = true;
            BurnOS::liveLog(QStringLiteral("voice: the loaded Kokoro gave nothing - each answer "
                                           "starts the program from now on"));
            return false;
        }
        QByteArray wav(44 + qsizetype(audio->n) * 2, '\0');
        char *h = wav.data();
        const auto put32 = [h](int at, quint32 v) { qToLittleEndian<quint32>(v, h + at); };
        const auto put16 = [h](int at, quint16 v) { qToLittleEndian<quint16>(v, h + at); };
        std::memcpy(h, "RIFF", 4);
        put32(4, quint32(36 + audio->n * 2));
        std::memcpy(h + 8, "WAVEfmt ", 8);
        put32(16, 16);
        put16(20, 1);
        put16(22, 1);
        put32(24, quint32(audio->sample_rate));
        put32(28, quint32(audio->sample_rate) * 2);
        put16(32, 2);
        put16(34, 16);
        std::memcpy(h + 36, "data", 4);
        put32(40, quint32(audio->n * 2));
        for (qint32 i = 0; i < audio->n; ++i) {
            const float s = qBound(-1.0f, audio->samples[i], 1.0f);
            qToLittleEndian<qint16>(qint16(std::lround(s * 32767.0f)), h + 44 + qsizetype(i) * 2);
        }
        destroy(audio);
        QFile out(wavPath);
        if (!out.open(QIODevice::WriteOnly) || out.write(wav) != wav.size()) {
            failed = true;
            return false;
        }
        return true;
    }
};

LoadedVoice &loadedVoice()
{
    static LoadedVoice one;
    return one;
}

// One answer at a time.
QThreadPool &voicePool()
{
    static QThreadPool *one = [] {
        auto *made = new QThreadPool(QCoreApplication::instance());
        made->setMaxThreadCount(1);
        return made;
    }();
    return *one;
}

// One sherpa-onnx run: the Kokoro model, the voice chosen in Settings, and the
// WAV to write. The live voice gets four threads, the cache two.
QStringList kokoroArgs(int threads, const QString &wavPath, const QString &text)
{
    const QString dir = BurnOS::Models::kokoroDir();
    return {QStringLiteral("--kokoro-model=%1/model.onnx").arg(dir),
            QStringLiteral("--kokoro-voices=%1/voices.bin").arg(dir),
            QStringLiteral("--kokoro-tokens=%1/tokens.txt").arg(dir),
            QStringLiteral("--kokoro-data-dir=%1/espeak-ng-data").arg(dir),
            QStringLiteral("--num-threads=%1").arg(threads),
            QStringLiteral("--sid=%1").arg(BurnOS::Settings::ttsVoice()),
            QStringLiteral("--output-filename=") + wavPath,
            text};
}

} // namespace

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
        BurnOS::liveLog(QStringLiteral("kokoro skipped: '%1' was made already")
                            .arg(spoken));
        play(cached);
        return true;
    }

    if (!QFileInfo::exists(cliPath())) {
        emit failed(tr("sherpa-onnx-offline-tts.exe is missing from the kokoro-tts folder."));
        return false;
    }

    if (!BurnOS::Models::ttsPresent()) {
        emit failed(tr("The Kokoro model is not in %1")
                        .arg(QDir::toNativeSeparators(BurnOS::Models::kokoroDir())));
        return false;
    }

    m_text = spoken;

    // A fresh file each time: the player keeps a handle on the previous one.
    m_wavPath = QDir::temp().filePath(
        QStringLiteral("burnos-tts-%1.wav").arg(++m_counter));

    // THE LOADED VOICE, off this thread. Only when it cannot be used does the
    // answer go the old way, through the Kokoro program.
    if (loadedVoice().usable()) {
        BurnOS::liveLog(QStringLiteral("kokoro synthesising %1 chars with voice %2")
                            .arg(spoken.size())
                            .arg(BurnOS::Settings::ttsVoice()));
        m_busy = true;
        m_saying = std::make_shared<std::atomic_bool>(false);
        const std::shared_ptr<std::atomic_bool> cancelled = m_saying;
        const QPointer<TtsEngine> self(this);
        const QString wav = m_wavPath;
        const int voice = BurnOS::Settings::ttsVoice();
        voicePool().start([self, spoken, wav, voice, cancelled]() {
            const bool ok = loadedVoice().say(spoken, voice, wav, cancelled.get());
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, spoken, wav, ok, cancelled]() {
                if (!self || cancelled->load())
                    return; // stopped meanwhile: nobody is waiting for it
                if (!ok) {
                    self->m_busy = false;
                    self->speak(spoken); // usable() is false now: the program
                    return;
                }
                BurnOS::liveLog(QStringLiteral("kokoro wrote %1 bytes of wav")
                                    .arg(QFileInfo(wav).size()));
                self->play(wav);
            }, Qt::QueuedConnection);
        });
        return true;
    }

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

    BurnOS::liveLog(QStringLiteral("kokoro synthesising %1 chars with voice %2")
                        .arg(spoken.size())
                        .arg(BurnOS::Settings::ttsVoice()));

    m_busy = true;
    m_proc->setWorkingDirectory(QFileInfo(cliPath()).absolutePath());
    m_proc->start(cliPath(), kokoroArgs(4, m_wavPath, spoken));
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
    // A voice still being made in the loaded model stops at its next sentence
    // and is dropped when it arrives.
    if (m_saying)
        m_saying->store(true);

    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
        m_proc->waitForFinished(2000);
    }
    if (m_player)
        m_player->stop();

    m_cancelled = false;
}

void TtsEngine::preload()
{
    if (!loadedVoice().usable() || !BurnOS::Models::ttsPresent())
        return;
    voicePool().start([]() {
        QElapsedTimer timer;
        timer.start();
        LoadedVoice &voice = loadedVoice();
        QMutexLocker locker(&voice.lock);
        if (voice.load()) {
            BurnOS::liveLog(QStringLiteral("voice: Kokoro loaded at startup in %1 ms - it stays "
                                           "loaded").arg(timer.elapsed()));
        } else if (!voice.failed.exchange(true)) {
            BurnOS::liveLog(QStringLiteral("voice: not kept loaded - %1. Each answer starts the "
                                           "Kokoro program instead.").arg(voice.error));
        }
    });
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
    if (m_toWarm.isEmpty() || !QFileInfo::exists(cliPath()) || !BurnOS::Models::ttsPresent())
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

    BurnOS::liveLog(QStringLiteral("voice cache: making '%1' with voice %2")
                        .arg(phrase)
                        .arg(BurnOS::Settings::ttsVoice()));
    m_warm->setWorkingDirectory(QFileInfo(cliPath()).absolutePath());
    m_warm->start(cliPath(), kokoroArgs(2, m_warmTarget + QStringLiteral(".part.wav"), phrase));
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
