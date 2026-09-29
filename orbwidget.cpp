#include "livelog.h"
#include "orbwidget.h"
#include "settings.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QToolTip>
#include <QBuffer>
#include <QDataStream>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QPixmap>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QRadialGradient>
#include <QTimer>
#include <QtMath>

namespace {
constexpr int   kFrameMs   = 16;     // roughly sixty frames per second
constexpr qreal kPhaseStep = 0.018;  // phase added on every frame
constexpr int   kWispCount = 5;      // orbiting light blobs

// The orb is painted this much larger than the window that holds it. Its
// outermost halo is a wash of a couple of percent alpha, so letting the window
// edge crop that is invisible - whereas leaving room for it padded the box out
// with a wide, empty border that had nothing in it.
constexpr qreal kOrbBleed = 1.18;

// One blue for the whole application: the idle orb, the icon and the word
// being spoken are all painted with exactly this colour.
const QColor kOrbBlue(90, 210, 255);
const QColor kOrbViolet(150, 110, 255);
// Speaking. A deep, saturated blue that nothing else in the application uses,
// so "it is talking" is a colour rather than a word.
const QColor kOrbDeepBlue(28, 104, 255);
const QColor kOrbDeepViolet(78, 44, 225);
// What a word settles to once the voice has moved past it.
const QColor kCaptionTrail(200, 236, 255);
// Live mode off: a pale, almost colourless grey, so one glance says the
// microphone is shut.
const QColor kOrbGrey(206, 213, 223);
const QColor kOrbGreyDeep(148, 156, 170);

// Blend two colours, 0 gives the first and 1 the second. Used so the orb
// CHANGES colour over a quarter of a second instead of snapping to it.
QColor mixColour(const QColor &a, const QColor &b, qreal t)
{
    const qreal k = qBound(qreal(0.0), t, qreal(1.0));
    return QColor(int(a.red()   + (b.red()   - a.red())   * k),
                  int(a.green() + (b.green() - a.green()) * k),
                  int(a.blue()  + (b.blue()  - a.blue())  * k),
                  int(a.alpha() + (b.alpha() - a.alpha()) * k));
}

void paintHalo(QPainter &p, const QPointF &centre, qreal radius,
               const QColor &colour, qreal strength)
{
    if (radius <= 0.0 || strength <= 0.0)
        return;

    QColor inner = colour;
    inner.setAlphaF(qBound(0.0, strength, 1.0));
    QColor mid = colour;
    mid.setAlphaF(qBound(0.0, strength * 0.35, 1.0));
    QColor outer = colour;
    outer.setAlphaF(0.0);

    QRadialGradient g(centre, radius);
    g.setColorAt(0.0, inner);
    g.setColorAt(0.45, mid);
    g.setColorAt(1.0, outer);

    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawEllipse(centre, radius, radius);
}
} // namespace

OrbWidget::OrbWidget(QWidget *parent)
    : QWidget(parent)
    , m_timer(new QTimer(this))
{
    // No opaque paint attribute here: the window behind us is translucent and
    // every frame is cleared to full transparency instead.
    setAutoFillBackground(false);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMinimumSize(120, 120);

    connect(m_timer, &QTimer::timeout, this, [this]() {
        m_phase += kPhaseStep;
        if (m_phase > qreal(2.0 * M_PI * 1000.0))
            m_phase = 0.0;

        // Snap up to a loud syllable, fall away slowly, so the orb follows
        // speech instead of flickering on every buffer.
        const qreal k = (m_levelTarget > m_level) ? 0.55 : 0.06;
        m_level += (m_levelTarget - m_level) * k;

        // THE SPEAKING ANIMATION IS ADVANCED HERE AND NOWHERE ELSE. One clock,
        // one fixed step per frame. It used to be moved on by the painter,
        // which ran it twice on frames that painted twice and not at all on
        // frames that did not - that is what made it stutter.
        const qreal wantMix = m_speaking ? qreal(1.0) : qreal(0.0);
        if (m_speakMix < wantMix)
            m_speakMix = qMin(wantMix, m_speakMix + 0.055);
        else if (m_speakMix > wantMix)
            m_speakMix = qMax(wantMix, m_speakMix - 0.045);

        // The voice. Snaps up on a syllable and falls away more slowly, the
        // same shape as the microphone's smoother above, so a word swells and
        // then lingers rather than flickering.
        const qreal ks = (m_speakTarget > m_speakLevel) ? 0.45 : 0.10;
        m_speakLevel += (m_speakTarget - m_speakLevel) * ks;
        if (m_speakLevel < 0.002)
            m_speakLevel = 0.0;

        // The lights whirl faster the louder it talks, and keep whatever angle
        // they have reached when it stops, so they never jump back.
        m_wispSpin += kPhaseStep * 1.8 * m_speakMix * m_speakLevel;

        // The working arcs fade in and out rather than switching.
        const qreal wantWork = m_working ? qreal(1.0) : qreal(0.0);
        if (m_workMix < wantWork)
            m_workMix = qMin(wantWork, m_workMix + 0.04);
        else if (m_workMix > wantWork)
            m_workMix = qMax(wantWork, m_workMix - 0.03);

        // The hint never cuts. It fades out before the wording changes and
        // fades back in afterwards, so speech starting simply dissolves it.
        if (m_hintText != m_hintWanted) {
            m_hintOpacity -= 0.09;
            if (m_hintOpacity <= 0.0) {
                m_hintOpacity = 0.0;
                m_hintText = m_hintWanted;
            }
        } else if (!m_hintText.isEmpty() && m_hintOpacity < 1.0) {
            m_hintOpacity = qMin(qreal(1.0), m_hintOpacity + 0.05);
        }

        update();
    });
    m_timer->start(kFrameMs);

    // Disarmed until something says otherwise, and the orb says so.
    updateHint();
}

void OrbWidget::drawHalo(QPainter &p, const QPointF &centre, qreal radius,
                         const QColor &colour, qreal strength)
{
    paintHalo(p, centre, radius, colour, strength);
}

void OrbWidget::setOrbDiameter(int px)
{
    m_orbDiameter = px;
    update();
}

void OrbWidget::showNotice(const QString &text, int holdMs)
{
    // A sentence does not fit inside an orb and the orb shows no text while it
    // is working, so a failure gets ONE word here and the detail goes to the
    // log, which is where anyone debugging one will actually look.
    Q_UNUSED(text);

    if (!m_notice) {
        m_notice = new QTimer(this);
        m_notice->setSingleShot(true);
        connect(m_notice, &QTimer::timeout, this, &OrbWidget::clearNotice);
    }
    m_noticeOn = true;
    m_notice->start(holdMs);
    updateHint();
    update();
}

void OrbWidget::stopSpeaking()
{
    setSpeaking(false);
    clearNotice();
}

void OrbWidget::clearNotice()
{
    if (m_notice)
        m_notice->stop();
    if (!m_noticeOn)
        return;
    m_noticeOn = false;
    updateHint();
    update();
}

void OrbWidget::setSpeechLevel(qreal level)
{
    // Only a target: the frame timer eases towards it, so a burst of updates
    // from the meter can never make the orb jump.
    m_speakTarget = m_speaking ? qBound(qreal(0.0), level, qreal(1.0))
                               : qreal(0.0);
}

bool OrbWidget::hitsOrb(const QPointF &pos) const
{
    // m_orbRect is worked out by the painter. Before the very first frame it is
    // still empty, so derive it the same way rather than reporting a miss and
    // leaving the orb undraggable for a frame or two.
    QRectF orb = m_orbRect;
    if (orb.isNull()) {
        const QRectF r = rect();
        qreal side = qMin(r.width(), r.height());
        if (m_orbDiameter > 0)
            side = qMin<qreal>(m_orbDiameter, side);
        orb = QRectF(r.left() + (r.width() - side) / 2.0,
                     r.top() + (r.height() - side) / 2.0, side, side);
    }
    if (orb.isNull())
        return false;

    const QPointF d = pos - orb.center();
    const qreal radius = orb.width() / 2.0;
    return QPointF::dotProduct(d, d) <= radius * radius;
}

QPixmap OrbWidget::renderOrb(int size)
{
    // ALWAYS painted large and scaled down. A radial gradient drawn straight
    // into 16 or 24 pixels bands visibly and the circle comes out lumpy, and
    // small is exactly where an icon is usually seen.
    const int super = qMax(size * 4, 256);

    QPixmap big(super, super);
    big.fill(Qt::transparent);

    QPainter p(&big);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QPointF c(super / 2.0, super / 2.0);
    const qreal unit = super / 2.0;
    // Big, and then bigger again. An icon is a tiny square to begin with, so
    // the orb has to fill it - anything floating in a transparent margin just
    // reads as small, however pretty the glow around it is.
    const qreal coreR = unit * 0.55;

    // The halos build light up additively, but ONLY the halos: leaving Plus on
    // for the core blew it out and ate its own edge, which is what made the
    // orb look neither round nor evenly lit.
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    // The outermost wash runs all the way to the edge of the bitmap. That is
    // safe because paintHalo's gradient reaches zero alpha at its own radius,
    // so it meets the border already invisible - what used to clip into a
    // square was the CORE being drawn additively over it, not the halo itself.
    paintHalo(p, c, unit * 0.98, kOrbViolet, 0.26);
    paintHalo(p, c, unit * 0.88, kOrbBlue, 0.46);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Perfectly concentric, and no offset highlight. The live orb carries one
    // so it looks lit from somewhere and alive; an icon that is not
    // symmetrical just looks like a mistake.
    QColor mid = kOrbBlue;
    mid.setAlpha(170);
    QColor edge = kOrbBlue;
    edge.setAlpha(0);

    QRadialGradient core(c, coreR);
    core.setColorAt(0.00, QColor(255, 255, 255, 255));
    core.setColorAt(0.40, QColor(238, 249, 255, 245));
    core.setColorAt(0.75, mid);
    core.setColorAt(1.00, edge);
    p.setPen(Qt::NoPen);
    p.setBrush(core);
    p.drawEllipse(c, coreR, coreR);

    // THE LIT EDGE, and it has to go on TOP. Painted underneath with the rest
    // of the halos it simply disappears behind the core, which is why the icon
    // came out as a white dot with a hard blue outline and no glow at all.
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    paintHalo(p, c, coreR * 1.55, kOrbBlue, 0.38);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.end();

    return big.scaled(size, size, Qt::IgnoreAspectRatio,
                      Qt::SmoothTransformation);
}

QIcon OrbWidget::orbIcon()
{
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256})
        icon.addPixmap(renderOrb(size));
    return icon;
}

bool OrbWidget::writeIco(const QString &path)
{
    // A Vista style icon file: every entry is a PNG, so the 256 pixel image
    // stays small and Windows picks whichever size it needs.
    const QList<int> sizes{16, 24, 32, 48, 64, 128, 256};
    QList<QByteArray> images;
    for (const int size : sizes) {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!renderOrb(size).save(&buffer, "PNG"))
            return false;
        images.append(png);
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out << quint16(0) << quint16(1) << quint16(sizes.size());

    quint32 offset = 6 + 16 * quint32(sizes.size());
    for (int i = 0; i < sizes.size(); ++i) {
        const int size = sizes.at(i);
        const quint8 dim = quint8(size >= 256 ? 0 : size);
        out << dim << dim << quint8(0) << quint8(0);
        out << quint16(1) << quint16(32)
            << quint32(images.at(i).size()) << offset;
        offset += quint32(images.at(i).size());
    }
    for (const QByteArray &png : std::as_const(images))
        out.writeRawData(png.constData(), png.size());

    file.close();
    return file.error() == QFileDevice::NoError;
}

void OrbWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = rect();

    // Wipe the frame to genuine transparency, so only the orb is ever visible.
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(r, Qt::transparent);

    // The orb sits in the middle of the window and NEVER changes size. The
    // answer is one word at a time in the middle of it, so there is nothing to
    // make room for.
    qreal side = qMin(r.width(), r.height());
    if (m_orbDiameter > 0)
        side = qMin<qreal>(m_orbDiameter, side);
    m_orbRect = QRectF(r.left() + (r.width() - side) / 2.0,
                       r.top() + (r.height() - side) / 2.0, side, side);

    const QPointF c = m_orbRect.center();
    const qreal half = side / 2.0;
    const qreal unit = half * kOrbBleed;

    const qreal pulse   = 0.5 + 0.5 * qSin(m_phase * 1.7);        // slow breath
    const qreal flicker = 0.5 + 0.5 * qSin(m_phase * 5.3 + 1.1);  // fast shimmer

    // The warp is the MICROPHONE'S, and only ever the microphone's. Driving it
    // from a made up loudness while the orb spoke made a spiky blob that threw
    // itself into a new shape every frame; speaking gets its own animation
    // instead, which is smooth by construction. See drawSpeakPulses.
    const qreal level = m_level;

    // THE VOICE, as the real loudness of the audio being played. The core
    // swells on every syllable and the light around it brightens with it -
    // size and brightness, never shape, which is what keeps it smooth and
    // what keeps it distinct from the microphone's warp.
    const qreal voice = m_speakMix * m_speakLevel;
    const qreal coreR = unit * (0.36 + 0.03 * pulse + 0.10 * level
                                + 0.15 * voice);

    // Pale blue means live mode is armed and the microphone is open, pale grey
    // means it is not listening, DEEP BLUE means it is speaking. A frameless
    // window with no title bar has nowhere else to show state, so the colour is
    // the feedback and no word is needed for any of it. A phrase actually being
    // captured pushes the listening blue brighter still. The change into and
    // out of the deep blue is EASED, never switched.
    QColor glow = m_active ? (m_capturing ? QColor(140, 232, 255) : kOrbBlue)
                           : kOrbGrey;
    QColor aura = m_active ? kOrbViolet : kOrbGreyDeep;
    if (m_speakMix > 0.0) {
        glow = mixColour(glow, kOrbDeepBlue, m_speakMix);
        aura = mixColour(aura, kOrbDeepViolet, m_speakMix);
    }

    // Additive blending so overlapping halos build up light instead of mud.
    p.setCompositionMode(QPainter::CompositionMode_Plus);

    // Outermost atmosphere fades to nothing just inside the widget edge.
    drawHalo(p, c, unit * (0.96 + 0.04 * pulse) * (1.0 + 0.03 * level + 0.04 * voice),
             aura, 0.13 + 0.06 * pulse + 0.10 * level + 0.12 * voice);
    drawHalo(p, c, unit * (0.78 + 0.06 * pulse) * (1.0 + 0.06 * level + 0.08 * voice),
             glow, 0.24 + 0.10 * pulse + 0.18 * level + 0.24 * voice);

    for (int i = 0; i < kWispCount; ++i) {
        const qreal a = (m_phase + m_wispSpin) * (0.6 + 0.12 * i)
                        + (2.0 * M_PI * i) / kWispCount;
        const qreal dist = unit * (0.30 + 0.05 * qSin(m_phase * 1.3 + i)
                                   + 0.12 * level);
        const QPointF wc(c.x() + dist * qCos(a),
                         c.y() + dist * qSin(a) * 0.55);
        const qreal wr = unit * (0.22 + 0.04 * qSin(m_phase * 2.1 + i * 0.7));
        drawHalo(p, wc, wr, (i % 2) ? aura : glow, 0.13 + 0.05 * flicker);
    }

    drawHalo(p, c, coreR * 1.9, glow, 0.30 + 0.12 * flicker + 0.30 * voice);

    // A soft ring around the core that glows with the voice - the one thing the
    // listening orb never has, so a glance tells which way the talking goes.
    if (voice > 0.01) {
        const qreal ringR = unit * (0.50 + 0.05 * voice);
        QColor ring = kOrbDeepBlue.lighter(135);
        p.setBrush(Qt::NoBrush);
        ring.setAlphaF(0.10 * voice);
        p.setPen(QPen(ring, unit * 0.10, Qt::SolidLine, Qt::RoundCap));
        p.drawEllipse(c, ringR, ringR);
        ring.setAlphaF(0.28 * voice);
        p.setPen(QPen(ring, unit * 0.035, Qt::SolidLine, Qt::RoundCap));
        p.drawEllipse(c, ringR, ringR);
        p.setPen(Qt::NoPen);
    }

    QColor coreMid = glow;
    coreMid.setAlpha(150);
    QColor coreEdge = aura;
    coreEdge.setAlpha(0);

    QRadialGradient core(c, coreR);
    // Speaking keeps a blue heart rather than a white one, so the deep blue is
    // not bleached out by the core sitting on top of it - blended, so the heart
    // cools and warms with everything else.
    core.setColorAt(0.0, mixColour(QColor(255, 255, 255, 255),
                                   QColor(214, 234, 255, 255), m_speakMix));
    core.setColorAt(0.35, mixColour(QColor(235, 246, 255, 235),
                                    QColor(150, 200, 255, 240), m_speakMix));
    core.setColorAt(0.75, coreMid);
    core.setColorAt(1.0, coreEdge);
    p.setPen(Qt::NoPen);
    p.setBrush(core);

    if (level <= 0.01) {
        p.drawEllipse(c, coreR, coreR);
    } else {
        // Voice warps the core out of round: three overlapping lobes turning
        // at different rates, their depth driven by how loud the speaker is.
        const qreal warp = level * 0.55;
        QPainterPath path;
        constexpr int kSteps = 128;
        for (int i = 0; i <= kSteps; ++i) {
            const qreal t = (2.0 * M_PI * i) / kSteps;
            const qreal r = coreR * (1.0 + warp * (0.45 * qSin(3.0 * t + m_phase * 2.2)
                                                   + 0.30 * qSin(5.0 * t - m_phase * 1.7)
                                                   + 0.20 * qSin(7.0 * t + m_phase * 3.1)));
            const QPointF pt(c.x() + r * qCos(t), c.y() + r * qSin(t));
            if (i == 0)
                path.moveTo(pt);
            else
                path.lineTo(pt);
        }
        path.closeSubpath();
        p.drawPath(path);
    }

    const QPointF hp(c.x() - coreR * 0.32 + coreR * 0.10 * qSin(m_phase * 0.9),
                     c.y() - coreR * 0.34 + coreR * 0.10 * qCos(m_phase * 0.7));
    QRadialGradient hi(hp, coreR * 0.55);
    hi.setColorAt(0.0, QColor(255, 255, 255, 170));
    hi.setColorAt(1.0, QColor(255, 255, 255, 0));
    p.setBrush(hi);
    p.drawEllipse(hp, coreR * 0.55, coreR * 0.55);

    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    if (m_initializing)
        drawSetup(p, c, half);
    else
        drawWorking(p, c, half);

    if (!m_initializing && !m_hintText.isEmpty() && m_hintOpacity > 0.01)
        drawHint(p);

}

void OrbWidget::drawHint(QPainter &p)
{
    if (m_orbRect.isNull())
        return;

    QFont f = font();
    f.setPointSizeF(11.5);
    f.setBold(true);
    f.setLetterSpacing(QFont::PercentageSpacing, 104.0);

    // Muted on purpose: this is a label, not the answer. It sits over the
    // bright core, so a mid grey with a soft dark edge reads on the blue orb
    // and on the grey one alike.
    const qreal a = qBound(qreal(0.0), m_hintOpacity, qreal(1.0));
    QColor ink(96, 108, 124);
    ink.setAlphaF(a * 0.88);
    QColor edge(10, 16, 26);
    edge.setAlphaF(a * 0.20);

    const QFontMetricsF fm(f);
    const qreal x = m_orbRect.center().x()
                    - fm.horizontalAdvance(m_hintText) / 2.0;
    const qreal y = m_orbRect.center().y()
                    + (fm.ascent() - fm.descent()) / 2.0;

    QPainterPath glyphs;
    glyphs.addText(x, y, f, m_hintText);

    p.save();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(edge, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(glyphs);
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    p.drawPath(glyphs);
    p.restore();
}

void OrbWidget::updateHint()
{
    // Shown only when the orb has nothing better to say: no setup ring, no
    // answer on screen, no phrase being captured and nothing being answered.
    // Armed and waiting says NOTHING. A word sitting there all day is noise;
    // the blue orb already says the microphone is open. The orb speaks only
    // when something is actually happening - and an answer takes many seconds,
    // so those waits do need a word or the whole thing reads as broken.
    QString wanted;
    if (!m_initializing && m_noticeOn) {
        // A failure outranks everything but the setup ring. One word, because
        // the reason is a sentence and sentences go to the log.
        wanted = tr("Error");
    } else if (!m_initializing && !m_speaking) {
        if (m_capturing)
            wanted = tr("Listening");
        else if (m_preparing)
            wanted = tr("Preparing");
        else if (m_thinking)
            wanted = tr("Thinking");
        else if (!m_active)
            wanted = tr("Muted");
    }

    if (m_hintWanted == wanted)
        return;
    m_hintWanted = wanted;
    update();
}

void OrbWidget::drawSetup(QPainter &p, const QPointF &c, qreal unit)
{
    const QRectF ring(c.x() - unit * 0.86, c.y() - unit * 0.86,
                      unit * 1.72, unit * 1.72);

    // Faint full circle behind, so the moving part reads as progress along a
    // track rather than a stray arc floating in space.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(120, 190, 255, 60), 3.0));
    p.drawEllipse(ring);

    p.setPen(QPen(QColor(200, 235, 255, 230), 3.6, Qt::SolidLine, Qt::RoundCap));
    if (m_statusFraction >= 0.0) {
        // Twelve o'clock, clockwise, like every other progress ring.
        p.drawArc(ring, 90 * 16, -int(360.0 * 16.0 * m_statusFraction));
    } else {
        // Size unknown: sweep a short arc so it still looks alive.
        const int start = int(-(m_phase * 240.0)) * 16 % (360 * 16);
        p.drawArc(ring, start, -70 * 16);
    }

    // Inside the orb, not under it. Nothing is ever painted outside the orb:
    // the answer goes inside it too, see drawCaption.
    QFont caption = font();
    caption.setPointSizeF(8.5);
    caption.setBold(true);
    p.setFont(caption);
    p.setPen(QColor(226, 242, 255, 235));
    p.drawText(QRectF(m_orbRect.left(), c.y() - 19.0, m_orbRect.width(), 16.0),
               Qt::AlignCenter, m_statusText);

    if (m_statusFraction >= 0.0) {
        QFont pct = caption;
        pct.setPointSizeF(8.0);
        pct.setBold(false);
        p.setFont(pct);
        p.setPen(QColor(170, 210, 245, 200));
        p.drawText(QRectF(m_orbRect.left(), c.y() + 2.0, m_orbRect.width(), 16.0),
                   Qt::AlignCenter,
                   QStringLiteral("%1%").arg(int(m_statusFraction * 100.0)));
    }
}

void OrbWidget::beginInitializing()
{
    m_initializing = true;
    m_statusText = tr("Initializing Burnos");
    m_statusFraction = -1.0;
    updateHint();
    update();
}

void OrbWidget::endInitializing()
{
    m_initializing = false;
    m_statusText.clear();
    m_statusFraction = -1.0;
    updateHint();
    update();
}

void OrbWidget::setStatus(const QString &text, qreal fraction)
{
    m_statusText = text;
    m_statusFraction = fraction > 1.0 ? 1.0 : fraction;
    update();
}

// There is deliberately no contextMenuEvent here. The orb covers the whole
// client area, so swallowing the right click would hide MainWindow's own menu,
// which is the one carrying Settings. Letting the event propagate is the fix.
void OrbWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        // Only the orb itself is interactive. The caption strip below it is
        // empty space, and clicking there must neither drag the window nor arm
        // anything. Windows is told the same thing in MainWindow::nativeEvent,
        // so a click out there reaches whatever is behind instead of dying here.
        if (!hitsOrb(event->position())) {
            QWidget::mousePressEvent(event);
            return;
        }

        QWidget *w = window();
        m_dragging = true;
        m_moved = false;
        m_pressPos = event->globalPosition().toPoint();
        m_dragOffset = m_pressPos - w->frameGeometry().topLeft();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void OrbWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        const QPoint here = event->globalPosition().toPoint();
        // Past the system drag threshold this counts as a move, not a click.
        // Without that slack, every click would also nudge the window.
        if ((here - m_pressPos).manhattanLength() > QApplication::startDragDistance())
            m_moved = true;
        window()->move(here - m_dragOffset);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void OrbWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        // Nothing to talk to until the models are on disk.
        if (!m_moved && !m_initializing) {
            setActive(!m_active);
            emit clicked();
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void OrbWidget::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (!m_active) {
        m_levelTarget = 0.0;
        m_capturing = false;
        m_thinking = false;  // disarmed, so it is not answering either
        m_preparing = false;
        m_speaking = false;
    }
    BurnOS::liveLog(QStringLiteral("orb is now %1")
                        .arg(m_active ? QStringLiteral("BLUE (live mode armed)")
                                      : QStringLiteral("GREY (live mode off)")));
    updateHint();
    update();
}

void OrbWidget::setLevel(qreal level)
{
    m_levelTarget = qBound(0.0, level, 1.0);
}

void OrbWidget::setCapturing(bool on)
{
    if (m_capturing == on)
        return;
    m_capturing = on;
    updateHint();
    update();
}

void OrbWidget::setThinking(bool on)
{
    if (m_thinking == on)
        return;
    m_thinking = on;
    updateHint();
    update();
}

void OrbWidget::setPreparing(bool on)
{
    if (m_preparing == on)
        return;
    m_preparing = on;
    updateHint();
    update();
}

void OrbWidget::setWorking(bool on)
{
    // WORK GOING ON IN THE BACKGROUND gets a light of its own and no word. The
    // hint words stay exactly as they were, because while tasks run the orb is
    // still listening, thinking and speaking as usual.
    if (m_working == on)
        return;
    m_working = on;
    BurnOS::liveLog(QStringLiteral("orb: background work %1")
                        .arg(on ? QStringLiteral("started") : QStringLiteral("finished")));
    update();
}

void OrbWidget::drawWorking(QPainter &p, const QPointF &c, qreal unit)
{
    if (m_workMix <= 0.01)
        return;

    const QRectF ring(c.x() - unit * 0.86, c.y() - unit * 0.86,
                      unit * 1.72, unit * 1.72);

    // A faint track, so the arcs read as going round something rather than as
    // stray marks.
    QColor track(120, 190, 255);
    track.setAlphaF(0.10 * m_workMix);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(track, 2.0));
    p.drawEllipse(ring);

    // About one turn every two and a half seconds - busy, not frantic.
    const int start = int(-(m_phase * 135.0) * 16.0) % (360 * 16);
    QColor arc(200, 235, 255);
    arc.setAlphaF(0.80 * m_workMix);
    p.setPen(QPen(arc, 2.6, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(ring, start, -50 * 16);
    p.drawArc(ring, start + 180 * 16, -50 * 16);
}

void OrbWidget::setSpeaking(bool on)
{
    // THIS is the answer, as far as the screen is concerned: the orb turns deep
    // blue and its core warps while the voice talks, and no words appear at
    // all. The hint stands down too, so nothing is written over it.
    if (m_speaking == on)
        return;
    m_speaking = on;
    if (!on)
        m_speakTarget = 0.0;
    updateHint();
    update();
}
