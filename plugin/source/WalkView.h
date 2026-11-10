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
    void loadModelForTest (const juce::File& f) { loadModel (f); }

private:
    void timerCallback() override;
    void step (double dt, bool fwd, bool back, bool left, bool right, float turn, bool fast);
    void pushPose();
    RoomParams roomNow() const;
    void buildRoomTris (const RoomParams& r, std::vector<Tri>& out) const;
    void loadModel (const juce::File& f);
    void chooseModel();
    void updateButtons();
    void drawMap (juce::Graphics& g, juce::Rectangle<float> area, const RoomParams& r, const EngineParams& params, V3 home);

    RysumProcessor& proc;
    Raster rast;
    std::vector<Tri> roomTris;
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
    juce::String status;
    double statusUntil = 0.0;
};
} // namespace rysum::ui
