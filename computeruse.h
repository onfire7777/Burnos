#ifndef COMPUTERUSE_H
#define COMPUTERUSE_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>

class QObject;

// One visible top level window.
struct DesktopWindow
{
    QString title;
    quint64 handle = 0;
    bool isNull() const { return handle == 0; }
};

// One control inside a window, as UI Automation sees it. The key is ours, not
// Windows': c0, c1, c2 and so on, because most controls have no AutomationId
// to answer with - and the same key is drawn on the screenshot, so the model
// can point at what it sees.
struct DesktopControl
{
    QString key;
    QString id;    // AutomationId, often empty
    QString name;  // what a person would call it
    QString type;  // Button, Edit, MenuItem, TabItem ...
    QString state; // on, off, selected, expanded, collapsed, disabled - or empty
    QString value; // what a field holds, cut short
    int x = 0;     // screen pixels
    int y = 0;
    int w = 0;
    int h = 0;

    // What the model is shown: one line of a list of fifty.
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

// What ONE task knows about the desktop between one action and the next: the
// window it is working in, the controls it was last shown there, and its last
// screenshot. Every background task carries its own, so two tasks never click
// with each other's list or aim at each other's picture.
struct DeskState
{
    DesktopWindow target;
    QList<DesktopControl> offered;
    // The last screenshot: of one window, or of the whole screen, and how far
    // it was shrunk. A point in the picture maps back through these.
    quint64 shotWindow = 0;
    bool shotScreen = false;
    int shotLeft = 0; // the screen's own top left, for a whole-screen picture
    int shotTop = 0;
    qreal shotScale = 1.0; // picture pixels per window pixel
};

// What one action did: the plain sentence handed back to the model, a picture
// if it took one, and the task's desk as the action left it.
struct DeskResult
{
    QString text;
    QByteArray png;
    DeskState state;
};

// EVERYTHING BURNOS DOES TO THIS MACHINE: windows, UI Automation, the mouse,
// the keyboard, screenshots, the clipboard, launching, files and commands.
// It is wide open by design - a voice agent that is meant to be able to do
// anything on the computer - so everything it does is written to the log.
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
QList<DesktopControl> controls(quint64 handle, int limit = 50);

// Invoked through UI Automation where the control supports it, and with a real
// mouse click where it does not.
bool click(quint64 handle, const DesktopControl &control, QString *error);

// Typed through UI Automation's value pattern where the control supports it,
// and by focusing and typing where it does not.
bool fill(quint64 handle, const DesktopControl &control, const QString &text,
          QString *error);

// ---------------------------------------------------------------------------
// THE HANDS
// ---------------------------------------------------------------------------

// One window's controls, worded for the model, and remembered in `state` as
// what their keys refer to.
QString describeControls(const DesktopWindow &target, DeskState &state);

// Carry out one computer_use action - the model's arguments as they came - and
// say in plain words what happened. BLOCKING, and meant to be: it runs on the
// hands thread, never on the thread the orb draws on. `cancelled` is checked
// through every wait, so a stopped task lets go at once.
DeskResult perform(const QVariantMap &args, const DeskState &state,
                   const std::atomic_bool &cancelled);

// EVERYTHING THAT TOUCHES THE DESKTOP RUNS ON ONE THREAD, one action at a time
// and in the order asked, because there is one screen and one mouse: UI
// Automation, the keyboard, clicks, screenshots, launching. The orb and the
// microphone never wait on an action, and UI Automation gets a thread of its
// own, which is where a UIA client belongs.
//
// `job` runs on that thread; `done` is called back on the main thread with
// what it returned - unless `guard` has been destroyed by then, in which case
// nobody is left to tell and it is not called at all.
void onHands(QObject *guard, std::function<DeskResult()> job,
             std::function<void(const DeskResult &)> done);

// ---------------------------------------------------------------------------
// CODING: files and commands
// ---------------------------------------------------------------------------

// One files action - read, write, edit, list, find or search - on any path on
// this machine, answered in plain words for the model. Blocking; it runs on a
// pool thread, never on the orb's.
QString files(const QVariantMap &args);

// A command line, run in a folder of the model's choosing for as long as it
// takes, up to a limit - a build, a test run, a script. NOTHING WAITS ON IT:
// the process runs on its own and `done` hears what it printed once it ends,
// is stopped, or runs out of time. Not called if `guard` is gone by then.
// A task that is stopped stops its command too: `cancelled` is watched, and
// the whole process tree is ended the moment it is set.
void runShell(QObject *guard, const QVariantMap &args,
              std::shared_ptr<std::atomic_bool> cancelled,
              std::function<void(const QString &)> done);

} // namespace Computer
} // namespace BurnOS

#endif // COMPUTERUSE_H
