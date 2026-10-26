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
        knobs[0]  = img (k_praestant_jpg, k_praestant_jpgSize);   // Praestant: a lever on the organ – photo of its pipes
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
        setColour (juce::ComboBox::backgroundColourId, col::bg);
        setColour (juce::ComboBox::textColourId, col::ink);
        setColour (juce::ComboBox::arrowColourId, col::dim);
        setColour (juce::PopupMenu::backgroundColourId, col::panel);
        setColour (juce::PopupMenu::textColourId, col::ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, col::ink);
        setColour (juce::PopupMenu::highlightedTextColourId, col::bg);
        setColour (juce::TooltipWindow::backgroundColourId, col::panel);
        setColour (juce::TooltipWindow::textColourId, col::ink);
        setColour (juce::AlertWindow::backgroundColourId, col::panel);
        setColour (juce::AlertWindow::textColourId, col::ink);
        setColour (juce::AlertWindow::outlineColourId, col::border);
        setColour (juce::TextEditor::backgroundColourId, col::bg);
        setColour (juce::TextEditor::textColourId, col::ink);
        setColour (juce::TextEditor::outlineColourId, col::border);
        setColour (juce::TextEditor::focusedOutlineColourId, col::dim);
        setColour (juce::TextEditor::highlightColourId, col::faint);
        setColour (juce::CaretComponent::caretColourId, col::ink);
        setColour (juce::ScrollBar::thumbColourId, col::faint);
        setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
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

    juce::Font getComboBoxFont (juce::ComboBox&) override { return Assets::get().monoFont (12.0f); }
    juce::Font getPopupMenuFont() override { return Assets::get().monoFont (13.0f); }

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
        if (s.getProperties().contains ("bipolar"))
        {
            const float mid = (float) x + (float) w * 0.5f;
            g.fillRect (juce::jmin (mid, pos), cy - 1.0f, std::abs (pos - mid), 2.0f);
            g.setColour (col::faint);
            g.fillRect (mid - 0.5f, cy - 4.0f, 1.0f, 8.0f);
        }
        else g.fillRect ((float) x, cy - 1.0f, pos - (float) x, 2.0f);
        g.setColour (s.isMouseOverOrDragging() ? col::white : col::ink);
        g.fillRect (pos - 1.5f, cy - 7.0f, 3.0f, 14.0f);
    }

    int getSliderThumbRadius (juce::Slider&) override { return 2; }
};

/** implemented by the editor content: right-click modulation menu + live modulation values for the sliders */
struct ModUiAccess
{
    virtual ~ModUiAccess() = default;
    virtual void showModMenu (juce::Component& target, const juce::String& paramId) = 0;
    virtual int paramIndexFor (const juce::String& paramId) = 0;
    virtual float modulatedNorm (int paramIndex) = 0;       // NaN = not modulated
};

/** slider that hands right-clicks to a callback instead of dragging */
class RowSlider : public juce::Slider
{
public:
    std::function<void()> onRightClick;
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }
        juce::Slider::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Slider::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override   { if (! e.mods.isPopupMenu()) juce::Slider::mouseUp (e); }
};

/** label + value + thin slider, attached to a parameter. Right-click = modulation menu; a modulated
    parameter shows where the modulation takes it right now. */
class ParamRow : public juce::Component, private juce::Timer
{
public:
    ParamRow (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& name, bool compactRow = false)
        : title (name), compact (compactRow), id (paramId)
    {
        slider.onRightClick = [this] { openMenu(); };
        startTimerHz (25);
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
        auto r = getLocalBounds().removeFromTop (compact ? 13 : 18);
        g.setColour (compact ? col::mute : col::ink);
        g.setFont (compact ? Assets::get().monoFont (10.0f) : Assets::get().sansFont (14.0f));
        g.drawText (title, r, juce::Justification::centredLeft);
        g.setColour (col::dim);
        g.setFont (Assets::get().monoFont (compact ? 10.0f : 12.0f));
        g.drawText (param != nullptr ? param->getCurrentValueAsText() : juce::String(), r, juce::Justification::centredRight);
    }

    void resized() override
    {
        if (compact) slider.setBounds (getLocalBounds().withTrimmedTop (11).withHeight (14));
        else         slider.setBounds (getLocalBounds().withTrimmedTop (16).withHeight (18));
    }

    void mouseDown (const juce::MouseEvent& e) override { if (e.mods.isPopupMenu()) openMenu(); }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (findAccess()) modNorm = access->modulatedNorm (index);
        if (std::isnan (modNorm) || param == nullptr) return;
        const auto sb = slider.getBounds().toFloat();
        const float x0 = (float) slider.getX() + (float) slider.getPositionOfValue (slider.getValue());
        const float x1 = (float) slider.getX() + (float) slider.getPositionOfValue (param->convertFrom0to1 (modNorm));
        const float cy = sb.getCentreY();
        g.setColour (col::white.withAlpha (0.55f));
        g.fillRect (juce::jmin (x0, x1), cy + 4.0f, std::abs (x1 - x0), 2.0f);
        juce::Path tri;
        tri.addTriangle (x1 - 4.0f, cy + 10.0f, x1 + 4.0f, cy + 10.0f, x1, cy + 4.0f);
        g.setColour (col::white);
        g.fillPath (tri);
    }

    RowSlider slider;
    const juce::String& getParamId() const { return id; }

private:
    void openMenu()
    {
        if (auto* a = findParentComponentOfClass<ModUiAccess>()) a->showModMenu (*this, id);
    }

    bool findAccess()
    {
        if (access == nullptr)
        {
            access = findParentComponentOfClass<ModUiAccess>();
            if (access != nullptr) index = access->paramIndexFor (id);
        }
        return access != nullptr;
    }

    void timerCallback() override
    {
        if (! isShowing() || ! findAccess()) return;
        const float n = access->modulatedNorm (index);
        const bool was = ! std::isnan (modNorm), is = ! std::isnan (n);
        if (was != is || (is && std::abs (n - modNorm) > 0.0005f)) { modNorm = n; repaint(); }
    }

    juce::String title;
    bool compact = false;
    juce::String id;
    ModUiAccess* access = nullptr;
    int index = -1;
    float modNorm = std::numeric_limits<float>::quiet_NaN();
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
