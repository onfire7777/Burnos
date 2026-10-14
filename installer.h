#ifndef INSTALLER_H
#define INSTALLER_H

#include <QObject>
#include <QString>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

// THE MODELS: per-user downloads, or a complete legacy ProgramData cache, and whether each
// is there. None of them ships with the program; ModelInstaller fetches them.
namespace BurnOS::Models {

QString sttModel();   // Parakeet, for parakeet-cli -m
QString vadModel();   // the Silero 6.2.0 weights sttengine.cpp computes by hand
QString kokoroDir();  // model.onnx, voices.bin, tokens.txt and espeak-ng-data
QString voiceModel(); // the speaker model voicecheck.cpp tells voices apart with
// The keyword model keywordspotter.cpp hears the wake word with: its folder -
// which also holds tokens.txt and bpe.model - and the names of its three parts.
QString wakeDir();
QString wakeEncoder();
QString wakeDecoder();
QString wakeJoiner();

bool sttPresent();
bool vadPresent();    // checked by size, so a truncated file does not count
bool ttsPresent();
bool voicePresent();  // checked by size; Burnos starts without it
bool wakePresent();   // every part checked by size; Burnos starts without it

} // namespace BurnOS::Models

// Fetches whatever is missing on first run. Every file is streamed to a .part
// and only renamed once complete - a half downloaded model that looks
// finished is worse than no model at all.
class ModelInstaller : public QObject
{
    Q_OBJECT

public:
    explicit ModelInstaller(QObject *parent = nullptr);

    // True when nothing needs downloading and the app can start normally.
    static bool everythingPresent();

    void start();

signals:
    // fraction is 0.0 to 1.0, or negative when the size is not yet known.
    void progress(const QString &stage, qreal fraction);
    void finished();
    void failed(const QString &reason);

private:
    void next();
    void download(const QUrl &url, const QString &destPath, const QString &stage,
                  bool extractAfter);
    void onFinished();
    bool extract(const QString &archivePath, QString *error);

    QNetworkAccessManager *m_net = nullptr;
    QNetworkReply *m_reply = nullptr;
    QFile *m_file = nullptr;
    QString m_stage;
    QString m_destPath;
    bool m_extractAfter = false;
    // The file being downloaded is one Burnos can start without: a failure
    // is logged and passed over, and tried again on the next start.
    bool m_optional = false;
    // Says whether what is being downloaded has arrived whole, once unpacked.
    bool (*m_present)() = nullptr;
    // The SHA256 the file being downloaded must have, hex; empty for none.
    QByteArray m_expectSha;
    int m_step = 0;
};

#endif // INSTALLER_H
