#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
class PipesView : public juce::Component
{
public:
    explicit PipesView (RysumProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void refresh();

private:
    void select (int stop, int key);
    void rebuildWave();
    int keyAt (juce::Point<int>) const;

    RysumProcessor& proc;
    int stop = 0, key = 29;        // a'
    std::array<juce::TextButton, kNumStops> stopButtons;
    juce::TextButton preview { juce::String (juce::CharPointer_UTF8 ("Vorh\xc3\xb6ren")) }, muteButton { "Stumm" };
    juce::Rectangle<int> nav, strip, header, wave, info;
    std::vector<float> envMax, envMin;
    std::shared_ptr<const SampleLibrary> shownLib;
    int lastChange = -1;
};
} // namespace rysum::ui
