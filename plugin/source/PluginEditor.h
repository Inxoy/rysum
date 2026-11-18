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
#include "VerlaufView.h"
#include "Theme.h"
#include "WalkView.h"

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

/** the interface language in the header: EN over DE, the current one lit; a click switches */
class LanguageButton : public juce::Button
{
public:
    LanguageButton() : juce::Button("Sprache") {}
    void paintButton(juce::Graphics &g, bool over, bool down) override
    {
        using namespace rysum::ui;
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(over || down ? col::dim : col::border);
        g.drawRect(r, 1.0f);
        const bool de = rysum::german();
        const int h = getHeight() / 2;
        g.setFont(Assets::get().monoFont(10.0f));
        g.setColour(!de ? (over ? col::white : col::ink) : col::faint);
        g.drawText("EN", getLocalBounds().withHeight(h).withTrimmedTop(2),
                   juce::Justification::centred);
        g.setColour(de ? (over ? col::white : col::ink) : col::faint);
        g.drawText("DE", getLocalBounds().withTrimmedTop(h).withTrimmedBottom(2),
                   juce::Justification::centred);
    }
};

/** fixed 1280 x 720 design, scaled as a whole when the window is resized */
namespace rysum::ui
{
/** the "Konzert" panel (play for listeners elsewhere, or listen to a player) */
std::unique_ptr<juce::Component> makeConcertPanel(RysumProcessor &);
}

namespace rysum::ui
{
/** where a button sits in the interface, as text: what a MIDI button mapped to it is saved with.
    Each step is the component's ID, else its name (the n-th of that name), else its place among
    the unnamed ones. */
juce::String buttonPath(juce::Component &root, juce::Button &b);
juce::Button *findButton(juce::Component &root, const juce::String &path);

/** "MIDI-Tasten zuordnen": lies over the whole interface. A click on any button waits for the next
    MIDI button (a pad, a controller button, a key) and gives it that button; nothing on screen
    is operated meanwhile. Switches go to their parameter (they then work with the window closed),
    everything else is clicked by the editor. */
class ButtonLearnOverlay : public juce::Component
{
public:
    ButtonLearnOverlay(RysumProcessor &, juce::Component &root);
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseMove(const juce::MouseEvent &) override;
    void mouseExit(const juce::MouseEvent &) override;
    bool keyPressed(const juce::KeyPress &) override;
    void visibilityChanged() override;
    /** from the editor's timer: what is on screen and what is mapped may have changed */
    void refresh();
    /** this button waits for its MIDI button; false = no place left */
    bool learn(juce::Button &);
    void forget(juce::Button &);
    /** what is mapped to a button ("CC 20", "Note C3"), empty = nothing */
    juce::String mappingOf(juce::Button &) const;
    std::function<void()> onClose;

private:
    struct Item
    {
        juce::Button *button = nullptr;
        juce::Rectangle<int> area;
        juce::String label;
        bool waiting = false;
        bool operator==(const Item &o) const
        {
            return button == o.button && area == o.area && label == o.label &&
                   waiting == o.waiting;
        }
    };
    void collect(juce::Component &parent, juce::Rectangle<int> clip, std::vector<Item> &out) const;
    void describe(juce::Button &, juce::String &label, bool &waiting) const;
    int paramOf(juce::Button &) const;
    int itemAt(juce::Point<int>) const;
    void stopWaiting();

    RysumProcessor &proc;
    juce::Component &root;
    std::vector<Item> items;
    int hover = -1, waitingParamSlot = -1;
    juce::String note;
};
} // namespace rysum::ui

class RysumContent : public juce::Component, public rysum::ui::ModUiAccess
{
public:
    /** "MIDI-Tasten zuordnen" on / off (Ctrl+M) */
    void startButtonLearn() override;
    void stopButtonLearn();
    rysum::ui::ButtonLearnOverlay &buttonLearn()
    {
        return learnOverlay;
    }
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
    float automatedNorm(int index) override
    {
        return proc.getAutomatedNorm(index);
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
    /** (the snapshot tool) the stop the Raum page shows */
    void selectRaumStop(int s)
    {
        space.selectStop(s);
    }
    void tick();
    void showPatchMenu();
    void askResetEverything();
    /** "Konzert": play for listeners elsewhere, or listen to a player */
    void showConcertPanel();
    void stepPatch(int delta);
    void askSavePatch();
    /** set by the editor: switches the full-screen view on / off */
    std::function<void()> onToggleFullscreen;
    /** set by the editor: the language was switched (the editor builds the interface anew) */
    std::function<void()> onLanguageChanged;
    /** the page on screen (for showView) */
    int currentView() const
    {
        return current;
    }
    void setFullscreenShown(bool on)
    {
        fullButton.active = on;
        fullButton.setTooltip(on ? rysum::tr("Zur\xc3\xbc"
                                                          "ck ins Fenster (Esc)")
                                 : rysum::tr("Vollbild (F11)"));
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
    rysum::ui::WalkView walk{proc};        // view 8: the Saal in 3D, walked with WASD and the mouse
    rysum::ui::VerlaufView verlauf{proc}; // view 9: automation over time
    rysum::ui::ButtonLearnOverlay learnOverlay{proc, *this};
    bool immersive = false;     // the 3D view fills the whole window
    bool immersiveFull = false; // ... and went to the full screen for it
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
    static constexpr int kNumTabs = 7;
    /** the page a tab shows: tabs 0..5 are views 0..5, the seventh (Verlauf) is view 9 */
    static int tabView(int tab)
    {
        return tab < 6 ? tab : 9;
    }
    std::array<juce::TextButton, kNumTabs> tabs;
    juce::TextButton keysButton{"Tastatur"}, latchButton{"Halten"};
    std::array<int, 18> keyNoteDown{}; // MIDI note each computer key is playing (-1 = up)
    int keysVelocity = 100;
    void updatePlayButtons();
    FullscreenButton fullButton;
    LanguageButton langButton;
    juce::ComboBox outputChoice;
    juce::TextButton patchPrev{juce::String::fromUTF8("\xe2\x97\x80")},
        patchNext{juce::String::fromUTF8("\xe2\x96\xb6")}, patchName, patchSave{"Speichern"};
    int shownPatchVersion = -1;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> outputAttachment;
    int current = 0;
    bool speakerPageShown = false;
    juce::Image grainLayer, vignette;
    // performance readout (Ctrl+Shift+P): what a repaint costs, how often, how much of the window;
    // logged to a file
    struct PaintStats
    {
        bool on = false;
        double paintStart = 0.0, secondStart = 0.0, lastTick = 0.0;
        int paints = 0, ticks = 0;
        double paintMs = 0.0, paintMax = 0.0, area = 0.0, tickMax = 0.0;
        juce::String shown;
    } stats;
    void updateStats();
    void tickBody();

public:
    int paintsDone = 0; // (counted for the benchmark)
private:
    void cycleRenderer();

public:
    void startStats();
    /** RYSUM_IMMERSIVE_TEST=1: the immersive 3D view in the real window - does it stay on, does
        the hidden pointer turn the view and stay in the middle, does the window come back; the
        result goes to Documents/Rysum/immersive-test.txt, then the program ends */
    void runImmersiveTest();

private:
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
    std::unique_ptr<RysumContent> content;
    juce::TooltipWindow tooltips{this, 600};
    void connectContent();
    /** the language changed: the interface is made anew, on the same page */
    void rebuildContent();

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
