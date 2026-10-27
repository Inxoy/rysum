#pragma once

#include "PluginProcessor.h"
#include "Theme.h"
#include "ConsoleView.h"
#include "SpaceView.h"
#include "KlangView.h"
#include "ModView.h"
#include "EffectsView.h"
#include "SaalView.h"

/** fixed 1120 x 720 design, scaled as a whole when the window is resized */
class RysumContent : public juce::Component, public rysum::ui::ModUiAccess
{
public:
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;
    void releaseComputerKeys();

public:
    void showModMenu (juce::Component& target, const juce::String& paramId) override;
    int paramIndexFor (const juce::String& paramId) override { return proc.getParamIndex (paramId); }
    float modulatedNorm (int index) override { return proc.getModulatedNorm (index); }
    int routesFor (int paramIndex, int* out, int maxCount) override
    {
        int n = 0;
        for (int r = 0; r < rysum::kNumRoutes && n < maxCount; ++r)
            if (proc.getRouteSrc (r) >= 0 && proc.getRouteTgt (r) == rysum::kGlobalTargetBase + paramIndex) out[n++] = r;
        return n;
    }
    int routeSource (int r) override { return proc.getRouteSrc (r); }
    bool routeUni (int r) override { return proc.getRouteUni (r); }
    juce::RangedAudioParameter* routeAmountParam (int r) override { return proc.apvts.getParameter (proc.getModAmountId (r)); }
    juce::String routeLabel (int r) override { const int s = proc.getRouteSrc (r); return s >= 0 ? rysum::modSourceNames()[s] : juce::String(); }
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
    rysum::ui::EffectsView fx;
    rysum::ui::SaalView saal;
    std::array<juce::TextButton, 6> tabs;
    juce::TextButton keysButton { "Tastatur" }, latchButton { "Halten" };
    std::array<int, 18> keyNoteDown {};                 // MIDI note each computer key is playing (-1 = up)
    int keysVelocity = 100;
    void updatePlayButtons();
    juce::ComboBox outputChoice;
    juce::TextButton patchPrev { juce::String::fromUTF8 ("\xe2\x97\x80") }, patchNext { juce::String::fromUTF8 ("\xe2\x96\xb6") },
                     patchName, patchSave { "Speichern" };
    int shownPatchVersion = -1;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> outputAttachment;
    int current = 0;
    juce::Image grainLayer, vignette;
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
    juce::SharedResourcePointer<rysum::ui::LookAndFeel> lnf;   // also the default look (context menus, dialogs)
    RysumContent content;
    juce::TooltipWindow tooltips { this, 600 };
};
