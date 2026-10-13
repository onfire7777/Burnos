#include "keywordspotter.h"

#include "installer.h"
#include "livelog.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QStringList>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace BurnOS::Wake {

namespace {

constexpr int kSampleRate = 16000;
// A tenth of a second at a time goes to the model: the microphone hands over
// 20 ms, and five of those are one piece of work instead of five.
constexpr qsizetype kChunkBytes = kSampleRate / 10 * 2;
// More than this waiting means the model cannot keep up; what is behind is
// dropped rather than answered seconds late.
constexpr int kMaxWaiting = 50;

// HOW READILY IT FIRES. Measured with BurnOS's eleven voices saying the wake
// word, near misses and ordinary sentences (_research/wake-word/kws_bench.py):
// at these two the wake word was caught for 11 voices of 11 and nothing fired
// on 66 ordinary sentences. A higher boost fires on more that was not meant.
constexpr float kBoost = 1.0f;
constexpr float kThreshold = 0.25f;

// A CLEAN STREAM FOR WHAT IS SAID, see Engine::hear. Something begins when a
// tenth of a second is this many times louder than the loudest tenth of the
// 1.2 s before it, and at least this loud; the three tenths before it are
// given to the stream again; and the stream is not reset again for a second,
// so that a reset never falls into the keyword it was made for. The loudest
// of the last 1.2 s, and not an average: with a television on, an average
// sinks between its syllables and its own next syllable set the reset off
// thirteen times a minute - the wake word was then missed more often over
// the television than with no reset at all (kws_onset_check2.py).
constexpr float kOnsetTimes = 4.0f;
constexpr float kOnsetLeast = 0.003f;
constexpr int kOnsetWindow = 12;
constexpr int kOnsetLead = 3;
constexpr int kOnsetHold = 10;

// As sherpa-onnx's C API lays them out (c-api.h, v1.13.8). The layout was
// proved against the shipped library with _research/wake-word/kws-probe.cpp.
struct TransducerFiles
{
    const char *encoder;
    const char *decoder;
    const char *joiner;
};
struct ParaformerFiles
{
    const char *encoder;
    const char *decoder;
};
struct OneFile
{
    const char *model;
};
struct ModelConfig
{
    TransducerFiles transducer;
    ParaformerFiles paraformer;
    OneFile zipformer2Ctc;
    const char *tokens;
    int32_t numThreads;
    const char *provider;
    int32_t debug;
    const char *modelType;
    const char *modelingUnit;
    const char *bpeVocab;
    const char *tokensBuf;
    int32_t tokensBufSize;
    OneFile nemoCtc;
    OneFile tOneCtc;
};
struct FeatureConfig
{
    int32_t sampleRate;
    int32_t featureDim;
};
struct SpotterConfig
{
    FeatureConfig features;
    ModelConfig model;
    int32_t maxActivePaths;
    int32_t numTrailingBlanks;
    float keywordsScore;
    float keywordsThreshold;
    const char *keywordsFile;
    const char *keywordsBuf;
    int32_t keywordsBufSize;
};
struct KeywordResult
{
    const char *keyword;
    const char *tokens;
    const char *const *tokensArr;
    int32_t count;
    float *timestamps;
    float startTime;
    const char *json;
};

// What the model hands back when the wake word is heard.
constexpr auto kWakeLabel = "WAKE";

// In the folder of the voice, for the reason voicecheck.cpp gives: the
// onnxruntime.dll it needs is there, and System32 has an older one.
QString libraryPath()
{
    return QCoreApplication::applicationDirPath()
           + QStringLiteral("/kokoro-tts/sherpa-onnx-c-api.dll");
}

// ---------------------------------------------------------------------------
// SPELLING. The model does not take words: a keyword is a row of the 500 word
// pieces it was trained on ("HEY BURNOS" is "_HE Y _B UR N O S"), and it has
// to be the row the model itself would write. That is decided by bpe.model, a
// sentencepiece unigram model: every piece has a score, and the right split
// of a phrase is the one whose scores add up highest. The file is a protobuf
// and is read by hand here - piece, score, kind - which is all of it this
// needs. Checked against sentencepiece itself: the same split for 60 words of
// 60 (kws_bench.py, part 1).
struct Speller
{
    QMutex lock;
    bool tried = false;
    QHash<QString, float> score;
    int longest = 1;

    static bool varint(const QByteArray &b, qsizetype &at, quint64 &value)
    {
        value = 0;
        for (int shift = 0; at < b.size() && shift < 64; shift += 7) {
            const quint8 c = quint8(b.at(at++));
            value |= quint64(c & 0x7f) << shift;
            if (c < 0x80)
                return true;
        }
        return false;
    }

    struct Field
    {
        int number = 0;
        int wire = 0;
        quint64 value = 0;
        QByteArray bytes;
    };

    static bool next(const QByteArray &b, qsizetype &at, Field &f)
    {
        quint64 key = 0;
        if (!varint(b, at, key))
            return false;
        f.number = int(key >> 3);
        f.wire = int(key & 7);
        f.value = 0;
        f.bytes.clear();
        const auto take = [&](qsizetype n) {
            if (n < 0 || at + n > b.size())
                return false;
            f.bytes = b.mid(at, n);
            at += n;
            return true;
        };
        switch (f.wire) {
        case 0:
            return varint(b, at, f.value);
        case 1:
            return take(8);
        case 2: {
            quint64 n = 0;
            return varint(b, at, n) && n < quint64(b.size()) + 1 && take(qsizetype(n));
        }
        case 5:
            return take(4);
        default:
            return false;
        }
    }

    // Called with the lock held.
    bool load()
    {
        if (tried)
            return !score.isEmpty();
        QFile file(Models::wakeDir() + QStringLiteral("/bpe.model"));
        // Not there yet on a first run: looked for again the next time.
        if (!file.open(QIODevice::ReadOnly))
            return false;
        tried = true;
        const QByteArray all = file.readAll();
        qsizetype at = 0;
        Field top;
        while (at < all.size() && next(all, at, top)) {
            if (top.number != 1 || top.wire != 2)
                continue;
            // One piece: its text, its score, and its kind - 1, or absent, for
            // a piece that can be written; the rest are markers.
            QString piece;
            float value = 0.0f;
            quint64 kind = 1;
            qsizetype in = 0;
            Field f;
            while (in < top.bytes.size() && next(top.bytes, in, f)) {
                if (f.number == 1 && f.wire == 2)
                    piece = QString::fromUtf8(f.bytes);
                else if (f.number == 2 && f.wire == 5)
                    std::memcpy(&value, f.bytes.constData(), 4);
                else if (f.number == 3 && f.wire == 0)
                    kind = f.value;
            }
            if (kind == 1 && !piece.isEmpty()) {
                score.insert(piece, value);
                longest = qMax(longest, int(piece.size()));
            }
        }
        return !score.isEmpty();
    }

    // The pieces of `phrase`, or none when it cannot be written in them.
    QStringList spell(const QString &phrase)
    {
        QMutexLocker locker(&lock);
        if (!load())
            return {};
        // As the model was trained: capitals, and a mark in front of each word.
        QString text;
        for (const QString &word : phrase.toUpper().split(QLatin1Char(' '), Qt::SkipEmptyParts))
            text += QChar(0x2581) + word;
        const int n = int(text.size());
        if (n == 0)
            return {};
        std::vector<double> best(size_t(n) + 1, -1e18);
        std::vector<int> from(size_t(n) + 1, 0);
        best[0] = 0.0;
        for (int i = 0; i < n; ++i) {
            if (best[size_t(i)] < -1e17)
                continue;
            for (int j = i + 1; j <= qMin(n, i + longest); ++j) {
                const auto found = score.constFind(text.mid(i, j - i));
                if (found == score.constEnd())
                    continue;
                const double total = best[size_t(i)] + double(found.value());
                if (total > best[size_t(j)]) {
                    best[size_t(j)] = total;
                    from[size_t(j)] = i;
                }
            }
        }
        if (best[size_t(n)] < -1e17)
            return {};
        QStringList pieces;
        for (int j = n; j > 0; j = from[size_t(j)])
            pieces.prepend(text.mid(from[size_t(j)], j - from[size_t(j)]));
        return pieces;
    }
};

Speller &speller()
{
    static Speller one;
    return one;
}

// Letters and apostrophes, one space between words: what can be spelled.
QString plainWords(const QString &phrase)
{
    QString out;
    for (const QChar c : phrase) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z'))
            || (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) || c == QLatin1Char('\''))
            out.append(c);
        else
            out.append(QLatin1Char(' '));
    }
    return out.simplified();
}

// One line of a keyword list: the pieces, then the label handed back.
QByteArray keywordLine(const QString &words, const char *label)
{
    const QStringList pieces = speller().spell(words);
    if (pieces.isEmpty())
        return {};
    return pieces.join(QLatin1Char(' ')).toUtf8() + " @" + label;
}

// THE WAKE WORD, in every way the model may hear it. Burnos is not a word the
// model was trained on, and "burn-oss" is written by it as BURN US as often as
// BURNOS (measured: 9 voices of 11) - so that spelling listens too.
QStringList wakeSpellings(const QString &phrase)
{
    const QString words = plainWords(phrase).toUpper();
    QStringList all{words};
    if (words.split(QLatin1Char(' ')).contains(QStringLiteral("BURNOS"))) {
        QString other = QLatin1Char(' ') + words + QLatin1Char(' ');
        other.replace(QStringLiteral(" BURNOS "), QStringLiteral(" BURN US "));
        all.append(other.simplified());
    }
    return all;
}

// ---------------------------------------------------------------------------
// ONE ENGINE for the process, loaded once and kept. Everything in it belongs
// to the one worker thread; the main thread only ever posts work to it.
struct Engine
{
    bool failed = false; // the library or the model is there and will not load
    bool bound = false;  // the library is loaded and its functions found
    QString error;
    const void *spotter = nullptr;
    QString madeFor;     // the wake word the spotter was made with
    QString listensFor;  // and how it is spelled for it, for the log
    const void *stream = nullptr;
    int streamTurn = 0; // the start() this stream was made for

    const void *(*create)(const SpotterConfig *) = nullptr;
    void (*destroy)(const void *) = nullptr;
    const void *(*createStream)(const void *) = nullptr;
    int32_t (*isReady)(const void *, const void *) = nullptr;
    void (*decode)(const void *, const void *) = nullptr;
    void (*reset)(const void *, const void *) = nullptr;
    const KeywordResult *(*result)(const void *, const void *) = nullptr;
    void (*destroyResult)(const KeywordResult *) = nullptr;
    void (*accept)(const void *, int32_t, const float *, int32_t) = nullptr;
    void (*destroyStream)(const void *) = nullptr;

    template <typename T>
    static bool find(HMODULE lib, const char *name, T &fn)
    {
        fn = reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(lib, name)));
        return fn != nullptr;
    }

    // The spotter for `wake`, made if there is none or it was made for another
    // wake word. The wake word is the only thing the model listens for, and it
    // is given its list when it is made, so another wake word is another
    // spotter - a second and a half, once, off the main thread.
    bool load(const QString &wake)
    {
        if (spotter && madeFor == wake)
            return true;
        if (failed)
            return false;
        if (!available()) {
            // Not on disk yet - the first run is still downloading it. Not a
            // failure: the next call looks again.
            error = QStringLiteral("the keyword model is not on disk yet");
            return false;
        }
        failed = true; // until proved otherwise below

        if (!bound) {
            const QString path = QDir::toNativeSeparators(libraryPath());
            HMODULE lib = LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
                                         LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!lib) {
                error = QStringLiteral("%1 would not load (error %2)").arg(path).arg(GetLastError());
                return false;
            }
            const bool found =
                find(lib, "SherpaOnnxCreateKeywordSpotter", create)
                && find(lib, "SherpaOnnxDestroyKeywordSpotter", destroy)
                && find(lib, "SherpaOnnxCreateKeywordStream", createStream)
                && find(lib, "SherpaOnnxIsKeywordStreamReady", isReady)
                && find(lib, "SherpaOnnxDecodeKeywordStream", decode)
                && find(lib, "SherpaOnnxResetKeywordStream", reset)
                && find(lib, "SherpaOnnxGetKeywordResult", result)
                && find(lib, "SherpaOnnxDestroyKeywordResult", destroyResult)
                && find(lib, "SherpaOnnxOnlineStreamAcceptWaveform", accept)
                && find(lib, "SherpaOnnxDestroyOnlineStream", destroyStream);
            if (!found) {
                error = QStringLiteral("%1 has no keyword spotter in it").arg(path);
                return false;
            }
            bound = true;
        }

        // The wake word, in every way the model may hear it.
        QByteArray keywords;
        QStringList spelled;
        for (const QString &words : wakeSpellings(wake)) {
            const QByteArray line = keywordLine(words, kWakeLabel);
            if (!line.isEmpty()) {
                keywords += line + '\n';
                spelled.append(words.toLower());
            }
        }
        if (keywords.isEmpty()) {
            // Settings refuses a wake word the model cannot spell; one that got
            // in some other way is no reason to fail. The recogniser still
            // finds it in what it writes.
            failed = false;
            error = QStringLiteral("the keyword model cannot spell '%1'").arg(wake);
            return false;
        }

        dropStream();
        if (spotter)
            destroy(spotter);
        spotter = nullptr;

        const QString dir = QDir::toNativeSeparators(Models::wakeDir());
        const QByteArray encoder = (dir + QStringLiteral("\\") + Models::wakeEncoder()).toUtf8();
        const QByteArray decoder = (dir + QStringLiteral("\\") + Models::wakeDecoder()).toUtf8();
        const QByteArray joiner = (dir + QStringLiteral("\\") + Models::wakeJoiner()).toUtf8();
        const QByteArray tokens = (dir + QStringLiteral("\\tokens.txt")).toUtf8();
        SpotterConfig config;
        std::memset(&config, 0, sizeof(config));
        config.features.sampleRate = kSampleRate;
        config.features.featureDim = 80;
        config.model.transducer.encoder = encoder.constData();
        config.model.transducer.decoder = decoder.constData();
        config.model.transducer.joiner = joiner.constData();
        config.model.tokens = tokens.constData();
        config.model.numThreads = 1;
        config.model.provider = "cpu";
        config.maxActivePaths = 4;
        config.numTrailingBlanks = 1;
        config.keywordsScore = kBoost;
        config.keywordsThreshold = kThreshold;
        config.keywordsBuf = keywords.constData();
        config.keywordsBufSize = int32_t(keywords.size());
        spotter = create(&config);
        if (!spotter) {
            error = QStringLiteral("the keyword model would not load from %1").arg(dir);
            return false;
        }
        madeFor = wake;
        listensFor = spelled.join(QStringLiteral("' or '"));
        failed = false;
        return true;
    }

    // THE WAKE WORD STARTS ON A CLEAN STREAM. The library resets a keyword
    // stream by itself after a second and a half without a word, again and
    // again while the room is quiet - and a keyword that begins in the few
    // tenths of a second before one of those resets loses its beginning and
    // is missed, about one time in six, always at the same place in that
    // second and a half. So the reset is done HERE, at the right moment: when
    // something far louder than the room has been begins, the stream is reset
    // and given the last 300 ms again, and whatever was said starts at the
    // start of a stream. Measured with three voices and every place in that
    // second and a half: the wake word 48 of 48, where it was 44 before
    // (_research/wake-word/kws_onset_check.py).
    QList<float> room;        // how loud each of the last tenths of a second was
    int held = 0;             // pieces to go before another reset is allowed
    QList<QByteArray> recent; // the last pieces heard, to be given again

    void dropStream()
    {
        if (stream)
            destroyStream(stream);
        stream = nullptr;
        room.clear();
        held = 0;
        recent.clear();
    }

    bool newStream()
    {
        dropStream();
        stream = createStream(spotter);
        return stream != nullptr;
    }

    void give(const QByteArray &pcm)
    {
        const qsizetype count = pcm.size() / 2;
        const auto *raw = reinterpret_cast<const qint16 *>(pcm.constData());
        std::vector<float> samples(static_cast<size_t>(count));
        for (qsizetype i = 0; i < count; ++i)
            samples[static_cast<size_t>(i)] = raw[i] / 32768.0f;
        accept(stream, kSampleRate, samples.data(), int32_t(count));
    }

    // Whether the wake word was heard in `pcm`, a tenth of a second.
    bool hear(const QByteArray &pcm)
    {
        const qsizetype count = pcm.size() / 2;
        const auto *raw = reinterpret_cast<const qint16 *>(pcm.constData());
        double square = 0.0;
        for (qsizetype i = 0; i < count; ++i)
            square += double(raw[i]) * double(raw[i]);
        const float level = count ? float(std::sqrt(square / double(count)) / 32768.0) : 0.0f;

        // Something far louder than the room has been begins: a clean stream
        // for it, and what led up to it given again.
        held = qMax(0, held - 1);
        if (!room.isEmpty() && held == 0) {
            const float loudest = *std::max_element(room.cbegin(), room.cend());
            if (level > qMax(kOnsetTimes * loudest, kOnsetLeast)) {
                reset(spotter, stream);
                for (const QByteArray &old : std::as_const(recent))
                    give(old);
                held = kOnsetHold;
            }
        }
        room.append(level);
        if (room.size() > kOnsetWindow)
            room.removeFirst();
        recent.append(pcm);
        if (recent.size() > kOnsetLead)
            recent.removeFirst();

        give(pcm);
        bool heard = false;
        while (isReady(spotter, stream)) {
            decode(spotter, stream);
            const KeywordResult *r = result(spotter, stream);
            if (!r)
                continue;
            if (r->keyword && r->keyword[0])
                heard = true;
            destroyResult(r);
            // The stream is not reset here, after a hit: it is reported once
            // with or without that, and the one reset that matters is the one
            // before the wake word, above.
        }
        return heard;
    }
};

Engine &engine()
{
    static Engine one;
    return one;
}

// One piece of work at a time, in the order the sound came.
QThreadPool &pool()
{
    static QThreadPool *one = [] {
        auto *made = new QThreadPool(QCoreApplication::instance());
        made->setMaxThreadCount(1);
        return made;
    }();
    return *one;
}

// The start() that is current. A keyword heard for an earlier one - before a
// stop, or before the one that was acted on - is nobody's any more.
std::atomic_int g_turn{0};
std::atomic_int g_waiting{0};
// Main thread only.
QPointer<QObject> g_guard;
std::function<void()> g_heard;
QByteArray g_pending;

// How many single letters have to change to turn one word into the other.
int lettersApart(const QString &a, const QString &b)
{
    std::vector<int> row(size_t(b.size()) + 1);
    for (qsizetype j = 0; j <= b.size(); ++j)
        row[size_t(j)] = int(j);
    for (qsizetype i = 1; i <= a.size(); ++i) {
        int diagonal = row[0];
        row[0] = int(i);
        for (qsizetype j = 1; j <= b.size(); ++j) {
            const int above = row[size_t(j)];
            row[size_t(j)] = std::min({above + 1, row[size_t(j - 1)] + 1,
                                       diagonal + (a.at(i - 1) == b.at(j - 1) ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[size_t(b.size())];
}

} // namespace

bool available()
{
    // BURNOS_NO_KEYWORD_MODEL, for the check that the wake word is still found
    // in what the recogniser writes when this model does not hear it - which,
    // for some voices, it does not. Never set in a normal run.
    if (qEnvironmentVariableIsSet("BURNOS_NO_KEYWORD_MODEL"))
        return false;
    return Models::wakePresent() && QFileInfo::exists(libraryPath());
}

void preload()
{
    if (!available())
        return;
    const QString wake = Settings::wakeWord();
    pool().start([wake]() {
        Engine &e = engine();
        if (e.spotter || e.failed)
            return;
        QElapsedTimer timer;
        timer.start();
        if (!e.load(wake)) {
            liveLog(QStringLiteral("wake word: NOT ready - %1. The wake word is still found in "
                                   "what the recogniser writes.").arg(e.error));
            return;
        }
        // A second of faint noise through it, so that the model has run once
        // and the first real second is as quick as every later one.
        if (e.newStream()) {
            QByteArray warm(int(kChunkBytes), '\0');
            auto *samples = reinterpret_cast<qint16 *>(warm.data());
            quint32 seed = 12345;
            for (int round = 0; round < 10; ++round) {
                for (qsizetype i = 0; i < kChunkBytes / 2; ++i) {
                    seed = seed * 1664525u + 1013904223u;
                    samples[i] = qint16(int(seed >> 22) - 512);
                }
                e.hear(warm);
            }
            e.dropStream();
        }
        liveLog(QStringLiteral("wake word: keyword model loaded at startup in %1 ms - it stays "
                               "loaded").arg(timer.elapsed()));
    });
}

void start(QObject *guard, std::function<void()> heard)
{
    g_guard = guard;
    g_heard = std::move(heard);
    g_pending.clear();
    const int turn = ++g_turn;
    const QString wake = Settings::wakeWord();
    pool().start([turn, wake]() {
        Engine &e = engine();
        // Stopped, or started afresh, before this came to its turn - a mute
        // undone within the moment, the first click of a double click: there
        // is nobody left to listen for.
        if (g_turn.load() != turn)
            return;
        const bool made = !e.spotter || e.madeFor != wake;
        QElapsedTimer timer;
        timer.start();
        if (!e.load(wake)) {
            liveLog(QStringLiteral("wake word: the keyword model is not listening - %1. The wake "
                                   "word is still found in what the recogniser writes.")
                        .arg(e.error));
            return;
        }
        if (made)
            liveLog(QStringLiteral("wake word: keyword model made for '%1' in %2 ms - it stays "
                                   "loaded").arg(wake).arg(timer.elapsed()));
        if (!e.newStream()) {
            liveLog(QStringLiteral("wake word: the keyword model gave no stream"));
            return;
        }
        if (g_turn.load() != turn) {
            e.dropStream();
            return;
        }
        e.streamTurn = turn;
        liveLog(QStringLiteral("wake word: the keyword model listens for '%1'").arg(e.listensFor));
    });
}

void feed(const QByteArray &pcm)
{
    if (!g_heard)
        return;
    g_pending.append(pcm);
    if (g_pending.size() < kChunkBytes)
        return;
    const QByteArray chunk = g_pending;
    g_pending.clear();
    if (g_waiting.load() >= kMaxWaiting)
        return;
    ++g_waiting;
    const int turn = g_turn.load();
    pool().start([turn, chunk]() {
        --g_waiting;
        Engine &e = engine();
        if (!e.stream || e.streamTurn != turn)
            return;
        if (!e.hear(chunk))
            return;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [turn]() {
            if (g_turn.load() != turn || !g_guard || !g_heard)
                return;
            g_heard();
        }, Qt::QueuedConnection);
    });
}

void stop()
{
    ++g_turn;
    g_guard.clear();
    g_heard = nullptr;
    g_pending.clear();
    // The stream goes; the model stays.
    pool().start([]() { engine().dropStream(); });
}

bool canSpell(const QString &phrase)
{
    const QString words = plainWords(phrase);
    // Under four letters there is too little to tell from any other sound,
    // and anything that is not a plain letter the model has no piece for.
    if (words != phrase.simplified() || QString(words).remove(QLatin1Char(' ')).size() < 4)
        return false;
    return !speller().spell(words).isEmpty();
}

QString withoutWakeWord(const QString &text, bool anywhere, bool *found)
{
    *found = false;
    // The wake word as one row of letters: the recogniser writes "Burnos" as
    // one word, as two, or a letter out, and all of those are it.
    QString wake;
    for (const QChar c : Settings::wakeWord().toLower()) {
        if (c.isLetterOrNumber())
            wake.append(c);
    }
    if (wake.size() < 4)
        return text;
    // How far off the recogniser's writing of it may be. For any phrase, a
    // letter in every four: "Hey Burn us" and "Hey Bernows" are it. For a
    // phrase the keyword model has already heard the wake word in, it is known
    // to be in there, and a letter in every three is let pass.
    const int allowed = anywhere ? qMax(2, int(wake.size()) / 3) : qMax(1, int(wake.size()) / 4);
    // And how much of it may be missing altogether. Unheard by the keyword
    // model, the writing alone decides whether Burnos wakes, and "Hey, burn
    // those old papers" must not: all but one letter of it has to be there.
    const int missing = anywhere ? allowed : 1;

    // The words of what was said, each with where it ends in the text.
    struct Word
    {
        QString letters;
        qsizetype end = 0;
    };
    QList<Word> words;
    Word current;
    for (qsizetype i = 0; i <= text.size(); ++i) {
        const bool space = i == text.size() || text.at(i).isSpace();
        if (!space) {
            if (text.at(i).isLetterOrNumber())
                current.letters.append(text.at(i).toLower());
            continue;
        }
        if (!current.letters.isEmpty()) {
            current.end = i;
            words.append(current);
        }
        current = Word();
    }

    const int wakeWords = int(Settings::wakeWord().simplified().count(QLatin1Char(' '))) + 1;
    const qsizetype lastStart = anywhere ? qMin<qsizetype>(words.size() - 1, 12) : 0;
    int bestApart = allowed + 1;
    qsizetype bestEnd = -1;
    for (qsizetype start = 0; start <= lastStart && start < words.size(); ++start) {
        QString run;
        for (qsizetype count = 1; count <= wakeWords + 2 && start + count <= words.size(); ++count) {
            run += words.at(start + count - 1).letters;
            if (run.size() > wake.size() + allowed)
                break;
            if (run.size() < wake.size() - missing)
                continue;
            const int apart = lettersApart(run, wake);
            // The closest match; of two as close, the one that ends later.
            if (apart < bestApart || (apart == bestApart && bestEnd >= 0
                                      && words.at(start + count - 1).end > bestEnd)) {
                bestApart = apart;
                bestEnd = words.at(start + count - 1).end;
            }
        }
    }
    if (bestEnd < 0) {
        // Woken, and no more was said than the wake word is long: it was the
        // wake word, however the recogniser wrote it.
        if (anywhere && !words.isEmpty() && words.size() <= wakeWords) {
            *found = true;
            return QString();
        }
        return text;
    }
    *found = true;
    QString rest = text.mid(bestEnd).trimmed();
    while (!rest.isEmpty() && !rest.at(0).isLetterOrNumber())
        rest.remove(0, 1);
    return rest.trimmed();
}

} // namespace BurnOS::Wake
