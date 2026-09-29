#ifndef INTEGRATIONSDIALOG_H
#define INTEGRATIONSDIALOG_H

#include "composio.h"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QString>

class ComposioClient;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;

// Manage integrations: the one place the orb's reach into other people's
// services is set up. Reached from Settings, because it is a page of its own -
// a searchable catalogue of fifteen hundred apps has no business as a row in a
// form of eight.
//
// WHAT IT DOES: holds the Composio key, searches the app catalogue, and
// connects or disconnects one app at a time. The sign in itself happens in the
// user's own browser and finishes out of band, so after sending them there this
// dialog simply keeps asking Composio whether the connection has appeared yet.
class IntegrationsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit IntegrationsDialog(QWidget *parent = nullptr);

private:
    // The key is saved the moment it stops being edited, not on a Save button:
    // nothing else in this dialog works until it is stored, so waiting would
    // mean showing an empty catalogue and an error for no reason.
    void keyEdited();
    void search();
    void repaintList();
    void connectSelected();
    void disconnectSelected();
    void selectionChanged();
    void say(const QString &message, bool isError = false);

    // What a person should see for an app we only know the slug of, because the
    // connections endpoint hands back slugs and nothing else.
    QString displayName(const QString &slug) const;

    ComposioClient *m_composio = nullptr;

    QLineEdit *m_key = nullptr;
    QCheckBox *m_showKey = nullptr;
    QLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    QPushButton *m_connect = nullptr;
    QPushButton *m_disconnect = nullptr;
    QPushButton *m_refresh = nullptr;
    QLabel *m_status = nullptr;

    // Typing in the search box is not a request per keystroke. This collapses a
    // typed word into one lookup.
    QTimer *m_typing = nullptr;
    // After the browser has been sent to a sign in page, ask Composio every few
    // seconds whether it finished. Gives up rather than polling for ever.
    QTimer *m_poll = nullptr;
    int m_polls = 0;
    QString m_awaiting;   // slug we are waiting to see appear

    // The catalogue as last searched, and every connection this install has.
    // Kept apart because they come from two different endpoints and the list on
    // screen is the two of them merged.
    QList<ComposioApp> m_catalogue;
    QHash<QString, ComposioApp> m_connected;   // by slug
    // Slug to proper name, learned from every catalogue result that goes past.
    QHash<QString, QString> m_names;
    // True once the catalogue has answered at least once, so "nothing matched"
    // is only ever said about a search that really came back empty.
    bool m_catalogueLoaded = false;
};

#endif // INTEGRATIONSDIALOG_H
