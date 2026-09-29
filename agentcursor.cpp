#include "agentcursor.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointer>
#include <QTimer>

namespace {
constexpr int kSize = 30; // the pointer's little box, in pixels

// The one agent cursor. Made and touched ONLY on the GUI thread; a worker
// thread reaches it through the static functions, which marshal here.
QPointer<AgentCursor> g_cursor;

AgentCursor *ensure()
{
    if (!g_cursor)
        g_cursor = new AgentCursor;
    return g_cursor;
}
} // namespace

AgentCursor::AgentCursor(QWidget *parent)
    : QWidget(parent)
{
    // Click through, never activated, above everything, no shadow of its own.
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool
                   | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus
                   | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    resize(kSize, kSize);

    m_fade = new QTimer(this);
    m_fade->setInterval(16);
    connect(m_fade, &QTimer::timeout, this, [this]() {
        m_glow = qMax(qreal(0.0), m_glow - qreal(0.08));
        update();
        if (m_glow <= 0.0)
            m_fade->stop();
    });

    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    m_idle->setInterval(2500);
    connect(m_idle, &QTimer::timeout, this, &QWidget::hide);
}

void AgentCursor::pointAt(const QPoint &screen, bool acting)
{
    QMetaObject::invokeMethod(
        qApp,
        [screen, acting]() {
            AgentCursor *c = ensure();
            // The arrow's tip is a few pixels in from the top left, so the box
            // is placed to put that tip on the point.
            c->move(screen.x() - 6, screen.y() - 4);
            if (!c->isVisible())
                c->show();
            c->raise();
            if (acting)
                c->flare();
            c->m_idle->start();
        },
        Qt::QueuedConnection);
}

void AgentCursor::flare()
{
    m_glow = 1.0;
    if (!m_fade->isActive())
        m_fade->start();
    update();
}

void AgentCursor::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // The same blues the orb uses, so the agent's pointer is plainly Burnos.
    const QColor blue(90, 210, 255);
    const QColor deep(28, 104, 255);

    if (m_glow > 0.0) {
        p.setPen(Qt::NoPen);
        QColor halo = blue;
        halo.setAlphaF(qreal(0.35) * m_glow);
        p.setBrush(halo);
        const qreal r = 8.0 + 9.0 * m_glow;
        p.drawEllipse(QPointF(7, 6), r, r);
    }

    // A small arrow, so it reads as a cursor and not as a button.
    QPainterPath arrow;
    arrow.moveTo(6, 4);
    arrow.lineTo(6, 21);
    arrow.lineTo(10.5, 16.5);
    arrow.lineTo(13.5, 22.5);
    arrow.lineTo(16, 21.2);
    arrow.lineTo(13, 15.2);
    arrow.lineTo(19, 15);
    arrow.closeSubpath();

    p.setPen(QPen(QColor(255, 255, 255, 235), 1.2));
    p.setBrush(deep);
    p.drawPath(arrow);
}
