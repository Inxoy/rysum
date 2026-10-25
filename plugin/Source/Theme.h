#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "RysumBinaryData.h"
#include "Common.h"

namespace rysum::ui
{
namespace col
{
    const juce::Colour bg     { 0xff0d0d0c };
    const juce::Colour ink    { 0xffe9e7e2 };
    const juce::Colour white  { 0xffffffff };
    const juce::Colour dim    { 0xffb9b7b2 };
    const juce::Colour mute   { 0xff8a8883 };
    const juce::Colour faint  { 0xff6f6d69 };
    const juce::Colour line   { 0xff2a2a28 };
    const juce::Colour border { 0xff3a3a37 };
    const juce::Colour track  { 0xff34342f };
    const juce::Colour panel  { 0xff141413 };
}

/** fonts and images from BinaryData, created once */
struct Assets
{
    juce::Typeface::Ptr sans, sansBold, mono, fraktur;
    juce::Image grainTile, wall, keys, knobBare;
    std::array<juce::Image, kNumStops> knobs;

    /** the editor keeps one alive via juce::SharedResourcePointer<Assets>; views use get() */
    static Assets& get()
    {
        jassert (instance() != nullptr);
        return *instance();
    }
    static Assets*& instance() { static Assets* p = nullptr; return p; }

    juce::Font sansFont (float h) const    { return juce::Font (juce::FontOptions (sans).withHeight (h)); }
    juce::Font boldFont (float h) const    { return juce::Font (juce::FontOptions (sansBold).withHeight (h)); }
    juce::Font monoFont (float h) const    { return juce::Font (juce::FontOptions (mono).withHeight (h)); }
    juce::Font frakturFont (float h) const { return juce::Font (juce::FontOptions (fraktur).withHeight (h)); }

    ~Assets() { instance() = nullptr; }

    Assets()
    {
        instance() = this;
        using namespace RysumData;
        sans     = juce::Typeface::createSystemTypefaceFor (IBMPlexSansCondensedRegular_ttf, IBMPlexSansCondensedRegular_ttfSize);
        sansBold = juce::Typeface::createSystemTypefaceFor (IBMPlexSansCondensedSemiBold_ttf, IBMPlexSansCondensedSemiBold_ttfSize);
        mono     = juce::Typeface::createSystemTypefaceFor (IBMPlexMonoRegular_ttf, IBMPlexMonoRegular_ttfSize);
        fraktur  = juce::Typeface::createSystemTypefaceFor (UnifrakturMaguntiaBook_ttf, UnifrakturMaguntiaBook_ttfSize);
        auto img = [] (const void* d, int n) { return juce::ImageCache::getFromMemory (d, n); };
        grainTile = img (grain_png, grain_pngSize);
        wall      = img (wall_jpg, wall_jpgSize);
        keys      = img (keys_jpg, keys_jpgSize);
        knobBare  = img (knob_bare_jpg, knob_bare_jpgSize);
        knobs[0]  = {};                                           // Praestant: no photo of the label
        knobs[1]  = img (k_gedackt_jpg, k_gedackt_jpgSize);
        knobs[2]  = img (k_oktave4_jpg, k_oktave4_jpgSize);
        knobs[3]  = img (k_oktave2_jpg, k_oktave2_jpgSize);
        knobs[4]  = img (k_sesquialtera_jpg, k_sesquialtera_jpgSize);
        knobs[5]  = img (k_mixtur_jpg, k_mixtur_jpgSize);
        knobs[6]  = img (k_trompete_jpg, k_trompete_jpgSize);
    }
};

/** draws an image like CSS object-fit: cover */
inline void drawCover (juce::Graphics& g, const juce::Image& img, juce::Rectangle<float> r, float yAlign = 0.5f)
{
    if (! img.isValid()) return;
    const float s = juce::jmax (r.getWidth() / (float) img.getWidth(), r.getHeight() / (float) img.getHeight());
    const float w = (float) img.getWidth() * s, h = (float) img.getHeight() * s;
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (r.toNearestInt());
    g.drawImage (img, juce::Rectangle<float> (r.getX() + (r.getWidth() - w) * 0.5f, r.getY() + (r.getHeight() - h) * yAlign, w, h),
                 juce::RectanglePlacement::stretchToFit);
}

/** tiles the grain texture over an area */
inline void drawGrain (juce::Graphics& g, juce::Rectangle<int> r, float opacity)
{
    auto& t = Assets::get().grainTile;
    if (! t.isValid()) return;
    g.setTiledImageFill (t, 0, 0, opacity);
    g.fillRect (r);
}

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, col::bg);
        setColour (juce::Label::textColourId, col::ink);
        setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        setColour (juce::TextButton::buttonOnColourId, col::ink);
        setColour (juce::TextButton::textColourOffId, col::dim);
        setColour (juce::TextButton::textColourOnId, col::bg);
        setColour (juce::ComboBox::outlineColourId, col::border);
        setColour (juce::PopupMenu::backgroundColourId, col::panel);
        setColour (juce::PopupMenu::textColourId, col::ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, col::ink);
        setColour (juce::PopupMenu::highlightedTextColourId, col::bg);
        setColour (juce::TooltipWindow::backgroundColourId, col::panel);
        setColour (juce::TooltipWindow::textColourId, col::ink);
        setDefaultSansSerifTypeface (Assets::get().sans);
    }


    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        if (b.getToggleState())
        {
            g.setColour (col::ink);
            g.fillRect (r);
        }
        else
        {
            g.setColour (over || down ? col::faint : col::border);
            g.drawRect (r, 1.0f);
        }
    }

    juce::Font getTextButtonFont (juce::TextButton&, int h) override { return Assets::get().monoFont (juce::jmin (12.0f, (float) h * 0.55f)); }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        g.setFont (getTextButtonFont (b, b.getHeight()));
        g.setColour (b.getToggleState() ? col::bg : col::dim);
        g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (4, 0), juce::Justification::centred, 1);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style == juce::Slider::LinearVertical)
        {
            const float cx = (float) x + (float) w * 0.5f;
            g.setColour (col::track);
            g.fillRect (cx - 1.0f, (float) y, 2.0f, (float) h);
            g.setColour (s.isMouseOverOrDragging() ? col::white : col::ink);
            g.fillRect ((float) x, pos - 2.0f, (float) w, 4.0f);
            return;
        }
        const float cy = (float) y + (float) h * 0.5f;
        g.setColour (col::track);
        g.fillRect ((float) x, cy - 1.0f, (float) w, 2.0f);
        g.setColour (col::ink);
        g.fillRect ((float) x, cy - 1.0f, pos - (float) x, 2.0f);
        g.setColour (s.isMouseOverOrDragging() ? col::white : col::ink);
        g.fillRect (pos - 1.5f, cy - 7.0f, 3.0f, 14.0f);
    }

    int getSliderThumbRadius (juce::Slider&) override { return 2; }
};

/** label + value + thin slider, attached to a parameter */
class ParamRow : public juce::Component
{
public:
    ParamRow (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& name)
        : title (name)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setDoubleClickReturnValue (true, (double) state.getParameterRange (paramId).convertFrom0to1 (
                                                    state.getParameter (paramId)->getDefaultValue()));
        addAndMakeVisible (slider);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, paramId, slider);
        param = state.getParameter (paramId);
        slider.onValueChange = [this] { repaint(); };
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().removeFromTop (18);
        g.setColour (col::ink);
        g.setFont (Assets::get().sansFont (14.0f));
        g.drawText (title, r, juce::Justification::centredLeft);
        g.setColour (col::dim);
        g.setFont (Assets::get().monoFont (12.0f));
        g.drawText (param != nullptr ? param->getCurrentValueAsText() : juce::String(), r, juce::Justification::centredRight);
    }

    void resized() override { slider.setBounds (getLocalBounds().withTrimmedTop (16).withHeight (18)); }

    juce::Slider slider;

private:
    juce::String title;
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

inline void drawSectionTitle (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, const juce::String& right = {})
{
    g.setColour (col::ink);
    g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
    g.setFont (Assets::get().boldFont (15.0f).withExtraKerningFactor (0.16f));
    g.drawText (title.toUpperCase(), r.withTrimmedTop (8).withHeight (20), juce::Justification::centredLeft);
    if (right.isNotEmpty())
    {
        g.setColour (col::mute);
        g.setFont (Assets::get().monoFont (11.0f));
        g.drawText (right, r.withTrimmedTop (8).withHeight (20), juce::Justification::centredRight);
    }
}

/** dot style per stop, as in the mockup */
inline void drawStopDot (juce::Graphics& g, int stop, juce::Point<float> c, float size, bool highlight)
{
    const juce::Colour fills[] { col::ink, col::mute, col::ink, col::mute, col::dim, col::dim, col::white };
    const bool hollow = stop == 2 || stop == 3 || stop == 5;
    const bool square = stop >= 4;
    auto r = juce::Rectangle<float> (size, size).withCentre (c);
    g.setColour (highlight ? col::white : fills[stop]);
    if (square) { if (hollow) g.drawRect (r, 1.0f); else g.fillRect (r); }
    else        { if (hollow) g.drawEllipse (r.reduced (0.5f), 1.0f); else g.fillEllipse (r); }
}
} // namespace rysum::ui
