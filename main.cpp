#include "mainwindow.h"
#include "orbwidget.h"
#include "sttengine.h"
#include "sttpaths.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStringList>

// Set by BurnOS.pro from its VERSION line; a build without it still runs.
#ifndef BURNOS_VERSION
#define BURNOS_VERSION "dev"
#endif

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setApplicationVersion(QStringLiteral(BURNOS_VERSION));

    // The orb IS the icon. Window, task bar and alt-tab all come out of the
    // very same paint code that draws the orb itself, so the two can never
    // drift apart the way a hand drawn asset would.
    a.setWindowIcon(OrbWidget::orbIcon());

    // Build helper: write that same orb out as a multi size .ico for the
    // executable's own resource, then exit without showing anything. The .pro
    // picks the file up on the next build.
    const QStringList args = a.arguments();
    const int exportAt = args.indexOf(QStringLiteral("--export-icon"));
    if (exportAt >= 0 && exportAt + 1 < args.size())
        return OrbWidget::writeIco(args.at(exportAt + 1)) ? 0 : 1;

    // Test helper: the voice detector's verdict on every 32 ms of a WAV,
    // written to a text file, so it can be checked against the official model.
    // A failure is written to the same file, since this program has no console.
    const int probeAt = args.indexOf(QStringLiteral("--vad-probe"));
    if (probeAt >= 0 && probeAt + 2 < args.size()) {
        QString error;
        if (SttEngine::probeFile(args.at(probeAt + 1), args.at(probeAt + 2), &error))
            return 0;
        QFile out(args.at(probeAt + 2));
        if (out.open(QIODevice::WriteOnly | QIODevice::Text))
            out.write("FAILED: " + error.toUtf8() + '\n');
        return 1;
    }

    // The speech to text model lives outside the install tree, so make sure
    // its folder is there before anything tries to load out of it. Neither
    // case is fatal: the orb still runs without speech.
    QString ttsError;
    if (!BurnOS::Tts::ensureDir(&ttsError))
        qWarning().noquote() << "TTS folder unavailable:" << ttsError;

    QString sttError;
    if (!BurnOS::Stt::ensureDir(&sttError))
        qWarning().noquote() << "STT folder unavailable:" << sttError;
    else if (!BurnOS::Stt::modelPresent())
        qInfo().noquote() << "STT model missing, expected at"
                          << QDir::toNativeSeparators(BurnOS::Stt::modelPath());

    MainWindow w;
    w.show();
    return a.exec();
}
