#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
/** the page about the organ itself (opened with the "Rysum" in the top bar): its history, the stops,
    photos. The text is a resource (Resources/orgel.txt) with a tiny markup:
      @middle / @right   column       # title      > subtitle       ## section
      - item             | stop | note (table)     ~ small text     [photos]   anything else: a paragraph */
class OrganView : public juce::Component
{
public:
    OrganView();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::StringArray lines;
    juce::Rectangle<int> photoCol, middleCol, rightCol;
};
} // namespace rysum::ui
