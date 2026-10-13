#ifndef TTSENGINE_H
#define TTSENGINE_H

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

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

    // False means nothing was spoken and failed() has already been emitted.
    bool speak(const QString &text);
    void stop();

    // Short replies said again and again - "On it.", "Okay." - are made once,
    // kept on disk and played the instant they are asked for, instead of after
    // two seconds of Kokoro every time. Kept per voice. Making them happens in
    // the background and never holds up a real answer.
    void prepare(const QStringList &phrases);

    // Loads the Kokoro model now, off the main thread, and keeps it for as
    // long as Burnos runs, so no answer waits for it. Called when Burnos opens.
    static void preload();

signals:
    // Emitted the moment playback begins, carrying the text being spoken.
    void speaking(const QString &text);
    // How LOUD the voice is right now, 0.0 to 1.0, every 16 ms while it plays
    // and a final 0.0 when it stops. Read off the WAV itself before playback,
    // so it follows the real syllables; the orb breathes to this.
    void level(qreal loudness);
    void finished();
    void failed(const QString &reason);

private:
    QString cliPath() const;
    void play(const QString &wavPath);
    // Where the ready made copy of this text lives, for the voice chosen now.
    QString cachePath(const QString &text) const;
    void warmNext();
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
    // Set when the answer being made in the loaded model is no longer wanted.
    std::shared_ptr<std::atomic_bool> m_saying;

    // Making the short replies, one at a time, apart from the real voice.
    QProcess *m_warm = nullptr;
    QStringList m_toWarm;
    QString m_warmTarget;
};

#endif // TTSENGINE_H
