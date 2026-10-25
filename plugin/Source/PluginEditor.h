#pragma once

#include "PluginProcessor.h"
#include "Theme.h"
#include "ConsoleView.h"
#include "SpaceView.h"

/** fixed 1120 x 720 design, scaled as a whole when the window is resized */
class RysumContent : public juce::Component
{
public:
    explicit RysumContent (RysumProcessor&);
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void showView (int v);
    void tick();

private:
    RysumProcessor& proc;
    rysum::ui::ConsoleView console;
    rysum::ui::SpaceView space;
    std::array<juce::TextButton, 2> tabs;
    int current = 0;
    juce::Image grainLayer;
};

class RysumEditor : public juce::AudioProcessorEditor,
                    private juce::Timer
{
public:
    explicit RysumEditor (RysumProcessor&);
    ~RysumEditor() override;
    void paint (juce::Graphics& g) override { g.fillAll (rysum::ui::col::bg); }
    void resized() override;

private:
    void timerCallback() override;
    RysumProcessor& proc;
    juce::SharedResourcePointer<rysum::ui::Assets> assets;     // fonts + images, alive while an editor exists
    rysum::ui::LookAndFeel lnf;
    RysumContent content;
    juce::TooltipWindow tooltips { this, 600 };
};
