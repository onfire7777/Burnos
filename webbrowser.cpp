#include "webbrowser.h"

#include "livelog.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QtEndian>

#include <string>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace BurnOS {

namespace {

// How long the browser has to open its debugging port.
constexpr int kStartMs = 15000;
// How long a page has to finish loading before it is read as it stands.
constexpr int kLoadMs = 20000;
constexpr int kSettleMs = 500;
// One action of the tool, start to finish.
constexpr int kCallMs = 75000;
// Closed after this long with nothing to do.
constexpr int kIdleMs = 5 * 60 * 1000;
// How much of a page one answer carries: a task's tool answers are cut at
// twelve thousand characters, and this leaves room for what goes round it.
constexpr int kPartChars = 9000;
constexpr quint64 kMaxMessageBytes = 64ull * 1024 * 1024;

struct Found
{
    QString path;
    QString name;
    bool edge = false;
};

// Where Windows says a program is, from its App Paths.
QString appPath(const QString &exe)
{
    for (const char *hive : {"HKEY_LOCAL_MACHINE", "HKEY_CURRENT_USER"}) {
        const QSettings key(QStringLiteral("%1\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%2")
                                .arg(QLatin1String(hive), exe),
                            QSettings::NativeFormat);
        QString path = key.value(QStringLiteral(".")).toString().trimmed();
        path.remove(QLatin1Char('"'));
        if (!path.isEmpty() && QFileInfo::exists(path))
            return QDir::fromNativeSeparators(path);
    }
    return QString();
}

// The browser to run hidden: Edge first, because every Windows has it, then
// Chrome, then Brave. BURNOS_BROWSER names another, for a test.
Found findBrowser()
{
    static Found found;
    if (!found.path.isEmpty())
        return found;

    const QString forced = QDir::fromNativeSeparators(qEnvironmentVariable("BURNOS_BROWSER"));
    if (!forced.isEmpty() && QFileInfo::exists(forced)) {
        found = {forced, QFileInfo(forced).completeBaseName(),
                 forced.contains(QLatin1String("msedge"), Qt::CaseInsensitive)};
        return found;
    }
    const QString pf86 = QDir::fromNativeSeparators(qEnvironmentVariable("ProgramFiles(x86)"));
    const QString pf = QDir::fromNativeSeparators(qEnvironmentVariable("ProgramFiles"));
    const QString local = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"));
    const QString edge = QStringLiteral("Microsoft Edge");
    const QString chrome = QStringLiteral("Google Chrome");
    const QString brave = QStringLiteral("Brave");
    const QList<Found> candidates{
        {appPath(QStringLiteral("msedge.exe")), edge, true},
        {pf86 + QStringLiteral("/Microsoft/Edge/Application/msedge.exe"), edge, true},
        {pf + QStringLiteral("/Microsoft/Edge/Application/msedge.exe"), edge, true},
        {appPath(QStringLiteral("chrome.exe")), chrome, false},
        {pf + QStringLiteral("/Google/Chrome/Application/chrome.exe"), chrome, false},
        {pf86 + QStringLiteral("/Google/Chrome/Application/chrome.exe"), chrome, false},
        {local + QStringLiteral("/Google/Chrome/Application/chrome.exe"), chrome, false},
        {pf + QStringLiteral("/BraveSoftware/Brave-Browser/Application/brave.exe"), brave, false},
        {local + QStringLiteral("/BraveSoftware/Brave-Browser/Application/brave.exe"), brave, false},
    };
    for (const Found &candidate : candidates) {
        if (candidate.path.size() > 12 && QFileInfo::exists(candidate.path)) {
            found = candidate;
            return found;
        }
    }
    return Found();
}

// The first number of the program's version - 155 - or 0.
int majorVersion(const QString &path)
{
#ifdef Q_OS_WIN
    const std::wstring file = QDir::toNativeSeparators(path).toStdWString();
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (!size)
        return 0;
    QByteArray data(int(size), '\0');
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data()))
        return 0;
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.constData(), L"\\", reinterpret_cast<LPVOID *>(&info), &length)
        || !info)
        return 0;
    return int(HIWORD(info->dwFileVersionMS));
#else
    Q_UNUSED(path);
    return 0;
#endif
}

// The profile folder the hidden browser keeps to itself. A test copy on its
// own settings tree (BURNOS_SETTINGS) has its own, so that two copies of
// Burnos never hand one browser to each other.
QString profileDir()
{
    QString dir = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"))
                  + QStringLiteral("/BurnOS/browser");
    const QString tree = qEnvironmentVariable("BURNOS_SETTINGS");
    if (!tree.isEmpty())
        dir += QLatin1Char('-') + tree;
    return dir;
}

// Text as a JavaScript string, quotes and all.
QString js(const QString &text)
{
    const QByteArray json = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

QString number(qint64 n)
{
    return QString::number(n);
}

// Why an address would not open, in words.
QString whyNot(const QString &url, const QString &error)
{
    QString why = error;
    if (error.contains(QLatin1String("NAME_NOT_RESOLVED")))
        why = QStringLiteral("there is no site at that address");
    else if (error.contains(QLatin1String("INTERNET_DISCONNECTED")))
        why = QStringLiteral("this PC is not connected to the internet");
    else if (error.contains(QLatin1String("TIMED_OUT")))
        why = QStringLiteral("the site did not answer");
    else if (error.contains(QLatin1String("CONNECTION_")))
        why = QStringLiteral("the site refused or dropped the connection (%1)").arg(error);
    else if (error.contains(QLatin1String("ERR_ABORTED")))
        why = QStringLiteral("it is a file to download, not a page, and the hidden browser "
                             "downloads nothing");
    else if (error.contains(QLatin1String("CERT_")) || error.contains(QLatin1String("SSL_")))
        why = QStringLiteral("its security certificate is not valid (%1)").arg(error);
    return QStringLiteral("Failed: could not open ") + url + QStringLiteral(" - ") + why + QLatin1Char('.');
}

// A page that is a wall and not the page: "prove you are human".
bool turnedAway(const QString &title, const QString &text)
{
    if (text.size() > 1200)
        return false;
    static const QRegularExpression wall(
        QStringLiteral("captcha|verif(y|ying) (that )?you('re| are) (a )?human|not a (ro)?bot|"
                       "prove your humanity|unusual traffic|just a moment|checking your browser|"
                       "access denied|are you a robot|complete the (following )?challenge|"
                       "attention required|enable javascript and cookies|verifying your request|"
                       "bots use"),
        QRegularExpression::CaseInsensitiveOption);
    return wall.match(title + QLatin1Char(' ') + text).hasMatch();
}

// THE PAGE'S WORDS. What a person would read: the article or main part where
// the page marks one and it is most of the page, otherwise all of the body -
// as the browser lays it out, so nothing hidden and no markup. Read a second
// time with the menus, sidebars and footers switched off for the instant it
// takes - on Wikipedia that is the first four hundred characters of every
// page - and that reading is the one kept, unless it lost most of the page.
const char kReadScript[] = R"JS((() => {
  const clean = (s) => (s || '').replace(/\u00a0/g, ' ').replace(/[ \t\f\v]+/g, ' ').replace(/ *\n */g, '\n').replace(/\n{3,}/g, '\n\n').trim();
  const pick = () => {
    const body = document.body ? clean(document.body.innerText) : '';
    let best = '';
    for (const el of document.querySelectorAll('article, main, [role="main"]')) {
      const t = clean(el.innerText);
      if (t.length > best.length) best = t;
    }
    return (best.length >= 1500 && best.length >= body.length * 0.4) ? best : body;
  };
  const full = pick();
  const hidden = [];
  let lean = '';
  try {
    for (const el of document.querySelectorAll('nav, aside, footer, [role="navigation"], [role="complementary"], [role="contentinfo"], [role="search"]')) {
      hidden.push([el, el.style.display]);
      el.style.display = 'none';
    }
    lean = pick();
  } finally {
    for (const [el, was] of hidden) el.style.display = was;
  }
  const text = (lean.length >= 400 && lean.length >= full.length * 0.3) ? lean : full;
  return JSON.stringify({title: document.title || '', url: location.href, type: document.contentType || '', text: text.slice(0, 400000)});
})())JS";

const char kLinksScript[] = R"JS((() => {
  const seen = new Set();
  const out = [];
  for (const a of document.querySelectorAll('a[href]')) {
    const href = a.href;
    if (!/^https?:/i.test(href)) continue;
    const text = (a.innerText || a.getAttribute('aria-label') || a.title || '').replace(/\s+/g, ' ').trim();
    if (!text) continue;
    const key = href + '|' + text;
    if (seen.has(key)) continue;
    seen.add(key);
    out.push([text.slice(0, 120), href]);
    if (out.length >= 2000) break;
  }
  return JSON.stringify(out);
})())JS";

// A link or a button by the words on it. A link with an address is not
// clicked but handed back, to be opened in this same tab - a click on one
// that opens a new tab would leave the task looking at the old page.
const char kClickScript[] = R"JS(((want) => {
  const norm = (s) => (s || '').replace(/\s+/g, ' ').trim().toLowerCase();
  want = norm(want);
  const seen = (el) => { const r = el.getBoundingClientRect(); if (r.width < 1 || r.height < 1) return false; const st = getComputedStyle(el); return st.visibility !== 'hidden' && st.display !== 'none'; };
  const words = (el) => el.innerText || el.value || el.getAttribute('aria-label') || el.title || '';
  let best = null, bestScore = 0;
  for (const el of document.querySelectorAll('a[href], button, [role="button"], [role="link"], [role="tab"], [role="menuitem"], input[type="submit"], input[type="button"], summary, label')) {
    const t = norm(words(el));
    if (!t || !seen(el)) continue;
    let score = 0;
    if (t === want) score = 4; else if (t.startsWith(want)) score = 3; else if (t.includes(want)) score = 2; else if (t.length >= 4 && want.includes(t)) score = 1;
    if (!score) continue;
    score = score * 10000 - Math.min(t.length, 9999);
    if (score > bestScore) { best = el; bestScore = score; }
  }
  if (!best) return JSON.stringify({found: false});
  const label = words(best).replace(/\s+/g, ' ').trim().slice(0, 80);
  const raw = best.tagName === 'A' ? (best.getAttribute('href') || '') : '';
  if (raw && raw.charAt(0) !== '#' && /^https?:/i.test(best.href)) return JSON.stringify({found: true, label: label, href: best.href});
  best.scrollIntoView({block: 'center'});
  best.click();
  return JSON.stringify({found: true, label: label});
}))JS";

// A text field by the words of its label, its placeholder or its name. Never
// a password field: the hidden browser signs in to nothing.
const char kTypeScript[] = R"JS(((field, text) => {
  const norm = (s) => (s || '').replace(/\s+/g, ' ').trim().toLowerCase();
  field = norm(field);
  const seen = (el) => { const r = el.getBoundingClientRect(); if (r.width < 1 || r.height < 1) return false; const st = getComputedStyle(el); return st.visibility !== 'hidden' && st.display !== 'none'; };
  const names = (el) => { const out = [el.getAttribute('aria-label'), el.placeholder, el.name, el.id, el.title]; if (el.labels) for (const l of el.labels) out.push(l.innerText); return norm(out.filter(Boolean).join(' | ')); };
  let best = null, bestScore = 0;
  const all = [];
  for (const el of document.querySelectorAll('input:not([type]), input[type="text"], input[type="search"], input[type="email"], input[type="url"], input[type="tel"], input[type="number"], textarea, [contenteditable="true"], [role="searchbox"], [role="textbox"]')) {
    if (!seen(el) || el.disabled || el.readOnly) continue;
    const n = names(el);
    all.push(n.slice(0, 60) || '(no name)');
    let score = 0;
    // No field named: the page's search box if it has one, else its first field.
    const searchy = el.type === 'search' || el.getAttribute('role') === 'searchbox' || el.name === 'q' || n.includes('search');
    if (!field) score = searchy ? 2 : 1; else if (n.includes(field)) score = 3; else if (field.split(' ').every((w) => n.includes(w))) score = 2;
    if (score > bestScore) { best = el; bestScore = score; }
  }
  if (!best) return JSON.stringify({found: false, fields: all.slice(0, 12)});
  best.scrollIntoView({block: 'center'});
  best.focus();
  if (best.isContentEditable) {
    best.textContent = text;
    best.dispatchEvent(new InputEvent('input', {bubbles: true, data: text, inputType: 'insertText'}));
  } else {
    const proto = best.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
    Object.getOwnPropertyDescriptor(proto, 'value').set.call(best, text);
    best.dispatchEvent(new Event('input', {bubbles: true}));
    best.dispatchEvent(new Event('change', {bubbles: true}));
  }
  return JSON.stringify({found: true, label: names(best).slice(0, 80) || 'a text field'});
}))JS";

// THE WORDS OF A PDF. A browser shows one and has no words of it to give, so
// PDF.js - built into this program, third_party/pdfjs - is run in the page:
// it fetches the same file again (from the browser's own cache) and hands
// back the text of each page, in reading order.
const char kPdfScript[] = R"JS((async (workerSource) => {
  pdfjsLib.GlobalWorkerOptions.workerSrc = URL.createObjectURL(new Blob([workerSource], {type: 'text/javascript'}));
  const bytes = new Uint8Array(await (await fetch(location.href)).arrayBuffer());
  const doc = await pdfjsLib.getDocument({data: bytes}).promise;
  let title = '';
  try { const meta = await doc.getMetadata(); title = (meta.info && meta.info.Title) || ''; } catch (e) {}
  const parts = [];
  let total = 0, read = 0;
  for (let n = 1; n <= doc.numPages && total < 400000; n++) {
    const page = await doc.getPage(n);
    const content = await page.getTextContent();
    let text = '';
    for (const item of content.items) { if (typeof item.str === 'string') text += item.str + (item.hasEOL ? '\n' : ''); }
    text = text.replace(/[ \t]+/g, ' ').replace(/ *\n */g, '\n').replace(/\n{3,}/g, '\n\n').trim();
    parts.push('[page ' + n + ']\n' + text);
    total += text.length;
    read = n;
  }
  return JSON.stringify({pages: doc.numPages, read: read, title: String(title), words: total, text: parts.join('\n\n').slice(0, 400000)});
}))JS";

// A file built into the program (web.qrc).
QByteArray builtIn(const QString &name)
{
    QFile file(QStringLiteral(":/web/") + name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// THE SEARCH ENGINES, in the order they are tried: the next one only when the
// one before gave nothing. Each script hands back [{title, url, snippet}],
// with the engine's own redirect taken off the address. Measured on
// 2026-10-13 (_research/web/cdp_probe.mjs): all three answer a hidden Edge
// that says it is Edge; Google does not, and is not asked.
struct Engine
{
    const char *name;
    const char *address;
    const char *script;
};
const Engine kEngines[] = {
    {"DuckDuckGo", "https://html.duckduckgo.com/html/?q=",
     R"JS(JSON.stringify([...document.querySelectorAll('.result')].filter((r) => !r.classList.contains('result--ad')).map((r) => {
  const a = r.querySelector('a.result__a');
  if (!a) return null;
  let u = a.href;
  try { const d = new URL(u).searchParams.get('uddg'); if (d) u = d; } catch (e) {}
  if (/duckduckgo\.com\/y\.js/.test(u)) return null;
  const s = r.querySelector('.result__snippet');
  return {title: (a.innerText || '').trim(), url: u, snippet: s ? (s.innerText || '').trim() : ''};
}).filter(Boolean)))JS"},
    {"Bing", "https://www.bing.com/search?q=",
     R"JS(JSON.stringify([...document.querySelectorAll('li.b_algo')].map((r) => {
  const a = r.querySelector('h2 a');
  if (!a) return null;
  let u = a.href;
  try {
    const p = new URL(u);
    const d = p.searchParams.get('u');
    if (p.hostname.endsWith('bing.com') && d && d.startsWith('a1')) {
      let b = d.slice(2).replace(/-/g, '+').replace(/_/g, '/');
      while (b.length % 4) b += '=';
      u = decodeURIComponent(escape(atob(b)));
    }
  } catch (e) {}
  const s = r.querySelector('.b_caption p, p');
  return {title: (a.innerText || a.textContent || '').trim(), url: u, snippet: s ? (s.innerText || s.textContent || '').trim() : ''};
}).filter(Boolean)))JS"},
    {"Brave Search", "https://search.brave.com/search?q=",
     R"JS(JSON.stringify([...document.querySelectorAll('div.snippet[data-type="web"]')].map((r) => {
  const a = r.querySelector('a[href^="http"]');
  if (!a) return null;
  const lines = (r.innerText || '').split('\n').map((l) => l.trim()).filter(Boolean);
  const head = (a.innerText || '').split('\n').map((l) => l.trim()).filter(Boolean);
  const title = head.length ? head[head.length - 1] : (lines[0] || '');
  const at = lines.indexOf(title);
  return {title: title, url: a.href, snippet: lines.slice(at + 1).join(' ').slice(0, 300)};
}).filter(Boolean)))JS"},
};
constexpr int kEngineCount = int(sizeof(kEngines) / sizeof(kEngines[0]));

// What an engine's script found, as a task is shown it.
QString resultsText(const QJsonArray &found, const QString &query, const QString &engine, int *count)
{
    QString out = QStringLiteral("SEARCH: ") + query + QStringLiteral("   (") + engine
                  + QStringLiteral(")\n");
    int listed = 0;
    QStringList seen;
    for (const QJsonValue &one : found) {
        const QJsonObject hit = one.toObject();
        const QString link = hit.value(QStringLiteral("url")).toString();
        if (link.isEmpty() || seen.contains(link))
            continue;
        seen.append(link);
        out += QStringLiteral("\n") + number(++listed) + QStringLiteral(". ")
               + hit.value(QStringLiteral("title")).toString().simplified().left(160)
               + QStringLiteral("\n   ") + link;
        const QString words = hit.value(QStringLiteral("snippet")).toString().simplified();
        if (!words.isEmpty())
            out += QStringLiteral("\n   ") + words.left(300);
        if (listed >= 10)
            break;
    }
    out += QStringLiteral("\n\nThese few words are not the answer. Open the two or three that look "
                          "best, with action open and their url, and read them.");
    *count = listed;
    return out;
}

} // namespace

// ===========================================================================
// THE PROCESS AND THE WIRE
// ===========================================================================

WebBrowser::WebBrowser(QObject *parent)
    : QObject(parent)
{
    m_clock.start();
    m_poll = new QTimer(this);
    m_poll->setInterval(100);
    connect(m_poll, &QTimer::timeout, this, &WebBrowser::pollPort);
    m_sweep = new QTimer(this);
    m_sweep->setInterval(1000);
    connect(m_sweep, &QTimer::timeout, this, &WebBrowser::sweep);
    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    // BURNOS_WEB_IDLE_MS, for the check that it closes: never set in a normal run.
    const int idle = qEnvironmentVariableIntValue("BURNOS_WEB_IDLE_MS");
    m_idle->setInterval(idle > 0 ? idle : kIdleMs);
    connect(m_idle, &QTimer::timeout, this, [this]() {
        if (m_state == State::Off)
            return;
        // A task that still has its tab may want its page again.
        if (m_running > 0 || !m_tabs.isEmpty()) {
            m_idle->start();
            return;
        }
        liveLog(QStringLiteral("web: nothing asked of the hidden browser for %1 seconds - closed, "
                               "and started again when it is next needed")
                    .arg(m_idle->interval() / 1000));
        shutDown();
    });
}

WebBrowser::~WebBrowser()
{
    stopProcess(false);
}

QString WebBrowser::program(QString *name)
{
    const Found found = findBrowser();
    if (name)
        *name = found.name;
    return found.path;
}

void WebBrowser::whenReady(Then then)
{
    if (m_state == State::Ready) {
        then(QString());
        return;
    }
    m_waiters.append(then);
    if (m_state == State::Off)
        start();
}

void WebBrowser::start()
{
    const Found found = findBrowser();
    if (found.path.isEmpty()) {
        failAll(tr("this PC has no browser Burnos can run hidden - it looks for Microsoft Edge, "
                   "Google Chrome and Brave"));
        return;
    }
    m_state = State::Starting;
    m_name = found.name;
    m_startClock.start();
    m_profile = profileDir() + (m_profileTry ? QStringLiteral("-2") : QString());
    QDir().mkpath(m_profile);
    // Left by the run before: it would be read as this run's port.
    QFile::remove(m_profile + QStringLiteral("/DevToolsActivePort"));

    QStringList args{
        QStringLiteral("--headless=new"),
        QStringLiteral("--remote-debugging-port=0"),
        QStringLiteral("--user-data-dir=") + QDir::toNativeSeparators(m_profile),
        QStringLiteral("--no-first-run"),
        QStringLiteral("--no-default-browser-check"),
        QStringLiteral("--disable-extensions"),
        QStringLiteral("--disable-sync"),
        QStringLiteral("--disable-background-networking"),
        QStringLiteral("--disable-component-update"),
        QStringLiteral("--disable-default-apps"),
        QStringLiteral("--mute-audio"),
        QStringLiteral("--window-size=1366,900"),
        // A page left behind is let go of at once, not kept whole in case of
        // a step back, and no process is kept warm for a page not asked for
        // yet: a dozen pages into some research that was eleven page
        // processes and 1.2 GB.
        QStringLiteral("--disable-features=BackForwardCache,SpareRendererForSitePerProcess,"
                       "Translate,MediaRouter,OptimizationHints"),
    };
    // IT SAYS WHAT IT IS. Left alone it calls itself "HeadlessChrome", and
    // DuckDuckGo, Google and Brave Search answer that word with a puzzle for
    // humans; as the Edge 155 it is, all but Google answer. Nothing else
    // about it is dressed up.
    const int major = majorVersion(found.path);
    if (major > 0) {
        QString agent = QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                                       "(KHTML, like Gecko) Chrome/%1.0.0.0 Safari/537.36").arg(major);
        if (found.edge)
            agent += QStringLiteral(" Edg/%1.0.0.0").arg(major);
        args.append(QStringLiteral("--user-agent=") + agent);
    }
    args.append(QStringLiteral("about:blank"));

    m_proc = new QProcess(this);
    m_proc->setStandardOutputFile(QProcess::nullDevice());
    m_proc->setStandardErrorFile(QProcess::nullDevice());
    connect(m_proc, &QProcess::started, this, [this]() {
#ifdef Q_OS_WIN
        // Into a job before it has started much of its own: closing the job
        // takes every process of the hidden browser with it, and Windows
        // closes the job when Burnos goes, however it goes.
        if (!m_proc)
            return;
        if (!m_job) {
            m_job = CreateJobObjectW(nullptr, nullptr);
            if (m_job) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(static_cast<HANDLE>(m_job), JobObjectExtendedLimitInformation,
                                        &limits, sizeof(limits));
            }
        }
        if (!m_job)
            return;
        HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                     static_cast<DWORD>(m_proc->processId()));
        if (!process)
            return;
        AssignProcessToJobObject(static_cast<HANDLE>(m_job), process);
        CloseHandle(process);
#endif
    });
    const auto gone = [this]() {
        if (m_state == State::Off)
            return;
        // Gone before it opened a port: a browser left over from a run that
        // crashed still holds the profile folder, and this one handed over to
        // it and left. Once, the second folder is tried.
        if (m_state == State::Starting && m_profileTry == 0) {
            liveLog(QStringLiteral("web: the hidden browser left at once - its profile folder is "
                                   "held by another; trying a second folder"));
            stopProcess(false);
            m_profileTry = 1;
            start();
            return;
        }
        died(tr("the hidden browser stopped"));
    };
    connect(m_proc, &QProcess::finished, this, gone);
    connect(m_proc, &QProcess::errorOccurred, this, [this, gone](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            gone();
    });

    liveLog(QStringLiteral("web: starting a hidden %1 %2 of Burnos's own, profile %3")
                .arg(found.name, major > 0 ? number(major) : QString(),
                     QDir::toNativeSeparators(m_profile)));
    m_proc->start(found.path, args);
    m_poll->start();
    m_sweep->start();
}

void WebBrowser::pollPort()
{
    if (m_state != State::Starting) {
        m_poll->stop();
        return;
    }
    // The browser writes its port and the path of its websocket here.
    QFile file(m_profile + QStringLiteral("/DevToolsActivePort"));
    if (file.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = file.readAll().split('\n');
        if (lines.size() >= 2 && !lines.at(1).trimmed().isEmpty()) {
            m_poll->stop();
            connectSocket(lines.at(0).trimmed().toInt(), QString::fromLatin1(lines.at(1).trimmed()));
            return;
        }
    }
    if (m_startClock.elapsed() > kStartMs)
        died(tr("the hidden browser did not come up in %1 seconds").arg(kStartMs / 1000));
}

void WebBrowser::connectSocket(int port, const QString &path)
{
    m_socket = new QTcpSocket(this);
    // Localhost is never behind the system proxy.
    m_socket->setProxy(QNetworkProxy::NoProxy);
    m_shaken = false;
    m_in.clear();
    m_message.clear();
    connect(m_socket, &QTcpSocket::connected, this, [this, port, path]() {
        // No Origin header: the browser takes a websocket from a program, and
        // refuses one from a web page it was not told about.
        quint32 words[4];
        QRandomGenerator::system()->fillRange(words);
        const QByteArray key = QByteArray(reinterpret_cast<const char *>(words), int(sizeof words)).toBase64();
        m_socket->write("GET " + path.toLatin1() + " HTTP/1.1\r\nHost: 127.0.0.1:"
                        + QByteArray::number(port)
                        + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key
                        + "\r\nSec-WebSocket-Version: 13\r\n\r\n");
    });
    connect(m_socket, &QTcpSocket::readyRead, this, &WebBrowser::onSocketData);
    connect(m_socket, &QTcpSocket::disconnected, this,
            [this]() { died(tr("the hidden browser hung up")); });
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        died(tr("the hidden browser could not be reached (%1)")
                 .arg(m_socket ? m_socket->errorString() : QString()));
    });
    m_socket->connectToHost(QHostAddress(QHostAddress::LocalHost), quint16(port));
}

void WebBrowser::onSocketData()
{
    if (!m_socket)
        return;
    m_in.append(m_socket->readAll());
    if (!m_shaken) {
        const qsizetype end = m_in.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        const QByteArray head = m_in.left(end);
        m_in.remove(0, end + 4);
        if (!head.startsWith("HTTP/1.1 101")) {
            died(tr("the hidden browser refused the connection (%1)")
                     .arg(QString::fromLatin1(head.left(60)).simplified()));
            return;
        }
        m_shaken = true;
        ready();
    }
    // A websocket frame: a byte of flags and kind, the length in one, three
    // or nine bytes, then that many bytes. What a server sends is not masked.
    while (m_shaken && m_socket) {
        if (m_in.size() < 2)
            return;
        const auto *bytes = reinterpret_cast<const uchar *>(m_in.constData());
        const bool last = (bytes[0] & 0x80) != 0;
        const int opcode = bytes[0] & 0x0f;
        quint64 length = bytes[1] & 0x7f;
        qsizetype at = 2;
        if (length == 126) {
            if (m_in.size() < 4)
                return;
            length = qFromBigEndian<quint16>(bytes + 2);
            at = 4;
        } else if (length == 127) {
            if (m_in.size() < 10)
                return;
            length = qFromBigEndian<quint64>(bytes + 2);
            at = 10;
        }
        if (length > kMaxMessageBytes) {
            died(tr("the hidden browser sent something far too large"));
            return;
        }
        if (quint64(m_in.size() - at) < length)
            return;
        const QByteArray payload = m_in.mid(at, qsizetype(length));
        m_in.remove(0, at + qsizetype(length));

        if (opcode == 0x9) { // ping: the same bytes back
            sendFrame(0xA, payload);
            continue;
        }
        if (opcode == 0x8) { // close
            died(tr("the hidden browser closed the connection"));
            return;
        }
        if (opcode == 0xA)
            continue;
        // Text, whole or in pieces.
        m_message.append(payload);
        if (!last)
            continue;
        const QByteArray whole = m_message;
        m_message.clear();
        dispatch(QJsonDocument::fromJson(whole).object());
    }
}

void WebBrowser::ready()
{
    m_state = State::Ready;
    m_profileTry = 0;
    const qint64 took = m_startClock.elapsed();
    send(QStringLiteral("Browser.getVersion"), QJsonObject(), QString(),
         [took](const QJsonObject &about, const QString &) {
        liveLog(QStringLiteral("web: the hidden browser is ready in %1 ms - %2, no window, nothing "
                               "shared with the user's own browser")
                    .arg(number(took), about.value(QStringLiteral("product")).toString()));
    });
    // It reads pages; it keeps no files.
    send(QStringLiteral("Browser.setDownloadBehavior"),
         QJsonObject{{QStringLiteral("behavior"), QStringLiteral("deny")}}, QString(), nullptr);
    const QList<Then> waiters = m_waiters;
    m_waiters.clear();
    for (const Then &then : waiters)
        then(QString());
}

void WebBrowser::sendFrame(int opcode, const QByteArray &payload)
{
    if (!m_socket)
        return;
    // What a client sends IS masked: four random bytes, and the payload
    // xor'ed with them.
    QByteArray frame;
    frame.reserve(payload.size() + 14);
    frame.append(char(0x80 | opcode));
    const quint64 length = quint64(payload.size());
    if (length < 126) {
        frame.append(char(0x80 | int(length)));
    } else if (length < 65536) {
        frame.append(char(0x80 | 126));
        uchar size[2];
        qToBigEndian<quint16>(quint16(length), size);
        frame.append(reinterpret_cast<const char *>(size), 2);
    } else {
        frame.append(char(0x80 | 127));
        uchar size[8];
        qToBigEndian<quint64>(length, size);
        frame.append(reinterpret_cast<const char *>(size), 8);
    }
    const quint32 key = QRandomGenerator::global()->generate();
    const char mask[4] = {char(key & 0xff), char((key >> 8) & 0xff), char((key >> 16) & 0xff),
                          char((key >> 24) & 0xff)};
    frame.append(mask, 4);
    const qsizetype at = frame.size();
    frame.append(payload);
    char *body = frame.data() + at;
    for (qsizetype i = 0; i < payload.size(); ++i)
        body[i] = char(body[i] ^ mask[i & 3]);
    m_socket->write(frame);
}

void WebBrowser::send(const QString &method, const QJsonObject &params, const QString &session,
                      Reply then, int timeoutMs)
{
    if (!m_socket || !m_shaken) {
        if (then) {
            QTimer::singleShot(0, this, [then]() {
                then(QJsonObject(), tr("the hidden browser is not running"));
            });
        }
        return;
    }
    const int id = ++m_nextId;
    QJsonObject message{{QStringLiteral("id"), id},
                        {QStringLiteral("method"), method},
                        {QStringLiteral("params"), params}};
    if (!session.isEmpty())
        message.insert(QStringLiteral("sessionId"), session);
    if (then)
        m_pending.insert(id, Pending{then, m_clock.elapsed() + timeoutMs, method});
    sendFrame(0x1, QJsonDocument(message).toJson(QJsonDocument::Compact));
}

void WebBrowser::dispatch(const QJsonObject &message)
{
    if (message.contains(QStringLiteral("id"))) {
        const auto it = m_pending.find(message.value(QStringLiteral("id")).toInt());
        if (it == m_pending.end())
            return;
        const Reply then = it->then;
        m_pending.erase(it);
        const QJsonObject error = message.value(QStringLiteral("error")).toObject();
        then(message.value(QStringLiteral("result")).toObject(),
             error.isEmpty() ? QString()
                             : error.value(QStringLiteral("message")).toString(QStringLiteral("refused")));
        return;
    }

    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonObject params = message.value(QStringLiteral("params")).toObject();
    if (method == QLatin1String("Page.javascriptDialogOpening")) {
        // An alert nobody can see would hold the page for ever.
        send(QStringLiteral("Page.handleJavaScriptDialog"),
             QJsonObject{{QStringLiteral("accept"), true}},
             message.value(QStringLiteral("sessionId")).toString(), nullptr);
        return;
    }
    QString lost;
    if (method == QLatin1String("Inspector.targetCrashed"))
        lost = message.value(QStringLiteral("sessionId")).toString();
    else if (method == QLatin1String("Target.detachedFromTarget"))
        lost = params.value(QStringLiteral("sessionId")).toString();
    if (lost.isEmpty())
        return;
    for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
        if (it->sessionId == lost) {
            liveLog(QStringLiteral("web: a page of the hidden browser was lost (%1)").arg(method));
            m_tabs.erase(it);
            return;
        }
    }
}

void WebBrowser::sweep()
{
    const qint64 now = m_clock.elapsed();
    QList<int> late;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (it->due <= now)
            late.append(it.key());
    }
    for (int id : std::as_const(late)) {
        const auto it = m_pending.find(id);
        if (it == m_pending.end())
            continue;
        const Pending pending = it.value();
        m_pending.erase(it);
        pending.then(QJsonObject(), tr("the hidden browser gave no answer to %1").arg(pending.method));
    }
}

void WebBrowser::stopProcess(bool politely)
{
    m_state = State::Off;
    if (m_poll)
        m_poll->stop();
    if (m_sweep)
        m_sweep->stop();
    if (m_idle)
        m_idle->stop();
    m_tabs.clear();

    if (m_socket) {
        m_socket->disconnect(this);
        if (politely && m_shaken) {
            // Asked to close, so that it leaves its profile folder in order.
            sendFrame(0x1, QJsonDocument(QJsonObject{{QStringLiteral("id"), ++m_nextId},
                                                     {QStringLiteral("method"),
                                                      QStringLiteral("Browser.close")}})
                               .toJson(QJsonDocument::Compact));
            m_socket->flush();
        }
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_shaken = false;
    m_in.clear();
    m_message.clear();

    QProcess *proc = m_proc;
    void *job = m_job;
    m_proc = nullptr;
    m_job = nullptr;
    const auto end = [proc, job]() {
#ifdef Q_OS_WIN
        if (job) {
            TerminateJobObject(static_cast<HANDLE>(job), 0);
            CloseHandle(static_cast<HANDLE>(job));
        }
#else
        Q_UNUSED(job);
#endif
        if (proc) {
            proc->kill();
            proc->deleteLater();
        }
    };
    if (proc)
        proc->disconnect(this);
    if (politely && proc)
        QTimer::singleShot(1500, proc, end);
    else
        end();
}

void WebBrowser::failAll(const QString &why)
{
    const QHash<int, Pending> pending = m_pending;
    m_pending.clear();
    const QList<Then> waiters = m_waiters;
    m_waiters.clear();
    for (const Pending &one : pending)
        one.then(QJsonObject(), why);
    for (const Then &then : waiters)
        then(why);
}

void WebBrowser::died(const QString &why)
{
    if (m_state == State::Off)
        return;
    liveLog(QStringLiteral("web: %1 - it is started again the next time it is needed").arg(why));
    stopProcess(false);
    failAll(why);
}

void WebBrowser::shutDown()
{
    if (m_state == State::Off && !m_proc)
        return;
    stopProcess(true);
    failAll(tr("the hidden browser was closed"));
}

// ===========================================================================
// A TAB AND ITS PAGE
// ===========================================================================

WebBrowser::Tab *WebBrowser::tab(int owner)
{
    const auto it = m_tabs.find(owner);
    return it == m_tabs.end() ? nullptr : &it.value();
}

void WebBrowser::tabFor(int owner, Then then)
{
    whenReady([this, owner, then](const QString &error) {
        if (!error.isEmpty()) {
            then(error);
            return;
        }
        if (tab(owner)) {
            then(QString());
            return;
        }
        send(QStringLiteral("Target.createTarget"),
             QJsonObject{{QStringLiteral("url"), QStringLiteral("about:blank")}}, QString(),
             [this, owner, then](const QJsonObject &made, const QString &error) {
            if (!error.isEmpty()) {
                then(error);
                return;
            }
            const QString target = made.value(QStringLiteral("targetId")).toString();
            send(QStringLiteral("Target.attachToTarget"),
                 QJsonObject{{QStringLiteral("targetId"), target}, {QStringLiteral("flatten"), true}},
                 QString(),
                 [this, owner, target, then](const QJsonObject &attached, const QString &error) {
                if (!error.isEmpty()) {
                    then(error);
                    return;
                }
                Tab made;
                made.targetId = target;
                made.sessionId = attached.value(QStringLiteral("sessionId")).toString();
                m_tabs.insert(owner, made);
                // For the dialogs a page may put up.
                send(QStringLiteral("Page.enable"), QJsonObject(), made.sessionId,
                     [then](const QJsonObject &, const QString &) { then(QString()); });
            });
        });
    });
}

void WebBrowser::release(int owner)
{
    const auto it = m_tabs.find(owner);
    if (it == m_tabs.end())
        return;
    send(QStringLiteral("Target.closeTarget"),
         QJsonObject{{QStringLiteral("targetId"), it->targetId}}, QString(), nullptr);
    m_tabs.erase(it);
    if (m_state == State::Ready && m_running == 0)
        m_idle->start();
}

void WebBrowser::evaluate(int owner, const QString &expression, Value then, bool awaitPromise,
                          int timeoutMs)
{
    const Tab *page = tab(owner);
    if (!page) {
        QTimer::singleShot(0, this, [then]() { then(QJsonValue(), tr("that page is gone")); });
        return;
    }
    send(QStringLiteral("Runtime.evaluate"),
         QJsonObject{{QStringLiteral("expression"), expression},
                     {QStringLiteral("returnByValue"), true},
                     {QStringLiteral("awaitPromise"), awaitPromise}},
         page->sessionId, [then](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty()) {
            then(QJsonValue(), error);
            return;
        }
        if (result.contains(QStringLiteral("exceptionDetails"))) {
            const QJsonObject details = result.value(QStringLiteral("exceptionDetails")).toObject();
            QString why = details.value(QStringLiteral("exception")).toObject()
                              .value(QStringLiteral("description")).toString();
            if (why.isEmpty())
                why = details.value(QStringLiteral("text")).toString(QStringLiteral("the page's script failed"));
            then(QJsonValue(), why.left(300));
            return;
        }
        then(result.value(QStringLiteral("result")).toObject().value(QStringLiteral("value")), QString());
    }, timeoutMs);
}

void WebBrowser::waitLoaded(int owner, std::function<void()> then)
{
    auto wait = std::make_shared<LoadWait>();
    wait->clock.start();
    pollLoaded(owner, wait, then);
}

void WebBrowser::pollLoaded(int owner, std::shared_ptr<LoadWait> wait, std::function<void()> then)
{
    if (m_state != State::Ready || !tab(owner)) {
        then();
        return;
    }
    // LOADED IS WHEN THE WORDS HAVE STOPPED ARRIVING, not when the browser
    // says so: half the web draws itself after that. Asked four times a
    // second - the page says how far it is and how many characters it shows.
    evaluate(owner, QStringLiteral("document.readyState + '|' + (document.body ? document.body.innerText.length : 0) + '|' + document.contentType"),
             [this, owner, wait, then](const QJsonValue &value, const QString &error) {
        const qint64 ms = wait->clock.elapsed();
        bool done = ms > kLoadMs;
        // An error here is a page on its way in: asked again.
        if (error.isEmpty()) {
            const QStringList said = value.toString().split(QLatin1Char('|'));
            const QString state = said.value(0);
            const int length = said.value(1).toInt();
            wait->steady = length == wait->lastLength ? wait->steady + 1 : 0;
            wait->lastLength = length;
            // A page with next to nothing on it may not have drawn itself yet -
            // but a PDF never has any words on it, however long it is given.
            const bool thin = length < 200 && ms < 3000
                              && !said.value(2).contains(QLatin1String("pdf"));
            if (state == QLatin1String("complete") && wait->steady >= 1 && ms >= kSettleMs && !thin)
                done = true;
            // Still fetching something after six seconds, and the words have
            // not changed for one: read as it stands.
            if (state != QLatin1String("loading") && wait->steady >= 4 && ms > 6000)
                done = true;
        }
        if (done) {
            then();
            return;
        }
        QTimer::singleShot(250, this, [this, owner, wait, then]() { pollLoaded(owner, wait, then); });
    });
}

void WebBrowser::readPage(int owner, Then then)
{
    evaluate(owner, QString::fromLatin1(kReadScript),
             [this, owner, then](const QJsonValue &value, const QString &error) {
        Tab *page = tab(owner);
        if (!page) {
            then(tr("that page is gone"));
            return;
        }
        if (!error.isEmpty()) {
            then(error);
            return;
        }
        const QJsonObject read = QJsonDocument::fromJson(value.toString().toUtf8()).object();
        page->title = read.value(QStringLiteral("title")).toString();
        page->url = read.value(QStringLiteral("url")).toString();
        page->type = read.value(QStringLiteral("type")).toString();
        page->text = read.value(QStringLiteral("text")).toString();
        page->at = 0;
        page->results = 0;
        page->pdfPages = 0;
        if (page->type.contains(QLatin1String("pdf"), Qt::CaseInsensitive)) {
            readPdf(owner, then);
            return;
        }

        // A SEARCH ENGINE'S PAGE IS READ AS RESULTS - title, address, a few
        // words - whether a search brought the tab here or going back did.
        // Read as a page it is a list of fifty countries and then the results.
        int engine = -1;
        for (int i = 0; i < kEngineCount && engine < 0; ++i) {
            if (page->url.startsWith(QLatin1String(kEngines[i].address)))
                engine = i;
        }
        if (engine < 0) {
            then(QString());
            return;
        }
        evaluate(owner, QString::fromLatin1(kEngines[engine].script),
                 [this, owner, engine, then](const QJsonValue &value, const QString &) {
            Tab *page = tab(owner);
            if (!page) {
                then(tr("that page is gone"));
                return;
            }
            const QJsonArray found = QJsonDocument::fromJson(value.toString().toUtf8()).array();
            if (!found.isEmpty()) {
                const QString query = QUrlQuery(QUrl(page->url))
                                          .queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded);
                page->text = resultsText(found, query, QString::fromLatin1(kEngines[engine].name),
                                         &page->results);
                page->title = QStringLiteral("Search: ") + query;
            }
            then(QString());
        });
    });
}

void WebBrowser::readPdf(int owner, Then then)
{
    static const QString library = QString::fromUtf8(builtIn(QStringLiteral("pdf.min.js")));
    static const QString worker = QString::fromUtf8(builtIn(QStringLiteral("pdf.worker.min.js")));
    if (library.isEmpty() || worker.isEmpty()) {
        liveLog(QStringLiteral("web: PDF.js is not in this build - the PDF is left unread"));
        then(QString());
        return;
    }
    QElapsedTimer clock;
    clock.start();
    // The library first, which leaves pdfjsLib in the page; then the reading,
    // which is handed the worker's source to start as a worker of the page's.
    evaluate(owner, library + QStringLiteral("\n;typeof pdfjsLib"),
             [this, owner, then, clock](const QJsonValue &loaded, const QString &error) {
        if (!error.isEmpty() || loaded.toString() != QLatin1String("object")) {
            liveLog(QStringLiteral("web: PDF.js would not load in the page (%1) - the PDF is left unread")
                        .arg(error.left(160)));
            then(QString());
            return;
        }
        evaluate(owner, QString::fromLatin1(kPdfScript) + QLatin1Char('(') + js(worker) + QLatin1Char(')'),
                 [this, owner, then, clock](const QJsonValue &value, const QString &error) {
            Tab *page = tab(owner);
            if (!page) {
                then(tr("that page is gone"));
                return;
            }
            if (!error.isEmpty()) {
                liveLog(QStringLiteral("web: the PDF could not be read (%1)").arg(error.left(200)));
                then(QString());
                return;
            }
            const QJsonObject read = QJsonDocument::fromJson(value.toString().toUtf8()).object();
            page->pdfPages = read.value(QStringLiteral("pages")).toInt();
            // A scan has pages and no words: left empty, and said so.
            if (read.value(QStringLiteral("words")).toInt() > 0)
                page->text = read.value(QStringLiteral("text")).toString();
            page->title = read.value(QStringLiteral("title")).toString().simplified();
            if (page->title.isEmpty())
                page->title = QUrl(page->url).fileName();
            liveLog(QStringLiteral("web: read a PDF of %1 page(s), %2 chars, in %3 ms")
                        .arg(number(page->pdfPages), number(page->text.size()), number(clock.elapsed())));
            then(QString());
        }, true, 60000);
    });
}

QString WebBrowser::report(int owner)
{
    Tab *page = tab(owner);
    if (!page)
        return tr("Failed: that page is gone.");
    if (page->results > 0) {
        page->at = int(page->text.size());
        return page->text;
    }
    const QString head = QStringLiteral("PAGE: ") + page->title.simplified().left(200)
                         + QStringLiteral("\nURL: ") + page->url + QLatin1Char('\n');
    if (page->type.contains(QLatin1String("pdf"), Qt::CaseInsensitive)) {
        if (page->text.isEmpty()) {
            return head + (page->pdfPages > 0
                               ? tr("This is a PDF of %1 page(s) with no words in it to read - "
                                    "scanned pictures of pages, most likely.").arg(page->pdfPages)
                               : tr("This is a PDF, and its words could not be read out of it."))
                   + tr(" Look for the same thing as a web page, or, if the user wants to look at "
                        "it, open it in their own browser with computer_use open_url.");
        }
        page->at = 0;
        return head + tr("A PDF of %1 page(s); each page starts with its number in square "
                         "brackets.\n").arg(page->pdfPages)
               + part(*page);
    }
    if (turnedAway(page->title, page->text)) {
        liveLog(QStringLiteral("web: TURNED AWAY by %1 - it asks for proof of a human")
                    .arg(QUrl(page->url).host()));
        return head + tr("THIS SITE TURNED THE HIDDEN BROWSER AWAY: it shows a check that a human "
                         "is asking, not its page. Do not try to get round it, and do not ask this "
                         "site again in this task - its other pages answer the same. If the fact "
                         "you are after is to be had elsewhere, get it from another source. If "
                         "what was asked for is what THIS site itself shows - its front page, a "
                         "post, a listing - then no other page is that: say in your report that "
                         "the site would not let Burnos's own browser in, give what a second-hand "
                         "source says only as second-hand, and offer to open the page in the "
                         "user's own browser. What it shows:\n\n")
               + page->text.left(500);
    }
    if (page->text.isEmpty()) {
        return head + tr("There are no words on it to read - it may be all pictures, or want "
                         "something clicked first. Use screenshot to look at it, or links to see "
                         "where it leads.");
    }
    page->at = 0;
    return head + part(*page);
}

QString WebBrowser::part(Tab &tab)
{
    const int total = int(tab.text.size());
    const int from = tab.at;
    if (from >= total)
        return tr("That was the end of the page: all %1 characters of it have been read.").arg(total);
    int to = qMin(total, from + kPartChars);
    // Cut at the end of a line where one is near.
    if (to < total) {
        const int line = int(tab.text.lastIndexOf(QLatin1Char('\n'), to));
        if (line > from + kPartChars - 1500)
            to = line;
    }
    tab.at = to;
    QString head;
    if (from == 0 && to >= total) {
        head = tr("All of it, %1 characters:").arg(total);
    } else {
        head = tr("Characters %1 to %2 of %3").arg(from + 1).arg(to).arg(total)
               + (to < total ? tr(" - action more gives the next part, and find with text jumps to "
                                  "a word:")
                             : tr(" - the end of the page:"));
    }
    return head + QStringLiteral("\n\n") + tab.text.mid(from, to - from).trimmed();
}

// ===========================================================================
// THE ACTIONS
// ===========================================================================

void WebBrowser::run(int owner, const QVariantMap &args, Done done)
{
    ++m_running;
    m_idle->stop();
    auto over = std::make_shared<bool>(false);
    const Done finish = [this, over, done](const QString &text, const QByteArray &png) {
        if (*over)
            return;
        *over = true;
        --m_running;
        if (m_state != State::Off)
            m_idle->start();
        done(text, png);
    };
    QTimer::singleShot(kCallMs, this, [finish]() {
        finish(tr("Failed: the hidden browser did not finish that in %1 seconds. Try it again, "
                  "or another page.").arg(kCallMs / 1000), QByteArray());
    });
    // Never from inside this call.
    QTimer::singleShot(0, this, [this, owner, args, finish]() { perform(owner, args, finish); });
}

void WebBrowser::perform(int owner, const QVariantMap &args, Done finish)
{
    const QString action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
    const QString text = args.value(QStringLiteral("text")).toString();
    QString query = args.value(QStringLiteral("query")).toString().simplified();
    QString url = args.value(QStringLiteral("url")).toString().trimmed();

    static const QStringList known{
        QStringLiteral("search"), QStringLiteral("open"), QStringLiteral("more"),
        QStringLiteral("find"), QStringLiteral("links"), QStringLiteral("click"),
        QStringLiteral("type"), QStringLiteral("back"), QStringLiteral("screenshot"),
    };
    if (!known.contains(action)) {
        finish(tr("Failed: web has no action '%1'. It has search, open, more, find, links, click, "
                  "type, back and screenshot.").arg(action.left(40)), QByteArray());
        return;
    }
    if (action == QLatin1String("search")) {
        if (query.isEmpty())
            query = text.simplified();
        if (query.isEmpty()) {
            finish(tr("Failed: search needs query, the words to search for."), QByteArray());
            return;
        }
    }
    if (action == QLatin1String("open")) {
        if (url.isEmpty()) {
            finish(tr("Failed: open needs url, the address of the page."), QByteArray());
            return;
        }
        if (!url.contains(QLatin1String("://")))
            url.prepend(QStringLiteral("https://"));
        const QUrl parsed(url);
        const QString scheme = parsed.scheme().toLower();
        if (!parsed.isValid() || parsed.host().isEmpty()
            || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) {
            finish(tr("Failed: the hidden browser opens web addresses only, http or https, and "
                      "that is not one."), QByteArray());
            return;
        }
    }
    if ((action == QLatin1String("find") || action == QLatin1String("click")) && text.trimmed().isEmpty()) {
        finish(tr("Failed: %1 needs text.").arg(action), QByteArray());
        return;
    }
    if (action == QLatin1String("type") && text.isEmpty()) {
        finish(tr("Failed: type needs text, the words to type."), QByteArray());
        return;
    }

    // What works on the page that is open needs one to be open.
    const bool onPage = action != QLatin1String("search") && action != QLatin1String("open");
    if (onPage) {
        Tab *page = tab(owner);
        if (!page || page->url.isEmpty()) {
            finish(tr("There is no page open in the hidden browser yet: use search, or open with "
                      "a url, first."), QByteArray());
            return;
        }
        if (action == QLatin1String("more")) {
            finish(QStringLiteral("PAGE: ") + page->title.simplified().left(200) + QLatin1Char('\n')
                       + part(*page), QByteArray());
            return;
        }
        if (action == QLatin1String("find")) {
            const QString want = text.simplified();
            QList<int> hits;
            int at = 0;
            while (hits.size() < 200) {
                at = int(page->text.indexOf(want, at, Qt::CaseInsensitive));
                if (at < 0)
                    break;
                hits.append(at);
                at += int(want.size());
            }
            if (hits.isEmpty()) {
                finish(QStringLiteral("'") + want + tr("' is not on this page as written. Try "
                                                        "fewer words, or another word for it."),
                       QByteArray());
                return;
            }
            const int total = int(page->text.size());
            QString out = QStringLiteral("'") + want + QStringLiteral("' ")
                          + tr("is on this page %1 time(s). The passages:").arg(hits.size());
            int shown = 0;
            int end = -1;
            for (int hit : std::as_const(hits)) {
                if (hit < end)
                    continue;
                if (++shown > 6)
                    break;
                const int from = qMax(0, hit - 350);
                end = qMin(total, hit + int(want.size()) + 450);
                out += QStringLiteral("\n\n--- at character %1 of %2 ---\n").arg(hit + 1).arg(total)
                       + page->text.mid(from, end - from).trimmed();
            }
            finish(out, QByteArray());
            return;
        }
    }

    const bool submit = args.value(QStringLiteral("submit")).toBool();
    const QString field = args.value(QStringLiteral("field")).toString();
    tabFor(owner, [this, owner, action, text, query, url, field, submit, finish](const QString &error) {
        if (!error.isEmpty()) {
            finish(tr("Failed: the hidden browser could not be used - ") + error
                       + tr(". Say so in your report."), QByteArray());
            return;
        }
        if (action == QLatin1String("search"))
            search(owner, query, 0, finish);
        else if (action == QLatin1String("open"))
            goTo(owner, url, QString(), finish);
        else if (action == QLatin1String("links"))
            links(owner, query, finish);
        else if (action == QLatin1String("click"))
            click(owner, text, finish);
        else if (action == QLatin1String("type"))
            type(owner, field, text, submit, finish);
        else if (action == QLatin1String("back"))
            back(owner, finish);
        else
            screenshot(owner, finish);
    });
}

void WebBrowser::search(int owner, const QString &query, int engine, Done finish)
{
    Tab *page = tab(owner);
    if (!page || engine >= kEngineCount) {
        liveLog(QStringLiteral("web: search '%1' - NO engine gave results").arg(query.left(80)));
        finish(tr("Failed: none of the search engines gave the hidden browser results for that - "
                  "they may be asking for proof of a human just now, or there is no connection. "
                  "If you know a site that would have it, open that directly."), QByteArray());
        return;
    }
    const Engine &with = kEngines[engine];
    const QString address = QString::fromLatin1(with.address)
                            + QString::fromLatin1(QUrl::toPercentEncoding(query));
    QElapsedTimer clock;
    clock.start();
    send(QStringLiteral("Page.navigate"), QJsonObject{{QStringLiteral("url"), address}},
         page->sessionId,
         [this, owner, query, engine, clock, finish](const QJsonObject &result, const QString &error) {
        if (!error.isEmpty() || !result.value(QStringLiteral("errorText")).toString().isEmpty()) {
            search(owner, query, engine + 1, finish);
            return;
        }
        waitLoaded(owner, [this, owner, query, engine, clock, finish]() {
            readPage(owner, [this, owner, query, engine, clock, finish](const QString &) {
                Tab *page = tab(owner);
                const QString name = QString::fromLatin1(kEngines[engine].name);
                if (!page || page->results == 0) {
                    liveLog(QStringLiteral("web: %1 gave nothing for '%2' - the next engine")
                                .arg(name, query.left(80)));
                    search(owner, query, engine + 1, finish);
                    return;
                }
                page->at = int(page->text.size());
                liveLog(QStringLiteral("web: search '%1' - %2 result(s) from %3 in %4 ms")
                            .arg(query.left(80), number(page->results), name,
                                 number(clock.elapsed())));
                finish(page->text, QByteArray());
            });
        });
    });
}

void WebBrowser::goTo(int owner, const QString &url, const QString &lead, Done finish)
{
    const Tab *page = tab(owner);
    if (!page) {
        finish(tr("Failed: the hidden browser's page is gone. Try that again."), QByteArray());
        return;
    }
    QElapsedTimer clock;
    clock.start();
    send(QStringLiteral("Page.navigate"), QJsonObject{{QStringLiteral("url"), url}}, page->sessionId,
         [this, owner, url, lead, clock, finish](const QJsonObject &result, const QString &error) {
        const QString failed = error.isEmpty() ? result.value(QStringLiteral("errorText")).toString()
                                               : error;
        if (!failed.isEmpty()) {
            liveLog(QStringLiteral("web: %1 would not open (%2)").arg(url.left(160), failed));
            finish(lead + whyNot(url, failed), QByteArray());
            return;
        }
        waitLoaded(owner, [this, owner, url, lead, clock, finish]() {
            readPage(owner, [this, owner, url, lead, clock, finish](const QString &error) {
                if (!error.isEmpty()) {
                    finish(QStringLiteral("Failed: ") + url
                               + tr(" opened, but its words could not be read (") + error
                               + QStringLiteral(")."), QByteArray());
                    return;
                }
                if (const Tab *page = tab(owner)) {
                    liveLog(QStringLiteral("web: opened %1 - '%2', %3 chars in %4 ms")
                                .arg(page->url.left(160), page->title.simplified().left(60),
                                     number(page->text.size()), number(clock.elapsed())));
                }
                finish(lead + report(owner), QByteArray());
            });
        });
    });
}

void WebBrowser::afterAction(int owner, const QString &lead, Done finish)
{
    // What was clicked or sent may load a page, or change this one: a moment
    // for it to begin, then until the words have settled.
    QTimer::singleShot(700, this, [this, owner, lead, finish]() {
        waitLoaded(owner, [this, owner, lead, finish]() {
            readPage(owner, [this, owner, lead, finish](const QString &error) {
                if (!error.isEmpty()) {
                    finish(lead + tr("The page that came of it could not be read (") + error
                               + QStringLiteral(")."), QByteArray());
                    return;
                }
                finish(lead + report(owner), QByteArray());
            });
        });
    });
}

void WebBrowser::click(int owner, const QString &text, Done finish)
{
    evaluate(owner, QString::fromLatin1(kClickScript) + QLatin1Char('(') + js(text) + QLatin1Char(')'),
             [this, owner, text, finish](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) {
            finish(tr("Failed: the click could not be made (") + error + QStringLiteral(")."),
                   QByteArray());
            return;
        }
        const QJsonObject did = QJsonDocument::fromJson(value.toString().toUtf8()).object();
        if (!did.value(QStringLiteral("found")).toBool()) {
            finish(tr("Nothing to click on this page says '") + text.simplified().left(80)
                       + tr("'. Use links to see what it has, or screenshot to look at it."),
                   QByteArray());
            return;
        }
        const QString lead = tr("Clicked '") + did.value(QStringLiteral("label")).toString()
                             + QStringLiteral("'.\n");
        const QString href = did.value(QStringLiteral("href")).toString();
        liveLog(QStringLiteral("web: clicked '%1'").arg(did.value(QStringLiteral("label")).toString()));
        if (!href.isEmpty())
            goTo(owner, href, lead, finish);
        else
            afterAction(owner, lead, finish);
    });
}

void WebBrowser::type(int owner, const QString &field, const QString &text, bool submit, Done finish)
{
    evaluate(owner, QString::fromLatin1(kTypeScript) + QLatin1Char('(') + js(field) + QLatin1Char(',')
                        + js(text) + QLatin1Char(')'),
             [this, owner, field, submit, finish](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) {
            finish(tr("Failed: that could not be typed (") + error + QStringLiteral(")."), QByteArray());
            return;
        }
        const QJsonObject did = QJsonDocument::fromJson(value.toString().toUtf8()).object();
        if (!did.value(QStringLiteral("found")).toBool()) {
            QStringList fields;
            for (const QJsonValue &one : did.value(QStringLiteral("fields")).toArray())
                fields.append(one.toString());
            finish((field.trimmed().isEmpty() ? tr("This page has no text field to type into.")
                                              : tr("No text field on this page matches '")
                                                    + field.simplified().left(60) + QStringLiteral("'."))
                       + (fields.isEmpty() ? QString()
                                           : tr(" Its text fields: ") + fields.join(QStringLiteral("; "))
                                                 + QLatin1Char('.'))
                       + tr(" The hidden browser never types into a password field."),
                   QByteArray());
            return;
        }
        const QString lead = tr("Typed into '") + did.value(QStringLiteral("label")).toString()
                             + QStringLiteral("'");
        if (!submit) {
            finish(lead + tr(". Nothing has been sent: type again with submit true to press Enter, "
                             "or click the button."), QByteArray());
            return;
        }
        const Tab *page = tab(owner);
        if (!page) {
            finish(tr("Failed: that page is gone."), QByteArray());
            return;
        }
        // Enter, as a key: the field has the focus.
        QJsonObject key{{QStringLiteral("type"), QStringLiteral("keyDown")},
                        {QStringLiteral("key"), QStringLiteral("Enter")},
                        {QStringLiteral("code"), QStringLiteral("Enter")},
                        {QStringLiteral("windowsVirtualKeyCode"), 13},
                        {QStringLiteral("nativeVirtualKeyCode"), 13},
                        {QStringLiteral("text"), QStringLiteral("\r")}};
        send(QStringLiteral("Input.dispatchKeyEvent"), key, page->sessionId, nullptr);
        key.insert(QStringLiteral("type"), QStringLiteral("keyUp"));
        key.remove(QStringLiteral("text"));
        send(QStringLiteral("Input.dispatchKeyEvent"), key, page->sessionId, nullptr);
        afterAction(owner, lead + tr(" and pressed Enter.\n"), finish);
    });
}

void WebBrowser::back(int owner, Done finish)
{
    const Tab *page = tab(owner);
    if (!page) {
        finish(tr("Failed: that page is gone."), QByteArray());
        return;
    }
    const QString session = page->sessionId;
    send(QStringLiteral("Page.getNavigationHistory"), QJsonObject(), session,
         [this, owner, session, finish](const QJsonObject &history, const QString &error) {
        const int at = history.value(QStringLiteral("currentIndex")).toInt();
        const QJsonArray entries = history.value(QStringLiteral("entries")).toArray();
        // Entry 0 is the empty page the tab began on.
        if (!error.isEmpty() || at <= 1 || at >= entries.size()) {
            finish(tr("There is no page before this one to go back to."), QByteArray());
            return;
        }
        send(QStringLiteral("Page.navigateToHistoryEntry"),
             QJsonObject{{QStringLiteral("entryId"),
                          entries.at(at - 1).toObject().value(QStringLiteral("id"))}},
             session, [this, owner, finish](const QJsonObject &, const QString &) {
            afterAction(owner, tr("Went back.\n"), finish);
        });
    });
}

void WebBrowser::links(int owner, const QString &query, Done finish)
{
    evaluate(owner, QString::fromLatin1(kLinksScript),
             [query, finish](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) {
            finish(tr("Failed: the page's links could not be read (") + error + QStringLiteral(")."),
                   QByteArray());
            return;
        }
        const QJsonArray all = QJsonDocument::fromJson(value.toString().toUtf8()).array();
        const QStringList words = query.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QStringList lines;
        int matching = 0;
        for (const QJsonValue &one : all) {
            const QJsonArray link = one.toArray();
            const QString label = link.at(0).toString();
            const QString href = link.at(1).toString();
            const QString both = (label + QLatin1Char(' ') + href).toLower();
            bool has = true;
            for (const QString &word : words)
                has = has && both.contains(word);
            if (!has)
                continue;
            if (++matching <= 60)
                lines.append(label + QStringLiteral(" -> ") + href);
        }
        if (lines.isEmpty()) {
            finish(words.isEmpty() ? tr("This page has no links.")
                                   : tr("None of this page's %1 links has those words. Call links "
                                        "with no query to see them all.").arg(all.size()),
                   QByteArray());
            return;
        }
        finish(tr("%1 of this page's %2 links%3:\n")
                       .arg(lines.size()).arg(all.size())
                       .arg(matching > lines.size() ? tr(" (the first of %1 that match - give query "
                                                         "words to narrow them)").arg(matching)
                                                    : QString())
                   + lines.join(QLatin1Char('\n')),
               QByteArray());
    });
}

void WebBrowser::screenshot(int owner, Done finish)
{
    const Tab *page = tab(owner);
    if (!page) {
        finish(tr("Failed: that page is gone."), QByteArray());
        return;
    }
    const QString what = page->title.simplified().left(120) + QStringLiteral(" - ") + page->url;
    send(QStringLiteral("Page.captureScreenshot"),
         QJsonObject{{QStringLiteral("format"), QStringLiteral("png")}}, page->sessionId,
         [what, finish](const QJsonObject &shot, const QString &error) {
        const QByteArray png = QByteArray::fromBase64(shot.value(QStringLiteral("data")).toString().toLatin1());
        if (!error.isEmpty() || png.isEmpty()) {
            finish(tr("Failed: no picture of the page could be taken (") + error + QStringLiteral(")."),
                   QByteArray());
            return;
        }
        finish(tr("A picture of the page in the hidden browser, the top of it: ") + what, png);
    });
}

// ===========================================================================
// THE PROBE
// ===========================================================================

int runWebProbe(const QStringList &script, const QString &outPath)
{
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 1;
    QStringList lines;
    for (const QString &raw : script) {
        const QString line = raw.trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#')))
            lines.append(line);
    }

    WebBrowser web;
    QEventLoop loop;
    int at = 0;
    int failures = 0;
    std::function<void()> next = [&]() {
        if (at >= lines.size()) {
            loop.quit();
            return;
        }
        const QString line = lines.at(at++);
        const int space = int(line.indexOf(QLatin1Char(' ')));
        const QString action = (space < 0 ? line : line.left(space)).toLower();
        QString rest = space < 0 ? QString() : line.mid(space + 1).trimmed();
        if (action == QLatin1String("wait")) {
            QTimer::singleShot(rest.toInt(), &loop, [&next]() { next(); });
            return;
        }
        // release: the tab is closed, as when a task ends.
        if (action == QLatin1String("release")) {
            web.release(1);
            out.write("### release\n\n");
            QTimer::singleShot(0, &loop, [&next]() { next(); });
            return;
        }
        // parallel a b c: each address on a tab of its own, all at once, as
        // three tasks would.
        if (action == QLatin1String("parallel")) {
            const QStringList urls = rest.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (urls.isEmpty()) {
                QTimer::singleShot(0, &loop, [&next]() { next(); });
                return;
            }
            out.write((QStringLiteral("### ") + line + QLatin1Char('\n')).toUtf8());
            auto left = std::make_shared<int>(int(urls.size()));
            auto clock = std::make_shared<QElapsedTimer>();
            clock->start();
            for (int i = 0; i < urls.size(); ++i) {
                const int owner = 2 + i;
                web.run(owner, QVariantMap{{QStringLiteral("action"), QStringLiteral("open")},
                                           {QStringLiteral("url"), urls.at(i)}},
                        [&, owner, left, clock](const QString &text, const QByteArray &) {
                    out.write((QStringLiteral("tab %1 after %2 ms: ").arg(owner).arg(clock->elapsed())
                               + text.left(170).simplified() + QLatin1Char('\n')).toUtf8());
                    if (text.startsWith(QLatin1String("Failed")))
                        ++failures;
                    web.release(owner);
                    if (--*left == 0) {
                        out.write("\n");
                        out.flush();
                        next();
                    }
                });
            }
            return;
        }
        QVariantMap args{{QStringLiteral("action"), action}};
        if (action == QLatin1String("search") || action == QLatin1String("links")) {
            args.insert(QStringLiteral("query"), rest);
        } else if (action == QLatin1String("open")) {
            args.insert(QStringLiteral("url"), rest);
        } else if (action == QLatin1String("type")) {
            // type field = words   and a ! at the end presses Enter
            if (rest.endsWith(QLatin1String(" !"))) {
                args.insert(QStringLiteral("submit"), true);
                rest.chop(2);
            }
            const int equals = int(rest.indexOf(QLatin1Char('=')));
            args.insert(QStringLiteral("field"), equals < 0 ? QString() : rest.left(equals).trimmed());
            args.insert(QStringLiteral("text"), (equals < 0 ? rest : rest.mid(equals + 1)).trimmed());
        } else {
            args.insert(QStringLiteral("text"), rest);
        }
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        web.run(1, args, [&, line, clock](const QString &text, const QByteArray &png) {
            out.write((QStringLiteral("### ") + line + QStringLiteral("   (")
                       + QString::number(clock->elapsed()) + QStringLiteral(" ms, ")
                       + QString::number(text.size()) + QStringLiteral(" chars)\n") + text
                       + QLatin1Char('\n')).toUtf8());
            if (!png.isEmpty()) {
                QFile shot(outPath + QStringLiteral(".png"));
                if (shot.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    shot.write(png);
                out.write("(a picture of " + QByteArray::number(png.size()) + " bytes, saved beside this file)\n");
            }
            out.write("\n");
            out.flush();
            if (text.startsWith(QLatin1String("Failed")))
                ++failures;
            next();
        });
    };
    QTimer::singleShot(0, &loop, [&next]() { next(); });
    loop.exec();
    web.release(1);
    web.shutDown();
    out.write("### END, " + QByteArray::number(failures) + " failed\n");
    out.close();
    return failures ? 2 : 0;
}

} // namespace BurnOS
