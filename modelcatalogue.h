#ifndef MODELCATALOGUE_H
#define MODELCATALOGUE_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;

namespace BurnOS {

// What OpenRouter itself says a model can take in and what it can be asked to
// do, read from the catalogue at https://openrouter.ai/api/v1/models - which
// needs no key. Fact, rather than a list somebody kept up to date by hand.
struct ModelFacts
{
    QString slug;
    QString name;
    bool seesImages = false;   // architecture.input_modalities holds "image"
    bool callsTools = false;   // supported_parameters holds "tools"

    // WHAT BURNOS ACTUALLY ASKS OF A MODEL. It operates the machine through a
    // tool call, and it checks its own work by looking at a screenshot, so a
    // model that cannot do both cannot do this job at all. This is not a
    // question of one model being weaker than another: OpenRouter refuses the
    // whole request outright, and the turn dies.
    bool canDoTheJob() const { return seesImages && callsTools; }

    // WHAT THE TALKING ASKS OF ONE, which is less. The talker is never sent a
    // picture - it hands anything that needs the screen to a task - but it does
    // call tools: to start that task, to steer or stop one, to remember. So a
    // fast model that only reads text can hold the conversation, PROVIDED a
    // separate working model does the work.
    bool canTalk() const { return callsTools; }
};

// The whole catalogue, fetched once per run.
//
// THIS EXISTS BECAUSE A MODEL WAS OFFERED THAT COULD NOT DO THE JOB. minimax
// m2.7 sat in the settings list recommended for driving the machine and it
// takes TEXT ONLY, so every turn that took a screenshot to check itself came
// back as a 404 from OpenRouter and the word Error on the orb. A shortlist
// written by hand has no way of knowing that. This asks.
class ModelCatalogue : public QObject
{
    Q_OBJECT

public:
    // One per process. Both the settings dialog and the window want the same
    // answers, and there is no sense fetching four hundred models twice.
    static ModelCatalogue *instance();

    // Fetches once. Calling it again once it has loaded does nothing.
    void load();

    bool isLoaded() const { return m_loaded; }

    // False for a model the catalogue has never heard of, which is NOT the same
    // thing as one it knows cannot do the job.
    bool knows(const QString &slug) const;
    ModelFacts facts(const QString &slug) const;

    // True unless the catalogue POSITIVELY KNOWS this model cannot do the job.
    // An unknown slug, and everything at all while the catalogue has not loaded,
    // is left alone: being offline must not take the user's own choice away.
    bool fitToRun(const QString &slug) const;
    // The same, for the lesser bar of the talking model alone.
    bool fitToTalk(const QString &slug) const;

    // Every model that can do the job, by slug, sorted.
    QStringList capable() const;

signals:
    void loaded();

private:
    explicit ModelCatalogue(QObject *parent = nullptr);

    QNetworkAccessManager *m_net = nullptr;
    QHash<QString, ModelFacts> m_facts;
    bool m_loaded = false;
    bool m_loading = false;
};

} // namespace BurnOS

#endif // MODELCATALOGUE_H
