#ifndef COMPUTERUSE_H
#define COMPUTERUSE_H

#include <QByteArray>
#include <QList>
#include <QString>
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
    QString type;  // Button, Edit, MenuItem, Tab ...
    QString state; // on, off, selected, expanded, collapsed, disabled - or empty
    QString value; // what a field holds, cut short
    int x = 0;     // screen pixels
    int y = 0;
    int w = 0;
    int h = 0;

    // What the model is shown: one line of a list of fifty.
    QString describe() const;
};

// What ONE task knows about the desktop between one action and the next: the
// window it is working in, the controls it was last shown there, and its last
// screenshot. Every task carries its own, so two tasks never click with each
// other's list or aim at each other's picture.
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
    // The agent's OWN pointer, in screen pixels: where its mouse is resting,
    // apart from the user's physical cursor. -1 until it first points at
    // something. Moved by the background mouse, never by SetCursorPos.
    int cursorX = -1;
    int cursorY = -1;
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
// It is wide open by design - a voice agent meant to be able to do anything
// on the computer - so everything it does is written to the log.
namespace BurnOS::Computer {

DesktopWindow foreground();

// One window's controls, worded for the model, and remembered in `state` as
// what their keys refer to - only those whose name holds every word of
// `filter`, when one is given.
QString describeControls(const DesktopWindow &target, DeskState &state,
                         const QString &filter = QString());

// Carry out one computer_use action - the model's arguments as they came - and
// say in plain words what happened. BLOCKING, and meant to be: it runs on the
// hands thread, never on the thread the orb draws on. `cancelled` is checked
// through every wait, so a stopped task lets go at once.
DeskResult perform(const QVariantMap &args, const DeskState &state,
                   const std::atomic_bool &cancelled);

// EVERYTHING THAT TOUCHES THE DESKTOP RUNS ON ONE THREAD, one action at a time
// and in the order asked, because there is one screen and one mouse. `job`
// runs there, under a single threaded COM apartment as UI Automation wants;
// `done` is called back on the main thread - unless `guard` is gone by then.
void onHands(QObject *guard, std::function<DeskResult()> job,
             std::function<void(const DeskResult &)> done);

// One files action - read, write, edit, list, find or search - on any path on
// this machine, answered in plain words for the model. Blocking; it runs on a
// pool thread, never on the orb's.
QString files(const QVariantMap &args);

// A command line, run in a folder of the model's choosing for as long as it
// takes, up to a limit - a build, a test run, a script. NOTHING WAITS ON IT:
// `done` hears what it printed once it ends, is stopped, or runs out of time,
// and not at all if `guard` is gone. A stopped task ends the whole process
// tree the moment `cancelled` is set.
void runShell(QObject *guard, const QVariantMap &args,
              std::shared_ptr<std::atomic_bool> cancelled,
              std::function<void(const QString &)> done);

} // namespace BurnOS::Computer

#endif // COMPUTERUSE_H
