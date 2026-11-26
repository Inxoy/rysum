#pragma once

#include "PluginProcessor.h"
#include "Theme.h"
#include "Walk3D.h"
#include "WalkGpu.h"

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
    void mouseDoubleClick (const juce::MouseEvent&) override;

    std::function<void()> onBack;
    /** Konzert: listening to someone, one only walks and looks - the hands, the room model and the
        Saal's settings are the player's (on while listening; always in the listener app) */
    bool isSpectator() const noexcept;
    void setAlwaysSpectator (bool on) { alwaysSpectator = on; refreshSpectator(); }
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
    void loadBuiltInRoomForTest (int index) { loadBuiltInRoom (index); }
    void setDarkForTest (bool on) { setDark (on); }
    // ---- the player's actions (the keys; also for the tests)
    /** E: a burst that pushes every pipe away from you (strength 1 = the usual one) */
    void shock (float strength = 1.0f);
    /** R: every pipe flies back to its place in the organ */
    void callHome();
    /** "Alles zuruecksetzen": nothing chosen, pinned, paused or left behind; every pipe flies home, whole again */
    void resetView();
    /** C: a hand clap where you stand */
    void clap();
    /** a click: the pipe at this point of the view is knocked and rings; false = no pipe there */
    bool strikeAt (juce::Point<float> at, float strength = 1.0f);
    /** Q down: take the pipe under the cross; Q up: throw it (fast = harder) */
    void setGrab (bool on, bool fast = false);
    /** F: while on, everything that moves stands still */
    void setFreeze (bool on);
    // ---- arranging the pipes: choosing some, formations, layouts (F1 to F8), undo, the magnet, the sweep
    /** Ctrl+click: one pipe into the choice or out of it (toggle) / only into it */
    void selectPipe (int stop, int key, bool toggle = true);
    /** Ctrl+double-click: all pipes of a stop */
    void selectRegister (int stop);
    void clearSelection();
    int selectedCount() const noexcept;
    bool isSelected (int stop, int key) const noexcept { return stop >= 0 && stop < kNumStops && key >= 0 && key < kNumKeys && ((sel[(size_t) stop] >> key) & 1u) != 0u; }
    /** 1 row, 2 circle around you, 3 spiral, 4 grid, 5 scattered: the chosen pipes (none chosen: all of the
        pulled stops) fly there and stay; false = no pipe to move */
    bool formation (int kind);
    /** Ctrl+F1..F8 / F1..F8: where the pipes stand now, kept (with the project) / brought back */
    bool storeLayout (int slot);
    bool recallLayout (int slot);
    /** Ctrl+Z: the pipes back to where they stood before the last shock, throw, call, formation, layout or magnet */
    bool undo();
    int undoDepth() const noexcept { return (int) undoStack.size(); }
    /** the right mouse button held: +1 draws the pipes to a point ahead of you, -1 (Shift) drives them away, 0 off */
    void setMagnet (int mode);
    /** Alt+drag: every pipe the pointer passes from 'from' to 'to' is knocked (once); how many */
    int sweepTo (juce::Point<float> from, juce::Point<float> to, float strength);
    // ---- more hands: setting a pipe down, pinning, sounding a pipe, the whirlwind, time
    /** the right mouse button while a pipe is in the hand: it is set down where the cross points (on the floor,
        against a wall, or in the air at the hand's distance) and stays */
    bool setDown();
    /** the middle mouse button: the pipe stays where it is, whatever pushes it (again: free) */
    bool togglePin (int stop, int key);
    bool isPinned (int stop, int key) const noexcept;
    /** the mouse held on a pipe: it sounds as long as it is held */
    bool bowStart (int stop, int key);
    void bowStop();
    bool isBowing() const noexcept { return bowStop_ >= 0; }
    /** T: a whirlwind where the cross points, for a few seconds */
    void whirl (float strength = 1.0f);
    /** P: everything that moves stands still until P again; Z held: slow motion */
    void setPause (bool on);
    bool isPaused() const noexcept { return paused; }
    void setSlow (bool on);
    // ---- the room
    /** Ctrl+wheel: the Saal larger or smaller (its "Groesse"); false with a room model loaded */
    bool scaleRoom (float factor);
    /** Alt+M: every surface of the Saal gets the next material of the one under the cross */
    bool materialAll();
    /** K: a lantern where the cross points (the light off: it lights the room); Shift+K: the nearest one
        taken away; Ctrl+K: all of them */
    bool placeLantern();
    bool removeLantern();
    void clearLanterns();
    /** H: the ears stay where you stand now while you walk on and watch; H again: back */
    void leaveEars (bool on);
    bool earsLeft() const noexcept { return earsAway; }
    // ---- the controls of this computer (Documents/Rysum/Steuerung.xml): mouse speed, upside-down mouse, walking pace
    struct Controls { float mouse = 1.0f, walk = 1.0f; bool invertY = false; };
    static Controls controls();
    static void setControls (const Controls&);
    /** tests: hold Tab (all the keys shown) */
    void setHelpForTest (bool on) { helpHeld = on; repaint(); }
    /** tests: what the timer does for the actions (the hand's place, a pipe taken once its stop hovers) */
    void updateActionsForTest() { updateActions (heldStop >= 0, frozen); }
    /** tests: the pipe under the cross (-1 = none), from the last frame drawn */
    int aimStopForTest() const noexcept { return aimStop; }
    int aimKeyForTest() const noexcept { return aimKey; }
    /** tests: a fixed resolution (no adapting to the frame time); how long the last frame took */
    void setTestMode (bool on) { testMode = on; if (on) { resScale = 1.0f; resLevel = 0; } }
    float frameMs() const noexcept { return lastFrameMs; }
    float geometryMs() const noexcept { return geoMs; }
    juce::String renderSize() const { return juce::String (rast.width()) + " x " + juce::String (rast.height()) + rysum::tr(" (Stufe ") + juce::String (resLevel) + ")"; }
    int renderWidth() const noexcept { return rast.width(); }
    // ---- who draws the picture: the graphics card (OpenGL) or the processor
    /** the choice of this machine (Documents/Rysum/Grafik.xml; the environment variable RYSUM_GPU = 0 / 1
        overrides it). G in the view switches. */
    static bool gpuWanted();
    static void setGpuWanted (bool);
    /** the card is drawing right now */
    bool gpuOn() const noexcept { return gpu != nullptr && gpu->isReady() && ! gpu->hasFailed(); }
    /** while something of the window lies over the view (the card's picture is a window of its own and would
        cover it): the processor draws */
    void setGpuBlocked (bool b) { gpuBlocked = b; }
    /** "Grafikkarte (name), 0.4 ms" / "Prozessor ..." */
    juce::String pictureInfo() const;
    /** writing the window wants shown over the view while the card draws (the readout) */
    juce::String overText;
    juce::String renderStats() const { return "raster + shade " + juce::String (rast.rasterMs, 1) + " ms, " + juce::String (rast.workerCount()) + " workers, " + juce::String (rast.trianglesDrawn) + " triangles"; }

private:
    enum Slot { kSlotWall = 0, kSlotFloor, kSlotCeiling, kSlotModel, kSlotMetal, kSlotDark, kSlotWood, kSlotGlass, kSlotWall0 = 8 };   // (kSlotWall0 + i: wall i, each with its own material; glass: the Vox humana)
    /** the Vox humana: a glass pipe as long as the note it sings, wrapped in rings - its harmonics */
    void addVoxPipe (V3 bottom, float len, float rad, float faceAngle, float glowAmt, const float* rings);
    /** its eight voices at its place in the Saal (and their glows, as lamps) */
    void addVoxHumana (const RoomParams& r, V3 homeP, bool dark);
    struct VoxShown { float len = 0.6f, glow = 0.0f; std::array<float, 8> rings {}; };
    std::array<VoxShown, 8> voxShown {};
    float voxLevelMax = 1.0e-3f;
    struct RTri { Raster::Vert a, b, c; int slot; float tone; };
    void addPipe (V3 bottom, float len, float rad, float faceAngle, float tone, float glowAmt, int stop);
    void addFigure (V3 feet, float height, float yawDeg);      // Konzert: someone else in the Saal
    void addWoodPipe (V3 bottom, float len, float rad, float faceAngle, float glowAmt);
    void addReedPipe (V3 bottom, float len, float rad, float faceAngle, float tone, float glowAmt, float px);
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
    /** a door to show: an opening in this wall (its middle, as the engine hears the door), so wide and high */
    struct DoorCut { int wall = -1; float halfW = 0.7f, height = 2.2f; };
    void buildRoomTris (const RoomParams& r, std::vector<RTri>& out, const DoorCut& door = {}) const;
    /** the door's leaf (open as far as the door is) and the Saal behind it, dimmer */
    void addDoorView (const RoomParams& r, const DoorCut& door, float open, const RoomParams& next, float dim);
    void loadModel (const juce::File& f);
    void chooseModel();
    void loadBuiltInRoom (int index);
    void updateButtons();
    void drawHelp (juce::Graphics& g, juce::Rectangle<int> area);
    void drawArranging (juce::Graphics& g, juce::Rectangle<int> area);
    void drawMoreHands (juce::Graphics& g, juce::Rectangle<int> area);
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
    // arranging (see formation, storeLayout, undo, setMagnet, sweepTo)
    std::array<uint64_t, kNumStops> sel {};                      // the chosen pipes (bit = key)
    std::vector<RysumProcessor::WalkLayout> undoStack;
    RysumProcessor::WalkLayout capture() const;
    int applyLayout (const RysumProcessor::WalkLayout& lay);     // how many pipes are sent on their way
    void pushUndo();
    double arrangeUntil = 0.0, arrangeLast = 0.0;                // (Arrange is sent again until stops just woken take it)
    int magnet = 0;
    float magnetDist = 3.0f;
    bool boxing = false, sweeping = false, helpHeld = false;
    juce::Point<float> boxFrom, boxTo, sweepLast;
    double sweepLastMs = 0.0;
    std::array<std::array<double, kNumKeys>, kNumStops> sweptAt {};
    uint32_t scatterSeed = 1;
    // more hands, the room
    V3 crossPoint (float fallback) const;           // where the cross meets a surface (or 'fallback' m ahead)
    int bowStop_ = -1, bowKey_ = -1;
    bool bowArmed = false;
    juce::Point<float> bowFrom;
    double bowDownMs = 0.0;
    double whirlFx = 0.0, whirlAgain = 0.0;
    V3 whirlPoint;
    float whirlStrength = 1.0f;
    bool paused = false, slow = false, earsAway = false;
    V3 earEye;
    float earPose[4] {};
    juce::TextButton controlsButton { juce::String::fromUTF8 ("Steuerung \xe2\x80\xa6") };
    void showControlsMenu();
    bool alwaysSpectator = false, spectatorShown = false;
    int handsShown = 0;
    int handLevel() const noexcept;                               // what the hands may do (2 = all; listening: the player's leave)
    bool handsMay (int need) const noexcept { return handLevel() >= need; }
    bool shownWhileListening (juce::Component*) const;
    std::map<int, std::array<float, 4>> figSmooth;   // Konzert: the others' places as drawn (eased)
    double figSmoothMs = 0.0;
    void refreshSpectator();
    bool anyPinned() const noexcept { for (const auto& m : proc.engine.pinMask) if (m.load() != 0) return true; return false; }
    bool overlayBusy() const noexcept { return ! proc.walkLanterns.empty() || anyPinned() || whirlFx > 0.0 || earsAway || helpHeld || magnet != 0 || boxing || selectedCount() > 0; }
    // a frame in two parts: the picture (the room and the pipes as triangles), and what is drawn over it
    struct Glow { V3 at; float amount, size; int stop = -1; };       // (stop: a sounding pipe's - its colour of light)
    // ---- "Stimmung": the look of the Saal (the look* parameters, as the modulation and the Verlauf set them)
    struct LookNow
    {
        float day = 0, hour = 12, rays = 0, rayWidth = 1.2f, dust = 0, dustStir = 0.5f, pipeLight = 0, pipeHue = 0.6f,
              pipeReach = 3, trails = 0, trailLen = 2, fog = 0, fogFollow = 1, doors = 0, doorDim = 0.6f, walls = 0,
              wallGrain = 0.5f, wear = 0, shine = 0, ripple = 0, sway = 0, shake = 0, focus = 0;
    };
    LookNow readLook() const;
    LookNow lookNow;
    double lookLastMs = 0.0;
    float lookDt = 1.0f / 60.0f;              // (seconds since the last frame)
    std::array<Look::Ripple, Look::kMaxRipples> ripples {};
    int rippleNext = 0;
    float shakeEnv = 0.0f, fogDensNow = 0.0f, shadowAmt = 0.0f;
    double lookClock = 0.0;                   // (seconds, for the idle sway)
    struct Scene
    {
        EngineParams params;
        RoomParams r;
        V3 homeP, eye;
        std::shared_ptr<const RoomMesh> mesh;
        std::vector<Glow> glows;
        struct FigureLabel { V3 head; juce::String name; };
        std::vector<FigureLabel> figureLabels;   // Konzert: the others in the Saal
    } scene;
    void buildScene (float physicalScale);
    std::unique_ptr<GpuPicture> gpu;
    GpuFrame gpuFrame;
    bool gpuBlocked = false, gpuGaveUp = false;
    int gpuWaiting = 0, gpuFrames = 0;
    int overTick = 0, overCross = 0;     // what is painted over the card's picture: when it was last painted
    bool overWasCard = false, overShock = false, overTextWasEmpty = true;
    juce::String gpuNote;                // why the card does not draw (shown once)
    void updateGpu();
    void drawOver (juce::Graphics&);
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
    juce::TextButton actLight { "L Licht aus" };
    // "Publikum": the audience's phones as hands (a QR code in the corner leads them to the page)
    juce::TextButton actAudience { "Publikum" };
    // the 360° recording (Pano360.h): the frame's triangles, recorded while the view builds them
    juce::TextButton actPano { juce::String::fromUTF8 ("360\xc2\xb0 Aufnahme") };
    std::vector<Raster::Call> panoCalls;
    size_t panoStatic = 0;               // (this frame: how many of panoCalls are the room / the model)
    bool panoCapturing = false, panoWarned = false, panoForce = false;
    PanoJob panoTestJob;                 // (tests: the frame captured by panoramaForTest)
    double panoLabelMs = 0.0;
    int panoFinishedSeen = 0;
    void panoClicked();
    void startPano (int width, int fps);
    void choosePanoFolder();
    void markVideo();
    void run360Test (int step);
    bool testStarted = false;
    juce::File testFolder;
    juce::String testNote;
    std::thread markThread;                 // (marking a video as 360 degrees: a copy of the whole file)
    std::atomic<bool> markStop { false }, markBusy { false };
    void stopPano();
    void updatePanoLabel();
public:
    /** tests: a 360° picture of the scene as it is now (equirectangular, 'width' wide) */
    juce::Image panoramaForTest (int width);
    /** tests: start / stop a recording into 'folder' */
    bool startPanoForTest (const juce::File& folder, int width, int fps);
    void stopPanoForTest() { stopPano(); }
    /** tests: a frame of the view (the scene kept when the recording wants one) */
    void frameForTest() { buildScene (1.0f); }
    V3 eyeForTest() const { return eyeNow(); }
    size_t panoTrianglesForTest() const noexcept { return panoTestJob.calls.size(); }
    /** the scene panoramaForTest last drew (to draw it again, e.g. on the graphics card) */
    const PanoJob& panoJobForTest() const noexcept { return panoTestJob; }
private:
    void setAudience (bool on);
    void drawAudience (juce::Graphics& g, juce::Rectangle<int> area);
    double audienceShockSeen = 0.0;
    double audienceAlone = 0.0; // since when no phone has come (for the firewall hint)
    bool audienceLoopback = false;   // (tests: only this computer can reach it - no firewall question)
public:
    /** tests: switch the audience on (only reachable from this computer) / off */
    bool setAudienceForTest (bool on) { audienceLoopback = true; setAudience (on); return proc.getAudience() != nullptr; }
private:
    int seenWalkSerial = 0;          // (proc.walkSerial when the view last took over the room, light and pose)
    std::vector<Raster::Lamp> lamps;
    void setDark (bool on);
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
    // "Stimmung": the look of the Saal - light, air, the room, the camera (a panel under the illusions)
    static constexpr int kLookRows = 23;
    std::array<std::unique_ptr<ParamRow>, kLookRows> lookRows;
    juce::TextButton actLook { juce::String::fromUTF8 ("Atmosph\xc3\xa4re") };
    juce::Rectangle<int> lookArea;
    void showLook (bool on);
public:
    void setLookOpenForTest (bool on) { showLook (on); }
private:
    juce::Image glowSprite;
    juce::String status;
    double statusUntil = 0.0;
};
} // namespace rysum::ui
