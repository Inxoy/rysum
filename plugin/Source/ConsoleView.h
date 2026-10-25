#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
/** one stop knob (photo) – toggles the stop parameter */
class StopTile : public juce::Button
{
public:
    StopTile (int stopIndex) : juce::Button (prettyLabel (kStops[(size_t) stopIndex].label)), stop (stopIndex)
    {
        setClickingTogglesState (true);
        setTooltip (prettyLabel (kStops[(size_t) stopIndex].label));
    }
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    int stop;
};

/** the 46 semitones C..a'' with the short-octave gaps; shows held keys, can be played with the mouse */
class KeyStrip : public juce::Component
{
public:
    explicit KeyStrip (RysumProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    int midiAt (juce::Point<int>) const;
    void play (int midi);
    RysumProcessor& proc;
    int playing = -1;
};

class ConsoleView : public juce::Component
{
public:
    explicit ConsoleView (RysumProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();                 // called by the editor's timer

private:
    RysumProcessor& proc;
    std::vector<std::unique_ptr<StopTile>> tiles;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> tileAttachments;
    std::vector<std::unique_ptr<ParamRow>> noiseRows, decayRows;
    juce::TextButton perPipe { "pro Pfeife" }, onSum { "Summe" };
    juce::Slider volume;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volumeAttachment;
    juce::TextButton folderButton { juce::String (juce::CharPointer_UTF8 ("Samples-Ordner \xe2\x80\xa6")) };
    std::unique_ptr<juce::FileChooser> chooser;
    KeyStrip keys;
    juce::Rectangle<int> stopsArea, noiseArea, decayArea, outArea, keyboardArea, meterArea, photoArea;
    float meterL = 0, meterR = 0;
};
} // namespace rysum::ui
