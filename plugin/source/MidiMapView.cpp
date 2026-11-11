#include "MidiMapView.h"

namespace rysum::ui
{
namespace
{
juce::String paramGroup(const juce::String &id)
{
    if (id.startsWith("stop") || id.startsWith("vol") || id.startsWith("swind") ||
        id.startsWith("rate") || id.startsWith("depth") || id.startsWith("ellipse") ||
        id.startsWith("motion"))
        return id == "motionSync" ? juce::String("Klang") : juce::String("Register");
    if (id.startsWith("deg") || id.startsWith("spc"))
        return "Verfall";
    if (id.startsWith("cloud") || id.startsWith("pipe"))
        return "Register";
    if (id.startsWith("clickRes"))
        return juce::String::fromUTF8("Traktur \xc2\xb7 Ausgang");
    if (id == "click" || id == "clickBlend" || id == "releaseClick" || id == "stopPull" ||
        id == "wind" || id == "master")
        return juce::String::fromUTF8("Traktur \xc2\xb7 Ausgang");
    if (id.startsWith("ir"))
        return "Hall";
    if (id.startsWith("fx"))
        return "Effekte";
    if (id.startsWith("sim"))
        return "Saal";
    if (id.startsWith("bin"))
        return juce::String::fromUTF8("Kopfh\xc3\xb6rer");
    if (id.startsWith("lfo") || id.startsWith("env") || id.startsWith("mod"))
        return "Modulation";
    return "Klang";
}
} // namespace

// ---------------------------------------------------------------------------
void RangeBar::paint(juce::Graphics &g)
{
    const float cy = (float)getHeight() * 0.5f;
    g.setColour(col::track);
    g.fillRect(6.0f, cy - 0.5f, (float)getWidth() - 12.0f, 1.0f);
    g.setColour(col::faint);
    g.fillRect(6.0f, cy - 3.0f, 1.0f, 6.0f);
    g.fillRect((float)getWidth() - 7.0f, cy - 3.0f, 1.0f, 6.0f);
    const float a = xOf(lo), b = xOf(hi);
    g.setColour(isEnabled() ? col::ink.withAlpha(0.85f) : col::faint);
    g.fillRect(juce::jmin(a, b), cy - 1.0f, juce::jmax(1.0f, std::abs(b - a)), 2.0f);
    for (int i = 0; i < 2; ++i)
    {
        const float x = i == 0 ? a : b;
        g.setColour(!isEnabled() ? col::faint
                                 : (dragging == i || isMouseOver() ? col::white : col::ink));
        drawLozenge(g, {x, cy}, 4.0f, 6.0f);
    }
    // an inverted range (top -> bottom) is marked with a small arrow
    if (hi < lo)
    {
        g.setColour(col::rubric.brighter(0.3f));
        drawLozenge(g, {(a + b) * 0.5f, cy}, 2.0f, 2.0f);
    }
}

void RangeBar::mouseDown(const juce::MouseEvent &e)
{
    if (!isEnabled())
        return;
    dragging = std::abs(e.position.x - xOf(lo)) <= std::abs(e.position.x - xOf(hi)) ? 0 : 1;
    mouseDrag(e);
}

void RangeBar::mouseDrag(const juce::MouseEvent &e)
{
    if (dragging < 0)
        return;
    float v = valueAt(e.position.x);
    if (e.mods.isShiftDown())
        v = std::round(v * 20.0f) / 20.0f;
    (dragging == 0 ? lo : hi) = v;
    if (onChange)
        onChange(lo, hi);
    repaint();
}

// ---------------------------------------------------------------------------
void MidiMapPanel::fillParamMenu(juce::PopupMenu &menu, RysumProcessor &proc)
{
    const juce::StringArray groups{"Register",
                                   juce::String::fromUTF8("Traktur \xc2\xb7 Ausgang"),
                                   "Klang",
                                   "Verfall",
                                   "Hall",
                                   "Saal",
                                   "Effekte",
                                   "Modulation",
                                   juce::String::fromUTF8("Kopfh\xc3\xb6rer")};
    for (auto &grp : groups)
    {
        juce::PopupMenu sub;
        for (int i = 0; i < proc.getNumParams(); ++i)
        {
            const auto id = proc.getParamAt(i)->getParameterID();
            if (id == "output" || id == "midiCC")
                continue; // the output format belongs to the track; the switch for the fixed CCs
                          // stays manual
            if (paramGroup(id) == grp)
                sub.addItem(1 + i, proc.getParamAt(i)->getName(64));
        }
        if (sub.getNumItems() > 0)
            menu.addSubMenu(grp, sub);
    }
}

MidiMapPanel::MidiMapPanel(RysumProcessor &p) : proc(p)
{
    for (int i = 0; i < RysumProcessor::kNumMidiMaps; ++i)
    {
        auto &R = rows[(size_t)i];
        R.learn.setTooltip(juce::String::fromUTF8(
            "Klicken, dann den Regler am Keyboard bewegen (nochmal klicken: abbrechen)"));
        R.learn.onClick = [this, i]
        {
            proc.learnMidi(proc.getLearningSlot() == i ? -1 : i);
            sync();
        };
        fillParamMenu(*R.target.getRootMenu(), proc);
        R.target.setTextWhenNothingSelected("Ziel");
        R.target.onChange = [this, i]
        {
            if (updating)
                return;
            auto m = proc.getMidiMap(i);
            const bool fresh = m.param < 0;
            m.param = rows[(size_t)i].target.getSelectedId() - 1;
            if (fresh)
            {
                // buttons for switches; a new row starts listening right away
                m.toggle =
                    dynamic_cast<juce::AudioParameterBool *>(proc.getParamAt(m.param)) != nullptr;
                m.lo = 0.0f;
                m.hi = 1.0f;
            }
            proc.setMidiMap(i, m);
            if (fresh && m.cc < 0)
                proc.learnMidi(i);
            sync();
        };
        R.range.onChange = [this, i](float lo, float hi)
        {
            auto m = proc.getMidiMap(i);
            m.lo = lo;
            m.hi = hi;
            proc.setMidiMap(i, m);
            list.repaint(rows[(size_t)i].area);
        };
        R.range.setTooltip(
            juce::String::fromUTF8("Bereich, den der Regler \xc3\xbc"
                                   "berstreicht. Linke Raute = Regler unten, rechte = Regler oben "
                                   "(vertauschen kehrt die Richtung um). Doppelklick: ganzer "
                                   "Bereich, Shift: 5-%-Schritte."));
        R.toggle.setClickingTogglesState(false);
        R.toggle.setTooltip(
            juce::String::fromUTF8("Taster: jeder Druck schaltet zwischen den beiden Enden des "
                                   "Bereichs um (f\xc3\xbcr Kn\xc3\xb6pfe / Pads). "
                                   "Aus: der Regler stellt den Wert direkt ein."));
        R.toggle.onClick = [this, i]
        {
            auto m = proc.getMidiMap(i);
            m.toggle = !m.toggle;
            proc.setMidiMap(i, m);
            sync();
        };
        R.clear.setTooltip("Zuordnung entfernen");
        R.clear.onClick = [this, i]
        {
            proc.clearMidiMap(i);
            sync();
        };
        for (juce::Component *c : {(juce::Component *)&R.learn, (juce::Component *)&R.target,
                                   (juce::Component *)&R.range, (juce::Component *)&R.toggle,
                                   (juce::Component *)&R.clear})
            list.addAndMakeVisible(c);
    }
    view.setViewedComponent(&list, false);
    view.setScrollBarsShown(true, false);
    view.setScrollBarThickness(6);
    view.setSingleStepSizes(kRowH, kRowH);
    addAndMakeVisible(view);
    sync();
}

void MidiMapPanel::resized()
{
    auto r = getLocalBounds();
    header = r.removeFromTop(16);
    view.setBounds(r);
    list.setSize(r.getWidth() - 10, kRowH * RysumProcessor::kNumMidiMaps);
    for (int i = 0; i < RysumProcessor::kNumMidiMaps; ++i)
    {
        auto &R = rows[(size_t)i];
        R.area = {0, i * kRowH, list.getWidth(), kRowH};
        auto top = R.area.withHeight(24).reduced(0, 1);
        top.removeFromLeft(22);
        R.learn.setBounds(top.removeFromLeft(96));
        top.removeFromLeft(8);
        R.clear.setBounds(top.removeFromRight(22));
        top.removeFromRight(6);
        R.toggle.setBounds(top.removeFromRight(58));
        top.removeFromRight(8);
        R.target.setBounds(top);
        auto bottom = R.area.withTrimmedTop(25).withHeight(16);
        bottom.removeFromLeft(22);
        R.loText = bottom.removeFromLeft(96);
        bottom.removeFromLeft(8);
        bottom.removeFromRight(28);
        R.hiText = bottom.removeFromRight(90);
        bottom.removeFromRight(6);
        R.range.setBounds(bottom);
    }
}

juce::String MidiMapPanel::rangeText(int slot, float norm) const
{
    const auto m = proc.getMidiMap(slot);
    if (auto *prm = proc.getParamAt(m.param))
        return prm->getText(norm, 24);
    return {};
}

void MidiMapPanel::sync()
{
    updating = true;
    const int learning = proc.getLearningSlot();
    for (int i = 0; i < RysumProcessor::kNumMidiMaps; ++i)
    {
        auto &R = rows[(size_t)i];
        const auto m = proc.getMidiMap(i);
        juce::String l;
        if (learning == i)
            l = juce::String::fromUTF8("bewegen \xe2\x80\xa6");
        else if (m.cc >= 0)
            l = "CC " + juce::String(m.cc) +
                (m.channel > 0 ? juce::String::fromUTF8(" \xc2\xb7 K") + juce::String(m.channel)
                               : juce::String());
        else
            l = "lernen";
        R.learn.setButtonText(l);
        R.learn.setToggleState(learning == i, juce::dontSendNotification);
        R.target.setSelectedId(m.param >= 0 ? m.param + 1 : 0, juce::dontSendNotification);
        R.toggle.setToggleState(m.toggle, juce::dontSendNotification);
        if (!R.range.isDragging())
            R.range.setRange(m.lo, m.hi);
        R.range.setEnabled(m.param >= 0);
        R.toggle.setEnabled(m.param >= 0);
        R.clear.setEnabled(m.used() || learning == i);
    }
    updating = false;
    lastVersion = proc.midiMapVersion.load();
    list.repaint();
}

void MidiMapPanel::reveal(int slot)
{
    if (slot < 0)
        return;
    sync();
    const int y = slot * kRowH;
    if (y < view.getViewPositionY() || y + kRowH > view.getViewPositionY() + view.getViewHeight())
        view.setViewPosition(0, juce::jmax(0, y - kRowH));
}

void MidiMapPanel::refresh()
{
    if (proc.midiMapVersion.load() != lastVersion)
        sync();
    const int last = proc.getLastController();
    if (last != lastCcShown)
    {
        lastCcShown = last;
        repaint(header);
    }
}

void MidiMapPanel::paint(juce::Graphics &g)
{
    auto &A = Assets::get();
    g.setFont(A.monoFont(10.0f));
    g.setColour(col::faint);
    g.drawText("Regler", header.withTrimmedLeft(22).withWidth(96),
               juce::Justification::centredLeft);
    g.drawText("Ziel", header.withTrimmedLeft(126).withWidth(120),
               juce::Justification::centredLeft);
    const int last = proc.getLastController();
    juce::String t;
    if (last < 0)
        t = "noch kein Regler empfangen";
    else
        t = juce::String::fromUTF8("zuletzt: CC ") + juce::String(last & 0xff) +
            juce::String::fromUTF8(" \xc2\xb7 Kanal ") + juce::String((last >> 8) & 0xff) +
            juce::String::fromUTF8(" \xc2\xb7 Wert ") + juce::String((last >> 16) & 0xff);
    g.setColour(last < 0 ? col::faint : col::dim);
    g.drawText(t, header.withTrimmedRight(14), juce::Justification::centredRight);
}

void MidiMapList::paint(juce::Graphics &g)
{
    auto &A = Assets::get();
    auto &proc = owner.proc;
    for (int i = 0; i < RysumProcessor::kNumMidiMaps; ++i)
    {
        auto &R = owner.rows[(size_t)i];
        const auto m = proc.getMidiMap(i);
        g.setColour(i % 2 == 0 ? col::panel.withAlpha(0.0f) : col::panel.withAlpha(0.5f));
        g.fillRect(R.area);
        g.setFont(A.monoFont(10.0f));
        g.setColour(m.used() ? col::mute : col::faint.withAlpha(0.6f));
        g.drawText(juce::String(i + 1), R.area.withHeight(24).withWidth(18),
                   juce::Justification::centredRight);
        if (m.param < 0)
            continue;
        g.setFont(A.monoFont(10.5f));
        g.setColour(col::dim);
        g.drawText(owner.rangeText(i, m.lo), R.loText, juce::Justification::centredRight);
        g.drawText(owner.rangeText(i, m.hi), R.hiText, juce::Justification::centredLeft);
    }
}
} // namespace rysum::ui
