#include "installer.h"
#include "sttpaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QUrl>

namespace {

// The exact q8_0 build the bundled parakeet-cli can load. Do not swap this
// for f16 or a newer conversion without testing: the CLI is version fussy.
constexpr auto kSttUrl =
    "https://huggingface.co/ggml-org/parakeet-GGUF/resolve/main/"
    "ggml-parakeet-tdt-0.6b-v3-q8_0.bin";

// Self contained Kokoro bundle: model, voices, tokens and espeak-ng-data.
constexpr auto kTtsUrl =
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/"
    "kokoro-en-v0_19.tar.bz2";

} // namespace

ModelInstaller::ModelInstaller(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

ModelInstaller::~ModelInstaller() = default;

bool ModelInstaller::everythingPresent()
{
    return BurnOS::Stt::modelPresent() && BurnOS::Tts::ready();
}

void ModelInstaller::start()
{
    m_cancelled = false;
    m_step = 0;
    next();
}

void ModelInstaller::cancel()
{
    m_cancelled = true;
    if (m_reply)
        m_reply->abort();
}

void ModelInstaller::next()
{
    if (m_cancelled)
        return;

    switch (m_step++) {
    case 0:
        if (!BurnOS::Stt::modelPresent()) {
            QString error;
            if (!BurnOS::Stt::ensureDir(&error)) {
                emit failed(error);
                return;
            }
            download(QUrl(QString::fromLatin1(kSttUrl)),
                     BurnOS::Stt::modelPath(),
                     tr("Downloading speech recognition"), false);
            return;
        }
        next();
        return;

    case 1:
        if (!BurnOS::Tts::ready()) {
            QString error;
            if (!BurnOS::Tts::ensureDir(&error)) {
                emit failed(error);
                return;
            }
            download(QUrl(QString::fromLatin1(kTtsUrl)),
                     BurnOS::Tts::dir() + QStringLiteral("/kokoro-en-v0_19.tar.bz2"),
                     tr("Downloading voice"), true);
            return;
        }
        next();
        return;

    default:
        emit progress(tr("Ready"), 1.0);
        emit finished();
        return;
    }
}

void ModelInstaller::download(const QUrl &url, const QString &destPath,
                              const QString &stage, bool extractAfter)
{
    m_stage = stage;
    m_destPath = destPath;
    m_extractAfter = extractAfter;

    // Write to .part so an interrupted run never leaves a file that passes
    // the exists() check but is truncated.
    delete m_file;
    m_file = new QFile(destPath + QStringLiteral(".part"), this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit failed(tr("Cannot write %1")
                        .arg(QDir::toNativeSeparators(m_file->fileName())));
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

    connect(m_reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 got, qint64 total) {
                emit progress(m_stage,
                              total > 0 ? qreal(got) / qreal(total) : -1.0);
            });

    connect(m_reply, &QNetworkReply::finished, this, &ModelInstaller::onFinished);
}

void ModelInstaller::onFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    if (reply)
        reply->deleteLater();

    if (m_cancelled)
        return;

    if (!reply || reply->error() != QNetworkReply::NoError) {
        if (m_file) {
            m_file->close();
            m_file->remove();
        }
        emit failed(reply ? reply->errorString() : tr("Download failed."));
        return;
    }

    // Anything still buffered belongs in the file before it is renamed.
    if (m_file) {
        m_file->write(reply->readAll());
        m_file->flush();
        m_file->close();
    }

    QFile::remove(m_destPath);
    if (!m_file || !m_file->rename(m_destPath)) {
        emit failed(tr("Could not finish writing %1")
                        .arg(QDir::toNativeSeparators(m_destPath)));
        return;
    }

    if (m_extractAfter) {
        emit progress(tr("Unpacking voice"), -1.0);
        QString error;
        if (!extract(m_destPath, BurnOS::Tts::dir(), &error)) {
            emit failed(error);
            return;
        }
        QFile::remove(m_destPath); // the archive is dead weight once unpacked
    }

    next();
}

bool ModelInstaller::extract(const QString &archivePath, const QString &destDir,
                             QString *error)
{
    // Windows ships bsdtar, which reads .tar.bz2 without any extra tooling.
    QProcess tar;
    tar.start(QStringLiteral("tar"),
              {QStringLiteral("-xf"), archivePath,
               QStringLiteral("-C"), destDir});

    if (!tar.waitForStarted(10000)) {
        if (error)
            *error = tr("tar is not available to unpack the voice.");
        return false;
    }

    // 329 MB of model to write out; give it room.
    if (!tar.waitForFinished(300000)) {
        tar.kill();
        if (error)
            *error = tr("Unpacking the voice timed out.");
        return false;
    }

    if (tar.exitCode() != 0) {
        if (error)
            *error = tr("tar failed: %1")
                         .arg(QString::fromUtf8(tar.readAllStandardError())
                                  .simplified()
                                  .left(200));
        return false;
    }

    if (!BurnOS::Tts::ready()) {
        if (error)
            *error = tr("The voice unpacked but is missing files.");
        return false;
    }

    return true;
}
