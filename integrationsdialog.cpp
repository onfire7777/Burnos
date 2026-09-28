#include "composio.h"
#include "integrationsdialog.h"
#include "settings.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Same palette as the settings dialog and the orb's own menu, with the list
// added. Two dialogs from one program must not look like two programs.
constexpr auto kStyle =
    "QDialog { background-color: #12161f; }"
    "QLabel { color: #d8e6f5; }"
    "QCheckBox { color: #d8e6f5; }"
    "QLineEdit { background-color: #0c1018; color: #d8e6f5;"
    " border: 1px solid #2c3648; border-radius: 3px; padding: 5px; }"
    "QListWidget { background-color: #0c1018; color: #d8e6f5;"
    " border: 1px solid #2c3648; border-radius: 3px; padding: 2px;"
    " outline: none; }"
    "QListWidget::item { padding: 5px 6px; border-radius: 3px; }"
    "QListWidget::item:selected { background-color: #2a3c5a; }"
    "QPushButton { background-color: #1b2331; color: #d8e6f5;"
    " border: 1px solid #2c3648; border-radius: 3px; padding: 5px 16px; }"
    "QPushButton:hover:enabled { background-color: #2a3c5a; }"
    "QPushButton:disabled { color: #566274; border-color: #1e2634; }";

// How long to keep asking whether a sign in finished, and how often. Two
// minutes is long enough for a password manager, a sign in and a consent
// screen; past that the user has plainly wandered off and Refresh is there.
constexpr int kPollMs = 4000;
constexpr int kMaxPolls = 30;

// Which role on a list item holds what. The slug is what every Composio call
// takes; the connection id is what disconnecting needs.
constexpr int kSlugRole = Qt::UserRole;
constexpr int kConnectionRole = Qt::UserRole + 1;
constexpr int kConnectedRole = Qt::UserRole + 2;

} // namespace

IntegrationsDialog::IntegrationsDialog(QWidget *parent)
    : QDialog(parent)
    , m_composio(new ComposioClient(this))
{
    setWindowTitle(tr("Manage Integrations"));
    setModal(true);
    setMinimumSize(540, 560);
    setStyleSheet(QString::fromLatin1(kStyle));

    // The orb stays on top, so a dialog without the same hint opens behind it
    // and looks like nothing happened.
    setWindowFlag(Qt::WindowStaysOnTopHint, true);

    auto *blurb = new QLabel(
        tr("Connect the apps you want Burnos to use. Signing in happens in your "
           "browser and Burnos never sees the password - Composio holds the "
           "connection and refreshes it. Ask for things out loud afterwards: "
           "\"email Sarah the notes\", \"post that in Slack\"."),
        this);
    blurb->setWordWrap(true);
    blurb->setStyleSheet(QStringLiteral("color: #8fa3bd;"));

    m_key = new QLineEdit(this);
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText(QStringLiteral("ak_..."));
    m_key->setToolTip(tr("Your Composio project API key, from Settings then "
                         "Project Settings on composio.dev. Stored encrypted "
                         "for this Windows user only."));
    connect(m_key, &QLineEdit::editingFinished, this, &IntegrationsDialog::keyEdited);

    m_showKey = new QCheckBox(tr("Show"), this);
    connect(m_showKey, &QCheckBox::toggled, this, [this](bool on) {
        m_key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });

    auto *keyRow = new QHBoxLayout;
    keyRow->setContentsMargins(0, 0, 0, 0);
    keyRow->addWidget(m_key, 1);
    keyRow->addWidget(m_showKey);

    auto *keyForm = new QFormLayout;
    keyForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    keyForm->setHorizontalSpacing(14);
    keyForm->addRow(tr("Composio key"), keyRow);

    m_search = new QLineEdit(this);
    m_search->setClearButtonEnabled(true);
    m_search->setPlaceholderText(tr("Search apps - Gmail, Slack, Notion, "
                                    "Calendar, GitHub..."));

    m_typing = new QTimer(this);
    m_typing->setSingleShot(true);
    m_typing->setInterval(350);
    connect(m_typing, &QTimer::timeout, this, &IntegrationsDialog::search);
    connect(m_search, &QLineEdit::textChanged, this, [this]() { m_typing->start(); });
    // Enter searches at once rather than waiting out the debounce.
    connect(m_search, &QLineEdit::returnPressed, this, [this]() {
        m_typing->stop();
        search();
    });

    m_list = new QListWidget(this);
    m_list->setAlternatingRowColors(false);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_list, &QListWidget::currentItemChanged,
            this, &IntegrationsDialog::selectionChanged);
    // Double clicking a row is the obvious way to connect it.
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item && !item->data(kConnectedRole).toBool())
            connectSelected();
    });

    m_connect = new QPushButton(tr("Connect"), this);
    m_connect->setEnabled(false);
    connect(m_connect, &QPushButton::clicked, this, &IntegrationsDialog::connectSelected);

    m_disconnect = new QPushButton(tr("Disconnect"), this);
    m_disconnect->setEnabled(false);
    connect(m_disconnect, &QPushButton::clicked,
            this, &IntegrationsDialog::disconnectSelected);

    m_refresh = new QPushButton(tr("Refresh"), this);
    connect(m_refresh, &QPushButton::clicked, this, [this]() {
        m_composio->refreshConnections();
        search();
    });

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->addWidget(m_connect);
    actions->addWidget(m_disconnect);
    actions->addWidget(m_refresh);
    actions->addStretch(1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(32);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(10);
    root->addWidget(blurb);
    root->addLayout(keyForm);
    root->addWidget(m_search);
    root->addWidget(m_list, 1);
    root->addLayout(actions);
    root->addWidget(m_status);
    root->addWidget(buttons);

    // Everything below is one of the two lists coming back, merged and redrawn.
    connect(m_composio, &ComposioClient::appsReady, this,
            [this](const QList<ComposioApp> &apps, bool connectedOnly) {
                if (connectedOnly) {
                    m_connected.clear();
                    for (const ComposioApp &app : apps) {
                        if (!app.connectionStatus.isEmpty())
                            m_connected.insert(app.slug, app);
                    }
                    // The sign in we sent them off to has landed.
                    if (!m_awaiting.isEmpty()
                        && m_connected.value(m_awaiting).isConnected()) {
                        say(tr("%1 is connected.").arg(displayName(m_awaiting)));
                        m_awaiting.clear();
                        if (m_poll)
                            m_poll->stop();
                    }
                } else {
                    m_catalogue = apps;
                    for (const ComposioApp &app : apps) {
                        if (!app.name.isEmpty())
                            m_names.insert(app.slug, app.name);
                    }
                }
                repaintList();
            });

    connect(m_composio, &ComposioClient::linkOpened, this, [this](const QString &slug) {
        m_awaiting = slug;
        m_polls = 0;
        say(tr("Sign in to %1 in the browser window that just opened. This will "
               "pick it up as soon as you have.").arg(displayName(slug)));
        if (!m_poll) {
            m_poll = new QTimer(this);
            m_poll->setInterval(kPollMs);
            connect(m_poll, &QTimer::timeout, this, [this]() {
                if (++m_polls > kMaxPolls || m_awaiting.isEmpty()) {
                    m_poll->stop();
                    if (!m_awaiting.isEmpty()) {
                        say(tr("Still not connected. Finish the sign in and press "
                               "Refresh."));
                        m_awaiting.clear();
                    }
                    return;
                }
                m_composio->refreshConnections();
            });
        }
        m_poll->start();
    });

    connect(m_composio, &ComposioClient::disconnected, this, [this](const QString &) {
        say(tr("Disconnected."));
        m_composio->refreshConnections();
    });

    connect(m_composio, &ComposioClient::failed, this, [this](const QString &reason) {
        say(reason, true);
    });

    // The key field is filled in behind a password mask, exactly as the
    // OpenRouter one is: shown to be corrected, never kept in a member.
    m_key->setText(BurnOS::Settings::composioKey());

    if (ComposioClient::isConfigured()) {
        m_composio->refreshConnections();
        search();
    } else {
        say(tr("Paste a Composio key above to see the apps you can connect. The "
               "free plan covers a hundred thousand actions a month."));
    }
}

void IntegrationsDialog::keyEdited()
{
    const QString typed = m_key->text().trimmed();
    if (typed == BurnOS::Settings::composioKey())
        return;

    BurnOS::Settings::setComposioKey(typed);
    if (typed.isEmpty()) {
        m_catalogue.clear();
        m_connected.clear();
        BurnOS::Settings::setComposioApps(QStringList());
        repaintList();
        say(tr("Key removed. Burnos will not reach any connected app now."));
        return;
    }

    say(tr("Key saved. Loading apps."));
    m_composio->refreshConnections();
    search();
}

void IntegrationsDialog::search()
{
    if (!ComposioClient::isConfigured())
        return;
    m_composio->searchApps(m_search->text(), 60);
}

void IntegrationsDialog::repaintList()
{
    const QString keep = m_list->currentItem()
                             ? m_list->currentItem()->data(kSlugRole).toString()
                             : QString();

    m_list->clear();

    // Connected first, always. They are the few rows that matter out of sixty,
    // and hunting for Gmail in an alphabet of apps nobody signed into is how a
    // settings page becomes something the user avoids opening.
    QStringList shown;
    auto addRow = [this, &shown](const ComposioApp &app, bool connected) {
        if (app.slug.isEmpty() || shown.contains(app.slug))
            return;
        shown.append(app.slug);

        const ComposioApp link = m_connected.value(app.slug);
        QString label = displayName(app.slug);
        if (connected)
            label += tr("   - connected");
        else if (link.isPending())
            label += tr("   - waiting for sign in");
        else if (app.noAuth)
            label += tr("   - no sign in needed");
        else if (!app.managedAuth)
            label += tr("   - needs its own credentials");

        auto *item = new QListWidgetItem(label, m_list);
        item->setData(kSlugRole, app.slug);
        item->setData(kConnectionRole, link.connectionId);
        item->setData(kConnectedRole, connected);
        if (!app.description.isEmpty())
            item->setToolTip(app.description);
        if (connected)
            item->setForeground(QColor(QStringLiteral("#7fd3a0")));
    };

    // A connected app is shown whatever the search says, because taking it away
    // has to stay possible without guessing its name back.
    for (auto it = m_connected.constBegin(); it != m_connected.constEnd(); ++it) {
        if (!it.value().isConnected())
            continue;
        ComposioApp app = it.value();
        app.name = displayName(app.slug);
        addRow(app, true);
    }

    for (const ComposioApp &app : std::as_const(m_catalogue))
        addRow(app, m_connected.value(app.slug).isConnected());

    if (m_list->count() == 0 && ComposioClient::isConfigured())
        say(tr("Nothing matched. Try the app's own name - Gmail rather than email."));

    if (!keep.isEmpty()) {
        for (int i = 0; i < m_list->count(); ++i) {
            if (m_list->item(i)->data(kSlugRole).toString() == keep) {
                m_list->setCurrentRow(i);
                break;
            }
        }
    }
    selectionChanged();
}

void IntegrationsDialog::selectionChanged()
{
    QListWidgetItem *item = m_list->currentItem();
    const bool connected = item && item->data(kConnectedRole).toBool();
    m_connect->setEnabled(item && !connected);
    m_disconnect->setEnabled(connected
                             && !item->data(kConnectionRole).toString().isEmpty());
}

void IntegrationsDialog::connectSelected()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    const QString slug = item->data(kSlugRole).toString();
    say(tr("Opening the sign in for %1.").arg(displayName(slug)));
    m_composio->connectApp(slug);
}

void IntegrationsDialog::disconnectSelected()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    const QString id = item->data(kConnectionRole).toString();
    if (id.isEmpty())
        return;
    say(tr("Disconnecting %1.").arg(displayName(item->data(kSlugRole).toString())));
    m_composio->disconnectApp(id);
}

void IntegrationsDialog::say(const QString &message, bool isError)
{
    m_status->setStyleSheet(isError ? QStringLiteral("color: #e58a8a;")
                                    : QStringLiteral("color: #8fa3bd;"));
    m_status->setText(message);
}

QString IntegrationsDialog::displayName(const QString &slug) const
{
    const QString known = m_names.value(slug);
    if (!known.isEmpty())
        return known;

    // The connections endpoint gives slugs and nothing else, so "google_calendar"
    // has to be made presentable without asking anybody.
    QString pretty = slug;
    pretty.replace(QLatin1Char('_'), QLatin1Char(' '));
    QStringList words = pretty.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (QString &word : words) {
        if (!word.isEmpty())
            word[0] = word.at(0).toUpper();
    }
    return words.join(QLatin1Char(' '));
}
