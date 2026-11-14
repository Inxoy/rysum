#pragma once

#include "PluginProcessor.h"
#include "Theme.h"
#include "Walk3D.h"

namespace rysum::ui
{
/** "Begehen": the Saal in 3D, walked with WASD and the mouse (and a jump). The camera is the listener: where you
    stand and where you look is what you hear (the pipes float where Ambisonics places them). A room model (OBJ)
    can be loaded: it is seen, its walls stop you and its floors carry you, and - unless switched off - it is heard
    (pipes behind its walls come around them, the late reverb is ray-traced through it). The Saal's shoebox stays
    underneath: its floor and ceiling close an open model, it gives the early reflections and the size.
    Every surface of the Saal can be of its own material: point at it and press M.
    The player's hands: E a shockwave, Q (held) carries the pipe under the cross and throws it, F (held) holds
    everything still, R calls the pipes home, C claps, a click knocks on a pipe. */
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
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    std::function<void()> onBack;
    /** "Immersiv": only the picture - no buttons, no writing, no map - over the whole window, and the mouse looks
        around without a button held (the pointer is hidden and kept in the view). Esc, I or losing the keyboard
        leaves it. */
    void setImmersive (bool on);
    bool isImmersive() const noexcept { return immersive; }
    void toggleImmersive();
    /** set by the editor: makes room before the immersive view starts (the full screen); true = the window
        changed, the view starts a moment later */
    std::function<bool()> prepareImmersive;
    /** set by the editor: the immersive view went on / off (it fills the whole window while on) */
    std::function<void (bool)> onImmersive;
    /** for the tests: one step of the walk without a timer (dt in seconds, keys as if held) */
    void stepForTest (double dt, bool fwd, bool back, bool left, bool right, float turnDeg);
    /** tests: stand at an offset from the listener's place (held inside the walls like walking), looking along yaw */
    void setPoseForTest (float x, float y, float yawDeg) { px = x; py = y; yaw = yawT = yawDeg; pitch = pitchT = 0.0f; vx = vy = vz = 0.0f; keepInside(); pushPose(); }
    void setPitchForTest (float pitchDeg) { pitch = pitchT = pitchDeg; pushPose(); }
    /** tests: the smooth walk of the timer for dt seconds with these keys held */
    void advanceForTest (double dt, bool fwd, bool bwd, bool left, bool right, bool jump, bool fast = false) { advance (dt, fwd, bwd, left, right, 0.0f, fast, jump); }
    float heightForTest() const noexcept { return fz; }
    float lookYawForTest() const noexcept { return yawT; }
    /** tests: the surface the cross points at (-1 = none), and M */
    int aimForTest() const noexcept { return aimSurf; }
    void cycleMaterialForTest() { cycleMaterial(); }
    void loadModelForTest (const juce::File& f) { loadModel (f); }
    // ---- the player's actions (the keys; also for the tests)
    /** E: a burst that pushes every pipe away from you (strength 1 = the usual one) */
    void shock (float strength = 1.0f);
    /** R: every pipe flies back to its place in the organ */
    void callHome();
    /** C: a hand clap where you stand */
    void clap();
    /** a click: the pipe at this point of the view is knocked and rings; false = no pipe there */
    bool strikeAt (juce::Point<float> at, float strength = 1.0f);
    /** Q down: take the pipe under the cross; Q up: throw it (fast = harder) */
    void setGrab (bool on, bool fast = false);
    /** F: while on, everything that moves stands still */
    void setFreeze (bool on);
    /** tests: what the timer does for the actions (the hand's place, a pipe taken once its stop hovers) */
    void updateActionsForTest() { updateActions (heldStop >= 0, frozen); }
    /** tests: the pipe under the cross (-1 = none), from the last frame drawn */
    int aimStopForTest() const noexcept { return aimStop; }
    int aimKeyForTest() const noexcept { return aimKey; }
    /** tests: a fixed resolution (no adapting to the frame time); how long the last frame took */
    void setTestMode (bool on) { testMode = on; if (on) { resScale = 1.0f; resLevel = 0; } }
    float frameMs() const noexcept { return lastFrameMs; }
    float geometryMs() const noexcept { return geoMs; }
    int renderWidth() const noexcept { return rast.width(); }
    juce::String renderStats() const { return "raster + shade " + juce::String (rast.rasterMs, 1) + " ms, " + juce::String (rast.workerCount()) + " workers, " + juce::String (rast.trianglesDrawn) + " triangles"; }

private:
    enum Slot { kSlotWall = 0, kSlotFloor, kSlotCeiling, kSlotModel, kSlotMetal, kSlotDark, kSlotWall0 = 8 };   // (kSlotWall0 + i: wall i, each with its own material)
    struct RTri { Raster::Vert a, b, c; int slot; float tone; };
    void addPipe (V3 bottom, float len, float rad, float faceAngle, float tone, float glowAmt);
    V3 pipeAt (const RoomParams& r, V3 homeP, Pos pos, int stop, int key) const;
    uint32_t flashSeen = 0;                                  // impacts already flashed
    struct Flash { V3 at; float life = 0.0f, strength = 0.0f; };
    std::array<Flash, 16> flashes {};
    bool pipesInRoom = false;           // this frame: walking in a shaped Saal (see pipeAt)
    const RoomMesh* slotMesh = nullptr; // the model whose surfaces are sorted into floor / ceiling / walls
    std::vector<uint8_t> meshSlots;
    float resScale = 1.0f, lastFrameMs = 0.0f, geoMs = 0.0f;
    int resLevel = 0, slowFrames = 0, quickFrames = 0;
    uint32_t frame = 0;
    bool testMode = false;
    Material::Kind wallLook = Material::plain;
    void timerCallback() override;
    void step (double dt, bool fwd, bool back, bool left, bool right, float turn, bool fast);            // (at once: the tests)
    void advance (double dt, bool fwd, bool back, bool left, bool right, float turnKeys, bool fast, bool jump);   // (smooth: the timer)
    void moveBy (float dx, float dy);
    float groundAt (float x, float y) const;
    int surfaceAhead (const RoomParams& r, V3 eye) const;
    void cycleMaterial();
    juce::String surfaceName (const RoomParams& r, int surf) const;
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
    // moving: speeds that build up and die away instead of starting and stopping at once; the mouse sets where to
    // look and the view follows within a few hundredths of a second; a jump (Space)
    float vx = 0.0f, vy = 0.0f, vz = 0.0f, fz = 0.0f;         // m/s; the feet above the Saal's floor (m)
    float yawT = 0.0f, pitchT = 0.0f, turnV = 0.0f;            // where to look; turning speed from the keys (degrees/s)
    float bobPhase = 0.0f, bobAmt = 0.0f, landDip = 0.0f;      // the steps, and giving in the knees after a jump
    bool onGround = true;
    int aimSurf = -1;                                          // the Saal's surface under the cross (kSurfFloor ...)
    // the player's actions
    struct Shown { V3 c; float len = 0.0f, rad = 0.0f; bool on = false; };
    std::array<std::array<Shown, kNumKeys>, kNumStops> shown {};   // the pipes as the last frame drew them
    float viewScale = 1.0f;                                    // component pixels per pixel of the picture
    int aimStop = -1, aimKey = -1;                             // the pipe under the cross
    int heldStop = -1, heldKey = -1;                           // the pipe in the hand (or about to be)
    float holdDist = 1.8f;
    bool qWas = false, frozen = false;
    double shockAt = 0.0, lastShock = 0.0, lastClap = 0.0, shockFx = 0.0;
    float shockAmt = 1.0f, dragged = 0.0f;
    juce::Point<float> downPos;
    bool pickPipe (juce::Point<float> at, float maxDist, int& stop, int& key) const;
    bool makeHover (int stop, bool tell);
    void updateActions (bool qDown, bool fDown);
    void say (const juce::String& text, double ms = 2500.0);
    V3 eyeNow() const;
    V3 lookDir() const;
    juce::Point<float> lastMouse;
    bool looking = false;
    double lastTime = 0.0;
    std::array<std::array<float, kNumKeys>, kNumStops> glow {};
    juce::TextButton back { juce::String::fromUTF8 ("\xe2\x86\x90 Saal") }, load { juce::String::fromUTF8 ("Raummodell laden \xe2\x80\xa6") },
                     clear { "Modell entfernen" }, fit { juce::String::fromUTF8 ("Ma\xc3\x9f" "e \xc3\xbc" "bernehmen") },
                     zUp { "Z oben" }, home { "Zum Platz" };
    // the player's hands as buttons (what the keys do; a MIDI button can be mapped to each)
    juce::TextButton immerse { "Immersiv" }, actShock { "E Druckwelle" }, actHome { juce::String::fromUTF8 ("R Zur\xc3\xbc" "ckrufen") },
                     actClap { "C Klatschen" }, actMat { "M Material" };
    HoldButton actGrab { "Q Greifen" }, actFreeze { "F Anhalten" }, actJump { "Springen" };
    juce::Rectangle<int> handsArea;
    bool immersive = false, lockFresh = false;
    int unfocused = 0;
    juce::Point<float> lockRef;
    void pollLockedMouse();
    juce::TextButton hear {
 juce::String::fromUTF8 ("Modell h\xc3\xb6rbar") };
    juce::ComboBox units, playerMode;
    juce::Slider playerForce;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> playerModeAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> playerForceAtt;
    std::unique_ptr<juce::FileChooser> chooser;
    // the illusions: the room plays itself, pipes grow and break, a pipe at the ear, the far end plays the past,
    // a room heard larger than it is seen
    std::array<std::unique_ptr<ParamRow>, 6> illusions;
    juce::Image glowSprite;
    juce::String status;
    double statusUntil = 0.0;
};
} // namespace rysum::ui
