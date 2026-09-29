#include "computeruse.h"
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
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <vector>

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

// A picture of one window or screen, shrunk to fit. The model answers in the
// PICTURE's pixels, and `scale` turns a point back into a real one.
struct WindowShot
{
    QByteArray png;
    int width = 0;
    int height = 0;
    qreal scale = 1.0; // picture pixels per window pixel
};

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

// Names that are not programs at all, or not by that name: "settings" must
// mean the Settings app rather than any shortcut called Settings, and
// "calculator" is calc.
struct AppEntry
{
    const char *key;
    const char *label;
    const char *command;
};

const AppEntry kApps[] = {
    {"firefox", "Firefox", "firefox"},
    {"chrome", "Chrome", "chrome"},
    {"edge", "Edge", "msedge"},
    {"notepad", "Notepad", "notepad"},
    {"explorer", "File Explorer", "explorer"},
    {"calculator", "Calculator", "calc"},
    {"terminal", "Terminal", "wt"},
    {"powershell", "PowerShell", "powershell"},
    {"settings", "Windows Settings", "ms-settings:"},
    {"taskmgr", "Task Manager", "taskmgr"},
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
    QString line = QStringLiteral("%1 '%2'").arg(type, label);
    if (!value.isEmpty())
        line += QStringLiteral(" = '%1'").arg(value);
    if (!state.isEmpty())
        line += QStringLiteral(" (%1)").arg(state);
    return line;
}

namespace BurnOS {
namespace Computer {
namespace {

// Defined further down with the actions that use them most; click and fill
// need them here.
void clickScreen(int x, int y, const QString &button, int clicks, bool stay);
bool sendKeys(const QString &keys, QString *error);
void typeHere(const QString &text);

QList<DesktopWindow> windows()
{
    QList<DesktopWindow> out;
    EnumBag bag{&out};
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&bag));
    return out;
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
            // ShellExecute resolves App Paths, so "firefox" finds Firefox
            // wherever it was installed, and it opens ms-settings: too.
            if (!shellOpen(QString::fromLatin1(a.command))) {
                if (error)
                    *error = QObject::tr("%1 would not start.").arg(QLatin1String(a.label));
                return false;
            }
            if (opened)
                *opened = QString::fromLatin1(a.label);
            liveLog(QStringLiteral("computer use: launched %1").arg(QLatin1String(a.label)));
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

} // namespace

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

namespace {

QList<DesktopControl> controls(quint64 handle, int limit = 50)
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

            // WHAT STATE IT IS IN, because "tick the box" means nothing to a
            // model that cannot tell whether it is ticked already.
            QStringList states;
            BOOL enabled = TRUE;
            e->get_CurrentIsEnabled(&enabled);
            if (!enabled)
                states.append(QStringLiteral("disabled"));
            IUnknown *pattern = nullptr;
            if (SUCCEEDED(e->GetCurrentPattern(UIA_TogglePatternId, &pattern)) && pattern) {
                ToggleState toggle = ToggleState_Off;
                static_cast<IUIAutomationTogglePattern *>(pattern)->get_CurrentToggleState(&toggle);
                pattern->Release();
                pattern = nullptr;
                states.append(toggle == ToggleState_On    ? QStringLiteral("on")
                              : toggle == ToggleState_Off ? QStringLiteral("off")
                                                          : QStringLiteral("mixed"));
            }
            if (SUCCEEDED(e->GetCurrentPattern(UIA_SelectionItemPatternId, &pattern)) && pattern) {
                BOOL selected = FALSE;
                static_cast<IUIAutomationSelectionItemPattern *>(pattern)->get_CurrentIsSelected(&selected);
                pattern->Release();
                pattern = nullptr;
                if (selected)
                    states.append(QStringLiteral("selected"));
            }
            if (SUCCEEDED(e->GetCurrentPattern(UIA_ExpandCollapsePatternId, &pattern)) && pattern) {
                ExpandCollapseState expand = ExpandCollapseState_LeafNode;
                static_cast<IUIAutomationExpandCollapsePattern *>(pattern)->get_CurrentExpandCollapseState(&expand);
                pattern->Release();
                pattern = nullptr;
                if (expand == ExpandCollapseState_Expanded)
                    states.append(QStringLiteral("expanded"));
                else if (expand == ExpandCollapseState_Collapsed)
                    states.append(QStringLiteral("collapsed"));
            }
            c.state = states.join(QStringLiteral(", "));
            if (type == UIA_EditControlTypeId || type == UIA_ComboBoxControlTypeId) {
                bool readable = false;
                QString value = readValue(e, &readable).simplified();
                if (value.size() > 40)
                    value = value.left(37) + QStringLiteral("...");
                if (value != name)
                    c.value = value;
            }
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
    clickScreen(control.x + control.w / 2, control.y + control.h / 2, QStringLiteral("left"), 1, false);
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
        clickScreen(control.x + control.w / 2, control.y + control.h / 2, QStringLiteral("left"), 1, false);
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
        sendKeys(QStringLiteral("ctrl+a delete"), nullptr);
    }

    typeHere(text);
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

// ---------------------------------------------------------------------------
// THE MOUSE: any button, once or twice, dragging, the wheel.
// ---------------------------------------------------------------------------

void sendMouse(DWORD flags, LONG data = 0)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = DWORD(data);
    SendInput(1, &in, sizeof(INPUT));
}

// The pointer is put back where the user left it afterwards, unless `stay`:
// a hover is meant to leave it where it landed.
void clickScreen(int x, int y, const QString &button, int clicks, bool stay = false)
{
    POINT was{};
    GetCursorPos(&was);
    SetCursorPos(x, y);
    QThread::msleep(40);

    DWORD down = MOUSEEVENTF_LEFTDOWN;
    DWORD up = MOUSEEVENTF_LEFTUP;
    if (button == QLatin1String("right")) {
        down = MOUSEEVENTF_RIGHTDOWN;
        up = MOUSEEVENTF_RIGHTUP;
    } else if (button == QLatin1String("middle")) {
        down = MOUSEEVENTF_MIDDLEDOWN;
        up = MOUSEEVENTF_MIDDLEUP;
    }
    const int times = qBound(1, clicks, 3);
    for (int i = 0; i < times; ++i) {
        sendMouse(down);
        QThread::msleep(30);
        sendMouse(up);
        if (i + 1 < times)
            QThread::msleep(60); // well inside any double click time
    }
    QThread::msleep(40);
    if (!stay)
        SetCursorPos(was.x, was.y);
}

void dragScreen(const QPoint &from, const QPoint &to)
{
    SetCursorPos(from.x(), from.y());
    QThread::msleep(60);
    sendMouse(MOUSEEVENTF_LEFTDOWN);
    QThread::msleep(80);
    // In steps, because many drop targets only notice a drag that moves.
    constexpr int kSteps = 12;
    for (int i = 1; i <= kSteps; ++i) {
        SetCursorPos(from.x() + (to.x() - from.x()) * i / kSteps,
                     from.y() + (to.y() - from.y()) * i / kSteps);
        QThread::msleep(15);
    }
    QThread::msleep(80);
    sendMouse(MOUSEEVENTF_LEFTUP);
}

// Positive notches scroll up, or right.
void wheelAt(int x, int y, int notches, bool horizontal)
{
    SetCursorPos(x, y);
    QThread::msleep(40);
    sendMouse(horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL, LONG(notches) * WHEEL_DELTA);
    QThread::msleep(60);
}

// ---------------------------------------------------------------------------
// THE KEYBOARD: any key, any chord, any sequence.
// ---------------------------------------------------------------------------

struct NamedKey
{
    const char *name;
    WORD vk;
    bool extended;
};

const NamedKey kNamedKeys[] = {
    {"enter", VK_RETURN, false},      {"return", VK_RETURN, false},
    {"escape", VK_ESCAPE, false},     {"esc", VK_ESCAPE, false},
    {"tab", VK_TAB, false},           {"space", VK_SPACE, false},
    {"backspace", VK_BACK, false},    {"delete", VK_DELETE, true},
    {"del", VK_DELETE, true},         {"insert", VK_INSERT, true},
    {"home", VK_HOME, true},          {"end", VK_END, true},
    {"pageup", VK_PRIOR, true},       {"pagedown", VK_NEXT, true},
    {"up", VK_UP, true},              {"down", VK_DOWN, true},
    {"left", VK_LEFT, true},          {"right", VK_RIGHT, true},
    {"ctrl", VK_CONTROL, false},      {"control", VK_CONTROL, false},
    {"alt", VK_MENU, false},          {"shift", VK_SHIFT, false},
    {"win", VK_LWIN, true},           {"windows", VK_LWIN, true},
    {"printscreen", VK_SNAPSHOT, false}, {"capslock", VK_CAPITAL, false},
    {"menu", VK_APPS, true},          {"volumeup", VK_VOLUME_UP, true},
    {"volumedown", VK_VOLUME_DOWN, true}, {"mute", VK_VOLUME_MUTE, true},
    {"playpause", VK_MEDIA_PLAY_PAUSE, true},
    {"nexttrack", VK_MEDIA_NEXT_TRACK, true},
    {"prevtrack", VK_MEDIA_PREV_TRACK, true},
};

// A key by name: a named key, f1 to f24, or any single character the keyboard
// can type - `shift` is set where the character needs it.
bool resolveKey(const QString &name, WORD *vk, bool *extended, bool *shift)
{
    *extended = false;
    *shift = false;
    for (const NamedKey &k : kNamedKeys) {
        if (name == QLatin1String(k.name)) {
            *vk = k.vk;
            *extended = k.extended;
            return true;
        }
    }
    if (name.size() >= 2 && name.size() <= 3 && name.startsWith(QLatin1Char('f'))) {
        bool ok = false;
        const int n = name.mid(1).toInt(&ok);
        if (ok && n >= 1 && n <= 24) {
            *vk = WORD(VK_F1 + n - 1);
            return true;
        }
    }
    if (name.size() == 1) {
        const SHORT scan = VkKeyScanW(wchar_t(name.at(0).unicode()));
        if (scan == -1)
            return false;
        *vk = LOBYTE(scan);
        *shift = (HIBYTE(scan) & 1) != 0;
        return true;
    }
    return false;
}

INPUT keyInput(WORD vk, bool extended, bool up)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
    return in;
}

// One chord - "ctrl+shift+s", "alt+f4", "win+r", "enter" - pressed in order
// and let go in reverse.
bool sendChord(const QString &chord, QString *error)
{
    struct Key
    {
        WORD vk;
        bool extended;
    };
    std::vector<Key> keys;
    const QStringList parts = chord.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString &raw : parts) {
        const QString part = raw.trimmed().toLower();
        WORD vk = 0;
        bool extended = false;
        bool shift = false;
        if (!resolveKey(part, &vk, &extended, &shift)) {
            if (error)
                *error = QObject::tr("I do not know a key called '%1'.").arg(part);
            return false;
        }
        const bool haveShift = std::any_of(keys.begin(), keys.end(),
                                           [](const Key &k) { return k.vk == VK_SHIFT; });
        if (shift && !haveShift)
            keys.push_back({VK_SHIFT, false});
        keys.push_back({vk, extended});
    }
    if (keys.empty()) {
        if (error)
            *error = QObject::tr("No keys were named.");
        return false;
    }
    std::vector<INPUT> seq;
    for (const Key &k : keys)
        seq.push_back(keyInput(k.vk, k.extended, false));
    for (auto it = keys.rbegin(); it != keys.rend(); ++it)
        seq.push_back(keyInput(it->vk, it->extended, true));
    SendInput(UINT(seq.size()), seq.data(), sizeof(INPUT));
    QThread::msleep(50);
    return true;
}

// Chords one after another, separated by spaces or commas: "ctrl+a delete".
bool sendKeys(const QString &keys, QString *error)
{
    QString spaced = keys;
    spaced.replace(QLatin1Char(','), QLatin1Char(' '));
    const QStringList chords = spaced.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (chords.isEmpty()) {
        if (error)
            *error = QObject::tr("No keys were named.");
        return false;
    }
    for (const QString &chord : chords) {
        if (!sendChord(chord, error))
            return false;
        QThread::msleep(40);
    }
    return true;
}

// Typed into whatever has the keyboard, a new line as Enter and a tab as Tab.
void typeHere(const QString &text)
{
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\r'))
            continue;
        if (c == QLatin1Char('\n')) {
            sendChord(QStringLiteral("enter"), nullptr);
            continue;
        }
        if (c == QLatin1Char('\t')) {
            sendChord(QStringLiteral("tab"), nullptr);
            continue;
        }
        INPUT pair[2] = {};
        pair[0].type = INPUT_KEYBOARD;
        pair[0].ki.dwFlags = KEYEVENTF_UNICODE;
        pair[0].ki.wScan = c.unicode();
        pair[1] = pair[0];
        pair[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(2, pair, sizeof(INPUT));
        // Notepad dropped the letter after a space at 8 ms a key - its spell
        // check runs on every word break - so a word break gets longer.
        QThread::msleep(c.isSpace() || c.isPunct() ? 35 : 12);
    }
}

// ---------------------------------------------------------------------------
// THE CLIPBOARD, straight through Win32: QClipboard belongs to the thread the
// orb draws on, and this runs on the hands.
// ---------------------------------------------------------------------------

bool openClipboardPatiently()
{
    // Another program can be holding it for a moment; that is not a failure.
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(nullptr))
            return true;
        QThread::msleep(30);
    }
    return false;
}

bool clipboardText(QString *text)
{
    if (!openClipboardPatiently())
        return false;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto *chars = static_cast<const wchar_t *>(GlobalLock(data))) {
            *text = QString::fromWCharArray(chars);
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return true;
}

bool setClipboardText(const QString &text)
{
    if (!openClipboardPatiently())
        return false;
    EmptyClipboard();
    const size_t bytes = size_t(text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (memory) {
        if (auto *chars = static_cast<wchar_t *>(GlobalLock(memory))) {
            text.toWCharArray(chars);
            chars[text.size()] = 0;
            GlobalUnlock(memory);
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok)
            GlobalFree(memory); // the clipboard only owns it once it took it
    }
    CloseClipboard();
    return ok;
}

// ---------------------------------------------------------------------------
// READING: the words in a window, without having to look at a picture.
// ---------------------------------------------------------------------------

QString rangeText(IUIAutomationElement *element, int maxChars)
{
    IUnknown *pattern = nullptr;
    if (FAILED(element->GetCurrentPattern(UIA_TextPatternId, &pattern)) || !pattern)
        return {};
    auto *text = static_cast<IUIAutomationTextPattern *>(pattern);
    QString out;
    IUIAutomationTextRange *range = nullptr;
    if (SUCCEEDED(text->get_DocumentRange(&range)) && range) {
        BSTR raw = nullptr;
        if (SUCCEEDED(range->GetText(maxChars, &raw)))
            out = fromBstr(raw);
        range->Release();
    }
    text->Release();
    return out;
}

// Three ways, best first: the element's own text, where it is a document or a
// field; the biggest document inside it - a web page in a browser, the file in
// an editor; and failing both, the names and values of everything visible.
QString readText(HWND top, const DesktopControl *only, int maxChars)
{
    IUIAutomation *ui = automation();
    if (!ui || !top || !IsWindow(top))
        return {};

    IUIAutomationElement *root = nullptr;
    if (only) {
        root = refind(quint64(top), *only);
    } else {
        const HWND host = uiaHost(top);
        if (FAILED(ui->ElementFromHandle(host, &root)) || !root) {
            root = nullptr;
            if (host != top && (FAILED(ui->ElementFromHandle(top, &root)) || !root))
                root = nullptr;
        }
    }
    if (!root)
        return {};

    QString text = rangeText(root, maxChars);

    if (text.trimmed().isEmpty()) {
        VARIANT yes;
        VariantInit(&yes);
        yes.vt = VT_BOOL;
        yes.boolVal = VARIANT_TRUE;
        IUIAutomationCondition *condition = nullptr;
        if (SUCCEEDED(ui->CreatePropertyCondition(UIA_IsTextPatternAvailablePropertyId,
                                                  yes, &condition))
            && condition) {
            IUIAutomationElementArray *found = nullptr;
            if (SUCCEEDED(root->FindAll(TreeScope_Descendants, condition, &found)) && found) {
                int count = 0;
                found->get_Length(&count);
                for (int i = 0; i < count && i < 20; ++i) {
                    IUIAutomationElement *e = nullptr;
                    if (FAILED(found->GetElement(i, &e)) || !e)
                        continue;
                    const QString candidate = rangeText(e, maxChars);
                    if (candidate.trimmed().size() > text.trimmed().size())
                        text = candidate;
                    e->Release();
                }
                found->Release();
            }
            condition->Release();
        }
    }

    // A browser's page or an editor's file IS its document text. An app like
    // Settings has only a line or two of that and everything else in labels,
    // so short document text is topped up with every visible name and value -
    // or the page's own heading could not be found in it.
    if (text.trimmed().size() < 500) {
        IUIAutomationCondition *all = nullptr;
        ui->CreateTrueCondition(&all);
        IUIAutomationElementArray *found = nullptr;
        if (all)
            root->FindAll(TreeScope_Descendants, all, &found);
        QStringList lines;
        int used = 0;
        if (found) {
            int count = 0;
            found->get_Length(&count);
            for (int i = 0; i < count && i < 3000 && used < maxChars; ++i) {
                IUIAutomationElement *e = nullptr;
                if (FAILED(found->GetElement(i, &e)) || !e)
                    continue;
                BOOL offscreen = FALSE;
                e->get_CurrentIsOffscreen(&offscreen);
                if (!offscreen) {
                    BSTR raw = nullptr;
                    e->get_CurrentName(&raw);
                    QString line = fromBstr(raw).simplified();
                    bool readable = false;
                    const QString value = readValue(e, &readable).simplified();
                    if (!value.isEmpty() && value != line)
                        line = line.isEmpty() ? value : line + QStringLiteral(": ") + value;
                    if (!line.isEmpty() && (lines.isEmpty() || lines.last() != line)) {
                        lines.append(line);
                        used += int(line.size()) + 1;
                    }
                }
                e->Release();
            }
            found->Release();
        }
        if (all)
            all->Release();
        const QString labels = lines.join(QLatin1Char('\n'));
        text = text.trimmed().isEmpty() ? labels : text.trimmed() + QLatin1Char('\n') + labels;
    }

    root->Release();
    if (text.size() > maxChars)
        text = text.left(maxChars) + QStringLiteral("\n... (there is more)");
    return text;
}

// ---------------------------------------------------------------------------
// SEEING: a window or the whole screen, with the controls marked on it.
// ---------------------------------------------------------------------------

// A 32 bit picture `w` by `h`, drawn by `draw` into a DIB section. The height
// goes in negative so the rows come out top down.
QImage grabInto(int w, int h, const std::function<bool(HDC mem, HDC screen)> &draw)
{
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ previous = SelectObject(mem, bmp);
    QImage image;
    if (draw(mem, screen) && bits)
        image = QImage(static_cast<const uchar *>(bits), w, h, w * 4, QImage::Format_RGB32).copy();
    SelectObject(mem, previous);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return image;
}

QImage grabWindowImage(HWND hwnd, QRect *where)
{
    RECT r{};
    if (!hwnd || !IsWindow(hwnd) || !GetWindowRect(hwnd, &r) || r.right <= r.left
        || r.bottom <= r.top)
        return {};
    *where = QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    // 2 is PW_RENDERFULLCONTENT, which a modern composed window needs; an old
    // one ignores it and wants 0.
    return grabInto(where->width(), where->height(), [hwnd](HDC mem, HDC) {
        return PrintWindow(hwnd, mem, 2) || PrintWindow(hwnd, mem, 0);
    });
}

// The monitor a window is on, as the user sees it - menus, tooltips and
// covering windows included. ONE monitor, not all of them: every monitor side
// by side came out 1280 by 360 once shrunk, too small to read anything.
QImage grabScreenImage(HWND onScreenOf, QRect *where)
{
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = onScreenOf && IsWindow(onScreenOf)
                                 ? MonitorFromWindow(onScreenOf, MONITOR_DEFAULTTONEAREST)
                                 : MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return {};
    const RECT &m = info.rcMonitor;
    *where = QRect(m.left, m.top, m.right - m.left, m.bottom - m.top);
    if (where->isEmpty())
        return {};
    return grabInto(where->width(), where->height(), [where](HDC mem, HDC screen) {
        return BitBlt(mem, 0, 0, where->width(), where->height(), screen, where->left(),
                      where->top(), SRCCOPY | CAPTUREBLT) != FALSE;
    });
}

// Shrunk to fit, with every control of the list outlined and its key drawn on
// it - SET OF MARKS - so the model can name what it SEES ("c7") instead of
// estimating pixels, and click it through UI Automation.
WindowShot finishShot(QImage image, const QRect &where, const QList<DesktopControl> &marks)
{
    WindowShot shot;
    if (image.isNull())
        return shot;

    // A full screen is several megabytes once base64 encoded, and the model
    // reads it perfectly well at a third of that.
    constexpr int kLongestSide = 1280;
    const int longest = qMax(image.width(), image.height());
    if (longest > kLongestSide) {
        shot.scale = qreal(kLongestSide) / qreal(longest);
        image = image.scaled(int(image.width() * shot.scale), int(image.height() * shot.scale),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    if (!marks.isEmpty()) {
        QPainter p(&image);
        QFont font;
        font.setPixelSize(11);
        font.setBold(true);
        p.setFont(font);
        const QFontMetrics metrics(font);
        const QColor ink(255, 0, 170);
        for (const DesktopControl &c : marks) {
            const QRect box(int((c.x - where.left()) * shot.scale),
                            int((c.y - where.top()) * shot.scale),
                            int(c.w * shot.scale), int(c.h * shot.scale));
            if (box.width() < 4 || box.height() < 4 || !image.rect().intersects(box))
                continue;
            p.setPen(QPen(ink, 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(box.adjusted(0, 0, -1, -1));
            QRect tag(box.left(), box.top() - metrics.height(),
                      metrics.horizontalAdvance(c.key) + 4, metrics.height());
            if (tag.top() < 0)
                tag.moveTop(box.top());
            p.fillRect(tag, ink);
            p.setPen(Qt::white);
            p.drawText(tag, Qt::AlignCenter, c.key);
        }
    }

    QBuffer buffer(&shot.png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    shot.width = image.width();
    shot.height = image.height();
    return shot;
}

// A point the model picked out of its last screenshot, back on the screen.
bool pictureToScreen(const DeskState &state, int px, int py, QPoint *out, QString *error)
{
    if (state.shotScreen) {
        *out = QPoint(state.shotLeft + int(px / state.shotScale),
                      state.shotTop + int(py / state.shotScale));
        return true;
    }
    const HWND hwnd = reinterpret_cast<HWND>(state.shotWindow);
    RECT r{};
    if (!hwnd || !IsWindow(hwnd) || !GetWindowRect(hwnd, &r)) {
        if (error)
            *error = QObject::tr("take a screenshot first, so there is a picture to "
                                 "pick a point out of.");
        return false;
    }
    *out = QPoint(r.left + int(px / state.shotScale), r.top + int(py / state.shotScale));
    return true;
}

// The window this task is working in, if it is still there, and its title as
// it reads now.
DesktopWindow stillThere(const DesktopWindow &w)
{
    const HWND hwnd = reinterpret_cast<HWND>(w.handle);
    if (!hwnd || !IsWindow(hwnd))
        return {};
    wchar_t title[512] = {};
    const int length = GetWindowTextW(hwnd, title, 511);
    DesktopWindow out;
    out.handle = w.handle;
    out.title = length > 0 ? QString::fromWCharArray(title, length) : w.title;
    return out;
}

// One computer_use action, by name.
QString carryOut(const QVariantMap &args, DeskState &state,
                 const std::atomic_bool &cancelled, QByteArray *png)
{
    const QString action = args.value(QStringLiteral("action")).toString();
    QString error;
    const QString button = args.value(QStringLiteral("button")).toString().toLower();
    const QString mouseButton = (button == QLatin1String("right") || button == QLatin1String("middle"))
                                    ? button
                                    : QStringLiteral("left");
    const int clicks = args.value(QStringLiteral("double")).toBool() ? 2 : 1;
    const QString wanted = args.value(QStringLiteral("window")).toString().trimmed();

    // The window an action means: the one named, else the one this task is
    // working in, else whatever is in front.
    const auto windowMeant = [&]() -> DesktopWindow {
        if (!wanted.isEmpty())
            return findWindow(wanted);
        const DesktopWindow own = stillThere(state.target);
        return own.isNull() ? foreground() : own;
    };
    const auto controlNamed = [&]() -> DesktopControl {
        const QString key = args.value(QStringLiteral("control")).toString().trimmed();
        if (key.isEmpty())
            return {};
        for (const DesktopControl &c : std::as_const(state.offered)) {
            if (c.key.compare(key, Qt::CaseInsensitive) == 0)
                return c;
        }
        return {};
    };
    // THE LIST OF CONTROLS IS FROM BEFORE THE LAST CHANGE. When the model names
    // a key that is no longer there, the NEW list goes back instead of a click
    // at whatever the old key pointed to - read from the task's OWN window, or
    // one task ends up holding another's controls.
    const auto listAgain = [&]() -> QString {
        const DesktopWindow own = stillThere(state.target);
        return QObject::tr("Failed: that control is not on screen any more - the "
                           "screen has changed since the list you were given. %1")
            .arg(describeControls(own.isNull() ? foreground() : own, state));
    };
    const auto pointOf = [&](const char *xName, const char *yName, QPoint *out) {
        return pictureToScreen(state, args.value(QLatin1String(xName)).toInt(),
                               args.value(QLatin1String(yName)).toInt(), out, &error);
    };
    const auto howClicked = [&]() -> QString {
        if (clicks > 1)
            return QObject::tr("Double clicked");
        if (mouseButton == QLatin1String("right"))
            return QObject::tr("Right clicked");
        if (mouseButton == QLatin1String("middle"))
            return QObject::tr("Middle clicked");
        return QObject::tr("Clicked");
    };

    if (action == QLatin1String("screenshot")) {
        const bool whole = args.value(QStringLiteral("screen")).toBool();
        QRect where;
        QImage image;
        DesktopWindow target;
        QString list;
        if (whole) {
            image = grabScreenImage(reinterpret_cast<HWND>(windowMeant().handle), &where);
        } else {
            target = windowMeant();
            if (target.isNull())
                return QObject::tr("Failed: I could not find that window.");
            image = grabWindowImage(reinterpret_cast<HWND>(target.handle), &where);
        }
        if (image.isNull())
            return QObject::tr("Failed: that would not be captured.");
        // The controls of what is pictured, read again so their keys can be
        // drawn on it and clicked by name.
        if (!whole)
            list = describeControls(target, state);
        const WindowShot shot = finishShot(image, where, state.offered);
        state.shotWindow = whole ? 0 : target.handle;
        state.shotScreen = whole;
        state.shotLeft = where.left();
        state.shotTop = where.top();
        state.shotScale = shot.scale;
        if (png)
            *png = shot.png;
        if (whole) {
            return QObject::tr("Screenshot of the whole screen your window is on, %1 "
                               "by %2 pixels. The controls of the window you are "
                               "working in are outlined with their keys; click any "
                               "other point with click_at, in this picture's pixels.")
                .arg(shot.width)
                .arg(shot.height);
        }
        return QObject::tr("Screenshot of '%1', %2 by %3 pixels. Each control is "
                           "outlined in the picture with its key: click one with "
                           "click and that key, or any other point with click_at "
                           "in this picture's pixels. %4")
            .arg(target.title)
            .arg(shot.width)
            .arg(shot.height)
            .arg(list);
    }

    if (action == QLatin1String("click_at")) {
        QPoint point;
        if (!pointOf("x", "y", &point))
            return QObject::tr("Failed: %1").arg(error);
        if (state.shotWindow)
            bringToFront(reinterpret_cast<HWND>(state.shotWindow));
        clickScreen(point.x(), point.y(), mouseButton, clicks);
        return QObject::tr("%1 at %2, %3 in that screenshot.")
            .arg(howClicked())
            .arg(args.value(QStringLiteral("x")).toInt())
            .arg(args.value(QStringLiteral("y")).toInt());
    }

    // A named control, but not a plain left click: UI Automation has no
    // pattern for a right or double click, so it is the real mouse, aimed at
    // the middle of the control.
    if (action == QLatin1String("click") && (mouseButton != QLatin1String("left") || clicks > 1)) {
        const DesktopControl c = controlNamed();
        if (!c.key.isEmpty() && c.w > 0 && c.h > 0) {
            bringToFront(reinterpret_cast<HWND>(state.target.handle));
            clickScreen(c.x + c.w / 2, c.y + c.h / 2, mouseButton, clicks);
            return QObject::tr("%1 %2.").arg(howClicked(), c.name.isEmpty() ? c.type : c.name);
        }
    }

    // Typing with no control named goes wherever the window's cursor is: an
    // editor, a terminal, a field that was just clicked.
    if (action == QLatin1String("type_text")
        && args.value(QStringLiteral("control")).toString().trimmed().isEmpty()) {
        const QString text = args.value(QStringLiteral("text")).toString();
        if (text.isEmpty())
            return QObject::tr("Failed: no text was given.");
        const DesktopWindow into = windowMeant();
        if (!into.isNull()) {
            if (!bringToFront(reinterpret_cast<HWND>(into.handle)))
                liveLog(QStringLiteral("computer use: that window would not come to the front"));
            state.target = into;
        }
        typeHere(text);
        return QObject::tr("Typed %1 characters into '%2', wherever its cursor was. "
                           "Take a screenshot to check it landed where you meant.")
            .arg(text.size())
            .arg(into.title);
    }

    if (action == QLatin1String("press_keys") || action == QLatin1String("press_key")) {
        const QString keys = (args.value(QStringLiteral("keys")).toString() + QLatin1Char(' ')
                              + args.value(QStringLiteral("key")).toString())
                                 .trimmed()
                                 .toLower();
        if (keys.isEmpty())
            return QObject::tr("Failed: no keys were given.");
        // Into the task's own window, so keys meant for it never land in
        // whatever another task left in front.
        const DesktopWindow into = windowMeant();
        if (!into.isNull()) {
            bringToFront(reinterpret_cast<HWND>(into.handle));
            state.target = into;
        }
        if (!sendKeys(keys, &error))
            return QObject::tr("Failed: %1").arg(error);
        return QObject::tr("Pressed %1.").arg(keys);
    }

    if (action == QLatin1String("drag")) {
        QPoint from;
        QPoint to;
        if (!pointOf("x", "y", &from) || !pointOf("to_x", "to_y", &to))
            return QObject::tr("Failed: %1").arg(error);
        if (state.shotWindow)
            bringToFront(reinterpret_cast<HWND>(state.shotWindow));
        dragScreen(from, to);
        return QObject::tr("Dragged from %1, %2 to %3, %4 in that screenshot.")
            .arg(args.value(QStringLiteral("x")).toInt())
            .arg(args.value(QStringLiteral("y")).toInt())
            .arg(args.value(QStringLiteral("to_x")).toInt())
            .arg(args.value(QStringLiteral("to_y")).toInt());
    }

    if (action == QLatin1String("scroll")) {
        const QString direction = args.value(QStringLiteral("direction")).toString().toLower();
        const bool horizontal = direction == QLatin1String("left") || direction == QLatin1String("right");
        if (!horizontal && direction != QLatin1String("up") && direction != QLatin1String("down"))
            return QObject::tr("Failed: scroll needs a direction - up, down, left or right.");
        const int amount = qBound(1, args.value(QStringLiteral("amount"), 5).toInt(), 50);
        QPoint at;
        const DesktopControl c = controlNamed();
        if (args.contains(QStringLiteral("x")) && args.contains(QStringLiteral("y"))) {
            if (!pointOf("x", "y", &at))
                return QObject::tr("Failed: %1").arg(error);
        } else if (!c.key.isEmpty() && c.w > 0 && c.h > 0) {
            at = QPoint(c.x + c.w / 2, c.y + c.h / 2);
        } else {
            const DesktopWindow w = windowMeant();
            RECT r{};
            if (w.isNull() || !GetWindowRect(reinterpret_cast<HWND>(w.handle), &r))
                return QObject::tr("Failed: there is no window to scroll.");
            at = QPoint((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        }
        const bool back = direction == QLatin1String("down") || direction == QLatin1String("left");
        wheelAt(at.x(), at.y(), back ? -amount : amount, horizontal);
        return QObject::tr("Scrolled %1 by %2.").arg(direction).arg(amount);
    }

    if (action == QLatin1String("move_mouse")) {
        QPoint at;
        const DesktopControl c = controlNamed();
        if (!c.key.isEmpty() && c.w > 0 && c.h > 0)
            at = QPoint(c.x + c.w / 2, c.y + c.h / 2);
        else if (!pointOf("x", "y", &at))
            return QObject::tr("Failed: %1").arg(error);
        SetCursorPos(at.x(), at.y());
        return QObject::tr("The pointer is resting there now. Take a screenshot with "
                           "screen true to see a tooltip or menu it opened.");
    }

    if (action == QLatin1String("read_text")) {
        const DesktopControl c = controlNamed();
        const DesktopWindow from = c.key.isEmpty() ? windowMeant() : stillThere(state.target);
        if (from.isNull())
            return QObject::tr("Failed: I could not find that window.");
        const QString text = readText(reinterpret_cast<HWND>(from.handle),
                                      c.key.isEmpty() ? nullptr : &c, 8000);
        if (text.trimmed().isEmpty()) {
            return QObject::tr("'%1' gave no text to read. Take a screenshot and read "
                               "it with your eyes instead.")
                .arg(from.title);
        }
        return QObject::tr("The text in '%1':\n%2").arg(from.title, text);
    }

    if (action == QLatin1String("window")) {
        const QString how = args.value(QStringLiteral("how")).toString().toLower();
        const DesktopWindow w = windowMeant();
        if (w.isNull())
            return QObject::tr("Failed: I could not find that window.");
        const HWND h = reinterpret_cast<HWND>(w.handle);
        if (how == QLatin1String("focus")) {
            if (!bringToFront(h))
                return QObject::tr("Failed: '%1' would not come to the front.").arg(w.title);
        } else if (how == QLatin1String("maximize")) {
            ShowWindow(h, SW_MAXIMIZE);
        } else if (how == QLatin1String("minimize")) {
            ShowWindow(h, SW_MINIMIZE);
        } else if (how == QLatin1String("restore")) {
            ShowWindow(h, SW_RESTORE);
        } else if (how == QLatin1String("close")) {
            PostMessageW(h, WM_CLOSE, 0, 0);
        } else {
            return QObject::tr("Failed: window needs how - focus, maximize, minimize, "
                               "restore or close.");
        }
        if (!pauseFor(400, cancelled))
            return QObject::tr("Stopped.");
        if (how == QLatin1String("close") || how == QLatin1String("minimize")) {
            if (state.target.handle == w.handle)
                state.target = DesktopWindow();
            return QObject::tr("Done: %1 '%2'. If it asked whether to save, that dialog "
                               "is in front now.")
                .arg(how, w.title);
        }
        state.target = w;
        return QObject::tr("Done: %1 '%2'.").arg(how, w.title);
    }

    if (action == QLatin1String("wait_for")) {
        const QString text = args.value(QStringLiteral("text")).toString().trimmed();
        const int seconds = qBound(1, args.value(QStringLiteral("seconds"), 10).toInt(), 60);
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < qint64(seconds) * 1000) {
            const DesktopWindow w = wanted.isEmpty() ? DesktopWindow() : findWindow(wanted);
            const bool windowOk = wanted.isEmpty() || !w.isNull();
            if (windowOk && text.isEmpty() && !wanted.isEmpty()) {
                state.target = w;
                return QObject::tr("'%1' is up, after %2 s.")
                    .arg(w.title)
                    .arg(clock.elapsed() / 1000);
            }
            if (windowOk && !text.isEmpty()) {
                const DesktopWindow where = w.isNull() ? windowMeant() : w;
                if (!where.isNull()
                    && readText(reinterpret_cast<HWND>(where.handle), nullptr, 20000)
                           .contains(text, Qt::CaseInsensitive)) {
                    return QObject::tr("'%1' appeared in '%2' after %3 s.")
                        .arg(text, where.title)
                        .arg(clock.elapsed() / 1000);
                }
            }
            if (!pauseFor(500, cancelled))
                return QObject::tr("Stopped.");
        }
        if (wanted.isEmpty() && text.isEmpty())
            return QObject::tr("Waited %1 seconds.").arg(seconds);
        return QObject::tr("Still not there after %1 seconds.").arg(seconds);
    }

    if (action == QLatin1String("clipboard")) {
        if (args.contains(QStringLiteral("text"))) {
            if (!setClipboardText(args.value(QStringLiteral("text")).toString()))
                return QObject::tr("Failed: the clipboard would not open.");
            return QObject::tr("The clipboard holds that text now - paste it with ctrl+v.");
        }
        QString text;
        if (!clipboardText(&text))
            return QObject::tr("Failed: the clipboard would not open.");
        if (text.isEmpty())
            return QObject::tr("The clipboard holds no text.");
        if (text.size() > 8000)
            text = text.left(8000) + QStringLiteral(" ... (there is more)");
        return QObject::tr("The clipboard holds:\n%1").arg(text);
    }

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

        // NO NEW WINDOW can mean it was already open: Settings, and many
        // single-window apps, bring back the window they already had. Look for
        // it by name before sending the model off to hunt for it.
        if (fresh.isNull()) {
            for (const DesktopWindow &w : windows()) {
                if (w.title.contains(opened, Qt::CaseInsensitive)
                    || w.title.contains(name, Qt::CaseInsensitive)) {
                    fresh = w;
                    bringToFront(reinterpret_cast<HWND>(w.handle));
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

        // Every window's title beforehand. Windows often will not let the
        // browser come to the front for a page handed to it, so the window in
        // front afterwards is NOT the page - the task was once given Notepad's
        // controls for a GitHub page. The page's window is the one whose title
        // changed to it.
        QHash<quint64, QString> titles;
        for (const DesktopWindow &w : windows())
            titles.insert(w.handle, w.title);

        QString opened;
        if (!openUrl(url, &opened, &error))
            return QObject::tr("Failed: %1").arg(error);

        // A page takes a moment to come up and the next move is to look at
        // it, so the controls handed back have to be the NEW ones.
        if (!pauseFor(2600, cancelled))
            return QObject::tr("Stopped.");

        // The words a page's title is likely to carry: the site and the path.
        const QUrl parsed = QUrl::fromUserInput(url);
        QStringList words = parsed.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QString site = parsed.host();
        if (site.startsWith(QStringLiteral("www.")))
            site = site.mid(4);
        words.append(site.section(QLatin1Char('.'), 0, 0));

        DesktopWindow front;
        int best = 0;
        for (const DesktopWindow &w : windows()) {
            const bool changed = !titles.contains(w.handle) || titles.value(w.handle) != w.title;
            wchar_t cls[128] = {};
            GetClassNameW(reinterpret_cast<HWND>(w.handle), cls, 127);
            const bool browser = wcscmp(cls, L"Chrome_WidgetWin_1") == 0
                                 || wcscmp(cls, L"MozillaWindowClass") == 0;
            int score = (changed ? 2 : 0) + (browser ? 1 : 0);
            for (const QString &word : std::as_const(words)) {
                if (word.size() >= 4 && w.title.contains(word, Qt::CaseInsensitive)) {
                    score += 3;
                    break;
                }
            }
            if (browser && score > best && score >= 3) {
                best = score;
                front = w;
            }
        }
        if (front.isNull())
            front = foreground();
        else
            bringToFront(reinterpret_cast<HWND>(front.handle));
        if (front.isNull())
            return QObject::tr("Opened %1 in the user's own browser.").arg(opened);
        return QObject::tr("Opened %1 in the browser the user already had open, so "
                           "they are signed in there as themselves. %2")
            .arg(opened, describeControls(front, state));
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

    if (action == QLatin1String("list_controls")) {
        const QString wanted = args.value(QStringLiteral("window")).toString().trimmed();
        const DesktopWindow target = wanted.isEmpty() ? foreground() : findWindow(wanted);
        if (target.isNull())
            return QObject::tr("Failed: I could not find that window.");
        return describeControls(target, state);
    }

    if (action == QLatin1String("click")) {
        const DesktopControl target = controlNamed();
        if (target.key.isEmpty())
            return listAgain();
        if (click(state.target.handle, target, &error))
            return QObject::tr("Clicked %1.").arg(target.name.isEmpty() ? target.type : target.name);
        return QObject::tr("Failed: %1").arg(error);
    }

    if (action == QLatin1String("type_text")) {
        const QString text = args.value(QStringLiteral("text")).toString();
        const DesktopControl target = controlNamed();
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

// A click or a key that opened a new window - a Save dialog, a confirmation,
// a settings page - is what the next step is about. The task was left looking
// at the window underneath, cancelled the Save dialog it could not see and
// typed the file name into the document instead. Now the new window is named
// at once, becomes the one the task works in, and its controls come back.
QString newWindowNote(const QSet<quint64> &before, DeskState &state,
                      const std::atomic_bool &cancelled)
{
    for (int waited = 0; waited < 800; waited += 100) {
        for (const DesktopWindow &w : windows()) {
            if (!before.contains(w.handle)) {
                pauseFor(400, cancelled); // let it finish drawing
                return QObject::tr(" A new window came up and you are working in it "
                                   "now: %1").arg(describeControls(w, state));
            }
        }
        if (!pauseFor(100, cancelled))
            return {};
    }
    return {};
}

// EVERY computer_use ACTION.
QString act(const QVariantMap &args, DeskState &state,
            const std::atomic_bool &cancelled, QByteArray *png)
{
    const QString action = args.value(QStringLiteral("action")).toString();
    const bool mayOpen = action == QLatin1String("click") || action == QLatin1String("click_at")
                         || action == QLatin1String("press_keys")
                         || action == QLatin1String("press_key");
    QSet<quint64> before;
    if (mayOpen) {
        for (const DesktopWindow &w : windows())
            before.insert(w.handle);
    }
    QString result = carryOut(args, state, cancelled, png);
    if (mayOpen && !result.startsWith(QStringLiteral("Failed"))
        && !result.startsWith(QStringLiteral("Stopped")))
        result += newWindowNote(before, state, cancelled);
    return result;
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
    liveLog(QStringLiteral("computer use: %1 -> %2")
                .arg(args.value(QStringLiteral("action")).toString(),
                     out.text.simplified().left(160)));
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

// ---------------------------------------------------------------------------
// CODING: files, and commands that take as long as they take.
// ---------------------------------------------------------------------------

namespace {

// Folders nobody means when they search a project, and which are enormous.
bool skippedFolder(const QString &name)
{
    return name == QLatin1String(".git") || name == QLatin1String("node_modules")
           || name == QLatin1String(".venv") || name == QLatin1String("__pycache__");
}

// Every file under root whose name matches, nearest folders first.
QStringList walkFiles(const QString &root, const QStringList &nameFilters, int cap)
{
    QStringList out;
    QStringList pending{root};
    while (!pending.isEmpty() && out.size() < cap) {
        const QDir dir(pending.takeFirst());
        const QFileInfoList here = dir.entryInfoList(
            nameFilters, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDir::Name);
        for (const QFileInfo &f : here) {
            out.append(f.absoluteFilePath());
            if (out.size() >= cap)
                break;
        }
        const QFileInfoList below = dir.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks, QDir::Name);
        for (const QFileInfo &d : below) {
            // A folder whose name matches is an answer too - "where is the
            // burnos-scratch folder" is a find.
            if (out.size() < cap && QDir::match(nameFilters, d.fileName()))
                out.append(d.absoluteFilePath() + QLatin1Char('/'));
            if (!skippedFolder(d.fileName()))
                pending.append(d.absoluteFilePath());
        }
    }
    return out;
}

QString native(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

bool readTextFile(const QString &path, QString *text, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QObject::tr("cannot open %1").arg(native(path));
        return false;
    }
    if (f.size() > 8 * 1024 * 1024) {
        *error = QObject::tr("%1 is over 8 MB, too big to read whole").arg(native(path));
        return false;
    }
    const QByteArray raw = f.readAll();
    if (raw.left(8192).contains('\0')) {
        *error = QObject::tr("%1 is not a text file").arg(native(path));
        return false;
    }
    *text = QString::fromUtf8(raw);
    return true;
}

// Written whole or not at all, so an interrupted write never leaves half a
// source file behind.
bool writeTextFile(const QString &path, const QString &text, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = QObject::tr("cannot make the folder for %1").arg(native(path));
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(text.toUtf8()) < 0 || !f.commit()) {
        *error = QObject::tr("cannot write %1").arg(native(path));
        return false;
    }
    return true;
}

} // namespace

QString files(const QVariantMap &args)
{
    const QString action = args.value(QStringLiteral("action")).toString();
    const QString given = QDir::fromNativeSeparators(
        args.value(QStringLiteral("path")).toString().trimmed());
    if (given.isEmpty())
        return QObject::tr("Failed: files needs a path.");
    // Taken as given; a path that is not absolute is under the user's home.
    const QString path = QDir::cleanPath(QDir::isAbsolutePath(given)
                                             ? given
                                             : QDir::home().filePath(given));
    QString error;

    if (action == QLatin1String("read")) {
        QString text;
        if (!readTextFile(path, &text, &error))
            return QObject::tr("Failed: %1").arg(error);
        const QStringList lines = text.split(QLatin1Char('\n'));
        const int total = int(lines.size());
        const int first = qMax(1, args.value(QStringLiteral("offset"), 1).toInt());
        const int count = qBound(1, args.value(QStringLiteral("limit"), 400).toInt(), 2000);
        const int last = qMin(total, first + count - 1);
        QString out = QObject::tr("%1, lines %2 to %3 of %4:\n")
                          .arg(native(path)).arg(first).arg(last).arg(total);
        for (int i = first; i <= last; ++i) {
            QString line = lines.at(i - 1);
            if (line.endsWith(QLatin1Char('\r')))
                line.chop(1);
            out += QStringLiteral("%1| %2\n").arg(i, 5).arg(line);
            if (out.size() > 60000) {
                out += QObject::tr("... (cut short: read again from line %1)\n").arg(i + 1);
                break;
            }
        }
        return out;
    }

    if (action == QLatin1String("write")) {
        const QString content = args.value(QStringLiteral("content")).toString();
        const bool existed = QFileInfo::exists(path);
        if (!writeTextFile(path, content, &error))
            return QObject::tr("Failed: %1").arg(error);
        liveLog(QStringLiteral("files: wrote %1").arg(native(path)));
        return QObject::tr("%1 %2, %3 lines.")
            .arg(existed ? QObject::tr("Overwrote") : QObject::tr("Created"), native(path))
            .arg(content.count(QLatin1Char('\n')) + 1);
    }

    if (action == QLatin1String("edit")) {
        QString oldText = args.value(QStringLiteral("old")).toString();
        QString newText = args.value(QStringLiteral("new")).toString();
        const bool all = args.value(QStringLiteral("replace_all")).toBool();
        if (oldText.isEmpty())
            return QObject::tr("Failed: edit needs old, the exact text to replace.");
        QString text;
        if (!readTextFile(path, &text, &error))
            return QObject::tr("Failed: %1").arg(error);
        qsizetype found = text.count(oldText);
        // Windows line endings in the file, plain ones in what was sent.
        if (found == 0 && text.contains(QStringLiteral("\r\n"))
            && !oldText.contains(QStringLiteral("\r\n"))) {
            const QString crlf = QString(oldText).replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
            if (text.count(crlf) > 0) {
                oldText = crlf;
                newText.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
                found = text.count(oldText);
            }
        }
        if (found == 0) {
            return QObject::tr("Failed: that text is not in %1. Read the file again and "
                               "copy the lines exactly.").arg(native(path));
        }
        if (found > 1 && !all) {
            return QObject::tr("Failed: that text is in %1 %2 times. Give more of the "
                               "lines around it so it is unique, or set replace_all.")
                .arg(native(path)).arg(found);
        }
        if (all)
            text.replace(oldText, newText);
        else
            text.replace(text.indexOf(oldText), oldText.size(), newText);
        if (!writeTextFile(path, text, &error))
            return QObject::tr("Failed: %1").arg(error);
        liveLog(QStringLiteral("files: edited %1").arg(native(path)));
        return QObject::tr("Edited %1: replaced %2 place(s).")
            .arg(native(path)).arg(all ? found : 1);
    }

    if (action == QLatin1String("list")) {
        const QDir dir(path);
        if (!dir.exists())
            return QObject::tr("Failed: %1 is not a folder.").arg(native(path));
        const QFileInfoList entries = dir.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
            QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
        QStringList lines;
        for (const QFileInfo &e : entries) {
            lines.append(e.isDir() ? e.fileName() + QLatin1Char('/')
                                   : QStringLiteral("%1  (%2 bytes)").arg(e.fileName()).arg(e.size()));
            if (lines.size() >= 300) {
                lines.append(QStringLiteral("... (more)"));
                break;
            }
        }
        return QObject::tr("%1 holds %2 item(s):\n%3")
            .arg(native(path)).arg(entries.size()).arg(lines.join(QLatin1Char('\n')));
    }

    if (action == QLatin1String("find")) {
        QString pattern = args.value(QStringLiteral("pattern")).toString().trimmed();
        if (pattern.isEmpty())
            pattern = args.value(QStringLiteral("glob")).toString().trimmed();
        // Only the name part counts: "**/*.cpp" and "*.cpp" both mean every
        // .cpp file anywhere under the path.
        pattern = pattern.section(QLatin1Char('/'), -1).section(QLatin1Char('\\'), -1);
        if (pattern.isEmpty())
            pattern = QStringLiteral("*");
        QStringList hits = walkFiles(path, {pattern}, 301);
        const bool more = hits.size() > 300;
        if (more)
            hits = hits.mid(0, 300);
        for (QString &hit : hits)
            hit = native(hit);
        if (hits.isEmpty())
            return QObject::tr("No file matching %1 under %2.").arg(pattern, native(path));
        return QObject::tr("%1 file(s) matching %2 under %3%4:\n%5")
            .arg(hits.size()).arg(pattern, native(path))
            .arg(more ? QObject::tr(" (the first 300)") : QString())
            .arg(hits.join(QLatin1Char('\n')));
    }

    if (action == QLatin1String("search")) {
        const QString expression = args.value(QStringLiteral("pattern")).toString();
        if (expression.isEmpty())
            return QObject::tr("Failed: search needs pattern, a regular expression.");
        const QRegularExpression re(expression,
                                    args.value(QStringLiteral("ignore_case")).toBool()
                                        ? QRegularExpression::CaseInsensitiveOption
                                        : QRegularExpression::NoPatternOption);
        if (!re.isValid())
            return QObject::tr("Failed: that is not a valid regular expression: %1").arg(re.errorString());
        QString filter = args.value(QStringLiteral("glob")).toString().trimmed();
        filter = filter.section(QLatin1Char('/'), -1).section(QLatin1Char('\\'), -1);
        if (filter.isEmpty())
            filter = QStringLiteral("*");
        const QStringList candidates = QFileInfo(path).isFile() ? QStringList{path}
                                                                : walkFiles(path, {filter}, 20000);
        QStringList hits;
        int searched = 0;
        for (const QString &file : candidates) {
            if (QFileInfo(file).size() > 2 * 1024 * 1024)
                continue;
            QString text;
            QString skip;
            if (!readTextFile(file, &text, &skip))
                continue;
            ++searched;
            const QStringList lines = text.split(QLatin1Char('\n'));
            for (int i = 0; i < lines.size() && hits.size() < 200; ++i) {
                if (!re.match(lines.at(i)).hasMatch())
                    continue;
                QString line = lines.at(i).trimmed();
                if (line.size() > 200)
                    line = line.left(200) + QStringLiteral("...");
                hits.append(QStringLiteral("%1:%2: %3").arg(native(file)).arg(i + 1).arg(line));
            }
            if (hits.size() >= 200)
                break;
        }
        if (hits.isEmpty()) {
            return QObject::tr("Nothing matches %1 in the %2 file(s) searched under %3.")
                .arg(expression).arg(searched).arg(native(path));
        }
        return QObject::tr("%1 match(es)%2:\n%3")
            .arg(hits.size())
            .arg(hits.size() >= 200 ? QObject::tr(" (the first 200)") : QString())
            .arg(hits.join(QLatin1Char('\n')));
    }

    return QObject::tr("Failed: '%1' is not something files does. Use read, write, "
                       "edit, list, find or search.").arg(action);
}

void runShell(QObject *guard, const QVariantMap &args,
              std::shared_ptr<std::atomic_bool> cancelled,
              std::function<void(const QString &)> done)
{
    const QPointer<QObject> alive(guard);
    const auto finish = [alive, done](const QString &result) {
        if (alive)
            done(result);
    };

    const QString command = args.value(QStringLiteral("command")).toString().trimmed();
    QString cwd = args.value(QStringLiteral("cwd")).toString().trimmed();
    const int timeoutS = qBound(5, args.value(QStringLiteral("timeout"), 120).toInt(), 900);
    if (command.isEmpty()) {
        finish(QObject::tr("Failed: no command was given."));
        return;
    }
    if (cwd.isEmpty()) {
        cwd = QDir::homePath();
    } else if (!QFileInfo(cwd).isDir()) {
        finish(QObject::tr("Failed: %1 is not a folder.").arg(cwd));
        return;
    }

    liveLog(QStringLiteral("shell: %1 (in %2, up to %3 s)")
                .arg(command.left(200), native(cwd))
                .arg(timeoutS));

    // NATIVE ARGUMENTS, and /s with one outer pair of quotes: QProcess quotes
    // each argument itself and cmd applies its own rules on top, and between
    // them a command like powershell -c "..." came back merely echoed.
    const QString nativeArgs = QStringLiteral("/s /c \"%1\"").arg(command);

    // Meant to keep running - a server, a program - so started and let go.
    if (command.startsWith(QStringLiteral("start "), Qt::CaseInsensitive)
        || args.value(QStringLiteral("background")).toBool()) {
        QProcess launcher;
        launcher.setProgram(QStringLiteral("cmd.exe"));
        launcher.setNativeArguments(nativeArgs);
        launcher.setWorkingDirectory(cwd);
        qint64 pid = 0;
        if (!launcher.startDetached(&pid))
            finish(QObject::tr("Failed: cmd would not start."));
        else
            finish(QObject::tr("Started in the background as process %1; it keeps "
                               "running on its own.").arg(pid));
        return;
    }

    auto *proc = new QProcess;
    proc->setProgram(QStringLiteral("cmd.exe"));
    proc->setNativeArguments(nativeArgs);
    proc->setWorkingDirectory(cwd);
    proc->setProcessChannelMode(QProcess::MergedChannels);

    struct Run
    {
        QByteArray output;
        bool over = false;
        QElapsedTimer clock;
    };
    const auto run = std::make_shared<Run>();
    run->clock.start();

    // The end of what it printed, which is where the answer - or the error -
    // usually is.
    const auto tail = [run]() {
        QString text = QString::fromLocal8Bit(run->output).trimmed();
        if (text.size() > 12000)
            text = QStringLiteral("... ") + text.right(12000);
        return text.isEmpty() ? QObject::tr("It printed nothing.") : text;
    };
    // The whole tree, not just cmd: a build's compilers are its children.
    const auto killTree = [proc]() {
        const qint64 pid = proc->processId();
        if (pid > 0) {
            QProcess::startDetached(QStringLiteral("taskkill"),
                                    {QStringLiteral("/PID"), QString::number(pid),
                                     QStringLiteral("/T"), QStringLiteral("/F")});
        }
        proc->kill();
    };

    QObject::connect(proc, &QProcess::readyRead, proc, [proc, run]() {
        run->output.append(proc->readAll());
        if (run->output.size() > 400000)
            run->output.remove(0, run->output.size() - 200000);
    });
    QObject::connect(proc, &QProcess::finished, proc,
                     [proc, run, tail, finish](int code, QProcess::ExitStatus) {
        if (!run->over) {
            run->over = true;
            run->output.append(proc->readAll());
            liveLog(QStringLiteral("shell: exit %1 after %2 s")
                        .arg(code)
                        .arg(run->clock.elapsed() / 1000));
            finish(QObject::tr("Exit code %1 after %2 s. %3")
                       .arg(code)
                       .arg(run->clock.elapsed() / 1000)
                       .arg(tail()));
        }
        proc->deleteLater();
    });
    QObject::connect(proc, &QProcess::errorOccurred, proc,
                     [proc, run, finish](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || run->over)
            return;
        run->over = true;
        finish(QObject::tr("Failed: cmd would not start."));
        proc->deleteLater();
    });

    // A stopped task stops its command; one that runs past its time is ended.
    auto *watch = new QTimer(proc);
    watch->setInterval(500);
    QObject::connect(watch, &QTimer::timeout, proc,
                     [proc, run, tail, finish, killTree, cancelled, timeoutS]() {
        if (run->over)
            return;
        const bool stopped = cancelled && cancelled->load();
        const bool late = run->clock.elapsed() >= qint64(timeoutS) * 1000;
        if (!stopped && !late)
            return;
        run->over = true;
        run->output.append(proc->readAll());
        liveLog(QStringLiteral("shell: %1, ending the process tree")
                    .arg(stopped ? QStringLiteral("task stopped")
                                 : QStringLiteral("out of time")));
        killTree();
        finish(stopped ? QObject::tr("Stopped.")
                       : QObject::tr("Stopped after %1 s - it was still running. What "
                                     "it printed: %2").arg(timeoutS).arg(tail()));
    });
    watch->start();
    proc->start();
}

} // namespace Computer
} // namespace BurnOS
