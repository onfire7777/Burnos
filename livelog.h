#ifndef LIVELOG_H
#define LIVELOG_H

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QString>
#include <QTextStream>

namespace BurnOS {

// A plain text trace of what live mode is doing, written next to the
// executable. An orb has no labels and no title bar, so this file is the only
// way to prove afterwards that the microphone, the voice detector and the
// colour of the orb all agreed with each other.
inline void liveLog(const QString &line)
{
    QFile file(QCoreApplication::applicationDirPath()
               + QStringLiteral("/burnos-live.log"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    QTextStream out(&file);
    out << QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"))
        << ' ' << line << '\n';
}

} // namespace BurnOS

#endif // LIVELOG_H
