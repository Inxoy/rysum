#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
/** Klang page: tuning, tremulant + wind model, freeze, distance/Doppler, MIDI + tempo, headphones
 */
class KlangView : public juce::Component
{
public:
    std::function<void()> onOpenSpeakers; // the editor shows the page "Lautsprecher"
    explicit KlangView(RysumProcessor &);
    void paint(juce::Graphics &) override;
    void resized() override;
    void refresh();

private:
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    ParamRow &row(const char *id, const juce::String &name);
    juce::TextButton &toggle(const char *id, const juce::String &text);

    RysumProcessor &proc;
    std::vector<std::unique_ptr<ParamRow>> rows;
    std::vector<std::unique_ptr<juce::TextButton>> toggles;
    std::vector<std::unique_ptr<ButtonAtt>> toggleAtts;
    juce::TextButton speakersButton{juce::String::fromUTF8("Lautsprecher (AllRAD) \xe2\x80\xa6")};
    juce::ComboBox temperament, binOrder;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> binOrderAtt;
    juce::TextButton sofaLoad{juce::String::fromUTF8("SOFA laden \xe2\x80\xa6")},
        sofaReset{"FABIAN"}, binOn{"Binaural an"};
    std::unique_ptr<juce::FileChooser> chooser;
    ParamRow *yaw = nullptr;
    juce::TextButton *dfEq = nullptr;
    juce::Rectangle<int> binStatusArea;
    juce::String shownBinStatus;
    std::unique_ptr<juce::ParameterAttachment> temperamentAtt;

    ParamRow *pitch = nullptr, *detune = nullptr, *tremRate = nullptr, *tremDepth = nullptr,
             *smear = nullptr, *release = nullptr, *doppler = nullptr, *distAmount = nullptr,
             *air = nullptr, *airInvert = nullptr;
    juce::Label scalaReference;
    juce::TextButton *tremOn = nullptr, *freezeOn = nullptr, *midiOn = nullptr, *syncOn = nullptr,
                     *mpeOn = nullptr;
    ParamRow *mpeBend = nullptr;
    ParamRow *dsfFrequency = nullptr, *dsfSpectrum = nullptr, *dsfLevel = nullptr,
             *dsfOffset = nullptr, *dsfPartials = nullptr, *dsfFootage = nullptr,
             *dsfMixtur = nullptr;
    juce::TextButton *dsfOn = nullptr, *dsfMidi = nullptr;
    juce::ComboBox dsfRegister;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> dsfRegisterAtt;
    juce::Rectangle<int> dsfBox;
    int midiTextTop = 118;
    juce::Rectangle<int> tuneBox, tremBox, freezeBox, distBox, midiBox, binBox;
    // wind model of the stops
    juce::TextButton windClassic{"klassisch"}, windUnstable{"instabil"};
    std::unique_ptr<juce::ParameterAttachment> windAtt;
    juce::Rectangle<int> windBox, windText;
    // free tuning: Scala files, MTS-ESP
    juce::TextButton scalaLoad{juce::String::fromUTF8("Scala laden \xe2\x80\xa6")},
        kbmClear{"KBM entfernen"};
    juce::TextButton *mtsFollow = nullptr;
    juce::TextButton loadPresetTuning{"Stimmung mit Preset laden"};
    juce::TextButton extendedRange{"Erweiterter Tonumfang"};
    juce::Rectangle<int> tuningStatusArea;
    juce::String shownTuningStatus;
    juce::TextButton midiMapButton{juce::String::fromUTF8("Eigene Zuordnungen \xe2\x80\xa6")};
    // two loudspeakers (transaural): below Freeze
    juce::TextButton trOn{"Transaural an"};
    juce::TextButton *trEq = nullptr;
    ParamRow *trAngle = nullptr, *trAmount = nullptr, *trBoost = nullptr;
    juce::Rectangle<int> trBox, trStatusArea;
    juce::String shownTrStatus;
};
} // namespace rysum::ui
