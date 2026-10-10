#include "installer.h"
#include "livelog.h"
#include "settings.h"
#include "sttengine.h"

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
// Nobody asks a question for half a minute. This stops a noisy room from
// recording for ever.
constexpr int kMaxPhraseMs = 30000;
// A push-to-talk hold shorter than this was a slip of the finger.
constexpr int kMinHeldMs = 300;

// Silero hears in steps of 512 samples, 32 ms at 16 kHz, and a phrase starts
// once two steps in a row sound like speech - a keystroke is one step at most.
constexpr int kVadChunkBytes = 512 * kBytesPerSample;
constexpr int kVadAttackMs = 64;

// OVER THE VOICE. Measured on 2026-09-28 with the Antlion microphone: the
// orb's own voice came back 270 ms late and 43 dB down - quiet, but Silero
// still calls it speech. A person talking into the microphone is 30 dB or more
// above that, so speech only counts over the voice when it is this many times
// louder than the loudest echo heard, and never below the floor.
constexpr float kBargeOverEcho = 8.0f;
constexpr float kBargeFloor = 0.012f;
// Three steps in a row, 96 ms, so one loud syllable of echo cannot do it.
constexpr int kBargeSteps = 3;
// The first moments of a voice are not listened for: its echo has not even
// arrived yet.
constexpr int kBargeWarmupMs = 250;
// Until this much of the echo has been heard, everything is taken to be echo.
constexpr int kEchoLearnSteps = 10;

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
    for (int i = 0; i < count; ++i)
        peak = qMax(peak, qAbs(static_cast<int>(samples[i])));

    if (count > 0) {
        // Deliberately hot. A raw linear level barely moves for normal speech,
        // so the curve is bent hard to make quiet talking still show.
        emit levelChanged(qBound(0.0, qPow(peak / 32768.0, 0.45) * 1.8, 1.0));
        emit audioChunk(QByteArray(data, static_cast<qsizetype>(size)));
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

    if (!BurnOS::Models::sttPresent()) {
        emit failed(tr("No model at %1")
                        .arg(QDir::toNativeSeparators(BurnOS::Models::sttModel())));
        return false;
    }

    if (!QFileInfo::exists(cliPath())) {
        emit failed(tr("parakeet-cli.exe is missing from the parakeet-streaming folder."));
        return false;
    }

    // THE EARS: Silero decides what is speech. The installer will not let the
    // orb start without its weights, so a load that fails here is a damaged
    // file - said, rather than listened past.
    if (!m_vad) {
        auto vad = std::make_unique<BurnOS::SileroVad>();
        QString why;
        if (!vad->load(BurnOS::Models::vadModel(), &why)) {
            emit failed(tr("The voice detector would not load: %1").arg(why));
            return false;
        }
        m_vad = std::move(vad);
        BurnOS::liveLog(QStringLiteral("voice detector: Silero %1, speech opens at %2")
                            .arg(m_vad->version())
                            .arg(BurnOS::Settings::vadThreshold(), 0, 'f', 2));
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

bool SttEngine::beginHeld()
{
    if (m_held)
        return true;
    if (!m_source) {
        // Live mode is off or paused: the microphone opens for this hold only.
        if (!BurnOS::Models::sttPresent() || !QFileInfo::exists(cliPath())) {
            emit failed(tr("The speech model is not installed yet."));
            return false;
        }
        if (!openMicrophone())
            return false;
        m_heldOwnsMic = true;
    }
    // WHAT WAS BEING SAID TO THE AGENT STAYS THE AGENT'S. A phrase the detector
    // had started ends here and goes to the recogniser as it is; only what
    // comes after the key is the hold's. With nothing being said, the hold
    // starts with the moment before the key, so no syllable is lost.
    if (m_capturing) {
        endPhrase(false, 0);
        m_heldPcm.clear();
    } else {
        m_heldPcm = m_preroll;
    }
    m_preroll.clear();
    m_attackBytes = 0;
    m_held = true;
    BurnOS::liveLog(QStringLiteral("push to talk DOWN%1")
                        .arg(m_heldOwnsMic ? QStringLiteral(" (microphone opened for it)")
                                           : QString()));
    return true;
}

bool SttEngine::endHeld()
{
    if (!m_held)
        return false;
    m_held = false;
    const QByteArray phrase = m_heldPcm;
    m_heldPcm.clear();
    if (m_heldOwnsMic) {
        m_heldOwnsMic = false;
        // Unless live mode was armed while the key was down: then it is in use.
        if (!m_live)
            closeMicrophone();
    }
    const int ms = bytesToMs(phrase.size());
    BurnOS::liveLog(QStringLiteral("push to talk UP after %1 ms%2")
                        .arg(ms)
                        .arg(ms < kMinHeldMs ? QStringLiteral(" - too short to be a phrase")
                                             : QString()));
    if (ms < kMinHeldMs)
        return false;
    transcribe(phrase, true);
    return true;
}

void SttEngine::setOverVoice(bool on)
{
    if (m_overVoice == on)
        return;
    m_overVoice = on;
    m_bargeSteps = 0;
    m_overVoiceBytes = 0;
    if (on && m_source) {
        BurnOS::liveLog(QStringLiteral("listening over the voice (echo %1)")
                            .arg(double(m_echoLevel), 0, 'f', 4));
    }
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

void SttEngine::onAudio(const QByteArray &pcm)
{
    // PUSH TO TALK IS DOWN: all of it is the phrase, and the detector is not
    // asked. A key held for minutes still reaches the recogniser half a minute
    // at a time - deferred, because this is the audio device's own write call.
    if (m_held) {
        m_heldPcm.append(pcm);
        if (m_heldPcm.size() >= msToBytes(kMaxPhraseMs)) {
            const QByteArray part = m_heldPcm;
            m_heldPcm.clear();
            QTimer::singleShot(0, this, [this, part]() { transcribe(part, true); });
        }
        return;
    }
    if (!m_live || !m_source)
        return;

    // A short pre-roll while waiting, the whole phrase once one has started;
    // whether one HAS started is Silero's decision.
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
        double square = 0.0;
        for (int i = 0; i < BurnOS::SileroVad::kChunk; ++i)
            square += double(m_vadIn[size_t(i)]) * double(m_vadIn[size_t(i)]);
        const float rms = float(std::sqrt(square / BurnOS::SileroVad::kChunk));
        const float speech = m_vad->process(m_vadIn.data());
        m_vadIn.erase(m_vadIn.begin(), m_vadIn.begin() + BurnOS::SileroVad::kChunk);
        decide(speech, rms);
    }
}

void SttEngine::decide(float speech, float rms)
{
    // Read every time, so the sensitivity slider applies to the very next step.
    // A phrase OPENS at the threshold and only counts as quiet again well under
    // it, so a voice trailing off at the end of a word does not end the phrase.
    const float openAt = float(BurnOS::Settings::vadThreshold());
    const float closeAt = qMax(0.15f, openAt - 0.15f);

    if (m_overVoice && !m_capturing) {
        m_overVoiceBytes += kVadChunkBytes;
        const float bargeAt = qMax(kBargeFloor, m_echoLevel * kBargeOverEcho);
        const bool learned = m_echoSteps >= kEchoLearnSteps;
        const bool warm = m_overVoiceBytes > msToBytes(kBargeWarmupMs);

        // SOMEBODY TALKING OVER IT: speech, and far louder than the echo.
        if (learned && warm && speech >= openAt && rms >= bargeAt) {
            if (++m_bargeSteps < kBargeSteps)
                return;
            BurnOS::liveLog(QStringLiteral("BARGE IN: speech %1 at rms %2 over %3 "
                                           "(echo %4)")
                                .arg(double(speech), 0, 'f', 3)
                                .arg(double(rms), 0, 'f', 4)
                                .arg(double(bargeAt), 0, 'f', 4)
                                .arg(double(m_echoLevel), 0, 'f', 4));
            m_bargeSteps = 0;
            m_overVoice = false;
            m_capturing = true;
            m_speech = m_preroll;
            m_preroll.clear();
            m_voicedBytes = qint64(kBargeSteps) * kVadChunkBytes;
            m_silenceBytes = 0;
            m_attackBytes = 0;
            emit bargeIn();
            emit speechStarted();
            return;
        }
        m_bargeSteps = 0;

        // Anything else that sounds like speech is the orb's own voice coming
        // back. How loud it gets is held at its peaks, fading only slowly, so
        // the loudest syllable of an answer sets the bar for the rest of it.
        //
        // ONCE LEARNED IT ONLY CREEPS UP, by half as much again per step. The
        // first 32 ms of somebody starting to talk over the voice is quieter
        // than the rest; taken in whole as echo, it lifted the bar above their
        // own voice and barge-in failed about half the time.
        if (speech >= closeAt) {
            const float heard = m_echoSteps < kEchoLearnSteps
                                    ? rms
                                    : qMin(rms, m_echoLevel * 1.5f);
            m_echoLevel = qMax(m_echoLevel * 0.995f, heard);
            if (m_echoSteps < kEchoLearnSteps && ++m_echoSteps == kEchoLearnSteps) {
                BurnOS::liveLog(QStringLiteral("echo learned: the voice comes back at "
                                               "rms %1, talking over it takes %2")
                                    .arg(double(m_echoLevel), 0, 'f', 4)
                                    .arg(double(qMax(kBargeFloor,
                                                     m_echoLevel * kBargeOverEcho)),
                                         0, 'f', 4));
            }
        } else {
            m_echoLevel = qMax(0.0005f, m_echoLevel * 0.9995f);
        }
        return;
    }

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

    // The microphone STAYS OPEN: whatever the user adds while we work is folded
    // into the same question by the Agent. Deferred by one turn of the event
    // loop, because we are inside the audio device's own write call.
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
        // BURNOS_VAD_OVERVOICE rehearses the file as if the orb were talking
        // the whole time, which is how talking over the voice is proved.
        if (qEnvironmentVariableIsSet("BURNOS_VAD_OVERVOICE"))
            setOverVoice(true);
        BurnOS::liveLog(QStringLiteral("SELF TEST: feeding %1 ms of %2%3")
                            .arg(bytesToMs(pcm.size()))
                            .arg(QFileInfo(file).fileName())
                            .arg(m_overVoice ? QStringLiteral(" over the voice") : QString()));
        for (qsizetype at = 0; at + chunk <= pcm.size() && m_live; at += chunk)
            onAudio(pcm.mid(at, chunk));
        BurnOS::liveLog(QStringLiteral("SELF TEST: fed the whole file"));
        return;
    }
    BurnOS::liveLog(QStringLiteral("SELF TEST: BURNOS_VAD_SELFTEST has to name a 16 kHz "
                                   "mono WAV"));
}

void SttEngine::transcribe(const QByteArray &pcm, bool held)
{
    if (m_pending) {
        // One recogniser at a time. Now that the microphone stays open while we
        // work, a second phrase can arrive before the first has been read, and
        // it waits its turn rather than overwriting the wav that is still open.
        m_queue.append({pcm, held});
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
                    if (m_pendingHeld)
                        emit dictated(out);
                    else
                        emit transcribed(out);
                    drainQueue();
                });
    }

    m_pending = true;
    m_pendingHeld = held;

    BurnOS::liveLog(QStringLiteral("transcribing %1 ms of audio").arg(bytesToMs(pcm.size())));

    // -ng because there is no GPU here and probing for one costs seconds.
    // -np so stdout carries the transcription and nothing else.
    m_proc->setWorkingDirectory(QFileInfo(cliPath()).absolutePath());
    m_proc->start(cliPath(), {QStringLiteral("-m"), BurnOS::Models::sttModel(),
                              QStringLiteral("-ng"),
                              QStringLiteral("-np"),
                              QStringLiteral("-f"), m_wavPath});
}

void SttEngine::drainQueue()
{
    if (m_pending || m_queue.isEmpty())
        return;
    const QPair<QByteArray, bool> next = m_queue.takeFirst();
    transcribe(next.first, next.second);
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
    if (!vad.load(BurnOS::Models::vadModel(), error))
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
