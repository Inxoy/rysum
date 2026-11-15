#include "VerlaufView.h"
#include "ModView.h"
#include <set>

namespace rysum::ui
{
namespace
{
const double kGridBeats[]{0.0, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0};
juce::StringArray gridNames()
{
    return {"Raster aus", "Raster 1/16", "Raster 1/8", "Raster 1/4", "Raster 1/2",
            "Raster 1 Takt", "Raster 2 Takte", "Raster 4 Takte"};
}
juce::String beatsText(double b)
{
    if (std::abs(b - std::round(b)) < 1.0e-6)
        return juce::String((int)std::round(b));
    return juce::String(b, b < 1.0 ? 3 : 2);
}
} // namespace

// ---------------------------------------------------------------------------
// FreeRow
// ---------------------------------------------------------------------------
FreeRow::FreeRow(const juce::String &t, double lo, double hi, double skewMid) : title(t)
{
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setRange(lo, hi, 0.0);
    if (skewMid > lo && skewMid < hi)
        slider.setSkewFactorFromMidPoint(skewMid);
    slider.onValueChange = [this]
    {
        repaint();
        if (onChange)
            onChange(slider.getValue());
    };
    slider.onDragStart = [this]
    {
        if (onDragStart)
            onDragStart();
    };
    addAndMakeVisible(slider);
}

void FreeRow::paint(juce::Graphics &g)
{
    auto r = getLocalBounds().removeFromTop(16);
    g.setColour(isEnabled() ? col::dim : col::faint);
    g.setFont(Assets::get().romanFont(11.5f));
    g.drawText(title, r, juce::Justification::centredLeft);
    g.setFont(Assets::get().monoFont(10.0f));
    g.drawText(format ? format(slider.getValue()) : juce::String(slider.getValue(), 2), r,
               juce::Justification::centredRight);
}

void FreeRow::resized()
{
    slider.setBounds(getLocalBounds().withTrimmedTop(14).withHeight(14));
}

// ---------------------------------------------------------------------------
// lane time <-> clock beats (first pass of a loop)
// ---------------------------------------------------------------------------
double VerlaufView::localOf(const AutoLane &l, double beat)
{
    const double tau = (beat - l.offset) * (double)l.speed;
    return l.reverse ? l.length - tau : tau;
}

double VerlaufView::beatOfLocal(const AutoLane &l, double t)
{
    const double tau = l.reverse ? l.length - t : t;
    return l.offset + tau / juce::jmax(1.0e-6, (double)l.speed);
}

// ---------------------------------------------------------------------------
// timeline geometry
// ---------------------------------------------------------------------------
float VerlaufTimeline::xOfBeat(double b) const
{
    const auto &s = owner.proc.getAuto();
    const float w = (float)(getWidth() - kHeaderW);
    return (float)kHeaderW + (float)((b - s.viewStart) / juce::jmax(1.0e-6, s.viewLen)) * w;
}

double VerlaufTimeline::beatOfX(float x) const
{
    const auto &s = owner.proc.getAuto();
    const float w = (float)juce::jmax(1, getWidth() - kHeaderW);
    return s.viewStart + (double)((x - (float)kHeaderW) / w) * s.viewLen;
}

juce::Rectangle<int> VerlaufTimeline::laneRect(int lane) const
{
    int y = kRulerH - scrollY;
    for (int l = 0; l < lane; ++l)
        y += l == owner.selLane ? 128 : 64;
    return {0, y, getWidth(), lane == owner.selLane ? 128 : 64};
}

int VerlaufTimeline::contentHeight() const
{
    const int n = (int)owner.proc.getAuto().lanes.size();
    int h = 0;
    for (int l = 0; l < n; ++l)
        h += l == owner.selLane ? 128 : 64;
    return h;
}

int VerlaufTimeline::laneAt(int y) const
{
    const int n = (int)owner.proc.getAuto().lanes.size();
    for (int l = 0; l < n; ++l)
        if (laneRect(l).contains(laneRect(l).getX(), y))
            return l;
    return -1;
}

juce::Rectangle<int> VerlaufTimeline::curveArea(juce::Rectangle<int> lane) const
{
    return lane.withTrimmedLeft(kHeaderW).reduced(0, 7);
}

juce::Rectangle<int> VerlaufTimeline::headerButton(juce::Rectangle<int> lane, int which) const
{
    // on / armed / delete, at the bottom right of the header
    const int w = 22, h = 18;
    const int x = kHeaderW - 12 - (3 - which) * (w + 4);
    return {x, lane.getBottom() - h - 8, w, h};
}

float VerlaufTimeline::yOfValue(juce::Rectangle<int> c, float v) const
{
    return (float)c.getBottom() - v * (float)c.getHeight();
}

float VerlaufTimeline::valueOfY(juce::Rectangle<int> c, float y) const
{
    return juce::jlimit(0.0f, 1.0f, ((float)c.getBottom() - y) / (float)juce::jmax(1, c.getHeight()));
}

int VerlaufTimeline::pointAt(int lane, juce::Point<float> p) const
{
    if (!owner.validLane(lane))
        return -1;
    const auto &l = owner.proc.getAuto().lanes[(size_t)lane];
    const auto c = curveArea(laneRect(lane));
    int best = -1;
    float bestD = 7.0f;
    for (int i = 0; i < (int)l.pts.size(); ++i)
    {
        const auto &pt = l.pts[(size_t)i];
        const float d = p.getDistanceFrom(
            {xOfBeat(VerlaufView::beatOfLocal(l, pt.t)), yOfValue(c, pt.v)});
        if (d < bestD)
        {
            bestD = d;
            best = i;
        }
    }
    return best;
}

double VerlaufTimeline::snap(double beat, bool fine) const
{
    const double g = owner.proc.getAuto().grid;
    if (fine || g <= 0.0)
        return beat;
    return std::round(beat / g) * g;
}

// ---------------------------------------------------------------------------
// timeline painting
// ---------------------------------------------------------------------------
void VerlaufTimeline::paint(juce::Graphics &g)
{
    auto &A = Assets::get();
    const auto &s = owner.proc.getAuto();
    g.setColour(col::panel.withAlpha(0.85f));
    g.fillRect(getLocalBounds().withTrimmedLeft(kHeaderW));

    // grid lines behind everything
    {
        const double gb = s.grid > 0.0 ? s.grid : 1.0;
        const float pxPer = (float)(getWidth() - kHeaderW) / (float)s.viewLen;
        double step = gb;
        while (step * pxPer < 7.0f)
            step *= 2.0;
        const double first = std::floor(s.viewStart / step) * step;
        for (double b = first; b <= s.viewStart + s.viewLen; b += step)
        {
            const float x = xOfBeat(b);
            if (x < (float)kHeaderW)
                continue;
            const bool bar = std::abs(std::fmod(b, 4.0)) < 1.0e-6;
            g.setColour(bar ? col::line.brighter(0.25f) : col::line.withAlpha(0.6f));
            g.fillRect(x, (float)kRulerH, 1.0f, (float)(getHeight() - kRulerH));
        }
    }

    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(0, kRulerH, getWidth(), getHeight() - kRulerH);
        for (int l = 0; l < (int)s.lanes.size(); ++l)
        {
            const auto r = laneRect(l);
            if (r.getBottom() < kRulerH || r.getY() > getHeight())
                continue;
            paintLane(g, l, r);
        }
        if (s.lanes.empty())
        {
            g.setColour(col::faint);
            g.setFont(A.monoFont(13.0f));
            g.drawFittedText(
                juce::String::fromUTF8(
                    "Noch keine Spur. \xe2\x80\x9e+ Spur\xe2\x80\x9c w\xc3\xa4hlt einen Regler "
                    "(oder Rechtsklick auf einen Regler \xe2\x86\x92 Im Verlauf "
                    "automatisieren).\nDann: klicken setzt Punkte, ziehen verschiebt sie, "
                    "Doppelklick l\xc3\xb6scht, Alt + ziehen biegt einen Abschnitt."),
                getLocalBounds().withTrimmedTop(kRulerH).reduced(40, 20),
                juce::Justification::centred, 4);
        }
    }

    paintRuler(g, {0, 0, getWidth(), kRulerH});

    // playhead
    const double now = owner.proc.getAutoNow();
    const float px = xOfBeat(now);
    if (px >= (float)kHeaderW && px <= (float)getWidth())
    {
        const bool rec = owner.proc.isAutoRecord() && owner.proc.isAutoMoving();
        g.setColour(rec ? col::rubric.brighter(0.3f) : col::white.withAlpha(0.85f));
        g.fillRect(px - 0.5f, 0.0f, 1.5f, (float)getHeight());
        juce::Path tri;
        tri.addTriangle(px - 5.0f, 0.0f, px + 5.0f, 0.0f, px, 7.0f);
        g.fillPath(tri);
    }
    // the header column's edge
    g.setColour(col::border);
    g.fillRect((float)kHeaderW - 1.0f, 0.0f, 1.0f, (float)getHeight());
}

void VerlaufTimeline::paintRuler(juce::Graphics &g, juce::Rectangle<int> r)
{
    auto &A = Assets::get();
    const auto &s = owner.proc.getAuto();
    g.setColour(col::bg);
    g.fillRect(r);
    g.setColour(col::border);
    g.fillRect(r.getX(), r.getBottom() - 1, r.getWidth(), 1);
    // labels: whole bars (host) or seconds (own clock), at least ~70 px apart
    const bool host = owner.proc.isAutoHostClock();
    const float pxPerBeat = (float)(getWidth() - kHeaderW) / (float)s.viewLen;
    double step;
    if (host)
    {
        step = 4.0;
        while (step * pxPerBeat < 70.0f)
            step *= 2.0;
        while (step > 1.0 && step * pxPerBeat > 160.0f)
            step *= 0.5;
    }
    else
    {
        const double beatsPerSec = s.tempo / 60.0;
        double sec = 1.0;
        static const double nice[]{0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
        for (double n : nice)
        {
            sec = n;
            if (n * beatsPerSec * pxPerBeat >= 70.0f)
                break;
        }
        step = sec * beatsPerSec;
    }
    g.setFont(A.monoFont(10.0f));
    const double first = std::floor(s.viewStart / step) * step;
    for (double b = first; b <= s.viewStart + s.viewLen + 1.0e-9; b += step)
    {
        const float x = xOfBeat(b);
        if (x < (float)kHeaderW - 0.5f)
            continue;
        g.setColour(col::faint);
        g.fillRect(x, (float)r.getBottom() - 8.0f, 1.0f, 7.0f);
        g.setColour(col::dim);
        g.drawText(owner.timeText(b), juce::Rectangle<float>(x + 3.0f, (float)r.getY() + 3.0f, 80.0f, 14.0f),
                   juce::Justification::centredLeft);
    }
    // left: what the clock is
    g.setColour(col::faint);
    g.setFont(A.romanFont(11.0f));
    g.drawText(host ? juce::String::fromUTF8("Takte \xc2\xb7 Host") : juce::String::fromUTF8("Sekunden \xc2\xb7 eigene Uhr"),
               r.withWidth(kHeaderW).reduced(10, 0), juce::Justification::centredLeft);
}

void VerlaufTimeline::paintLane(juce::Graphics &g, int li, juce::Rectangle<int> r)
{
    auto &A = Assets::get();
    auto &P = owner.proc;
    const auto &s = P.getAuto();
    const auto &l = s.lanes[(size_t)li];
    const bool sel = li == owner.selLane;
    const int param = owner.laneParam(li);
    const int steps = P.autoSteps(param);
    const bool live = s.on && l.on && param >= 0;

    // header
    auto h = r.withWidth(kHeaderW);
    g.setColour(sel ? col::panel.brighter(0.08f) : col::bg.withAlpha(0.6f));
    g.fillRect(h);
    if (sel)
    {
        g.setColour(col::rubric);
        g.fillRect(h.getX(), h.getY() + 6, 2, h.getHeight() - 12);
    }
    auto ht = h.reduced(12, 7);
    g.setColour(l.on ? col::ink : col::faint);
    g.setFont(A.sansFont(13.5f));
    g.drawText(owner.laneName(li), ht.removeFromTop(18), juce::Justification::centredLeft, true);
    g.setFont(A.monoFont(10.0f));
    g.setColour(col::faint);
    juce::String info = param >= 0 ? ModView::paramGroup(P.getParamAt(param)->getParameterID())
                                   : juce::String::fromUTF8("(unbekannter Regler)");
    if (l.loop)
        info << juce::String::fromUTF8(" \xc2\xb7 Schleife ") << beatsText(l.length);
    if (l.reverse)
        info << juce::String::fromUTF8(" \xc2\xb7 r\xc3\xbc"
                                       "ckw.");
    if (std::abs(l.speed - 1.0f) > 1.0e-3f)
        info << juce::String::fromUTF8(" \xc2\xb7 \xc3\x97") << juce::String(l.speed, 2);
    g.drawText(info, ht.removeFromTop(14), juce::Justification::centredLeft, true);
    // the value right now
    if (param >= 0)
    {
        const float an = P.getAutomatedNorm(param);
        const bool touched = P.isAutoTouched(param);
        g.setColour(touched ? col::rubric.brighter(0.4f) : col::dim);
        const juce::String vt =
            touched ? juce::String("Hand") : (std::isnan(an) ? juce::String("-") : owner.valueText(li, an));
        g.drawText(vt, ht.removeFromTop(14), juce::Justification::centredLeft, true);
    }
    // header buttons
    const char *labels[]{"an", "\xe2\x97\x8f", "\xc3\x97"};
    for (int b = 0; b < 3; ++b)
    {
        const auto br = headerButton(r, b);
        const bool active = b == 0 ? l.on : (b == 1 ? l.armed : false);
        const bool over = hoverLane == li && br.contains(hover.toInt());
        if (active)
        {
            g.setColour(b == 1 ? col::rubric : col::ink.withAlpha(0.85f));
            g.fillRect(br);
        }
        g.setColour(over ? col::white : col::border);
        g.drawRect(br, 1);
        g.setColour(active ? (b == 1 ? col::white : col::bg) : (over ? col::white : col::dim));
        g.setFont(b == 0 ? A.monoFont(10.0f) : A.sansFont(12.0f));
        g.drawText(juce::String::fromUTF8(labels[b]), br, juce::Justification::centred);
    }
    // separator
    g.setColour(col::line);
    g.fillRect(r.getX(), r.getBottom() - 1, r.getWidth(), 1);

    // the curve area
    const auto c = curveArea(r);
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(r.withTrimmedLeft(kHeaderW));
    // the lane's region: where its points are (darker beyond it when it loops)
    if (l.loop || l.reverse || std::abs(l.speed - 1.0f) > 1.0e-3f)
    {
        const float x0 = xOfBeat(l.offset), x1 = xOfBeat(l.offset + l.length / juce::jmax(1.0e-6, (double)l.speed));
        g.setColour(col::bg.withAlpha(0.45f));
        g.fillRect(juce::Rectangle<float>((float)kHeaderW, (float)r.getY(), juce::jmax(0.0f, x0 - (float)kHeaderW), (float)r.getHeight()));
        g.fillRect(juce::Rectangle<float>(x1, (float)r.getY(), juce::jmax(0.0f, (float)getWidth() - x1), (float)r.getHeight()));
        g.setColour(col::rubric.withAlpha(0.7f));
        const float dash[]{3.0f, 3.0f};
        g.drawDashedLine({x1, (float)r.getY() + 3.0f, x1, (float)r.getBottom() - 3.0f}, dash, 2, 1.0f);
        g.drawDashedLine({x0, (float)r.getY() + 3.0f, x0, (float)r.getBottom() - 3.0f}, dash, 2, 1.0f);
    }
    // value guides: middle, and the steps of a switch
    g.setColour(col::line.withAlpha(0.8f));
    if (steps >= 2 && steps <= 12)
        for (int k = 0; k < steps; ++k)
            g.fillRect((float)c.getX(), yOfValue(c, (float)k / (float)(steps - 1)), (float)c.getWidth(), 1.0f);
    else
        g.fillRect((float)c.getX(), yOfValue(c, 0.5f), (float)c.getWidth(), 1.0f);

    if (l.pts.empty())
    {
        g.setColour(col::faint);
        g.setFont(A.monoFont(10.0f));
        g.drawText(owner.tool == VerlaufView::toolPoints
                       ? juce::String::fromUTF8("klicken: Punkt setzen")
                       : juce::String::fromUTF8("ziehen: zeichnen"),
                   c.reduced(10, 0), juce::Justification::centredLeft);
    }
    else
    {
        // what is heard: the curve evaluated along the clock (loops, stretch, waves included)
        juce::Path path, fill;
        bool started = false;
        float lastX = 0.0f;
        for (int x = c.getX(); x <= c.getRight(); ++x)
        {
            const float v = autoValue(l, beatOfX((float)x), steps);
            if (std::isnan(v))
                continue;
            const float y = yOfValue(c, v);
            if (!started)
            {
                path.startNewSubPath((float)x, y);
                fill.startNewSubPath((float)x, (float)c.getBottom());
                fill.lineTo((float)x, y);
                started = true;
            }
            else
            {
                path.lineTo((float)x, y);
                fill.lineTo((float)x, y);
            }
            lastX = (float)x;
        }
        if (started)
        {
            fill.lineTo(lastX, (float)c.getBottom());
            fill.closeSubPath();
            g.setColour(col::ink.withAlpha(live ? 0.07f : 0.03f));
            g.fillPath(fill);
            g.setColour(live ? (sel ? col::ink : col::dim) : col::faint);
            g.strokePath(path, juce::PathStrokeType(sel ? 1.5f : 1.1f));
        }
        // the selected segment
        if (sel && owner.selPoint >= 0 && owner.selPoint < (int)l.pts.size())
        {
            const auto &a = l.pts[(size_t)owner.selPoint];
            const double b0 = VerlaufView::beatOfLocal(l, a.t);
            const double b1 = owner.selPoint + 1 < (int)l.pts.size()
                                  ? VerlaufView::beatOfLocal(l, l.pts[(size_t)owner.selPoint + 1].t)
                                  : b0 + 0.0;
            const float xa = xOfBeat(juce::jmin(b0, b1)), xb = xOfBeat(juce::jmax(b0, b1));
            g.setColour(col::rubric.withAlpha(0.14f));
            g.fillRect(juce::Rectangle<float>(xa, (float)c.getY(), juce::jmax(2.0f, xb - xa), (float)c.getHeight()));
        }
        // the points (first pass)
        for (int i = 0; i < (int)l.pts.size(); ++i)
        {
            const auto &pt = l.pts[(size_t)i];
            const float x = xOfBeat(VerlaufView::beatOfLocal(l, pt.t)), y = yOfValue(c, pt.v);
            if (x < (float)kHeaderW - 6.0f || x > (float)getWidth() + 6.0f)
                continue;
            const bool hot = (hoverLane == li && hoverPoint == i) || (dragLane == li && dragPoint == i);
            const bool selected = sel && i == owner.selPoint;
            g.setColour(selected ? col::rubric.brighter(0.3f) : (hot ? col::white : col::ink));
            drawLozenge(g, {x, y}, hot || selected ? 4.5f : 3.5f, hot || selected ? 6.0f : 5.0f);
            if (pt.shape != asCurve && sel)
            {
                g.setColour(col::faint);
                g.setFont(A.monoFont(9.0f));
                g.drawText(autoShapeNames()[pt.shape], juce::Rectangle<float>(x + 6.0f, (float)c.getY(), 60.0f, 12.0f),
                           juce::Justification::centredLeft);
            }
        }
    }
    // the pencil stroke being drawn
    if (dragLane == li && drag == Drag::pencil && stroke.size() > 1)
    {
        juce::Path p;
        for (size_t i = 0; i < stroke.size(); ++i)
        {
            const float x = xOfBeat(VerlaufView::beatOfLocal(l, stroke[i].t)), y = yOfValue(c, stroke[i].v);
            if (i == 0)
                p.startNewSubPath(x, y);
            else
                p.lineTo(x, y);
        }
        g.setColour(col::rubric.brighter(0.4f));
        g.strokePath(p, juce::PathStrokeType(1.6f));
    }
    // hover readout
    if (hoverLane == li && hover.x > (float)kHeaderW && drag == Drag::none)
    {
        const float v = valueOfY(c, hover.y);
        g.setColour(col::dim);
        g.setFont(A.monoFont(9.5f));
        const auto txt = owner.timeText(beatOfX(hover.x)) + "  " + owner.valueText(li, v);
        g.drawText(txt, juce::Rectangle<float>(hover.x + 10.0f, hover.y - 16.0f, 200.0f, 13.0f),
                   juce::Justification::centredLeft);
    }
    else if (dragLane == li && drag == Drag::point && dragPoint >= 0 && dragPoint < (int)l.pts.size())
    {
        const auto &pt = l.pts[(size_t)dragPoint];
        const float x = xOfBeat(VerlaufView::beatOfLocal(l, pt.t)), y = yOfValue(c, pt.v);
        g.setColour(col::white);
        g.setFont(A.monoFont(9.5f));
        g.drawText(owner.timeText(VerlaufView::beatOfLocal(l, pt.t)) + "  " + owner.valueText(li, pt.v),
                   juce::Rectangle<float>(x + 10.0f, y - 16.0f, 200.0f, 13.0f), juce::Justification::centredLeft);
    }
}

// ---------------------------------------------------------------------------
// timeline mouse
// ---------------------------------------------------------------------------
void VerlaufTimeline::mouseMove(const juce::MouseEvent &e)
{
    hover = e.position;
    hoverLane = laneAt(e.y);
    hoverPoint = hoverLane >= 0 && e.x > kHeaderW ? pointAt(hoverLane, e.position) : -1;
    setMouseCursor(hoverPoint >= 0 ? juce::MouseCursor::DraggingHandCursor
                   : (e.x > kHeaderW && hoverLane >= 0 && owner.tool != VerlaufView::toolPoints)
                       ? juce::MouseCursor::CrosshairCursor
                       : juce::MouseCursor::NormalCursor);
    repaint();
}

void VerlaufTimeline::mouseExit(const juce::MouseEvent &)
{
    hover = {-1.0f, -1.0f};
    hoverLane = hoverPoint = -1;
    repaint();
}

void VerlaufTimeline::mouseDown(const juce::MouseEvent &e)
{
    auto &s = owner.set();
    drag = Drag::none;
    dragLane = dragPoint = -1;
    if (e.y < kRulerH)
    {
        if (e.x > kHeaderW)
        {
            // the ruler: the own clock jumps there (the host's position is the host's)
            drag = Drag::seek;
            owner.proc.autoSeek(juce::jmax(0.0, snap(beatOfX((float)e.x), e.mods.isAltDown())));
        }
        return;
    }
    const int li = laneAt(e.y);
    if (li < 0)
    {
        if (e.mods.isMiddleButtonDown())
        {
            drag = Drag::pan;
            panStart = s.viewStart;
        }
        return;
    }
    const auto r = laneRect(li);
    if (e.x < kHeaderW)
    {
        // header buttons
        for (int b = 0; b < 3; ++b)
            if (headerButton(r, b).contains(e.getPosition()))
            {
                owner.pushUndo();
                auto &l = s.lanes[(size_t)li];
                if (b == 0)
                    l.on = !l.on;
                else if (b == 1)
                    l.armed = !l.armed;
                else
                {
                    s.lanes.erase(s.lanes.begin() + li);
                    if (owner.selLane >= (int)s.lanes.size())
                        owner.selLane = (int)s.lanes.size() - 1;
                    owner.selPoint = -1;
                }
                owner.changed();
                return;
            }
        if (e.mods.isPopupMenu())
        {
            owner.selLane = li;
            owner.showLaneMenu(li);
            return;
        }
        if (owner.selLane != li)
            owner.selPoint = -1;
        owner.selLane = li;
        owner.syncControls();
        repaint();
        return;
    }
    if (e.mods.isMiddleButtonDown())
    {
        drag = Drag::pan;
        panStart = s.viewStart;
        return;
    }
    if (owner.selLane != li)
        owner.selPoint = -1;
    owner.selLane = li;
    auto &l = s.lanes[(size_t)li];
    const auto c = curveArea(laneRect(li));
    const int hit = pointAt(li, e.position);
    if (e.mods.isPopupMenu())
    {
        if (hit >= 0)
            owner.showPointMenu(li, hit);
        else
        {
            // the segment under the mouse
            const int seg = autoFindSegment(l.pts, VerlaufView::localOf(l, beatOfX((float)e.x)));
            owner.selPoint = seg;
            owner.syncControls();
            owner.showPointMenu(li, seg);
        }
        return;
    }
    dragLane = li;
    const bool fine = e.mods.isAltDown() && owner.tool != VerlaufView::toolPoints;
    if (owner.tool == VerlaufView::toolPencil)
    {
        owner.pushUndo();
        drag = Drag::pencil;
        stroke.clear();
        stroke.push_back({VerlaufView::localOf(l, beatOfX((float)e.x)), valueOfY(c, (float)e.y)});
        repaint();
        return;
    }
    if (owner.tool == VerlaufView::toolStairs)
    {
        owner.pushUndo();
        drag = Drag::stairs;
        lastStairCell = std::numeric_limits<double>::quiet_NaN();
        mouseDrag(e);
        juce::ignoreUnused(fine);
        return;
    }
    // points
    if (hit >= 0)
    {
        owner.pushUndo();
        owner.selPoint = hit;
        dragPoint = hit;
        drag = Drag::point;
        owner.syncControls();
        repaint();
        return;
    }
    const int seg = autoFindSegment(l.pts, VerlaufView::localOf(l, beatOfX((float)e.x)));
    if (e.mods.isAltDown() && seg >= 0)
    {
        // Alt + drag on a segment: bend it
        owner.pushUndo();
        owner.selPoint = seg;
        drag = Drag::bend;
        bendStart = l.pts[(size_t)seg].bend;
        owner.syncControls();
        return;
    }
    // a new point (it takes the shape of the segment it lands in)
    owner.pushUndo();
    AutoPoint pt;
    pt.t = VerlaufView::localOf(l, snap(beatOfX((float)e.x), e.mods.isShiftDown()));
    pt.v = valueOfY(c, (float)e.y);
    if (const int steps = owner.proc.autoSteps(owner.laneParam(li)); steps >= 2)
    {
        pt.v = autoQuantise(pt.v, steps);
        pt.shape = asStep;
    }
    if (seg >= 0)
    {
        const auto &a = l.pts[(size_t)seg];
        pt.shape = a.shape;
        pt.rate = a.rate;
        pt.depth = a.depth;
        pt.bend = a.bend;
    }
    l.pts.push_back(pt);
    autoSort(l);
    for (int i = 0; i < (int)l.pts.size(); ++i)
        if (l.pts[(size_t)i].t == pt.t && l.pts[(size_t)i].v == pt.v)
            dragPoint = i;
    owner.selPoint = dragPoint;
    drag = Drag::point;
    owner.changed();
}

void VerlaufTimeline::mouseDrag(const juce::MouseEvent &e)
{
    auto &s = owner.set();
    hover = e.position;
    if (drag == Drag::seek)
    {
        owner.proc.autoSeek(juce::jmax(0.0, snap(beatOfX((float)e.x), e.mods.isAltDown())));
        return;
    }
    if (drag == Drag::pan)
    {
        const double perPx = s.viewLen / juce::jmax(1, getWidth() - kHeaderW);
        s.viewStart = juce::jmax(0.0, panStart - e.getDistanceFromDragStartX() * perPx);
        repaint();
        return;
    }
    if (!owner.validLane(dragLane))
        return;
    auto &l = s.lanes[(size_t)dragLane];
    const auto c = curveArea(laneRect(dragLane));
    const int steps = owner.proc.autoSteps(owner.laneParam(dragLane));
    if (drag == Drag::point && dragPoint >= 0 && dragPoint < (int)l.pts.size())
    {
        auto &pt = l.pts[(size_t)dragPoint];
        double t = VerlaufView::localOf(l, snap(beatOfX((float)e.x), e.mods.isShiftDown()));
        // stays between its neighbours
        const double lo = dragPoint > 0 ? l.pts[(size_t)dragPoint - 1].t + 1.0e-4 : -1.0e9;
        const double hi = dragPoint + 1 < (int)l.pts.size() ? l.pts[(size_t)dragPoint + 1].t - 1.0e-4 : 1.0e9;
        if (e.mods.isCommandDown())
            t = pt.t; // Ctrl: only the value
        pt.t = juce::jlimit(lo, juce::jmax(lo, hi), t);
        float v = valueOfY(c, (float)e.y);
        if (steps >= 2)
            v = autoQuantise(v, steps);
        pt.v = v;
        owner.changed();
        return;
    }
    if (drag == Drag::bend && owner.selPoint >= 0 && owner.selPoint < (int)l.pts.size())
    {
        auto &a = l.pts[(size_t)owner.selPoint];
        const bool rising = owner.selPoint + 1 < (int)l.pts.size() && l.pts[(size_t)owner.selPoint + 1].v >= a.v;
        a.bend = juce::jlimit(-1.0f, 1.0f, bendStart + (rising ? -1.0f : 1.0f) * (float)e.getDistanceFromDragStartY() / 80.0f);
        owner.changed();
        return;
    }
    if (drag == Drag::pencil)
    {
        stroke.push_back({VerlaufView::localOf(l, beatOfX((float)e.x)), valueOfY(c, (float)e.y)});
        repaint();
        return;
    }
    if (drag == Drag::stairs)
    {
        // one step per grid cell (the grid's own size, 1/4 without a grid)
        const double gsz = s.grid > 0.0 ? s.grid : 1.0;
        const double cell = std::floor(beatOfX((float)e.x) / gsz);
        float v = valueOfY(c, (float)e.y);
        if (steps >= 2)
            v = autoQuantise(v, steps);
        double t0 = VerlaufView::localOf(l, cell * gsz), t1 = VerlaufView::localOf(l, (cell + 1.0) * gsz);
        if (t1 < t0)
            std::swap(t0, t1);
        // the value after the cell stays as it was
        const float after = autoValueAtLocal(l, t1);
        AutoPoint a;
        a.t = t0;
        a.v = v;
        a.shape = asStep;
        std::vector<AutoPoint> with{a};
        const bool hasNext = std::any_of(l.pts.begin(), l.pts.end(), [&](const AutoPoint &p) { return p.t >= t1 - 1.0e-6; });
        if (!hasNext && !std::isnan(after) && !std::isnan(lastStairCell) && lastStairCell < cell)
        {
            // drawing on to the right: nothing to keep after the cell
        }
        else if (!hasNext && !std::isnan(after))
        {
            AutoPoint b;
            b.t = t1;
            b.v = after;
            b.shape = asStep;
            with.push_back(b);
        }
        autoReplaceRange(l, t0, t1 - 1.0e-6, with);
        lastStairCell = cell;
        owner.changed();
    }
}

void VerlaufTimeline::mouseUp(const juce::MouseEvent &)
{
    auto &s = owner.set();
    if (drag == Drag::pencil && owner.validLane(dragLane) && stroke.size() >= 1)
    {
        auto &l = s.lanes[(size_t)dragLane];
        const int steps = owner.proc.autoSteps(owner.laneParam(dragLane));
        std::stable_sort(stroke.begin(), stroke.end(), [](const AutoPoint &a, const AutoPoint &b) { return a.t < b.t; });
        const auto c = curveArea(laneRect(dragLane));
        // tolerance of about one pixel: one beat counts as (pixels per beat / lane height)
        const double pxPerBeat = (double)(getWidth() - kHeaderW) / s.viewLen;
        auto thin = autoThin(stroke, 1.2f / (float)juce::jmax(1, c.getHeight()), pxPerBeat / juce::jmax(1, c.getHeight()));
        for (auto &p : thin)
            if (steps >= 2)
            {
                p.v = autoQuantise(p.v, steps);
                p.shape = asStep;
            }
        autoReplaceRange(l, stroke.front().t, stroke.back().t, thin);
        owner.selPoint = -1;
        owner.changed();
    }
    stroke.clear();
    drag = Drag::none;
    dragLane = dragPoint = -1;
    owner.syncControls();
    repaint();
}

void VerlaufTimeline::mouseDoubleClick(const juce::MouseEvent &e)
{
    const int li = laneAt(e.y);
    if (li < 0 || e.x < kHeaderW || e.y < kRulerH)
        return;
    const int hit = pointAt(li, e.position);
    if (hit >= 0)
    {
        auto &l = owner.set().lanes[(size_t)li];
        // (the mouse down before has pushed an undo step already)
        l.pts.erase(l.pts.begin() + hit);
        owner.selPoint = -1;
        owner.changed();
    }
}

void VerlaufTimeline::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w)
{
    auto &s = owner.set();
    const float d = (std::abs(w.deltaY) > std::abs(w.deltaX) ? w.deltaY : -w.deltaX);
    if (e.mods.isCommandDown() && e.x > kHeaderW)
    {
        // zoom around the mouse
        const double at = beatOfX((float)e.x);
        const double k = std::pow(0.8, d * 4.0);
        const double len = juce::jlimit(1.0, 4096.0, s.viewLen * k);
        s.viewStart = juce::jmax(0.0, at - (at - s.viewStart) * (len / s.viewLen));
        s.viewLen = len;
    }
    else if (e.mods.isShiftDown() || std::abs(w.deltaX) > std::abs(w.deltaY))
        s.viewStart = juce::jmax(0.0, s.viewStart - d * s.viewLen * 0.25);
    else
    {
        const int maxScroll = juce::jmax(0, contentHeight() - (getHeight() - kRulerH));
        scrollY = juce::jlimit(0, maxScroll, scrollY - juce::roundToInt(d * 120.0f));
    }
    repaint();
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------
VerlaufView::VerlaufView(RysumProcessor &p) : proc(p)
{
    setWantsKeyboardFocus(true);
    auto toggle = [this](juce::TextButton &b, const juce::String &tip)
    {
        b.setTooltip(tip);
        addAndMakeVisible(b);
    };
    toggle(onButton, juce::String::fromUTF8("Alle Spuren an / aus (aus: die Regler gelten wie von Hand gestellt)"));
    onButton.setClickingTogglesState(true);
    onButton.onClick = [this]
    {
        set().on = onButton.getToggleState();
        changed();
    };
    toggle(clockHost, juce::String::fromUTF8("Die Uhr folgt der Songposition des Hosts (Takte). Ohne Host "
                                             "(Standalone) l\xc3\xa4uft die eigene Uhr."));
    toggle(clockOwn, juce::String::fromUTF8("Die eigene Uhr des Plugins, unabh\xc3\xa4ngig vom Host: \xe2\x96\xb6 / "
                                            "\xe2\x96\xa0 / \xe2\x8f\xae oder die erste Taste starten sie."));
    clockHost.setRadioGroupId(7101);
    clockOwn.setRadioGroupId(7101);
    clockHost.onClick = [this]
    {
        set().clock = 0;
        changed();
    };
    clockOwn.onClick = [this]
    {
        set().clock = 1;
        changed();
    };
    toggle(playButton, "Eigene Uhr starten");
    toggle(stopButton, "Eigene Uhr anhalten");
    toggle(homeButton, juce::String::fromUTF8("Zur\xc3\xbc"
                                              "ck an den Anfang"));
    playButton.onClick = [this] { proc.autoPlay(true); };
    stopButton.onClick = [this]
    {
        // a second stop goes back to the start
        if (!proc.isAutoOwnRunning())
            proc.autoSeek(0.0);
        proc.autoPlay(false);
    };
    homeButton.onClick = [this] { proc.autoSeek(0.0); };
    toggle(keyStart, juce::String::fromUTF8("Die eigene Uhr startet mit der ersten Taste"));
    keyStart.setClickingTogglesState(true);
    keyStart.onClick = [this]
    {
        set().startOnKey = keyStart.getToggleState();
        changed();
    };
    toggle(recButton, juce::String::fromUTF8("Aufnahme: bewegt man w\xc3\xa4hrend die Uhr l\xc3\xa4uft den Regler "
                                             "einer scharfen Spur (\xe2\x97\x8f im Kopf der Spur), wird die Bewegung "
                                             "in die Spur geschrieben. Ohne Aufnahme gibt die Spur nach, solange man "
                                             "den Regler h\xc3\xa4lt."));
    recButton.setClickingTogglesState(true);
    recButton.onClick = [this]
    {
        if (recButton.getToggleState())
            pushUndo();
        proc.setAutoRecord(recButton.getToggleState());
    };
    toggle(toolPts, juce::String::fromUTF8("Punkte: klicken setzt, ziehen verschiebt (Umschalt: ohne Raster, Strg: nur "
                                           "der Wert), Doppelklick l\xc3\xb6scht, Alt + ziehen biegt einen Abschnitt, "
                                           "Rechtsklick: Form"));
    toggle(toolPen, juce::String::fromUTF8("Stift: freihand zeichnen, ersetzt die Punkte unter dem Strich"));
    toggle(toolStair, juce::String::fromUTF8("Treppe: je Rasterfeld eine Stufe zeichnen"));
    for (auto *b : {&toolPts, &toolPen, &toolStair})
        b->setRadioGroupId(7102);
    toolPts.onClick = [this] { tool = toolPoints; };
    toolPen.onClick = [this] { tool = toolPencil; };
    toolStair.onClick = [this] { tool = toolStairs; };
    toolPts.setToggleState(true, juce::dontSendNotification);
    toggle(zoomIn, juce::String::fromUTF8("N\xc3\xa4her (Strg + Mausrad)"));
    toggle(zoomOut, "Weiter (Strg + Mausrad)");
    toggle(zoomAll, juce::String::fromUTF8("Alle Punkte zeigen"));
    auto zoom = [this](double k)
    {
        auto &s = set();
        const double mid = s.viewStart + s.viewLen * 0.5;
        s.viewLen = juce::jlimit(1.0, 4096.0, s.viewLen * k);
        s.viewStart = juce::jmax(0.0, mid - s.viewLen * 0.5);
        timeline.repaint();
    };
    zoomIn.onClick = [zoom] { zoom(0.5); };
    zoomOut.onClick = [zoom] { zoom(2.0); };
    zoomAll.onClick = [this]
    {
        auto &s = set();
        double end = 16.0;
        for (auto &l : s.lanes)
        {
            for (auto &pt : l.pts)
                end = juce::jmax(end, beatOfLocal(l, pt.t));
            end = juce::jmax(end, l.offset + l.length / juce::jmax(1.0e-6, (double)l.speed));
        }
        s.viewStart = 0.0;
        s.viewLen = juce::jlimit(4.0, 4096.0, end * 1.05);
        timeline.repaint();
    };
    toggle(undoButton, juce::String::fromUTF8("Letzte \xc3\x84nderung zur\xc3\xbc"
                                              "cknehmen (Strg+Z, Strg+Y: wiederholen)"));
    undoButton.onClick = [this] { undo(); };
    toggle(addButton, juce::String::fromUTF8("Eine Spur f\xc3\xbcr einen Regler anlegen"));
    addButton.onClick = [this] { showAddMenu(); };
    gridBox.addItemList(gridNames(), 1);
    gridBox.onChange = [this]
    {
        if (updating)
            return;
        set().grid = kGridBeats[juce::jlimit(0, 7, gridBox.getSelectedItemIndex())];
        timeline.repaint();
    };
    gridBox.setTooltip(juce::String::fromUTF8("Raster f\xc3\xbcr Punkte und Treppen (Umschalt beim Ziehen: ohne)"));
    addAndMakeVisible(gridBox);

    tempoRow.format = [](double v) { return juce::String(v, 1) + " BPM"; };
    tempoRow.slider.setTooltip(juce::String::fromUTF8("Tempo der eigenen Uhr: 60 BPM = ein Schlag je Sekunde"));
    tempoRow.onDragStart = [this] { pushUndo(); };
    tempoRow.onChange = [this](double v)
    {
        if (updating)
            return;
        set().tempo = v;
        changed();
    };
    addAndMakeVisible(tempoRow);

    // lane settings
    auto beatsFmt = [](double v) { return beatsText(v) + juce::String::fromUTF8(" Schl\xc3\xa4ge"); };
    lenRow.format = beatsFmt;
    offsetRow.format = beatsFmt;
    speedRow.format = [](double v) { return juce::String::fromUTF8("\xc3\x97 ") + juce::String(v, v < 1.0 ? 3 : 2); };
    auto laneEdit = [this](FreeRow &row, std::function<void(AutoLane &, double)> fn)
    {
        row.onDragStart = [this] { pushUndo(); };
        row.onChange = [this, fn](double v)
        {
            if (updating || !validLane(selLane))
                return;
            fn(set().lanes[(size_t)selLane], v);
            changed();
        };
        addAndMakeVisible(row);
    };
    laneEdit(lenRow, [](AutoLane &l, double v) { l.length = v; });
    laneEdit(speedRow, [](AutoLane &l, double v) { l.speed = (float)v; });
    laneEdit(offsetRow, [](AutoLane &l, double v) { l.offset = v; });
    lenRow.slider.setTooltip(juce::String::fromUTF8("Die L\xc3\xa4nge der Schleife (und des r\xc3\xbc"
                                                    "ckw\xc3\xa4rts gelesenen Bereichs), in Schl\xc3\xa4gen der Spur"));
    speedRow.slider.setTooltip(juce::String::fromUTF8("Zeitdehnung: \xc3\x97 2 = doppelt so schnell, \xc3\x97 0,5 = halb so "
                                                      "schnell. Spuren mit verschiedenen L\xc3\xa4ngen und Tempi "
                                                      "laufen gegeneinander."));
    offsetRow.slider.setTooltip(juce::String::fromUTF8("Wo die Spur auf der Uhr beginnt"));
    for (auto *b : {&loopButton, &revButton})
    {
        b->setClickingTogglesState(true);
        addAndMakeVisible(*b);
    }
    loopButton.setTooltip(juce::String::fromUTF8("Die Spur wiederholt ihre L\xc3\xa4nge immer wieder"));
    revButton.setTooltip(juce::String::fromUTF8("Die Spur wird von hinten gelesen"));
    loopButton.onClick = [this]
    {
        if (!validLane(selLane))
            return;
        pushUndo();
        set().lanes[(size_t)selLane].loop = loopButton.getToggleState();
        changed();
    };
    revButton.onClick = [this]
    {
        if (!validLane(selLane))
            return;
        pushUndo();
        set().lanes[(size_t)selLane].reverse = revButton.getToggleState();
        changed();
    };
    clearButton.setTooltip(juce::String::fromUTF8("Alle Punkte dieser Spur l\xc3\xb6schen"));
    clearButton.onClick = [this]
    {
        if (!validLane(selLane))
            return;
        pushUndo();
        set().lanes[(size_t)selLane].pts.clear();
        selPoint = -1;
        changed();
    };
    addAndMakeVisible(clearButton);

    // segment settings
    for (int k = 0; k < kNumAutoShapes; ++k)
    {
        auto &b = shapeButtons[(size_t)k];
        b.setButtonText(autoShapeNames()[k]);
        b.setRadioGroupId(7103);
        b.onClick = [this, k]
        {
            if (updating || !validLane(selLane))
                return;
            auto &l = set().lanes[(size_t)selLane];
            if (selPoint < 0 || selPoint >= (int)l.pts.size())
                return;
            pushUndo();
            l.pts[(size_t)selPoint].shape = k;
            changed();
        };
        addAndMakeVisible(b);
    }
    shapeButtons[asCurve].setTooltip("Gerade oder gebogene Linie zum Punkt danach");
    shapeButtons[asStep].setTooltip(juce::String::fromUTF8("H\xc3\xa4lt den Wert bis zum Punkt danach, dann springt er"));
    shapeButtons[asWave].setTooltip(juce::String::fromUTF8("Die Linie plus eine Sinuswelle (Tempo: Wellen je Schlag)"));
    shapeButtons[asJitter].setTooltip(juce::String::fromUTF8("Die Linie plus weiches zuf\xc3\xa4lliges Wandern "
                                                             "(jedes Mal gleich)"));
    shapeButtons[asStairs].setTooltip(juce::String::fromUTF8("Die Linie in Stufen (Tempo: Stufen je Schlag)"));
    bendRow.format = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + " %"; };
    rateRow.format = [](double v) { return juce::String(v, v < 1.0 ? 2 : 1) + juce::String::fromUTF8(" / Schlag"); };
    depthRow.format = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + " %"; };
    auto segEdit = [this](FreeRow &row, std::function<void(AutoPoint &, double)> fn)
    {
        row.onDragStart = [this] { pushUndo(); };
        row.onChange = [this, fn](double v)
        {
            if (updating || !validLane(selLane))
                return;
            auto &l = set().lanes[(size_t)selLane];
            if (selPoint < 0 || selPoint >= (int)l.pts.size())
                return;
            fn(l.pts[(size_t)selPoint], v);
            changed();
        };
        addAndMakeVisible(row);
    };
    segEdit(bendRow, [](AutoPoint &p, double v) { p.bend = (float)v; });
    segEdit(rateRow, [](AutoPoint &p, double v) { p.rate = (float)v; });
    segEdit(depthRow, [](AutoPoint &p, double v) { p.depth = (float)v; });

    addAndMakeVisible(timeline);
    syncControls();
    startTimerHz(30);
}

VerlaufView::~VerlaufView() = default;

int VerlaufView::laneParam(int lane) const
{
    if (!validLane(lane))
        return -1;
    const int p = proc.getParamIndex(proc.getAuto().lanes[(size_t)lane].paramId);
    return proc.isAutoTarget(p) ? p : -1;
}

juce::String VerlaufView::laneName(int lane) const
{
    const int p = laneParam(lane);
    if (p < 0)
        return validLane(lane) ? proc.getAuto().lanes[(size_t)lane].paramId : juce::String();
    return proc.getParamAt(p)->getName(64);
}

juce::String VerlaufView::valueText(int lane, float v) const
{
    const int p = laneParam(lane);
    if (p < 0)
        return juce::String(juce::roundToInt(v * 100.0f)) + " %";
    auto *prm = proc.getParamAt(p);
    return prm->getText(autoQuantise(v, proc.autoSteps(p)), 64);
}

juce::String VerlaufView::timeText(double beats) const
{
    if (proc.isAutoHostClock())
    {
        // bars . beats (4/4)
        const double b = juce::jmax(0.0, beats);
        const int bar = (int)std::floor(b / 4.0) + 1;
        const double inBar = b - (bar - 1) * 4.0;
        if (std::abs(inBar - std::round(inBar)) < 1.0e-6 && (int)std::round(inBar) == 0)
            return juce::String(bar);
        return juce::String(bar) + "." + juce::String(inBar + 1.0, std::abs(inBar - std::round(inBar)) < 1.0e-6 ? 0 : 2);
    }
    const double sec = juce::jmax(0.0, beats) * 60.0 / juce::jmax(1.0, proc.getAuto().tempo);
    const int m = (int)(sec / 60.0);
    const double r = sec - m * 60.0;
    const bool whole = std::abs(r - std::round(r)) < 1.0e-6;
    return juce::String(m) + ":" + (r < 10.0 ? "0" : "") + juce::String(r, whole ? 0 : 2);
}

void VerlaufView::changed()
{
    proc.autoChanged();
    shownVersion = proc.getAutoVersion();
    syncControls();
    timeline.repaint();
    repaint(titleBox);
}

void VerlaufView::pushUndo()
{
    undoStack.push_back(set());
    if (undoStack.size() > 100)
        undoStack.erase(undoStack.begin());
    redoStack.clear();
}

void VerlaufView::restore(const AutoSet &from)
{
    // the lanes and the clock come back; where one looks (zoom, scroll, grid) stays
    auto &s = set();
    const double vs = s.viewStart, vl = s.viewLen, gr = s.grid;
    s = from;
    s.viewStart = vs;
    s.viewLen = vl;
    s.grid = gr;
}

void VerlaufView::undo()
{
    if (undoStack.empty())
        return;
    redoStack.push_back(set());
    restore(undoStack.back());
    undoStack.pop_back();
    if (!validLane(selLane))
        selLane = (int)set().lanes.size() - 1;
    selPoint = -1;
    changed();
}

void VerlaufView::redo()
{
    if (redoStack.empty())
        return;
    undoStack.push_back(set());
    restore(redoStack.back());
    redoStack.pop_back();
    if (!validLane(selLane))
        selLane = (int)set().lanes.size() - 1;
    selPoint = -1;
    changed();
}

bool VerlaufView::keyPressed(const juce::KeyPress &k)
{
    if (k == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0))
    {
        undo();
        return true;
    }
    if (k == juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0) ||
        k == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
    {
        redo();
        return true;
    }
    if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) && validLane(selLane))
    {
        auto &l = set().lanes[(size_t)selLane];
        if (selPoint >= 0 && selPoint < (int)l.pts.size())
        {
            pushUndo();
            l.pts.erase(l.pts.begin() + selPoint);
            selPoint = -1;
            changed();
            return true;
        }
    }
    if (k == juce::KeyPress::spaceKey)
    {
        proc.autoPlay(!proc.isAutoOwnRunning());
        return true;
    }
    return false;
}

void VerlaufView::addLaneFor(const juce::String &paramId)
{
    auto &s = set();
    for (int l = 0; l < (int)s.lanes.size(); ++l)
        if (s.lanes[(size_t)l].paramId == paramId)
        {
            selLane = l;
            selPoint = -1;
            changed();
            return;
        }
    if ((int)s.lanes.size() >= kMaxAutoLanes)
        return;
    const int p = proc.getParamIndex(paramId);
    if (!proc.isAutoTarget(p))
        return;
    pushUndo();
    AutoLane l;
    l.paramId = paramId;
    // it starts where the control stands: one point at the beginning
    AutoPoint a;
    a.t = 0.0;
    a.v = proc.getParamAt(p)->getValue();
    if (proc.autoSteps(p) >= 2)
        a.shape = asStep;
    l.pts.push_back(a);
    s.lanes.push_back(std::move(l));
    selLane = (int)s.lanes.size() - 1;
    selPoint = 0;
    changed();
    // the new lane in view
    timeline.scrollY = juce::jmax(0, timeline.contentHeight() - (timeline.getHeight() - VerlaufTimeline::kRulerH));
}

void VerlaufView::showAddMenu()
{
    juce::PopupMenu menu;
    const int base = 1;
    const juce::StringArray groups{"Register",  juce::String::fromUTF8("Traktur \xc2\xb7 Ausgang"),
                                   "Verfall",   "Klang",
                                   "Hall",      "Saal",
                                   "Eingang",   "Effekte",
                                   "Modulation"};
    std::set<int> taken;
    for (auto &l : set().lanes)
        taken.insert(proc.getParamIndex(l.paramId));
    juce::PopupMenu switches;
    for (int i = 0; i < proc.getNumParams(); ++i)
        if (proc.isAutoTarget(i) && !proc.isModTarget(i))
            switches.addItem(base + i, proc.getParamAt(i)->getName(64), true, taken.count(i) > 0);
    menu.addSectionHeader(juce::String::fromUTF8("Eine Spur f\xc3\xbcr \xe2\x80\xa6"));
    if (switches.getNumItems() > 0)
        menu.addSubMenu(juce::String::fromUTF8("Schalter (Register, Bewegung, Freeze \xe2\x80\xa6)"), switches);
    for (auto &grp : groups)
    {
        juce::PopupMenu sub;
        for (int i = 0; i < proc.getNumParams(); ++i)
            if (proc.isModTarget(i) && proc.isAutoTarget(i) &&
                ModView::paramGroup(proc.getParamAt(i)->getParameterID()) == grp)
                sub.addItem(base + i, proc.getParamAt(i)->getName(64), true, taken.count(i) > 0);
        if (sub.getNumItems() > 0)
            menu.addSubMenu(grp, sub);
    }
    if ((int)set().lanes.size() >= kMaxAutoLanes)
    {
        menu.clear();
        menu.addItem(-1, juce::String::fromUTF8("Alle 16 Spuren belegt"), false);
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addButton),
                       [safe = juce::Component::SafePointer<VerlaufView>(this), base](int r)
                       {
                           if (safe == nullptr || r < base)
                               return;
                           safe->addLaneFor(safe->proc.getParamAt(r - base)->getParameterID());
                       });
}

void VerlaufView::showLaneMenu(int lane)
{
    if (!validLane(lane))
        return;
    juce::PopupMenu m;
    const auto &l = set().lanes[(size_t)lane];
    m.addItem(1, "Spur an", true, l.on);
    m.addItem(2, juce::String::fromUTF8("Aufnahmebereit (\xe2\x97\x8f)"), true, l.armed);
    m.addItem(3, "Schleife", true, l.loop);
    m.addItem(4, juce::String::fromUTF8("R\xc3\xbc"
                                        "ckw\xc3\xa4rts"),
              true, l.reverse);
    m.addSeparator();
    m.addItem(5, "Punkte leeren");
    m.addItem(6, juce::String::fromUTF8("Spur l\xc3\xb6schen"));
    m.addItem(7, "Spur nach oben", lane > 0);
    m.addItem(8, "Spur nach unten", lane + 1 < (int)set().lanes.size());
    m.showMenuAsync(juce::PopupMenu::Options(),
                    [safe = juce::Component::SafePointer<VerlaufView>(this), lane](int r)
                    {
                        if (safe == nullptr || r <= 0 || !safe->validLane(lane))
                            return;
                        auto &s = safe->set();
                        safe->pushUndo();
                        auto &l = s.lanes[(size_t)lane];
                        switch (r)
                        {
                        case 1: l.on = !l.on; break;
                        case 2: l.armed = !l.armed; break;
                        case 3: l.loop = !l.loop; break;
                        case 4: l.reverse = !l.reverse; break;
                        case 5: l.pts.clear(); safe->selPoint = -1; break;
                        case 6:
                            s.lanes.erase(s.lanes.begin() + lane);
                            safe->selLane = juce::jmin(safe->selLane, (int)s.lanes.size() - 1);
                            safe->selPoint = -1;
                            break;
                        case 7:
                            std::swap(s.lanes[(size_t)lane], s.lanes[(size_t)lane - 1]);
                            safe->selLane = lane - 1;
                            break;
                        case 8:
                            std::swap(s.lanes[(size_t)lane], s.lanes[(size_t)lane + 1]);
                            safe->selLane = lane + 1;
                            break;
                        default: break;
                        }
                        safe->changed();
                    });
}

void VerlaufView::showPointMenu(int lane, int point)
{
    if (!validLane(lane))
        return;
    const auto &l = set().lanes[(size_t)lane];
    if (point < 0 || point >= (int)l.pts.size())
    {
        showLaneMenu(lane);
        return;
    }
    selPoint = point;
    syncControls();
    juce::PopupMenu m;
    m.addSectionHeader(juce::String::fromUTF8("Abschnitt ab diesem Punkt"));
    for (int k = 0; k < kNumAutoShapes; ++k)
        m.addItem(10 + k, autoShapeNames()[k], true, l.pts[(size_t)point].shape == k);
    m.addItem(20, "Biegung aufheben", std::abs(l.pts[(size_t)point].bend) > 1.0e-4f);
    m.addSeparator();
    m.addItem(1, juce::String::fromUTF8("Punkt l\xc3\xb6schen"));
    m.showMenuAsync(juce::PopupMenu::Options(),
                    [safe = juce::Component::SafePointer<VerlaufView>(this), lane, point](int r)
                    {
                        if (safe == nullptr || r <= 0 || !safe->validLane(lane))
                            return;
                        auto &l = safe->set().lanes[(size_t)lane];
                        if (point >= (int)l.pts.size())
                            return;
                        safe->pushUndo();
                        if (r == 1)
                        {
                            l.pts.erase(l.pts.begin() + point);
                            safe->selPoint = -1;
                        }
                        else if (r == 20)
                            l.pts[(size_t)point].bend = 0.0f;
                        else if (r >= 10 && r < 10 + kNumAutoShapes)
                            l.pts[(size_t)point].shape = r - 10;
                        safe->changed();
                    });
}

void VerlaufView::syncControls()
{
    const juce::ScopedValueSetter<bool> svs(updating, true);
    const auto &s = set();
    onButton.setToggleState(s.on, juce::dontSendNotification);
    clockHost.setToggleState(s.clock == 0, juce::dontSendNotification);
    clockOwn.setToggleState(s.clock == 1, juce::dontSendNotification);
    keyStart.setToggleState(s.startOnKey, juce::dontSendNotification);
    recButton.setToggleState(proc.isAutoRecord(), juce::dontSendNotification);
    int gi = 0;
    for (int i = 0; i < 8; ++i)
        if (std::abs(kGridBeats[i] - s.grid) < 1.0e-9)
            gi = i;
    gridBox.setSelectedItemIndex(gi, juce::dontSendNotification);
    tempoRow.slider.setValue(s.tempo, juce::dontSendNotification);
    tempoRow.repaint();
    undoButton.setEnabled(!undoStack.empty());

    const bool lane = validLane(selLane);
    for (juce::Component *c : {(juce::Component *)&lenRow, (juce::Component *)&speedRow,
                               (juce::Component *)&offsetRow, (juce::Component *)&loopButton,
                               (juce::Component *)&revButton, (juce::Component *)&clearButton})
        c->setEnabled(lane);
    bool seg = false;
    if (lane)
    {
        const auto &l = s.lanes[(size_t)selLane];
        lenRow.slider.setValue(l.length, juce::dontSendNotification);
        speedRow.slider.setValue(l.speed, juce::dontSendNotification);
        offsetRow.slider.setValue(l.offset, juce::dontSendNotification);
        loopButton.setToggleState(l.loop, juce::dontSendNotification);
        revButton.setToggleState(l.reverse, juce::dontSendNotification);
        if (selPoint >= 0 && selPoint < (int)l.pts.size())
        {
            seg = true;
            const auto &p = l.pts[(size_t)selPoint];
            for (int k = 0; k < kNumAutoShapes; ++k)
                shapeButtons[(size_t)k].setToggleState(p.shape == k, juce::dontSendNotification);
            bendRow.slider.setValue(p.bend, juce::dontSendNotification);
            rateRow.slider.setValue(p.rate, juce::dontSendNotification);
            depthRow.slider.setValue(p.depth, juce::dontSendNotification);
            const bool texture = p.shape == asWave || p.shape == asJitter || p.shape == asStairs;
            rateRow.setEnabled(texture);
            depthRow.setEnabled(p.shape == asWave || p.shape == asJitter);
            bendRow.setEnabled(p.shape != asStep);
        }
    }
    for (auto &b : shapeButtons)
    {
        b.setEnabled(seg);
        if (!seg)
            b.setToggleState(false, juce::dontSendNotification);
    }
    if (!seg)
        for (auto *r : {&bendRow, &rateRow, &depthRow})
            r->setEnabled(false);
    for (auto *r : {&lenRow, &speedRow, &offsetRow, &bendRow, &rateRow, &depthRow})
        r->repaint();
    repaint(laneBox.getUnion(segBox));
}

void VerlaufView::visibilityChanged()
{
    if (!isVisible())
        return;
    // (a loaded project, lanes added from a slider's menu)
    shownVersion = proc.getAutoVersion();
    if (!validLane(selLane) && !set().lanes.empty())
        selLane = 0;
    syncControls();
}

void VerlaufView::timerCallback()
{
    if (!isShowing())
        return;
    if (proc.getAutoVersion() != shownVersion)
    {
        // (recording, a loaded project)
        shownVersion = proc.getAutoVersion();
        if (!validLane(selLane))
            selLane = set().lanes.empty() ? -1 : 0;
        if (validLane(selLane) && selPoint >= (int)set().lanes[(size_t)selLane].pts.size())
            selPoint = -1;
        syncControls();
        timeline.repaint();
    }
    const double now = proc.getAutoNow();
    if (std::abs(now - shownNow) > 1.0e-9)
    {
        shownNow = now;
        auto &s = set();
        // the view follows the playhead while it moves
        if (proc.isAutoMoving() && timeline.drag == VerlaufTimeline::Drag::none &&
            (now > s.viewStart + s.viewLen || now < s.viewStart))
            s.viewStart = juce::jmax(0.0, now - s.viewLen * 0.1);
        timeline.repaint();
        repaint(titleBox);
    }
    const bool rec = proc.isAutoRecord();
    if (recButton.getToggleState() != rec)
        recButton.setToggleState(rec, juce::dontSendNotification);
    const bool own = !proc.isAutoHostClock();
    playButton.setEnabled(own);
    stopButton.setEnabled(own);
    homeButton.setEnabled(own);
    playButton.setToggleState(own && proc.isAutoOwnRunning(), juce::dontSendNotification);
}

void VerlaufView::paint(juce::Graphics &g)
{
    auto &A = Assets::get();
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.setOpacity(0.07f);
        drawCover(g, A.wall, getLocalBounds().toFloat(), 0.6f);
    }
    const auto &s = set();
    const bool host = proc.isAutoHostClock();
    juce::String right = timeText(proc.getAutoNow());
    right << (host ? juce::String::fromUTF8("  \xc2\xb7  folgt dem Host") : juce::String::fromUTF8("  \xc2\xb7  eigene Uhr"));
    if (proc.isAutoRecord())
        right << juce::String::fromUTF8("  \xc2\xb7  Aufnahme");
    drawSectionTitle(g, titleBox, "Verlauf", right);
    drawSectionTitle(g, laneBox, "Spur",
                     validLane(selLane) ? laneName(selLane) : juce::String::fromUTF8("keine gew\xc3\xa4hlt"));
    juce::String segRight;
    if (validLane(selLane))
    {
        const auto &l = s.lanes[(size_t)selLane];
        if (selPoint >= 0 && selPoint < (int)l.pts.size())
            segRight = "Punkt " + juce::String(selPoint + 1) + " von " + juce::String((int)l.pts.size()) + "  " +
                       timeText(beatOfLocal(l, l.pts[(size_t)selPoint].t));
        else
            segRight = juce::String::fromUTF8("einen Punkt w\xc3\xa4hlen");
    }
    drawSectionTitle(g, segBox, "Abschnitt", segRight);
    // a frame around the timeline
    g.setColour(col::border);
    g.drawRect(timeline.getBounds().expanded(1), 1);
}

void VerlaufView::resized()
{
    auto r = getLocalBounds().reduced(28, 0).withTrimmedTop(18).withTrimmedBottom(14);
    titleBox = r.removeFromTop(36);
    // the bar under the title
    auto bar = r.removeFromTop(26);
    auto place = [&bar](juce::Component &c, int w, int gap = 4)
    {
        c.setBounds(bar.removeFromLeft(w));
        bar.removeFromLeft(gap);
    };
    place(onButton, 78, 12);
    place(clockHost, 54, 2);
    place(clockOwn, 80, 8);
    place(playButton, 30, 2);
    place(stopButton, 30, 2);
    place(homeButton, 30, 8);
    place(keyStart, 80, 8);
    tempoRow.setBounds(bar.removeFromLeft(132).withTrimmedTop(-4));
    bar.removeFromLeft(12);
    place(recButton, 86, 12);
    place(toolPts, 62, 2);
    place(toolPen, 50, 2);
    place(toolStair, 62, 10);
    place(gridBox, 100, 10);
    // right end: zoom, undo, add
    addButton.setBounds(bar.removeFromRight(66));
    bar.removeFromRight(8);
    undoButton.setBounds(bar.removeFromRight(60));
    bar.removeFromRight(8);
    zoomAll.setBounds(bar.removeFromRight(44));
    bar.removeFromRight(2);
    zoomIn.setBounds(bar.removeFromRight(26));
    bar.removeFromRight(2);
    zoomOut.setBounds(bar.removeFromRight(26));

    r.removeFromTop(10);
    auto bottom = r.removeFromBottom(122);
    r.removeFromBottom(14);
    timeline.setBounds(r);

    const int gap = 24;
    laneBox = bottom.removeFromLeft((bottom.getWidth() - gap) / 2);
    bottom.removeFromLeft(gap);
    segBox = bottom;
    {
        auto b = laneBox.withTrimmedTop(38);
        auto row1 = b.removeFromTop(34);
        const int w = (row1.getWidth() - 2 * 18) / 3;
        lenRow.setBounds(row1.removeFromLeft(w));
        row1.removeFromLeft(18);
        speedRow.setBounds(row1.removeFromLeft(w));
        row1.removeFromLeft(18);
        offsetRow.setBounds(row1);
        b.removeFromTop(10);
        auto row2 = b.removeFromTop(24);
        loopButton.setBounds(row2.removeFromLeft(90));
        row2.removeFromLeft(6);
        revButton.setBounds(row2.removeFromLeft(90));
        clearButton.setBounds(row2.removeFromRight(80));
    }
    {
        auto b = segBox.withTrimmedTop(38);
        auto row1 = b.removeFromTop(24);
        const int bw = (row1.getWidth() - 4 * 4) / kNumAutoShapes;
        for (auto &sb : shapeButtons)
        {
            sb.setBounds(row1.removeFromLeft(bw));
            row1.removeFromLeft(4);
        }
        b.removeFromTop(10);
        auto row2 = b.removeFromTop(34);
        const int w = (row2.getWidth() - 2 * 18) / 3;
        bendRow.setBounds(row2.removeFromLeft(w));
        row2.removeFromLeft(18);
        rateRow.setBounds(row2.removeFromLeft(w));
        row2.removeFromLeft(18);
        depthRow.setBounds(row2);
    }
}
} // namespace rysum::ui
