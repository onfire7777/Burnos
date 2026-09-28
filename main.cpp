#include "mainwindow.h"
#include "orbwidget.h"
#include "sttpaths.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QStringList>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

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
