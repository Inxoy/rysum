#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
/** floor plan of the simulated room: walls, pipes, listener (draggable), mirror images and
    reflection paths of the last struck pipe */
class SaalPlan : public juce::Component
{
public:
    explicit SaalPlan (RysumProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    struct Frame { float scale = 10.0f; juce::Point<float> origin; };          // room metres -> screen
    Frame frame() const;
    juce::Point<float> toScreen (const Frame& f, float x, float y) const { return { f.origin.x + x * f.scale, f.origin.y - y * f.scale }; }
    void setListener (juce::Point<float> screen);

    RysumProcessor& proc;
    bool dragging = false;
};

/** sixth page: the simulated, playable room */
class SaalView : public juce::Component
{
public:
    explicit SaalView (RysumProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    ParamRow& row (const char* id, const juce::String& name);
    void setParams (std::initializer_list<std::pair<const char*, float>> values);

    RysumProcessor& proc;
    SaalPlan plan { proc };
    juce::TextButton onButton { "Saal an" };
    std::unique_ptr<ButtonAtt> onAtt;
    juce::ComboBox order;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> orderAtt;
    std::vector<std::unique_ptr<juce::TextButton>> roomPresets, materials;
    std::vector<std::unique_ptr<ParamRow>> rows;
    std::array<ParamRow*, 4> dims {}, surf {}, levels {};
    juce::Rectangle<int> saalBox, dimBox, surfBox, levelBox, planBox;
    juce::String shownStatus;
    // late reverb model: convolution / FDN
    juce::TextButton tailConv { "Faltung" }, tailFdn { "FDN" };
    std::unique_ptr<juce::ParameterAttachment> tailAtt;
    // Lucier mode
    juce::TextButton lucOn { "Lucier an" }, lucRestart { juce::String::fromUTF8 ("Wachsen neu beginnen") };
    std::unique_ptr<ButtonAtt> lucOnAtt;
    juce::ComboBox lucSource;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> lucSourceAtt;
    std::array<ParamRow*, 4> lucRows {};
    juce::Rectangle<int> lucBox, lucGenArea, lucCurveArea, lucPeakArea, lucNoteArea;
    int shownLucVersion = -1;
    float shownLucGen = -1.0f, shownLucLevel = -1.0f;
    void paintLucier (juce::Graphics&);
};
} // namespace rysum::ui
