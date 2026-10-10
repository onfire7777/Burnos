#include "settings.h"

#include <QByteArray>
#include <QSettings>
#include <QUuid>
#include <QtGlobal>

#include <windows.h>
#include <wincrypt.h>

namespace BurnOS {
namespace Settings {
namespace {

constexpr auto kOrg     = "BurnOS";
constexpr auto kApp     = "BurnOS";
constexpr auto kKeyPath = "openrouter/key";

// Everything else the settings dialog can change. Only the key is encrypted:
// the rest is plain preference, worth nothing to anyone reading the registry.
constexpr auto kModelPath  = "openrouter/model";
// Since v0.5. Absent means tasks work on the talking model above.
constexpr auto kTaskModelPath = "openrouter/taskModel";
constexpr auto kTokensPath = "openrouter/maxTokens";
constexpr auto kHangoverPath = "stt/hangoverMs";
constexpr auto kSensitivityPath = "stt/sensitivity";
constexpr auto kDictateKeyPath = "stt/transcriptionKey";
constexpr auto kDictateCodePath = "stt/transcriptionKeyCode";
constexpr auto kVoicePath  = "tts/voice";
constexpr auto kVolumePath = "tts/volume";
constexpr auto kOnTopPath  = "ui/alwaysOnTop";
constexpr auto kOrbSizePath = "ui/orbSize";
constexpr auto kOrbPosPath  = "ui/orbPosition";

// Composio: the key is encrypted like the OpenRouter one, the other two are
// plain - a user id we made up and a list of app names.
constexpr auto kComposioKeyPath  = "composio/key";
constexpr auto kComposioUserPath = "composio/userId";
constexpr auto kComposioAppsPath = "composio/apps";
constexpr auto kComposioSessionPath = "composio/sessionId";

// The talking model when none is chosen: quick, cheap, and able to do the work
// as well, so an install with nothing set still does everything. The choices
// and their timings are in settingsdialog.cpp; this is only the fallback.
constexpr auto kDefaultModel = "xiaomi/mimo-v2.6-flash";
constexpr int kDefaultTokens = 1200;
constexpr int kMinTokens     = 256;
constexpr int kMaxTokens     = 4000;
constexpr int kMaxVoice      = 10; // eleven speakers, numbered from zero

// A second of quiet ends a phrase. It was 650 ms and that caught people mid
// thought: a natural pause for breath was being read as the end of the
// question. Anything under a third of a second would fire between two words.
// 90 works out at an open threshold of 0.014. Measured on this machine: the
// room floor sits around 0.0005 and ordinary speech well above 0.02, so this
// is roughly thirty times the noise and still catches a quiet voice. It was
// briefly 0.030 and the user had to shout.
constexpr int kDefaultSensitivity = 90;

constexpr int kDefaultHangover = 1000;
constexpr int kMinHangover     = 300;
constexpr int kMaxHangover     = 3000;

// The orb's idle diameter. 210 is what it shipped as. The lower bound is set
// by the answer, which is painted inside the orb and needs room to wrap; the
// upper bound is taste.
constexpr int kDefaultOrbSize = 210;
constexpr int kMinOrbSize     = 140;
constexpr int kMaxOrbSize     = 320;

QSettings store()
{
    // Per user, under HKCU. An API key has no business in the machine wide
    // ProgramData tree, which every account on the box can read.
    return QSettings(QString::fromLatin1(kOrg), QString::fromLatin1(kApp));
}

// Windows DPAPI. The ciphertext is bound to this Windows user, so copying
// the registry value to another account or machine yields nothing useful.
QByteArray protect(const QByteArray &plain)
{
    if (plain.isEmpty())
        return {};

    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()));
    in.cbData = static_cast<DWORD>(plain.size());

    DATA_BLOB out{};
    if (!CryptProtectData(&in, const_cast<wchar_t *>(L"BurnOS OpenRouter key"),
                          nullptr, nullptr, nullptr, 0, &out))
        return {};

    const QByteArray cipher(reinterpret_cast<const char *>(out.pbData),
                            static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return cipher;
}

QByteArray unprotect(const QByteArray &cipher)
{
    if (cipher.isEmpty())
        return {};

    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(cipher.constData()));
    in.cbData = static_cast<DWORD>(cipher.size());

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
        return {};

    const QByteArray plain(reinterpret_cast<const char *>(out.pbData),
                           static_cast<int>(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

} // namespace

QString openRouterKey()
{
    const QByteArray cipher = store().value(QString::fromLatin1(kKeyPath))
                                  .toByteArray();
    return QString::fromUtf8(unprotect(cipher));
}

void setOpenRouterKey(const QString &key)
{
    QSettings s = store();
    const QString trimmed = key.trimmed();

    if (trimmed.isEmpty()) {
        s.remove(QString::fromLatin1(kKeyPath));
    } else {
        const QByteArray cipher = protect(trimmed.toUtf8());
        if (!cipher.isEmpty())
            s.setValue(QString::fromLatin1(kKeyPath), cipher);
    }
    s.sync();
}

bool hasOpenRouterKey()
{
    return !store().value(QString::fromLatin1(kKeyPath)).toByteArray().isEmpty();
}

QString defaultOpenRouterModel()
{
    return QString::fromLatin1(kDefaultModel);
}

QString openRouterModel()
{
    const QString stored =
        store().value(QString::fromLatin1(kModelPath)).toString().trimmed();
    return stored.isEmpty() ? defaultOpenRouterModel() : stored;
}

void setOpenRouterModel(const QString &model)
{
    QSettings s = store();
    const QString trimmed = model.trimmed();

    // NOTHING IS STORED WHEN IT EQUALS THE SHIPPED DEFAULT, on purpose. Writing
    // it down would freeze this install on today's choice: a later build that
    // moves to a better default would be silently overruled by a registry value
    // the user never knowingly set.
    if (trimmed.isEmpty() || trimmed == defaultOpenRouterModel())
        s.remove(QString::fromLatin1(kModelPath));
    else
        s.setValue(QString::fromLatin1(kModelPath), trimmed);
    s.sync();
}

QString taskModel()
{
    const QString stored =
        store().value(QString::fromLatin1(kTaskModelPath)).toString().trimmed();
    return stored.isEmpty() ? openRouterModel() : stored;
}

bool hasTaskModel()
{
    return !store().value(QString::fromLatin1(kTaskModelPath)).toString().trimmed().isEmpty();
}

void setTaskModel(const QString &model)
{
    // Stored exactly as chosen, even when it happens to be the default: there
    // is no shipped working model to follow, and "same as talking" is said by
    // storing nothing at all.
    QSettings s = store();
    const QString trimmed = model.trimmed();
    if (trimmed.isEmpty())
        s.remove(QString::fromLatin1(kTaskModelPath));
    else
        s.setValue(QString::fromLatin1(kTaskModelPath), trimmed);
    s.sync();
}

int replyTokens()
{
    const int tokens =
        store().value(QString::fromLatin1(kTokensPath), kDefaultTokens).toInt();
    return qBound(kMinTokens, tokens, kMaxTokens);
}

void setReplyTokens(int tokens)
{
    QSettings s = store();
    s.setValue(QString::fromLatin1(kTokensPath),
               qBound(kMinTokens, tokens, kMaxTokens));
    s.sync();
}

int micSensitivity()
{
    return qBound(0, store().value(QString::fromLatin1(kSensitivityPath),
                                   kDefaultSensitivity).toInt(), 100);
}

void setMicSensitivity(int percent)
{
    QSettings s = store();
    s.setValue(QString::fromLatin1(kSensitivityPath), qBound(0, percent, 100));
    s.sync();
}

double vadThreshold()
{
    // 0 -> 0.85 (only unmistakable speech), 100 -> 0.35 (anything that is
    // probably a voice). Silero's own default of 0.5 sits at 70. Noise is not
    // what this has to fight any more - the network barely scores a keyboard
    // or a door at all - so the slider can sit high, as it does by default.
    return 0.85 - 0.50 * (micSensitivity() / 100.0);
}

bool hasOrbPosition()
{
    // Asked separately, because 0,0 is a perfectly good place to leave the orb
    // and cannot be used to mean "never moved".
    return store().value(QString::fromLatin1(kOrbPosPath)).isValid();
}

QPoint orbPosition()
{
    return store().value(QString::fromLatin1(kOrbPosPath)).toPoint();
}

void setOrbPosition(const QPoint &topLeft)
{
    QSettings s = store();
    s.setValue(QString::fromLatin1(kOrbPosPath), topLeft);
    s.sync();
}

int orbSize()
{
    const int px = store().value(QString::fromLatin1(kOrbSizePath),
                                 kDefaultOrbSize).toInt();
    return qBound(kMinOrbSize, px, kMaxOrbSize);
}

void setOrbSize(int px)
{
    QSettings s = store();
    s.setValue(QString::fromLatin1(kOrbSizePath),
               qBound(kMinOrbSize, px, kMaxOrbSize));
    s.sync();
}

QString transcriptionKey()
{
    return transcriptionKeyCode()
               ? store().value(QString::fromLatin1(kDictateKeyPath)).toString()
               : QString();
}

quint32 transcriptionKeyCode()
{
    return store().value(QString::fromLatin1(kDictateCodePath), 0).toUInt();
}

void setTranscriptionKey(const QString &shown, quint32 code)
{
    QSettings s = store();
    s.setValue(QString::fromLatin1(kDictateKeyPath), shown);
    s.setValue(QString::fromLatin1(kDictateCodePath), code);
    s.sync();
}

int hangoverMs()
{
    const int ms = store().value(QString::fromLatin1(kHangoverPath),
                                 kDefaultHangover).toInt();
    return qBound(kMinHangover, ms, kMaxHangover);
}

void setHangoverMs(int ms)
{
    QSettings s = store();
    s.setValue(QString::fromLatin1(kHangoverPath),
               qBound(kMinHangover, ms, kMaxHangover));
    s.sync();
}

int ttsVoice()
{
    return qBound(0, store().value(QString::fromLatin1(kVoicePath), 0).toInt(),
                  kMaxVoice);
}

void setTtsVoice(int voice)
{
    QSettings s = store();
    s.setValue(QString::fromLatin1(kVoicePath), qBound(0, voice, kMaxVoice));
    s.sync();
}

int ttsVolume()
{
    return qBound(0,
                  store().value(QString::fromLatin1(kVolumePath), 100).toInt(),
                  100);
}

void setTtsVolume(int percent)
{
    QSettings s = store();
    s.setValue(QString::fromLatin1(kVolumePath), qBound(0, percent, 100));
    s.sync();
}

QString composioKey()
{
    const QByteArray cipher =
        store().value(QString::fromLatin1(kComposioKeyPath)).toByteArray();
    return QString::fromUtf8(unprotect(cipher));
}

void setComposioKey(const QString &key)
{
    QSettings s = store();
    const QString trimmed = key.trimmed();

    // A session belongs to the project its key came from, so a new key means
    // a new session, made on first use.
    s.remove(QString::fromLatin1(kComposioSessionPath));

    if (trimmed.isEmpty()) {
        s.remove(QString::fromLatin1(kComposioKeyPath));
    } else {
        const QByteArray cipher = protect(trimmed.toUtf8());
        if (!cipher.isEmpty())
            s.setValue(QString::fromLatin1(kComposioKeyPath), cipher);
    }
    s.sync();
}

bool hasComposioKey()
{
    return !store().value(QString::fromLatin1(kComposioKeyPath))
                .toByteArray().isEmpty();
}

QString composioUserId()
{
    QSettings s = store();
    QString id = s.value(QString::fromLatin1(kComposioUserPath)).toString();
    if (!id.isEmpty())
        return id;

    // Minted once, on the first call, and kept for the life of the install.
    // Every connected account Composio holds hangs off this string, so losing
    // it means every app has to be connected again.
    id = QStringLiteral("burnos-")
         + QUuid::createUuid().toString(QUuid::Id128).left(12);
    s.setValue(QString::fromLatin1(kComposioUserPath), id);
    s.sync();
    return id;
}

QStringList composioApps()
{
    return store().value(QString::fromLatin1(kComposioAppsPath)).toStringList();
}

void setComposioApps(const QStringList &slugs)
{
    QSettings s = store();
    if (slugs.isEmpty())
        s.remove(QString::fromLatin1(kComposioAppsPath));
    else
        s.setValue(QString::fromLatin1(kComposioAppsPath), slugs);
    s.sync();
}

QString composioSessionId()
{
    return store().value(QString::fromLatin1(kComposioSessionPath)).toString();
}

void setComposioSessionId(const QString &id)
{
    QSettings s = store();
    if (id.isEmpty())
        s.remove(QString::fromLatin1(kComposioSessionPath));
    else
        s.setValue(QString::fromLatin1(kComposioSessionPath), id);
    s.sync();
}

bool alwaysOnTop()
{
    return store().value(QString::fromLatin1(kOnTopPath), true).toBool();
}

void setAlwaysOnTop(bool onTop)
{
    QSettings s = store();
    s.setValue(QString::fromLatin1(kOnTopPath), onTop);
    s.sync();
}

} // namespace Settings
} // namespace BurnOS
