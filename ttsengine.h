#ifndef TTSENGINE_H
#define TTSENGINE_H

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>

class QAudioOutput;
class QMediaPlayer;
class QProcess;
class QTimer;

// Speaks text with a local Kokoro model through the bundled
// sherpa-onnx-offline-tts.exe, then plays the WAV it produced. Nothing leaves
// the machine and nothing blocks: synthesis is a child process.
class TtsEngine : public QObject
{
    Q_OBJECT

public:
    explicit TtsEngine(QObject *parent = nullptr);
    ~TtsEngine() override;

    bool isBusy() const { return m_busy; }

    // False means nothing was spoken and failed() has already been emitted.
    bool speak(const QString &text);
    void stop();

signals:
    void started();
    // Emitted the moment playback begins, carrying the text being spoken.
    void speaking(const QString &text);
    // Playback position as 0.0 to 1.0.
    void progress(qreal fraction);
    // How LOUD the voice is right now, 0.0 to 1.0, every 16 ms while it plays
    // and a final 0.0 when it stops. Read off the WAV itself before playback,
    // so it follows the real syllables; the orb breathes to this.
    void level(qreal loudness);
    void finished();
    void failed(const QString &reason);

private:
    QString cliPath() const;
    void play(const QString &wavPath);
    // Turns the WAV into one loudness per 10 ms, normalised to its own peak.
    void loadEnvelope(const QString &wavPath);
    void stopMeter();

    QList<float> m_envelope;   // one value per 10 ms of audio, 0..1
    qint64 m_lastPosMs = 0;    // the player's last reported position
    QElapsedTimer m_sincePos;  // and how long ago it reported it
    QTimer *m_meter = nullptr; // emits level() between the player's reports

    QProcess *m_proc = nullptr;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QString m_wavPath;
    QString m_text;
    int m_counter = 0;
    bool m_busy = false;
    // True only inside stop(). Neither failed() nor finished() may escape a
    // deliberate cancellation, or the caller reports an error for something it
    // asked for itself.
    bool m_cancelled = false;
};

#endif // TTSENGINE_H
