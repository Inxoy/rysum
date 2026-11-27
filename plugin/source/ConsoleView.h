#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
/** one stop knob (photo) – toggles the stop parameter */
class StopTile : public juce::Button
{
public:
    StopTile(int stopIndex)
        : juce::Button(prettyLabel(kStops[(size_t)stopIndex].label)), stop(stopIndex)
    {
        setClickingTogglesState(true);
        setTooltip(prettyLabel(kStops[(size_t)stopIndex].label));
    }
    void paintButton(juce::Graphics &, bool over, bool down) override;

private:
    int stop;
};

/** the Vox humana's knob: the neural stop (no photo - a typeset label on the bare knob) */
class VoxTile : public juce::Button
{
public:
    VoxTile() : juce::Button("Vox humana")
    {
        setClickingTogglesState(true);
        setTooltip(rysum::tr("Vox humana 8\xe2\x80\xb2 \xe2\x80\x93 ein neuronales Register (NeuralRysum): ein Netz, das die "
            "Rysumer Orgel spielen gelernt hat, synthetisiert jeden Ton. Spielt mit den Tasten und, mit "
            "\xe2\x80\x9e" "Eingang spielt die Orgel\xe2\x80\x9c, mit dem, was am Eingang erklingt."));
    }
    void paintButton(juce::Graphics &, bool over, bool down) override;
    juce::String error; // the model could not be loaded
};

/** the Vox humana's settings (opens from the console) */
class VoxPanel : public juce::Component
{
public:
    explicit VoxPanel(RysumProcessor &);
    void paint(juce::Graphics &) override;
    void resized() override;

private:
    RysumProcessor &proc;
    juce::ToggleButton plays{"Eingang spielt die Orgel"}, follow{"Klangfarbe folgt dem Eingang"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> playsAtt, followAtt;
    std::vector<std::unique_ptr<ParamRow>> rows;
    std::vector<juce::Rectangle<int>> heads;
};

/** the 46 semitones C..a'' with the short-octave gaps; shows held keys, can be played with the
 * mouse */
class KeyStrip : public juce::Component
{
public:
    explicit KeyStrip (RysumProcessor& p) : proc (p) {}
    /** repaints only when the keys shown down changed */
    void refresh()
    {
        uint64_t held = proc.engine.heldMask.load();
        if (const int nk = proc.engine.netKeyNow.load(); nk >= 0) held |= (uint64_t) 1 << nk;
        if (held != shownHeld) { shownHeld = held; repaint(); }
    }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    int midiAt(juce::Point<int>) const;
    void play(int midi);
    RysumProcessor &proc;
    int playing = -1;
    uint64_t shownHeld = ~(uint64_t) 0;
};

/** a scene slot A..D: click = go there (an empty one stores the sound), right-click = store / clear
 */
class SceneButton : public juce::Button
{
public:
    SceneButton(RysumProcessor &p, int s)
        : juce::Button(juce::String::charToString((juce::juce_wchar)('A' + s))), proc(p), slot(s)
    {
    }
    void paintButton(juce::Graphics &, bool over, bool down) override;
    using juce::Button::clicked;
    void clicked(const juce::ModifierKeys &) override;
    void mouseDown(const juce::MouseEvent &) override;
    bool current = false;

private:
    void showMenu();
    RysumProcessor &proc;
    int slot;
};

class ConsoleView : public juce::Component
{
public:
    explicit ConsoleView(RysumProcessor &);
    void paint(juce::Graphics &) override;
    void resized() override;
    void refresh(); // called by the editor's timer

private:
    RysumProcessor &proc;
    std::vector<std::unique_ptr<StopTile>> tiles;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>>
        tileAttachments;
    std::vector<std::unique_ptr<ParamRow>> noiseRows, decayRows, stopVol, stopWind;
    // Vox humana
    VoxTile voxTile;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> voxAtt;
    std::unique_ptr<ParamRow> voxVol, voxWind;
    juce::TextButton voxMore{juce::String::fromUTF8("Vox \xe2\x80\xa6")};
    juce::TextButton testToneButton{"Testton"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> testToneAttachment;
    std::unique_ptr<ParamRow> testToneLevel, testToneColour;
    juce::TextButton perPipe{"pro Pfeife"}, onSum{"Summe"};
    // Verfall: of the sound ("Klang") or of the spatial image ("Raum")
    juce::TextButton tabSound { "Klang" }, tabSpace { "Raum" }, tabGlitch { "Glitch" };
    std::vector<std::unique_ptr<ParamRow>> spaceRows, glitchRows;
    bool spaceTab = false;
    int decayTab = 0;                          // 0 Klang, 1 Raum, 2 Glitch
    void showDecayTab (int tab);
    // Traktur: the noises, or the resonated clicks
    juce::TextButton tabNoise { juce::String::fromUTF8 ("Ger\xc3\xa4usche") }, tabRes { "Resonanz" };
    std::vector<std::unique_ptr<ParamRow>> resRows;
    bool resTab = false;
    void showNoiseTab (bool res);
    // the third tab: the stop arpeggio
    juce::TextButton tabArp { "Arpeggio" };
    juce::ComboBox arpMode, arpRate, arpOrder;
    juce::Label arpModeLabel, arpRateLabel, arpOrderLabel, arpNote;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> arpModeAtt, arpRateAtt, arpOrderAtt;
    int actionTab = 0;     // 0 noises, 1 resonance, 2 arpeggio
    void showActionTab (int tab);
    juce::String shownSpaceInfo;
    juce::String spaceInfo() const;
    juce::Slider volume;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volumeAttachment;
    juce::TextButton folderButton{
        juce::String(juce::CharPointer_UTF8("Samples-Ordner \xe2\x80\xa6"))};
    std::unique_ptr<juce::FileChooser> chooser;
    KeyStrip keys;
    // scenes
    std::vector<std::unique_ptr<SceneButton>> sceneButtons;
    std::unique_ptr<ParamRow> sceneMorph;
    juce::Rectangle<int> scenesArea, outInfoArea;
    int shownSceneVersion = -1;
    float shownMorph = -1.0f;
    juce::Rectangle<int> stopsArea, noiseArea, decayArea, outArea, keyboardArea, meterArea,
        photoArea;
    juce::ComboBox spreadChoice; // notes to single pipes
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> spreadAtt;
    juce::Rectangle<int> spreadLabel;
    float meterL = 0, meterR = 0;
};
} // namespace rysum::ui
