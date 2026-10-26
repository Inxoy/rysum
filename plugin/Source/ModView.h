#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
class ModView;

/** the scrollable list of routings */
class MatrixList : public juce::Component
{
public:
    explicit MatrixList (ModView& o) : owner (o) {}
    void paint (juce::Graphics&) override;
private:
    ModView& owner;
};

/** fourth page: 4 LFOs, 2 envelopes, 24 routings (scrollable) */
class ModView : public juce::Component
{
public:
    explicit ModView (RysumProcessor&);
    ~ModView() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

    /** fills a combo with all modulation targets (pipe targets + parameters, grouped in sub-menus) */
    static void fillTargetMenu (juce::PopupMenu& menu, RysumProcessor& proc, int idOffset);

private:
    friend class MatrixList;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;

    struct Lfo
    {
        juce::ComboBox shape, div, spreadMode;
        juce::TextButton sync { "Sync" };
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
        juce::TextButton clear { juce::String::fromUTF8 ("\xc3\x97") };
        juce::Rectangle<int> row;
    };

    void syncRoutes();
    void routeEdited (int r);
    float paramValue (const juce::String& id) const;

    RysumProcessor& proc;
    std::array<Lfo, kNumLfos> lfos;
    std::array<Env, kNumEnvs> envs;
    std::array<Route, kNumRoutes> routes;
    juce::Rectangle<int> matrixBox;
    MatrixList list { *this };
    juce::Viewport matrixView;
    static constexpr int kRowH = 24;
    int lastRouteVersion = -1;
    bool updating = false;
};
} // namespace rysum::ui
