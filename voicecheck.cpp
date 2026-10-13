#include "voicecheck.h"

#include "installer.h"
#include "livelog.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace BurnOS::Voice {

namespace {

constexpr int kSampleRate = 16000;
// A voice is plain within a few seconds; more of the phrase only costs time.
constexpr int kMaxSeconds = 12;
// The detector's own step, so a loudness measured here means what one
// measured in sttengine.cpp means.
constexpr int kFrame = 512;

// THE NEAR VOICE, see nearPart(). The loud end of a phrase is its loudest 160 ms
// - not a share of its frames, because "mute" said into ten seconds of
// television is a twentieth of the phrase and still the loudest thing in it.
// A frame belongs to the near voice at this share of that, and so do the
// frames this close to one. Measured with stand-in voices and a television
// voice before, under and after them (_research/voice-check/tv_bench.py).
constexpr int kTopFrames = 5;
constexpr float kNearShare = 0.30f;
constexpr int kNearSpread = 4;
// WHERE the near voice begins and ends is what goes to the recogniser, and a
// soft first or last word must not be cut off: in a room with no other voice
// it is drawn generously, at an eighth of the loud end, and all that falls
// outside is silence. With another, quieter voice in the phrase - a
// television - it is drawn where the near voice itself is, because at an
// eighth, and at a fifth, the television's loudest syllables still got in and
// its words were read along with the speaker's.
constexpr float kSpanShare = 0.12f;
constexpr float kSpanShareCrowded = 0.30f;
// In a crowded phrase, what lies between the near voice's own sounds goes to
// the recogniser at this share of its loudness: none of it. At a fifth the
// recogniser still wrote the television's words down, at nothing a lone
// "unmute" over a television came back as "Unmute." (tv-test.ps1).
constexpr float kFarGain = 0.0f;
// And what counts as the near voice's own sound there: anything within reach
// of a moment at this share of its loudest. At 0.15 a television at 15% of
// the speaker's loudness stayed in, and an "unmute" over it came back as
// "Call the code"; at 0.30 it came back as "Unmute.". The price is a last
// soft "s" - "Open settings" was written "Open setting" for two voices of
// eleven - which is why only a phrase the length of a command is gated
// (SttEngine::admit), and why the commands are matched without it.
constexpr float kGateShare = 0.30f;

// As sherpa-onnx's C API lays it out (c-api.h, v1.13.8).
struct ExtractorConfig
{
    const char *model;
    int32_t numThreads;
    int32_t debug;
    const char *provider;
};

// In the folder of the voice, because the onnxruntime.dll it needs is there
// already - the very same file - and Windows keeps an older one of its own in
// System32 that must not be picked up instead.
QString libraryPath()
{
    return QCoreApplication::applicationDirPath()
           + QStringLiteral("/kokoro-tts/sherpa-onnx-c-api.dll");
}

// ONE ENGINE for the process, loaded the first time a voiceprint is wanted and
// kept. The library is found by name at run time, so Burnos links nothing new
// and starts exactly as before when the file is not there.
struct Engine
{
    QMutex lock;
    bool tried = false;
    QString error;
    const void *extractor = nullptr;
    int dim = 0;

    const void *(*create)(const ExtractorConfig *) = nullptr;
    int32_t (*dimOf)(const void *) = nullptr;
    const void *(*createStream)(const void *) = nullptr;
    int32_t (*isReady)(const void *, const void *) = nullptr;
    const float *(*compute)(const void *, const void *) = nullptr;
    void (*destroyEmbedding)(const float *) = nullptr;
    void (*accept)(const void *, int32_t, const float *, int32_t) = nullptr;
    void (*inputFinished)(const void *) = nullptr;
    void (*destroyStream)(const void *) = nullptr;

    template <typename T>
    static bool find(HMODULE lib, const char *name, T &fn)
    {
        fn = reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(lib, name)));
        return fn != nullptr;
    }

    // Called with the lock held.
    bool load()
    {
        if (tried)
            return extractor != nullptr;
        tried = true;
        QElapsedTimer timer;
        timer.start();

        const QString path = QDir::toNativeSeparators(libraryPath());
        // ALTERED SEARCH PATH: what the library needs is looked for in its own
        // folder first.
        HMODULE lib = LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
                                     LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!lib) {
            error = QStringLiteral("%1 would not load (error %2)").arg(path).arg(GetLastError());
            return false;
        }
        const bool found =
            find(lib, "SherpaOnnxCreateSpeakerEmbeddingExtractor", create)
            && find(lib, "SherpaOnnxSpeakerEmbeddingExtractorDim", dimOf)
            && find(lib, "SherpaOnnxSpeakerEmbeddingExtractorCreateStream", createStream)
            && find(lib, "SherpaOnnxSpeakerEmbeddingExtractorIsReady", isReady)
            && find(lib, "SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding", compute)
            && find(lib, "SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding", destroyEmbedding)
            && find(lib, "SherpaOnnxOnlineStreamAcceptWaveform", accept)
            && find(lib, "SherpaOnnxOnlineStreamInputFinished", inputFinished)
            && find(lib, "SherpaOnnxDestroyOnlineStream", destroyStream);
        if (!found) {
            error = QStringLiteral("%1 is not the library this was written for").arg(path);
            return false;
        }

        const QByteArray model = QDir::toNativeSeparators(Models::voiceModel()).toUtf8();
        const ExtractorConfig config{model.constData(), 2, 0, "cpu"};
        extractor = create(&config);
        if (!extractor) {
            error = QStringLiteral("the voice model would not load from %1")
                        .arg(QString::fromUtf8(model));
            return false;
        }
        dim = dimOf(extractor);
        liveLog(QStringLiteral("voice check: model loaded in %1 ms, %2 numbers to a voiceprint")
                    .arg(timer.elapsed())
                    .arg(dim));
        return true;
    }
};

Engine &engine()
{
    static Engine one;
    return one;
}

// One at a time, in the order asked: a phrase must not overtake the one before.
QThreadPool &pool()
{
    static QThreadPool *one = [] {
        auto *made = new QThreadPool(QCoreApplication::instance());
        made->setMaxThreadCount(1);
        return made;
    }();
    return *one;
}

} // namespace

bool available()
{
    return Models::voicePresent() && QFileInfo::exists(libraryPath());
}

void preload()
{
    if (!available())
        return;
    pool().start([]() {
        // A second of faint noise: enough to load the model and run it once.
        QByteArray warm(32000, '\0');
        auto *samples = reinterpret_cast<qint16 *>(warm.data());
        quint32 seed = 12345;
        for (int i = 0; i < 16000; ++i) {
            seed = seed * 1664525u + 1013904223u;
            samples[i] = qint16(int(seed >> 22) - 512);
        }
        QElapsedTimer timer;
        timer.start();
        const Print made = printNow(warm);
        liveLog(QStringLiteral("voice check: %1 at startup in %2 ms - it stays loaded")
                    .arg(made.isEmpty() ? QStringLiteral("NOT ready") : QStringLiteral("ready"))
                    .arg(timer.elapsed()));
    });
}

Print printNow(const QByteArray &pcm, QString *error)
{
    Engine &e = engine();
    QMutexLocker locker(&e.lock);
    const bool firstTry = !e.tried;
    if (!e.load()) {
        if (firstTry)
            liveLog(QStringLiteral("voice check: %1").arg(e.error));
        if (error)
            *error = e.error;
        return {};
    }
    const qsizetype count = qMin<qsizetype>(pcm.size() / 2, qsizetype(kSampleRate) * kMaxSeconds);
    const auto *raw = reinterpret_cast<const qint16 *>(pcm.constData());
    std::vector<float> samples(static_cast<size_t>(count));
    for (qsizetype i = 0; i < count; ++i)
        samples[static_cast<size_t>(i)] = raw[i] / 32768.0f;

    Print made;
    const void *stream = e.createStream(e.extractor);
    e.accept(stream, kSampleRate, samples.data(), int32_t(count));
    e.inputFinished(stream);
    if (e.isReady(e.extractor, stream)) {
        if (const float *values = e.compute(e.extractor, stream)) {
            double square = 0.0;
            for (int i = 0; i < e.dim; ++i)
                square += double(values[i]) * double(values[i]);
            const double norm = square > 0.0 ? std::sqrt(square) : 1.0;
            made.reserve(e.dim);
            for (int i = 0; i < e.dim; ++i)
                made.append(float(values[i] / norm));
            e.destroyEmbedding(values);
        }
    } else if (error) {
        *error = QStringLiteral("too little audio for a voiceprint");
    }
    e.destroyStream(stream);
    return made;
}

void print(QObject *guard, const QByteArray &pcm, std::function<void(const Print &)> done)
{
    const QPointer<QObject> alive(guard);
    pool().start([alive, pcm, done]() {
        const Print made = printNow(pcm);
        // Back on the main thread, and only there is the guard looked at.
        QMetaObject::invokeMethod(QCoreApplication::instance(), [alive, done, made]() {
            if (alive)
                done(made);
        }, Qt::QueuedConnection);
    });
}

float alike(const Print &a, const Print &b)
{
    if (a.isEmpty() || a.size() != b.size())
        return -1.0f;
    double dot = 0.0;
    for (qsizetype i = 0; i < a.size(); ++i)
        dot += double(a[i]) * double(b[i]);
    return float(dot);
}

Spoken spoken(const QByteArray &pcm)
{
    Spoken result;
    const auto *raw = reinterpret_cast<const qint16 *>(pcm.constData());
    const qsizetype frames = pcm.size() / 2 / kFrame;
    if (frames < 4)
        return result;
    std::vector<float> loud(static_cast<size_t>(frames));
    for (qsizetype f = 0; f < frames; ++f) {
        double square = 0.0;
        for (int i = 0; i < kFrame; ++i) {
            const double s = raw[f * kFrame + i] / 32768.0;
            square += s * s;
        }
        loud[static_cast<size_t>(f)] = float(std::sqrt(square / kFrame));
    }
    // Speech is whatever comes within reach of the loud end of the recording;
    // the gaps between sentences fall far below it.
    std::vector<float> sorted = loud;
    std::sort(sorted.begin(), sorted.end());
    const float top = sorted[static_cast<size_t>(frames * 9 / 10)];
    // A tenth of it: a quarter threw away two thirds of a real reading, whose
    // soft consonants sit far below its vowels.
    const float floor = std::max(top * 0.10f, 0.002f);
    double sum = 0.0;
    int kept = 0;
    for (qsizetype f = 0; f < frames; ++f) {
        if (loud[static_cast<size_t>(f)] < floor)
            continue;
        result.pcm.append(pcm.constData() + f * kFrame * 2, kFrame * 2);
        sum += loud[static_cast<size_t>(f)];
        ++kept;
    }
    result.level = kept ? float(sum / kept) : 0.0f;
    return result;
}

Near nearPart(const QByteArray &pcm, bool crowded)
{
    Near result;
    result.to = pcm.size();
    const auto *raw = reinterpret_cast<const qint16 *>(pcm.constData());
    const qsizetype frames = pcm.size() / 2 / kFrame;
    if (frames < kTopFrames) {
        // Too little to choose from: all of it.
        result.pcm = pcm;
        result.ms = int(pcm.size() / 2 * 1000 / kSampleRate);
        return result;
    }
    std::vector<float> loud(static_cast<size_t>(frames));
    for (qsizetype f = 0; f < frames; ++f) {
        double square = 0.0;
        for (int i = 0; i < kFrame; ++i) {
            const double s = raw[f * kFrame + i] / 32768.0;
            square += s * s;
        }
        loud[static_cast<size_t>(f)] = float(std::sqrt(square / kFrame));
    }
    // The loud end: the loudest run of five frames, so that one click is not it.
    float top = 0.0f;
    float run = 0.0f;
    for (qsizetype f = 0; f < frames; ++f) {
        run += loud[static_cast<size_t>(f)];
        if (f >= kTopFrames)
            run -= loud[static_cast<size_t>(f - kTopFrames)];
        if (f >= kTopFrames - 1)
            top = std::max(top, run / kTopFrames);
    }
    const float floor = std::max(top * kNearShare, 0.0005f);
    const float spanFloor = std::max(top * (crowded ? kSpanShareCrowded : kSpanShare), 0.0005f);

    std::vector<char> keep(static_cast<size_t>(frames), 0);
    // What stays audible in the gated copy: more than is kept for the voiceprint.
    std::vector<char> open(static_cast<size_t>(frames), 0);
    const float gateFloor = std::max(top * kGateShare, 0.0005f);
    double sum = 0.0;
    int nearFrames = 0;
    qsizetype first = -1;
    qsizetype last = -1;
    for (qsizetype f = 0; f < frames; ++f) {
        const float level = loud[static_cast<size_t>(f)];
        if (level >= spanFloor) {
            if (first < 0)
                first = f;
            last = f;
        }
        if (level >= gateFloor) {
            const qsizetype a = std::max<qsizetype>(0, f - kNearSpread);
            const qsizetype b = std::min<qsizetype>(frames - 1, f + kNearSpread);
            for (qsizetype k = a; k <= b; ++k)
                open[static_cast<size_t>(k)] = 1;
        }
        if (level < floor)
            continue;
        sum += level;
        ++nearFrames;
        const qsizetype from = std::max<qsizetype>(0, f - kNearSpread);
        const qsizetype to = std::min<qsizetype>(frames - 1, f + kNearSpread);
        for (qsizetype k = from; k <= to; ++k)
            keep[static_cast<size_t>(k)] = 1;
    }
    int kept = 0;
    for (qsizetype f = 0; f < frames; ++f) {
        if (!keep[static_cast<size_t>(f)])
            continue;
        result.pcm.append(pcm.constData() + f * kFrame * 2, kFrame * 2);
        ++kept;
    }
    result.level = nearFrames ? float(sum / nearFrames) : 0.0f;
    result.ms = kept * kFrame * 1000 / kSampleRate;
    if (crowded && nearFrames > 0) {
        // The same phrase with the far voice turned down wherever the near one
        // is not speaking. The loudness changes over a whole frame, 32 ms, so
        // nothing clicks.
        result.gated = pcm;
        auto *out = reinterpret_cast<qint16 *>(result.gated.data());
        float gain = open[0] ? 1.0f : kFarGain;
        for (qsizetype f = 0; f < frames; ++f) {
            const float target = open[static_cast<size_t>(f)] ? 1.0f : kFarGain;
            for (int i = 0; i < kFrame; ++i) {
                const float g = gain + (target - gain) * float(i + 1) / kFrame;
                out[f * kFrame + i] = qint16(raw[f * kFrame + i] * g);
            }
            gain = target;
        }
        for (qsizetype i = frames * kFrame; i < pcm.size() / 2; ++i)
            out[i] = qint16(raw[i] * gain);
    }
    if (first >= 0) {
        result.from = std::max<qsizetype>(0, first - kNearSpread) * kFrame * 2;
        result.to = std::min<qsizetype>(frames, last + 1 + kNearSpread) * kFrame * 2;
    }
    return result;
}

} // namespace BurnOS::Voice
