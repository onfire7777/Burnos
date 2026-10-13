#include "installer.h"
#include "livelog.h"
#include "settings.h"
#include "sttengine.h"
#include "voicecheck.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMediaDevices>
#include <QProcess>
#include <QTimer>
#include <QtMath>
#include <QFloat16>
#include <QHash>

#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThreadPool>

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

// Last, and only for loading the recogniser's library from its own folder.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

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
// Less voiced audio than this was a cough, a door or a keyboard. It was 350 ms
// until a quick "mute" turned out to be about that long: the owner's own log
// has them at 352 and 384 ms, and phrases of 256 and 320 ms thrown away.
constexpr int kMinSpeechMs = 250;
// Nobody asks a question for half a minute. This stops a noisy room from
// recording for ever.
constexpr int kMaxPhraseMs = 30000;
// A push-to-talk hold shorter than this was a slip of the finger.
constexpr int kMinHeldMs = 300;

// What a real microphone hands over at a time: 20 ms.
constexpr int kMicChunkBytes = 640;

// THE VOICE CHECK cannot judge a phrase with less speech in it than this - on
// half a second of "yes" or "stop" the speaker model scores a stranger as high
// as the owner (measured, see _research/voice-check). Such a phrase counts on
// loudness instead: the owner sits at the microphone and a television does
// not, so it has to come in at this share of the owner's own loudness.
constexpr int kShortPhraseMs = 1200;
constexpr double kLoudShare = 0.5;
// A LONGER PHRASE THAT IS AS LOUD AS THE OWNER may score this far under what
// Settings asks for. With a television talking under him the owner's own score
// drops - measured with stand-in voices, from 0.89 in a quiet room to 0.59 with
// a television at a third of his loudness, and his own log shows the same -
// while a television across the room never comes in at half his loudness.
// The price is somebody else AT the microphone: with this margin about one of
// their longer phrases in twenty gets through (one in eight at 0.20, none at
// 0.10, where the owner loses one in fourteen of his own).
constexpr double kLoudMargin = 0.15;
// What is kept either side of the near voice when a phrase goes to the
// recogniser: enough that no word is clipped, and no more of the television.
constexpr int kKeepBeforeMs = 200;
constexpr int kKeepAfterMs = 300;
// This much speech far quieter than the phrase's own voice, and somebody else
// was talking in the room while it was said.
constexpr int kCrowdedMs = 500;
// This much of it in a phrase the length of a command, and that voice is
// silenced around the near one before the recogniser reads it.
constexpr int kGateFarMs = 1000;

// A FAR VOICE DOES NOT KEEP A PHRASE OPEN. Silero calls a television speech,
// and rightly, so with one on a phrase used to run until the television paused
// - seconds after the owner had finished - and the answer waited for it. Each
// phrase now follows how loud its own voice is, and speech at less than this
// share of that counts as quiet: one second of it ends the phrase, exactly as
// silence does. A phrase that is only the television has the television as its
// voice, and ends when the television pauses, as before. At a quarter, which
// was tried first, a television at a fifth of the speaker's loudness still
// kept phrases open: its loudest syllables got in, and the loudness being
// followed sank towards it.
constexpr float kFarShare = 0.40f;
// How quickly that loudness follows the voice: a twentieth of the way each
// 32 ms, so it settles within a second and one cough does not carry it off.
constexpr float kNearFollow = 0.05f;
// A phrase being kept at less than this share of the taught voice's loudness
// is not the owner: the answer does not wait for it, and wake word mode does
// not stay awake for it. The owner's own log has the television at 4 to 17
// hundredths of his loudness and himself at 42 and up while it was on.
constexpr float kOwnerShare = 0.25f;

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

// THE RECOGNISER, KEPT LOADED. parakeet-cli.exe reads the 638 MB model in again
// for every phrase, which is most of a second before a word is recognised.
// The same recogniser is in parakeet.dll beside it: loaded once when Burnos
// opens, kept until it closes, and a phrase then costs only its own reading -
// measured, a one second phrase in 140 ms instead of 820. Found by name at run
// time, so nothing new is linked; if it will not load, every phrase goes to
// parakeet-cli as before. BURNOS_STT_CLI forces that, for comparing the two.
struct Recogniser
{
    QMutex lock;
    bool tried = false;
    std::atomic_bool failed{false};
    QString error;
    void *ctx = nullptr;
    void *params = nullptr;
    int (*full)(void *, void *, const float *, int) = nullptr;
    int (*segments)(void *) = nullptr;
    const char *(*segmentText)(void *, int) = nullptr;

    static QString dir()
    {
        return QCoreApplication::applicationDirPath() + QStringLiteral("/parakeet-streaming");
    }

    bool usable() const
    {
        return !failed && !qEnvironmentVariableIsSet("BURNOS_STT_CLI")
               && QFileInfo::exists(dir() + QStringLiteral("/parakeet.dll"));
    }

    template <typename T>
    static bool find(HMODULE lib, const char *name, T &fn)
    {
        fn = reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(lib, name)));
        return fn != nullptr;
    }

    static void quiet(int, const char *, void *) {}

    // Called with the lock held.
    bool load()
    {
        if (tried)
            return ctx != nullptr;
        tried = true;
        const QString folder = QDir::toNativeSeparators(dir());
        const QString path = folder + QStringLiteral("\\parakeet.dll");
        HMODULE lib = LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
                                     LOAD_WITH_ALTERED_SEARCH_PATH);
        HMODULE ggml = lib ? GetModuleHandleW(L"ggml.dll") : nullptr;
        if (!lib || !ggml) {
            error = QStringLiteral("%1 would not load (error %2)").arg(path).arg(GetLastError());
            return false;
        }
        void (*loadBackends)(const char *) = nullptr;
        // parakeet_context_params is { bool use_gpu; int gpu_device; } - eight
        // bytes, which Windows passes by value in a register: zero is "no GPU".
        // parakeet_full_params is far larger and is passed by pointer.
        void *(*init)(const char *, quint64) = nullptr;
        void *(*defaults)(int) = nullptr;
        void (*logSet)(void (*)(int, const char *, void *), void *) = nullptr;
        if (!find(ggml, "ggml_backend_load_all_from_path", loadBackends)
            || !find(lib, "parakeet_init_from_file_with_params", init)
            || !find(lib, "parakeet_full_default_params_by_ref", defaults)
            || !find(lib, "parakeet_full", full)
            || !find(lib, "parakeet_full_n_segments", segments)
            || !find(lib, "parakeet_full_get_segment_text", segmentText)
            || !find(lib, "parakeet_log_set", logSet)) {
            error = QStringLiteral("%1 is not the library this was written for").arg(path);
            return false;
        }
        logSet(quiet, nullptr);
        // ggml looks for its CPU backends beside the EXECUTABLE, which here is
        // Burnos and not parakeet-cli: it is told where they are. The folder
        // is on the library search path only while they load.
        SetDllDirectoryW(reinterpret_cast<LPCWSTR>(folder.utf16()));
        loadBackends(folder.toLocal8Bit().constData());
        ctx = init(QDir::toNativeSeparators(BurnOS::Models::sttModel()).toLocal8Bit().constData(), 0);
        SetDllDirectoryW(nullptr);
        if (!ctx) {
            error = QStringLiteral("the speech model would not load");
            return false;
        }
        params = defaults(0);
        if (!params) {
            error = QStringLiteral("the recogniser gave no settings");
            ctx = nullptr;
            return false;
        }
        static_cast<int *>(params)[1] = 4; // n_threads, parakeet-cli's own default
        return true;
    }

    // One phrase, on whichever thread calls. False when it has to go to
    // parakeet-cli instead - and then every later one does too.
    bool run(const QByteArray &pcm, QString *text)
    {
        QMutexLocker locker(&lock);
        if (!load()) {
            if (!failed.exchange(true))
                BurnOS::liveLog(QStringLiteral("recogniser: not kept loaded - %1. Each phrase goes "
                                               "to parakeet-cli instead.").arg(error));
            return false;
        }
        const qsizetype count = pcm.size() / 2;
        const auto *raw = reinterpret_cast<const qint16 *>(pcm.constData());
        std::vector<float> samples(static_cast<size_t>(count));
        for (qsizetype i = 0; i < count; ++i)
            samples[static_cast<size_t>(i)] = raw[i] / 32768.0f;
        if (full(ctx, params, samples.data(), int(count)) != 0) {
            failed = true;
            BurnOS::liveLog(QStringLiteral("recogniser: a phrase failed in the loaded model - each "
                                           "phrase goes to parakeet-cli from now on"));
            return false;
        }
        QStringList parts;
        const int n = segments(ctx);
        for (int i = 0; i < n; ++i)
            parts.append(QString::fromUtf8(segmentText(ctx, i)));
        *text = parts.join(QLatin1Char(' ')).simplified();
        return true;
    }
};

Recogniser &recogniser()
{
    static Recogniser one;
    return one;
}

// One phrase at a time: the loaded model is not safe to use from two threads.
QThreadPool &recogniserPool()
{
    static QThreadPool *one = [] {
        auto *made = new QThreadPool(QCoreApplication::instance());
        made->setMaxThreadCount(1);
        return made;
    }();
    return *one;
}

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
    const QString file = qEnvironmentVariable("BURNOS_MIC_WAV");
    if (file.isEmpty())
        return;
    QString error;
    m_fileMic = readPcm(file, &error);
    if (m_fileMic.isEmpty()) {
        BurnOS::liveLog(QStringLiteral("MIC WAV: %1").arg(error));
        return;
    }
    BurnOS::liveLog(QStringLiteral("MIC WAV: %1 (%2 ms) stands in for the microphone, in real "
                                   "time, from the moment it first opens")
                        .arg(QFileInfo(file).fileName())
                        .arg(bytesToMs(m_fileMic.size())));
    auto *timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    timer->setInterval(10);
    connect(timer, &QTimer::timeout, this, &SttEngine::fileMicTick);
    timer->start();
}

void SttEngine::fileMicTick()
{
    if (!m_fileClock.isValid())
        return;
    // Whatever is due by the clock, 20 ms at a time. A microphone that is
    // shut hears none of it, and the time passes all the same. After the end
    // of the file the room is silent, so the last phrase can end.
    const qint64 due = m_fileClock.elapsed() * kBytesPerSecond / 1000;
    while (m_fileAt + kMicChunkBytes <= due) {
        const bool inFile = m_fileAt + kMicChunkBytes <= m_fileMic.size();
        const QByteArray chunk = inFile ? m_fileMic.mid(m_fileAt, kMicChunkBytes)
                                        : QByteArray(kMicChunkBytes, '\0');
        if (inFile && m_fileAt + 2 * kMicChunkBytes > m_fileMic.size())
            BurnOS::liveLog(QStringLiteral("MIC WAV: the whole file has been played"));
        m_fileAt += kMicChunkBytes;
        if (m_source)
            onAudio(chunk);
    }
}

SttEngine::~SttEngine()
{
    // THE MICROPHONE IS TAKEN DOWN BY HAND, THE SOURCE BEFORE ITS TAP. Left to
    // QObject the tap goes first - it was made first - and the audio source,
    // on its way out, writes what it still holds into a tap that is no longer
    // there: Burnos crashed while closing about one time in six with the
    // microphone open, and every crash dump since September is this one. The
    // taps are unhooked first, so that last write reaches nothing of ours -
    // by now the orb it would move has gone as well. Sources and taps that a
    // mute has already let go of, and that are still waiting to be deleted,
    // are children too and go the same way.
    const auto taps = findChildren<AudioLevelTap *>(Qt::FindDirectChildrenOnly);
    for (AudioLevelTap *tap : taps)
        tap->disconnect(this);
    qDeleteAll(findChildren<QAudioSource *>(Qt::FindDirectChildrenOnly));
    m_source = nullptr;
    m_tap = nullptr;
}

QString SttEngine::cliPath() const
{
    // The CLI sits in its own folder so Windows resolves its DLLs from there.
    return QCoreApplication::applicationDirPath()
           + QStringLiteral("/parakeet-streaming/parakeet-cli.exe");
}

bool SttEngine::readyToHear(bool sayWhyNot)
{
    if (!BurnOS::Models::sttPresent()) {
        if (sayWhyNot)
            emit failed(tr("No model at %1")
                            .arg(QDir::toNativeSeparators(BurnOS::Models::sttModel())));
        return false;
    }

    if (!QFileInfo::exists(cliPath())) {
        if (sayWhyNot)
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
            if (sayWhyNot)
                emit failed(tr("The voice detector would not load: %1").arg(why));
            return false;
        }
        m_vad = std::move(vad);
        BurnOS::liveLog(QStringLiteral("voice detector: Silero %1, speech opens at %2")
                            .arg(m_vad->version())
                            .arg(BurnOS::Settings::vadThreshold(), 0, 'f', 2));
    }
    return true;
}

bool SttEngine::startLive()
{
    if (m_live && !m_aside)
        return true;
    if (!readyToHear(true))
        return false;

    listenForWakeWord(false);
    if (m_live && m_source) {
        // FROM HEARING ONLY COMMANDS: the microphone is open already and stays
        // open, and a phrase being kept goes on being kept - so nothing is
        // lost between a wake word and what is said after it.
        m_aside = false;
        if (m_capturing)
            emit speechStarted();
    } else {
        m_aside = false;
        if (!openMicrophone()) {
            m_live = false;
            return false;
        }
    }

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

    const bool wasListening = !m_aside;
    m_live = false;
    m_aside = false;
    listenForWakeWord(false);
    closeMicrophone();
    m_speech.clear();
    m_preroll.clear();
    m_queue.clear();
    if (m_capturing) {
        m_capturing = false;
        if (wasListening)
            emit speechEnded();
    }

    BurnOS::liveLog(QStringLiteral("live mode DISARMED: the microphone is shut"));
}

bool SttEngine::startAside(bool wakeWord)
{
    if (!readyToHear(false) || (!m_source && !openMicrophone())) {
        // Nothing to hear with: shut, and deaf to commands too.
        m_live = false;
        m_aside = false;
        listenForWakeWord(false);
        return false;
    }
    const bool wasListening = m_live && !m_aside;
    m_live = true;
    m_aside = true;
    // The orb is told nothing of a room it is not listening to: a phrase
    // being kept goes on being kept, but is no longer its business.
    if (wasListening && m_capturing)
        emit speechEnded();
    emit levelChanged(0.0);
    listenForWakeWord(wakeWord);
    BurnOS::liveLog(QStringLiteral("NOT LISTENING: the microphone stays open for spoken commands%1 "
                                   "- nothing else that is heard is answered, remembered or sent "
                                   "anywhere")
                        .arg(wakeWord ? QStringLiteral(" and the wake word") : QString()));
    return true;
}

void SttEngine::listenForWakeWord(bool on)
{
    if (!on || !BurnOS::Wake::available()) {
        if (m_wakeWord) {
            m_wakeWord = false;
            BurnOS::Wake::stop();
        }
        return;
    }
    // Started afresh each time: the wake word may have been changed.
    m_wakeWord = true;
    BurnOS::Wake::start(this, [this]() {
        if (!m_live || !m_aside || !m_wakeWord)
            return;
        BurnOS::liveLog(QStringLiteral("WAKE WORD heard by the keyword model"));
        emit wakeWordHeard();
    });
}

bool SttEngine::isReading() const
{
    if (m_checking > 0 || (m_pending && m_pendingKind != Heard::Aside))
        return true;
    for (const Phrase &phrase : m_queue) {
        if (phrase.kind != Heard::Aside)
            return true;
    }
    return false;
}

void SttEngine::pauseListening()
{
    if (!m_source)
        return;

    closeMicrophone();
    // A wake word the keyword model is still working out is nobody's now.
    // It listens again when the microphone opens again.
    if (m_wakeWord)
        BurnOS::Wake::stop();
    m_speech.clear();
    m_preroll.clear();
    m_queue.clear();
    m_silenceBytes = 0;
    m_voicedBytes = 0;
    m_attackBytes = 0;
    // A phrase cut off here is over, and nothing will come of it.
    if (m_capturing) {
        m_capturing = false;
        if (!m_aside) {
            emit speechEnded();
            emit ignored();
        }
    }
    BurnOS::liveLog(QStringLiteral("the microphone is shut for a moment"));
}

bool SttEngine::resumeListening()
{
    if (!m_live)
        return false;
    if (m_source)
        return true;

    if (!openMicrophone())
        return false;

    if (m_aside) {
        // Muted or waiting all along: commands are heard again - and the wake
        // word, as it is in Settings now.
        if (m_wakeWord)
            listenForWakeWord(true);
        BurnOS::liveLog(QStringLiteral("spoken commands are heard again"));
        return true;
    }
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
        // Unless it was armed while the key was down: then it is in use.
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
    transcribe(phrase, Heard::Held);
    return true;
}

bool SttEngine::isCapturingNear() const
{
    if (!m_capturing)
        return false;
    float floor = m_quietFloor;
    if (BurnOS::Settings::voiceCheckOn() && BurnOS::Voice::available())
        floor = qMax(floor, float(BurnOS::Settings::voiceLevel()) * kOwnerShare);
    return m_nearLevel >= floor;
}

void SttEngine::setOverVoice(bool on)
{
    if (m_overVoice == on)
        return;
    m_overVoice = on;
    m_bargeSteps = 0;
    m_overVoiceBytes = 0;
    // THE ROOM'S VOICE IS LET GO WHEN BURNOS STARTS TO SPEAK. A television
    // that was being kept as a phrase would otherwise go on being kept under
    // the answer, and while a phrase is being kept nobody can talk over the
    // voice: that takes the listening that follows here.
    if (on && m_capturing && !m_aside && !isCapturingNear()) {
        BurnOS::liveLog(QStringLiteral("let go: %1 ms of a far quieter voice, as the answer starts")
                            .arg(bytesToMs(m_speech.size())));
        m_capturing = false;
        m_speech.clear();
        m_preroll.clear();
        m_silenceBytes = 0;
        m_voicedBytes = 0;
        m_farBytes = 0;
        m_attackBytes = 0;
        emit speechEnded();
        emit ignored();
    }
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
    // Not listening, the orb is told nothing of how loud the room is.
    connect(m_tap, &AudioLevelTap::levelChanged, this, [this](qreal level) {
        if (!m_aside)
            emit levelChanged(level);
    });
    // With a file standing in for the microphone, the real one is not heard.
    connect(m_tap, &AudioLevelTap::audioChunk, this, [this](const QByteArray &pcm) {
        if (m_fileMic.isEmpty())
            onAudio(pcm);
    });
    if (!m_fileMic.isEmpty() && !m_fileClock.isValid())
        m_fileClock.start();

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

    resetPhrase();

    BurnOS::liveLog(QStringLiteral("microphone open: %1").arg(device.description()));
    return true;
}

void SttEngine::resetPhrase()
{
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
            QTimer::singleShot(0, this, [this, part]() { transcribe(part, Heard::Held); });
        }
        return;
    }
    if (!m_live || !m_source)
        return;
    // WAITING FOR THE WAKE WORD: the keyword model is given the sound as well,
    // so that it can say so the moment the wake word has been said - while
    // the phrase it began goes on being kept below, like any other.
    if (m_aside && m_wakeWord)
        BurnOS::Wake::feed(pcm);

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
            m_nearLevel = rms;
            m_farBytes = 0;
            m_silenceBytes = 0;
            m_attackBytes = 0;
            emit bargeIn();
            if (!m_aside)
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
        m_nearLevel = rms;
        m_farBytes = 0;
        m_silenceBytes = 0;
        m_attackBytes = 0;
        BurnOS::liveLog(QStringLiteral("speech STARTED, Silero %1 over %2")
                            .arg(double(speech), 0, 'f', 3)
                            .arg(double(openAt), 0, 'f', 2));
        if (!m_aside)
            emit speechStarted();
        return;
    }

    // How much of the phrase is speech at all is counted as it always was:
    // that is what tells a word from a cough.
    if (speech >= closeAt)
        m_voicedBytes += kVadChunkBytes;
    if (speech >= closeAt && rms >= m_nearLevel * kFarShare) {
        // The voice this phrase is made of. How loud it is follows it.
        m_nearLevel += (rms - m_nearLevel) * kNearFollow;
        m_silenceBytes = 0;
    } else {
        // Quiet - or speech far quieter than that voice, which is somebody
        // further away and counts as quiet.
        if (speech >= closeAt)
            m_farBytes += kVadChunkBytes;
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
    const int farMs = bytesToMs(m_farBytes);
    // Who it is for is settled as it ends: a phrase that the wake word was
    // said in is for the agent, though it began while Burnos was waiting.
    const Heard kind = m_aside ? Heard::Aside : Heard::Live;
    m_speech.clear();
    m_capturing = false;
    m_silenceBytes = 0;
    m_voicedBytes = 0;
    m_farBytes = 0;
    if (kind == Heard::Live)
        emit speechEnded();

    if (voicedMs < kMinSpeechMs) {
        BurnOS::liveLog(QStringLiteral("discarded: only %1 ms of voice").arg(voicedMs));
        // Nothing will come of it, and whoever is waiting for this phrase to be
        // read has to be told so - or the orb goes on thinking about a cough.
        // Deferred: this is the audio device's own write call.
        if (kind == Heard::Live)
            QTimer::singleShot(0, this, [this]() { emit ignored(); });
        return;
    }

    BurnOS::liveLog(QStringLiteral("speech ENDED after %1 ms of quiet, %2 ms of voice%3%4")
                        .arg(tooLong ? 0 : hangoverMs)
                        .arg(voicedMs)
                        .arg(farMs > 0 ? QStringLiteral(" (%1 ms of it a far quieter voice, which "
                                                        "did not keep the phrase open)").arg(farMs)
                                       : QString())
                        .arg(tooLong ? QStringLiteral(" (length cap)") : QString()));

    // The microphone STAYS OPEN: whatever the user adds while we work is folded
    // into the same question by the Agent. Deferred by one turn of the event
    // loop, because we are inside the audio device's own write call.
    QTimer::singleShot(0, this, [this, phrase, farMs, kind]() { admit(phrase, farMs, kind); });
}

void SttEngine::admit(const QByteArray &phrase, int farMs, Heard kind)
{
    // THE PART SAID AT THE MICROPHONE - the loud part of the phrase. With a
    // television on, a phrase is often the television, then the owner, then
    // the television again; half a second of a far quieter voice in it says
    // that this is such a phrase.
    const BurnOS::Voice::Near part = BurnOS::Voice::nearPart(phrase, farMs >= kCrowdedMs);

    // WAKE WORD MODE, AWAKE: far quieter than whoever woke Burnos is the room
    // talking, not somebody asking it the next thing.
    if (kind == Heard::Live && m_quietFloor > 0.0f && part.level < m_quietFloor) {
        BurnOS::liveLog(QStringLiteral("ignored: far quieter than whoever woke Burnos (%1% of the "
                                       "least it answers) - somebody further away")
                            .arg(int(100.0f * part.level / m_quietFloor)));
        emit ignored();
        return;
    }

    // WHAT GOES TO THE RECOGNISER IS WHAT WAS SAID AT THE MICROPHONE, with a
    // moment either side: "mute", said into the middle of the television's
    // sentence, is still the whole of what was said. Handed the television's
    // words as well, the recogniser has been seen to write THOSE down and
    // leave out the louder voice altogether. The pauses inside the near
    // voice stay, so no word is run into the next; in a quiet room all this
    // takes off is the silence at the end.
    const qsizetype from = qMax<qsizetype>(0, part.from - msToBytes(kKeepBeforeMs));
    const qsizetype to = qMin<qsizetype>(phrase.size(), part.to + msToBytes(kKeepAfterMs));
    const int before = bytesToMs(from);
    const int after = bytesToMs(phrase.size() - to);
    const bool trimmed = before + after >= 300;
    // In a crowded phrase the other voice is silenced as well, wherever the
    // near one is not speaking: left in, in the moment before a lone
    // "unmute", the recogniser wrote "Called the code".
    // Only for a phrase the length of a command. A sentence is read well
    // enough with the television under it, and silencing its soft sounds
    // would cost a dictation its plurals.
    // And only with a full second of the far voice in it: in a quiet room a
    // short phrase has up to 600 ms of its own soft sounds counted as one,
    // and gated there "Open my notes" lost its last letter for one voice.
    const bool gate = part.ms < kShortPhraseMs && farMs >= kGateFarMs
                      && part.gated.size() == phrase.size();
    if (gate)
        BurnOS::liveLog(QStringLiteral("to the recogniser: the other voice in the room silenced "
                                       "around what was said (%1 ms of it)").arg(farMs));
    const QByteArray &whole = gate ? part.gated : phrase;
    const QByteArray said = trimmed ? whole.mid(from, to - from) : whole;
    const QString leftOut = trimmed ? QStringLiteral("%1 ms before it and %2 ms after it left out")
                                          .arg(before).arg(after)
                                    : QString();

    // No voice taught, the check switched off, or its model not on disk: every
    // phrase goes to the recogniser, as before there was a check.
    const BurnOS::Voice::Print owner = BurnOS::Settings::voiceCheckOn()
                                           ? BurnOS::Settings::voicePrint()
                                           : BurnOS::Voice::Print();
    if (owner.isEmpty() || !BurnOS::Voice::available()) {
        m_lastLevel = part.level;
        // Said only when it is more than the silence that ends every phrase.
        if (before + after >= 1500)
            BurnOS::liveLog(QStringLiteral("to the recogniser: what was said at the microphone - %1")
                                .arg(leftOut));
        transcribe(said, kind);
        return;
    }

    // WHOSE VOICE WAS THAT - AT THE MICROPHONE. Judged whole, a phrase with a
    // television in it sounds like nobody: the owner was turned away. So it
    // is the part said at the microphone that is judged, for whose voice it
    // is and for how loud it is. The voiceprint is made off this thread -
    // tens of milliseconds - and the phrase waits for it, in order.
    if (kind != Heard::Aside)
        ++m_checking;
    BurnOS::Voice::print(this, part.pcm, [this, said, leftOut, part, owner, kind](
                                             const BurnOS::Voice::Print &print) {
        if (kind != Heard::Aside)
            --m_checking;
        const double alike = double(BurnOS::Voice::alike(owner, print));
        const double needed = BurnOS::Settings::voiceThreshold();
        const double own = BurnOS::Settings::voiceLevel();
        const double share = own > 0.0 ? double(part.level) / own : 0.0;
        const bool isShort = part.ms < kShortPhraseMs;
        const bool loud = share >= kLoudShare;

        bool mine = false;
        QString verdict;
        if (print.isEmpty() && !isShort) {
            // The check itself failed. Better heard than deaf: let it through.
            // (Of a word or two there may simply be too little for a
            // voiceprint; that is judged on loudness, below.)
            mine = true;
            verdict = QStringLiteral("NO VOICEPRINT could be made - let through");
        } else if (alike >= needed) {
            mine = true;
            verdict = QStringLiteral("YOU");
        } else if (isShort && loud) {
            mine = true;
            verdict = QStringLiteral("short, and as loud as you - let through");
        } else if (!isShort && loud && alike >= needed - kLoudMargin) {
            mine = true;
            verdict = QStringLiteral("close to your voice, and as loud as you - let through");
        } else {
            verdict = isShort ? QStringLiteral("short and too quiet to be you - IGNORED")
                              : QStringLiteral("NOT YOU - IGNORED");
        }

        BurnOS::liveLog(QStringLiteral("voice check: %1 (alike %2, needs %3; %4 ms of voice at "
                                       "%5% of your loudness%6)")
                            .arg(verdict)
                            .arg(alike, 0, 'f', 2)
                            .arg(needed, 0, 'f', 2)
                            .arg(part.ms)
                            .arg(int(share * 100.0))
                            .arg(leftOut.isEmpty() ? QString() : QStringLiteral("; ") + leftOut));
        if (mine) {
            m_lastLevel = part.level;
            transcribe(said, kind);
        } else if (kind != Heard::Aside) {
            emit ignored();
        }
    });
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

void SttEngine::deliver(const QString &text, Heard kind)
{
    // What was heard while not listening is not written down here: whoever
    // looks for a command in it says what came of it, and only that.
    if (kind == Heard::Aside) {
        emit overheard(text);
        return;
    }
    BurnOS::liveLog(QStringLiteral("transcribed: %1")
                        .arg(text.isEmpty() ? QStringLiteral("(nothing)") : text));
    if (kind == Heard::Held)
        emit dictated(text);
    else
        emit transcribed(text);
}

void SttEngine::transcribe(const QByteArray &pcm, Heard kind)
{
    if (m_pending) {
        // One recogniser at a time. Now that the microphone stays open while we
        // work, a second phrase can arrive before the first has been read, and
        // it waits its turn rather than overwriting the wav that is still open.
        m_queue.append({pcm, kind});
        BurnOS::liveLog(QStringLiteral("queued %1 ms behind the recogniser")
                            .arg(bytesToMs(pcm.size())));
        return;
    }

    // THE LOADED RECOGNISER, off this thread. Only when it cannot be used does
    // the phrase go the old way, through a WAV file and parakeet-cli.
    if (recogniser().usable()) {
        m_pending = true;
        m_pendingKind = kind;
        BurnOS::liveLog(QStringLiteral("transcribing %1 ms of audio").arg(bytesToMs(pcm.size())));
        const QPointer<SttEngine> self(this);
        recogniserPool().start([self, pcm, kind]() {
            QString text;
            const bool ok = recogniser().run(pcm, &text);
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, pcm, kind, ok, text]() {
                if (!self)
                    return;
                self->m_pending = false;
                if (!ok) {
                    self->transcribe(pcm, kind); // usable() is false now: parakeet-cli
                    return;
                }
                self->deliver(text, kind);
                self->drainQueue();
            }, Qt::QueuedConnection);
        });
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
                    deliver(out, m_pendingKind);
                    drainQueue();
                });
    }

    m_pending = true;
    m_pendingKind = kind;

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
    const Phrase next = m_queue.takeFirst();
    transcribe(next.pcm, next.kind);
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

void SttEngine::preload()
{
    if (!recogniser().usable() || !BurnOS::Models::sttPresent())
        return;
    recogniserPool().start([]() {
        QElapsedTimer timer;
        timer.start();
        // Half a second of silence: loads the model and runs it once.
        QString text;
        if (recogniser().run(QByteArray(16000, '\0'), &text))
            BurnOS::liveLog(QStringLiteral("recogniser: speech model loaded at startup in %1 ms - "
                                           "it stays loaded").arg(timer.elapsed()));
    });
}

bool SttEngine::voiceProbe(const QStringList &wavs, const QString &outPath)
{
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    bool ok = BurnOS::Voice::available();
    if (!ok)
        out.write("FAILED: the voice library or the voice model is not there\n");
    BurnOS::Voice::Print first;
    for (const QString &wav : wavs) {
        if (!ok)
            break;
        QString error;
        const QByteArray pcm = readPcm(wav, &error);
        QElapsedTimer timer;
        timer.start();
        const BurnOS::Voice::Print print =
            pcm.isEmpty() ? BurnOS::Voice::Print() : BurnOS::Voice::printNow(pcm, &error);
        if (print.isEmpty()) {
            out.write(QStringLiteral("FAILED: %1: %2\n").arg(QFileInfo(wav).fileName(), error).toUtf8());
            ok = false;
            break;
        }
        out.write(QStringLiteral("%1  %2 s  voiceprint in %3 ms  %4\n")
                      .arg(QFileInfo(wav).fileName(), -26)
                      .arg(pcm.size() / double(kBytesPerSecond), 0, 'f', 1)
                      .arg(timer.elapsed())
                      .arg(first.isEmpty()
                               ? QStringLiteral("(the reference)")
                               : QStringLiteral("alike %1").arg(double(BurnOS::Voice::alike(first, print)), 0, 'f', 3))
                      .toUtf8());
        if (first.isEmpty())
            first = print;
    }
    return ok;
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
