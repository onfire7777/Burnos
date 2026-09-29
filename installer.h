#ifndef INSTALLER_H
#define INSTALLER_H

#include <QObject>
#include <QString>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

// Fetches the speech models on first run. Both are large, so everything is
// streamed to disk as it arrives rather than held in memory, and each file is
// written to a .part and only renamed once complete - a half downloaded model
// that looks finished is worse than no model at all.
class ModelInstaller : public QObject
{
    Q_OBJECT

public:
    explicit ModelInstaller(QObject *parent = nullptr);
    ~ModelInstaller() override;

    // True when nothing needs downloading and the app can start normally.
    static bool everythingPresent();

    void start();
    void cancel();

signals:
    // fraction is 0.0 to 1.0, or negative when the size is not yet known.
    void progress(const QString &stage, qreal fraction);
    void finished();
    void failed(const QString &reason);

private:
    void next();
    void download(const QUrl &url, const QString &destPath,
                  const QString &stage, bool extractAfter);
    void onFinished();
    bool extract(const QString &archivePath, const QString &destDir,
                 QString *error);

    QNetworkAccessManager *m_net = nullptr;
    QNetworkReply *m_reply = nullptr;
    QFile *m_file = nullptr;
    QString m_stage;
    QString m_destPath;
    bool m_extractAfter = false;
    // The SHA256 the file being downloaded must have, hex; empty for none.
    QByteArray m_expectSha;
    int m_step = 0;
    bool m_cancelled = false;
};

#endif // INSTALLER_H
