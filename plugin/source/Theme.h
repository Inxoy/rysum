#pragma once

#include "Common.h"
#include "Lang.h"
#include "Modulation.h"
#include "RysumBinaryData.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace rysum::ui
{
// black and white, like the photos – plus one rubric red for initials
namespace col
{
const juce::Colour bg{0xff0d0d0c};
const juce::Colour ink{0xffe9e7e2};
const juce::Colour white{0xffffffff};
const juce::Colour dim{0xffb9b7b2};
const juce::Colour mute{0xffa3a19b};
const juce::Colour faint{0xff858380};
const juce::Colour line{0xff2a2a28};
const juce::Colour border{0xff3f3f3c};
const juce::Colour track{0xff34342f};
const juce::Colour panel{0xff141413};
const juce::Colour rubric{0xffa8442e}; // the red of rubricated initials
} // namespace col

/** sepia-tones a photo (like an old print), in place */
inline juce::Image toneImage(juce::Image img, float amount = 0.55f)
{
    if (!img.isValid())
        return img;
    img = img.convertedToFormat(juce::Image::ARGB);
    juce::Image::BitmapData bd(img, juce::Image::BitmapData::readWrite);
    for (int y = 0; y < bd.height; ++y)
        for (int x = 0; x < bd.width; ++x)
        {
            const auto c = bd.getPixelColour(x, y);
            const float l = c.getPerceivedBrightness();
            // dark umber -> parchment
            const float r = juce::jlimit(0.0f, 1.0f, 0.08f + l * 0.95f),
                        gg = juce::jlimit(0.0f, 1.0f, 0.06f + l * 0.84f),
                        b = juce::jlimit(0.0f, 1.0f, 0.04f + l * 0.66f);
            const float o = 1.0f - amount;
            bd.setPixelColour(x, y,
                              juce::Colour::fromFloatRGBA(c.getFloatRed() * o + r * amount,
                                                          c.getFloatGreen() * o + gg * amount,
                                                          c.getFloatBlue() * o + b * amount,
                                                          c.getFloatAlpha()));
        }
    return img;
}

/** fonts and images from BinaryData, created once */
struct Assets
{
    juce::Typeface::Ptr sans, sansBold, mono, fraktur, fell, fellItalic;
    juce::Image grainTile, wall, keys, knobBare, orgPipes, jambLeft, jambRight;
    std::array<juce::Image, kNumStops> knobs;

    /** the editor keeps one alive via juce::SharedResourcePointer<Assets>; views use get() */
    static Assets &get()
    {
        jassert(instance() != nullptr);
        return *instance();
    }
    static Assets *&instance()
    {
        static Assets *p = nullptr;
        return p;
    }

    // IM Fell (as "Rysum Fell") everywhere: roman for labels, italic for values and small notes.
    // Fell has a small x-height, so sizes are scaled up a little.
    // sizes are the design sizes; small text is set larger than its nominal size so it stays
    // readable
    static float readable(float h, float k)
    {
        return (h + juce::jmax(0.0f, 14.0f - h) * 0.45f + 0.5f) * k;
    }
    juce::Font sansFont(float h) const
    {
        return juce::Font(juce::FontOptions(fell).withHeight(readable(h, 1.16f)));
    }
    juce::Font boldFont(float h) const
    {
        return juce::Font(juce::FontOptions(fell).withHeight(readable(h, 1.16f)));
    }
    juce::Font monoFont(float h) const
    {
        return juce::Font(juce::FontOptions(fellItalic).withHeight(readable(h, 1.2f)));
    }
    juce::Font romanFont(float h) const
    {
        return juce::Font(juce::FontOptions(fell).withHeight(readable(h, 1.16f)));
    }
    juce::Font frakturFont(float h) const
    {
        return juce::Font(juce::FontOptions(fraktur).withHeight(h));
    }

    ~Assets()
    {
        instance() = nullptr;
    }

    Assets()
    {
        instance() = this;
        using namespace RysumData;
        sans = juce::Typeface::createSystemTypefaceFor(IBMPlexSansCondensedRegular_ttf,
                                                       IBMPlexSansCondensedRegular_ttfSize);
        sansBold = juce::Typeface::createSystemTypefaceFor(IBMPlexSansCondensedSemiBold_ttf,
                                                           IBMPlexSansCondensedSemiBold_ttfSize);
        mono = juce::Typeface::createSystemTypefaceFor(IBMPlexMonoRegular_ttf,
                                                       IBMPlexMonoRegular_ttfSize);
        fraktur = juce::Typeface::createSystemTypefaceFor(UnifrakturMaguntiaBook_ttf,
                                                          UnifrakturMaguntiaBook_ttfSize);
        fell = juce::Typeface::createSystemTypefaceFor(RysumFellRoman_ttf, RysumFellRoman_ttfSize);
        fellItalic =
            juce::Typeface::createSystemTypefaceFor(RysumFellItalic_ttf, RysumFellItalic_ttfSize);
        auto img = [](const void *d, int n) { return juce::ImageCache::getFromMemory(d, n); };
        grainTile = juce::ImageCache::getFromMemory(grain_png, grain_pngSize);
        wall = img(wall_jpg, wall_jpgSize);
        keys = img(keys_jpg, keys_jpgSize);
        knobBare = img(knob_bare_jpg, knob_bare_jpgSize);
        knobs[0] = img(k_praestant_jpg,
                       k_praestant_jpgSize); // Praestant: a wooden lever on the organ, no label
        knobs[1] = img(k_gedackt_jpg, k_gedackt_jpgSize);
        knobs[2] = img(k_oktave4_jpg, k_oktave4_jpgSize);
        knobs[3] = img(k_oktave2_jpg, k_oktave2_jpgSize);
        knobs[4] = img(k_sesquialtera_jpg, k_sesquialtera_jpgSize);
        knobs[5] = img(k_mixtur_jpg, k_mixtur_jpgSize);
        knobs[6] = img(k_trompete_jpg, k_trompete_jpgSize);
        knobs[7] = img(k_praestant16_jpg, k_praestant16_jpgSize);
        orgPipes = img(org_pipes_jpg, org_pipes_jpgSize);
        jambLeft = img(org_jamb_l_jpg, org_jamb_l_jpgSize);
        jambRight = img(org_jamb_r_jpg, org_jamb_r_jpgSize);
    }
};

/** benchmark switches: drawing effects that can be left out, to measure what each one costs (0 =
 * draw all) */
namespace dbg
{
inline int skip = 0;
enum
{
    skipOverlay = 1,
    skipCover = 2,
    skipGrain = 4,
    skipClipPath = 8,
    skipTranslucent = 16
};
} // namespace dbg

/** draws an image like CSS object-fit: cover */
inline void drawCover(juce::Graphics &g, const juce::Image &img, juce::Rectangle<float> r,
                      float yAlign = 0.5f)
{
    if (dbg::skip & dbg::skipCover)
        return;
    if (!img.isValid())
        return;
    const float s =
        juce::jmax(r.getWidth() / (float)img.getWidth(), r.getHeight() / (float)img.getHeight());
    const float w = (float)img.getWidth() * s, h = (float)img.getHeight() * s;
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(r.toNearestInt());
    g.drawImage(img,
                juce::Rectangle<float>(r.getX() + (r.getWidth() - w) * 0.5f,
                                       r.getY() + (r.getHeight() - h) * yAlign, w, h),
                juce::RectanglePlacement::stretchToFit);
}

/** tiles the grain texture over an area */
inline void drawGrain(juce::Graphics &g, juce::Rectangle<int> r, float opacity)
{
    if (dbg::skip & dbg::skipGrain)
        return;
    auto &t = Assets::get().grainTile;
    if (!t.isValid())
        return;
    g.setTiledImageFill(t, 0, 0, opacity);
    g.fillRect(r);
}

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColour(juce::ResizableWindow::backgroundColourId, col::bg);
        setColour(juce::Label::textColourId, col::ink);
        setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        setColour(juce::TextButton::buttonOnColourId, col::ink);
        setColour(juce::TextButton::textColourOffId, col::dim);
        setColour(juce::TextButton::textColourOnId, col::bg);
        setColour(juce::ComboBox::outlineColourId, col::border);
        setColour(juce::ComboBox::backgroundColourId, col::bg);
        setColour(juce::ComboBox::textColourId, col::ink);
        setColour(juce::ComboBox::arrowColourId, col::dim);
        setColour(juce::PopupMenu::backgroundColourId, col::panel);
        setColour(juce::PopupMenu::textColourId, col::ink);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, col::ink);
        setColour(juce::PopupMenu::highlightedTextColourId, col::bg);
        setColour(juce::TooltipWindow::backgroundColourId, col::panel);
        setColour(juce::TooltipWindow::textColourId, col::ink);
        setColour(juce::AlertWindow::backgroundColourId, col::panel);
        setColour(juce::AlertWindow::textColourId, col::ink);
        setColour(juce::AlertWindow::outlineColourId, col::border);
        setColour(juce::TextEditor::backgroundColourId, col::bg);
        setColour(juce::TextEditor::textColourId, col::ink);
        setColour(juce::TextEditor::outlineColourId, col::border);
        setColour(juce::TextEditor::focusedOutlineColourId, col::dim);
        setColour(juce::TextEditor::highlightColourId, col::faint);
        setColour(juce::CaretComponent::caretColourId, col::ink);
        setColour(juce::ScrollBar::thumbColourId, col::faint);
        setColour(juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
        setColour(juce::ScrollBar::thumbColourId, col::faint);
        setDefaultSansSerifTypeface(Assets::get().fell);
    }

    ~LookAndFeel() override
    {
        // The shared object, rather than an individual editor, owns this lifetime.
        if (&juce::LookAndFeel::getDefaultLookAndFeel() == this)
            juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

    void drawButtonBackground(juce::Graphics &g, juce::Button &b, const juce::Colour &, bool over,
                              bool down) override
    {
        // a double rule, like a ruled frame in an old book; pulled = parchment
        auto r = b.getLocalBounds().toFloat().reduced(0.5f);
        if (b.getToggleState())
        {
            g.setColour(col::ink);
            g.fillRect(r);
            g.setColour(col::bg.withAlpha(0.35f));
            g.drawRect(r.reduced(2.0f), 0.6f);
        }
        else
        {
            g.setColour(over || down ? col::dim : col::border);
            g.drawRect(r, 1.0f);
            g.setColour((over || down ? col::dim : col::border).withAlpha(0.45f));
            g.drawRect(r.reduced(2.5f), 0.6f);
        }
    }

    juce::Font getComboBoxFont(juce::ComboBox &) override
    {
        return Assets::get().romanFont(13.0f);
    }
    juce::Font getPopupMenuFont() override
    {
        return Assets::get().romanFont(14.0f);
    }
    juce::Font getTextButtonFont(juce::TextButton &, int h) override
    {
        return Assets::get().romanFont(juce::jmin(13.5f, (float)h * 0.56f));
    }

    void drawComboBox(juce::Graphics &g, int w, int h, bool, int, int, int, int,
                      juce::ComboBox &box) override
    {
        auto r = juce::Rectangle<float>(0, 0, (float)w, (float)h).reduced(0.5f);
        g.setColour(col::bg.withAlpha(0.6f));
        g.fillRect(r);
        g.setColour(box.isMouseOver(true) ? col::dim : col::border);
        g.drawRect(r, 1.0f);
        g.setColour(col::border.withAlpha(0.45f));
        g.drawRect(r.reduced(2.5f), 0.6f);
        // a small lozenge as the arrow
        const float cx = (float)w - 13.0f, cy = (float)h * 0.5f;
        juce::Path p;
        p.startNewSubPath(cx - 4.0f, cy - 2.0f);
        p.lineTo(cx, cy + 2.5f);
        p.lineTo(cx + 4.0f, cy - 2.0f);
        g.setColour(box.isEnabled() ? col::dim : col::faint);
        g.strokePath(p, juce::PathStrokeType(1.2f));
    }

    void positionComboBoxText(juce::ComboBox &box, juce::Label &label) override
    {
        label.setBounds(6, 1, box.getWidth() - 26, box.getHeight() - 2);
        label.setFont(getComboBoxFont(box));
    }

    void drawPopupMenuBackground(juce::Graphics &g, int w, int h) override
    {
        g.fillAll(col::panel);
        g.setColour(col::border);
        g.drawRect(0, 0, w, h, 1);
    }

    void drawScrollbar(juce::Graphics &g, juce::ScrollBar &, int x, int y, int w, int h,
                       bool vertical, int thumbPos, int thumbSize, bool over, bool) override
    {
        g.setColour(col::line);
        if (vertical)
            g.fillRect((float)x + (float)w * 0.5f - 0.5f, (float)y, 1.0f, (float)h);
        g.setColour(over ? col::dim : col::faint);
        if (vertical)
            g.fillRect((float)x + 1.0f, (float)thumbPos, (float)w - 2.0f, (float)thumbSize);
        else
            g.fillRect((float)thumbPos, (float)y + 1.0f, (float)thumbSize, (float)h - 2.0f);
    }

    void drawButtonText(juce::Graphics &g, juce::TextButton &b, bool, bool) override
    {
        g.setFont(getTextButtonFont(b, b.getHeight()));
        g.setColour(b.getToggleState() ? col::bg : col::dim);
        g.drawFittedText(rysum::tr(b.getButtonText()), b.getLocalBounds().reduced(4, 0),
                         juce::Justification::centred, 1);
    }

    // ---- the interface language: texts are given in German and shown via rysum::tr (Lang.h),
    // so a component keeps its German name (MIDI button paths) and only what is drawn changes
    void drawToggleButton(juce::Graphics &g, juce::ToggleButton &button, bool over,
                          bool down) override
    {
        const auto fontSize = juce::jmin(15.0f, (float)button.getHeight() * 0.75f);
        const auto tickWidth = fontSize * 1.1f;
        drawTickBox(g, button, 4.0f, ((float)button.getHeight() - tickWidth) * 0.5f, tickWidth,
                    tickWidth, button.getToggleState(), button.isEnabled(), over, down);
        g.setColour(button.findColour(juce::ToggleButton::textColourId));
        g.setFont(fontSize);
        if (!button.isEnabled())
            g.setOpacity(0.5f);
        g.drawFittedText(rysum::tr(button.getButtonText()),
                         button.getLocalBounds()
                             .withTrimmedLeft(juce::roundToInt(tickWidth) + 10)
                             .withTrimmedRight(2),
                         juce::Justification::centredLeft, 10);
    }
    void drawLabel(juce::Graphics &g, juce::Label &label) override
    {
        g.fillAll(label.findColour(juce::Label::backgroundColourId));
        if (!label.isBeingEdited())
        {
            const auto alpha = label.isEnabled() ? 1.0f : 0.5f;
            const juce::Font font(getLabelFont(label));
            g.setColour(label.findColour(juce::Label::textColourId).withMultipliedAlpha(alpha));
            g.setFont(font);
            const auto textArea = getLabelBorderSize(label).subtractedFrom(label.getLocalBounds());
            g.drawFittedText(rysum::tr(label.getText()), textArea, label.getJustificationType(),
                             juce::jmax(1, (int)((float)textArea.getHeight() / font.getHeight())),
                             label.getMinimumHorizontalScale());
            g.setColour(label.findColour(juce::Label::outlineColourId).withMultipliedAlpha(alpha));
        }
        else if (label.isEnabled())
            g.setColour(label.findColour(juce::Label::outlineColourId));
        g.drawRect(label.getLocalBounds());
    }
    void drawPopupMenuItem(juce::Graphics &g, const juce::Rectangle<int> &area, bool isSeparator,
                           bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                           const juce::String &text, const juce::String &shortcutKeyText,
                           const juce::Drawable *icon, const juce::Colour *textColour) override
    {
        juce::LookAndFeel_V4::drawPopupMenuItem(g, area, isSeparator, isActive, isHighlighted,
                                                isTicked, hasSubMenu, rysum::tr(text),
                                                shortcutKeyText, icon, textColour);
    }
    void drawPopupMenuSectionHeader(juce::Graphics &g, const juce::Rectangle<int> &area,
                                    const juce::String &name) override
    {
        juce::LookAndFeel_V4::drawPopupMenuSectionHeader(g, area, rysum::tr(name));
    }
    void getIdealPopupMenuItemSize(const juce::String &text, bool isSeparator, int standardHeight,
                                   int &idealWidth, int &idealHeight) override
    {
        juce::LookAndFeel_V4::getIdealPopupMenuItemSize(rysum::tr(text), isSeparator,
                                                        standardHeight, idealWidth, idealHeight);
    }
    juce::Rectangle<int> getTooltipBounds(const juce::String &tip, juce::Point<int> pos,
                                          juce::Rectangle<int> parentArea) override
    {
        return juce::LookAndFeel_V4::getTooltipBounds(rysum::tr(tip), pos, parentArea);
    }
    void drawTooltip(juce::Graphics &g, const juce::String &text, int w, int h) override
    {
        juce::LookAndFeel_V4::drawTooltip(g, rysum::tr(text), w, h);
    }

    void drawLinearSlider(juce::Graphics &g, int x, int y, int w, int h, float pos, float, float,
                          juce::Slider::SliderStyle style, juce::Slider &s) override
    {
        // lozenge thumbs on a ruled line
        auto lozenge = [&g](float cx, float cy, float rw, float rh, juce::Colour c)
        {
            juce::Path p;
            p.startNewSubPath(cx, cy - rh);
            p.lineTo(cx + rw, cy);
            p.lineTo(cx, cy + rh);
            p.lineTo(cx - rw, cy);
            p.closeSubPath();
            g.setColour(c);
            g.fillPath(p);
            g.setColour(col::bg);
            g.strokePath(p, juce::PathStrokeType(1.0f));
        };
        const auto thumbCol = s.isMouseOverOrDragging() ? col::white : col::ink;
        if (style == juce::Slider::LinearVertical)
        {
            const float cx = (float)x + (float)w * 0.5f;
            g.setColour(col::track);
            g.fillRect(cx - 0.5f, (float)y, 1.0f, (float)h);
            g.setColour(col::border);
            g.fillRect(cx - 3.5f, (float)y, 1.0f, (float)h);
            g.fillRect(cx + 2.5f, (float)y, 1.0f, (float)h);
            lozenge(cx, pos, (float)w * 0.5f, 6.0f, thumbCol);
            return;
        }
        const float cy = (float)y + (float)h * 0.5f;
        g.setColour(col::track);
        g.fillRect((float)x, cy - 0.5f, (float)w, 1.0f);
        g.setColour(col::ink.withAlpha(0.85f));
        if (s.getProperties().contains("bipolar"))
        {
            const float mid = (float)x + (float)w * 0.5f;
            g.fillRect(juce::jmin(mid, pos), cy - 1.0f, std::abs(pos - mid), 2.0f);
            g.setColour(col::faint);
            g.fillRect(mid - 0.5f, cy - 4.0f, 1.0f, 8.0f);
        }
        else
            g.fillRect((float)x, cy - 1.0f, pos - (float)x, 2.0f);
        // tick marks at the ends, like a scale on an old instrument
        g.setColour(col::faint);
        g.fillRect((float)x, cy - 3.0f, 1.0f, 6.0f);
        g.fillRect((float)(x + w) - 1.0f, cy - 3.0f, 1.0f, 6.0f);
        lozenge(pos, cy, 4.5f, 7.0f, thumbCol);
    }

    int getSliderThumbRadius(juce::Slider &) override
    {
        return 5;
    }
};

/** implemented by the editor content: right-click modulation menu + live modulation values for the
 * sliders */
struct ModUiAccess
{
    virtual ~ModUiAccess() = default;
    virtual void showModMenu(juce::Component &target, const juce::String &paramId) = 0;
    virtual int paramIndexFor(const juce::String &paramId) = 0;
    virtual float modulatedNorm(int paramIndex) = 0; // NaN = not modulated
    // routings that act on a parameter (for drawing / dragging their range at the slider)
    virtual int routesFor(int paramIndex, int *out, int maxCount) = 0;
    virtual int routeSource(int r) = 0;
    virtual bool routeUni(int r) = 0;
    virtual juce::RangedAudioParameter *routeAmountParam(int r) = 0;
    virtual juce::String routeLabel(int r) = 0;
    /** opens the MIDI controller assignments on the modulation page */
    virtual void openMidiMap() {}
    /** MIDI learn state of a parameter: -2 = waiting for a controller, -1 = none, else the
     * controller number */
    virtual int midiControllerFor(int)
    {
        return -1;
    }
    /** opens the mode in which a click on any button waits for a MIDI button to press it */
    virtual void startButtonLearn() {}
    /** Verlauf: the curve's value of an automated parameter (before modulation), NaN = none */
    virtual float automatedNorm(int)
    {
        return std::numeric_limits<float>::quiet_NaN();
    }
};

/** a switch of the interface bound to its parameter; the button also says which parameter it is,
    so a MIDI button learned for it drives the parameter (and works while the window is closed) */
inline std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
attachButton(juce::AudioProcessorValueTreeState &state, const juce::String &paramId,
             juce::Button &b)
{
    b.getProperties().set("midiParam", paramId);
    return std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, paramId,
                                                                                 b);
}

/** a button that acts for as long as it is held: by the mouse, or by a MIDI button mapped to it */
struct HoldButton : juce::TextButton
{
    using juce::TextButton::TextButton;
    bool midiHeld = false;
    bool held() const
    {
        return isDown() || midiHeld;
    }
};

/** parses what someone typed into a value field, in the units the parameter shows
    ("50" on a % slider = 50 %, "300 ms", "1.5 s", "440" on the pitch = a' in Hz, "22 kHz" ...) */
inline float parseValueText(juce::RangedAudioParameter &p, const juce::String &typed)
{
    auto t = typed.trim().replace(",", ".");
    const auto lower = t.toLowerCase();
    const auto shown = p.getCurrentValueAsText().toLowerCase();
    int i = 0;
    while (i < t.length() &&
           !(juce::CharacterFunctions::isDigit(t[i]) || t[i] == '-' || t[i] == '+' || t[i] == '.'))
        ++i;
    const float num = t.substring(i).replace("+", "").getFloatValue();
    const auto range = p.getNormalisableRange();
    float v = num;
    if (p.getParameterID() == "pitch")
    {
        // a' in Hz (as shown) or cents
        if (lower.contains("ct") || std::abs(num) <= 50.0f)
            v = num;
        else
            v = (float)(1200.0 * std::log2(num / kA4));
    }
    else if (shown.contains("%") && range.end <= 1.0f + 1.0e-6f)
        v = num / 100.0f;
    else if (shown.contains("khz"))
        v = lower.contains("khz") || !lower.contains("hz") ? num * 1000.0f : num;
    else if (shown.endsWith(" ms") || shown.endsWith(" s"))
    {
        if (lower.contains("ms"))
            v = num / 1000.0f;
        else if (lower.contains("s"))
            v = num;
        else
            v = shown.endsWith(" ms") ? num / 1000.0f : num;
    }
    return juce::jlimit(range.start, range.end, v);
}

/** a one-line text field over a value: Return applies, Escape / clicking elsewhere cancels */
class InlineValueEditor
{
public:
    void open(juce::Component &parent, juce::Rectangle<int> area, juce::RangedAudioParameter *p)
    {
        if (p == nullptr)
            return;
        param = p;
        ed = std::make_unique<juce::TextEditor>();
        ed->setFont(Assets::get().monoFont(12.0f));
        ed->setJustification(juce::Justification::centredRight);
        ed->setText(p->getCurrentValueAsText(), false);
        ed->setBounds(area);
        ed->setIndents(3, 1);
        parent.addAndMakeVisible(*ed);
        ed->selectAll();
        ed->grabKeyboardFocus();
        ed->onReturnKey = [this]
        {
            const float v = parseValueText(*param, ed->getText());
            param->beginChangeGesture();
            param->setValueNotifyingHost(param->convertTo0to1(v));
            param->endChangeGesture();
            close();
        };
        ed->onEscapeKey = [this] { close(); };
        ed->onFocusLost = [this] { close(); };
    }
    bool isOpen() const
    {
        return ed != nullptr;
    }

private:
    void close()
    {
        // deferred: we may be inside one of the editor's own callbacks
        if (ed == nullptr)
            return;
        auto *e = ed.release();
        e->onReturnKey = nullptr;
        e->onEscapeKey = nullptr;
        e->onFocusLost = nullptr;
        e->setVisible(false);
        juce::MessageManager::callAsync([e] { delete e; });
    }
    std::unique_ptr<juce::TextEditor> ed;
    juce::RangedAudioParameter *param = nullptr;
};

/** slider that hands right-clicks to a callback and lets its owner grab presses (range handles) */
class RowSlider : public juce::Slider
{
public:
    // Optional value-unit raster for precision controls; Alt keeps normal range dragging.
    double fineDragIncrement = 0.0;
    std::function<void()> onRightClick;
    std::function<bool(const juce::MouseEvent &)> interceptDown; // return true to take the drag
    std::function<void(const juce::MouseEvent &)> interceptDrag, interceptUp;

    void mouseDown(const juce::MouseEvent &e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onRightClick)
                onRightClick();
            return;
        }
        intercepting = interceptDown && interceptDown(e);
        if (!intercepting && fineDragIncrement > 0.0 && !e.mods.isAltDown())
        {
            fineGesture = std::make_unique<juce::Slider::ScopedDragNotification>(*this);
            // Track clicks retain the normal absolute jump. A press on the
            // thumb starts a fine drag without moving the value first.
            if (std::abs(e.position.x - getPositionOfValue(getValue())) > 6.0f)
                setValue(proportionOfLengthToValue(proportionAtX(e.position.x)),
                         juce::sendNotificationSync);
            fineStartValue = getValue();
            return;
        }
        if (!intercepting)
            juce::Slider::mouseDown(e);
    }
    void mouseDrag(const juce::MouseEvent &e) override
    {
        if (e.mods.isPopupMenu())
            return;
        if (intercepting)
        {
            if (interceptDrag)
                interceptDrag(e);
            return;
        }
        if (fineGesture != nullptr)
        {
            const double next =
                fineStartValue +
                static_cast<double>(e.getDistanceFromDragStartX()) * fineDragIncrement;
            setValue(std::round(next / fineDragIncrement) * fineDragIncrement,
                     juce::sendNotificationSync);
            return;
        }
        juce::Slider::mouseDrag(e);
    }
    void mouseUp(const juce::MouseEvent &e) override
    {
        if (fineGesture != nullptr)
        {
            fineGesture.reset();
            return;
        }
        if (e.mods.isPopupMenu())
            return;
        if (intercepting)
        {
            intercepting = false;
            if (interceptUp)
                interceptUp(e);
            return;
        }
        juce::Slider::mouseUp(e);
    }
    void mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) override
    {
        if (fineDragIncrement <= 0.0 || e.mods.isAltDown())
        {
            juce::Slider::mouseWheelMove(e, wheel);
            return;
        }
        if (!isEnabled() || fineGesture != nullptr || e.mods.isAnyMouseButtonDown())
            return;
        const double delta =
            std::abs(wheel.deltaY) >= std::abs(wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
        if (std::abs(delta) <= 0.0)
            return;
        const double direction = (delta > 0.0 ? 1.0 : -1.0) * (wheel.isReversed ? -1.0 : 1.0);
        juce::Slider::ScopedDragNotification gesture(*this);
        const double next = getValue() + direction * fineDragIncrement;
        setValue(std::round(next / fineDragIncrement) * fineDragIncrement,
                 juce::sendNotificationSync);
    }
    void mouseDoubleClick(const juce::MouseEvent &e) override
    {
        if (!intercepting)
        {
            fineGesture.reset();
            juce::Slider::mouseDoubleClick(e);
            fineStartValue = getValue();
        }
    }

    /** normalised (0..1 of the parameter) value under an x position */
    double proportionAtX(float x)
    {
        const double x0 = getPositionOfValue(getMinimum()), x1 = getPositionOfValue(getMaximum());
        return juce::jlimit(0.0, 1.0, (x - x0) / juce::jmax(1.0, x1 - x0));
    }

private:
    std::unique_ptr<juce::Slider::ScopedDragNotification> fineGesture;
    double fineStartValue = 0.0;
    bool intercepting = false;
};

/** label + value + thin slider, attached to a parameter.
    - click the value to type a number
    - right-click = modulation menu
    - modulated: the red bar is each routing's range (drag its lozenge to change the amount),
      the white tick is where the modulation takes the parameter right now */
class ParamRow : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    /** the tooltip also shows over the slider */
    void setTooltip(const juce::String &t) override
    {
        SettableTooltipClient::setTooltip(t);
        slider.setTooltip(t);
    }
    void setDisabledValueText(juce::String text, juce::String label = {})
    {
        disabledValueText = std::move(text);
        disabledTitle = std::move(label);
        repaint();
    }

    ParamRow(juce::AudioProcessorValueTreeState &state, const juce::String &paramId,
             const juce::String &name, bool compactRow = false)
        : title(name), compact(compactRow), id(paramId)
    {
        slider.onRightClick = [this] { openMenu(); };
        slider.interceptDown = [this](const juce::MouseEvent &e) { return handleDown(e); };
        slider.interceptDrag = [this](const juce::MouseEvent &e) { handleDrag(e); };
        slider.interceptUp = [this](const juce::MouseEvent &) { handleUp(); };
        startTimerHz(25);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setDoubleClickReturnValue(true,
                                         (double)state.getParameterRange(paramId).convertFrom0to1(
                                             state.getParameter(paramId)->getDefaultValue()));
        addAndMakeVisible(slider);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            state, paramId, slider);
        param = state.getParameter(paramId);
        slider.onValueChange = [this] { repaint(); };
    }

    void paint(juce::Graphics &g) override
    {
        if (findAccess())
            midiCc = access->midiControllerFor(index);
        auto r = headerArea();
        g.setColour(compact ? col::dim : col::ink);
        g.setFont(compact ? Assets::get().romanFont(11.5f) : Assets::get().sansFont(14.0f));
        g.drawText(rysum::tr(!isEnabled() && disabledTitle.isNotEmpty() ? disabledTitle : title), r,
                   juce::Justification::centredLeft);
        g.setFont(Assets::get().monoFont(compact ? 10.0f : 12.0f));
        if (!isEnabled() && disabledValueText.isNotEmpty())
        {
            g.setColour(col::dim);
            g.drawText(rysum::tr(disabledValueText), r, juce::Justification::centredRight);
            return;
        }
        if (dragRoute >= 0 && access != nullptr)
        {
            auto *a = access->routeAmountParam(dragRoute);
            g.setColour(col::rubric.brighter(0.4f));
            g.drawText(access->routeLabel(dragRoute) + ": " +
                           (a != nullptr ? a->getCurrentValueAsText() : juce::String()),
                       r, juce::Justification::centredRight);
        }
        else if (midiCc == -2)
        {
            g.setColour(col::rubric.brighter(0.5f));
            g.drawText(rysum::tr("MIDI: Regler bewegen \xe2\x80\xa6"), r,
                       juce::Justification::centredRight);
        }
        else
        {
            // automated (Verlauf): the value the curve gives it right now
            const bool automated = !std::isnan(autoNorm) && param != nullptr;
            const juce::String valueText = rysum::trText(
                automated ? param->getText(autoNorm, 1024)
                          : (param != nullptr ? param->getCurrentValueAsText() : juce::String()));
            g.setColour(valueHover ? col::white : col::dim);
            g.drawText(valueText, r, juce::Justification::centredRight);
            if (midiCc >= 0 || automated)
            {
                // a small tag: this one follows a controller / the Verlauf
                const auto tag = automated ? rysum::tr("Verlauf") : midiCc >= 128 ? "Note " + juce::MidiMessage::getMidiNoteName(midiCc - 128, true, true, 4) : "CC " + juce::String(midiCc); // (128 + n: note n)
                g.setFont(Assets::get().monoFont(compact ? 8.5f : 9.5f));
                g.setColour(automated ? col::rubric.brighter(0.35f) : col::faint);
                const int tw = juce::roundToInt(juce::GlyphArrangement::getStringWidth(
                                   g.getCurrentFont(), tag)) +
                               4;
                const int vw = juce::roundToInt(juce::GlyphArrangement::getStringWidth(
                    Assets::get().monoFont(compact ? 10.0f : 12.0f), valueText));
                g.drawText(tag, r.withTrimmedRight(vw + 6).removeFromRight(tw),
                           juce::Justification::centredRight);
            }
        }
    }

    void resized() override
    {
        if (compact)
            slider.setBounds(getLocalBounds().withTrimmedTop(14).withHeight(14));
        else
            slider.setBounds(getLocalBounds().withTrimmedTop(16).withHeight(18));
    }

    void mouseDown(const juce::MouseEvent &e) override
    {
        if (e.mods.isPopupMenu())
        {
            openMenu();
            return;
        }
        if (valueArea().contains(e.getPosition()))
            editor.open(*this, valueArea().expanded(2, 1).withTrimmedLeft(-30), param);
    }
    void mouseMove(const juce::MouseEvent &e) override
    {
        const bool h = valueArea().contains(e.getPosition());
        if (h != valueHover)
        {
            valueHover = h;
            setMouseCursor(h ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor);
            repaint(headerArea());
        }
    }
    void mouseExit(const juce::MouseEvent &) override
    {
        if (valueHover)
        {
            valueHover = false;
            setMouseCursor(juce::MouseCursor::NormalCursor);
            repaint(headerArea());
        }
    }

    void paintOverChildren(juce::Graphics &g) override
    {
        if (!isEnabled() || param == nullptr || !findAccess())
            return;
        modNorm = access->modulatedNorm(index);
        const auto sb = slider.getBounds().toFloat();
        const float cy = sb.getCentreY();
        auto xOfNorm = [this](float n)
        {
            return (float)slider.getX() + (float)slider.getPositionOfValue(
                                              param->convertFrom0to1(juce::jlimit(0.0f, 1.0f, n)));
        };

        // routing ranges (red) with a draggable lozenge at the full-amount end
        nHandles = access->routesFor(index, handleRoute.data(), (int)handleRoute.size());
        const float base = std::isnan(autoNorm) ? param->getValue() : autoNorm;
        for (int i = 0; i < nHandles; ++i)
        {
            const int r = handleRoute[(size_t)i];
            auto *ap = access->routeAmountParam(r);
            if (ap == nullptr)
                continue;
            const float amt = ap->convertFrom0to1(ap->getValue());
            const float lo = base + amt * sourceMin(access->routeSource(r), access->routeUni(r)),
                        hi = base + amt;
            const float y = cy + (compact ? 4.5f : 5.5f) + 3.5f * (float)i;
            const float xa = xOfNorm(juce::jmin(lo, hi)), xb = xOfNorm(juce::jmax(lo, hi));
            g.setColour(col::rubric.withAlpha(r == dragRoute ? 1.0f : 0.85f));
            g.fillRect(xa, y - 0.75f, juce::jmax(1.0f, xb - xa), 1.5f);
            const float hx = xOfNorm(hi);
            handleX[(size_t)i] = hx - (float)slider.getX();
            handleY[(size_t)i] = y - (float)slider.getY();
            g.setColour(col::rubric.brighter(r == dragRoute ? 0.6f : 0.3f));
            juce::Path p;
            p.startNewSubPath(hx, y - 3.0f);
            p.lineTo(hx + 2.5f, y);
            p.lineTo(hx, y + 3.0f);
            p.lineTo(hx - 2.5f, y);
            p.closeSubPath();
            g.fillPath(p);
        }
        // where the modulation is right now
        if (!std::isnan(modNorm))
        {
            const float x1 = xOfNorm(modNorm);
            g.setColour(col::white);
            g.fillRect(x1 - 1.0f, cy - 5.0f, 2.0f, 10.0f);
        }
    }

    RowSlider slider;
    const juce::String &getParamId() const
    {
        return id;
    }

private:
    juce::Rectangle<int> headerArea() const
    {
        return getLocalBounds().removeFromTop(compact ? 16 : 18);
    }
    juce::Rectangle<int> valueArea() const
    {
        return headerArea().removeFromRight(compact ? 72 : 88);
    }

    bool handleDown(const juce::MouseEvent &e)
    {
        for (int i = 0; i < nHandles; ++i)
            if (std::abs(e.position.x - handleX[(size_t)i]) < 6.0f &&
                std::abs(e.position.y - handleY[(size_t)i]) < 4.0f)
            {
                dragRoute = handleRoute[(size_t)i];
                if (auto *ap = access->routeAmountParam(dragRoute))
                    ap->beginChangeGesture();
                handleDrag(e);
                return true;
            }
        return false;
    }
    void handleDrag(const juce::MouseEvent &e)
    {
        if (dragRoute < 0 || access == nullptr)
            return;
        auto *ap = access->routeAmountParam(dragRoute);
        if (ap == nullptr)
            return;
        const float n = (float)slider.proportionAtX(e.position.x);
        float amt = juce::jlimit(-1.0f, 1.0f, n - param->getValue());
        if (e.mods.isShiftDown())
            amt = std::round(amt * 20.0f) / 20.0f; // shift: 5 % steps
        ap->setValueNotifyingHost(ap->convertTo0to1(amt));
        repaint();
    }
    void handleUp()
    {
        if (dragRoute >= 0 && access != nullptr)
            if (auto *ap = access->routeAmountParam(dragRoute))
                ap->endChangeGesture();
        dragRoute = -1;
        repaint();
    }

    void openMenu()
    {
        if (auto *a = findParentComponentOfClass<ModUiAccess>())
            a->showModMenu(*this, id);
    }

    bool findAccess()
    {
        if (access == nullptr)
        {
            access = findParentComponentOfClass<ModUiAccess>();
            if (access != nullptr)
                index = access->paramIndexFor(id);
        }
        return access != nullptr;
    }

    void timerCallback() override
    {
        if (!isShowing() || !findAccess())
            return;
        const float n = access->modulatedNorm(index);
        std::array<int, 3> rr{};
        const int nr = access->routesFor(index, rr.data(), 3);
        const bool was = !std::isnan(modNorm), is = !std::isnan(n);
        // the routing ranges too (their amounts, the value they hang from): the pages no longer
        // repaint every row every frame - a repaint only when something here changed (in full
        // screen each one costs a lot)
        float sig = param != nullptr ? param->getValue() : 0.0f;
        for (int i = 0; i < nr; ++i)
            if (auto *ap = access->routeAmountParam(rr[(size_t)i]))
                sig += (float)(i + 2) * 7.31f * ap->getValue() +
                       (float)access->routeSource(rr[(size_t)i]);
        if (was != is || (is && std::abs(n - modNorm) > 0.0005f) || nr != nHandles ||
            std::abs(sig - routeSig) > 1.0e-6f)
        {
            modNorm = n;
            routeSig = sig;
            repaint();
        }
        // Verlauf: the slider follows the curve (the parameter itself stays where the hand left
        // it); while it is held, the hand counts
        {
            const float an = access->automatedNorm(index);
            const bool wasA = !std::isnan(autoNorm), isA = !std::isnan(an);
            if (param != nullptr && !slider.isMouseButtonDown())
            {
                if (isA)
                    slider.setValue(param->convertFrom0to1(an), juce::dontSendNotification);
                else if (wasA)
                    slider.setValue(param->convertFrom0to1(param->getValue()),
                                    juce::dontSendNotification);
            }
            if (wasA != isA || (isA && std::abs(an - autoNorm) > 0.0005f))
            {
                autoNorm = an;
                repaint();
            }
        }
        const int cc = access->midiControllerFor(index);
        if (cc != midiCc)
        {
            midiCc = cc;
            repaint(headerArea());
        }
    }

    juce::String disabledValueText, disabledTitle;
    juce::String title;
    bool compact = false;
    juce::String id;
    ModUiAccess *access = nullptr;
    int index = -1;
    float modNorm = std::numeric_limits<float>::quiet_NaN();
    float autoNorm = std::numeric_limits<float>::quiet_NaN(); // Verlauf: the curve's value
    float routeSig = 0.0f; // what the routing ranges were drawn from
    juce::RangedAudioParameter *param = nullptr;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    InlineValueEditor editor;
    bool valueHover = false;
    std::array<int, 3> handleRoute{};
    std::array<float, 3> handleX{}, handleY{};
    int nHandles = 0, dragRoute = -1;
    int midiCc = -1;
};

/** a small printer's lozenge ornament */
inline void drawLozenge(juce::Graphics &g, juce::Point<float> c, float rw, float rh)
{
    juce::Path p;
    p.startNewSubPath(c.x, c.y - rh);
    p.lineTo(c.x + rw, c.y);
    p.lineTo(c.x, c.y + rh);
    p.lineTo(c.x - rw, c.y);
    p.closeSubPath();
    g.fillPath(p);
}

/** section head: thick-thin double rule with a lozenge, the title in spaced capitals with a red
 * initial */
inline void drawSectionTitle(juce::Graphics &g, juce::Rectangle<int> r,
                             const juce::String &germanTitle, const juce::String &germanRight = {})
{
    auto &A = Assets::get();
    const auto title = rysum::tr(germanTitle), right = rysum::trText(germanRight);
    const float x0 = (float)r.getX(), x1 = (float)r.getRight(), y = (float)r.getY();
    g.setColour(col::ink.withAlpha(0.9f));
    g.fillRect(x0, y, x1 - x0, 1.5f);
    g.setColour(col::ink.withAlpha(0.45f));
    g.fillRect(x0, y + 3.5f, x1 - x0, 0.6f);
    g.setColour(col::rubric);
    drawLozenge(g, {x1 - 2.0f, y + 2.0f}, 3.0f, 4.5f);

    auto line = r.withTrimmedTop(8).withHeight(22);
    // umlauts explicitly: toUpperCase depends on the C locale for non-ASCII letters
    const auto upper =
        title.toUpperCase()
            .replace(juce::String::fromUTF8("\xc3\xa4"), juce::String::fromUTF8("\xc3\x84"))
            .replace(juce::String::fromUTF8("\xc3\xb6"), juce::String::fromUTF8("\xc3\x96"))
            .replace(juce::String::fromUTF8("\xc3\xbc"), juce::String::fromUTF8("\xc3\x9c"));
    if (upper.isNotEmpty())
    {
        const auto initial = upper.substring(0, 1), rest = upper.substring(1);
        const auto big = A.romanFont(19.0f),
                   small = A.romanFont(14.5f).withExtraKerningFactor(0.14f);
        g.setColour(col::rubric.brighter(0.25f));
        g.setFont(big);
        const int iw = juce::roundToInt(juce::GlyphArrangement::getStringWidth(big, initial)) + 2;
        g.drawText(initial, line.withWidth(iw + 4), juce::Justification::centredLeft);
        g.setColour(col::ink);
        g.setFont(small);
        g.drawText(rest, line.withTrimmedLeft(iw).translated(0, 1),
                   juce::Justification::centredLeft);
    }
    if (right.isNotEmpty())
    {
        g.setColour(col::mute);
        g.setFont(A.monoFont(11.0f));
        g.drawText(right, line, juce::Justification::centredRight);
    }
}

/** dot style per stop, as in the mockup */
inline void drawStopDot(juce::Graphics &g, int stop, juce::Point<float> c, float size,
                        bool highlight)
{
    const juce::Colour fills[]{col::ink, col::mute, col::ink,   col::mute,
                               col::dim, col::dim,  col::white, col::ink};
    auto r = juce::Rectangle<float>(size, size).withCentre(c);
    g.setColour(highlight ? col::white : fills[juce::jlimit(0, 7, stop)]);
    if (stop == kPraestant16)
    {
        juce::Path p;
        const float h = size * 0.75f;
        p.startNewSubPath(c.x, c.y - h);
        p.lineTo(c.x + h, c.y);
        p.lineTo(c.x, c.y + h);
        p.lineTo(c.x - h, c.y);
        p.closeSubPath();
        g.fillPath(p);
        return;
    }
    const bool hollow = stop == 2 || stop == 3 || stop == 5;
    const bool square = stop >= 4;
    if (square)
    {
        if (hollow)
            g.drawRect(r, 1.0f);
        else
            g.fillRect(r);
    }
    else
    {
        if (hollow)
            g.drawEllipse(r.reduced(0.5f), 1.0f);
        else
            g.fillEllipse(r);
    }
}

/** the symbol of the Vox humana (Raum, Saal): rings in rings - its harmonics */
inline void drawVoxGlyph(juce::Graphics &g, juce::Point<float> c, float size, bool on)
{
    g.setColour(on ? col::ink : col::faint);
    for (int i = 0; i < 3; ++i)
    {
        const float s = size * (1.0f - 0.3f * (float)i);
        g.drawEllipse(juce::Rectangle<float>(s, s * 0.62f).withCentre(c.translated(0.0f, -size * 0.12f * (float)i)),
                      i == 0 ? 1.4f : 1.0f);
    }
    g.fillEllipse(juce::Rectangle<float>(size * 0.22f, size * 0.22f).withCentre(c.translated(0.0f, -size * 0.3f)));
}
} // namespace rysum::ui
