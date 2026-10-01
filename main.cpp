#include "agent.h"
#include "mainwindow.h"
#include "orbwidget.h"
#include "sttengine.h"

#include <QApplication>
#include <QFile>
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

    // Test helpers: the real conversation path with no microphone, no voice
    // and no window. --talk-probe says each line of a script and writes what
    // was answered; --context-probe writes what would be sent for one
    // sentence without asking the model. See BurnOS::runProbe.
    const int talkAt = args.indexOf(QStringLiteral("--talk-probe"));
    if (talkAt >= 0 && talkAt + 2 < args.size()) {
        QFile script(args.at(talkAt + 1));
        QStringList lines;
        if (script.open(QIODevice::ReadOnly | QIODevice::Text))
            lines = QString::fromUtf8(script.readAll()).split(QLatin1Char('\n'));
        return BurnOS::runProbe(lines, args.at(talkAt + 2));
    }
    const int contextAt = args.indexOf(QStringLiteral("--context-probe"));
    if (contextAt >= 0 && contextAt + 2 < args.size()) {
        return BurnOS::runProbe({QStringLiteral("@context ") + args.at(contextAt + 1)},
                                args.at(contextAt + 2));
    }

    MainWindow w;
    w.show();
    return a.exec();
}
