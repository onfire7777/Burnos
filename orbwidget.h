#ifndef ORBWIDGET_H
#define ORBWIDGET_H

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QIcon>
#include <QList>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QWidget>

class QPainter;
class QTimer;

// A self contained widget that paints an animated, glowing orb on a fully
// transparent background. It also carries the window interaction, because it
// covers the whole client area of a frameless window: drag with the left
// button to move, click without dragging to toggle active, right click for
// the menu.
class OrbWidget : public QWidget
{
    Q_OBJECT

public:
    explicit OrbWidget(QWidget *parent = nullptr);

    bool isActive() const { return m_active; }
    void setActive(bool active);

    // True when a point in this widget's coordinates lands on the orb itself
    // rather than the corners around it. Only the orb is solid: everything
    // else is see through and click through.
    bool hitsOrb(const QPointF &pos) const;

    // The orb's diameter. The window is exactly this square, and every pixel of
    // it that is not orb is invisible and clicks straight through.
    void setOrbDiameter(int px);

    // NO TEXT IS EVER SHOWN FOR AN ANSWER. While the orb is speaking it goes
    // deep blue and its core warps, exactly as it does while the user speaks -
    // see setSpeaking. The answer is something to hear.
    //
    // A failure is the one exception: one word on the orb for a few seconds,
    // taken away again by itself, because the reason is a sentence and
    // sentences go to the log rather than onto a 210 pixel orb.
    void showNotice(const QString &text, int holdMs = 8000);
    void clearNotice();

    // Nothing is being said any more: the deep blue goes, the warp settles back
    // and any failure word is taken down. EVERY path out of a turn ends here.
    void stopSpeaking();

    // The orb on its own: window, task bar and executable icon all come out
    // of the same paint code, so the icon can never drift from the orb.
    static QPixmap renderOrb(int size);
    static QIcon orbIcon();
    static bool writeIco(const QString &path);

signals:
    // A left press and release that never crossed the drag threshold.
    void clicked();


public slots:
    // Live microphone loudness, 0.0 to 1.0. Drives the warp while recording.
    void setLevel(qreal level);

    // True while the voice detector is actually keeping a phrase, so the armed
    // orb can look busier than the armed orb that is merely waiting.
    void setCapturing(bool on);

    // True from the end of a phrase until we are listening again. The hint is
    // hidden throughout, because the microphone is shut and claiming to be
    // listening would be a lie.
    void setThinking(bool on);

    // True while the voice is talking. The orb eases to deep blue and, fed by
    // setSpeechLevel, swells and brightens on every syllable. No words.
    void setSpeaking(bool on);

    // How loud the voice is right now, 0.0 to 1.0, from the TTS engine's
    // meter. Ignored unless speaking.
    void setSpeechLevel(qreal level);

    // True from the model answering until the voice actually starts. Kokoro
    // takes the better part of half a minute over a long reply and that wait
    // needs a word of its own, or it looks identical to thinking.
    void setPreparing(bool on);

    // True while any background task is running. Two short arcs chase each
    // other round the rim; no word, and nothing else about the orb changes,
    // because it is still listening and talking as usual the whole time.
    void setWorking(bool on);

    // First run setup: a ring and a caption over the orb. While this is on,
    // clicking does nothing, because there is nothing to talk to yet.
    void beginInitializing();
    void endInitializing();
    void setStatus(const QString &text, qreal fraction);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void drawHalo(QPainter &p, const QPointF &centre, qreal radius,
                  const QColor &colour, qreal strength);
    void drawSetup(QPainter &p, const QPointF &centre, qreal unit);
    void drawHint(QPainter &p);
    void drawWorking(QPainter &p, const QPointF &centre, qreal unit);

    // Works out which hint belongs on the orb right now and starts the fade.
    void updateHint();

    qreal m_phase = 0.0;
    qreal m_level = 0.0;       // smoothed, what the painter uses
    qreal m_levelTarget = 0.0; // newest measurement from the microphone
    QTimer *m_timer = nullptr;
    bool m_dragging = false;
    bool m_moved = false;
    bool m_active = false;    // live mode armed
    bool m_capturing = false; // a phrase is being kept right now
    bool m_initializing = false;
    QString m_statusText;
    qreal m_statusFraction = -1.0; // negative means the size is unknown yet
    bool m_thinking = false;       // answering, so the microphone is shut
    bool m_speaking = false;       // the voice is saying the answer
    bool m_preparing = false;      // the answer is back, the voice is not yet

    // The muted word in the middle of the orb: "I'm listening" while armed and
    // waiting, "Muted" while disarmed, and nothing at all once speech is heard.
    // m_hintText is what is on screen and m_hintWanted is what should be; the
    // difference between them is always crossed by fading out first.
    QString m_hintText;
    QString m_hintWanted;
    qreal m_hintOpacity = 0.0;

    QRectF m_orbRect;              // the square the orb is painted in
    int m_orbDiameter = 0;         // 0 means "as big as the widget allows"

    // The speaking animation. m_speakMix eases 0 to 1 as the voice starts and
    // back down as it stops, and everything about the deep blue is multiplied
    // by it, so the orb never changes state in a single frame. m_speakLevel
    // is the REAL loudness of the voice, fed by setSpeechLevel and smoothed the
    // same way as the microphone; m_wispSpin is how far the lights have been
    // whirled on by it. All advanced by the frame timer and by nothing else.
    qreal m_speakMix = 0.0;
    qreal m_speakTarget = 0.0;
    qreal m_speakLevel = 0.0;
    qreal m_wispSpin = 0.0;

    bool m_working = false;        // background tasks are running
    qreal m_workMix = 0.0;         // eased 0 to 1, so the arcs fade in and out

    bool m_noticeOn = false;       // a failure word is up on the orb
    QTimer *m_notice = nullptr;    // and this takes it away again

    QPoint m_pressPos;
    QPoint m_dragOffset;
};

#endif // ORBWIDGET_H
