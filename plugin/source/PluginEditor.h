#pragma once

#include "ConsoleView.h"
#include "EffectsView.h"
#include "KlangView.h"
#include "ModView.h"
#include "OrganView.h"
#include "PluginProcessor.h"
#include "SaalView.h"
#include "SpaceView.h"
#include "SpeakerView.h"
#include "Theme.h"

/** the full-screen toggle in the header: four corner marks (outwards = go full screen, inwards =
 * back) */
class FullscreenButton : public juce::Button
{
public:
    FullscreenButton() : juce::Button("Vollbild") {}
    bool active = false;
    void paintButton(juce::Graphics &g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(over || down ? rysum::ui::col::dim : rysum::ui::col::border);
        g.drawRect(r, 1.0f);
        g.setColour((over || down ? rysum::ui::col::dim : rysum::ui::col::border).withAlpha(0.45f));
        g.drawRect(r.reduced(2.5f), 0.6f);
        // corner marks
        auto c = r.reduced(8.0f);
        const float L = c.getWidth() * 0.38f;
        g.setColour(over ? rysum::ui::col::white : rysum::ui::col::ink);
        juce::Path p;
        auto corner = [&](juce::Point<float> at, float dx, float dy)
        {
            // outwards: the mark points to the corner; inwards: from the corner towards the centre
            if (!active)
            {
                p.startNewSubPath(at.x + dx * L, at.y);
                p.lineTo(at);
                p.lineTo(at.x, at.y + dy * L);
            }
            else
            {
                const juce::Point<float> in(at.x + dx * L, at.y + dy * L);
                p.startNewSubPath(at.x + dx * L, at.y);
                p.lineTo(in);
                p.lineTo(at.x, at.y + dy * L);
            }
        };
        corner(c.getTopLeft(), 1.0f, 1.0f);
        corner(c.getTopRight(), -1.0f, 1.0f);
        corner(c.getBottomLeft(), 1.0f, -1.0f);
        corner(c.getBottomRight(), -1.0f, -1.0f);
        g.strokePath(p, juce::PathStrokeType(1.4f));
    }
};

/** fixed 1280 x 720 design, scaled as a whole when the window is resized */
class RysumContent : public juce::Component, public rysum::ui::ModUiAccess
{
public:
    bool keyPressed(const juce::KeyPress &) override;
    bool keyStateChanged(bool isKeyDown) override;
    void releaseComputerKeys();

public:
    void showModMenu(juce::Component &target, const juce::String &paramId) override;
    int paramIndexFor(const juce::String &paramId) override
    {
        return proc.getParamIndex(paramId);
    }
    float modulatedNorm(int index) override
    {
        return proc.getModulatedNorm(index);
    }
    int routesFor(int paramIndex, int *out, int maxCount) override
    {
        int n = 0;
        for (int r = 0; r < rysum::kNumRoutes && n < maxCount; ++r)
            if (proc.getRouteSrc(r) >= 0 &&
                proc.getRouteTgt(r) == rysum::kGlobalTargetBase + paramIndex)
                out[n++] = r;
        return n;
    }
    int routeSource(int r) override
    {
        return proc.getRouteSrc(r);
    }
    bool routeUni(int r) override
    {
        return proc.getRouteUni(r);
    }
    juce::RangedAudioParameter *routeAmountParam(int r) override
    {
        return proc.apvts.getParameter(proc.getModAmountId(r));
    }
    juce::String routeLabel(int r) override
    {
        const int s = proc.getRouteSrc(r);
        return s >= 0 ? rysum::modSourceNames()[s] : juce::String();
    }
    void openMidiMap() override
    {
        mod.showMidiTab(true);
        showView(5);
    }
    int midiControllerFor(int paramIndex) override
    {
        const int learning = proc.getLearningSlot();
        int cc = -1;
        for (int slot : proc.midiMapsFor(paramIndex))
        {
            if (slot == learning)
                return -2;
            if (cc < 0)
                cc = proc.getMidiMap(slot).cc;
        }
        return cc;
    }
    explicit RysumContent(RysumProcessor &);
    void paint(juce::Graphics &) override;
    void paintOverChildren(juce::Graphics &) override;
    void resized() override;
    void showView(int v);
    void tick();
    void showPatchMenu();
    void stepPatch(int delta);
    void askSavePatch();
    /** set by the editor: switches the full-screen view on / off */
    std::function<void()> onToggleFullscreen;
    void setFullscreenShown(bool on)
    {
        fullButton.active = on;
        fullButton.setTooltip(on ? juce::String::fromUTF8("Zur\xc3\xbc"
                                                          "ck ins Fenster (Esc)")
                                 : juce::String("Vollbild (F11)"));
        fullButton.repaint();
    }

private:
    RysumProcessor &proc;
    rysum::ui::ConsoleView console;
    rysum::ui::SpaceView space;
    rysum::ui::KlangView klang;
    rysum::ui::ModView mod;
    rysum::ui::EffectsView fx;
    rysum::ui::SaalView saal;
    rysum::ui::OrganView organ;
    rysum::ui::SpeakerView speakers{proc}; // view 7: the loudspeaker layout
    juce::TextButton outInfoButton;        // over the channel info in the top bar: opens it
    /** the "Rysum" in the top bar opens the page about the organ (and goes back) */
    struct LogoButton : juce::Button
    {
        LogoButton() : juce::Button("Rysum")
        {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        }
        void paintButton(juce::Graphics &, bool, bool) override {}
    } logoButton;
    int viewBeforeOrgan = 0;
    juce::TextButton diceButton{"Zufall"};
    void showDiceMenu();
    std::array<juce::TextButton, 6> tabs;
    juce::TextButton keysButton{"Tastatur"}, latchButton{"Halten"};
    std::array<int, 18> keyNoteDown{}; // MIDI note each computer key is playing (-1 = up)
    int keysVelocity = 100;
    void updatePlayButtons();
    FullscreenButton fullButton;
    juce::ComboBox outputChoice;
    juce::TextButton patchPrev{juce::String::fromUTF8("\xe2\x97\x80")},
        patchNext{juce::String::fromUTF8("\xe2\x96\xb6")}, patchName, patchSave{"Speichern"};
    int shownPatchVersion = -1;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> outputAttachment;
    int current = 0;
    bool speakerPageShown = false;
    juce::Image grainLayer, vignette;
};

class RysumEditor : public juce::AudioProcessorEditor, private juce::MultiTimer
{
public:
    explicit RysumEditor(RysumProcessor &);
    ~RysumEditor() override;
    void paint(juce::Graphics &g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override
    {
        if (fullscreen != nullptr)
            toggleFullscreen();
    }
    void toggleFullscreen();
    bool isFullscreen() const
    {
        return fullscreen != nullptr;
    }

private:
    void timerCallback(int id) override;
    bool sizeRestored = false;
    float scaleToSave = 1.0f, savedScale = -1.0f;
    juce::SharedResourcePointer<rysum::ui::Assets>
        assets; // fonts + images, alive while an editor exists
    juce::SharedResourcePointer<rysum::ui::LookAndFeel>
        lnf; // also the default look (context menus, dialogs)
    RysumContent content;
    juce::TooltipWindow tooltips{this, 600};

    /** the interface on a whole screen: a borderless window the content moves into (and back) */
    class FullscreenWindow : public juce::Component
    {
    public:
        explicit FullscreenWindow(RysumEditor &e) : editor(e)
        {
            setOpaque(true);
            setWantsKeyboardFocus(true);
        }
        void paint(juce::Graphics &g) override
        {
            g.fillAll(juce::Colours::black);
        }
        void resized() override;
        bool keyPressed(const juce::KeyPress &k) override;
        void userTriedToCloseWindow() override
        {
            editor.toggleFullscreen();
        }

    private:
        RysumEditor &editor;
        juce::TooltipWindow tips{this, 600};
    };
    std::unique_ptr<FullscreenWindow> fullscreen;
};
