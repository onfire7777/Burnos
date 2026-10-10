#ifndef STTENGINE_H
#define STTENGINE_H

#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

#include <memory>
#include <vector>

class QAudioSource;
class QProcess;

namespace BurnOS {
class SileroVad;
}

// Sits between the microphone and the engine, so the orb can be handed how
// loud the room is on the way past.
class AudioLevelTap : public QIODevice
{
    Q_OBJECT

public:
    explicit AudioLevelTap(QObject *parent = nullptr);

signals:
    void levelChanged(qreal level); // 0.0 silent, 1.0 loud, for the orb
    void audioChunk(const QByteArray &pcm);

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 size) override;
};

// Live mode. The microphone is held open, speech starts the recording by
// itself and a short silence ends it; the phrase is then handed to the bundled
// parakeet-cli.exe. Nothing here blocks: the recogniser is a child process and
// answers by signal.
class SttEngine : public QObject
{
    Q_OBJECT

public:
    explicit SttEngine(QObject *parent = nullptr);
    ~SttEngine() override;

    bool isLive() const { return m_live; }           // armed
    bool isCapturing() const { return m_capturing; } // keeping a phrase
    bool isReading() const { return m_pending; }     // a phrase is in the recogniser

    // Arm live mode and open the microphone. False means it never opened and
    // failed() has been emitted.
    bool startLive();
    // Disarm: close the microphone and throw away anything half heard.
    void stopLive();
    // Close the microphone but stay armed, so our own answer coming out of the
    // speakers can never be heard as the next question.
    void pauseListening();
    // Open it again after a turn, if live mode is still armed.
    bool resumeListening();

    // TALKING OVER THE VOICE. While the orb speaks, the microphone stays open
    // and hears the orb's own voice come back from the speakers. That echo is
    // learned, and only speech clearly louder than it - somebody talking into
    // the microphone - counts: then bargeIn() fires and a phrase starts. On a
    // setup where the echo is loud, nobody can be louder and this simply never
    // fires, which is exactly how v0.3 behaved.
    void setOverVoice(bool on);

    // PUSH TO TALK. From beginHeld() to endHeld() everything the microphone
    // hears is one phrase, whatever the voice detector makes of it, and it
    // comes back through dictated() instead of transcribed(). It works with
    // live mode off too: the microphone is opened for the hold and shut after
    // it. endHeld() says whether a phrase went to the recogniser.
    bool beginHeld();
    bool endHeld();

    // THE CHECK on the phrase logic: the 16 kHz mono WAV named by
    // BURNOS_VAD_SELFTEST is fed through it as if it were the microphone -
    // with BURNOS_VAD_OVERVOICE, as if the orb were talking over it. Never in
    // a normal run.
    void selfTest();

    // The voice detector on its own, for proving it against the official
    // model: every 32 ms of a 16 kHz mono WAV, one speech probability per line.
    // Run as Burnos.exe --vad-probe in.wav out.txt, never in normal use.
    static bool probeFile(const QString &wavPath, const QString &outPath,
                          QString *error);

signals:
    void transcribed(const QString &text);
    // The same, for a phrase taken between beginHeld() and endHeld().
    void dictated(const QString &text);
    void failed(const QString &reason);
    void levelChanged(qreal level);
    void speechStarted();
    void speechEnded();
    // The user started talking over the orb's voice. Stop the voice now.
    void bargeIn();

private:
    QString cliPath() const;
    bool openMicrophone();
    void closeMicrophone();
    void onAudio(const QByteArray &pcm);
    // One Silero verdict on 32 ms of audio, with that audio's loudness: start,
    // carry on or end a phrase - or, over the voice, learn the echo.
    void decide(float speech, float rms);
    // The phrase is over: hand it to the recogniser, or drop it if it was too
    // short to be anybody talking.
    void endPhrase(bool tooLong, int hangoverMs);
    // Raw 16 kHz mono 16 bit samples out of a WAV, for the self test and probe.
    static QByteArray readPcm(const QString &wavPath, QString *error);
    void transcribe(const QByteArray &pcm, bool held = false);
    void drainQueue();
    bool writeWav(const QByteArray &pcm, const QString &wavPath,
                  QString *error) const;

    QAudioSource *m_source = nullptr;
    AudioLevelTap *m_tap = nullptr;
    QProcess *m_proc = nullptr;
    QString m_wavPath;

    bool m_live = false;      // live mode armed by the user
    bool m_capturing = false; // a phrase is being kept right now
    bool m_pending = false;   // a phrase is inside the recogniser right now
    bool m_pendingHeld = false; // and it was taken by push to talk
    bool m_held = false;        // push to talk is down
    bool m_heldOwnsMic = false; // the microphone was opened just for this hold
    QByteArray m_heldPcm;       // what has been said while it is down
    // Phrases heard while the recogniser was busy, oldest first. The microphone
    // no longer closes while we work, so this can happen whenever somebody
    // carries on talking.
    QList<QPair<QByteArray, bool>> m_queue; // each with whether it was held
    QByteArray m_speech;      // the phrase being kept
    QByteArray m_preroll;     // the moment before it, so no syllable is lost
    qint64 m_silenceBytes = 0;
    qint64 m_voicedBytes = 0;
    qint64 m_attackBytes = 0; // how long it has sounded like speech, not yet a phrase
    qint64 m_idleBytes = 0;   // quiet audio since the last trace line
    int m_beats = 0;          // how many of those lines were written

    // The Silero voice detector, and the samples waiting for a full 32 ms.
    std::unique_ptr<BurnOS::SileroVad> m_vad;
    std::vector<float> m_vadIn;

    // Over the voice: how loud the orb's own voice comes back, held at its
    // peaks and kept for the life of the process, how much of it has been
    // heard, and how long the current voice has been playing.
    bool m_overVoice = false;
    float m_echoLevel = 0.004f;
    int m_echoSteps = 0;
    int m_bargeSteps = 0;
    qint64 m_overVoiceBytes = 0;
};

#endif // STTENGINE_H
