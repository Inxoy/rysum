#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
/** fifth page: sounds and effects that are easy to place and follow in space
    (own sounds, tracker path, crackle, wind leaks, upper partials elsewhere, wandering echoes) */
class EffectsView : public juce::Component
{
public:
    explicit EffectsView (RysumProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAtt = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    ParamRow& row (const char* id, const juce::String& name);
    juce::TextButton& toggle (const char* id, const juce::String& text);
    juce::ComboBox& combo (const char* id);
    float value (const char* id) const;
    void drawCompass (juce::Graphics&, juce::Rectangle<int> area, int kind) const;

    RysumProcessor& proc;
    std::vector<std::unique_ptr<ParamRow>> rows;
    std::vector<std::unique_ptr<juce::TextButton>> toggles;
    std::vector<std::unique_ptr<ButtonAtt>> toggleAtts;
    std::vector<std::unique_ptr<juce::ComboBox>> combos;
    std::vector<std::unique_ptr<ComboAtt>> comboAtts;

    juce::TextButton openFolder { juce::String::fromUTF8 ("Ordner \xc3\xb6" "ffnen") }, rescan { "Neu einlesen" };
    juce::ComboBox* custPlace = nullptr;
    juce::ComboBox* echoDiv = nullptr;
    juce::TextButton *pathWait = nullptr, *windRound = nullptr, *echoSync = nullptr;
    std::array<ParamRow*, 6> custRows {}, pathRows {}, crackRows {}, leakRows {}, partRows {}, echoRows {};
    ParamRow* echoTime = nullptr;

    juce::Rectangle<int> custBox, pathBox, crackBox, leakBox, partBox, echoBox;
    juce::Rectangle<int> partCompass, echoCompass, pathNote, crackNote, leakNote, partNote, custNote;
    int shownCustom = -1;
};
} // namespace rysum::ui
