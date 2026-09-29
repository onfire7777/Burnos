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
#include <QFloat16>
#include <QHash>

#include <cmath>
#include <cstring>

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

// Silero hears in steps of 512 samples, 32 ms at 16 kHz, and a phrase starts
// once two steps in a row sound like speech - a keystroke is one step at most.
constexpr int kVadChunkBytes = 512 * kBytesPerSample;
constexpr int kVadAttackMs = 64;

qint64 msToBytes(int ms)
{
    return kBytesPerSecond * ms / 1000;
}

int bytesToMs(qint64 bytes)
{
    return static_cast<int>(bytes * 1000 / kBytesPerSecond);
}
} // namespace

namespace BurnOS {

// ===========================================================================
// SILERO VAD, RUN BY HAND.
//
// A network of about 300 thousand weights that tells speech from everything
// else far better than any loudness threshold: a keyboard, a door, a tone or
// white noise barely registers, a quiet voice does. The weights are Silero
// 6.2.0 as whisper.cpp publishes them (ggml-silero-v6.2.0.bin, MIT), and the
// network is computed here in plain C++ - no ONNX Runtime, no library at all.
// That costs well under a millisecond per 32 ms of audio.
//
// PROVED AGAINST THE ORIGINAL on 2026-09-28: the same audio through the
// official silero_vad.onnx 6.2.0 and through this network agreed on every
// 32 ms step, see the v0.4 plan and Burnos.exe --vad-probe.
//
// One step:
//   the last 64 samples of the step before, then the new 512
//   -> 64 more on the right by reflection
//   -> an STFT done as a 258 by 256 convolution, hop 128: four frames, and
//      the magnitude of 129 frequency bins from each
//   -> four convolutions, kernel 3, padding 1, strides 1 2 2 1, each + ReLU
//   -> one LSTM cell of 128, whose state carries on to the next step
//   -> ReLU, a 128 to 1 projection and a sigmoid: the chance this is speech.
// ===========================================================================
class SileroVad
{
public:
    static constexpr int kChunk = 512;
    static constexpr int kContext = 64;

    bool load(const QString &path, QString *error);
    bool isLoaded() const { return m_loaded; }
    QString version() const { return m_version; }
    // A new stream: no memory of what came before.
    void reset();
    // 512 samples of 16 kHz audio scaled to -1..1; the chance it is speech.
    float process(const float *chunk);

private:
    struct Conv
    {
        int cout = 0;
        int cin = 0;
        int k = 0;
        int stride = 1;
        std::vector<float> w; // [cout][cin][k]
        std::vector<float> b; // [cout]
    };

    static float sigmoid(float v) { return 1.0f / (1.0f + std::exp(-v)); }
    static void convRelu(const Conv &c, const std::vector<float> &in, int len,
                         std::vector<float> &out, int *outLen);

    std::vector<float> m_basis; // [258][256], real rows then imaginary rows
    Conv m_enc[4];
    std::vector<float> m_wih;   // [512][128], gates in, forget, cell, out
    std::vector<float> m_whh;   // [512][128]
    std::vector<float> m_bias;  // bias_ih + bias_hh, [512]
    std::vector<float> m_outW;  // [128]
    float m_outB = 0.0f;
    float m_h[128] = {};
    float m_c[128] = {};
    float m_context[kContext] = {};
    QString m_version;
    bool m_loaded = false;
};

bool SileroVad::load(const QString &path, QString *error)
{
    m_loaded = false;
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open %1").arg(path));
    const QByteArray bytes = file.readAll();
    const char *d = bytes.constData();
    const qsizetype size = bytes.size();
    qsizetype at = 0;
    bool ok = true;
    const auto i32 = [&]() -> qint32 {
        if (at + 4 > size) {
            ok = false;
            return 0;
        }
        qint32 v = 0;
        std::memcpy(&v, d + at, 4);
        at += 4;
        return v;
    };

    // THE HEADER, which pins down the shape. Anything else is refused rather
    // than guessed at: a network read with the wrong shape still produces
    // numbers, just not meaningful ones.
    if (i32() != 0x67676d6c)
        return fail(QStringLiteral("not a ggml file"));
    const qint32 typeLength = i32();
    if (!ok || typeLength <= 0 || typeLength > 64 || at + typeLength > size)
        return fail(QStringLiteral("damaged header"));
    const QByteArray type(d + at, typeLength);
    at += typeLength;
    if (type != "silero-16k")
        return fail(QStringLiteral("not the 16 kHz Silero model"));
    const qint32 major = i32();
    const qint32 minor = i32();
    const qint32 patch = i32();
    const qint32 window = i32();
    const qint32 context = i32();
    const qint32 layers = i32();
    if (!ok || window != kChunk || context != kContext || layers != 4)
        return fail(QStringLiteral("unexpected window, context or depth"));

    static const int kIn[4] = {129, 128, 64, 64};
    static const int kOut[4] = {128, 64, 64, 128};
    static const int kStride[4] = {1, 2, 2, 1};
    for (int i = 0; i < 4; ++i) {
        const qint32 cin = i32();
        const qint32 cout = i32();
        const qint32 k = i32();
        if (!ok || cin != kIn[i] || cout != kOut[i] || k != 3)
            return fail(QStringLiteral("unexpected encoder layer %1").arg(i));
        m_enc[i].cin = cin;
        m_enc[i].cout = cout;
        m_enc[i].k = k;
        m_enc[i].stride = kStride[i];
    }
    const qint32 lstmIn = i32();
    const qint32 lstmHidden = i32();
    const qint32 finalIn = i32();
    const qint32 finalOut = i32();
    if (!ok || lstmIn != 128 || lstmHidden != 128 || finalIn != 128 || finalOut != 1)
        return fail(QStringLiteral("unexpected decoder"));

    // THE TENSORS: dimensions, name, then the data as float32 or float16.
    QHash<QString, std::vector<float>> tensors;
    while (at < size) {
        const qint32 dims = i32();
        const qint32 nameLength = i32();
        const qint32 kind = i32();
        if (!ok || dims < 0 || dims > 4 || nameLength <= 0 || nameLength > 256
            || (kind != 0 && kind != 1))
            return fail(QStringLiteral("damaged tensor header"));
        qint64 count = 1;
        for (int i = 0; i < dims; ++i) {
            const qint32 n = i32();
            if (!ok || n <= 0)
                return fail(QStringLiteral("damaged tensor shape"));
            count *= n;
        }
        if (at + nameLength > size)
            return fail(QStringLiteral("truncated tensor name"));
        const QString name = QString::fromLatin1(d + at, nameLength);
        at += nameLength;
        const qint64 length = count * (kind == 1 ? 2 : 4);
        if (at + length > size)
            return fail(QStringLiteral("truncated tensor %1").arg(name));
        std::vector<float> values(size_t(count), 0.0f);
        if (kind == 1) {
            std::vector<qfloat16> half(static_cast<size_t>(count));
            std::memcpy(half.data(), d + at, size_t(length));
            qFloatFromFloat16(values.data(), half.data(), count);
        } else {
            std::memcpy(values.data(), d + at, size_t(length));
        }
        at += length;
        tensors.insert(name, std::move(values));
    }

    const auto take = [&tensors](const QString &name, size_t expected,
                                 std::vector<float> *into) {
        auto it = tensors.find(name);
        if (it == tensors.end() || it->size() != expected)
            return false;
        *into = std::move(*it);
        return true;
    };

    bool have = take(QStringLiteral("_model.stft.forward_basis_buffer"), 258 * 256, &m_basis);
    for (int i = 0; i < 4 && have; ++i) {
        Conv &c = m_enc[i];
        have = take(QStringLiteral("_model.encoder.%1.reparam_conv.weight").arg(i),
                    size_t(c.cout) * size_t(c.cin) * size_t(c.k), &c.w)
               && take(QStringLiteral("_model.encoder.%1.reparam_conv.bias").arg(i),
                       size_t(c.cout), &c.b);
    }
    std::vector<float> bih;
    std::vector<float> bhh;
    std::vector<float> outB;
    have = have
           && take(QStringLiteral("_model.decoder.rnn.weight_ih"), 512 * 128, &m_wih)
           && take(QStringLiteral("_model.decoder.rnn.weight_hh"), 512 * 128, &m_whh)
           && take(QStringLiteral("_model.decoder.rnn.bias_ih"), 512, &bih)
           && take(QStringLiteral("_model.decoder.rnn.bias_hh"), 512, &bhh)
           && take(QStringLiteral("_model.decoder.decoder.2.weight"), 128, &m_outW)
           && take(QStringLiteral("_model.decoder.decoder.2.bias"), 1, &outB);
    if (!have)
        return fail(QStringLiteral("a tensor is missing or the wrong size"));

    m_bias.resize(512);
    for (size_t i = 0; i < 512; ++i)
        m_bias[i] = bih[i] + bhh[i];
    m_outB = outB[0];
    m_version = QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
    reset();
    m_loaded = true;
    return true;
}

void SileroVad::reset()
{
    std::fill(std::begin(m_h), std::end(m_h), 0.0f);
    std::fill(std::begin(m_c), std::end(m_c), 0.0f);
    std::fill(std::begin(m_context), std::end(m_context), 0.0f);
}

void SileroVad::convRelu(const Conv &c, const std::vector<float> &in, int len,
                         std::vector<float> &out, int *outLen)
{
    // Padding of one on each side, as the network was trained with.
    const int n = (len + 2 - c.k) / c.stride + 1;
    *outLen = n;
    out.assign(size_t(c.cout) * size_t(n), 0.0f);
    for (int o = 0; o < c.cout; ++o) {
        for (int t = 0; t < n; ++t) {
            float sum = c.b[size_t(o)];
            for (int i = 0; i < c.cin; ++i) {
                const float *w = c.w.data() + (size_t(o) * size_t(c.cin) + size_t(i)) * size_t(c.k);
                const float *row = in.data() + size_t(i) * size_t(len);
                for (int k = 0; k < c.k; ++k) {
                    const int at = t * c.stride + k - 1;
                    if (at >= 0 && at < len)
                        sum += w[k] * row[at];
                }
            }
            out[size_t(o) * size_t(n) + size_t(t)] = sum > 0.0f ? sum : 0.0f;
        }
    }
}

float SileroVad::process(const float *chunk)
{
    // The last 64 samples of the step before, the 512 new ones, and 64 more by
    // reflecting the end back on itself - exactly the input the network was
    // trained on.
    constexpr int kJoined = kContext + kChunk; // 576
    float x[kJoined + 64];
    std::copy(m_context, m_context + kContext, x);
    std::copy(chunk, chunk + kChunk, x + kContext);
    std::copy(x + kJoined - kContext, x + kJoined, m_context);
    for (int j = 0; j < 64; ++j)
        x[kJoined + j] = x[kJoined - 2 - j];

    // The STFT as a convolution: four frames of 256, hop 128, and the
    // magnitude of each of 129 bins. Laid out [bin][frame].
    std::vector<float> a(129 * 4, 0.0f);
    for (int t = 0; t < 4; ++t) {
        const float *frame = x + t * 128;
        for (int f = 0; f < 129; ++f) {
            const float *re = m_basis.data() + size_t(f) * 256;
            const float *im = m_basis.data() + size_t(f + 129) * 256;
            float r = 0.0f;
            float i = 0.0f;
            for (int k = 0; k < 256; ++k) {
                r += re[k] * frame[k];
                i += im[k] * frame[k];
            }
            a[size_t(f) * 4 + size_t(t)] = std::sqrt(r * r + i * i);
        }
    }

    // The encoder: 129 x 4 -> 128 x 4 -> 64 x 2 -> 64 x 1 -> 128 x 1.
    std::vector<float> b;
    int len = 4;
    for (const Conv &c : m_enc) {
        int outLen = 0;
        convRelu(c, a, len, b, &outLen);
        a.swap(b);
        len = outLen;
    }

    // The LSTM cell. Every gate is worked out from the OLD hidden state before
    // either state is updated.
    float gates[512];
    for (int r = 0; r < 512; ++r) {
        const float *wi = m_wih.data() + size_t(r) * 128;
        const float *wh = m_whh.data() + size_t(r) * 128;
        float sum = m_bias[size_t(r)];
        for (int k = 0; k < 128; ++k)
            sum += wi[k] * a[size_t(k) * size_t(len)] + wh[k] * m_h[k];
        gates[r] = sum;
    }
    for (int j = 0; j < 128; ++j) {
        const float in = sigmoid(gates[j]);
        const float forget = sigmoid(gates[128 + j]);
        const float cell = std::tanh(gates[256 + j]);
        const float out = sigmoid(gates[384 + j]);
        m_c[j] = forget * m_c[j] + in * cell;
        m_h[j] = out * std::tanh(m_c[j]);
    }

    float z = m_outB;
    for (int j = 0; j < 128; ++j)
        z += m_outW[size_t(j)] * (m_h[j] > 0.0f ? m_h[j] : 0.0f);
    return sigmoid(z);
}

} // namespace BurnOS

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

    // THE EARS. Silero decides what is speech whenever its weights load; the
    // old loudness gate is only what is left if they will not, and the log
    // says which one is listening.
    if (!m_vad) {
        m_vad = std::make_unique<BurnOS::SileroVad>();
        QString why;
        if (m_vad->load(BurnOS::Stt::vadModelPath(), &why)) {
            BurnOS::liveLog(QStringLiteral("voice detector: Silero %1, speech opens at %2")
                                .arg(m_vad->version())
                                .arg(BurnOS::Settings::vadThreshold(), 0, 'f', 2));
        } else {
            BurnOS::liveLog(QStringLiteral("voice detector: Silero unavailable (%1), "
                                           "listening by loudness instead")
                                .arg(why));
        }
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
    // A new stream: Silero starts with no memory of the last one.
    m_vadIn.clear();
    if (m_vad)
        m_vad->reset();

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

    if (m_vad && m_vad->isLoaded())
        hearBySilero(pcm);
    else
        hearByLevel(pcm, rms);
}

void SttEngine::hearByLevel(const QByteArray &pcm, qreal rms)
{
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

    endPhrase(tooLong, hangoverMs);
}

void SttEngine::hearBySilero(const QByteArray &pcm)
{
    // The audio itself is kept exactly as it always was - a short pre-roll
    // while waiting, the whole phrase once one has started. Only the DECISION
    // is Silero's.
    if (m_capturing) {
        m_speech.append(pcm);
    } else {
        m_preroll.append(pcm);
        const qint64 keep = msToBytes(kPrerollMs);
        if (m_preroll.size() > keep)
            m_preroll.remove(0, static_cast<qsizetype>(m_preroll.size() - keep));
    }

    const qint16 *samples = reinterpret_cast<const qint16 *>(pcm.constData());
    const int count = static_cast<int>(pcm.size() / 2);
    for (int i = 0; i < count; ++i)
        m_vadIn.push_back(samples[i] / 32768.0f);

    // The microphone hands over 20 ms at a time and Silero listens in 32 ms
    // steps, so whatever is left over waits for the next buffer.
    while (m_vadIn.size() >= size_t(BurnOS::SileroVad::kChunk)) {
        const float speech = m_vad->process(m_vadIn.data());
        m_vadIn.erase(m_vadIn.begin(), m_vadIn.begin() + BurnOS::SileroVad::kChunk);
        decide(speech);
    }
}

void SttEngine::decide(float speech)
{
    // Read every time, so the sensitivity slider applies to the very next step.
    // A phrase OPENS at the threshold and only counts as quiet again well under
    // it, so a voice trailing off at the end of a word does not end the phrase.
    const float openAt = float(BurnOS::Settings::vadThreshold());
    const float closeAt = qMax(0.15f, openAt - 0.15f);

    if (!m_capturing) {
        // The first seconds of every open microphone are traced, so a run can
        // be proved to have been listening at all.
        m_idleBytes += kVadChunkBytes;
        if (m_beats < 5 && m_idleBytes >= msToBytes(1000)) {
            m_idleBytes = 0;
            ++m_beats;
            BurnOS::liveLog(QStringLiteral("listening: speech %1, opens at %2")
                                .arg(double(speech), 0, 'f', 3)
                                .arg(double(openAt), 0, 'f', 2));
        }

        if (speech < openAt) {
            m_attackBytes = 0;
            return;
        }
        // Two steps in a row, 64 ms. A keystroke that fools one step does not
        // fool two, and the pre-roll is far longer, so nothing is clipped.
        m_attackBytes += kVadChunkBytes;
        if (m_attackBytes < msToBytes(kVadAttackMs))
            return;

        m_capturing = true;
        m_speech = m_preroll;
        m_preroll.clear();
        m_voicedBytes = m_attackBytes;
        m_silenceBytes = 0;
        m_attackBytes = 0;
        BurnOS::liveLog(QStringLiteral("speech STARTED, Silero %1 over %2")
                            .arg(double(speech), 0, 'f', 3)
                            .arg(double(openAt), 0, 'f', 2));
        emit speechStarted();
        return;
    }

    if (speech >= closeAt) {
        m_voicedBytes += kVadChunkBytes;
        m_silenceBytes = 0;
    } else {
        m_silenceBytes += kVadChunkBytes;
    }

    const int hangoverMs = BurnOS::Settings::hangoverMs();
    const bool quietEnough = m_silenceBytes >= msToBytes(hangoverMs);
    const bool tooLong = m_speech.size() >= msToBytes(kMaxPhraseMs);
    if (quietEnough || tooLong)
        endPhrase(tooLong, hangoverMs);
}

void SttEngine::endPhrase(bool tooLong, int hangoverMs)
{
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

    // REAL AUDIO when the variable names a WAV: speech, noise, whatever it
    // holds goes through the real detector, and whatever it calls a phrase goes
    // on to the recogniser and the Agent exactly as a spoken one would.
    const QString file = qEnvironmentVariable("BURNOS_VAD_SELFTEST");
    if (QFileInfo(file).isFile()) {
        QString error;
        const QByteArray pcm = readPcm(file, &error);
        if (pcm.isEmpty()) {
            BurnOS::liveLog(QStringLiteral("SELF TEST: %1").arg(error));
            return;
        }
        BurnOS::liveLog(QStringLiteral("SELF TEST: feeding %1 ms of %2")
                            .arg(bytesToMs(pcm.size()))
                            .arg(QFileInfo(file).fileName()));
        for (qsizetype at = 0; at + chunk <= pcm.size() && m_live; at += chunk)
            onAudio(pcm.mid(at, chunk), 0.0);
        BurnOS::liveLog(QStringLiteral("SELF TEST: fed the whole file"));
        return;
    }
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

QByteArray SttEngine::readPcm(const QString &wavPath, QString *error)
{
    // Plain RIFF WAVE, walked chunk by chunk, and only the one format the
    // detector and the recogniser both take: 16 kHz, mono, 16 bit.
    QFile f(wavPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot open %1").arg(wavPath);
        return {};
    }
    const QByteArray bytes = f.readAll();
    if (bytes.size() < 12 || bytes.left(4) != "RIFF" || bytes.mid(8, 4) != "WAVE") {
        if (error)
            *error = QStringLiteral("%1 is not a WAV file").arg(wavPath);
        return {};
    }

    const auto u16 = [&bytes](qsizetype at) {
        quint16 v = 0;
        std::memcpy(&v, bytes.constData() + at, 2);
        return v;
    };
    const auto u32 = [&bytes](qsizetype at) {
        quint32 v = 0;
        std::memcpy(&v, bytes.constData() + at, 4);
        return v;
    };

    int channels = 0;
    int rate = 0;
    int bits = 0;
    qsizetype at = 12;
    while (at + 8 <= bytes.size()) {
        const QByteArray id = bytes.mid(at, 4);
        const quint32 length = u32(at + 4);
        at += 8;
        if (id == "fmt " && length >= 16 && at + 16 <= bytes.size()) {
            channels = u16(at + 2);
            rate = int(u32(at + 4));
            bits = u16(at + 14);
        } else if (id == "data") {
            if (channels != 1 || rate != kSampleRate || bits != 16) {
                if (error)
                    *error = QStringLiteral("%1 is %2 Hz, %3 channel(s), %4 bit - it has to "
                                            "be 16000 Hz mono 16 bit")
                                 .arg(wavPath).arg(rate).arg(channels).arg(bits);
                return {};
            }
            return bytes.mid(at, qMin<qsizetype>(length, bytes.size() - at));
        }
        at += qsizetype(length + (length & 1));
    }
    if (error)
        *error = QStringLiteral("%1 has no audio in it").arg(wavPath);
    return {};
}

bool SttEngine::probeFile(const QString &wavPath, const QString &outPath, QString *error)
{
    BurnOS::SileroVad vad;
    if (!vad.load(BurnOS::Stt::vadModelPath(), error))
        return false;

    const QByteArray pcm = readPcm(wavPath, error);
    if (pcm.isEmpty())
        return false;

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("cannot write %1").arg(outPath);
        return false;
    }

    // From a fresh state, 512 samples at a time, exactly as the official
    // wrapper walks a file - so the two lists line up step for step.
    const qint16 *samples = reinterpret_cast<const qint16 *>(pcm.constData());
    const int count = int(pcm.size() / 2);
    float chunk[BurnOS::SileroVad::kChunk];
    for (int start = 0; start + BurnOS::SileroVad::kChunk <= count;
         start += BurnOS::SileroVad::kChunk) {
        for (int i = 0; i < BurnOS::SileroVad::kChunk; ++i)
            chunk[i] = samples[start + i] / 32768.0f;
        out.write(QByteArray::number(double(vad.process(chunk)), 'f', 6) + '\n');
    }
    return true;
}
