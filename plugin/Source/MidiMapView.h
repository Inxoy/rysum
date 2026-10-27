#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
/** a range lo..hi (normalised 0..1) with two lozenge handles; double-click = whole range */
class RangeBar : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void (float lo, float hi)> onChange;
    void setRange (float l, float h) { if (! juce::approximatelyEqual (l, lo) || ! juce::approximatelyEqual (h, hi)) { lo = l; hi = h; repaint(); } }
    float getLo() const { return lo; }
    float getHi() const { return hi; }
    bool isDragging() const { return dragging >= 0; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragging = -1; repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { lo = 0.0f; hi = 1.0f; if (onChange) onChange (lo, hi); repaint(); }

private:
    float xOf (float v) const { return 6.0f + v * ((float) getWidth() - 12.0f); }
    float valueAt (float x) const { return juce::jlimit (0.0f, 1.0f, (x - 6.0f) / juce::jmax (1.0f, (float) getWidth() - 12.0f)); }
    float lo = 0.0f, hi = 1.0f;
    int dragging = -1;          // 0 = lo, 1 = hi
};

class MidiMapPanel;

/** the scrollable list of controller assignments */
class MidiMapList : public juce::Component
{
public:
    explicit MidiMapList (MidiMapPanel& o) : owner (o) {}
    void paint (juce::Graphics&) override;
private:
    MidiMapPanel& owner;
};

/** "MIDI-Regler" on the modulation page: knobs, faders and buttons of a MIDI controller -> any parameter */
class MidiMapPanel : public juce::Component
{
public:
    explicit MidiMapPanel (RysumProcessor&);
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh();
    /** scrolls a slot into view (after "MIDI lernen" on a slider) */
    void reveal (int slot);

    static constexpr int kRowH = 44;
    static void fillParamMenu (juce::PopupMenu& menu, RysumProcessor& proc);

private:
    friend class MidiMapList;
    struct Row
    {
        juce::TextButton learn, toggle { "Taster" }, clear { juce::String::fromUTF8 ("\xc3\x97") };
        juce::ComboBox target;
        RangeBar range;
        juce::Rectangle<int> area, loText, hiText;
    };
    void sync();
    void edited (int slot);
    juce::String rangeText (int slot, float norm) const;

    RysumProcessor& proc;
    std::array<Row, RysumProcessor::kNumMidiMaps> rows;
    MidiMapList list { *this };
    juce::Viewport view;
    juce::Rectangle<int> header;
    int lastVersion = -1, lastCcShown = -2;
    bool updating = false;
};
} // namespace rysum::ui
