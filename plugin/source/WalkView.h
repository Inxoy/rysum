#pragma once

#include "PluginProcessor.h"
#include "Theme.h"
#include "Walk3D.h"

namespace rysum::ui
{
/** "Begehen": the Saal in 3D, walked with WASD and the mouse. The camera is the listener: where you stand and
    where you look is what you hear (the pipes float where Ambisonics places them). A room model (OBJ) can be
    loaded; it is only seen - the sound keeps using the Saal's room. */
class WalkView : public juce::Component, private juce::Timer
{
public:
    explicit WalkView (RysumProcessor&);
    ~WalkView() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool) override;
    void focusLost (FocusChangeType) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    std::function<void()> onBack;
    /** for the tests: one step of the walk without a timer (dt in seconds, keys as if held) */
    void stepForTest (double dt, bool fwd, bool back, bool left, bool right, float turnDeg);
    /** tests: stand at an offset from the listener's place (held inside the walls like walking), looking along yaw */
    void setPoseForTest (float x, float y, float yawDeg) { px = x; py = y; yaw = yawDeg; pitch = 0.0f; keepInside(); pushPose(); }
    void loadModelForTest (const juce::File& f) { loadModel (f); }
    /** tests: a fixed resolution (no adapting to the frame time); how long the last frame took */
    void setTestMode (bool on) { testMode = on; if (on) { resScale = 1.0f; resLevel = 0; } }
    float frameMs() const noexcept { return lastFrameMs; }
    float geometryMs() const noexcept { return geoMs; }
    int renderWidth() const noexcept { return rast.width(); }
    juce::String renderStats() const { return "raster + shade " + juce::String (rast.rasterMs, 1) + " ms, " + juce::String (rast.workerCount()) + " workers, " + juce::String (rast.trianglesDrawn) + " triangles"; }

private:
    enum Slot { kSlotWall = 0, kSlotFloor, kSlotCeiling, kSlotModel, kSlotMetal, kSlotDark };
    struct RTri { Raster::Vert a, b, c; int slot; float tone; };
    void addPipe (V3 bottom, float len, float rad, float faceAngle, float tone, float glowAmt);
    V3 pipeAt (const RoomParams& r, V3 homeP, Pos pos) const;
    bool pipesInRoom = false;           // this frame: walking in a shaped Saal (see pipeAt)
    const RoomMesh* slotMesh = nullptr; // the model whose surfaces are sorted into floor / ceiling / walls
    std::vector<uint8_t> meshSlots;
    float resScale = 1.0f, lastFrameMs = 0.0f, geoMs = 0.0f;
    int resLevel = 0, slowFrames = 0, quickFrames = 0;
    uint32_t frame = 0;
    bool testMode = false;
    Material::Kind wallLook = Material::plain;
    void timerCallback() override;
    void step (double dt, bool fwd, bool back, bool left, bool right, float turn, bool fast);
    void pushPose();
    void keepInside();
    RoomParams roomNow() const;
    void buildRoomTris (const RoomParams& r, std::vector<RTri>& out) const;
    void loadModel (const juce::File& f);
    void chooseModel();
    void updateButtons();
    void drawMap (juce::Graphics& g, juce::Rectangle<float> area, const RoomParams& r, const EngineParams& params, V3 home);

    RysumProcessor& proc;
    Raster rast;
    std::vector<RTri> roomTris;
    float px = 0.0f, py = 0.0f, yaw = 0.0f, pitch = 0.0f, fov = 80.0f;    // pose (offset from the listener's place)
    juce::Point<float> lastMouse;
    bool looking = false;
    double lastTime = 0.0;
    std::array<std::array<float, kNumKeys>, kNumStops> glow {};
    juce::TextButton back { juce::String::fromUTF8 ("\xe2\x86\x90 Saal") }, load { juce::String::fromUTF8 ("Raummodell laden \xe2\x80\xa6") },
                     clear { "Modell entfernen" }, fit { juce::String::fromUTF8 ("Ma\xc3\x9f" "e \xc3\xbc" "bernehmen") },
                     zUp { "Z oben" }, home { "Zum Platz" };
    juce::ComboBox units;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Image glowSprite;
    juce::String status;
    double statusUntil = 0.0;
};
} // namespace rysum::ui
