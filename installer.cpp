#include "installer.h"
#include "livelog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUrl>

namespace {

// The exact q8_0 build the bundled parakeet-cli can load. Do not swap this
// for f16 or a newer conversion without testing: the CLI is version fussy.
// Its name differs from parakeet-cli's own default on purpose - always -m.
constexpr auto kSttUrl =
    "https://huggingface.co/ggml-org/parakeet-GGUF/resolve/main/"
    "ggml-parakeet-tdt-0.6b-v3-q8_0.bin";
constexpr auto kSttFile = "ggml-parakeet-tdt-0.6b-v3-q8_0.bin";

// Self contained Kokoro bundle: model, voices, tokens and espeak-ng-data.
constexpr auto kTtsUrl =
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/"
    "kokoro-en-v0_19.tar.bz2";

// The voice detector: Silero 6.2.0 as whisper.cpp converted it (MIT), under
// 1 MB. Checked against a fixed SHA256 once it lands, because it is read by
// hand rather than by a library that would notice a bad file itself.
constexpr auto kVadUrl =
    "https://huggingface.co/ggml-org/whisper-vad/resolve/main/"
    "ggml-silero-v6.2.0.bin";
constexpr auto kVadFile = "ggml-silero-v6.2.0.bin";
constexpr qint64 kVadBytes = 885098;
constexpr auto kVadSha256 =
    "2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987";

// The voice check: 3D-Speaker's ERes2Net trained on VoxCeleb (Apache 2.0), as
// sherpa-onnx publishes it, with the fingerprint from that release's own
// checksum file. Burnos starts without it - only the voice check needs it.
constexpr auto kVoiceUrl =
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/"
    "3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx";
constexpr auto kVoiceFile = "3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx";
constexpr qint64 kVoiceBytes = 26485263;
constexpr auto kVoiceSha256 =
    "c59158379255ad66e161679cca6af8d52d51e389e3224ab7d7a7baae295c2db5";

// The wake word: sherpa-onnx's small English keyword model (a zipformer trained
// on GigaSpeech, Apache 2.0), with the fingerprint from that release's own
// checksum file. An archive of 17 MB that unpacks into a folder of its own
// name; three of its files are the model, and tokens.txt and bpe.model say how
// a keyword is spelled. Burnos starts without it - only the wake word needs it.
constexpr auto kWakeUrl =
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/"
    "sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01.tar.bz2";
constexpr auto kWakeFolder = "sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01";
constexpr auto kWakeSha256 =
    "f170013b4716e41b62b9bfd809687c207cef798ef9bc6534d524e17af9b6561a";
constexpr auto kWakeEncoder = "encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx";
constexpr auto kWakeDecoder = "decoder-epoch-12-avg-2-chunk-16-left-64.onnx";
constexpr auto kWakeJoiner = "joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx";
constexpr qint64 kWakeEncoderBytes = 4807159;
constexpr qint64 kWakeDecoderBytes = 1063189;
constexpr qint64 kWakeJoinerBytes = 163380;
constexpr qint64 kWakeTokensBytes = 5006;
constexpr qint64 kWakeSpellingBytes = 244837;

QString root()
{
    static const QString path = []() {
        const QString override = qEnvironmentVariable("BURNOS_MODEL_DIR");
        if (!override.isEmpty())
            return QDir::fromNativeSeparators(override);
        // Reuse a complete older installation without writing into ProgramData.
        const QString old = QDir::fromNativeSeparators(qEnvironmentVariable(
            "ProgramData", QStringLiteral("C:/ProgramData"))) + QStringLiteral("/BurnOS");
        const QString wake = old + QStringLiteral("/STT/") + QLatin1String(kWakeFolder) + QLatin1Char('/');
        const auto whole = [](const QString &file, qint64 bytes) {
            const QFileInfo info(file);
            return info.isFile() && (bytes ? info.size() == bytes : info.size() > 0);
        };
        if (whole(old + QStringLiteral("/STT/") + QLatin1String(kSttFile), 0)
            && whole(old + QStringLiteral("/STT/") + QLatin1String(kVadFile), kVadBytes)
            && whole(old + QStringLiteral("/STT/") + QLatin1String(kVoiceFile), kVoiceBytes)
            && whole(wake + QLatin1String(kWakeEncoder), kWakeEncoderBytes)
            && whole(wake + QLatin1String(kWakeDecoder), kWakeDecoderBytes)
            && whole(wake + QLatin1String(kWakeJoiner), kWakeJoinerBytes)
            && whole(wake + QStringLiteral("tokens.txt"), kWakeTokensBytes)
            && whole(wake + QStringLiteral("bpe.model"), kWakeSpellingBytes)
            && whole(old + QStringLiteral("/TTS/kokoro-en-v0_19/model.onnx"), 0)
            && whole(old + QStringLiteral("/TTS/kokoro-en-v0_19/voices.bin"), 0)
            && whole(old + QStringLiteral("/TTS/kokoro-en-v0_19/tokens.txt"), 0)
            && QFileInfo(old + QStringLiteral("/TTS/kokoro-en-v0_19/espeak-ng-data")).isDir())
            return old;
        QString local = qEnvironmentVariable("LOCALAPPDATA");
        if (local.isEmpty())
            local = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        return QDir::fromNativeSeparators(local) + QStringLiteral("/BurnOS/models");
    }();
    return path;
}

QString sttDir() { return root() + QStringLiteral("/STT"); }
QString ttsDir() { return root() + QStringLiteral("/TTS"); }
QString ttsArchive() { return ttsDir() + QStringLiteral("/kokoro-en-v0_19.tar.bz2"); }
QString wakeArchive()
{
    return sttDir() + QLatin1Char('/') + QLatin1String(kWakeFolder) + QStringLiteral(".tar.bz2");
}

// Made if missing, and PROVED WRITABLE: a folder made by another user exists
// and is still no use to download into.
bool ensureDir(const QString &path, QString *error)
{
    const QString native = QDir::toNativeSeparators(path);
    if (!QDir().mkpath(path)) {
        *error = QObject::tr("Could not create %1.").arg(native);
        return false;
    }
    QTemporaryFile probe(path + QStringLiteral("/.burnos-XXXXXX"));
    if (!probe.open()) {
        *error = QObject::tr("%1 exists but is not writable.").arg(native);
        return false;
    }
    return true;
}

// The downloads, in order. Each is skipped when it is on disk already. One
// that is an archive is unpacked into the folder it was downloaded to.
struct Step
{
    bool (*present)();
    const char *url;
    QString (*dest)();
    const char *stage;
    bool extract;
    const char *sha256; // the fingerprint it must have, or null for none
    bool optional;      // Burnos starts without it
};

const Step kSteps[] = {
    {BurnOS::Models::sttPresent, kSttUrl, BurnOS::Models::sttModel,
     QT_TRANSLATE_NOOP("ModelInstaller", "Downloading speech recognition"), false, nullptr, false},
    {BurnOS::Models::ttsPresent, kTtsUrl, ttsArchive,
     QT_TRANSLATE_NOOP("ModelInstaller", "Downloading voice"), true, nullptr, false},
    {BurnOS::Models::vadPresent, kVadUrl, BurnOS::Models::vadModel,
     QT_TRANSLATE_NOOP("ModelInstaller", "Downloading voice detector"), false, kVadSha256, false},
    {BurnOS::Models::voicePresent, kVoiceUrl, BurnOS::Models::voiceModel,
     QT_TRANSLATE_NOOP("ModelInstaller", "Downloading voice check"), false, kVoiceSha256, true},
    {BurnOS::Models::wakePresent, kWakeUrl, wakeArchive,
     QT_TRANSLATE_NOOP("ModelInstaller", "Downloading wake word"), true, kWakeSha256, true},
};
constexpr int kStepCount = int(sizeof(kSteps) / sizeof(kSteps[0]));

} // namespace

namespace BurnOS::Models {

QString sttModel() { return sttDir() + QLatin1Char('/') + QLatin1String(kSttFile); }
QString vadModel() { return sttDir() + QLatin1Char('/') + QLatin1String(kVadFile); }
QString kokoroDir() { return ttsDir() + QStringLiteral("/kokoro-en-v0_19"); }
QString voiceModel() { return sttDir() + QLatin1Char('/') + QLatin1String(kVoiceFile); }

QString wakeDir() { return sttDir() + QLatin1Char('/') + QLatin1String(kWakeFolder); }
QString wakeEncoder() { return QString::fromLatin1(kWakeEncoder); }
QString wakeDecoder() { return QString::fromLatin1(kWakeDecoder); }
QString wakeJoiner() { return QString::fromLatin1(kWakeJoiner); }

bool voicePresent()
{
    const QFileInfo info(voiceModel());
    return info.isFile() && info.size() == kVoiceBytes;
}

bool wakePresent()
{
    const QString dir = wakeDir() + QLatin1Char('/');
    const auto whole = [&dir](const QString &name, qint64 bytes) {
        const QFileInfo info(dir + name);
        return info.isFile() && info.size() == bytes;
    };
    return whole(wakeEncoder(), kWakeEncoderBytes) && whole(wakeDecoder(), kWakeDecoderBytes)
           && whole(wakeJoiner(), kWakeJoinerBytes)
           && whole(QStringLiteral("tokens.txt"), kWakeTokensBytes)
           && whole(QStringLiteral("bpe.model"), kWakeSpellingBytes);
}

bool sttPresent()
{
    const QFileInfo info(sttModel());
    return info.isFile() && info.size() > 0;
}

bool vadPresent()
{
    const QFileInfo info(vadModel());
    return info.isFile() && info.size() == kVadBytes;
}

bool ttsPresent()
{
    const QString dir = kokoroDir();
    return QFileInfo::exists(dir + QStringLiteral("/model.onnx"))
           && QFileInfo::exists(dir + QStringLiteral("/voices.bin"))
           && QFileInfo::exists(dir + QStringLiteral("/tokens.txt"))
           && QFileInfo(dir + QStringLiteral("/espeak-ng-data")).isDir();
}

} // namespace BurnOS::Models

ModelInstaller::ModelInstaller(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

bool ModelInstaller::everythingPresent()
{
    for (const Step &step : kSteps) {
        if (!step.present())
            return false;
    }
    return true;
}

void ModelInstaller::start()
{
    m_step = 0;
    next();
}

void ModelInstaller::next()
{
    while (m_step < kStepCount && kSteps[m_step].present())
        ++m_step;
    if (m_step == kStepCount) {
        emit progress(tr("Ready"), 1.0);
        emit finished();
        return;
    }

    const Step &step = kSteps[m_step++];
    const QString dest = step.dest();
    QString error;
    if (!ensureDir(QFileInfo(dest).absolutePath(), &error)) {
        emit failed(error);
        return;
    }
    m_expectSha = QByteArray(step.sha256 ? step.sha256 : "");
    m_optional = step.optional;
    m_present = step.present;
    download(QUrl(QString::fromLatin1(step.url)), dest, tr(step.stage), step.extract);
}

void ModelInstaller::download(const QUrl &url, const QString &destPath,
                              const QString &stage, bool extractAfter)
{
    m_stage = stage;
    m_destPath = destPath;
    m_extractAfter = extractAfter;

    delete m_file;
    m_file = new QFile(destPath + QStringLiteral(".part"), this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit failed(tr("Cannot write %1").arg(QDir::toNativeSeparators(m_file->fileName())));
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "Burnos");

    emit progress(m_stage, -1.0);
    m_reply = m_net->get(request);

    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        if (m_file && m_reply)
            m_file->write(m_reply->readAll());
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        emit progress(m_stage, total > 0 ? qreal(got) / qreal(total) : -1.0);
    });
    connect(m_reply, &QNetworkReply::finished, this, &ModelInstaller::onFinished);
}

void ModelInstaller::onFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        m_file->close();
        m_file->remove();
        if (m_optional) {
            BurnOS::liveLog(QStringLiteral("setup: %1 failed (%2) - going on without it")
                                .arg(m_stage, reply->errorString()));
            next();
            return;
        }
        emit failed(reply->errorString());
        return;
    }

    // Anything still buffered belongs in the file before it is renamed.
    m_file->write(reply->readAll());
    m_file->close();

    // A file with a known fingerprint is checked before it is let in.
    if (!m_expectSha.isEmpty()) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const bool read = m_file->open(QIODevice::ReadOnly) && hash.addData(m_file);
        m_file->close();
        if (!read || hash.result().toHex() != m_expectSha) {
            m_file->remove();
            if (m_optional) {
                BurnOS::liveLog(QStringLiteral("setup: %1 arrived damaged - going on without it")
                                    .arg(m_stage));
                next();
                return;
            }
            emit failed(tr("The voice detector download was damaged. Restart Burnos "
                           "to fetch it again."));
            return;
        }
    }

    QFile::remove(m_destPath);
    if (!m_file->rename(m_destPath)) {
        emit failed(tr("Could not finish writing %1").arg(QDir::toNativeSeparators(m_destPath)));
        return;
    }

    if (m_extractAfter) {
        emit progress(tr("Unpacking"), -1.0);
        QString error;
        if (!extract(m_destPath, &error)) {
            if (m_optional) {
                QFile::remove(m_destPath);
                BurnOS::liveLog(QStringLiteral("setup: %1 would not unpack (%2) - going on "
                                               "without it").arg(m_stage, error));
                next();
                return;
            }
            emit failed(error);
            return;
        }
        QFile::remove(m_destPath); // the archive is dead weight once unpacked
    }

    next();
}

bool ModelInstaller::extract(const QString &archivePath, QString *error)
{
    // Windows ships bsdtar, which reads .tar.bz2 without any extra tooling.
    QProcess tar;
    // Into the folder the archive is in: each one holds a folder of its own.
    tar.start(QStringLiteral("tar"),
              {QStringLiteral("-xf"), archivePath, QStringLiteral("-C"),
               QFileInfo(archivePath).absolutePath()});
    if (!tar.waitForStarted(10000)) {
        *error = tr("tar is not available to unpack the download.");
        return false;
    }
    // 329 MB of voice model to write out; give it room.
    if (!tar.waitForFinished(300000)) {
        tar.kill();
        *error = tr("Unpacking timed out.");
        return false;
    }
    if (tar.exitCode() != 0) {
        *error = tr("tar failed: %1")
                     .arg(QString::fromUtf8(tar.readAllStandardError()).simplified().left(200));
        return false;
    }
    if (m_present && !m_present()) {
        *error = tr("The download unpacked but is missing files.");
        return false;
    }
    return true;
}
