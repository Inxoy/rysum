#pragma once

#include "MidiMapView.h"
#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
class ModView;

/** the scrollable list of routings */
class MatrixList : public juce::Component
{
public:
    explicit MatrixList(ModView &o) : owner(o) {}
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override; // click an amount to type it
private:
    ModView &owner;
    InlineValueEditor editor;
};

/** modulation page: 4 LFOs, 2 envelopes, 24 routings (scrollable) / MIDI controller assignments */
class ModView : public juce::Component
{
public:
    explicit ModView(RysumProcessor &);
    ~ModView() override;
    void paint(juce::Graphics &) override;
    void resized() override;
    void refresh();

    /** the list at the bottom right: modulation routings or MIDI controller assignments */
    void showMidiTab(bool midi, int revealSlot = -1);

    /** fills a combo with all modulation targets (pipe targets + parameters, grouped in sub-menus)
     */
    static void fillTargetMenu(juce::PopupMenu &menu, RysumProcessor &proc, int idOffset);

private:
    friend class MatrixList;
    using ComboAtt = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;

    struct Lfo
    {
        juce::ComboBox shape, div, spreadMode;
        juce::TextButton sync{"Sync"};
        std::unique_ptr<ComboAtt> shapeAtt, divAtt, spreadModeAtt;
        std::unique_ptr<ButtonAtt> syncAtt;
        std::unique_ptr<ParamRow> rate, spread;
        juce::Rectangle<int> box, scope;
    };
    struct Env
    {
        juce::ComboBox mode;
        std::unique_ptr<ComboAtt> modeAtt;
        std::unique_ptr<ParamRow> delay, attack, decay, sustain, release;
        juce::Rectangle<int> box, scope;
    };
    struct Route
    {
        juce::ComboBox src, tgt;
        RowSlider amount;
        std::unique_ptr<SliderAtt> amountAtt;
        juce::TextButton clear{juce::String::fromUTF8("\xc3\x97")};
        juce::TextButton uni{juce::String::fromUTF8("\xc2\xb1")};
        juce::Rectangle<int> row;
    };

    void syncRoutes();
    void routeEdited(int r);
    float paramValue(const juce::String &id) const;

    RysumProcessor &proc;
    std::array<Lfo, kNumLfos> lfos;
    std::array<Env, kNumEnvs> envs;
    std::array<Route, kNumRoutes> routes;
    juce::Rectangle<int> matrixBox;
    MatrixList list{*this};
    juce::Viewport matrixView;
    static constexpr int kRowH = 24;
    int lastRouteVersion = -1;
    bool updating = false;
    juce::TextButton tabMod{"Modulation"}, tabMidi{"MIDI-Regler"};
    MidiMapPanel midiPanel{proc};
    bool midiTab = false;
};
} // namespace rysum::ui
