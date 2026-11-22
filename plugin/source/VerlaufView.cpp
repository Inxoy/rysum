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
    return {"Raster aus", rysum::tr("Raster 1/16"), rysum::tr("Raster 1/8"), rysum::tr("Raster 1/4"), rysum::tr("Raster 1/2"),
            rysum::tr("Raster 1 Takt"), rysum::tr("Raster 2 Takte"), rysum::tr("Raster 4 Takte")};
}
juce::String beatsText(double b)
{
    if (std::abs(b - std::round(b)) < 1.0e-6)
        return juce::String((int)std::round(b));
    return juce::String(b, b < 1.0 ? 3 : 2);
}
/** a clock time as bar.beat (4/4, both from 1): "5" for a whole bar, "5.3", "5.3.50" between */
juce::String barText(double beats)
{
    const double b = juce::jmax(0.0, beats);
    const int bar = (int)std::floor(b / 4.0 + 1.0e-9) + 1;
    const double inBar = juce::jmax(0.0, b - (bar - 1) * 4.0);
    const double beat = std::floor(inBar + 1.0e-9);
    const double frac = juce::jmax(0.0, inBar - beat);
    if (beat < 0.5 && frac < 1.0e-6)
        return juce::String(bar);
    juce::String t = juce::String(bar) + "." + juce::String((int)beat + 1);
    if (frac > 1.0e-6)
        t << "." << juce::String(juce::roundToInt(frac * 100.0)).paddedLeft('0', 2);
    return t;
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
    g.drawText(rysum::tr(title), r, juce::Justification::centredLeft);
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

int VerlaufTimeline::laneHeight(int lane) const
{
    // a height of its own (its lower edge dragged), else taller while chosen
    if (owner.validLane(lane))
        if (const int h = owner.proc.getAuto().lanes[(size_t)lane].height; h > 0)
            return juce::jlimit(kMinLaneH, kMaxLaneH, h);
    return lane == owner.selLane ? 128 : 64;
}

juce::Rectangle<int> VerlaufTimeline::laneRect(int lane) const
{
    int y = kRulerH - scrollY;
    for (int l = 0; l < lane; ++l)
        y += laneHeight(l);
    return {0, y, getWidth(), laneHeight(lane)};
}

int VerlaufTimeline::contentHeight() const
{
    const int n = (int)owner.proc.getAuto().lanes.size();
    int h = 0;
    for (int l = 0; l < n; ++l)
        h += laneHeight(l);
    return h;
}

bool VerlaufTimeline::onLowerEdge(int lane, int y) const
{
    if (!owner.validLane(lane))
        return false;
    const int b = laneRect(lane).getBottom();
    return y >= b - 4 && y < b + 1;
}

juce::Rectangle<float> VerlaufTimeline::markerFlag(int i) const
{
    const auto &m = owner.proc.getAuto().markers[(size_t)i];
    const float x = xOfBeat(m.beat);
    const float w = juce::GlyphArrangement::getStringWidth(Assets::get().monoFont(9.5f), m.name) + 9.0f;
    return {x, 1.0f, juce::jmax(10.0f, w), (float)kMarkH - 3.0f};
}

int VerlaufTimeline::markerAt(float x) const
{
    const auto &ms = owner.proc.getAuto().markers;
    // the latest flag under x wins (flags overlap to the right)
    for (int i = (int)ms.size() - 1; i >= 0; --i)
    {
        const auto f = markerFlag(i);
        if (x >= f.getX() - 3.0f && x <= f.getRight())
            return i;
    }
    return -1;
}

int VerlaufTimeline::lineAt(int lane, juce::Point<float> p) const
{
    if (!owner.validLane(lane) || p.x < (float)kHeaderW)
        return -1;
    const auto &l = owner.proc.getAuto().lanes[(size_t)lane];
    if (l.pts.size() < 2)
        return -1;
    const auto c = curveArea(laneRect(lane));
    const int steps = owner.proc.autoSteps(owner.laneParam(lane));
    // near the drawn line: its height here, or anywhere it passes within 2 px to the sides (steps,
    // steep slopes)
    bool near = false;
    for (int dx = -2; dx <= 2 && !near; ++dx)
    {
        const float v = autoValue(l, beatOfX(p.x + (float)dx), steps);
        near = !std::isnan(v) && std::abs(yOfValue(c, v) - p.y) < 5.0f;
    }
    if (!near)
        return -1;
    const int seg = autoFindSegment(l.pts, autoLocalTime(l, beatOfX(p.x)));
    return seg >= 0 && seg + 1 < (int)l.pts.size() ? seg : -1;
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
    // on / armed / menu, at the bottom right of the header (a low lane: in the middle, right)
    static const int widths[]{32, 24, 24};
    const int h = 18;
    int x = kHeaderW - 10;
    for (int b = 2; b >= which; --b)
        x -= widths[b] + (b < 2 ? 4 : 0);
    const int y = lane.getHeight() < 56 ? lane.getCentreY() - h / 2 : lane.getBottom() - h - 8;
    return {x, y, widths[which], h};
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
                rysum::tr("Noch keine Spur.\n\xe2\x80\x9e+ Spur\xe2\x80\x9c w\xc3\xa4hlt einen Regler, "
                    "Rechtsklick auf einen Regler \xe2\x86\x92 Im Verlauf automatisieren \xe2\x80\x93 "
                    "oder: \xe2\x97\x8f Aufnahme an, \xe2\x96\xb6, und einen Regler bewegen: er bekommt "
                    "seine Spur, und die Bewegung wird aufgeschrieben."),
                getLocalBounds().withTrimmedTop(kRulerH).reduced(40, 20),
                juce::Justification::centred, 4);
        }
        // more lanes than fit: where one is in them (the mouse wheel rolls them)
        const int visible = getHeight() - kRulerH, content = contentHeight();
        if (content > visible && visible > 0)
        {
            const float th = (float)visible * (float)visible / (float)content;
            const float ty = (float)kRulerH + (float)(visible - th) * (float)scrollY / (float)(content - visible);
            g.setColour(col::dim.withAlpha(0.5f));
            g.fillRoundedRectangle(juce::Rectangle<float>((float)getWidth() - 5.0f, ty + 2.0f, 3.0f, th - 4.0f), 1.5f);
        }
    }

    // the markers: a thin line down through the lanes
    {
        const auto &s2 = owner.proc.getAuto();
        const float dash[]{2.0f, 4.0f};
        g.setColour(col::rubric.withAlpha(0.45f));
        for (const auto &m : s2.markers)
        {
            const float x = xOfBeat(m.beat);
            if (x > (float)kHeaderW && x < (float)getWidth())
                g.drawDashedLine({x, (float)kRulerH, x, (float)getHeight()}, dash, 2, 1.0f);
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
    // the band on top: the repeated section (A-B) and the markers
    {
        auto band = juce::Rectangle<float>((float)kHeaderW, (float)r.getY(), (float)(getWidth() - kHeaderW), (float)kMarkH);
        g.setColour(col::panel.withAlpha(0.6f));
        g.fillRect(band);
        g.setColour(col::line);
        g.fillRect(band.getX(), band.getBottom() - 1.0f, band.getWidth(), 1.0f);
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(band.toNearestInt());
        {
            const float xa = xOfBeat(s.cycleStart), xb = xOfBeat(s.cycleEnd);
            const auto cr = juce::Rectangle<float>(xa, band.getY() + 2.0f, juce::jmax(2.0f, xb - xa), band.getHeight() - 4.0f);
            g.setColour(s.cycle ? col::rubric.withAlpha(0.45f) : col::faint.withAlpha(0.18f));
            g.fillRect(cr);
            g.setColour(s.cycle ? col::rubric.brighter(0.3f) : col::faint.withAlpha(0.5f));
            g.fillRect(xa, band.getY() + 1.0f, 2.0f, band.getHeight() - 2.0f);
            g.fillRect(xb - 2.0f, band.getY() + 1.0f, 2.0f, band.getHeight() - 2.0f);
        }
        g.setFont(A.monoFont(9.5f));
        for (int i = 0; i < (int)s.markers.size(); ++i)
        {
            const auto f = markerFlag(i);
            const bool hot = (drag == Drag::marker && dragMarker == i) ||
                             (drag == Drag::none && hover.y >= 0.0f && hover.y < (float)kMarkH && markerAt(hover.x) == i);
            g.setColour(hot ? col::rubric.brighter(0.4f) : col::rubric.withAlpha(0.85f));
            g.fillRect(f);
            g.setColour(col::white);
            g.drawText(rysum::tr(s.markers[(size_t)i].name), f.withTrimmedLeft(4.0f), juce::Justification::centredLeft, false);
        }
    }
    // labels: bars and beats (the unit of every lane setting: 4 beats = 1 bar), at least ~70 px
    // apart; with the own clock the seconds as well, smaller
    const bool host = owner.proc.isAutoHostClock();
    const float pxPerBeat = (float)(getWidth() - kHeaderW) / (float)s.viewLen;
    double step = 4.0;
    while (step * pxPerBeat < 70.0f)
        step *= 2.0;
    while (step > 0.25 && step * pxPerBeat > 160.0f)
        step *= 0.5;
    const double first = std::floor(s.viewStart / step) * step;
    for (double b = first; b <= s.viewStart + s.viewLen + 1.0e-9; b += step)
    {
        const float x = xOfBeat(b);
        if (x < (float)kHeaderW - 0.5f)
            continue;
        const bool bar = std::abs(std::fmod(b, 4.0)) < 1.0e-6;
        g.setColour(col::faint);
        g.fillRect(x, (float)r.getBottom() - (bar ? 8.0f : 5.0f), 1.0f, bar ? 7.0f : 4.0f);
        g.setColour(bar ? col::ink.withAlpha(0.8f) : col::dim);
        g.setFont(A.monoFont(10.0f));
        const auto lbl = barText(b);
        const float y0 = (float)r.getY() + (float)kMarkH;
        g.drawText(lbl, juce::Rectangle<float>(x + 3.0f, y0 + 2.0f, 60.0f, 13.0f),
                   juce::Justification::centredLeft);
        if (!host)
        {
            g.setColour(col::faint);
            g.setFont(A.monoFont(8.5f));
            g.drawText(owner.secondsText(b), juce::Rectangle<float>(x + 3.0f, y0 + 13.0f, 60.0f, 10.0f),
                       juce::Justification::centredLeft);
        }
    }
    // left: what the clock is (and what the band is)
    g.setColour(col::faint);
    g.setFont(A.monoFont(9.0f));
    g.drawText(rysum::tr("Marken \xc2\xb7 A\xe2\x80\x93"
                                      "B"),
               juce::Rectangle<int>(10, r.getY(), kHeaderW - 20, kMarkH), juce::Justification::centredLeft);
    g.setFont(A.romanFont(11.0f));
    g.drawText(host ? rysum::tr("Takt.Schlag \xc2\xb7 Host")
                    : rysum::tr("Takt.Schlag \xc2\xb7 eigene Uhr ") +
                          (owner.tempoLane() != nullptr ? juce::String("(Tempo-Spur)")
                                                        : juce::String(s.tempo, 0) + " BPM"),
               juce::Rectangle<int>(10, r.getY() + kMarkH, kHeaderW - 20, kRulerH - kMarkH),
               juce::Justification::centredLeft);
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
    const int pipeT = autoPipeTarget(l.paramId), pathS = autoPathStop(l.paramId);
    const bool tempoL = l.paramId == kAutoTempoId;
    const bool anySolo = std::any_of(s.lanes.begin(), s.lanes.end(), [](const AutoLane &x) { return x.on && x.solo; });
    // (a solo elsewhere silences this lane; the tempo lane always counts)
    const bool live = s.on && l.on && (param >= 0 || pipeT >= 0 || pathS >= 0 || tempoL) &&
                      (!anySolo || l.solo || tempoL);

    // header
    auto h = r.withWidth(kHeaderW);
    g.setColour(sel ? col::panel.brighter(0.08f) : col::bg.withAlpha(0.6f));
    g.fillRect(h);
    if (sel)
    {
        g.setColour(col::rubric);
        g.fillRect(h.getX(), h.getY() + 6, 2, h.getHeight() - 12);
    }
    const bool low = r.getHeight() < 56; // a low lane: only its name, the buttons beside it
    auto ht = h.reduced(12, low ? 0 : 7);
    if (low)
        ht = ht.withRight(headerButton(r, 0).getX() - 4);
    g.setColour(l.on ? col::ink : col::faint);
    g.setFont(A.sansFont(13.5f));
    g.drawText(owner.laneName(li), low ? ht : ht.removeFromTop(18), juce::Justification::centredLeft, true);
    if (low)
        ht = {};
    g.setFont(A.monoFont(10.0f));
    g.setColour(col::faint);
    juce::String info =
        param >= 0 ? rysum::tr(ModView::paramGroup(P.getParamAt(param)->getParameterID()))
        : pipeT >= 0
            ? (l.spread ? rysum::tr("je Pfeife \xc2\xb7 breitet sich aus ") +
                              juce::String(l.waveSpeed, l.waveSpeed < 10.0f ? 1 : 0) + " m/S."
                        : rysum::tr("je Pfeife"))
        : pathS >= 0 ? rysum::tr("Fahrplan der Bahn")
        : tempoL     ? (P.isAutoHostClock() ? rysum::tr("ruht \xe2\x80\x93 die Uhr ist die des Hosts")
                                            : rysum::tr("eigene Uhr \xc2\xb7 20 bis 300 BPM"))
                     : rysum::tr("(unbekannter Regler)");
    if (l.solo)
        info = juce::String::fromUTF8("SOLO \xc2\xb7 ") + info;
    else if (anySolo && !tempoL)
        info = rysum::tr("stumm (Solo) \xc2\xb7 ") + info;
    if (l.perNote && !tempoL)
        info << (pipeT >= 0 ? rysum::tr(" \xc2\xb7 je Anschlag neu, jede Taste f\xc3\xbcr sich")
                            : rysum::tr(" \xc2\xb7 je Anschlag neu"));
    if (l.loop)
        info << rysum::tr(" \xc2\xb7 Schleife ") << beatsText(l.length);
    if (l.reverse)
        info << rysum::tr(" \xc2\xb7 r\xc3\xbc"
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
    else if (live)
    {
        // per-pipe and path lanes: the curve at the clock (at the origin, for a spreading one; per
        // note: since the last key)
        const float v = autoValue(l, l.perNote ? P.getAutoSinceKey() : P.getAutoNow());
        g.setColour(col::dim);
        g.drawText(std::isnan(v) ? juce::String("-") : owner.valueText(li, v), ht.removeFromTop(14),
                   juce::Justification::centredLeft, true);
    }
    // header buttons: on / off (says which), armed for recording, the lane's menu
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
        const juce::String label = b == 0   ? juce::String(l.on ? rysum::tr("an") : rysum::tr("aus"))
                                   : b == 1 ? juce::String::fromUTF8("\xe2\x97\x8f")
                                            : juce::String::fromUTF8("\xe2\x80\xa6");
        g.setFont(b == 0 ? A.monoFont(10.0f) : A.sansFont(b == 1 ? 11.0f : 14.0f));
        g.drawText(label, br, juce::Justification::centred);
    }
    // separator
    g.setColour(col::line);
    g.fillRect(r.getX(), r.getBottom() - 1, r.getWidth(), 1);

    // the curve area
    const auto c = curveArea(r);
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(r.withTrimmedLeft(kHeaderW));
    if (l.perNote && !tempoL)
    {
        // "je Anschlag neu": the ruler's 0 is the key press; a mark where the last key has got to
        const double since = P.getAutoSinceKey();
        const float xs = xOfBeat(since);
        if (since >= 0.0 && since < 1.0e6 && xs > (float)kHeaderW && xs < (float)getWidth())
        {
            g.setColour(col::rubric.withAlpha(0.55f));
            g.fillRect(juce::Rectangle<float>(xs - 0.5f, (float)r.getY() + 2.0f, 1.5f, (float)r.getHeight() - 4.0f));
            if (r.getHeight() >= 40)
            {
                g.setFont(A.monoFont(9.0f));
                g.drawText(rysum::tr("letzte Taste"), juce::Rectangle<float>(xs + 3.0f, (float)r.getBottom() - 13.0f, 60.0f, 11.0f),
                           juce::Justification::centredLeft);
            }
        }
    }
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
        if (l.loop)
        {
            // the repeats: where each one begins (numbered), and the points of the first pass
            // faintly where they come again (only the first pass is edited)
            const double span = l.length / juce::jmax(1.0e-6, (double)l.speed);
            const int k0 = juce::jmax(1, (int)std::floor((s.viewStart - l.offset) / span));
            g.setFont(A.monoFont(9.0f));
            for (int k = k0; k < k0 + 400; ++k)
            {
                const double b = l.offset + span * k;
                const float xk = xOfBeat(b);
                if (xk > (float)getWidth())
                    break;
                if (span * (getWidth() - kHeaderW) / s.viewLen < 6.0)
                    break; // (too close to show)
                g.setColour(col::rubric.withAlpha(0.35f));
                g.drawDashedLine({xk, (float)r.getY() + 3.0f, xk, (float)r.getBottom() - 3.0f}, dash, 2, 1.0f);
                if (r.getHeight() >= 40)
                {
                    g.setColour(col::rubric.withAlpha(0.7f));
                    g.drawText(juce::String::fromUTF8("\xc3\x97") + juce::String(k + 1),
                               juce::Rectangle<float>(xk + 3.0f, (float)r.getY() + 2.0f, 40.0f, 11.0f),
                               juce::Justification::centredLeft);
                }
                if (!l.reverse)
                {
                    const auto c0 = curveArea(r);
                    g.setColour(col::ink.withAlpha(0.22f));
                    for (const auto &pt : l.pts)
                    {
                        if (pt.t < 0.0 || pt.t >= l.length)
                            continue;
                        const float x = xOfBeat(b + pt.t / juce::jmax(1.0e-6, (double)l.speed));
                        if (x > (float)kHeaderW && x < (float)getWidth())
                            drawLozenge(g, {x, yOfValue(c0, pt.v)}, 3.0f, 4.0f);
                    }
                }
            }
        }
    }
    // value guides: middle, and the steps of a switch
    g.setColour(col::line.withAlpha(0.8f));
    if (steps >= 2 && steps <= 12)
        for (int k = 0; k < steps; ++k)
            g.fillRect((float)c.getX(), yOfValue(c, (float)k / (float)(steps - 1)), (float)c.getWidth(), 1.0f);
    else
        g.fillRect((float)c.getX(), yOfValue(c, 0.5f), (float)c.getWidth(), 1.0f);
    // the scale: what the top and the bottom of the lane mean (and the middle, when there is room)
    if (r.getHeight() >= 56)
    {
        g.setFont(A.monoFont(8.5f));
        g.setColour(col::faint);
        const float lx = (float)c.getX() + 4.0f;
        g.drawText(owner.valueText(li, 1.0f), juce::Rectangle<float>(lx, (float)c.getY() - 1.0f, 120.0f, 10.0f),
                   juce::Justification::centredLeft);
        g.drawText(owner.valueText(li, 0.0f), juce::Rectangle<float>(lx, (float)c.getBottom() - 9.0f, 120.0f, 10.0f),
                   juce::Justification::centredLeft);
        if (r.getHeight() >= 110 && steps < 2)
            g.drawText(owner.valueText(li, 0.5f), juce::Rectangle<float>(lx, yOfValue(c, 0.5f) - 10.0f, 120.0f, 10.0f),
                       juce::Justification::centredLeft);
    }

    if (l.pts.empty())
    {
        g.setColour(col::faint);
        g.setFont(A.monoFont(10.0f));
        g.drawText(owner.tool == VerlaufView::toolPoints
                       ? rysum::tr("klicken: Punkt setzen")
                       : rysum::tr("ziehen: zeichnen"),
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
            // its line, in the colour of the choice (that is what the shapes below change)
            if (xb - xa > 1.0f)
            {
                juce::Path segPath;
                bool first = true;
                for (float x = juce::jmax(xa, (float)c.getX()); x <= juce::jmin(xb, (float)c.getRight()); x += 1.0f)
                {
                    const float v = autoValue(l, beatOfX(x), steps);
                    if (std::isnan(v))
                        continue;
                    if (first)
                        segPath.startNewSubPath(x, yOfValue(c, v));
                    else
                        segPath.lineTo(x, yOfValue(c, v));
                    first = false;
                }
                g.setColour(col::rubric.brighter(0.3f));
                g.strokePath(segPath, juce::PathStrokeType(2.2f));
            }
        }
        // the section under the mouse (a click chooses it)
        if (hoverLane == li && drag == Drag::none && hoverPoint < 0 && owner.tool == VerlaufView::toolPoints)
            if (const int hs = lineAt(li, hover); hs >= 0 && !(sel && hs == owner.selPoint))
            {
                const float xa = xOfBeat(VerlaufView::beatOfLocal(l, l.pts[(size_t)hs].t)),
                            xb = xOfBeat(VerlaufView::beatOfLocal(l, l.pts[(size_t)hs + 1].t));
                g.setColour(col::white.withAlpha(0.05f));
                g.fillRect(juce::Rectangle<float>(juce::jmin(xa, xb), (float)c.getY(),
                                                  juce::jmax(2.0f, std::abs(xb - xa)), (float)c.getHeight()));
            }
        // the points (first pass)
        for (int i = 0; i < (int)l.pts.size(); ++i)
        {
            const auto &pt = l.pts[(size_t)i];
            const float x = xOfBeat(VerlaufView::beatOfLocal(l, pt.t)), y = yOfValue(c, pt.v);
            if (x < (float)kHeaderW - 6.0f || x > (float)getWidth() + 6.0f)
                continue;
            const bool hot = (hoverLane == li && hoverPoint == i) || (dragLane == li && dragPoint == i);
            const bool selected = sel && (i == owner.selPoint || owner.isSelected(i));
            g.setColour(selected ? col::rubric.brighter(0.3f) : (hot ? col::white : col::ink));
            drawLozenge(g, {x, y}, hot || selected ? 4.5f : 3.5f, hot || selected ? 6.0f : 5.0f);
            // the section's shape and its variation, beside its first point
            juce::String tag = pt.shape != asCurve ? rysum::tr(autoShapeNames()[pt.shape]) : juce::String();
            if (pt.chance < 1.0f)
                tag << (tag.isEmpty() ? "" : " ") << juce::roundToInt(pt.chance * 100.0f) << " %";
            if (pt.every > 1)
                tag << (tag.isEmpty() ? "" : " ") << rysum::tr("jedes ") << pt.every << ".";
            if (pt.vary > 0.0f)
                tag << (tag.isEmpty() ? "" : " ") << juce::String::fromUTF8("\xc2\xb1") << juce::roundToInt(pt.vary * 100.0f) << " %";
            if (tag.isNotEmpty() && (sel || pt.chance < 1.0f || pt.every > 1 || pt.vary > 0.0f) && r.getHeight() >= 40)
            {
                g.setColour(pt.chance < 1.0f || pt.every > 1 || pt.vary > 0.0f ? col::rubric.brighter(0.3f).withAlpha(0.8f)
                                                                                 : col::faint);
                g.setFont(A.monoFont(9.0f));
                g.drawText(tag, juce::Rectangle<float>(x + 6.0f, (float)c.getY(), 140.0f, 12.0f),
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
    // the frame being drawn around points
    if (dragLane == li && drag == Drag::box && dragMoved)
    {
        const auto box = juce::Rectangle<float>(downPos, hover);
        g.setColour(col::rubric.withAlpha(0.08f));
        g.fillRect(box);
        g.setColour(col::rubric.withAlpha(0.7f));
        g.drawRect(box, 1.0f);
    }
    // the readout: time and value where the mouse is, or of the point it is on / moves, in a box
    // that stays readable over the curve
    {
        juce::String txt;
        juce::Point<float> at;
        bool strong = false;
        if (dragLane == li && drag == Drag::point && dragPoint >= 0 && dragPoint < (int)l.pts.size())
        {
            const auto &pt = l.pts[(size_t)dragPoint];
            at = {xOfBeat(VerlaufView::beatOfLocal(l, pt.t)), yOfValue(c, pt.v)};
            txt = owner.timeText(VerlaufView::beatOfLocal(l, pt.t)) + "   " + owner.valueText(li, pt.v);
            if (owner.selPts.size() > 1)
                txt << "   (" << (int)owner.selPts.size() << rysum::tr(" Punkte)");
            strong = true;
        }
        else if (dragLane == li && (drag == Drag::bend || (drag == Drag::line && dragMoved)) &&
                 owner.selPoint >= 0 && owner.selPoint < (int)l.pts.size())
        {
            at = hover;
            txt = rysum::tr("Biegung ") + juce::String(juce::roundToInt(l.pts[(size_t)owner.selPoint].bend * 100.0f)) + " %";
            strong = true;
        }
        else if (hoverLane == li && hover.x > (float)kHeaderW && drag == Drag::none && !onLowerEdge(li, (int)hover.y))
        {
            if (hoverPoint >= 0 && hoverPoint < (int)l.pts.size())
            {
                const auto &pt = l.pts[(size_t)hoverPoint];
                at = {xOfBeat(VerlaufView::beatOfLocal(l, pt.t)), yOfValue(c, pt.v)};
                txt = owner.timeText(VerlaufView::beatOfLocal(l, pt.t)) + "   " + owner.valueText(li, pt.v);
            }
            else
            {
                at = hover;
                const double b = beatOfX(hover.x);
                const float onCurve = autoValue(l, b, steps);
                // on the line: the curve's value there; elsewhere: what a click would set
                const float v = lineAt(li, hover) >= 0 && !std::isnan(onCurve) ? onCurve : valueOfY(c, hover.y);
                txt = owner.timeText(owner.tool == VerlaufView::toolPoints ? snap(b, false) : b) + "   " +
                      owner.valueText(li, v);
            }
        }
        if (txt.isNotEmpty())
        {
            const auto font = A.monoFont(10.0f);
            const float w = juce::GlyphArrangement::getStringWidth(font, txt) + 10.0f;
            auto box = juce::Rectangle<float>(at.x + 12.0f, at.y - 22.0f, w, 15.0f);
            if (box.getRight() > (float)getWidth() - 2.0f)
                box.setX(at.x - 12.0f - w);
            box.setY(juce::jlimit((float)r.getY() + 1.0f, (float)r.getBottom() - 16.0f, box.getY()));
            g.setColour(col::bg.withAlpha(0.92f));
            g.fillRect(box);
            g.setColour(strong ? col::rubric.withAlpha(0.8f) : col::border);
            g.drawRect(box, 1.0f);
            g.setColour(strong ? col::white : col::ink.withAlpha(0.85f));
            g.setFont(font);
            g.drawText(txt, box, juce::Justification::centred);
        }
    }
}

// ---------------------------------------------------------------------------
// timeline mouse
// ---------------------------------------------------------------------------
void VerlaufTimeline::ensureUndo()
{
    if (!undoPushed)
    {
        owner.pushUndo();
        undoPushed = true;
    }
}

VerlaufTimeline::Hover VerlaufTimeline::hoverKind() const
{
    if (hover.x < 0.0f)
        return Hover::none;
    if (hover.y < (float)kMarkH && hover.x > (float)kHeaderW)
    {
        if (markerAt(hover.x) >= 0)
            return Hover::marker;
        const auto &s = owner.proc.getAuto();
        if (std::abs(hover.x - xOfBeat(s.cycleStart)) < 5.0f || std::abs(hover.x - xOfBeat(s.cycleEnd)) < 5.0f)
            return Hover::cycleEdge;
        return Hover::band;
    }
    if (hover.y < (float)kRulerH)
        return hover.x > (float)kHeaderW ? Hover::ruler : Hover::none;
    const int li = hoverLane;
    if (!owner.validLane(li))
        return Hover::none;
    if (onLowerEdge(li, (int)hover.y))
        return Hover::edge;
    if (hover.x < (float)kHeaderW)
    {
        const auto r = laneRect(li);
        for (int b = 0; b < 3; ++b)
            if (headerButton(r, b).contains(hover.toInt()))
                return Hover::headerButton;
        return Hover::header;
    }
    if (hoverPoint >= 0)
        return Hover::point;
    if (owner.tool == VerlaufView::toolPoints && lineAt(li, hover) >= 0)
        return Hover::line;
    return Hover::empty;
}

juce::String VerlaufTimeline::hintText() const
{
    auto U = [](const char *t) { return juce::String::fromUTF8(t); };
    const juce::String sep = U("   \xc2\xb7   ");
    switch (drag)
    {
    case Drag::point:
        return rysum::tr("Ziehen: Zeit und Wert") + sep + rysum::tr("Umschalt: ohne Raster") + sep + rysum::tr("Strg: nur der Wert") +
               (owner.selPts.size() > 1 ? sep + rysum::tr("alle gew\xc3\xa4hlten Punkte gehen mit") : juce::String());
    case Drag::box:
        return rysum::tr("Rahmen: die Punkte darin w\xc3\xa4hlen") + sep + rysum::tr("Umschalt: zur Auswahl dazu");
    case Drag::line:
    case Drag::bend:
        return rysum::tr("Nach oben oder unten ziehen: den Abschnitt biegen");
    case Drag::pencil:
        return rysum::tr("Stift: der Strich ersetzt die Punkte unter ihm");
    case Drag::stairs:
        return rysum::tr("Treppe: je Rasterfeld eine Stufe");
    case Drag::resize:
        return rysum::tr("Ziehen: die H\xc3\xb6he der Spur");
    case Drag::seek:
        return rysum::tr("Ziehen: die Uhr stellen");
    case Drag::marker:
        return rysum::tr("Ziehen: die Marke verschieben") + sep + rysum::tr("Umschalt: ohne Raster");
    case Drag::cycleA:
    case Drag::cycleB:
    case Drag::cycleNew:
        return rysum::tr("Ziehen: den wiederholten Abschnitt (A\xe2\x80\x93"
                 "B) legen");
    case Drag::pan:
        return rysum::tr("Ziehen: die Ansicht verschieben");
    default:
        break;
    }
    juce::String selTail;
    if (owner.selPts.size() > 1)
        selTail = sep + juce::String((int)owner.selPts.size()) +
                  rysum::tr(" Punkte gew\xc3\xa4hlt: ziehen verschiebt alle, Entf l\xc3\xb6scht, Strg+C / Strg+V "
                    "kopiert, Esc hebt auf");
    const auto hk = hoverKind();
    if ((hk == Hover::point || hk == Hover::line || hk == Hover::empty) && hoverLane != owner.selLane &&
        owner.proc.getAuto().lanes[(size_t)hoverLane].height <= 0)
        return rysum::tr("Klicken w\xc3\xa4hlt diese Spur") + sep + rysum::tr("dann: Punkte setzen, ziehen, zeichnen");
    switch (hk)
    {
    case Hover::band:
        return rysum::tr("Marken: Doppelklick setzt eine (M: an der Uhr)") + sep +
               rysum::tr("ziehen legt den wiederholten Abschnitt A\xe2\x80\x93"
                 "B") +
               sep + rysum::tr("Bild auf / ab: zur vorigen / n\xc3\xa4"
                       "chsten Marke") +
               sep + rysum::tr("Rechtsklick: Men\xc3\xbc");
    case Hover::marker:
        return rysum::tr("Marke: klicken springt dorthin") + sep + rysum::tr("ziehen verschiebt") + sep +
               rysum::tr("Doppelklick: umbenennen") + sep + rysum::tr("Rechtsklick: Men\xc3\xbc");
    case Hover::cycleEdge:
        return rysum::tr("Ziehen: Anfang oder Ende des wiederholten Abschnitts") + sep +
               rysum::tr("A\xe2\x80\x93"
                 "B oben in der Leiste schaltet ihn an / aus");
    case Hover::ruler:
        return owner.proc.isAutoHostClock()
                   ? rysum::tr("Lineal: Takt.Schlag des Hosts \xe2\x80\x93 die Uhr stellt der Host")
                   : rysum::tr("Lineal: klicken oder ziehen stellt die Uhr") + sep + rysum::tr("Leertaste: Start / Halt");
    case Hover::header:
        return rysum::tr("Spurkopf: klicken w\xc3\xa4hlt die Spur (ihre Einstellungen unten links)") + sep +
               rysum::tr("Rechtsklick: Men\xc3\xbc") + sep + rysum::tr("unteren Rand ziehen: H\xc3\xb6he");
    case Hover::headerButton:
        return rysum::tr("an / aus: die Spur wirkt oder nicht") + sep + rysum::tr("\xe2\x97\x8f: aufnahmebereit") + sep +
               rysum::tr("\xe2\x80\xa6: Men\xc3\xbc (Schleife, H\xc3\xb6he, L\xc3\xb6schen \xe2\x80\xa6)");
    case Hover::edge:
        return rysum::tr("Unteren Rand ziehen: die H\xc3\xb6he der Spur") + sep + rysum::tr("Doppelklick: wieder automatisch");
    case Hover::point:
        return rysum::tr("Punkt: ziehen verschiebt") + sep + rysum::tr("Doppelklick l\xc3\xb6scht") + sep +
               rysum::tr("Umschalt + Klick: zur Auswahl") + sep + rysum::tr("Rechtsklick: Form") + selTail;
    case Hover::line:
        return rysum::tr("Linie: klicken w\xc3\xa4hlt den Abschnitt (seine Form unten rechts)") + sep +
               rysum::tr("ziehen biegt ihn") + sep + rysum::tr("Alt + Klick: hier einen Punkt setzen") + selTail;
    case Hover::empty:
        if (owner.tool == VerlaufView::toolPencil)
            return rysum::tr("Stift: ziehen zeichnet frei und ersetzt die Punkte unter dem Strich") + sep + rysum::tr("Strg+Z: zur\xc3\xbc"
                                                                                                     "ck");
        if (owner.tool == VerlaufView::toolStairs)
            return rysum::tr("Treppe: ziehen setzt je Rasterfeld eine Stufe (Raster oben)") + sep + rysum::tr("Strg+Z: zur\xc3\xbc"
                                                                                              "ck");
        return rysum::tr("Klicken: Punkt setzen") + sep + rysum::tr("ziehen: Rahmen um Punkte") + sep + rysum::tr("Strg + Mausrad: Zoom") +
               sep + rysum::tr("Umschalt + Mausrad: Zeit") + sep + rysum::tr("Mausrad: Spuren rollen") + selTail;
    default:
        break;
    }
    return rysum::tr("Leertaste: eigene Uhr starten / anhalten") + sep + rysum::tr("Strg+Z / Strg+Y: zur\xc3\xbc"
                                                                   "ck / wieder") +
           sep + rysum::tr("Strg+A: alle Punkte der Spur") + selTail;
}

juce::String VerlaufTimeline::getTooltip()
{
    const auto p = getMouseXYRelative();
    const int li = laneAt(p.y);
    if (li >= 0 && p.x < kHeaderW)
    {
        const auto r = laneRect(li);
        if (headerButton(r, 0).contains(p))
            return rysum::tr("Spur an / aus (aus: der Regler steht, wie er von Hand gestellt ist)");
        if (headerButton(r, 1).contains(p))
            return rysum::tr("Aufnahmebereit: mit \xe2\x97\x8f Aufnahme und laufender Uhr wird das "
                                          "Bewegen des Reglers in diese Spur geschrieben");
        if (headerButton(r, 2).contains(p))
            return rysum::tr("Men\xc3\xbc der Spur: Schleife, R\xc3\xbc"
                                          "ckw\xc3\xa4rts, H\xc3\xb6he, verschieben, leeren, l\xc3\xb6schen");
    }
    return {};
}

void VerlaufTimeline::mouseMove(const juce::MouseEvent &e)
{
    hover = e.position;
    hoverLane = laneAt(e.y);
    hoverPoint = hoverLane >= 0 && e.x > kHeaderW && !onLowerEdge(hoverLane, e.y) ? pointAt(hoverLane, e.position) : -1;
    const auto k = hoverKind();
    setMouseCursor(k == Hover::cycleEdge                                           ? juce::MouseCursor::LeftRightResizeCursor
                   : k == Hover::marker                                            ? juce::MouseCursor::PointingHandCursor
                   : k == Hover::edge                                              ? juce::MouseCursor::UpDownResizeCursor
                   : k == Hover::point                                             ? juce::MouseCursor::DraggingHandCursor
                   : k == Hover::line || k == Hover::headerButton                  ? juce::MouseCursor::PointingHandCursor
                   : k == Hover::empty && owner.tool != VerlaufView::toolPoints    ? juce::MouseCursor::CrosshairCursor
                                                                                   : juce::MouseCursor::NormalCursor);
    repaint();
    owner.hintChanged();
}

void VerlaufTimeline::mouseExit(const juce::MouseEvent &)
{
    hover = {-1.0f, -1.0f};
    hoverLane = hoverPoint = -1;
    repaint();
    owner.hintChanged();
}

void VerlaufTimeline::addPointAt(int li, juce::Point<float> p, bool fine)
{
    // a new point (it takes the shape of the section it lands in)
    auto &l = owner.set().lanes[(size_t)li];
    const auto c = curveArea(laneRect(li));
    ensureUndo();
    AutoPoint pt;
    pt.t = VerlaufView::localOf(l, snap(beatOfX(p.x), fine));
    pt.v = valueOfY(c, p.y);
    if (const int steps = owner.proc.autoSteps(owner.laneParam(li)); steps >= 2)
    {
        pt.v = autoQuantise(pt.v, steps);
        pt.shape = asStep;
    }
    const int seg = autoFindSegment(l.pts, pt.t);
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
    owner.selPts = {dragPoint};
    owner.changed();
}

void VerlaufTimeline::mouseDown(const juce::MouseEvent &e)
{
    auto &s = owner.set();
    drag = Drag::none;
    dragLane = dragPoint = -1;
    dragMoved = undoPushed = false;
    downPos = hover = e.position;
    owner.grabKeyboardFocus();
    if (e.y < kMarkH && e.x > kHeaderW)
    {
        // the band: markers and the repeated section
        const int mi = markerAt((float)e.x);
        if (e.mods.isPopupMenu())
        {
            owner.showMarkerMenu(mi, snap(beatOfX((float)e.x), e.mods.isShiftDown()));
            return;
        }
        if (mi >= 0)
        {
            drag = Drag::marker;
            dragMarker = mi;
            markerStart = s.markers[(size_t)mi].beat;
        }
        else if (std::abs((float)e.x - xOfBeat(s.cycleStart)) < 5.0f)
            drag = Drag::cycleA;
        else if (std::abs((float)e.x - xOfBeat(s.cycleEnd)) < 5.0f)
            drag = Drag::cycleB;
        else
            drag = Drag::cycleNew;
        owner.hintChanged();
        return;
    }
    if (e.y < kRulerH)
    {
        if (e.x > kHeaderW)
        {
            // the ruler: the own clock jumps there (the host's position is the host's)
            drag = Drag::seek;
            owner.proc.autoSeek(juce::jmax(0.0, snap(beatOfX((float)e.x), e.mods.isAltDown())));
        }
        owner.hintChanged();
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
    if (onLowerEdge(li, e.y) && !e.mods.isPopupMenu() && !e.mods.isMiddleButtonDown())
    {
        drag = Drag::resize;
        dragLane = li;
        resizeStart = laneHeight(li);
        owner.hintChanged();
        return;
    }
    const auto r = laneRect(li);
    if (e.x < kHeaderW)
    {
        // header buttons
        for (int b = 0; b < 3; ++b)
            if (headerButton(r, b).contains(e.getPosition()))
            {
                if (owner.selLane != li)
                {
                    owner.selPoint = -1;
                    owner.clearSelection();
                }
                owner.selLane = li;
                if (b == 2)
                {
                    owner.syncControls();
                    owner.showLaneMenu(li);
                    return;
                }
                owner.pushUndo();
                auto &l = s.lanes[(size_t)li];
                if (b == 0)
                    l.on = !l.on;
                else
                    l.armed = !l.armed;
                owner.changed();
                return;
            }
        if (owner.selLane != li)
        {
            owner.selPoint = -1;
            owner.clearSelection();
        }
        owner.selLane = li;
        if (e.mods.isPopupMenu())
        {
            owner.showLaneMenu(li);
            return;
        }
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
    {
        owner.selPoint = -1;
        owner.clearSelection();
        const auto before = laneRect(li);
        owner.selLane = li;
        if (laneRect(li) != before && !e.mods.isPopupMenu())
        {
            // the chosen lane grows (or the one before it shrinks): this click only chooses it -
            // a point set now would land somewhere else than under the mouse
            owner.syncControls();
            repaint();
            owner.hintChanged();
            return;
        }
    }
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
            // the section under the mouse
            const int seg = autoFindSegment(l.pts, autoLocalTime(l, beatOfX((float)e.x)));
            owner.selPoint = seg;
            owner.syncControls();
            owner.showPointMenu(li, seg);
        }
        return;
    }
    dragLane = li;
    if (owner.tool == VerlaufView::toolPencil)
    {
        ensureUndo();
        drag = Drag::pencil;
        stroke.clear();
        stroke.push_back({VerlaufView::localOf(l, beatOfX((float)e.x)), valueOfY(c, (float)e.y)});
        repaint();
        return;
    }
    if (owner.tool == VerlaufView::toolStairs)
    {
        ensureUndo();
        drag = Drag::stairs;
        lastStairCell = std::numeric_limits<double>::quiet_NaN();
        mouseDrag(e);
        return;
    }
    // points
    if (hit >= 0)
    {
        if (e.mods.isShiftDown())
        {
            // Shift + click: to the selection (or out of it)
            auto &sp = owner.selPts;
            if (sp.empty() && owner.selPoint >= 0 && owner.selPoint < (int)l.pts.size() && owner.selPoint != hit)
                sp.push_back(owner.selPoint);
            if (owner.isSelected(hit))
                sp.erase(std::find(sp.begin(), sp.end(), hit));
            else
                sp.push_back(hit);
            std::sort(sp.begin(), sp.end());
            owner.selPoint = hit;
            owner.syncControls();
            repaint();
            if (!owner.isSelected(hit))
            {
                dragLane = -1;
                return;
            }
        }
        else if (!owner.isSelected(hit))
            owner.selPts = {hit};
        owner.selPoint = hit;
        dragPoint = hit;
        drag = Drag::point;
        dragOrig = l.pts;
        owner.syncControls();
        repaint();
        return;
    }
    const int seg = lineAt(li, e.position);
    if (seg >= 0 && !e.mods.isAltDown())
    {
        // the line: a click chooses this section, a drag bends it
        owner.selPoint = seg;
        owner.clearSelection();
        drag = Drag::line;
        bendStart = l.pts[(size_t)seg].bend;
        owner.syncControls();
        repaint();
        owner.hintChanged();
        return;
    }
    if (seg >= 0)
    {
        // Alt on the line: a point right here (and it can be dragged on)
        addPointAt(li, e.position, e.mods.isShiftDown());
        drag = Drag::point;
        dragOrig = l.pts;
        return;
    }
    // empty space: a click sets a point (on release), a drag draws a frame
    drag = Drag::box;
    boxBase = e.mods.isShiftDown() ? owner.selPts : std::vector<int>{};
    owner.hintChanged();
}

void VerlaufTimeline::mouseDrag(const juce::MouseEvent &e)
{
    auto &s = owner.set();
    hover = e.position;
    if (!dragMoved && e.position.getDistanceFrom(downPos) > 3.0f)
    {
        dragMoved = true;
        owner.hintChanged();
    }
    if (drag == Drag::seek)
    {
        owner.proc.autoSeek(juce::jmax(0.0, snap(beatOfX((float)e.x), e.mods.isAltDown())));
        return;
    }
    if (drag == Drag::marker || drag == Drag::cycleA || drag == Drag::cycleB || drag == Drag::cycleNew)
    {
        if (!dragMoved)
            return;
        ensureUndo();
        const double b = juce::jmax(0.0, snap(beatOfX(e.position.x), e.mods.isShiftDown()));
        const double minLen = s.grid > 0.0 ? s.grid : 0.25;
        if (drag == Drag::marker && dragMarker >= 0 && dragMarker < (int)s.markers.size())
            s.markers[(size_t)dragMarker].beat = b;
        else if (drag == Drag::cycleA)
            s.cycleStart = juce::jmin(b, s.cycleEnd - minLen);
        else if (drag == Drag::cycleB)
            s.cycleEnd = juce::jmax(b, s.cycleStart + minLen);
        else
        {
            const double a = juce::jmax(0.0, snap(beatOfX(downPos.x), e.mods.isShiftDown()));
            s.cycleStart = juce::jmin(a, b);
            s.cycleEnd = juce::jmax(juce::jmax(a, b), s.cycleStart + minLen);
            s.cycle = true;
        }
        owner.changed();
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
    if (drag == Drag::resize)
    {
        if (!dragMoved)
            return;
        ensureUndo();
        l.height = juce::jlimit(kMinLaneH, kMaxLaneH, resizeStart + e.getDistanceFromDragStartY());
        owner.changed();
        return;
    }
    const auto c = curveArea(laneRect(dragLane));
    const int steps = owner.proc.autoSteps(owner.laneParam(dragLane));
    if (drag == Drag::point && dragPoint >= 0 && dragPoint < (int)dragOrig.size() && dragOrig.size() == l.pts.size())
    {
        if (!dragMoved)
            return; // (a click on a point only chooses it)
        ensureUndo();
        // the point under the mouse follows it (on the grid); the other chosen ones keep their
        // distance to it, all of them between the points that are not chosen
        const auto &anchor = dragOrig[(size_t)dragPoint];
        double t = VerlaufView::localOf(l, snap(beatOfX(e.position.x), e.mods.isShiftDown()));
        if (e.mods.isCommandDown())
            t = anchor.t; // Ctrl: only the value
        double dt = t - anchor.t;
        // (the value moves by as much as the mouse did: grabbing a point does not make it jump)
        float dv = valueOfY(c, e.position.y) - valueOfY(c, downPos.y);
        std::vector<int> moving = owner.selPts;
        if (std::find(moving.begin(), moving.end(), dragPoint) == moving.end())
            moving = {dragPoint};
        auto chosen = [&](int i) { return std::find(moving.begin(), moving.end(), i) != moving.end(); };
        const int n = (int)dragOrig.size();
        double lo = -1.0e18, hi = 1.0e18;
        float vlo = -1.0f, vhi = 1.0f;
        for (int i : moving)
        {
            int p = i - 1;
            while (p >= 0 && chosen(p))
                --p;
            if (p >= 0)
                lo = juce::jmax(lo, dragOrig[(size_t)p].t - dragOrig[(size_t)i].t + 1.0e-4);
            int q = i + 1;
            while (q < n && chosen(q))
                ++q;
            if (q < n)
                hi = juce::jmin(hi, dragOrig[(size_t)q].t - dragOrig[(size_t)i].t - 1.0e-4);
            vlo = juce::jmax(vlo, -dragOrig[(size_t)i].v);
            vhi = juce::jmin(vhi, 1.0f - dragOrig[(size_t)i].v);
        }
        dt = juce::jlimit(lo, juce::jmax(lo, hi), dt);
        dv = juce::jlimit(vlo, juce::jmax(vlo, vhi), dv);
        l.pts = dragOrig;
        for (int i : moving)
        {
            auto &pt = l.pts[(size_t)i];
            pt.t += dt;
            pt.v = juce::jlimit(0.0f, 1.0f, pt.v + dv);
            if (steps >= 2)
                pt.v = autoQuantise(pt.v, steps);
        }
        owner.changed();
        return;
    }
    if ((drag == Drag::bend || drag == Drag::line) && owner.selPoint >= 0 && owner.selPoint < (int)l.pts.size())
    {
        if (!dragMoved)
            return;
        ensureUndo();
        auto &a = l.pts[(size_t)owner.selPoint];
        const bool rising = owner.selPoint + 1 < (int)l.pts.size() && l.pts[(size_t)owner.selPoint + 1].v >= a.v;
        a.bend = juce::jlimit(-1.0f, 1.0f, bendStart + (rising ? -1.0f : 1.0f) * (float)e.getDistanceFromDragStartY() / 80.0f);
        owner.changed();
        return;
    }
    if (drag == Drag::box)
    {
        if (!dragMoved)
            return;
        // the points inside the frame (plus what was chosen before, with Shift)
        const auto box = juce::Rectangle<float>(downPos, e.position);
        std::vector<int> sp = boxBase;
        for (int i = 0; i < (int)l.pts.size(); ++i)
        {
            const auto &pt = l.pts[(size_t)i];
            if (box.contains(xOfBeat(VerlaufView::beatOfLocal(l, pt.t)), yOfValue(c, pt.v)) &&
                std::find(sp.begin(), sp.end(), i) == sp.end())
                sp.push_back(i);
        }
        std::sort(sp.begin(), sp.end());
        owner.selPts = sp;
        owner.selPoint = sp.empty() ? -1 : sp.front();
        owner.syncControls();
        repaint();
        owner.hintChanged();
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
        owner.clearSelection();
        owner.changed();
    }
}

void VerlaufTimeline::mouseUp(const juce::MouseEvent &e)
{
    auto &s = owner.set();
    if (drag == Drag::marker && !dragMoved && dragMarker >= 0 && dragMarker < (int)s.markers.size())
        owner.proc.autoSeek(s.markers[(size_t)dragMarker].beat); // a click on a marker: there
    else if (drag == Drag::cycleNew && !dragMoved)
        owner.proc.autoSeek(juce::jmax(0.0, snap(beatOfX(downPos.x), e.mods.isShiftDown())));
    if (drag == Drag::marker && dragMoved)
        owner.sortMarkers();
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
        owner.clearSelection();
        owner.changed();
    }
    else if (drag == Drag::box && !dragMoved && owner.validLane(dragLane))
        addPointAt(dragLane, downPos, e.mods.isShiftDown()); // a click on empty space: a point
    stroke.clear();
    drag = Drag::none;
    dragLane = dragPoint = -1;
    dragOrig.clear();
    owner.syncControls();
    repaint();
    owner.hintChanged();
}

void VerlaufTimeline::mouseDoubleClick(const juce::MouseEvent &e)
{
    if (e.y < kMarkH && e.x > kHeaderW)
    {
        // the band: a marker renamed, or a new one
        const int mi = markerAt((float)e.x);
        if (mi >= 0)
            owner.renameMarker(mi);
        else
            owner.addMarkerAt(juce::jmax(0.0, snap(beatOfX((float)e.x), e.mods.isShiftDown())));
        return;
    }
    const int li = laneAt(e.y);
    if (li < 0 || e.y < kRulerH)
        return;
    if (onLowerEdge(li, e.y))
    {
        // the lower edge: the lane's height automatic again
        owner.pushUndo();
        owner.set().lanes[(size_t)li].height = 0;
        owner.changed();
        return;
    }
    if (e.x < kHeaderW)
        return;
    const int hit = pointAt(li, e.position);
    if (hit >= 0)
    {
        auto &l = owner.set().lanes[(size_t)li];
        owner.pushUndo();
        l.pts.erase(l.pts.begin() + hit);
        owner.selPoint = -1;
        owner.clearSelection();
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
    toggle(pieceButton, rysum::tr("St\xc3\xbc"
                                               "cke: neu, \xc3\xb6"
                                               "ffnen, speichern (Strg+S), die mitgelieferten und "
                                               "die eigenen (Dokumente/Rysum/St\xc3\xbc"
                                               "cke)"));
    pieceButton.onClick = [this] { showPieceMenu(); };
    toggle(onButton, rysum::tr("Alle Spuren an / aus (aus: die Regler gelten wie von Hand gestellt)"));
    onButton.setClickingTogglesState(true);
    onButton.onClick = [this]
    {
        set().on = onButton.getToggleState();
        changed();
    };
    toggle(clockHost, rysum::tr("Die Uhr folgt der Songposition des Hosts (Takte). Ohne Host "
                                             "(Standalone) l\xc3\xa4uft die eigene Uhr."));
    toggle(clockOwn, rysum::tr("Die eigene Uhr des Plugins, unabh\xc3\xa4ngig vom Host: \xe2\x96\xb6 / "
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
    toggle(playButton, rysum::tr("Eigene Uhr starten"));
    toggle(stopButton, rysum::tr("Eigene Uhr anhalten"));
    toggle(homeButton, rysum::tr("Zur\xc3\xbc"
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
    toggle(cycleButton, rysum::tr("A\xe2\x80\x93"
                                               "B: die eigene Uhr wiederholt den Abschnitt im Band \xc3\xbc"
                                               "ber dem Lineal (dort ziehen, oder Rechtsklick auf eine Marke: "
                                               "bis zur n\xc3\xa4"
                                               "chsten wiederholen)"));
    cycleButton.setClickingTogglesState(true);
    cycleButton.onClick = [this]
    {
        pushUndo();
        set().cycle = cycleButton.getToggleState();
        changed();
    };
    toggle(keyStart, rysum::tr("Die eigene Uhr startet von selbst mit der ersten gespielten Taste "
                                            "(statt mit \xe2\x96\xb6)"));
    keyStart.setClickingTogglesState(true);
    keyStart.onClick = [this]
    {
        set().startOnKey = keyStart.getToggleState();
        changed();
    };
    toggle(recButton, rysum::tr("Aufnahme: bewegt man w\xc3\xa4hrend die Uhr l\xc3\xa4uft einen Regler, "
                                             "wird die Bewegung in seine Spur geschrieben (Spuren mit \xe2\x97\x8f im "
                                             "Kopf). Ein Regler ohne Spur bekommt beim Anfassen eine. Ohne Aufnahme "
                                             "gibt die Spur nach, solange man den Regler h\xc3\xa4lt."));
    recButton.setClickingTogglesState(true);
    recButton.onClick = [this]
    {
        if (recButton.getToggleState())
            pushUndo();
        proc.setAutoRecord(recButton.getToggleState());
    };
    toggle(toolPts, rysum::tr("Punkte: klicken setzt, ziehen verschiebt, ein Rahmen w\xc3\xa4hlt mehrere, "
                                           "Klick auf die Linie w\xc3\xa4hlt den Abschnitt (ziehen biegt ihn), "
                                           "Doppelklick l\xc3\xb6scht - die Zeile unter den Spuren sagt, was wo geht"));
    toggle(toolPen, rysum::tr("Stift: freihand zeichnen, ersetzt die Punkte unter dem Strich"));
    toggle(toolStair, rysum::tr("Treppe: je Rasterfeld eine Stufe zeichnen"));
    for (auto *b : {&toolPts, &toolPen, &toolStair})
        b->setRadioGroupId(7102);
    toolPts.onClick = [this] { tool = toolPoints; hintChanged(); };
    toolPen.onClick = [this] { tool = toolPencil; hintChanged(); };
    toolStair.onClick = [this] { tool = toolStairs; hintChanged(); };
    toolPts.setToggleState(true, juce::dontSendNotification);
    toggle(zoomIn, rysum::tr("N\xc3\xa4her (Strg + Mausrad)"));
    toggle(zoomOut, rysum::tr("Weiter (Strg + Mausrad)"));
    toggle(zoomAll, rysum::tr("Alle Punkte zeigen"));
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
    toggle(undoButton, rysum::tr("Letzte \xc3\x84nderung zur\xc3\xbc"
                                              "cknehmen (Strg+Z, Strg+Y: wiederholen)"));
    undoButton.onClick = [this] { undo(); };
    toggle(addButton, rysum::tr("Eine Spur f\xc3\xbcr einen Regler anlegen"));
    addButton.onClick = [this] { showAddMenu(); };
    gridBox.addItemList(gridNames(), 1);
    gridBox.onChange = [this]
    {
        if (updating)
            return;
        set().grid = kGridBeats[juce::jlimit(0, 7, gridBox.getSelectedItemIndex())];
        timeline.repaint();
    };
    gridBox.setTooltip(rysum::tr("Raster f\xc3\xbcr Punkte und Treppen (Umschalt beim Ziehen: ohne)"));
    addAndMakeVisible(gridBox);

    tempoRow.format = [](double v) { return juce::String(v, 1) + " BPM"; };
    tempoRow.slider.setTooltip(rysum::tr("Tempo der eigenen Uhr: 60 BPM = ein Schlag je Sekunde"));
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
    // (beats, as the ruler counts them: 4 beats = 1 bar)
    auto beatsFmt = [](double v)
    {
        juce::String t = beatsText(v) + rysum::tr(" Schl\xc3\xa4ge");
        if (v >= 4.0 && std::abs(v / 4.0 - std::round(v / 4.0)) < 1.0e-6)
            t << juce::String::fromUTF8(" \xc2\xb7 ") << juce::String((int)std::round(v / 4.0))
              << ((int)std::round(v / 4.0) == 1 ? rysum::tr(" Takt") : rysum::tr(" Takte"));
        return t;
    };
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
    lenRow.slider.setTooltip(rysum::tr("Die L\xc3\xa4nge der Schleife (und des r\xc3\xbc"
                                                    "ckw\xc3\xa4rts gelesenen Bereichs), in Schl\xc3\xa4gen der Spur"));
    speedRow.slider.setTooltip(rysum::tr("Zeitdehnung: \xc3\x97 2 = doppelt so schnell, \xc3\x97 0,5 = halb so "
                                                      "schnell. Spuren mit verschiedenen L\xc3\xa4ngen und Tempi "
                                                      "laufen gegeneinander."));
    offsetRow.slider.setTooltip(rysum::tr("Wo die Spur auf der Uhr beginnt"));
    for (auto *b : {&loopButton, &revButton})
    {
        b->setClickingTogglesState(true);
        addAndMakeVisible(*b);
    }
    loopButton.setTooltip(rysum::tr("Die Spur wiederholt ihre L\xc3\xa4nge immer wieder"));
    revButton.setTooltip(rysum::tr("Die Spur wird von hinten gelesen"));
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
    // with every key the lane starts again: per pipe for every note on its own, else from the last key
    perNoteButton.setClickingTogglesState(true);
    perNoteButton.setTooltip(rysum::tr("Die Spur beginnt mit jedem Anschlag von vorn, statt der Uhr zu folgen. Eine Spur \"je Pfeife\" l\xc3\xa4uft "
        "f\xc3\xbcr jede Taste f\xc3\xbcr sich: jeder Ton hat seinen eigenen Verlauf (z. B. ein Register blendet je Ton langsam ein, ein anderes "
        "aus \xe2\x80\x93 Vektorsynthese). Jede andere Spur beginnt mit der zuletzt gedr\xc3\xbc" "ckten Taste neu. Ihre Uhr l\xc3\xa4uft im Tempo, "
        "auch wenn der Host steht."));
    perNoteButton.onClick = [this]
    {
        if (!validLane(selLane) || set().lanes[(size_t)selLane].paramId == kAutoTempoId)
            return;
        pushUndo();
        set().lanes[(size_t)selLane].perNote = perNoteButton.getToggleState();
        changed();
    };
    addAndMakeVisible(perNoteButton);
    stopBox.setTooltip(rysum::tr("Auf welche Pfeifen die Spur wirkt: alle, oder nur die eines Registers. Mehrere Spuren auf je ein Register "
        "blenden zwischen Registern \xc3\xbc" "ber (z. B. Pfeife: Registerwind)."));
    stopBox.addItem(rysum::tr("alle Register"), 1);
    for (int k = 0; k < kNumStops; ++k)
        stopBox.addItem(rysum::tr("nur ") + rysum::tr(kStops[(size_t)kDisplayOrder[(size_t)k]].label), 2 + kDisplayOrder[(size_t)k]);
    stopBox.onChange = [this]
    {
        if (updating || !selectedIsPipeLane())
            return;
        pushUndo();
        set().lanes[(size_t)selLane].stop = stopBox.getSelectedId() - 2;
        changed();
    };
    addChildComponent(stopBox);
    clearButton.setTooltip(rysum::tr("Alle Punkte dieser Spur l\xc3\xb6schen"));
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

    // spreading through the room (per-pipe lanes)
    spreadButton.setClickingTogglesState(true);
    spreadButton.setTooltip(rysum::tr("Die Geste breitet sich vom Ursprung (rechts im Plan) durch den Raum aus: jede Pfeife h\xc3\xb6rt "
        "sie, wenn die Front bei ihr ankommt. Aus: alle Pfeifen zugleich."));
    inwardButton.setClickingTogglesState(true);
    inwardButton.setTooltip(rysum::tr("Die Front l\xc3\xa4uft von weit drau\xc3\x9f"
        "en (25 m) auf den Ursprung zu: die fernen Pfeifen zuerst"));
    waveRow.format = [](double v) { return juce::String(v, v < 10.0 ? 1 : 0) + rysum::tr(" m / Schlag"); };
    waveRow.slider.setTooltip(rysum::tr("Wie schnell die Front l\xc3\xa4uft. Langsam: aus einer Geste wird ein Kanon im Raum. "
        "(Schall: etwa 170 m je Schlag bei 60 BPM.)"));
    spreadButton.onClick = [this]
    {
        if (!selectedIsPipeLane())
            return;
        pushUndo();
        set().lanes[(size_t)selLane].spread = spreadButton.getToggleState();
        changed();
    };
    inwardButton.onClick = [this]
    {
        if (!selectedIsPipeLane())
            return;
        pushUndo();
        set().lanes[(size_t)selLane].inward = inwardButton.getToggleState();
        changed();
    };
    waveRow.onDragStart = [this] { pushUndo(); };
    waveRow.onChange = [this](double v)
    {
        if (updating || !selectedIsPipeLane())
            return;
        set().lanes[(size_t)selLane].waveSpeed = (float)v;
        changed();
    };
    addChildComponent(spreadButton);
    addChildComponent(inwardButton);
    addChildComponent(waveRow);
    addChildComponent(originPad);

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
    shapeButtons[asCurve].setTooltip(rysum::tr("Gerade oder gebogene Linie zum Punkt danach"));
    shapeButtons[asStep].setTooltip(rysum::tr("H\xc3\xa4lt den Wert bis zum Punkt danach, dann springt er"));
    shapeButtons[asWave].setTooltip(rysum::tr("Die Linie plus eine Sinuswelle (Tempo: Wellen je Schlag)"));
    shapeButtons[asJitter].setTooltip(rysum::tr("Die Linie plus weiches zuf\xc3\xa4lliges Wandern "
                                                             "(jedes Mal gleich)"));
    shapeButtons[asStairs].setTooltip(rysum::tr("Die Linie in Stufen (Tempo: Stufen je Schlag)"));
    bendRow.format = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + " %"; };
    rateRow.format = [](double v) { return juce::String(v, v < 1.0 ? 2 : 1) + rysum::tr(" / Schlag"); };
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
    // variation from pass to pass
    everyRow.slider.setRange(1.0, 8.0, 1.0);
    chanceRow.format = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + " %"; };
    everyRow.format = [](double v)
    {
        const int n = juce::roundToInt(v);
        return n <= 1 ? rysum::tr("jedes Mal") : rysum::tr("jedes ") + juce::String(n) + ".";
    };
    varyRow.format = [](double v) { return juce::String::fromUTF8("\xc2\xb1 ") + juce::String(juce::roundToInt(v * 100.0)) + " %"; };
    segEdit(chanceRow, [](AutoPoint &p, double v) { p.chance = (float)v; });
    segEdit(everyRow, [](AutoPoint &p, double v) { p.every = juce::jlimit(1, 8, juce::roundToInt(v)); });
    segEdit(varyRow, [](AutoPoint &p, double v) { p.vary = (float)v; });
    chanceRow.slider.setTooltip(rysum::tr("Wie wahrscheinlich der Abschnitt sich bei einem Durchgang bewegt \xe2\x80\x93 sonst bleibt der Wert "
        "stehen. Jeder Durchgang einer Schleife w\xc3\xbcrfelt neu (und beim n\xc3\xa4"
        "chsten Mal an derselben Stelle gleich)."));
    everyRow.slider.setTooltip(rysum::tr("Der Abschnitt bewegt sich nur bei jedem n-ten Durchgang der "
                                                      "Schleife, sonst bleibt der Wert stehen."));
    varyRow.slider.setTooltip(rysum::tr("Der Wert dieses Punkts wandert von Durchgang zu Durchgang um "
                                                     "bis zu so viel."));

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
    {
        if (!validLane(lane))
            return {};
        const auto &id = proc.getAuto().lanes[(size_t)lane].paramId;
        if (id == kAutoTempoId)
            return rysum::tr("Tempo der eigenen Uhr");
        if (const int t = autoPipeTarget(id); t >= 0)
        {
            const int st = proc.getAuto().lanes[(size_t)lane].stop;
            return pipeTargetNames()[t] +
                   (st >= 0 ? juce::String::fromUTF8(" \xc2\xb7 ") + rysum::tr(kStops[(size_t)st].label) : juce::String());
        }
        if (const int s = autoPathStop(id); s >= 0)
            return rysum::tr("Bahn: ") + rysum::tr(kStops[(size_t)s].label);
        return id;
    }
    return rysum::trText(proc.getParamAt(p)->getName(64));
}

juce::String VerlaufView::valueText(int lane, float v) const
{
    const int p = laneParam(lane);
    if (p < 0 && validLane(lane))
    {
        const auto &id = proc.getAuto().lanes[(size_t)lane].paramId;
        if (id == kAutoTempoId)
            return juce::String(autoTempoBpm(v), 1) + " BPM";
        if (autoPipeTarget(id) >= 0)
        {
            // the offset the pipes get (like a routing: 100 % = the target's full swing)
            const int o = juce::roundToInt(autoPipeOffset(v) * 100.0f);
            return (o > 0 ? "+" : "") + juce::String(o) + " %";
        }
        if (autoPathStop(id) >= 0)
            return juce::String(juce::roundToInt(v * 100.0f)) + rysum::tr(" % der Bahn");
    }
    if (p < 0)
        return juce::String(juce::roundToInt(v * 100.0f)) + " %";
    auto *prm = proc.getParamAt(p);
    return rysum::trText(prm->getText(autoQuantise(v, proc.autoSteps(p)), 64));
}

juce::String VerlaufView::timeText(double beats) const
{
    // bar.beat (the unit of the lane settings); with the own clock also the time in seconds
    if (proc.isAutoHostClock())
        return clockText(proc, beats);
    return barText(beats) + juce::String::fromUTF8(" \xc2\xb7 ") + secondsText(beats);
}

juce::String VerlaufView::clockText(const RysumProcessor &proc, double beats)
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
    selPts.clear();
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
    selPts.clear();
    changed();
}

bool VerlaufView::keyPressed(const juce::KeyPress &k)
{
    if (k == juce::KeyPress('s', juce::ModifierKeys::commandModifier, 0))
    {
        savePiece();
        return true;
    }
    // markers: M sets one at the clock, Bild auf / ab jump to the previous / next one
    if (k == juce::KeyPress('m') || k == juce::KeyPress('M'))
    {
        addMarkerAt(juce::jmax(0.0, proc.getAutoNow()));
        return true;
    }
    if (k == juce::KeyPress::pageUpKey)
    {
        jumpMarker(-1);
        return true;
    }
    if (k == juce::KeyPress::pageDownKey)
    {
        jumpMarker(+1);
        return true;
    }
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
        deleteSelection();
        return true;
    }
    if (k == juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0) && validLane(selLane))
    {
        // every point of the chosen lane
        selPts.clear();
        for (int i = 0; i < (int)set().lanes[(size_t)selLane].pts.size(); ++i)
            selPts.push_back(i);
        selPoint = selPts.empty() ? -1 : 0;
        syncControls();
        timeline.repaint();
        hintChanged();
        return true;
    }
    if (k == juce::KeyPress('c', juce::ModifierKeys::commandModifier, 0))
    {
        copySelection(false);
        return true;
    }
    if (k == juce::KeyPress('x', juce::ModifierKeys::commandModifier, 0))
    {
        copySelection(true);
        return true;
    }
    if (k == juce::KeyPress('v', juce::ModifierKeys::commandModifier, 0))
    {
        paste();
        return true;
    }
    if (k == juce::KeyPress::escapeKey)
    {
        selPts.clear();
        timeline.repaint();
        hintChanged();
        return true;
    }
    if (k == juce::KeyPress::spaceKey)
    {
        proc.autoPlay(!proc.isAutoOwnRunning());
        return true;
    }
    return false;
}

void VerlaufView::deleteSelection()
{
    if (!validLane(selLane))
        return;
    auto &l = set().lanes[(size_t)selLane];
    std::vector<int> gone = selPts;
    if (gone.empty() && selPoint >= 0 && selPoint < (int)l.pts.size())
        gone = {selPoint};
    gone.erase(std::remove_if(gone.begin(), gone.end(), [&](int i) { return i < 0 || i >= (int)l.pts.size(); }),
               gone.end());
    if (gone.empty())
        return;
    pushUndo();
    std::sort(gone.begin(), gone.end());
    for (auto it = gone.rbegin(); it != gone.rend(); ++it)
        l.pts.erase(l.pts.begin() + *it);
    selPoint = -1;
    selPts.clear();
    changed();
    hintChanged();
}

void VerlaufView::copySelection(bool cut)
{
    if (!validLane(selLane))
        return;
    const auto &l = set().lanes[(size_t)selLane];
    std::vector<int> which = selPts;
    if (which.empty() && selPoint >= 0 && selPoint < (int)l.pts.size())
        which = {selPoint};
    if (which.empty())
        return;
    std::sort(which.begin(), which.end());
    clip.clear();
    const double t0 = l.pts[(size_t)which.front()].t;
    for (int i : which)
        if (i >= 0 && i < (int)l.pts.size())
        {
            auto p = l.pts[(size_t)i];
            p.t -= t0;
            clip.push_back(p);
        }
    if (cut)
        deleteSelection();
}

void VerlaufView::paste()
{
    if (clip.empty() || !validLane(selLane))
        return;
    auto &l = set().lanes[(size_t)selLane];
    // where: under the mouse when it is over the curves, else at the clock (on the grid)
    const auto m = timeline.getMouseXYRelative();
    const bool overCurves = timeline.isMouseOver() && m.x > VerlaufTimeline::kHeaderW && m.y > VerlaufTimeline::kRulerH;
    const double at = timeline.snap(overCurves ? timeline.beatOfX((float)m.x) : proc.getAutoNow(), false);
    const double t0 = localOf(l, at);
    const int steps = proc.autoSteps(laneParam(selLane));
    std::vector<AutoPoint> with;
    for (auto p : clip)
    {
        p.t += t0;
        if (steps >= 2)
            p.v = autoQuantise(p.v, steps);
        with.push_back(p);
    }
    pushUndo();
    autoReplaceRange(l, with.front().t, with.back().t, with);
    // the pasted points are the selection now
    selPts.clear();
    for (int i = 0; i < (int)l.pts.size(); ++i)
        for (const auto &p : with)
            if (std::abs(l.pts[(size_t)i].t - p.t) < 1.0e-9 && l.pts[(size_t)i].v == p.v)
            {
                selPts.push_back(i);
                break;
            }
    selPoint = selPts.empty() ? -1 : selPts.front();
    changed();
    hintChanged();
}

// ---------------------------------------------------------------------------
// markers and the repeated section
// ---------------------------------------------------------------------------
void VerlaufView::sortMarkers()
{
    auto &m = set().markers;
    std::stable_sort(m.begin(), m.end(), [](const AutoMarker &a, const AutoMarker &b) { return a.beat < b.beat; });
    changed();
}

void VerlaufView::addMarkerAt(double beat)
{
    auto &s = set();
    // the next free letter: A, B, C ... (then A2, B2 ...)
    juce::String name;
    for (int round = 1; name.isEmpty(); ++round)
        for (char c = 'A'; c <= 'Z' && name.isEmpty(); ++c)
        {
            const juce::String n = juce::String::charToString((juce::juce_wchar)c) + (round > 1 ? juce::String(round) : juce::String());
            if (std::none_of(s.markers.begin(), s.markers.end(), [&](const AutoMarker &m) { return m.name == n; }))
                name = n;
        }
    pushUndo();
    s.markers.push_back({beat, name});
    sortMarkers();
    hintChanged();
}

void VerlaufView::renameMarker(int i)
{
    if (i < 0 || i >= (int)set().markers.size())
        return;
    auto *w = new juce::AlertWindow(rysum::tr("Marke"), rysum::tr("Name der Marke"), juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor("name", set().markers[(size_t)i].name);
    w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton(rysum::tr("Abbrechen"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<VerlaufView> safe(this);
    w->enterModalState(true,
                       juce::ModalCallbackFunction::create(
                           [safe, w, i](int r)
                           {
                               if (safe == nullptr || r != 1 || i >= (int)safe->set().markers.size())
                                   return;
                               const auto name = w->getTextEditorContents("name").trim();
                               if (name.isEmpty())
                                   return;
                               safe->pushUndo();
                               safe->set().markers[(size_t)i].name = name.substring(0, 24);
                               safe->changed();
                           }),
                       true);
}

void VerlaufView::showMarkerMenu(int i, double beat)
{
    juce::PopupMenu m;
    const auto &s = set();
    if (i >= 0)
    {
        m.addSectionHeader(rysum::tr("Marke ") + s.markers[(size_t)i].name);
        m.addItem(1, rysum::tr("Hierhin springen"));
        m.addItem(2, rysum::tr("Umbenennen"));
        m.addItem(3, rysum::tr("Bis zur n\xc3\xa4"
                                            "chsten Marke wiederholen"),
                  i + 1 < (int)s.markers.size());
        m.addItem(4, rysum::tr("L\xc3\xb6schen"));
    }
    else
    {
        m.addItem(10, rysum::tr("Marke hier setzen"));
        m.addItem(11, rysum::tr("Marke an der Uhr setzen  (M)"));
        m.addSeparator();
        m.addItem(12, rysum::tr("Wiederholung beginnt hier (A)"));
        m.addItem(13, rysum::tr("Wiederholung endet hier (B)"));
        m.addItem(14, rysum::tr("Wiederholung an"), true, s.cycle);
    }
    juce::Component::SafePointer<VerlaufView> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options(),
                    [safe, i, beat](int r)
                    {
                        if (safe == nullptr || r <= 0)
                            return;
                        auto &S = safe->set();
                        const double minLen = S.grid > 0.0 ? S.grid : 0.25;
                        if (i >= 0 && i < (int)S.markers.size())
                        {
                            if (r == 1)
                                safe->proc.autoSeek(S.markers[(size_t)i].beat);
                            else if (r == 2)
                                safe->renameMarker(i);
                            else if (r == 3 && i + 1 < (int)S.markers.size())
                            {
                                safe->pushUndo();
                                S.cycleStart = S.markers[(size_t)i].beat;
                                S.cycleEnd = juce::jmax(S.cycleStart + minLen, S.markers[(size_t)i + 1].beat);
                                S.cycle = true;
                                safe->changed();
                            }
                            else if (r == 4)
                            {
                                safe->pushUndo();
                                S.markers.erase(S.markers.begin() + i);
                                safe->changed();
                            }
                            return;
                        }
                        if (r == 10)
                            safe->addMarkerAt(beat);
                        else if (r == 11)
                            safe->addMarkerAt(juce::jmax(0.0, safe->proc.getAutoNow()));
                        else
                        {
                            safe->pushUndo();
                            if (r == 12)
                                S.cycleStart = juce::jmin(beat, S.cycleEnd - minLen);
                            else if (r == 13)
                                S.cycleEnd = juce::jmax(beat, S.cycleStart + minLen);
                            else if (r == 14)
                                S.cycle = !S.cycle;
                            safe->changed();
                        }
                    });
}

void VerlaufView::jumpMarker(int direction)
{
    const auto &ms = set().markers;
    if (ms.empty())
        return;
    const double now = proc.getAutoNow();
    // (a marker just passed counts as "here": the previous one is the one before it)
    if (direction < 0)
    {
        for (int i = (int)ms.size() - 1; i >= 0; --i)
            if (ms[(size_t)i].beat < now - 0.5)
            {
                proc.autoSeek(ms[(size_t)i].beat);
                return;
            }
        proc.autoSeek(ms.front().beat);
    }
    else
        for (const auto &m : ms)
            if (m.beat > now + 1.0e-3)
            {
                proc.autoSeek(m.beat);
                return;
            }
}

// ---------------------------------------------------------------------------
// the tempo lane: seconds along the clock
// ---------------------------------------------------------------------------
const AutoLane *VerlaufView::tempoLane() const
{
    for (const auto &l : proc.getAuto().lanes)
        if (l.on && l.paramId == kAutoTempoId && !l.pts.empty())
            return &l;
    return nullptr;
}

double VerlaufView::secondsAt(double beats) const
{
    const auto &s = proc.getAuto();
    const auto *tl = tempoLane();
    beats = juce::jmax(0.0, beats);
    if (tl == nullptr)
        return beats * 60.0 / juce::jmax(1.0, s.tempo);
    // the seconds every quarter beat, summed (rebuilt when the piece changes)
    constexpr double step = 0.25;
    constexpr int n = 16384;
    if (secCacheVersion != proc.getAutoVersion() || (int)secCache.size() != n + 1)
    {
        secCache.assign((size_t)n + 1, 0.0);
        for (int i = 0; i < n; ++i)
        {
            const float v = autoValue(*tl, ((double)i + 0.5) * step);
            const double bpm = std::isnan(v) ? s.tempo : autoTempoBpm(v);
            secCache[(size_t)i + 1] = secCache[(size_t)i] + step * 60.0 / bpm;
        }
        secCacheVersion = proc.getAutoVersion();
    }
    const double x = beats / step;
    const int i = juce::jmin(n - 1, (int)std::floor(x));
    const double f = x - (double)i;
    return secCache[(size_t)i] + f * (secCache[(size_t)i + 1] - secCache[(size_t)i]);
}

juce::String VerlaufView::secondsText(double beats) const
{
    const double sec = secondsAt(beats);
    const int m = (int)(sec / 60.0);
    const double r = sec - m * 60.0;
    const bool whole = std::abs(r - std::round(r)) < 1.0e-3;
    return juce::String(m) + ":" + (r < 9.995 ? "0" : "") + juce::String(r, whole ? 0 : 2);
}

// ---------------------------------------------------------------------------
// editing tools
// ---------------------------------------------------------------------------
juce::PopupMenu VerlaufView::paramMenu(int base) const
{
    juce::PopupMenu menu;
    const juce::StringArray groups{"Register",  juce::String::fromUTF8("Traktur \xc2\xb7 Ausgang"),
                                   juce::String::fromUTF8("Verfall"),   juce::String::fromUTF8("Klang"),
                                   juce::String::fromUTF8("Hall"),      "Saal",
                                   juce::String::fromUTF8("Eingang"),   juce::String::fromUTF8("Effekte"),
                                   "Modulation", juce::String::fromUTF8("Summe")};
    std::set<int> taken;
    for (auto &l : proc.getAuto().lanes)
        taken.insert(proc.getParamIndex(l.paramId));
    juce::PopupMenu switches;
    for (int i = 0; i < proc.getNumParams(); ++i)
        if (proc.isAutoTarget(i) && !proc.isModTarget(i))
            switches.addItem(base + i, rysum::trText(proc.getParamAt(i)->getName(64)), true, taken.count(i) > 0);
    if (switches.getNumItems() > 0)
        menu.addSubMenu(rysum::tr("Schalter (Register, Bewegung, Freeze \xe2\x80\xa6)"), switches);
    for (auto &grp : groups)
    {
        juce::PopupMenu sub;
        for (int i = 0; i < proc.getNumParams(); ++i)
            if (proc.isModTarget(i) && proc.isAutoTarget(i) &&
                ModView::paramGroup(proc.getParamAt(i)->getParameterID()) == grp)
                sub.addItem(base + i, rysum::trText(proc.getParamAt(i)->getName(64)), true, taken.count(i) > 0);
        if (sub.getNumItems() > 0)
            menu.addSubMenu(rysum::tr(grp), sub);
    }
    return menu;
}

void VerlaufView::copyLaneTo(const juce::String &paramId)
{
    if (!validLane(selLane) || (int)set().lanes.size() >= kMaxAutoLanes)
        return;
    const int p = proc.getParamIndex(paramId);
    if (!proc.isAutoTarget(p))
        return;
    pushUndo();
    auto &s = set();
    AutoLane l = s.lanes[(size_t)selLane];
    l.paramId = paramId;
    l.armed = false;
    l.solo = false;
    l.height = 0;
    l.spread = false;
    if (const int steps = proc.autoSteps(p); steps >= 2)
        for (auto &pt : l.pts)
        {
            pt.v = autoQuantise(pt.v, steps);
            pt.shape = asStep;
        }
    // (an existing lane on that control gives way: one lane per control)
    s.lanes.erase(std::remove_if(s.lanes.begin(), s.lanes.end(), [&](const AutoLane &x) { return x.paramId == paramId; }),
                  s.lanes.end());
    s.lanes.push_back(std::move(l));
    selLane = (int)s.lanes.size() - 1;
    selPoint = -1;
    selPts.clear();
    changed();
}

void VerlaufView::transformPoints(int kind)
{
    if (!validLane(selLane))
        return;
    auto &l = set().lanes[(size_t)selLane];
    if (l.pts.empty())
        return;
    // the chosen points (or all of them), kept marked through the sort
    std::vector<std::pair<AutoPoint, bool>> all;
    std::vector<int> which = selPts.size() >= 2 ? selPts : std::vector<int>{};
    for (int i = 0; i < (int)l.pts.size(); ++i)
        all.push_back({l.pts[(size_t)i], which.empty() || isSelected(i)});
    double t0 = 1.0e18, t1 = -1.0e18;
    for (auto &[p, on] : all)
        if (on)
        {
            t0 = juce::jmin(t0, p.t);
            t1 = juce::jmax(t1, p.t);
        }
    const int steps = proc.autoSteps(laneParam(selLane));
    const double g = set().grid > 0.0 ? set().grid : 0.25;
    juce::Random rnd;
    pushUndo();
    for (auto &[p, on] : all)
    {
        if (!on)
            continue;
        switch (kind)
        {
        case 1: p.t = t0 + (p.t - t0) * 2.0; break;          // twice as long
        case 2: p.t = t0 + (p.t - t0) * 0.5; break;          // half as long
        case 3: p.v = 1.0f - p.v; p.bend = -p.bend; break;   // upside down
        case 4: p.t = t0 + t1 - p.t; break;                   // backwards
        case 5: p.v = 0.5f + (p.v - 0.5f) * 0.5f; break;     // towards the middle
        case 6: p.v = 0.5f + (p.v - 0.5f) * 1.5f; break;     // apart
        case 7: p.t = std::round(p.t / g) * g; break;        // onto the grid
        case 8:                                              // a little off
            p.v += 0.1f * (rnd.nextFloat() - 0.5f);
            p.t += g * 0.25 * (rnd.nextDouble() - 0.5);
            break;
        default: break;
        }
        p.v = juce::jlimit(0.0f, 1.0f, p.v);
        if (steps >= 2)
            p.v = autoQuantise(p.v, steps);
    }
    std::stable_sort(all.begin(), all.end(), [](const auto &a, const auto &b) { return a.first.t < b.first.t; });
    // two points at one time (onto the grid): the later one goes
    std::vector<std::pair<AutoPoint, bool>> kept;
    for (auto &x : all)
        if (kept.empty() || std::abs(kept.back().first.t - x.first.t) > 1.0e-6)
            kept.push_back(x);
        else
            kept.back() = x;
    l.pts.clear();
    selPts.clear();
    for (int i = 0; i < (int)kept.size(); ++i)
    {
        l.pts.push_back(kept[(size_t)i].first);
        if (kept[(size_t)i].second && !which.empty())
            selPts.push_back(i);
    }
    selPoint = selPts.empty() ? -1 : selPts.front();
    changed();
}

void VerlaufView::fillLane(int kind)
{
    if (!validLane(selLane))
        return;
    auto &s = set();
    auto &l = s.lanes[(size_t)selLane];
    // where: the loop of a looping lane, else what is in view
    double t0, t1;
    if (l.loop)
    {
        t0 = 0.0;
        t1 = l.length;
    }
    else
    {
        t0 = localOf(l, s.viewStart);
        t1 = localOf(l, s.viewStart + s.viewLen);
        if (t1 < t0)
            std::swap(t0, t1);
        t0 = juce::jmax(0.0, t0);
    }
    const double g = s.grid > 0.0 ? s.grid : 1.0;
    const int steps = proc.autoSteps(laneParam(selLane));
    std::vector<AutoPoint> pts;
    auto add = [&](double t, float v, int shape = asCurve)
    {
        AutoPoint p;
        p.t = t;
        p.v = juce::jlimit(0.0f, 1.0f, v);
        p.shape = shape;
        if (steps >= 2)
        {
            p.v = autoQuantise(p.v, steps);
            p.shape = asStep;
        }
        pts.push_back(p);
    };
    juce::Random rnd;
    switch (kind)
    {
    case 1: // a wave: one per bar
    {
        add(t0, 0.5f, asWave);
        pts.back().rate = 0.25f;
        pts.back().depth = 0.8f;
        add(t1, 0.5f);
        break;
    }
    case 2: // a random walk, one step per grid cell
    {
        float v = 0.5f;
        for (double t = t0; t <= t1 + 1.0e-9; t += g)
        {
            add(t, v);
            v = juce::jlimit(0.05f, 0.95f, v + 0.3f * (rnd.nextFloat() - 0.5f));
        }
        break;
    }
    case 3: // random steps
        for (double t = t0; t < t1 - 1.0e-9; t += g)
            add(t, rnd.nextFloat(), asStep);
        add(t1, pts.empty() ? 0.5f : pts.back().v, asStep);
        break;
    case 4: // up
        add(t0, 0.0f);
        add(t1, 1.0f);
        break;
    case 5: // down
        add(t0, 1.0f);
        add(t1, 0.0f);
        break;
    default:
    {
        // Euclidean rhythms: k hits spread as evenly as possible over n grid cells
        static const int ks[]{3, 5, 5, 7, 9}, ns[]{8, 8, 16, 16, 16};
        const int idx = juce::jlimit(0, 4, kind - 10);
        const int k = ks[idx], n = ns[idx];
        for (int i = 0; i < n; ++i)
        {
            const bool hit = ((i * k) % n) < k;
            add(t0 + i * g, hit ? 1.0f : 0.0f, asStep);
        }
        add(t0 + n * g, 0.0f, asStep);
        t1 = juce::jmax(t1, t0 + n * g);
        break;
    }
    }
    if (pts.empty())
        return;
    pushUndo();
    autoReplaceRange(l, t0, t1, pts);
    selPoint = -1;
    selPts.clear();
    changed();
}

// ---------------------------------------------------------------------------
// pieces
// ---------------------------------------------------------------------------
void VerlaufView::pieceError(const juce::String &message)
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                           rysum::tr("St\xc3\xbc"
                                                                  "ck"),
                                           message, "OK", this);
}

void VerlaufView::pieceLoaded()
{
    undoStack.clear();
    redoStack.clear();
    selLane = set().lanes.empty() ? -1 : 0;
    selPoint = -1;
    selPts.clear();
    timeline.scrollY = 0;
    shownVersion = proc.getAutoVersion();
    syncControls();
    timeline.repaint();
    repaint();
}

void VerlaufView::whenPieceSafe(std::function<void()> then)
{
    if (!proc.isPieceChanged() || set().lanes.empty())
    {
        then();
        return;
    }
    const auto name = proc.getPieceName().isNotEmpty() ? proc.getPieceName()
                                                       : rysum::tr("Das unbenannte St\xc3\xbc"
                                                                                "ck");
    auto opts = juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::QuestionIcon)
                    .withTitle(rysum::tr("St\xc3\xbc"
                                                      "ck ge\xc3\xa4ndert"))
                    .withMessage(name + rysum::tr(" hat ungespeicherte \xc3\x84nderungen."))
                    .withButton(rysum::tr("Speichern"))
                    .withButton(rysum::tr("Verwerfen"))
                    .withButton(rysum::tr("Abbrechen"))
                    .withAssociatedComponent(this);
    juce::Component::SafePointer<VerlaufView> safe(this);
    juce::AlertWindow::showAsync(opts,
                                 [safe, then](int r)
                                 {
                                     if (safe == nullptr)
                                         return;
                                     if (r == 1)
                                         safe->savePiece(then); // (and on, once it is saved)
                                     else if (r == 2)
                                         then();
                                 });
}

void VerlaufView::savePiece(std::function<void()> then)
{
    const auto f = proc.getPieceFile();
    if (f == juce::File())
    {
        savePieceAs(std::move(then));
        return;
    }
    juce::String err;
    if (!proc.savePiece(f, err))
    {
        pieceError(err);
        return;
    }
    repaint(titleBox);
    if (then)
        then();
}

void VerlaufView::savePieceAs(std::function<void()> then)
{
    auto folder = RysumProcessor::getPieceFolder();
    folder.createDirectory();
    const auto name = proc.getPieceName().isNotEmpty() ? proc.getPieceName() : rysum::tr("Neues St\xc3\xbc"
                                                                                                     "ck");
    chooser = std::make_unique<juce::FileChooser>(rysum::tr("St\xc3\xbc"
                                                                         "ck speichern"),
                                                  folder.getChildFile(name + RysumProcessor::kPieceExtension),
                                                  juce::String("*") + RysumProcessor::kPieceExtension);
    juce::Component::SafePointer<VerlaufView> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                             juce::FileBrowserComponent::warnAboutOverwriting,
                         [safe, then](const juce::FileChooser &fc)
                         {
                             if (safe == nullptr || fc.getResult() == juce::File())
                                 return;
                             juce::String err;
                             if (!safe->proc.savePiece(fc.getResult(), err))
                             {
                                 safe->pieceError(err);
                                 return;
                             }
                             safe->repaint(safe->titleBox);
                             if (then)
                                 then();
                         });
}

void VerlaufView::openPieceFile(const juce::File &f)
{
    juce::Component::SafePointer<VerlaufView> safe(this);
    whenPieceSafe(
        [safe, f]
        {
            if (safe == nullptr)
                return;
            juce::String err;
            if (!safe->proc.loadPiece(f, err))
                safe->pieceError(err);
            else
                safe->pieceLoaded();
        });
}

void VerlaufView::showPieceMenu()
{
    juce::PopupMenu m;
    m.addItem(1, rysum::tr("Neues St\xc3\xbc"
                                        "ck"));
    m.addItem(2, rysum::tr("\xc3\x96"
                                        "ffnen \xe2\x80\xa6"));
    m.addItem(3, rysum::tr("Speichern  (Strg+S)"), !set().lanes.empty() || proc.getPieceFile() != juce::File());
    m.addItem(4, rysum::tr("Speichern unter \xe2\x80\xa6"));
    m.addItem(5, rysum::tr("Ordner zeigen"));
    m.addSeparator();
    const auto own = RysumProcessor::getPieceFiles();
    m.addSectionHeader(rysum::tr("Meine St\xc3\xbc"
                                              "cke"));
    if (own.isEmpty())
        m.addItem(-1, rysum::tr("(noch keine \xe2\x80\x93 \xe2\x80\x9eSpeichern unter\xe2\x80\x9c legt "
                                             "sie hier ab)"),
                  false);
    for (int i = 0; i < own.size() && i < 200; ++i)
        m.addItem(200 + i, own[i].getFileNameWithoutExtension(), true, own[i] == proc.getPieceFile());
    m.addSectionHeader(rysum::tr("Mitgeliefert"));
    const auto factory = RysumProcessor::factoryPieceNames();
    for (int i = 0; i < factory.size(); ++i)
        m.addItem(100 + i, factory[i], true,
                  proc.getPieceFile() == juce::File() && proc.getPieceName() == factory[i]);
    juce::Component::SafePointer<VerlaufView> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&pieceButton),
                    [safe, own](int r)
                    {
                        if (safe == nullptr || r <= 0)
                            return;
                        auto &P = safe->proc;
                        if (r == 1)
                            safe->whenPieceSafe(
                                [safe]
                                {
                                    if (safe == nullptr)
                                        return;
                                    safe->proc.newPiece();
                                    safe->pieceLoaded();
                                });
                        else if (r == 2)
                        {
                            safe->chooser = std::make_unique<juce::FileChooser>(
                                rysum::tr("St\xc3\xbc"
                                                       "ck \xc3\xb6"
                                                       "ffnen"),
                                RysumProcessor::getPieceFolder(),
                                juce::String("*") + RysumProcessor::kPieceExtension);
                            safe->chooser->launchAsync(juce::FileBrowserComponent::openMode |
                                                           juce::FileBrowserComponent::canSelectFiles,
                                                       [safe](const juce::FileChooser &fc)
                                                       {
                                                           if (safe != nullptr && fc.getResult().existsAsFile())
                                                               safe->openPieceFile(fc.getResult());
                                                       });
                        }
                        else if (r == 3)
                            safe->savePiece();
                        else if (r == 4)
                            safe->savePieceAs();
                        else if (r == 5)
                        {
                            auto folder = RysumProcessor::getPieceFolder();
                            folder.createDirectory();
                            folder.revealToUser();
                        }
                        else if (r >= 200 && r - 200 < own.size())
                            safe->openPieceFile(own[r - 200]);
                        else if (r >= 100 && r < 200)
                            safe->whenPieceSafe(
                                [safe, r]
                                {
                                    if (safe == nullptr)
                                        return;
                                    safe->proc.loadFactoryPiece(r - 100);
                                    safe->pieceLoaded();
                                });
                        juce::ignoreUnused(P);
                    });
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
    if (autoPipeTarget(paramId) >= 0 || autoPathStop(paramId) >= 0 || paramId == kAutoTempoId)
    {
        pushUndo();
        AutoLane l;
        l.paramId = paramId;
        if (paramId == kAutoTempoId)
        {
            // the tempo as it is now, at the start (the own clock follows the lane from here on)
            l.pts = {{0.0, autoTempoNorm(s.tempo)}};
            s.clock = 1;
        }
        else if (autoPipeTarget(paramId) >= 0)
        {
            // a gesture that spreads from in front of the listener: nothing at first, a swell
            // and back (0.5 = no change)
            l.spread = true;
            l.pts = {{0.0, 0.5f}, {2.0, 0.85f}, {4.0, 0.5f}};
        }
        else
        {
            // a timetable: one pass along the path in 16 beats
            l.pts = {{0.0, 0.0f}, {16.0, 1.0f}};
        }
        s.lanes.push_back(std::move(l));
        selLane = (int)s.lanes.size() - 1;
        selPoint = 0;
        changed();
        timeline.scrollY =
            juce::jmax(0, timeline.contentHeight() - (timeline.getHeight() - VerlaufTimeline::kRulerH));
        return;
    }
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
    menu.addSectionHeader(rysum::tr("Uhr"));
    menu.addItem(7000, rysum::tr("Tempo der eigenen Uhr"), true, tempoLane() != nullptr);
    menu.addSectionHeader(rysum::tr("Im Raum"));
    {
        // per-pipe targets: the gesture spreads through the room from an origin
        juce::PopupMenu pipes;
        const auto names = pipeTargetNames();
        for (int t = 0; t < kNumPipeTargets; ++t)
        {
            bool has = false;
            for (auto &l : set().lanes)
                has = has || l.paramId == pipeTargetId(t);
            pipes.addItem(5000 + t, names[t].fromFirstOccurrenceOf(rysum::tr("Pfeife: "), false, false), true, has);
        }
        menu.addSubMenu(rysum::tr("Je Pfeife \xe2\x80\x93 breitet sich im Raum aus"), pipes);
        // timed paths
        juce::PopupMenu paths;
        for (int k = 0; k < kNumStops; ++k)
        {
            const int st = kDisplayOrder[(size_t)k];
            bool has = false;
            for (auto &l : set().lanes)
                has = has || l.paramId == autoPathLaneId(st);
            paths.addItem(6000 + st, rysum::tr(kStops[(size_t)st].label), true, has);
        }
        menu.addSubMenu(rysum::tr("Fahrplan einer gezeichneten Bahn"), paths);
    }
    menu.addSectionHeader(rysum::tr("Regler"));
    {
        const auto pm = paramMenu(base);
        for (juce::PopupMenu::MenuItemIterator it(pm); it.next();)
            menu.addItem(it.getItem());
    }
    if ((int)set().lanes.size() >= kMaxAutoLanes)
    {
        menu.clear();
        menu.addItem(-1, rysum::tr("Alle 16 Spuren belegt"), false);
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addButton),
                       [safe = juce::Component::SafePointer<VerlaufView>(this), base](int r)
                       {
                           if (safe == nullptr || r < base)
                               return;
                           if (r == 7000)
                               safe->addLaneFor(kAutoTempoId);
                           else if (r >= 6000 && r < 6000 + kNumStops)
                               safe->addLaneFor(autoPathLaneId(r - 6000));
                           else if (r >= 5000 && r < 5000 + kNumPipeTargets)
                               safe->addLaneFor(pipeTargetId(r - 5000));
                           else if (auto *prm = safe->proc.getParamAt(r - base))
                               safe->addLaneFor(prm->getParameterID());
                       });
}

void VerlaufView::showLaneMenu(int lane)
{
    if (!validLane(lane))
        return;
    juce::PopupMenu m;
    const auto &l = set().lanes[(size_t)lane];
    m.addItem(1, rysum::tr("Spur an"), true, l.on);
    m.addItem(2, rysum::tr("Aufnahmebereit (\xe2\x97\x8f)"), true, l.armed);
    m.addItem(3, rysum::tr("Schleife"), true, l.loop);
    m.addItem(4, rysum::tr("R\xc3\xbc"
                                        "ckw\xc3\xa4rts"),
              true, l.reverse);
    m.addSeparator();
    const bool collapsed = l.height > 0 && l.height <= VerlaufTimeline::kMinLaneH;
    m.addItem(9, collapsed ? rysum::tr("Aufklappen") : rysum::tr("Einklappen"));
    m.addItem(10, rysum::tr("H\xc3\xb6he automatisch"), l.height > 0);
    m.addItem(7, rysum::tr("Spur nach oben"), lane > 0);
    m.addItem(8, rysum::tr("Spur nach unten"), lane + 1 < (int)set().lanes.size());
    m.addItem(11, rysum::tr("Solo (nur Solo-Spuren wirken)"), true, l.solo);
    m.addSeparator();
    // editing tools
    {
        const bool several = lane == selLane && selPts.size() >= 2;
        juce::PopupMenu tr;
        tr.addSectionHeader(several ? juce::String((int)selPts.size()) + rysum::tr(" gew\xc3\xa4hlte Punkte")
                                    : rysum::tr("alle Punkte der Spur"));
        tr.addItem(101, rysum::tr("Zeitlich dehnen (\xc3\x97 2)"));
        tr.addItem(102, rysum::tr("Zeitlich stauchen (\xc3\x97 \xc2\xbd)"));
        tr.addItem(104, rysum::tr("Zeitlich umkehren"));
        tr.addItem(103, rysum::tr("Werte spiegeln (oben \xe2\x86\x94 unten)"));
        tr.addItem(105, rysum::tr("Werte zur Mitte (\xc3\x97 \xc2\xbd)"));
        tr.addItem(106, rysum::tr("Werte auseinander (\xc3\x97 1,5)"));
        tr.addItem(107, rysum::tr("Aufs Raster"));
        tr.addItem(108, rysum::tr("Ein wenig daneben (Zufall)"));
        m.addSubMenu(several ? rysum::tr("Auswahl bearbeiten") : rysum::tr("Punkte bearbeiten"),
                     tr, lane == selLane && !l.pts.empty());
        juce::PopupMenu fill;
        fill.addSectionHeader(l.loop ? rysum::tr("in die Schleife") : rysum::tr("in den sichtbaren Bereich"));
        fill.addItem(201, rysum::tr("Welle (eine je Takt)"));
        fill.addItem(202, rysum::tr("Zufallsweg"));
        fill.addItem(203, rysum::tr("Zufallsstufen"));
        fill.addItem(204, rysum::tr("Rampe hinauf"));
        fill.addItem(205, rysum::tr("Rampe hinab"));
        juce::PopupMenu eu;
        const char *eus[]{"3 aus 8", "5 aus 8", "5 aus 16", "7 aus 16", "9 aus 16"};
        for (int i = 0; i < 5; ++i)
            eu.addItem(210 + i, eus[i]);
        fill.addSubMenu(rysum::tr("Euklidischer Rhythmus (je Rasterfeld)"), eu);
        m.addSubMenu(rysum::tr("F\xc3\xbcllen"), fill, lane == selLane);
        m.addSubMenu(rysum::tr("Kopie f\xc3\xbcr einen anderen Regler"), paramMenu(1000),
                     lane == selLane && (int)set().lanes.size() < kMaxAutoLanes);
        bool varies = false;
        for (auto &p : l.pts)
            varies = varies || p.chance < 1.0f || p.every > 1 || p.vary > 0.0f;
        m.addItem(12, rysum::tr("Variationen neu w\xc3\xbcrfeln"), varies);
    }
    m.addSeparator();
    m.addItem(5, rysum::tr("Punkte leeren"), !l.pts.empty());
    m.addItem(6, rysum::tr("Spur l\xc3\xb6schen (Strg+Z holt sie zur\xc3\xbc"
                                        "ck)"));
    m.showMenuAsync(juce::PopupMenu::Options(),
                    [safe = juce::Component::SafePointer<VerlaufView>(this), lane](int r)
                    {
                        if (safe == nullptr || r <= 0 || !safe->validLane(lane))
                            return;
                        // (the tools keep their own undo step)
                        if (r >= 1000)
                        {
                            if (auto *prm = safe->proc.getParamAt(r - 1000))
                                safe->copyLaneTo(prm->getParameterID());
                            return;
                        }
                        if (r >= 200)
                        {
                            safe->fillLane(r >= 210 ? 10 + (r - 210) : r - 200);
                            return;
                        }
                        if (r >= 100)
                        {
                            safe->transformPoints(r - 100);
                            return;
                        }
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
                        case 9:
                            l.height = l.height > 0 && l.height <= VerlaufTimeline::kMinLaneH ? 0 : VerlaufTimeline::kMinLaneH;
                            break;
                        case 10: l.height = 0; break;
                        case 11: l.solo = !l.solo; break;
                        case 12: l.seed = (uint32_t)juce::Random::getSystemRandom().nextInt(1 << 30) + 2u; break;
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
                        safe->selPts.clear();
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
    m.addSectionHeader(rysum::tr("Abschnitt ab diesem Punkt"));
    for (int k = 0; k < kNumAutoShapes; ++k)
        m.addItem(10 + k, autoShapeNames()[k], true, l.pts[(size_t)point].shape == k);
    m.addItem(20, rysum::tr("Biegung aufheben"), std::abs(l.pts[(size_t)point].bend) > 1.0e-4f);
    if (lane == selLane && selPts.size() >= 2)
    {
        juce::PopupMenu tr;
        tr.addItem(101, rysum::tr("Zeitlich dehnen (\xc3\x97 2)"));
        tr.addItem(102, rysum::tr("Zeitlich stauchen (\xc3\x97 \xc2\xbd)"));
        tr.addItem(104, rysum::tr("Zeitlich umkehren"));
        tr.addItem(103, rysum::tr("Werte spiegeln (oben \xe2\x86\x94 unten)"));
        tr.addItem(105, rysum::tr("Werte zur Mitte (\xc3\x97 \xc2\xbd)"));
        tr.addItem(106, rysum::tr("Werte auseinander (\xc3\x97 1,5)"));
        tr.addItem(107, rysum::tr("Aufs Raster"));
        tr.addItem(108, rysum::tr("Ein wenig daneben (Zufall)"));
        m.addSubMenu(juce::String((int)selPts.size()) + rysum::tr(" gew\xc3\xa4hlte Punkte"), tr);
    }
    m.addSeparator();
    m.addItem(1, rysum::tr("Punkt l\xc3\xb6schen"));
    m.showMenuAsync(juce::PopupMenu::Options(),
                    [safe = juce::Component::SafePointer<VerlaufView>(this), lane, point](int r)
                    {
                        if (safe == nullptr || r <= 0 || !safe->validLane(lane))
                            return;
                        if (r >= 100)
                        {
                            safe->transformPoints(r - 100);
                            return;
                        }
                        auto &l = safe->set().lanes[(size_t)lane];
                        if (point >= (int)l.pts.size())
                            return;
                        safe->pushUndo();
                        if (r == 1)
                        {
                            l.pts.erase(l.pts.begin() + point);
                            safe->selPoint = -1;
                            safe->selPts.clear();
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
    {
        const bool pipeLane = selectedIsPipeLane();
        for (juce::Component *c : {(juce::Component *)&spreadButton, (juce::Component *)&inwardButton,
                                   (juce::Component *)&waveRow, (juce::Component *)&originPad,
                                   (juce::Component *)&stopBox})
            c->setVisible(pipeLane);
        const bool tempoLane = lane && s.lanes[(size_t)selLane].paramId == kAutoTempoId;
        perNoteButton.setEnabled(lane && !tempoLane);
        perNoteButton.setToggleState(lane && !tempoLane && s.lanes[(size_t)selLane].perNote,
                                     juce::dontSendNotification);
        if (pipeLane)
        {
            const auto &l = s.lanes[(size_t)selLane];
            stopBox.setSelectedId(l.stop + 2, juce::dontSendNotification);
            spreadButton.setToggleState(l.spread, juce::dontSendNotification);
            inwardButton.setToggleState(l.inward, juce::dontSendNotification);
            waveRow.slider.setValue(l.waveSpeed, juce::dontSendNotification);
            inwardButton.setEnabled(l.spread);
            waveRow.setEnabled(l.spread);
            waveRow.repaint();
            originPad.repaint();
        }
    }
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
            chanceRow.slider.setValue(p.chance, juce::dontSendNotification);
            everyRow.slider.setValue(p.every, juce::dontSendNotification);
            varyRow.slider.setValue(p.vary, juce::dontSendNotification);
            const bool hasNext = selPoint + 1 < (int)l.pts.size();
            chanceRow.setEnabled(hasNext);
            everyRow.setEnabled(hasNext && l.loop);
            varyRow.setEnabled(true);
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
        for (auto *r : {&bendRow, &rateRow, &depthRow, &chanceRow, &everyRow, &varyRow})
            r->setEnabled(false);
    for (auto *r : {&lenRow, &speedRow, &offsetRow, &bendRow, &rateRow, &depthRow, &chanceRow, &everyRow, &varyRow})
        r->repaint();
    cycleButton.setToggleState(s.cycle, juce::dontSendNotification);
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
        // (recording rewrites the points: the indices of a selection no longer mean the same)
        if (timeline.drag == VerlaufTimeline::Drag::none)
            selPts.clear();
        repaint(titleBox); // (the piece's name, changed or not)
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
        if (originPad.isVisible())
            originPad.repaint();
    }
    {
        // lanes "je Anschlag neu": their marker (time since the last key) moves on its own clock
        const double since = proc.getAutoSinceKey();
        if (std::abs(since - shownSinceKey) > 1.0e-9 &&
            std::any_of(set().lanes.begin(), set().lanes.end(), [](const AutoLane &l) { return l.perNote; }))
        {
            shownSinceKey = since;
            timeline.repaint();
        }
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
    // the piece on the page (and whether it has changed since it was saved), then the clock
    juce::String right = proc.getPieceName().isNotEmpty() ? proc.getPieceName()
                                                          : rysum::tr("Unbenanntes St\xc3\xbc"
                                                                                   "ck");
    if (proc.isPieceChanged() && !s.lanes.empty())
        right << rysum::tr(" (ge\xc3\xa4ndert)");
    right << juce::String::fromUTF8("  \xc2\xb7  ") << timeText(proc.getAutoNow());
    right << (host ? rysum::tr("  \xc2\xb7  folgt dem Host") : rysum::tr("  \xc2\xb7  eigene Uhr"));
    if (proc.isAutoRecord())
        right << rysum::tr("  \xc2\xb7  Aufnahme");
    drawSectionTitle(g, titleBox, rysum::tr("Verlauf"), right);
    drawSectionTitle(g, laneBox, rysum::tr("Spur"),
                     validLane(selLane) ? laneName(selLane) : rysum::tr("keine gew\xc3\xa4hlt"));
    juce::String segRight;
    if (validLane(selLane))
    {
        const auto &l = s.lanes[(size_t)selLane];
        if (selPts.size() > 1)
            segRight = juce::String((int)selPts.size()) + rysum::tr(" Punkte gew\xc3\xa4hlt \xc2\xb7 "
                                                                                  "Form gilt ab Punkt ") +
                       juce::String(selPoint + 1);
        else if (selPoint >= 0 && selPoint + 1 < (int)l.pts.size())
            segRight = rysum::tr("von Punkt ") + juce::String(selPoint + 1) + rysum::tr(" bis ") +
                       juce::String(selPoint + 2) + "  (" + timeText(beatOfLocal(l, l.pts[(size_t)selPoint].t)) +
                       ")";
        else if (selPoint >= 0 && selPoint < (int)l.pts.size())
            segRight = rysum::tr("nach dem letzten Punkt \xe2\x80\x93 der Wert bleibt");
        else
            segRight = rysum::tr("auf eine Linie klicken");
    }
    drawSectionTitle(g, segBox, rysum::tr("Abschnitt"), segRight);
    // a frame around the timeline
    g.setColour(col::border);
    g.drawRect(timeline.getBounds().expanded(1), 1);
    // the thin lines between the groups of the bar
    g.setColour(col::dim.withAlpha(0.55f));
    for (int x : barSeps)
        g.fillRect((float)x, (float)onButton.getY() + 3.0f, 1.0f, (float)onButton.getHeight() - 6.0f);
    // the hint line: what the mouse can do where it is
    if (!hintBox.isEmpty())
    {
        g.setColour(col::faint);
        g.setFont(A.monoFont(10.0f));
        g.drawText(timeline.hintText(), hintBox, juce::Justification::centredLeft, true);
    }
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
    // in groups, a thin line between them: on | the clock | recording | drawing | ... | view | edit
    barSeps.clear();
    auto sepL = [&]
    {
        barSeps.add(bar.getX() + 6);
        bar.removeFromLeft(13);
    };
    auto sepR = [&]
    {
        barSeps.add(bar.getRight() - 7);
        bar.removeFromRight(13);
    };
    place(pieceButton, 64, 6);
    place(onButton, 74, 0);
    sepL();
    place(clockHost, 54, 2);
    place(clockOwn, 70, 6);
    place(playButton, 28, 2);
    place(stopButton, 28, 2);
    place(homeButton, 28, 4);
    place(cycleButton, 38, 6);
    place(keyStart, 88, 6);
    tempoRow.setBounds(bar.removeFromLeft(92).withTrimmedTop(-4));
    sepL();
    place(recButton, 86, 0);
    sepL();
    place(toolPts, 54, 2);
    place(toolPen, 44, 2);
    place(toolStair, 54, 6);
    place(gridBox, 84, 0);
    // right end: view (zoom), then edit (undo, add)
    addButton.setBounds(bar.removeFromRight(66));
    bar.removeFromRight(6);
    undoButton.setBounds(bar.removeFromRight(60));
    sepR();
    zoomAll.setBounds(bar.removeFromRight(44));
    bar.removeFromRight(2);
    zoomIn.setBounds(bar.removeFromRight(26));
    bar.removeFromRight(2);
    zoomOut.setBounds(bar.removeFromRight(26));

    r.removeFromTop(10);
    auto bottom = r.removeFromBottom(152);
    r.removeFromBottom(8);
    hintBox = r.removeFromBottom(16).withTrimmedLeft(2);
    r.removeFromBottom(4);
    timeline.setBounds(r);

    const int gap = 24;
    laneBox = bottom.removeFromLeft((bottom.getWidth() - gap) / 2);
    bottom.removeFromLeft(gap);
    segBox = bottom;
    {
        auto b = laneBox.withTrimmedTop(38);
        // the origin plan on the right (per-pipe lanes)
        originPad.setBounds(b.removeFromRight(b.getHeight()).reduced(2));
        b.removeFromRight(16);
        {
            auto row3 = b.withTrimmedTop(34 + 8 + 24 + 10);
            row3 = row3.withHeight(34);
            stopBox.setBounds(row3.removeFromLeft(132).withTrimmedTop(10));
            row3.removeFromLeft(8);
            spreadButton.setBounds(row3.removeFromLeft(88).withTrimmedTop(10));
            row3.removeFromLeft(6);
            inwardButton.setBounds(row3.removeFromLeft(80).withTrimmedTop(10));
            row3.removeFromLeft(16);
            waveRow.setBounds(row3);
        }
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
        row2.removeFromLeft(6);
        perNoteButton.setBounds(row2.removeFromLeft(130));
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
        // variation from pass to pass
        b.removeFromTop(6);
        auto row3 = b.removeFromTop(34);
        chanceRow.setBounds(row3.removeFromLeft(w));
        row3.removeFromLeft(18);
        everyRow.setBounds(row3.removeFromLeft(w));
        row3.removeFromLeft(18);
        varyRow.setBounds(row3);
    }
}
bool VerlaufView::selectedIsPipeLane() const
{
    return validLane(selLane) && autoPipeTarget(proc.getAuto().lanes[(size_t)selLane].paramId) >= 0;
}

// ---------------------------------------------------------------------------
// the origin of a spreading lane
// ---------------------------------------------------------------------------
juce::Point<float> OriginPad::toPad(float x, float y) const
{
    const auto c = getLocalBounds().toFloat().getCentre();
    const float k = (float)juce::jmin(getWidth(), getHeight()) * 0.5f / kRange;
    return {c.x + x * k, c.y - y * k};
}

juce::Point<float> OriginPad::fromPad(juce::Point<float> p) const
{
    const auto c = getLocalBounds().toFloat().getCentre();
    const float k = (float)juce::jmin(getWidth(), getHeight()) * 0.5f / kRange;
    return {(p.x - c.x) / k, (c.y - p.y) / k};
}

void OriginPad::paint(juce::Graphics &g)
{
    if (!owner.selectedIsPipeLane())
        return;
    auto &A = Assets::get();
    auto &P = owner.proc;
    const auto &l = P.getAuto().lanes[(size_t)owner.selLane];
    const auto b = getLocalBounds().toFloat();
    const auto c = b.getCentre();
    const float R = juce::jmin(b.getWidth(), b.getHeight()) * 0.5f;
    g.setColour(col::panel);
    g.fillEllipse(b.withSizeKeepingCentre(2 * R, 2 * R));
    // rings every 5 m
    g.setColour(col::line);
    for (int m = 5; m <= (int)kRange; m += 5)
    {
        const float r = R * (float)m / kRange;
        g.drawEllipse(juce::Rectangle<float>(2 * r, 2 * r).withCentre(c), m == (int)kRange ? 1.0f : 0.6f);
    }
    g.setColour(col::border);
    g.drawEllipse(b.withSizeKeepingCentre(2 * R, 2 * R), 1.0f);
    // the pipes of the pulled stops
    const auto params = P.readParams();
    g.setColour(col::dim.withAlpha(0.55f));
    for (int s = 0; s < kNumStops; ++s)
    {
        if (!params.stopOn[(size_t)s])
            continue;
        for (int k = 0; k < kNumKeys; ++k)
        {
            const auto pos = P.engine.displayPosition(s, k, params);
            float x = 0, y = 0, z = 0;
            autoPosToXyz(pos.az, pos.el, pos.dist, x, y, z);
            const auto q = toPad(x, y);
            if (b.contains(q))
                g.fillRect(juce::Rectangle<float>(2.0f, 2.0f).withCentre(q));
        }
    }
    // the listener (front up)
    {
        juce::Path tri;
        tri.addTriangle(c.x, c.y - 4.5f, c.x - 3.5f, c.y + 3.0f, c.x + 3.5f, c.y + 3.0f);
        g.setColour(col::ink);
        g.fillPath(tri);
    }
    if (l.spread)
    {
        // the fronts: one ring per point of the curve that the clock has passed (each change
        // runs through the room)
        const auto o = toPad(l.ox, l.oy);
        const double now = P.getAutoNow();
        const float k = R / kRange;
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(juce::Rectangle<float>(2 * R, 2 * R).withCentre(c).toNearestInt());
        for (const auto &pt : l.pts)
        {
            const double since = now - VerlaufView::beatOfLocal(l, pt.t);
            if (since < 0.0)
                continue;
            float run = (float)since * l.waveSpeed;
            float rad = l.inward ? kAutoInwardRadius - run : run;
            if (rad < 0.0f || rad > kRange * 2.2f || run > kAutoInwardRadius * 2.0f)
                continue;
            const float fade = juce::jlimit(0.15f, 1.0f, 1.0f - run / (kRange * 2.2f));
            g.setColour(col::rubric.withAlpha(0.8f * fade));
            g.drawEllipse(juce::Rectangle<float>(2 * rad * k, 2 * rad * k).withCentre(o), 1.2f);
        }
        g.setColour(col::rubric.brighter(0.3f));
        drawLozenge(g, o, 4.5f, 6.0f);
    }
    else
    {
        g.setColour(col::faint);
        g.setFont(A.monoFont(9.5f));
        g.drawFittedText(rysum::tr("alle Pfeifen\nzugleich"), getLocalBounds().reduced(8),
                         juce::Justification::centredBottom, 2);
    }
}

void OriginPad::mouseDown(const juce::MouseEvent &e)
{
    if (!owner.selectedIsPipeLane() || !owner.set().lanes[(size_t)owner.selLane].spread)
        return;
    owner.pushUndo();
    mouseDrag(e);
}

void OriginPad::mouseDrag(const juce::MouseEvent &e)
{
    if (!owner.selectedIsPipeLane())
        return;
    auto &l = owner.set().lanes[(size_t)owner.selLane];
    if (!l.spread)
        return;
    auto q = fromPad(e.position);
    if (!e.mods.isShiftDown())
        q = {std::round(q.x * 2.0f) * 0.5f, std::round(q.y * 2.0f) * 0.5f}; // half metres
    l.ox = juce::jlimit(-kRange, kRange, q.x);
    l.oy = juce::jlimit(-kRange, kRange, q.y);
    owner.changed();
    repaint();
}

void OriginPad::mouseDoubleClick(const juce::MouseEvent &)
{
    if (!owner.selectedIsPipeLane())
        return;
    auto &l = owner.set().lanes[(size_t)owner.selLane];
    l.ox = l.oy = l.oz = 0.0f; // at the listener
    owner.changed();
    repaint();
}
} // namespace rysum::ui
