#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
class VerlaufView;

/** a slider with its title and value above it, for values that are not plugin parameters */
class FreeRow : public juce::Component
{
public:
    FreeRow(const juce::String &title, double lo, double hi, double skewMid = 0.0);
    void paint(juce::Graphics &) override;
    void resized() override;
    juce::Slider slider;
    std::function<juce::String(double)> format;
    std::function<void(double)> onChange;    // while dragging
    std::function<void()> onDragStart;       // (undo point)

private:
    juce::String title;
};

/** the lanes over time: ruler on top, a header per lane on the left, the curves on the right */
class VerlaufTimeline : public juce::Component
{
public:
    explicit VerlaufTimeline(VerlaufView &o) : owner(o) {}
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseMove(const juce::MouseEvent &) override;
    void mouseExit(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

    static constexpr int kHeaderW = 214, kRulerH = 26;
    int scrollY = 0;

    // geometry
    float xOfBeat(double b) const;
    double beatOfX(float x) const;
    juce::Rectangle<int> laneRect(int lane) const; // in this component, scrolled
    int laneAt(int y) const;
    int contentHeight() const;

private:
    friend class VerlaufView;
    VerlaufView &owner;
    enum class Drag
    {
        none,
        point,
        bend,
        pencil,
        stairs,
        seek,
        pan
    } drag = Drag::none;
    int dragLane = -1, dragPoint = -1;
    float bendStart = 0.0f;
    std::vector<AutoPoint> stroke; // pencil
    double lastStairCell = std::numeric_limits<double>::quiet_NaN();
    double panStart = 0.0;
    juce::Point<float> hover{-1.0f, -1.0f};
    int hoverLane = -1, hoverPoint = -1;

    void paintRuler(juce::Graphics &, juce::Rectangle<int>);
    void paintLane(juce::Graphics &, int lane, juce::Rectangle<int>);
    // header buttons of a lane: on, armed, delete
    juce::Rectangle<int> headerButton(juce::Rectangle<int> lane, int which) const;
    juce::Rectangle<int> curveArea(juce::Rectangle<int> lane) const;
    float yOfValue(juce::Rectangle<int> curve, float v) const;
    float valueOfY(juce::Rectangle<int> curve, float y) const;
    int pointAt(int lane, juce::Point<float> p) const;
    double snap(double beat, bool fine) const;
};

/** the "Verlauf" page: automation over time, lane by lane */
class VerlaufView : public juce::Component, private juce::Timer
{
public:
    explicit VerlaufView(RysumProcessor &);
    ~VerlaufView() override;
    void paint(juce::Graphics &) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress &) override;
    void visibilityChanged() override;

    /** adds a lane for a parameter (or selects the one there is) */
    void addLaneFor(const juce::String &paramId);

    // lane geometry helpers (lane time <-> clock beats)
    static double localOf(const AutoLane &, double beat);
    static double beatOfLocal(const AutoLane &, double t);

private:
    friend class VerlaufTimeline;
    RysumProcessor &proc;
    AutoSet &set()
    {
        return proc.getAuto();
    }
    void timerCallback() override;
    void changed(); // after an edit: to the audio, redraw
    void pushUndo();
    void restore(const AutoSet &from);
    void undo();
    void redo();
    void syncControls();
    void showAddMenu();
    void showLaneMenu(int lane);
    void showPointMenu(int lane, int point);
    juce::String timeText(double beats) const;
    juce::String laneName(int lane) const;
    juce::String valueText(int lane, float v) const;
    int laneParam(int lane) const;
    bool validLane(int l) const
    {
        return l >= 0 && l < (int)proc.getAuto().lanes.size();
    }

    enum Tool
    {
        toolPoints = 0,
        toolPencil,
        toolStairs
    };
    int tool = toolPoints;
    int selLane = -1, selPoint = -1;
    std::vector<AutoSet> undoStack, redoStack;
    int shownVersion = -1;
    double shownNow = -1.0;
    bool updating = false;

    juce::Rectangle<int> titleBox, laneBox, segBox;
    juce::TextButton onButton{"Verlauf an"}, clockHost{"Host"}, clockOwn{"Eigene Uhr"},
        playButton{juce::String::fromUTF8("\xe2\x96\xb6")},
        stopButton{juce::String::fromUTF8("\xe2\x96\xa0")},
        homeButton{juce::String::fromUTF8("|\xe2\x97\x80")}, keyStart{"Erste Taste"},
        recButton{juce::String::fromUTF8("\xe2\x97\x8f Aufnahme")}, toolPts{"Punkte"},
        toolPen{"Stift"}, toolStair{"Treppe"}, zoomIn{"+"}, zoomOut{juce::String::fromUTF8("\xe2\x88\x92")},
        zoomAll{"Alles"}, undoButton{juce::String::fromUTF8("Zur\xc3\xbc"
                                                            "ck")},
        addButton{"+ Spur"};
    juce::ComboBox gridBox;
    FreeRow tempoRow{"Tempo", 10.0, 400.0, 90.0};
    // lane
    FreeRow lenRow{juce::String::fromUTF8("L\xc3\xa4nge"), 0.25, 256.0, 16.0},
        speedRow{"Dehnung", 0.0625, 16.0, 1.0}, offsetRow{"Versatz", 0.0, 256.0, 16.0};
    juce::TextButton loopButton{"Schleife"},
        revButton{juce::String::fromUTF8("R\xc3\xbc"
                                         "ckw\xc3\xa4rts")},
        clearButton{"Leeren"};
    // segment
    std::array<juce::TextButton, kNumAutoShapes> shapeButtons;
    FreeRow bendRow{"Biegung", -1.0, 1.0}, rateRow{"Tempo", 0.05, 32.0, 2.0},
        depthRow{"Tiefe", 0.0, 1.0};
    VerlaufTimeline timeline{*this};
};
} // namespace rysum::ui
