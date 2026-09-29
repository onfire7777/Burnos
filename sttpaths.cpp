#include "sttpaths.h"

#include <QDir>
#include <QFileInfo>
#include <QLatin1Char>
#include <QProcessEnvironment>
#include <QTemporaryFile>

namespace BurnOS {
namespace Stt {
namespace {

// The model BurnOS ships with. Note this is the q8_0 quantization, so the
// name deliberately differs from parakeet-cli.exe's own default of
// models/ggml-parakeet-tdt-0.6b-v3.bin - always pass -m modelPath().
constexpr auto kModelFileName = "ggml-parakeet-tdt-0.6b-v3-q8_0.bin";

// Silero VAD 6.2.0, converted by whisper.cpp. Checked by size here and by
// SHA256 when it is downloaded.
constexpr auto kVadFileName = "ggml-silero-v6.2.0.bin";
constexpr qint64 kVadBytes = 885098;
constexpr auto kVadSha256 =
    "2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987";

QString programDataDir()
{
    // %ProgramData% is the supported way to reach C:/ProgramData. The
    // literal path is only a fallback for a stripped environment.
    QString root = QProcessEnvironment::systemEnvironment()
                       .value(QStringLiteral("ProgramData"));
    if (root.isEmpty())
        root = QStringLiteral("C:/ProgramData");
    return QDir::fromNativeSeparators(root);
}

} // namespace

QString dataDir()
{
    return programDataDir() + QStringLiteral("/BurnOS");
}

QString dir()
{
    return dataDir() + QStringLiteral("/STT");
}

QString modelPath()
{
    return dir() + QLatin1Char('/') + QLatin1String(kModelFileName);
}

bool ensureDir(QString *error)
{
    const QString path = dir();
    const QString native = QDir::toNativeSeparators(path);
    const QFileInfo info(path);

    if (info.exists() && !info.isDir()) {
        if (error)
            *error = QStringLiteral("%1 exists but is not a folder.").arg(native);
        return false;
    }

    if (!info.exists() && !QDir().mkpath(path)) {
        if (error)
            *error = QStringLiteral("Could not create %1.").arg(native);
        return false;
    }

    // Existing is not the same as usable: the folder may have been made by
    // another user, leaving us read only. Prove we can write before any
    // caller starts downloading a model into it.
    QTemporaryFile probe(path + QStringLiteral("/.burnos-XXXXXX"));
    if (!probe.open()) {
        if (error)
            *error = QStringLiteral("%1 exists but is not writable.").arg(native);
        return false;
    }

    return true;
}

bool modelPresent()
{
    const QFileInfo info(modelPath());
    return info.exists() && info.isFile() && info.size() > 0;
}

QString vadModelPath()
{
    return dir() + QLatin1Char('/') + QLatin1String(kVadFileName);
}

bool vadPresent()
{
    const QFileInfo info(vadModelPath());
    return info.exists() && info.isFile() && info.size() == kVadBytes;
}

QByteArray vadSha256()
{
    return QByteArray(kVadSha256);
}

qint64 vadBytes()
{
    return kVadBytes;
}

} // namespace Stt

namespace Tts {

QString dir()
{
    return Stt::dataDir() + QStringLiteral("/TTS");
}

QString modelDir()
{
    return dir() + QStringLiteral("/kokoro-en-v0_19");
}

QString modelPath()     { return modelDir() + QStringLiteral("/model.onnx"); }
QString voicesPath()    { return modelDir() + QStringLiteral("/voices.bin"); }
QString tokensPath()    { return modelDir() + QStringLiteral("/tokens.txt"); }
QString espeakDataDir() { return modelDir() + QStringLiteral("/espeak-ng-data"); }

bool ensureDir(QString *error)
{
    const QString path = dir();
    const QString native = QDir::toNativeSeparators(path);
    const QFileInfo info(path);

    if (info.exists() && !info.isDir()) {
        if (error)
            *error = QStringLiteral("%1 exists but is not a folder.").arg(native);
        return false;
    }

    if (!info.exists() && !QDir().mkpath(path)) {
        if (error)
            *error = QStringLiteral("Could not create %1.").arg(native);
        return false;
    }

    return true;
}

bool ready()
{
    return QFileInfo::exists(modelPath())
           && QFileInfo::exists(voicesPath())
           && QFileInfo::exists(tokensPath())
           && QFileInfo(espeakDataDir()).isDir();
}

} // namespace Tts
} // namespace BurnOS
