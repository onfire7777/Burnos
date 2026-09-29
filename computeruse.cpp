#include "computeruse.h"

#include <QUrl>
#include "livelog.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QThread>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QSet>

#include <windows.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <UIAutomation.h>

namespace {

// ---------------------------------------------------------------------------
// COM, held open for the life of the process. UI Automation is a COM API and
// re-initialising it per call is both slow and a good way to lose the proxy.
// ---------------------------------------------------------------------------
IUIAutomation *automation()
{
    static IUIAutomation *instance = []() -> IUIAutomation * {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IUIAutomation *ui = nullptr;
        const HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            __uuidof(IUIAutomation),
                                            reinterpret_cast<void **>(&ui));
        if (FAILED(hr)) {
            BurnOS::liveLog(QStringLiteral("UI Automation unavailable, hr 0x%1")
                                .arg(quint32(hr), 8, 16, QLatin1Char('0')));
            return nullptr;
        }
        return ui;
    }();
    return instance;
}

QString fromBstr(BSTR s)
{
    if (!s)
        return {};
    const QString out = QString::fromWCharArray(s, int(SysStringLen(s)));
    SysFreeString(s);
    return out;
}

QString controlTypeName(CONTROLTYPEID id)
{
    switch (id) {
    case UIA_ButtonControlTypeId:      return QStringLiteral("Button");
    case UIA_EditControlTypeId:        return QStringLiteral("Edit");
    case UIA_MenuItemControlTypeId:    return QStringLiteral("MenuItem");
    case UIA_TabItemControlTypeId:     return QStringLiteral("Tab");
    case UIA_ListItemControlTypeId:    return QStringLiteral("ListItem");
    case UIA_CheckBoxControlTypeId:    return QStringLiteral("CheckBox");
    case UIA_RadioButtonControlTypeId: return QStringLiteral("Radio");
    case UIA_ComboBoxControlTypeId:    return QStringLiteral("ComboBox");
    case UIA_HyperlinkControlTypeId:   return QStringLiteral("Link");
    case UIA_TextControlTypeId:        return QStringLiteral("Text");
    case UIA_DocumentControlTypeId:    return QStringLiteral("Document");
    case UIA_TreeItemControlTypeId:    return QStringLiteral("TreeItem");
    default:                           return QStringLiteral("Control");
    }
}

// Only the kinds of control it makes sense to aim a spoken command at. A window
// has hundreds of panes and separators and none of them are ever what somebody
// meant.
bool worthOffering(CONTROLTYPEID id)
{
    switch (id) {
    case UIA_ButtonControlTypeId:
    case UIA_EditControlTypeId:
    case UIA_MenuItemControlTypeId:
    case UIA_TabItemControlTypeId:
    case UIA_ListItemControlTypeId:
    case UIA_CheckBoxControlTypeId:
    case UIA_RadioButtonControlTypeId:
    case UIA_ComboBoxControlTypeId:
    case UIA_HyperlinkControlTypeId:
    case UIA_TreeItemControlTypeId:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// A PACKAGED APP IS NOT ONE WINDOW, and this is what broke typing into the
// Microsoft Store. The titled top level window is an ApplicationFrameWindow
// owned by ApplicationFrameHost.exe; the actual XAML content lives in a
// Windows.UI.Core.CoreWindow child belonging to the app's own process.
// Activating and clicking want the FRAME - that is the window Windows knows
// about. UI Automation wants the CONTENT, because a walk from the frame drags
// in the host's chrome and buries the real fields under it.
// ---------------------------------------------------------------------------
struct ChildByClass
{
    const wchar_t *className;
    HWND found;
};

BOOL CALLBACK findChildByClass(HWND child, LPARAM param)
{
    auto *bag = reinterpret_cast<ChildByClass *>(param);
    wchar_t cls[160] = {};
    GetClassNameW(child, cls, 159);
    if (wcscmp(cls, bag->className) == 0) {
        bag->found = child;
        return FALSE;
    }
    return TRUE;
}

HWND uiaHost(HWND top)
{
    wchar_t cls[160] = {};
    if (!top || GetClassNameW(top, cls, 159) <= 0)
        return top;
    if (wcscmp(cls, L"ApplicationFrameWindow") != 0)
        return top;

    ChildByClass bag{L"Windows.UI.Core.CoreWindow", nullptr};
    EnumChildWindows(top, findChildByClass, reinterpret_cast<LPARAM>(&bag));
    return bag.found ? bag.found : top;
}

// Whether this element offers a value that can be WRITTEN. A read only value
// pattern is common and SetValue on one fails with access denied.
bool supportsValueWrite(IUIAutomationElement *e)
{
    if (!e)
        return false;
    IUnknown *pattern = nullptr;
    if (FAILED(e->GetCurrentPattern(UIA_ValuePatternId, &pattern)) || !pattern)
        return false;
    auto *value = static_cast<IUIAutomationValuePattern *>(pattern);
    BOOL readOnly = TRUE;
    value->get_CurrentIsReadOnly(&readOnly);
    value->Release();
    return !readOnly;
}

// What the field holds right now. `readable` says whether UI Automation could
// read it AT ALL, and that distinction is the whole point: an empty string from
// a field that cannot be read is not evidence that typing failed.
QString readValue(IUIAutomationElement *e, bool *readable)
{
    if (readable)
        *readable = false;
    if (!e)
        return {};

    IUnknown *pattern = nullptr;
    if (SUCCEEDED(e->GetCurrentPattern(UIA_ValuePatternId, &pattern)) && pattern) {
        auto *value = static_cast<IUIAutomationValuePattern *>(pattern);
        BSTR raw = nullptr;
        const HRESULT hr = value->get_CurrentValue(&raw);
        value->Release();
        if (SUCCEEDED(hr)) {
            if (readable)
                *readable = true;
            return fromBstr(raw);
        }
    }

    // XAML fields that ignore the value pattern often still answer the old
    // accessibility one.
    pattern = nullptr;
    if (SUCCEEDED(e->GetCurrentPattern(UIA_LegacyIAccessiblePatternId, &pattern))
        && pattern) {
        auto *legacy = static_cast<IUIAutomationLegacyIAccessiblePattern *>(pattern);
        BSTR raw = nullptr;
        const HRESULT hr = legacy->get_CurrentValue(&raw);
        legacy->Release();
        if (SUCCEEDED(hr)) {
            if (readable)
                *readable = true;
            return fromBstr(raw);
        }
    }
    return {};
}

// How well this element answers to what we recorded about it. Below zero means
// do not touch it.
//
// `forTyping` IS WHAT STOPS THE PLACEHOLDER WINNING. A Store search box is a
// wrapper, a static caption and an inner text field that all carry the SAME
// name, and only the last of the three can be typed into. Taking the first
// match, which is what this used to do, aimed every keystroke at a label.
int matchScore(IUIAutomationElement *e, const DesktopControl &target, bool forTyping)
{
    BSTR raw = nullptr;
    e->get_CurrentAutomationId(&raw);
    const QString id = fromBstr(raw);
    raw = nullptr;
    e->get_CurrentName(&raw);
    const QString name = fromBstr(raw).simplified();

    int score = -1000;
    if (!target.id.isEmpty() && id == target.id)
        score = 100;
    else if (!target.name.isEmpty() && name == target.name)
        score = 60;
    else if (!target.name.isEmpty()
             && name.compare(target.name, Qt::CaseInsensitive) == 0)
        score = 40;
    if (score < 0)
        return score;

    BOOL offscreen = FALSE;
    e->get_CurrentIsOffscreen(&offscreen);
    if (offscreen)
        score -= 80;

    BOOL enabled = TRUE;
    e->get_CurrentIsEnabled(&enabled);
    if (!enabled)
        score -= 80;

    CONTROLTYPEID type = 0;
    e->get_CurrentControlType(&type);

    if (forTyping) {
        if (supportsValueWrite(e))
            score += 50;
        BOOL focusable = FALSE;
        e->get_CurrentIsKeyboardFocusable(&focusable);
        if (focusable)
            score += 30;
        if (type == UIA_EditControlTypeId || type == UIA_DocumentControlTypeId
            || type == UIA_ComboBoxControlTypeId)
            score += 25;
        // A caption drawn inside the field, or the box around it, is not it.
        if (type == UIA_TextControlTypeId || type == UIA_GroupControlTypeId
            || type == UIA_PaneControlTypeId)
            score -= 60;
    }

    // Where it is now against where it was when the model was shown the list.
    // Two identically named fields are told apart by this and nothing else.
    RECT r{};
    if (SUCCEEDED(e->get_CurrentBoundingRectangle(&r)) && target.w > 0
        && target.h > 0) {
        const int dx = ((r.left + r.right) / 2) - (target.x + target.w / 2);
        const int dy = ((r.top + r.bottom) / 2) - (target.y + target.h / 2);
        score += qMax(0, 40 - (qAbs(dx) + qAbs(dy)) / 8);
    }
    return score;
}

// Find the element again by what we recorded about it. UI Automation elements
// are not safe to keep across calls, so the list the model chose from is only a
// description and the element has to be looked up once more to act on it.
//
// Every candidate is scored and the BEST one wins, rather than the first one
// found - see matchScore for why that matters.
IUIAutomationElement *refind(quint64 handle, const DesktopControl &target,
                             bool forTyping = false)
{
    IUIAutomation *ui = automation();
    if (!ui)
        return nullptr;

    HWND top = reinterpret_cast<HWND>(handle);
    const HWND host = uiaHost(top);

    IUIAutomationElement *root = nullptr;
    if (FAILED(ui->ElementFromHandle(host, &root)) || !root) {
        if (host == top)
            return nullptr;
        // The content window of a packaged app can refuse; the frame answers.
        if (FAILED(ui->ElementFromHandle(top, &root)) || !root)
            return nullptr;
    }

    IUIAutomationCondition *all = nullptr;
    ui->CreateTrueCondition(&all);

    IUIAutomationElementArray *found = nullptr;
    root->FindAll(TreeScope_Descendants, all, &found);

    IUIAutomationElement *best = nullptr;
    int bestScore = 0;
    if (found) {
        int count = 0;
        found->get_Length(&count);
        for (int i = 0; i < count; ++i) {
            IUIAutomationElement *e = nullptr;
            if (FAILED(found->GetElement(i, &e)) || !e)
                continue;

            const int score = matchScore(e, target, forTyping);
            if (score > 0 && (!best || score > bestScore)) {
                if (best)
                    best->Release();
                best = e;
                bestScore = score;
                continue;
            }
            e->Release();
        }
        found->Release();
    }

    if (all)
        all->Release();
    root->Release();
    return best;
}

// A real mouse click, put back where it was afterwards, for the controls that
// expose no pattern at all.
void realClick(int screenX, int screenY)
{
    POINT was{};
    GetCursorPos(&was);
    SetCursorPos(screenX, screenY);
    QThread::msleep(40);

    INPUT down{};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    INPUT up{};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &down, sizeof(INPUT));
    QThread::msleep(30);
    SendInput(1, &up, sizeof(INPUT));
    QThread::msleep(40);

    SetCursorPos(was.x, was.y);
}

// Put a window in front, and make sure it REALLY got there.
//
// SetForegroundWindow on its own is not enough and does not say so. Windows
// refuses it from a process that does not already own the foreground: it
// returns FALSE, the window stays where it was, and every keystroke that
// follows lands in whatever was in front instead. A packaged app takes no
// typed text whatsoever while it is in the background, so this is the
// difference between typing working and silently doing nothing at all.
// Borrowing the current foreground thread's input queue makes us count as its
// owner for a moment, which is the documented way through.
bool bringToFront(HWND top)
{
    if (!top || !IsWindow(top))
        return false;
    if (GetForegroundWindow() == top)
        return true;

    if (IsIconic(top))
        ShowWindow(top, SW_RESTORE);

    const HWND already = GetForegroundWindow();
    const DWORD ours = GetCurrentThreadId();
    const DWORD theirs = already ? GetWindowThreadProcessId(already, nullptr) : 0;
    const bool attached = theirs && theirs != ours
                          && AttachThreadInput(ours, theirs, TRUE);

    SetForegroundWindow(top);
    BringWindowToTop(top);

    if (attached)
        AttachThreadInput(ours, theirs, FALSE);

    // A packaged app hands the foreground to its own content window
    // asynchronously, so the answer is not there on the first look.
    for (int waited = 0; waited < 700; waited += 40) {
        if (GetForegroundWindow() == top)
            return true;
        QThread::msleep(40);
    }
    return false;
}

void typeUnicode(const QString &text)
{
    // Per UTF-16 unit rather than per character, on purpose: a surrogate pair
    // is sent as its two halves and Windows puts them back together.
    for (int i = 0; i < text.size(); ++i) {
        INPUT pair[2] = {};
        pair[0].type = INPUT_KEYBOARD;
        pair[0].ki.dwFlags = KEYEVENTF_UNICODE;
        pair[0].ki.wScan = text.at(i).unicode();
        pair[1] = pair[0];
        pair[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(2, pair, sizeof(INPUT));
        // A XAML field runs its own text handling between keystrokes and drops
        // characters that arrive faster than it can deal with them. Four
        // milliseconds was enough for Notepad and lost letters in the Store.
        QThread::msleep(12);
    }
}

void pressWithControl(WORD vk)
{
    INPUT seq[4] = {};
    for (INPUT &in : seq)
        in.type = INPUT_KEYBOARD;
    seq[0].ki.wVk = VK_CONTROL;
    seq[1].ki.wVk = vk;
    seq[2].ki.wVk = vk;
    seq[2].ki.dwFlags = KEYEVENTF_KEYUP;
    seq[3].ki.wVk = VK_CONTROL;
    seq[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, seq, sizeof(INPUT));
    QThread::msleep(40);
}

void pressPlain(WORD vk)
{
    INPUT pair[2] = {};
    pair[0].type = INPUT_KEYBOARD;
    pair[0].ki.wVk = vk;
    pair[1] = pair[0];
    pair[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, pair, sizeof(INPUT));
    QThread::msleep(30);
}

struct AppEntry
{
    const char *key;
    const char *label;
    const char *command;
    const char *guidance;
};

// The whole of what the orb may start. Deliberately short.
const AppEntry kApps[] = {
    {"firefox",    "Firefox",          "firefox",
     "the Firefox web browser"},
    {"chrome",     "Chrome",           "chrome",
     "the Google Chrome web browser"},
    {"edge",       "Edge",             "msedge",
     "the Microsoft Edge web browser"},
    {"notepad",    "Notepad",          "notepad",
     "Notepad, for writing plain text"},
    {"explorer",   "File Explorer",    "explorer",
     "File Explorer, for browsing files and folders"},
    {"calculator", "Calculator",       "calc",
     "the Windows Calculator"},
    {"terminal",   "Terminal",         "wt",
     "Windows Terminal, a command line"},
    {"powershell", "PowerShell",       "powershell",
     "a PowerShell command line window"},
    {"settings",   "Windows Settings", "ms-settings:",
     "the Windows Settings app - the SYSTEM settings, not Burnos's own"},
    {"taskmgr",    "Task Manager",     "taskmgr",
     "Task Manager, for looking at running processes"},
};

struct KeyEntry
{
    const char *name;
    WORD vk;
};

const KeyEntry kKeys[] = {
    {"enter",     VK_RETURN},
    {"escape",    VK_ESCAPE},
    {"tab",       VK_TAB},
    {"space",     VK_SPACE},
    {"backspace", VK_BACK},
    {"delete",    VK_DELETE},
    {"up",        VK_UP},
    {"down",      VK_DOWN},
    {"left",      VK_LEFT},
    {"right",     VK_RIGHT},
    {"home",      VK_HOME},
    {"end",       VK_END},
    {"pageup",    VK_PRIOR},
    {"pagedown",  VK_NEXT},
    {"f5",        VK_F5},
};

// ShellExecute on one string. Anything over 32 is success; the value itself is
// a fake HINSTANCE and means nothing else.
bool shellOpen(const QString &what)
{
    if (what.trimmed().isEmpty())
        return false;
    const HINSTANCE rc = ShellExecuteW(
        nullptr, L"open",
        reinterpret_cast<const wchar_t *>(what.utf16()),
        nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(rc) > 32;
}

// Spacing and punctuation thrown away, so "Microsoft Store" and "microsoft
// store" and "MicrosoftStore" are all the same word.
QString squash(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        if (c.isLetterOrNumber())
            out.append(c.toLower());
    }
    return out;
}

// How well a name answers to what was said: 3 it is exactly that, 2 it starts
// with it, 1 it holds it somewhere, 0 no.
int nameScore(const QString &candidate, const QString &needle)
{
    const QString a = squash(candidate);
    const QString b = squash(needle);
    if (a.isEmpty() || b.isEmpty())
        return 0;
    if (a == b)
        return 3;
    if (a.startsWith(b))
        return 2;
    if (a.contains(b))
        return 1;
    return 0;
}

// One way of opening one program: what to hand ShellExecute, what to call it
// afterwards, and how well it answered to what was asked for.
struct Candidate
{
    QString target;
    QString label;
    int score = 0;
};

struct InstalledApp
{
    QString name;  // what Windows calls it in the Start menu
    QString aumid; // the only handle a packaged app has
};

// ---------------------------------------------------------------------------
// EVERY APP WINDOWS ITSELF WILL OPEN, packaged ones included.
//
// This is the whole reason for this block. A Store app - the Microsoft Store
// itself, Spotify, WhatsApp, Calculator on a clean install - HAS NO EXECUTABLE
// TO RUN and puts no shortcut in Programs either, so `start "" "spotify"`
// cannot work and never could. Its only handle is an application user model id
// out of the AppsFolder shell namespace, opened as shell:AppsFolder\<aumid>.
// That namespace is where Windows gets the Start menu's own app list from, so
// if the Start menu can open it, this can.
// ---------------------------------------------------------------------------
QList<InstalledApp> enumerateAppsFolder()
{
    QList<InstalledApp> out;

    // UI Automation may not have been touched yet, and this needs an apartment
    // of its own either way. Calling it twice is harmless.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IShellItem *folder = nullptr;
    if (FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr,
                                           __uuidof(IShellItem),
                                           reinterpret_cast<void **>(&folder)))
        || !folder) {
        BurnOS::liveLog(QStringLiteral("launch: the apps folder would not open"));
        return out;
    }

    IEnumShellItems *items = nullptr;
    if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems,
                                        __uuidof(IEnumShellItems),
                                        reinterpret_cast<void **>(&items)))
        && items) {
        IShellItem *item = nullptr;
        while (items->Next(1, &item, nullptr) == S_OK && item) {
            LPWSTR display = nullptr;
            LPWSTR parsing = nullptr;
            // NORMALDISPLAY is the name a person sees. PARENTRELATIVEPARSING is
            // the launch id - an aumid for a packaged app, and something
            // ShellExecute understands for a desktop one.
            item->GetDisplayName(SIGDN_NORMALDISPLAY, &display);
            item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &parsing);

            InstalledApp app;
            if (display) {
                app.name = QString::fromWCharArray(display).simplified();
                CoTaskMemFree(display);
            }
            if (parsing) {
                app.aumid = QString::fromWCharArray(parsing);
                CoTaskMemFree(parsing);
            }
            if (!app.name.isEmpty() && !app.aumid.isEmpty())
                out.append(app);

            item->Release();
            item = nullptr;
        }
        items->Release();
    }

    folder->Release();
    return out;
}

// Held for a minute at a time. Walking the namespace takes a few hundred
// milliseconds and nobody installs a program in the middle of a sentence.
const QList<InstalledApp> &installedApps()
{
    static QList<InstalledApp> cache;
    static qint64 taken = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (cache.isEmpty() || now - taken > 60000) {
        cache = enumerateAppsFolder();
        taken = now;
        BurnOS::liveLog(QStringLiteral("launch: %1 installed apps Windows can open")
                            .arg(cache.size()));
    }
    return cache;
}

Candidate bestInstalledApp(const QString &wanted)
{
    Candidate best;
    for (const InstalledApp &app : installedApps()) {
        const int score = nameScore(app.name, wanted);
        if (score > best.score) {
            best.score = score;
            best.label = app.name;
            best.target = QStringLiteral("shell:AppsFolder\\") + app.aumid;
            if (score == 3)
                break; // an exact name cannot be beaten
        }
    }
    return best;
}

// The Start Menu shortcuts as well, because a classic installer that drops a
// .lnk in Programs and registers nothing in AppsFolder does still exist.
Candidate bestShortcut(const QString &wanted)
{
    const QStringList roots{
        qEnvironmentVariable("ProgramData")
            + QStringLiteral("/Microsoft/Windows/Start Menu/Programs"),
        qEnvironmentVariable("APPDATA")
            + QStringLiteral("/Microsoft/Windows/Start Menu/Programs"),
    };

    Candidate best;
    for (const QString &root : roots) {
        if (!QFileInfo::exists(root))
            continue;
        QDirIterator it(root, QStringList{QStringLiteral("*.lnk")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QString base = QFileInfo(path).completeBaseName();
            const int score = nameScore(base, wanted);
            if (score > best.score) {
                best.score = score;
                best.label = base;
                best.target = path;
                if (score == 3)
                    return best;
            }
        }
    }
    return best;
}

struct EnumBag
{
    QList<DesktopWindow> *out;
};

BOOL CALLBACK collectWindow(HWND hwnd, LPARAM param)
{
    if (!IsWindowVisible(hwnd))
        return TRUE;

    // Tool windows, and anything with no title, are never what a person meant.
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        return TRUE;

    wchar_t title[512] = {};
    const int length = GetWindowTextW(hwnd, title, 511);
    if (length <= 0)
        return TRUE;

    RECT r{};
    GetWindowRect(hwnd, &r);
    if (r.right - r.left < 60 || r.bottom - r.top < 40)
        return TRUE;

    DesktopWindow w;
    w.title = QString::fromWCharArray(title, length);
    w.handle = quint64(hwnd);
    reinterpret_cast<EnumBag *>(param)->out->append(w);
    return TRUE;
}

} // namespace

QString DesktopControl::describe() const
{
    QString label = name.isEmpty() ? id : name;
    if (label.size() > 60)
        label = label.left(57) + QStringLiteral("...");
    if (label.isEmpty())
        label = QStringLiteral("(unnamed)");
    return QStringLiteral("%1 \"%2\"").arg(type, label);
}

namespace BurnOS {
namespace Computer {

QStringList knownApps()
{
    QStringList keys;
    for (const AppEntry &a : kApps)
        keys.append(QString::fromLatin1(a.key));
    return keys;
}

QString appLabel(const QString &appKey)
{
    for (const AppEntry &a : kApps) {
        if (appKey == QLatin1String(a.key))
            return QString::fromLatin1(a.label);
    }
    return appKey;
}

QString appGuidance(const QString &appKey)
{
    for (const AppEntry &a : kApps) {
        if (appKey == QLatin1String(a.key))
            return QString::fromLatin1(a.guidance);
    }
    return appKey;
}

bool launch(const QString &appKey, QString *error)
{
    for (const AppEntry &a : kApps) {
        if (appKey != QLatin1String(a.key))
            continue;

        // ShellExecute, not QProcess: it resolves App Paths, so "firefox" finds
        // Firefox wherever it was installed, and it handles ms-settings: too.
        const QString command = QString::fromLatin1(a.command);
        const HINSTANCE rc = ShellExecuteW(
            nullptr, L"open",
            reinterpret_cast<const wchar_t *>(command.utf16()),
            nullptr, nullptr, SW_SHOWNORMAL);

        // ShellExecute returns a fake HINSTANCE; anything above 32 is success.
        if (reinterpret_cast<INT_PTR>(rc) <= 32) {
            if (error)
                *error = QObject::tr("%1 would not start.").arg(appLabel(appKey));
            return false;
        }
        liveLog(QStringLiteral("computer use: launched %1").arg(appLabel(appKey)));
        return true;
    }

    if (error)
        *error = QObject::tr("I am not allowed to open that.");
    return false;
}

bool launchAnything(const QString &spokenName, QString *opened, QString *error)
{
    const QString wanted = spokenName.simplified();
    if (wanted.isEmpty()) {
        if (error)
            *error = QObject::tr("I did not catch what to open.");
        return false;
    }

    // The aliases first, because some of them are not programs at all:
    // "settings" has to mean ms-settings: rather than any shortcut that happens
    // to be called Settings, and "calculator" is calc.
    for (const AppEntry &a : kApps) {
        if (wanted.compare(QLatin1String(a.key), Qt::CaseInsensitive) == 0
            || wanted.compare(QLatin1String(a.label), Qt::CaseInsensitive) == 0) {
            if (!launch(QString::fromLatin1(a.key), error))
                return false;
            if (opened)
                *opened = appLabel(QString::fromLatin1(a.key));
            return true;
        }
    }

    // Then the two real sources, scored against each other. A packaged app
    // wins a tie: AppsFolder is Windows' own list and its names are the ones
    // people actually say, whereas a .lnk basename is whatever the installer
    // felt like calling it.
    const Candidate packaged = bestInstalledApp(wanted);
    const Candidate shortcut = bestShortcut(wanted);
    const Candidate best = packaged.score >= shortcut.score ? packaged : shortcut;

    const auto byName = [&]() -> bool {
        // Let Windows resolve it as a command, which is what finds a program by
        // its executable name through App Paths.
        QString squashed = wanted;
        squashed.remove(QLatin1Char(' '));
        const QStringList attempts{wanted, wanted + QStringLiteral(".exe"),
                                   squashed, squashed + QStringLiteral(".exe")};
        for (const QString &attempt : attempts) {
            if (shellOpen(attempt)) {
                if (opened)
                    *opened = wanted;
                liveLog(QStringLiteral("computer use: opened %1 as \"%2\"")
                            .arg(wanted, attempt));
                return true;
            }
        }
        return false;
    };

    // A score of one means the name was merely found SOMEWHERE inside a longer
    // one, which is a guess. An executable of exactly that name is the better
    // bet, so try that first and keep the weak match as a fallback.
    if (best.score == 1 && byName())
        return true;

    if (best.score > 0 && shellOpen(best.target)) {
        if (opened)
            *opened = best.label;
        liveLog(QStringLiteral("computer use: opened %1 via %2")
                    .arg(best.label,
                         best.target.startsWith(QStringLiteral("shell:AppsFolder"))
                             ? QStringLiteral("the apps folder")
                             : QStringLiteral("a Start menu shortcut")));
        return true;
    }

    if (best.score != 1 && byName())
        return true;

    if (error)
        *error = QObject::tr("I could not find anything called %1 on this machine.")
                     .arg(wanted);
    liveLog(QStringLiteral("computer use: nothing on this machine matches \"%1\"")
                .arg(wanted));
    return false;
}

bool openUrl(const QString &url, QString *opened, QString *error)
{
    QString wanted = url.trimmed();
    if (wanted.isEmpty()) {
        if (error)
            *error = QObject::tr("I did not catch which page to open.");
        return false;
    }

    // Spoken addresses arrive bare - "github.com", never "https://github.com" -
    // and the shell only treats a string as a web address once it has a scheme.
    // Without this Windows would take it for a file name and quietly fail.
    if (!wanted.contains(QStringLiteral("://")))
        wanted.prepend(QStringLiteral("https://"));

    // ONLY http and https ever reach ShellExecute. Every other scheme on this
    // machine is a protocol handler, and a misheard sentence must not be able
    // to fire one.
    const QUrl parsed(wanted, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.host().isEmpty()
        || (parsed.scheme() != QLatin1String("http")
            && parsed.scheme() != QLatin1String("https"))) {
        if (error) {
            *error = QObject::tr("%1 is not a web address I can open.")
                         .arg(url.trimmed());
        }
        return false;
    }

    // THE SHELL, NOT A BROWSER BY NAME. Windows gives the address to whichever
    // browser is already running as the user, so the page loads in the profile
    // they are signed in on rather than in a fresh window that knows nobody.
    const QString full = parsed.toString(QUrl::FullyEncoded);
    const HINSTANCE rc = ShellExecuteW(
        nullptr, L"open",
        reinterpret_cast<const wchar_t *>(full.utf16()),
        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        if (error)
            *error = QObject::tr("Windows would not open %1.").arg(parsed.host());
        return false;
    }

    if (opened)
        *opened = full;
    liveLog(QStringLiteral("computer use: handed %1 to the default browser")
                .arg(full));
    return true;
}

bool runCommand(const QString &command, QString *output, QString *error)
{
    const QString line = command.trimmed();
    if (line.isEmpty()) {
        if (error)
            *error = QObject::tr("No command was given.");
        return false;
    }

    liveLog(QStringLiteral("computer use: cmd /c %1").arg(line.left(200)));

    // No parent, and it deletes itself when it finishes, because a command that
    // outlives the wait below has to be allowed to carry on running.
    // `start` hands the program to Windows and the shell returns at once - but
    // the child inherits our output pipe and holds it open, so waiting on it
    // would block for as long as the program lives. Let it go instead.
    if (line.startsWith(QStringLiteral("start "), Qt::CaseInsensitive)) {
        QProcess launcher;
        launcher.setProgram(QStringLiteral("cmd.exe"));
        launcher.setNativeArguments(QStringLiteral("/s /c \"%1\"").arg(line));
        if (!launcher.startDetached()) {
            if (error)
                *error = QObject::tr("cmd would not start.");
            return false;
        }
        if (output)
            *output = QObject::tr("Started.");
        return true;
    }

    auto *proc = new QProcess;
    QObject::connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
    proc->setProcessChannelMode(QProcess::MergedChannels);

    // NATIVE ARGUMENTS, not a QStringList. QProcess quotes each argument for
    // itself, and cmd has its own quoting rules on top; between them a command
    // like powershell -c "..." came back with the quotes eaten and PowerShell
    // simply echoed the text instead of running it. /s plus one pair of outer
    // quotes tells cmd to take everything between them verbatim.
    proc->setProgram(QStringLiteral("cmd.exe"));
    proc->setNativeArguments(QStringLiteral("/s /c \"%1\"").arg(line));
    proc->start();

    if (!proc->waitForStarted(4000)) {
        if (error)
            *error = QObject::tr("cmd would not start.");
        proc->deleteLater();
        return false;
    }

    // Most of what gets asked for here finishes at once - `start` returns the
    // moment it has handed the program to Windows. Anything still going after
    // six seconds is left to run rather than killed.
    if (!proc->waitForFinished(6000)) {
        if (output)
            *output = QObject::tr("Started, and still running.");
        return true;
    }

    const int code = proc->exitCode();
    QString printed = QString::fromLocal8Bit(proc->readAll()).trimmed();
    if (printed.size() > 2000)
        printed = printed.left(2000) + QStringLiteral("... (cut short)");

    liveLog(QStringLiteral("computer use: cmd exit %1, %2 chars of output")
                .arg(code)
                .arg(printed.size()));

    if (output) {
        if (printed.isEmpty()) {
            *output = code == 0
                          ? QObject::tr("Done. It printed nothing.")
                          : QObject::tr("It exited with code %1 and printed nothing.").arg(code);
        } else if (code == 0) {
            *output = printed;
        } else {
            *output = QObject::tr("Exit code %1. %2").arg(code).arg(printed);
        }
    }
    return true;
}

QStringList knownKeys()
{
    QStringList names;
    for (const KeyEntry &k : kKeys)
        names.append(QString::fromLatin1(k.name));
    return names;
}

bool pressKey(const QString &keyName, QString *error)
{
    for (const KeyEntry &k : kKeys) {
        if (keyName != QLatin1String(k.name))
            continue;

        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = k.vk;
        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &down, sizeof(INPUT));
        QThread::msleep(25);
        SendInput(1, &up, sizeof(INPUT));

        liveLog(QStringLiteral("computer use: pressed %1").arg(keyName));
        return true;
    }

    if (error)
        *error = QObject::tr("I cannot press that key.");
    return false;
}

DesktopWindow findWindow(const QString &titlePart)
{
    const QString needle = titlePart.trimmed();
    if (needle.isEmpty())
        return {};
    for (const DesktopWindow &w : windows()) {
        if (w.title.contains(needle, Qt::CaseInsensitive))
            return w;
    }
    return {};
}

WindowShot capture(quint64 handle)
{
    WindowShot shot;
    HWND hwnd = reinterpret_cast<HWND>(handle);
    if (!hwnd || !IsWindow(hwnd))
        return shot;

    RECT r{};
    if (!GetWindowRect(hwnd, &r))
        return shot;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return shot;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // negative so the rows come out top down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ previous = SelectObject(mem, bmp);

    // 2 is PW_RENDERFULLCONTENT, which is what a modern composed window needs.
    // Older windows answer 0 and ignore the flag, so try both.
    BOOL ok = PrintWindow(hwnd, mem, 2);
    if (!ok)
        ok = PrintWindow(hwnd, mem, 0);

    if (ok && bits) {
        const QImage raw(reinterpret_cast<const uchar *>(bits), w, h, w * 4,
                         QImage::Format_RGB32);
        QImage image = raw.copy(); // the DIB goes away in a moment

        // A full screen picture is several megabytes once base64 encoded, and
        // the model reads it perfectly well at a third of that.
        constexpr int kLongestSide = 1280;
        const int longest = qMax(image.width(), image.height());
        if (longest > kLongestSide) {
            shot.scale = qreal(kLongestSide) / qreal(longest);
            image = image.scaled(int(image.width() * shot.scale),
                                 int(image.height() * shot.scale),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }

        QBuffer buffer(&shot.png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        shot.width = image.width();
        shot.height = image.height();
    }

    SelectObject(mem, previous);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    if (shot.isNull())
        liveLog(QStringLiteral("computer use: that window would not be captured"));
    return shot;
}

bool clickAt(quint64 handle, int windowX, int windowY, QString *error)
{
    HWND hwnd = reinterpret_cast<HWND>(handle);
    RECT r{};
    if (!hwnd || !IsWindow(hwnd) || !GetWindowRect(hwnd, &r)) {
        if (error)
            *error = QObject::tr("That window is not there any more.");
        return false;
    }

    const int x = r.left + windowX;
    const int y = r.top + windowY;
    if (x < r.left || x > r.right || y < r.top || y > r.bottom) {
        if (error)
            *error = QObject::tr("That point is outside the window.");
        return false;
    }

    SetForegroundWindow(hwnd);
    QThread::msleep(80);
    realClick(x, y);
    liveLog(QStringLiteral("computer use: clicked %1,%2 in the window")
                .arg(windowX)
                .arg(windowY));
    return true;
}

QList<DesktopWindow> windows()
{
    QList<DesktopWindow> out;
    EnumBag bag{&out};
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&bag));
    return out;
}

DesktopWindow foreground()
{
    HWND hwnd = GetForegroundWindow();
    if (!hwnd)
        return {};

    wchar_t title[512] = {};
    const int length = GetWindowTextW(hwnd, title, 511);

    DesktopWindow w;
    w.handle = quint64(hwnd);
    w.title = length > 0 ? QString::fromWCharArray(title, length) : QString();
    return w;
}

QList<DesktopControl> controls(quint64 handle, int limit)
{
    QList<DesktopControl> out;
    IUIAutomation *ui = automation();
    if (!ui || !handle)
        return out;

    IUIAutomationElement *root = nullptr;
    if (FAILED(ui->ElementFromHandle(reinterpret_cast<HWND>(handle), &root)) || !root)
        return out;

    IUIAutomationCondition *all = nullptr;
    ui->CreateTrueCondition(&all);

    IUIAutomationElementArray *found = nullptr;
    root->FindAll(TreeScope_Descendants, all, &found);

    if (found) {
        int count = 0;
        found->get_Length(&count);
        for (int i = 0; i < count && out.size() < limit; ++i) {
            IUIAutomationElement *e = nullptr;
            if (FAILED(found->GetElement(i, &e)) || !e)
                continue;

            CONTROLTYPEID type = 0;
            e->get_CurrentControlType(&type);

            BOOL offscreen = FALSE;
            e->get_CurrentIsOffscreen(&offscreen);

            if (!worthOffering(type) || offscreen) {
                e->Release();
                continue;
            }

            BSTR raw = nullptr;
            e->get_CurrentName(&raw);
            const QString name = fromBstr(raw).simplified();
            raw = nullptr;
            e->get_CurrentAutomationId(&raw);
            const QString id = fromBstr(raw);

            // Something has to be sayable about it or it cannot be chosen.
            if (name.isEmpty() && id.isEmpty()) {
                e->Release();
                continue;
            }

            RECT r{};
            e->get_CurrentBoundingRectangle(&r);

            DesktopControl c;
            c.key = QStringLiteral("c%1").arg(out.size());
            c.id = id;
            c.name = name;
            c.type = controlTypeName(type);
            c.x = r.left;
            c.y = r.top;
            c.w = r.right - r.left;
            c.h = r.bottom - r.top;
            out.append(c);

            e->Release();
        }
        found->Release();
    }

    if (all)
        all->Release();
    root->Release();
    return out;
}

bool click(quint64 handle, const DesktopControl &control, QString *error)
{
    IUIAutomationElement *e = refind(handle, control);

    if (e) {
        // Invoke where it is offered, then Select, then Expand: between them
        // those three cover buttons, menu items, list rows and tabs.
        IUnknown *pattern = nullptr;
        if (SUCCEEDED(e->GetCurrentPattern(UIA_InvokePatternId, &pattern)) && pattern) {
            auto *invoke = static_cast<IUIAutomationInvokePattern *>(pattern);
            const HRESULT hr = invoke->Invoke();
            invoke->Release();
            e->Release();
            if (SUCCEEDED(hr)) {
                liveLog(QStringLiteral("computer use: invoked %1").arg(control.describe()));
                return true;
            }
            e = nullptr;
        } else if (SUCCEEDED(e->GetCurrentPattern(UIA_SelectionItemPatternId, &pattern))
                   && pattern) {
            auto *select = static_cast<IUIAutomationSelectionItemPattern *>(pattern);
            const HRESULT hr = select->Select();
            select->Release();
            e->Release();
            if (SUCCEEDED(hr)) {
                liveLog(QStringLiteral("computer use: selected %1").arg(control.describe()));
                return true;
            }
            e = nullptr;
        } else {
            e->Release();
            e = nullptr;
        }
    }

    // No pattern, or the pattern refused. Fall back to a real click on the
    // middle of where the control was.
    if (control.w <= 0 || control.h <= 0) {
        if (error)
            *error = QObject::tr("I could not find that on screen.");
        return false;
    }

    SetForegroundWindow(reinterpret_cast<HWND>(handle));
    QThread::msleep(80);
    realClick(control.x + control.w / 2, control.y + control.h / 2);
    liveLog(QStringLiteral("computer use: clicked %1 with the mouse")
                .arg(control.describe()));
    return true;
}

bool fill(quint64 handle, const DesktopControl &control, const QString &text,
          QString *error)
{
    if (text.isEmpty()) {
        if (error)
            *error = QObject::tr("I did not catch what to type.");
        return false;
    }

    HWND top = reinterpret_cast<HWND>(handle);
    if (!top || !IsWindow(top)) {
        if (error)
            *error = QObject::tr("That window is not there any more.");
        return false;
    }

    // THE WINDOW COMES FIRST, before any of the three ways of writing text.
    // A background window takes no keystrokes, a packaged one takes nothing at
    // all, and UI Automation cheerfully reports success either way - which is
    // precisely what "it will not type into the Store" looked like.
    if (!bringToFront(top)) {
        liveLog(QStringLiteral("computer use: that window would not come to the "
                               "front, typing into it anyway"));
    }

    IUIAutomationElement *e = refind(handle, control, true);

    bool readable = false;
    const QString before = readValue(e, &readable);

    // NOTHING HERE IS TRUSTED WITHOUT READING IT BACK. Every route below can
    // return S_OK and change nothing, so each one is checked before the next
    // is skipped.
    const auto took = [&]() -> bool {
        bool stillReadable = false;
        const QString now = readValue(e, &stillReadable);
        return stillReadable && now.contains(text);
    };

    // 1. The value pattern. Fastest, and it disturbs neither the caret nor the
    //    mouse, so it is worth trying first where the field offers one.
    if (e && supportsValueWrite(e)) {
        IUnknown *pattern = nullptr;
        if (SUCCEEDED(e->GetCurrentPattern(UIA_ValuePatternId, &pattern)) && pattern) {
            auto *value = static_cast<IUIAutomationValuePattern *>(pattern);
            BSTR bstr = SysAllocStringLen(
                reinterpret_cast<const OLECHAR *>(text.utf16()), UINT(text.size()));
            value->SetValue(bstr);
            SysFreeString(bstr);
            value->Release();
            QThread::msleep(60);
            if (took()) {
                liveLog(QStringLiteral("computer use: filled %1")
                            .arg(control.describe()));
                e->Release();
                return true;
            }
            liveLog(QStringLiteral("computer use: the value pattern on %1 said yes "
                                   "and changed nothing")
                        .arg(control.describe()));
        }
    }

    // 2. The old accessibility pattern. Some XAML fields answer this one when
    //    they quietly ignore a value pattern write.
    if (e) {
        IUnknown *pattern = nullptr;
        if (SUCCEEDED(e->GetCurrentPattern(UIA_LegacyIAccessiblePatternId, &pattern))
            && pattern) {
            auto *legacy = static_cast<IUIAutomationLegacyIAccessiblePattern *>(pattern);
            BSTR bstr = SysAllocStringLen(
                reinterpret_cast<const OLECHAR *>(text.utf16()), UINT(text.size()));
            legacy->SetValue(bstr);
            SysFreeString(bstr);
            legacy->Release();
            QThread::msleep(60);
            if (took()) {
                liveLog(QStringLiteral("computer use: filled %1 the old way")
                            .arg(control.describe()));
                e->Release();
                return true;
            }
        }
    }

    // 3. Focus it and type for real. THIS IS WHAT ACTUALLY WORKS ON A STORE
    //    APP: the keystrokes go through the app's own input handling, so a
    //    search box reacts exactly as it would to a person - which matters,
    //    because a search box that has its text set behind its back often does
    //    not search.
    bool focused = false;
    if (e) {
        e->SetFocus();
        QThread::msleep(90);
        BOOL has = FALSE;
        e->get_CurrentHasKeyboardFocus(&has);
        focused = has;
    }
    if (!focused && control.w > 0 && control.h > 0) {
        // SetFocus refused, or there was no element at all. Click the middle of
        // where the control was: a real click is what gives a XAML field its
        // caret, and a focus call that reports success without one is common.
        realClick(control.x + control.w / 2, control.y + control.h / 2);
        QThread::msleep(120);
        focused = true;
    }
    if (!focused) {
        if (error)
            *error = QObject::tr("I could not get into that field.");
        if (e)
            e->Release();
        return false;
    }

    // Only clear it when we can see there is something to clear. Firing ctrl+A
    // and delete blind is how you empty a document that merely looked like a
    // text box.
    if (readable && !before.isEmpty()) {
        pressWithControl('A');
        pressPlain(VK_DELETE);
    }

    typeUnicode(text);
    QThread::msleep(80);

    if (took()) {
        liveLog(QStringLiteral("computer use: typed into %1").arg(control.describe()));
        if (e)
            e->Release();
        return true;
    }

    bool stillReadable = false;
    readValue(e, &stillReadable);
    if (e)
        e->Release();

    if (stillReadable) {
        // It can be read, and it does not hold what we sent. That is a real
        // failure and the model is told so, rather than being left to believe
        // the field is filled in.
        if (error)
            *error = QObject::tr("I typed it but the field did not take it.");
        liveLog(QStringLiteral("computer use: %1 refused every way of typing into it")
                    .arg(control.describe()));
        return false;
    }

    // Nothing to read back. The keystrokes were delivered to whatever holds the
    // keyboard, which is as far as this can go, so call it done but say in the
    // log that it was not checked.
    liveLog(QStringLiteral("computer use: typed into %1 unverified - it reports no "
                           "value to read")
                .arg(control.describe()));
    return true;
}

// ---------------------------------------------------------------------------
// ONE ACTION, AS THE MODEL ASKED FOR IT.
//
// Everything below runs on the hands thread, so it may block and it may sleep:
// nothing the user can see or hear waits on it. It used to live in MainWindow
// and hold the thread the orb draws on for up to nine seconds, inside nested
// event loops, while a program started.
// ---------------------------------------------------------------------------

namespace {

// Sleep in short slices, letting go the moment the task is stopped. False if
// it was.
bool pauseFor(int ms, const std::atomic_bool &cancelled)
{
    for (int waited = 0; waited < ms; waited += 50) {
        if (cancelled.load())
            return false;
        QThread::msleep(ulong(qMin(50, ms - waited)));
    }
    return !cancelled.load();
}

QString act(const QVariantMap &args, DeskState &state,
            const std::atomic_bool &cancelled, QByteArray *png)
{
    const QString action = args.value(QStringLiteral("action")).toString();
    QString error;

    const auto namedControl = [&args, &state]() -> DesktopControl {
        const QString wanted = args.value(QStringLiteral("control")).toString().trimmed();
        for (const DesktopControl &c : std::as_const(state.offered)) {
            if (c.key.compare(wanted, Qt::CaseInsensitive) == 0)
                return c;
        }
        return {};
    };

    // THE LIST OF CONTROLS IS FROM BEFORE THE LAST CHANGE, and a step that
    // opens something moves the screen on underneath it. When the model names
    // a key that is no longer there, re-read what is in front and hand the NEW
    // list back, instead of aiming at whatever the old key pointed to.
    //
    // THE TASK'S OWN WINDOW, never simply whatever is in front. With several
    // tasks at work the window in front may well be another task's, and
    // re-reading that handed one task the other's controls - the calculator
    // task found itself holding the browser's list. Only when its own window
    // has gone does it fall back to the one in front.
    const auto listAgain = [&state]() -> QString {
        DesktopWindow again;
        const HWND own = reinterpret_cast<HWND>(state.target.handle);
        if (own && IsWindow(own)) {
            wchar_t title[512] = {};
            const int length = GetWindowTextW(own, title, 511);
            again.handle = state.target.handle;
            again.title = length > 0 ? QString::fromWCharArray(title, length)
                                     : state.target.title;
        } else {
            again = foreground();
        }
        return QObject::tr("Failed: that control is not on screen any more - the "
                           "screen has changed since the list you were given. %1")
            .arg(describeControls(again, state));
    };

    if (action == QLatin1String("launch")) {
        const QString name = args.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty())
            return QObject::tr("Failed: no program was named.");

        // What is already open, so the new window can be told from the rest.
        QSet<quint64> before;
        for (const DesktopWindow &w : windows())
            before.insert(w.handle);

        QString opened;
        if (!launchAnything(name, &opened, &error))
            return QObject::tr("Failed: %1").arg(error);
        if (opened.isEmpty())
            opened = name;

        // WAIT FOR THE WINDOW. The next move is nearly always to type into what
        // was just opened, and a program takes a second or two to put a window
        // up - handing back now would hand back whatever was in front BEFORE.
        DesktopWindow fresh;
        QElapsedTimer clock;
        clock.start();
        while (fresh.isNull() && clock.elapsed() < 9000) {
            if (!pauseFor(150, cancelled))
                return QObject::tr("Stopped.");
            for (const DesktopWindow &w : windows()) {
                if (!before.contains(w.handle)) {
                    fresh = w;
                    break;
                }
            }
        }

        if (fresh.isNull()) {
            return QObject::tr("%1 was started but it has not put a window up yet. "
                               "Use list_windows in a moment to see whether it came up.")
                .arg(opened);
        }

        // A XAML app reports almost nothing for its first second on screen, so
        // reading its controls the instant the window exists finds an empty
        // shell. Let it finish drawing itself.
        if (!pauseFor(900, cancelled))
            return QObject::tr("Stopped.");
        return QObject::tr("Opened %1. %2").arg(opened, describeControls(fresh, state));
    }

    if (action == QLatin1String("open_url")) {
        const QString url = args.value(QStringLiteral("url")).toString().trimmed();
        QString opened;
        if (!openUrl(url, &opened, &error))
            return QObject::tr("Failed: %1").arg(error);

        // A page takes a moment to come up and the next move is to look at
        // it, so the controls handed back have to be the NEW ones.
        if (!pauseFor(2600, cancelled))
            return QObject::tr("Stopped.");

        const DesktopWindow front = foreground();
        if (front.isNull())
            return QObject::tr("Opened %1 in the user's own browser.").arg(opened);
        return QObject::tr("Opened %1 in the browser the user already had open, so "
                           "they are signed in there as themselves. %2")
            .arg(opened, describeControls(front, state));
    }

    if (action == QLatin1String("run_command")) {
        const QString command = args.value(QStringLiteral("command")).toString().trimmed();
        QString printed;
        if (runCommand(command, &printed, &error))
            return printed;
        return QObject::tr("Failed: %1").arg(error);
    }

    if (action == QLatin1String("list_windows")) {
        const QList<DesktopWindow> open = windows();
        if (open.isEmpty())
            return QObject::tr("Nothing is open.");
        QStringList titles;
        for (const DesktopWindow &w : open)
            titles.append(w.title);
        return QObject::tr("%1 windows are open: %2")
            .arg(open.size())
            .arg(titles.join(QStringLiteral("; ")));
    }

    if (action == QLatin1String("screenshot")) {
        const QString wanted = args.value(QStringLiteral("window")).toString().trimmed();
        const DesktopWindow target = wanted.isEmpty() ? foreground() : findWindow(wanted);
        if (target.isNull())
            return QObject::tr("Failed: I could not find that window.");

        const WindowShot shot = capture(target.handle);
        if (shot.isNull())
            return QObject::tr("Failed: that window would not be captured.");

        state.shotWindow = target.handle;
        state.shotScale = shot.scale;
        if (png)
            *png = shot.png;

        return QObject::tr("Screenshot of '%1', %2 by %3 pixels.")
            .arg(target.title)
            .arg(shot.width)
            .arg(shot.height);
    }

    if (action == QLatin1String("click_at")) {
        if (state.shotWindow == 0) {
            return QObject::tr("Failed: take a screenshot first, so there is a "
                               "picture to pick a point out of.");
        }
        const int pictureX = args.value(QStringLiteral("x")).toInt();
        const int pictureY = args.value(QStringLiteral("y")).toInt();

        // Back out of the picture's pixels into the window's own.
        const int windowX = int(pictureX / state.shotScale);
        const int windowY = int(pictureY / state.shotScale);

        if (clickAt(state.shotWindow, windowX, windowY, &error)) {
            return QObject::tr("Clicked at %1, %2 in that screenshot.")
                .arg(pictureX)
                .arg(pictureY);
        }
        return QObject::tr("Failed: %1").arg(error);
    }

    if (action == QLatin1String("list_controls")) {
        const QString wanted = args.value(QStringLiteral("window")).toString().trimmed();
        const DesktopWindow target = wanted.isEmpty() ? foreground() : findWindow(wanted);
        if (target.isNull())
            return QObject::tr("Failed: I could not find that window.");
        return describeControls(target, state);
    }

    if (action == QLatin1String("click")) {
        const DesktopControl target = namedControl();
        if (target.key.isEmpty())
            return listAgain();
        if (click(state.target.handle, target, &error))
            return QObject::tr("Clicked %1.").arg(target.name.isEmpty() ? target.type : target.name);
        return QObject::tr("Failed: %1").arg(error);
    }

    if (action == QLatin1String("press_key")) {
        const QString key = args.value(QStringLiteral("key")).toString().trimmed().toLower();
        if (pressKey(key, &error))
            return QObject::tr("Pressed %1.").arg(key);
        return QObject::tr("Failed: %1").arg(error);
    }

    if (action == QLatin1String("type_text")) {
        const QString text = args.value(QStringLiteral("text")).toString();
        const DesktopControl target = namedControl();
        if (text.isEmpty())
            return QObject::tr("Failed: no text was given.");
        if (target.key.isEmpty())
            return listAgain();
        if (fill(state.target.handle, target, text, &error))
            return QObject::tr("Typed it in.");
        return QObject::tr("Failed: %1").arg(error);
    }

    return QObject::tr("Failed: %1 is not something I can do.").arg(action);
}

// The thread every action runs on. It runs a message loop - exec() - under a
// single threaded COM apartment, which is what UI Automation and the shell's
// app list were written for, and between actions COM can deliver to it.
class HandsThread : public QThread
{
protected:
    void run() override
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        exec();
        CoUninitialize();
    }
};

QObject *hands()
{
    static QObject *const context = []() {
        auto *thread = new HandsThread;
        thread->setObjectName(QStringLiteral("Burnos hands"));
        auto *object = new QObject;
        object->moveToThread(thread);
        thread->start();
        // Never deleted, on purpose: a QThread destroyed while an action is
        // still sleeping takes the whole process down with it. At exit it is
        // asked to stop and given a moment to finish what it is doing.
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, qApp, [thread]() {
            thread->quit();
            thread->wait(3000);
        });
        return object;
    }();
    return context;
}

} // namespace

QString describeControls(const DesktopWindow &target, DeskState &state)
{
    state.target = target;
    state.offered = controls(target.handle);

    if (target.isNull() || target.title.isEmpty()) {
        return QObject::tr("There is no window in front, so there is nothing to "
                           "click or type into.");
    }
    if (state.offered.isEmpty()) {
        return QObject::tr("'%1' offers nothing that can be clicked or typed into. "
                           "Take a screenshot of it and use click_at instead.")
            .arg(target.title);
    }

    QStringList lines;
    for (const DesktopControl &c : std::as_const(state.offered))
        lines.append(QStringLiteral("%1: %2").arg(c.key, c.describe()));
    return QObject::tr("'%1' has these controls, and the key to give back for each "
                       "one:\n%2")
        .arg(target.title, lines.join(QLatin1Char('\n')));
}

DeskResult perform(const QVariantMap &args, const DeskState &state,
                   const std::atomic_bool &cancelled)
{
    DeskResult out;
    out.state = state;
    out.text = act(args, out.state, cancelled, &out.png);
    return out;
}

void onHands(QObject *guard, std::function<DeskResult()> job,
             std::function<void(const DeskResult &)> done)
{
    const QPointer<QObject> alive(guard);
    QMetaObject::invokeMethod(
        hands(),
        [alive, job, done]() {
            const DeskResult result = job();
            QMetaObject::invokeMethod(
                qApp,
                [alive, result, done]() {
                    if (alive)
                        done(result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

} // namespace Computer
} // namespace BurnOS
