#include "installer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
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

QString root()
{
    // %ProgramData% is the supported way to C:/ProgramData; the literal path
    // is only a fallback for a stripped environment.
    const QString data = qEnvironmentVariable("ProgramData", QStringLiteral("C:/ProgramData"));
    return QDir::fromNativeSeparators(data) + QStringLiteral("/BurnOS");
}

QString sttDir() { return root() + QStringLiteral("/STT"); }
QString ttsDir() { return root() + QStringLiteral("/TTS"); }
QString ttsArchive() { return ttsDir() + QStringLiteral("/kokoro-en-v0_19.tar.bz2"); }

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

// The three downloads, in order. Each is skipped when it is on disk already.
struct Step
{
    bool (*present)();
    const char *url;
    QString (*dest)();
    const char *stage;
    bool extract;
    bool checked; // against kVadSha256
};

const Step kSteps[] = {
    {BurnOS::Models::sttPresent, kSttUrl, BurnOS::Models::sttModel,
     QT_TRANSLATE_NOOP("ModelInstaller", "Downloading speech recognition"), false, false},
    {BurnOS::Models::ttsPresent, kTtsUrl, ttsArchive,
     QT_TRANSLATE_NOOP("ModelInstaller", "Downloading voice"), true, false},
    {BurnOS::Models::vadPresent, kVadUrl, BurnOS::Models::vadModel,
     QT_TRANSLATE_NOOP("ModelInstaller", "Downloading voice detector"), false, true},
};
constexpr int kStepCount = int(sizeof(kSteps) / sizeof(kSteps[0]));

} // namespace

namespace BurnOS::Models {

QString sttModel() { return sttDir() + QLatin1Char('/') + QLatin1String(kSttFile); }
QString vadModel() { return sttDir() + QLatin1Char('/') + QLatin1String(kVadFile); }
QString kokoroDir() { return ttsDir() + QStringLiteral("/kokoro-en-v0_19"); }

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
    m_expectSha = step.checked ? QByteArray(kVadSha256) : QByteArray();
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
        emit progress(tr("Unpacking voice"), -1.0);
        QString error;
        if (!extract(m_destPath, &error)) {
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
    tar.start(QStringLiteral("tar"),
              {QStringLiteral("-xf"), archivePath, QStringLiteral("-C"), ttsDir()});
    if (!tar.waitForStarted(10000)) {
        *error = tr("tar is not available to unpack the voice.");
        return false;
    }
    // 329 MB of model to write out; give it room.
    if (!tar.waitForFinished(300000)) {
        tar.kill();
        *error = tr("Unpacking the voice timed out.");
        return false;
    }
    if (tar.exitCode() != 0) {
        *error = tr("tar failed: %1")
                     .arg(QString::fromUtf8(tar.readAllStandardError()).simplified().left(200));
        return false;
    }
    if (!BurnOS::Models::ttsPresent()) {
        *error = tr("The voice unpacked but is missing files.");
        return false;
    }
    return true;
}
