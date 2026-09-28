#ifndef STTENGINE_H
#define STTENGINE_H

#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QObject>
#include <QString>

class QAudioSource;
class QProcess;

// Sits between the microphone and the engine so the same bytes can be measured
// on their way past: the orb needs the waveform to warp to, and the voice
// detector needs the loudness. Neither can be read back out of a file.
class AudioLevelTap : public QIODevice
{
    Q_OBJECT

public:
    explicit AudioLevelTap(QObject *parent = nullptr);

signals:
    void levelChanged(qreal level); // 0.0 silent, 1.0 loud, for the orb
    // The same buffer with its RMS, 0.0 to 1.0, for the detector.
    void audioChunk(const QByteArray &pcm, qreal rms);

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

    bool isLive() const { return m_live; }                 // armed
    bool isListening() const { return m_source != nullptr; } // mic open
    bool isCapturing() const { return m_capturing; }       // keeping a phrase

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

    // Proof without a microphone: synthetic tone then silence, fed straight
    // through the detector, so the thresholds and the 650 ms hangover can be
    // read back out of the log. Armed by BURNOS_VAD_SELFTEST in the
    // environment, never in a normal run.
    void selfTest();

signals:
    void transcribed(const QString &text);
    void failed(const QString &reason);
    void levelChanged(qreal level);
    void speechStarted();
    void speechEnded();

private:
    QString cliPath() const;
    bool openMicrophone();
    void closeMicrophone();
    void onAudio(const QByteArray &pcm, qreal rms);
    void transcribe(const QByteArray &pcm);
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
    // Phrases heard while the recogniser was busy, oldest first. The microphone
    // no longer closes while we work, so this can happen whenever somebody
    // carries on talking.
    QList<QByteArray> m_queue;
    QByteArray m_speech;      // the phrase being kept
    QByteArray m_preroll;     // the moment before it, so no syllable is lost
    qint64 m_silenceBytes = 0;
    qint64 m_voicedBytes = 0;
    qint64 m_attackBytes = 0; // how long it has been loud without opening yet
    qint64 m_idleBytes = 0;   // quiet audio since the last trace line
    int m_beats = 0;          // how many of those lines were written
    qreal m_floor = 0.004;    // running estimate of the room's own noise
};

#endif // STTENGINE_H
