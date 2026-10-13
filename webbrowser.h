#ifndef WEBBROWSER_H
#define WEBBROWSER_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <memory>

class QProcess;
class QTcpSocket;
class QTimer;

namespace BurnOS {

// A BROWSER OF BURNOS'S OWN, WITH NO WINDOW. A task that has to find something
// out - search, open a page, read it, follow a link - does it here, and the
// user's own browser, with their tabs, their history and their logins, is not
// touched and nothing appears on their screen.
//
// ALL OF IT IS IN THIS PROGRAM. There is no Node, no Playwright and nothing to
// install beside Burnos.exe: it starts the Chromium-engine browser the PC
// already has - Microsoft Edge, which every Windows 10 and 11 comes with, or
// Chrome or Brave - as a SEPARATE HIDDEN PROCESS (--headless) on a profile
// folder of its own under %LOCALAPPDATA%\BurnOS, and drives it over the
// DevTools protocol: JSON over a websocket, which is written out by hand below
// because Qt's websocket module is one more DLL and this needs sixty lines.
//
// It says it is the Edge (or Chrome) it is, rather than "HeadlessChrome",
// because search engines turn the word away unread. It hides nothing else, and
// a site that still asks for proof of a human is reported as that, not worked
// round. It is signed in to nothing and types no passwords.
//
// Started the first time it is needed (a fifth of a second), one tab for each
// task, and closed again after five minutes with nothing to do: with a few
// heavy pages behind it, it holds more memory than everything else Burnos
// loads put together.
class WebBrowser : public QObject
{
    Q_OBJECT

public:
    // What an action came to, in words for a model, and a picture when the
    // action was a screenshot. Called exactly once, and never from inside run.
    using Done = std::function<void(const QString &text, const QByteArray &png)>;

    explicit WebBrowser(QObject *parent = nullptr);
    ~WebBrowser() override;

    // The browser it would run hidden; empty when the PC has none.
    static QString program(QString *name = nullptr);
    static bool available() { return !program().isEmpty(); }

    // One action of the web tool, on `owner`'s own tab - a task's id.
    //   search  query            results: title, address, a few words each
    //   open    url              the page's words, the first part of them
    //   more                     the next part of the page
    //   find    text             the passages of the page that have it
    //   links   [query]          the page's links, those with the words
    //   click   text             a link or button by its visible words
    //   type    text [field] [submit]   into a text field; Enter with submit
    //   back                     the page before
    //   screenshot               what the page looks like
    void run(int owner, const QVariantMap &args, Done done);
    // The owner is finished: its tab is closed.
    void release(int owner);
    // Closes the hidden browser. The next run starts it again.
    void shutDown();

private:
    enum class State { Off, Starting, Ready };
    using Reply = std::function<void(const QJsonObject &result, const QString &error)>;
    using Then = std::function<void(const QString &error)>;
    using Value = std::function<void(const QJsonValue &value, const QString &error)>;
    struct Pending
    {
        Reply then;
        qint64 due = 0;
        QString method;
    };
    struct Tab
    {
        QString targetId;
        QString sessionId;
        // The page as it was last read: what `more` and `find` work on.
        QString title;
        QString url;
        QString type;
        QString text;
        int at = 0;
        // A search engine's page of results, read as results: how many.
        int results = 0;
        // A PDF whose words were read out of it: how many pages it has.
        int pdfPages = 0;
    };
    struct LoadWait
    {
        QElapsedTimer clock;
        int lastLength = -1;
        int steady = 0;
    };

    // The process and the wire.
    void whenReady(Then then);
    void start();
    void pollPort();
    void connectSocket(int port, const QString &path);
    void onSocketData();
    void ready();
    void sendFrame(int opcode, const QByteArray &payload);
    void send(const QString &method, const QJsonObject &params, const QString &session,
              Reply then, int timeoutMs = 30000);
    void dispatch(const QJsonObject &message);
    void sweep();
    void stopProcess(bool politely);
    void failAll(const QString &why);
    void died(const QString &why);

    // A tab and its page.
    Tab *tab(int owner);
    void tabFor(int owner, Then then);
    void evaluate(int owner, const QString &expression, Value then, bool awaitPromise = false,
                  int timeoutMs = 30000);
    void waitLoaded(int owner, std::function<void()> then);
    void pollLoaded(int owner, std::shared_ptr<LoadWait> wait, std::function<void()> then);
    void readPage(int owner, Then then);
    void readPdf(int owner, Then then);
    QString report(int owner);
    static QString part(Tab &tab);

    // The actions.
    void perform(int owner, const QVariantMap &args, Done finish);
    void search(int owner, const QString &query, int engine, Done finish);
    void goTo(int owner, const QString &url, const QString &lead, Done finish);
    void afterAction(int owner, const QString &lead, Done finish);
    void click(int owner, const QString &text, Done finish);
    void type(int owner, const QString &field, const QString &text, bool submit, Done finish);
    void back(int owner, Done finish);
    void links(int owner, const QString &query, Done finish);
    void screenshot(int owner, Done finish);

    State m_state = State::Off;
    QProcess *m_proc = nullptr;
    // The Windows job object the hidden browser and everything it starts
    // belong to, so that none of it outlives Burnos.
    void *m_job = nullptr;
    QTcpSocket *m_socket = nullptr;
    bool m_shaken = false;
    QByteArray m_in;
    QByteArray m_message;
    QString m_profile;
    // 1 once the first profile folder turned out to be held by a browser left
    // over from a run that crashed: the second folder is used instead.
    int m_profileTry = 0;
    QString m_name;
    QElapsedTimer m_startClock;
    QElapsedTimer m_clock;
    QTimer *m_poll = nullptr;
    QTimer *m_sweep = nullptr;
    QTimer *m_idle = nullptr;
    int m_nextId = 0;
    int m_running = 0;
    QHash<int, Pending> m_pending;
    QList<Then> m_waiters;
    QHash<int, Tab> m_tabs;
};

// Burnos.exe --web-probe script.txt out.txt: the hidden browser with no model,
// no window and no microphone. Each line of the script is one action - search
// words, open address, more, find words, links words, click words, type field
// = words (a ! at the end presses Enter), back, screenshot, wait ms - and what
// each came to is written to the out file. 0 when none of them failed.
int runWebProbe(const QStringList &script, const QString &outPath);

} // namespace BurnOS

#endif // WEBBROWSER_H
