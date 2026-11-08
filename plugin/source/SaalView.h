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
    SaalPlan (RysumProcessor& p, int& editRoom) : proc (p), shownRoom (editRoom) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    /** copy the shown plan into the drawn one and switch to it */
    void takeOverPlan();

private:
    struct Frame { float scale = 10.0f; juce::Point<float> origin; };          // room metres -> screen
    Frame frame() const;
    juce::Point<float> toScreen (const Frame& f, float x, float y) const { return { f.origin.x + x * f.scale, f.origin.y - y * f.scale }; }
    void setListener (juce::Point<float> screen);

    RysumProcessor& proc;
    int& shownRoom;                              // the room shown (0..3)
    bool dragging = false;
    int dragVertex = -1, hoverVertex = -1;
    RoomParams heardNow() const;
    bool drawing() const;                        // the shown room has the drawn plan: its corners can be edited
    int vertexAt (juce::Point<float> screen) const;
    juce::Point<float> toPlan (juce::Point<float> screen) const;   // screen -> fractions of width / length
    void setShape (int shape);
    std::vector<PlanPath> paths = std::vector<PlanPath> (1024);
    std::vector<PlanImage> cand = std::vector<PlanImage> (512);
};

/** the floor plans as small pictures to choose from */
class ShapeStrip : public juce::Component
{
public:
    ShapeStrip (RysumProcessor& p, int& editRoom);
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh();

private:
    struct Icon : public juce::Button
    {
        Icon (ShapeStrip& o, int s) : juce::Button ({}), owner (o), shape (s) {}
        void paintButton (juce::Graphics&, bool over, bool down) override;
        ShapeStrip& owner;
        int shape;
    };
    RysumProcessor& proc;
    int& shownRoom;
    std::vector<std::unique_ptr<Icon>> icons;
    int shownShape = -1, shownRoomIdx = -1;
    float shownAmt = -1.0f;
    int currentShape() const;
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
    int editRoom = 0;
    SaalPlan plan { proc, editRoom };
    ShapeStrip shapes { proc, editRoom };
    std::array<ParamRow*, rysum::kNumRooms> formR {};
    // several rooms: which one is edited / shown, and which stop sounds in which room
    std::array<juce::TextButton, rysum::kNumRooms> roomTabs;
    std::array<juce::TextButton, rysum::kNumStops + 1> assign;          // stops in console order, then the input
    std::array<std::array<ParamRow*, 4>, rysum::kNumRooms> dimsR {}, surfR {};
    std::array<std::array<ParamRow*, 2>, rysum::kNumRooms> lisR {};
    std::array<ParamRow*, 2> gainRows {};
    juce::Rectangle<int> roomsArea;
    std::array<int, rysum::kNumStops + 1> shownAssign {};
    void selectRoom (int r);
    juce::String assignId (int slot) const;
    void showAssignMenu (int slot);
    juce::TextButton onButton { "Saal an" };
    std::unique_ptr<ButtonAtt> onAtt;
    juce::ComboBox order;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> orderAtt;
    std::vector<std::unique_ptr<juce::TextButton>> roomPresets, materials;
    std::vector<std::unique_ptr<ParamRow>> rows;
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
    // ... as a piece: record, then it plays by itself
    juce::TextButton lucModePiece { juce::String::fromUTF8 ("St\xc3\xbc" "ck (Aufnahme)") }, lucModeLive { "Live" };
    std::unique_ptr<juce::ParameterAttachment> lucModeAtt;
    juce::TextButton lucRec { "Aufnehmen" }, lucPlay { juce::String::fromUTF8 ("\xe2\x96\xb6 Abspielen") },
                     lucStop { juce::String::fromUTF8 ("\xe2\x96\xa0") }, lucLoad { juce::String::fromUTF8 ("Aufnahme laden \xe2\x80\xa6") },
                     lucExport { juce::String::fromUTF8 ("St\xc3\xbc" "ck als WAV \xe2\x80\xa6") };
    juce::ComboBox lucRecSrc;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> lucRecSrcAtt;
    std::array<ParamRow*, 3> pieceRows {};
    std::unique_ptr<juce::FileChooser> lucChooser;
    juce::Rectangle<int> lucStatusArea;
    juce::String shownPieceStatus;
    bool pieceMode() const;
    void updateLucierMode();
    juce::String pieceStatus() const;
};
} // namespace rysum::ui
