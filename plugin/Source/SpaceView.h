#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
/** top-down view of the sphere with every pipe as a dot */
class SpaceCircle : public juce::Component
{
public:
    SpaceCircle (RysumProcessor& p, int& selectedStop) : proc (p), selected (selectedStop) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    std::function<void()> onUserMovedPipes;

private:
    juce::Point<float> toScreen (Pos p) const;
    Pos fromScreen (juce::Point<float> s) const;
    float radius() const { return (float) juce::jmin (getWidth(), getHeight()) * 0.5f - 34.0f; }
    juce::Point<float> centre() const { return getLocalBounds().toFloat().getCentre(); }

    RysumProcessor& proc;
    int& selected;
    int dragKey = -1;
    bool rotating = false;
    float rotateStartAngle = 0.0f;
    std::array<Pos, kNumKeys> rotateStart {};
};

/** small square that pulls / pushes a stop (same look as on the Spieltisch) */
class PullToggle : public juce::Button
{
public:
    PullToggle() : juce::Button ("ziehen")
    {
        setClickingTogglesState (true);
        setTooltip (juce::String::fromUTF8 ("Register ziehen / absto\xc3\x9f" "en"));
    }
    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto sq = juce::Rectangle<float> (9.0f, 9.0f).withCentre (getLocalBounds().toFloat().getCentre());
        g.setColour (getToggleState() ? col::ink : (over ? col::dim : col::faint));
        if (getToggleState()) g.fillRect (sq); else g.drawRect (sq, 1.0f);
    }
};

/** a row of the stop list: dot, name, current motion */
class StopRow : public juce::Button
{
public:
    StopRow (int s) : juce::Button (prettyLabel (kStops[(size_t) s].label)), stop (s) {}
    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto& A = Assets::get();
        auto r = getLocalBounds();
        const bool sel = getToggleState();
        if (sel) { g.setColour (col::ink); g.fillRect (r.withTrimmedLeft (18)); }
        else if (over) { g.setColour (col::line); g.fillRect (r.withTrimmedLeft (18)); }
        drawStopDot (g, stop, { 5.0f, (float) r.getCentreY() }, 8.0f, false);
        g.setColour (sel ? col::bg : (pulled ? col::ink : col::faint));
        g.setFont (A.sansFont (14.0f));
        g.drawText (getButtonText(), r.withTrimmedLeft (28), juce::Justification::centredLeft);
        g.setColour (sel ? col::bg : col::mute);
        g.setFont (A.monoFont (11.0f));
        g.drawText (motion, r.withTrimmedRight (8), juce::Justification::centredRight);
    }
    juce::String motion;
    bool pulled = true;
private:
    int stop;
};

class SpaceView : public juce::Component
{
public:
    explicit SpaceView (RysumProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    void selectStop (int s);
    void layoutChanged();
    void syncLayoutControls();

    RysumProcessor& proc;
    int selected = 6;
    SpaceCircle circle;
    std::vector<std::unique_ptr<StopRow>> stopButtons;
    std::vector<std::unique_ptr<PullToggle>> pullButtons;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> pullAttachments;
    std::array<juce::TextButton, 6> layoutButtons;
    std::array<juce::TextButton, 6> motionButtons;
    juce::Slider width, centre, elevation, spread, distance;
    std::unique_ptr<ParamRow> rateRow, depthRow, ellipseRow, irDry, irWet;
    juce::TextButton irLoad { juce::String::fromUTF8 ("Impulsantwort laden \xe2\x80\xa6") }, irClear { "x" }, irFuma { "FuMa" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> irFumaAttachment;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> left, right, stopsBox, layoutBox, motionBox, hallBox;
    bool updating = false;
};
} // namespace rysum::ui
