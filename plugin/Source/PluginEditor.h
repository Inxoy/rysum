#pragma once

#include "PluginProcessor.h"
#include "Theme.h"
#include "ConsoleView.h"
#include "SpaceView.h"
#include "KlangView.h"
#include "ModView.h"

/** fixed 1120 x 720 design, scaled as a whole when the window is resized */
class RysumContent : public juce::Component, public rysum::ui::ModUiAccess
{
public:
    void showModMenu (juce::Component& target, const juce::String& paramId) override;
    int paramIndexFor (const juce::String& paramId) override { return proc.getParamIndex (paramId); }
    float modulatedNorm (int index) override { return proc.getModulatedNorm (index); }
    explicit RysumContent (RysumProcessor&);
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void showView (int v);
    void tick();
    void showPatchMenu();
    void stepPatch (int delta);
    void askSavePatch();

private:
    RysumProcessor& proc;
    rysum::ui::ConsoleView console;
    rysum::ui::SpaceView space;
    rysum::ui::KlangView klang;
    rysum::ui::ModView mod;
    std::array<juce::TextButton, 4> tabs;
    juce::ComboBox outputChoice;
    juce::TextButton patchPrev { juce::String::fromUTF8 ("\xe2\x97\x80") }, patchNext { juce::String::fromUTF8 ("\xe2\x96\xb6") },
                     patchName, patchSave { "Speichern" };
    int shownPatchVersion = -1;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> outputAttachment;
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
