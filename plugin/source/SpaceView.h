#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
/** top-down view of the sphere with every pipe as a dot */
class SpaceCircle : public juce::Component
{
public:
    SpaceCircle(RysumProcessor &p, int &selectedStop, bool &inputSelected)
        : proc(p), selected(selectedStop), inputSel(inputSelected)
    {
    }
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;

    std::function<void()> onUserMovedPipes;
    /** drawing a trajectory instead of moving pipes; the finished stroke goes here */
    bool drawMode = false;
    std::function<void(std::vector<RysumProcessor::DrawnPath::Pt>)> onPathDrawn;
    /** Verlauf: before a timetable mark is changed here (one undo step on the Verlauf page) */
    std::function<void()> onTimetableEdit;

private:
    // Verlauf: the timetable of the selected stop's path - its lane (nullptr = none), where along
    // the path a screen point is nearest (u 0..1, distance in pixels), the mark being dragged
    rysum::AutoLane *timetable() const;
    float nearestU(juce::Point<float> s, float &distPx) const;
    int dragMark = -1;
    std::vector<RysumProcessor::DrawnPath::Pt> stroke;
    double strokeStart = 0.0;
    juce::Point<float> lastStrokePos;
    juce::Point<float> toScreen(Pos p) const;
    Pos fromScreen(juce::Point<float> s) const;
    float radius() const
    {
        return (float)juce::jmin(getWidth(), getHeight()) * 0.5f - 34.0f;
    }
    juce::Point<float> centre() const
    {
        return getLocalBounds().toFloat().getCentre();
    }

    RysumProcessor &proc;
    int &selected;
    bool &inputSel;
    int dragKey = -1;
    // the audio input: where its sources are drawn, dragging them
    int inputPositions(std::array<Pos, 2> &out) const;
    bool draggingInput = false;
    float inDragAngle = 0.0f, inDragAz = 0.0f, inGrabOffset = 0.0f;
    bool rotating = false;
    float rotateStartAngle = 0.0f;
    std::array<Pos, kNumKeys> rotateStart{};
};

/** the symbol of the audio input: a ring with a dot (a microphone seen from above) */
inline void drawInputGlyph(juce::Graphics &g, juce::Point<float> c, float size, bool on,
                           const juce::String &label = {})
{
    const auto colr = on ? col::rubric.brighter(0.45f) : col::faint;
    g.setColour(colr);
    g.drawEllipse(juce::Rectangle<float>(size, size).withCentre(c), 1.4f);
    g.fillEllipse(juce::Rectangle<float>(size * 0.36f, size * 0.36f).withCentre(c));
    if (label.isNotEmpty())
    {
        g.setFont(Assets::get().monoFont(9.5f));
        g.drawText(label,
                   juce::Rectangle<float>(12.0f, 10.0f)
                       .withCentre(c.translated(size * 0.5f + 6.0f, -size * 0.5f - 2.0f)),
                   juce::Justification::centred);
    }
}

/** the input's row in the list (under the stops) */
class InputRow : public juce::Button
{
public:
    InputRow() : juce::Button("Eingang") {}
    void paintButton(juce::Graphics &g, bool over, bool) override
    {
        auto &A = Assets::get();
        auto r = getLocalBounds();
        const bool sel = getToggleState();
        if (sel)
        {
            g.setColour(col::ink);
            g.fillRect(r.withTrimmedLeft(18));
        }
        else if (over)
        {
            g.setColour(col::line);
            g.fillRect(r.withTrimmedLeft(18));
        }
        drawInputGlyph(g, {5.0f, (float)r.getCentreY()}, 9.0f, on);
        g.setColour(sel ? col::bg : (on ? col::ink : col::faint));
        g.setFont(A.sansFont(14.0f));
        g.drawText(juce::String::fromUTF8("Eingang (Audio)"), r.withTrimmedLeft(28),
                   juce::Justification::centredLeft);
        g.setColour(sel ? col::bg : col::mute);
        g.setFont(A.monoFont(11.0f));
        g.drawText(status, r.withTrimmedRight(8), juce::Justification::centredRight);
    }
    juce::String status;
    bool on = false;
};

/** the 41 pipes of a stop: on / muted (click, drag across; right-click: patterns) */
class PipeStrip : public juce::Component, public juce::SettableTooltipClient
{
public:
    PipeStrip(RysumProcessor &p, int &selectedStop) : proc(p), stop(selectedStop)
    {
        setTooltip(juce::String::fromUTF8(
            "Pfeifen des Registers: Klick schaltet eine Pfeife stumm / wieder an, \xc3\xbc"
            "ber mehrere ziehen. "
            "Rechtsklick: Muster. Stumme Pfeifen spielen nicht (beim Verteilen der T\xc3\xb6ne "
            "werden sie \xc3\xbc"
            "bersprungen)."));
    }
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;

private:
    int keyAt(float x) const;
    RysumProcessor &proc;
    int &stop;
    bool paintMuted = false;
    int lastKey = -1;
};

/** small square that pulls / pushes a stop (same look as on the Spieltisch) */
class PullToggle : public juce::Button
{
public:
    PullToggle() : juce::Button("ziehen")
    {
        setClickingTogglesState(true);
        setTooltip(juce::String::fromUTF8("Register ziehen / absto\xc3\x9f"
                                          "en"));
    }
    void paintButton(juce::Graphics &g, bool over, bool) override
    {
        auto sq =
            juce::Rectangle<float>(9.0f, 9.0f).withCentre(getLocalBounds().toFloat().getCentre());
        g.setColour(getToggleState() ? col::ink : (over ? col::dim : col::faint));
        if (getToggleState())
            g.fillRect(sq);
        else
            g.drawRect(sq, 1.0f);
    }
};

/** a row of the stop list: dot, name, current motion */
class StopRow : public juce::Button
{
public:
    StopRow(int s) : juce::Button(prettyLabel(kStops[(size_t)s].label)), stop(s) {}
    void paintButton(juce::Graphics &g, bool over, bool) override
    {
        auto &A = Assets::get();
        auto r = getLocalBounds();
        const bool sel = getToggleState();
        if (sel)
        {
            g.setColour(col::ink);
            g.fillRect(r.withTrimmedLeft(18));
        }
        else if (over)
        {
            g.setColour(col::line);
            g.fillRect(r.withTrimmedLeft(18));
        }
        drawStopDot(g, stop, {5.0f, (float)r.getCentreY()}, 8.0f, false);
        g.setColour(sel ? col::bg : (pulled ? col::ink : col::faint));
        g.setFont(A.sansFont(14.0f));
        g.drawText(getButtonText(), r.withTrimmedLeft(28), juce::Justification::centredLeft);
        g.setColour(sel ? col::bg : col::mute);
        g.setFont(A.monoFont(11.0f));
        g.drawText(motion, r.withTrimmedRight(8), juce::Justification::centredRight);
    }
    juce::String motion;
    bool pulled = true;

private:
    int stop;
};

class SpaceView : public juce::Component
{
public:
    explicit SpaceView(RysumProcessor &);
    void paint(juce::Graphics &) override;
    void resized() override;
    void refresh();
    void selectStop(int s);
    void selectInput();
    /** Verlauf: the stop's timetable lane is wanted (created by the Verlauf page; exists = it is
        about to be switched on / off - an undo step) */
    std::function<void(int stop, bool exists)> onTimetable;
    /** before a timetable mark is changed on the plan */
    void setTimetableEditHook(std::function<void()> f)
    {
        circle.onTimetableEdit = std::move(f);
    }

private:
    void updateVisibility();
    void paintInput(juce::Graphics &);
    void layoutChanged();
    void syncLayoutControls();

    RysumProcessor &proc;
    int selected = 6;
    bool inputSel = false;
    SpaceCircle circle;
    PipeStrip pipeStrip{proc, selected};
    // the audio input
    InputRow inputRow;
    PullToggle inputPull;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> inputPullAttachment,
        inGateAttachment;
    juce::TextButton inMono{"Mono"}, inStereo{"Stereo"},
        inGate{juce::String::fromUTF8("Tasten \xc3\xb6"
                                      "ffnen")};
    std::unique_ptr<juce::ParameterAttachment> inModeAttachment;
    juce::ComboBox inFollow;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> inFollowAttachment;
    std::unique_ptr<ParamRow> inLevel, inSend, inAz, inEl, inDist, inWidth, inSpin, inRelease;
    juce::Rectangle<int> inputBox, inMeter, inHelp, pipeStripLabel;
    float inMeterShown = 0.0f;
    std::vector<std::unique_ptr<StopRow>> stopButtons;
    std::vector<std::unique_ptr<PullToggle>> pullButtons;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>>
        pullAttachments;
    std::array<juce::TextButton, 7> layoutButtons;
    std::array<juce::TextButton, 13>
        motionButtons; // (8..12: the motions in the Saal, a row of their own)
    juce::Rectangle<int> motionSaalLabel;
    juce::TextButton drawButton{juce::String::fromUTF8("Bahn zeichnen")};
    juce::TextButton timetableButton{"Fahrplan"};
    juce::TextButton pathLoop{"Schleife"},
        pathPing{juce::String::fromUTF8("Hin & zur\xc3\xbc"
                                        "ck")},
        pathEven{juce::String::fromUTF8("gleichm\xc3\xa4\xc3\x9fig")}, pathDrawnT{"Zeichentempo"},
        jumpHard{juce::String::fromUTF8("harte Spr\xc3\xbcnge")};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> jumpHardAttachment;
    std::array<juce::TextButton, 4> ellForm;
    std::unique_ptr<juce::ParameterAttachment> ellFormAttachment;
    std::unique_ptr<ParamRow> spreadRow, ellDirRow, ellTiltRow;
    juce::Rectangle<int> ellPreview;
    juce::TextButton tabMotion{"Bewegung"}, tabHall{"Hall"}, tabCloud{"Wolke"}, tabPipe{"Pfeife"};
    bool hallTab = false, cloudTab = false, pipeTab = false;
    void showTab(int tab); // 0 = Bewegung, 1 = Hall, 2 = Wolke, 3 = Pfeife
    // the sound of the selected stop's pipes: Ansprache, Abriss, upper partials
    std::array<std::unique_ptr<ParamRow>, 8> pipeRows;
    juce::TextButton pipeAll{juce::String::fromUTF8("f\xc3\xbcr alle Register \xc3\xbc"
                                                    "bernehmen")};
    // grain cloud of the selected stop
    std::array<std::unique_ptr<ParamRow>, 5> cloudRows;
    juce::TextButton cloudAll{juce::String::fromUTF8("f\xc3\xbcr alle Register \xc3\xbc"
                                                     "bernehmen")};
    juce::Rectangle<int> cloudNote;
    int shownGrains = -1;
    juce::Slider width, centre, elevation, spread, distance;
    std::unique_ptr<ParamRow> rateRow, depthRow, ellipseRow, irDry, irWet, irEnvelop, irDirect;
    juce::ComboBox irOrder;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> irOrderAttachment;
    juce::TextButton irLoad{juce::String::fromUTF8("IR laden \xe2\x80\xa6")}, irClear{"x"},
        irFuma{"FuMa"}, irN3D{"N3D"}, irFollow{juce::String::fromUTF8("folgt Pfeifen")};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> irFumaAttachment,
        irN3DAttachment, irFollowAttachment;
    std::unique_ptr<juce::FileChooser> chooser;
    std::function<void()> chooseIrFile;
    juce::Rectangle<int> left, right, stopsBox, layoutBox, motionBox, hallBox, tabBox;
    bool updating = false;
};
} // namespace rysum::ui
