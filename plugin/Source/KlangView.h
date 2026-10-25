#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
/** third page: tuning, tremulant, freeze, distance/Doppler, MIDI + tempo */
class KlangView : public juce::Component
{
public:
    explicit KlangView (RysumProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    ParamRow& row (const char* id, const juce::String& name);
    juce::TextButton& toggle (const char* id, const juce::String& text);

    RysumProcessor& proc;
    std::vector<std::unique_ptr<ParamRow>> rows;
    std::vector<std::unique_ptr<juce::TextButton>> toggles;
    std::vector<std::unique_ptr<ButtonAtt>> toggleAtts;
    juce::ComboBox temperament;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> temperamentAtt;

    ParamRow *pitch = nullptr, *detune = nullptr, *tremRate = nullptr, *tremDepth = nullptr, *smear = nullptr, *release = nullptr,
             *doppler = nullptr, *distAmount = nullptr, *air = nullptr;
    juce::TextButton *tremOn = nullptr, *freezeOn = nullptr, *midiOn = nullptr, *syncOn = nullptr;
    juce::Rectangle<int> tuneBox, tremBox, freezeBox, distBox, midiBox, noteBox;
};
} // namespace rysum::ui
