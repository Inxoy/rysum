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

/** a spreading lane's origin, from above (front up, the listener in the middle, 20 m to the
    edge): the pipes of the pulled stops, the origin (drag it; double-click: at the listener) and
    the fronts of the gesture running through the room */
class OriginPad : public juce::Component
{
public:
    explicit OriginPad(VerlaufView &o) : owner(o) {}
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    static constexpr float kRange = 20.0f;

private:
    VerlaufView &owner;
    juce::Point<float> toPad(float x, float y) const;
    juce::Point<float> fromPad(juce::Point<float>) const;
};

/** the lanes over time: ruler on top, a header per lane on the left, the curves on the right */
class VerlaufTimeline : public juce::Component, public juce::TooltipClient
{
public:
    explicit VerlaufTimeline(VerlaufView &o) : owner(o) {}
    juce::String getTooltip() override;
    /** what the mouse can do where it is now (the hint line under the lanes) */
    juce::String hintText() const;
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseMove(const juce::MouseEvent &) override;
    void mouseExit(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

    // the ruler: a band for markers and the repeated section on top, the bars below
    static constexpr int kHeaderW = 214, kMarkH = 15, kRulerH = 41;
    int scrollY = 0;

    // geometry
    float xOfBeat(double b) const;
    double beatOfX(float x) const;
    juce::Rectangle<int> laneRect(int lane) const; // in this component, scrolled
    int laneAt(int y) const;
    int contentHeight() const;
    int laneHeight(int lane) const;
    static constexpr int kMinLaneH = 30, kMaxLaneH = 400;

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
        pan,
        box,    // pressed on empty space: a click sets a point, a drag draws a frame
        line,   // pressed on the line: a click chooses the section, a drag bends it
        resize, // the lane's lower edge
        marker, // a marker: a click jumps there, a drag moves it
        cycleA, // an end of the repeated section
        cycleB,
        cycleNew // pressed in the marker band: a click sets the clock, a drag the section
    } drag = Drag::none;
    int dragMarker = -1;
    double markerStart = 0.0;
    /** the marker whose flag is at x (in the band), -1 = none */
    int markerAt(float x) const;
    juce::Rectangle<float> markerFlag(int i) const;
    int dragLane = -1, dragPoint = -1;
    float bendStart = 0.0f;
    bool dragMoved = false, undoPushed = false;
    juce::Point<float> downPos;
    std::vector<AutoPoint> dragOrig; // the lane's points when a move began
    std::vector<int> boxBase;        // selection before the frame (Shift: the frame adds to it)
    int resizeStart = 0;
    void ensureUndo();               // the undo step of a drag, at its first change
    void addPointAt(int lane, juce::Point<float> p, bool fine);
    /** the section (index of its first point) whose drawn line passes near p, -1 = none */
    int lineAt(int lane, juce::Point<float> p) const;
    bool onLowerEdge(int lane, int y) const;
    enum class Hover
    {
        none,
        ruler,
        band,
        marker,
        cycleEdge,
        header,
        headerButton,
        edge,
        point,
        line,
        empty
    };
    Hover hoverKind() const;
    std::vector<AutoPoint> stroke; // pencil
    double lastStairCell = std::numeric_limits<double>::quiet_NaN();
    double panStart = 0.0;
    juce::Point<float> hover{-1.0f, -1.0f};
    int hoverLane = -1, hoverPoint = -1;

    void paintRuler(juce::Graphics &, juce::Rectangle<int>);
    void paintLane(juce::Graphics &, int lane, juce::Rectangle<int>);
    // header buttons of a lane: on, armed, menu
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
    friend class OriginPad;
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

public:
    /** a clock time as the ruler shows it: bars.beats (host) or m:ss (own clock) */
    static juce::String clockText(const RysumProcessor &, double beats);
    /** an edit from another page (the Raum page's timetable marks): one undo step */
    void noteExternalEdit()
    {
        pushUndo();
    }

private:
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
    // several points of the chosen lane (frame, Shift + click, Ctrl + A): moved, copied, deleted
    // together
    std::vector<int> selPts;
    bool isSelected(int point) const
    {
        return std::find(selPts.begin(), selPts.end(), point) != selPts.end();
    }
    void clearSelection()
    {
        selPts.clear();
    }
    // markers and the repeated section
    void addMarkerAt(double beat);
    void renameMarker(int i);
    void showMarkerMenu(int i, double beat);
    void jumpMarker(int direction); // to the previous (-1) / next (+1) marker
    void sortMarkers();
    juce::TextButton cycleButton{juce::String::fromUTF8("A\xe2\x80\x93"
                                                        "B")};
    // the tempo lane: seconds at a clock time (the ruler's second line), cached per version
    double secondsAt(double beats) const;
    juce::String secondsText(double beats) const;
    const AutoLane *tempoLane() const;
    mutable std::vector<double> secCache;
    mutable int secCacheVersion = -1;
    // editing tools
    juce::PopupMenu paramMenu(int base) const; // every control a lane can drive
    void transformPoints(int kind);            // the selection (or all points of the lane)
    void fillLane(int kind);                   // a pattern into the loop / the visible range
    void copyLaneTo(const juce::String &paramId);
    std::vector<AutoPoint> clip; // copied points, times from the first one
    void copySelection(bool cut);
    void paste();
    void deleteSelection();
    juce::Rectangle<int> hintBox;
    void hintChanged()
    {
        repaint(hintBox);
    }
    std::vector<AutoSet> undoStack, redoStack;
    int shownVersion = -1;
    double shownNow = -1.0;
    bool updating = false;

    // pieces: new, open, save, the ones that come with Rysum, one's own
    juce::TextButton pieceButton{juce::String::fromUTF8("St\xc3\xbc"
                                                        "ck \xe2\x96\xbe")};
    std::unique_ptr<juce::FileChooser> chooser;
    void showPieceMenu();
    /** runs 'then' once unsaved changes of the piece are saved or let go (asks first) */
    void whenPieceSafe(std::function<void()> then);
    void savePiece(std::function<void()> then = {});
    void savePieceAs(std::function<void()> then = {});
    void openPieceFile(const juce::File &f);
    void pieceLoaded(); // another piece is on the page: undo, selection, view start anew
    void pieceError(const juce::String &message);

public:
    /** the page's test hook: the piece menu's actions without the menu */
    void testPieceLoaded()
    {
        pieceLoaded();
    }
    void testFill(int kind)
    {
        fillLane(kind);
    }
    void testTransform(int kind)
    {
        transformPoints(kind);
    }

private:
    juce::Rectangle<int> titleBox, laneBox, segBox;
    juce::Array<int> barSeps; // x of the thin lines between the groups of the bar
    juce::TextButton onButton{"Verlauf an"}, clockHost{"Host"}, clockOwn{"Eigene Uhr"},
        playButton{juce::String::fromUTF8("\xe2\x96\xb6")},
        stopButton{juce::String::fromUTF8("\xe2\x96\xa0")},
        homeButton{juce::String::fromUTF8("|\xe2\x97\x80")}, keyStart{"Start mit Taste"},
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
    // spreading (per-pipe lanes)
    juce::TextButton spreadButton{"Ausbreitung"}, inwardButton{"nach innen"};
    FreeRow waveRow{"Geschwindigkeit der Front", 0.25, 100.0, 6.0};
    OriginPad originPad{*this};
    bool selectedIsPipeLane() const;
    // segment
    std::array<juce::TextButton, kNumAutoShapes> shapeButtons;
    FreeRow bendRow{"Biegung", -1.0, 1.0}, rateRow{"Tempo", 0.05, 32.0, 2.0},
        depthRow{"Tiefe", 0.0, 1.0};
    // variation from pass to pass
    FreeRow chanceRow{"Chance", 0.0, 1.0}, everyRow{juce::String::fromUTF8("Nur jedes n-te Mal"), 1.0, 8.0},
        varyRow{"Streuung", 0.0, 0.5};
    VerlaufTimeline timeline{*this};
};
} // namespace rysum::ui
