#include "livelog.h"
#include "modelcatalogue.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

// Public. No key, no account, no rate limit worth worrying about for one call
// a run - which is why this can be asked before the user has typed a key in.
constexpr auto kCatalogueUrl = "https://openrouter.ai/api/v1/models";

// Long enough for four hundred models on a slow line, short enough that a dead
// network does not leave the settings dialog saying "checking" for ever.
constexpr int kTimeoutMs = 15000;

} // namespace

namespace BurnOS {

ModelCatalogue *ModelCatalogue::instance()
{
    static ModelCatalogue catalogue;
    return &catalogue;
}

ModelCatalogue::ModelCatalogue(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

void ModelCatalogue::load()
{
    if (m_loaded || m_loading)
        return;
    m_loading = true;

    QNetworkRequest request{QUrl(QString::fromLatin1(kCatalogueUrl))};
    request.setRawHeader("X-Title", "Burnos");
    request.setTransferTimeout(kTimeoutMs);

    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_loading = false;

        if (reply->error() != QNetworkReply::NoError) {
            // BEING OFFLINE IS NOT A REASON TO OVERRULE THE USER. Nothing is
            // known, so nothing is refused, and the orb runs on whatever they
            // chose exactly as it did before this existed.
            liveLog(QStringLiteral("model catalogue unavailable: %1")
                        .arg(reply->errorString()));
            return;
        }

        const QJsonArray data = QJsonDocument::fromJson(reply->readAll())
                                    .object()
                                    .value(QStringLiteral("data"))
                                    .toArray();

        int able = 0;
        for (const QJsonValue &value : data) {
            const QJsonObject model = value.toObject();

            ModelFacts f;
            f.slug = model.value(QStringLiteral("id")).toString();
            if (f.slug.isEmpty())
                continue;
            f.name = model.value(QStringLiteral("name")).toString();

            const QJsonArray inputs = model.value(QStringLiteral("architecture"))
                                          .toObject()
                                          .value(QStringLiteral("input_modalities"))
                                          .toArray();
            for (const QJsonValue &mode : inputs) {
                if (mode.toString() == QLatin1String("image"))
                    f.seesImages = true;
            }

            const QJsonArray params =
                model.value(QStringLiteral("supported_parameters")).toArray();
            for (const QJsonValue &parameter : params) {
                if (parameter.toString() == QLatin1String("tools"))
                    f.callsTools = true;
            }

            if (f.canDoTheJob())
                ++able;
            m_facts.insert(f.slug, f);
        }

        m_loaded = !m_facts.isEmpty();
        liveLog(QStringLiteral("model catalogue: %1 models, %2 can both see the "
                               "screen and call tools")
                    .arg(m_facts.size())
                    .arg(able));
        if (m_loaded)
            emit loaded();
    });
}

bool ModelCatalogue::knows(const QString &slug) const
{
    return m_facts.contains(slug.trimmed());
}

ModelFacts ModelCatalogue::facts(const QString &slug) const
{
    return m_facts.value(slug.trimmed());
}

bool ModelCatalogue::fitToRun(const QString &slug) const
{
    const QString wanted = slug.trimmed();
    const auto found = m_facts.constFind(wanted);
    if (found == m_facts.cend())
        return true;   // never heard of it, so not ours to refuse
    return found.value().canDoTheJob();
}

QStringList ModelCatalogue::capable() const
{
    QStringList slugs;
    for (auto it = m_facts.cbegin(); it != m_facts.cend(); ++it) {
        if (it.value().canDoTheJob())
            slugs.append(it.key());
    }
    slugs.sort();
    return slugs;
}

} // namespace BurnOS
