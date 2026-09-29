#ifndef AGENTCURSOR_H
#define AGENTCURSOR_H

#include <QPoint>
#include <QWidget>

class QTimer;

// A small pointer the agent OWNS and the user can SEE. It glides to wherever
// the agent is about to click or type and flares when it acts. The agent's
// real actions go straight to the window as background messages and never move
// the physical mouse, so without this there would be nothing on screen to say
// where the agent is working - this is that second cursor, made visible.
//
// Frameless, always on top and CLICK THROUGH: it sits over everything yet the
// real pointer, and every real click, passes straight through it.
class AgentCursor : public QWidget
{
    Q_OBJECT

public:
    explicit AgentCursor(QWidget *parent = nullptr);

    // From ANY thread: put the agent's pointer at a screen point. `acting` is a
    // click or a keypress, which flares it. Marshalled onto the GUI thread, so
    // the hands thread may call it directly.
    static void pointAt(const QPoint &screen, bool acting);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void flare();

    qreal m_glow = 0.0;      // 1.0 the instant it acts, eased back to 0
    QTimer *m_fade = nullptr; // eases the flare down
    QTimer *m_idle = nullptr; // hides it after a spell of no pointing
};

#endif // AGENTCURSOR_H
