#ifndef COMPUTERUSE_H
#define COMPUTERUSE_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// One visible top level window.
struct DesktopWindow
{
    QString title;
    quint64 handle = 0;
    bool isNull() const { return handle == 0; }
};

// One control inside a window, as UI Automation sees it. The key is ours, not
// Windows': c0, c1, c2 and so on, because Jev answers with a key from a list we
// hand it and most controls have no AutomationId to answer with.
struct DesktopControl
{
    QString key;
    QString id;   // AutomationId, often empty
    QString name; // what a person would call it
    QString type; // Button, Edit, MenuItem, TabItem ...
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    // What Jev is shown. Short on purpose: it is one line of a list of forty.
    QString describe() const;
};

// A picture of one window, and what it took to make it fit. The model is shown
// the PNG and answers in the PICTURE's pixels, so `scale` is what turns a point
// it names back into a point on the real window.
struct WindowShot
{
    QByteArray png;
    int width = 0;   // of the picture
    int height = 0;
    qreal scale = 1.0; // picture pixels per window pixel
    bool isNull() const { return png.isEmpty(); }
};

// The things Burnos is allowed to do to this machine.
//
// Deliberately a CLOSED SET. Nothing here takes an arbitrary command line or an
// arbitrary key sequence, so a misheard sentence cannot turn into an arbitrary
// process. Adding a capability means adding it here on purpose.
namespace BurnOS {
namespace Computer {

// Every program the orb may start, keyed by the word Jev picks. Anything not
// on this list cannot be launched at all.
QStringList knownApps();
QString appLabel(const QString &appKey);      // "Firefox"
QString appGuidance(const QString &appKey);   // what to tell Jev about it
bool launch(const QString &appKey, QString *error);

// Start ANYTHING on this machine by the name the user said for it, and hand
// back in `opened` what Windows calls the thing that was started.
//
// THREE SOURCES, in order of how well the name matched: the aliases above, the
// AppsFolder shell namespace, and the Start Menu shortcuts - then, failing all
// three, whatever Windows makes of the name as a command. AppsFolder is the
// one that matters: a Store app has NO executable and no shortcut, so it can
// only be opened through the application user model id listed there, and a
// command line for it does not exist to be found.
bool launchAnything(const QString &spokenName, QString *opened, QString *error);

// Open a web address in the browser the user ALREADY HAS, on the profile they
// are signed in on.
//
// This is what "open GitHub" kept getting wrong. Launching a browser by name
// starts it fresh out of the apps folder, which can come up on a profile the
// user has never signed in on, and typing the address in by hand then loads the
// site with none of their cookies - so GitHub appeared signed out even though
// they are signed in. Handing the address to the shell instead lets Windows
// give it to the browser already running, in the session that has their
// account in it.
bool openUrl(const QString &url, QString *opened, QString *error);

// Run a command line through cmd.exe and hand back whatever it printed.
//
// This is the general way in, and it is how a program gets opened too:
// `start "" "firefox"` rather than a separate launcher. It is WIDE OPEN by
// design - anything cmd can do, this can do - which is exactly what was asked
// for, and worth remembering before anything else is wired into it.
bool runCommand(const QString &command, QString *output, QString *error);

// Every key the orb may press, same reasoning.
QStringList knownKeys();
bool pressKey(const QString &keyName, QString *error);

QList<DesktopWindow> windows();
DesktopWindow foreground();

// The first visible window whose title contains this, case insensitively.
DesktopWindow findWindow(const QString &titlePart);

// A PNG of one window. Shrunk if it is large, because a full screen picture is
// megabytes of base64 and the model does not need that much of it.
WindowShot capture(quint64 handle);

// A real mouse click at a point given in the WINDOW's own pixels, measured
// from its top left corner. This is what lets the model act on what it saw in
// a screenshot rather than on what UI Automation happens to expose.
bool clickAt(quint64 handle, int windowX, int windowY, QString *error);

// Visible, named controls of one window, nearest the top left first. Capped,
// because the whole list travels to Jev as the options of a question.
QList<DesktopControl> controls(quint64 handle, int limit = 40);

// Invoked through UI Automation where the control supports it, and with a real
// mouse click where it does not.
bool click(quint64 handle, const DesktopControl &control, QString *error);

// Typed through UI Automation's value pattern where the control supports it,
// and by focusing and typing where it does not.
bool fill(quint64 handle, const DesktopControl &control, const QString &text,
          QString *error);

} // namespace Computer
} // namespace BurnOS

#endif // COMPUTERUSE_H
