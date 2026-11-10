#include "WalkView.h"
#include "RoomSim.h"

namespace rysum::ui
{
namespace
{
juce::String U (const char* s) { return juce::String::fromUTF8 (s); }

constexpr float kFootage[kNumStops] { 8.0f, 8.0f, 4.0f, 2.0f, 2.67f, 1.33f, 8.0f, 16.0f };
const juce::Colour kMetal[kNumStops] { juce::Colour (0xffd9d6cd), juce::Colour (0xff8e877a), juce::Colour (0xffc9c6bd), juce::Colour (0xffbdbab2),
                                       juce::Colour (0xffb5afa2), juce::Colour (0xffaaa69c), juce::Colour (0xffc8a15a), juce::Colour (0xffe4e0d6) };
constexpr float kUnitScale[] { 1.0f, 0.01f, 0.001f, 0.0254f, 0.3048f };

/** how long / wide a pipe is drawn: an 8' C is 2.4 m, every octave halves it */
void pipeSize (int stop, int key, float& len, float& rad)
{
    len = 2.44f * kFootage[stop] / 8.0f * std::exp2 (-(float) key / 12.0f);
    if (stop == 1) len *= 0.5f;                            // Gedackt: stopped, half as long
    len = juce::jlimit (0.12f, 6.0f, len);
    rad = juce::jlimit (0.012f, 0.13f, len * (stop == 1 ? 0.07f : 0.035f));
}

bool insidePlan (const FloorPlan& f, float W, float L, float x, float y)
{
    bool in = false;
    for (int i = 0, j = f.n - 1; i < f.n; j = i++)
    {
        const float xi = f.x[(size_t) i] * W, yi = f.y[(size_t) i] * L, xj = f.x[(size_t) j] * W, yj = f.y[(size_t) j] * L;
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = ! in;
    }
    return in;
}
} // namespace

WalkView::WalkView (RysumProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
    for (auto* b : { &back, &load, &clear, &fit, &zUp, &home })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    back.onClick = [this] { if (onBack) onBack(); };
    back.setTooltip (U ("Zur\xc3\xbc" "ck zum Saal (Esc)"));
    load.onClick = [this] { chooseModel(); };
    load.setTooltip (U ("Ein 3D-Modell (OBJ, mit MTL-Farben) als Raum: nur zu sehen \xe2\x80\x93 der Klang bleibt der des Saals."));
    clear.onClick = [this] { proc.clearWalkModel(); status = U ("Modell entfernt"); statusUntil = juce::Time::getMillisecondCounterHiRes() + 2500; updateButtons(); repaint(); };
    fit.setTooltip (U ("Der Saal bekommt die Ma\xc3\x9f" "e des Modells (rechteckig, Breite \xc3\x97 L\xc3\xa4nge \xc3\x97 H\xc3\xb6he)"));
    fit.onClick = [this]
    {
        auto m = proc.getWalkMesh();
        if (m == nullptr) return;
        auto set = [this] (const char* key, float v)
        {
            if (auto* prm = proc.apvts.getParameter (RysumProcessor::roomParamId (0, key))) prm->setValueNotifyingHost (prm->convertTo0to1 (v));
        };
        set ("W", juce::jlimit (2.0f, 40.0f, m->size.x));
        set ("L", juce::jlimit (2.0f, 100.0f, m->size.y));
        set ("H", juce::jlimit (2.0f, 40.0f, m->size.z));
        set ("Size", 1.0f);
        set ("Shape", (float) shapeBox);
        status = U ("Saal: ") + juce::String (m->size.x, 1) + U (" \xc3\x97 ") + juce::String (m->size.y, 1) + U (" \xc3\x97 ") + juce::String (m->size.z, 1) + " m";
        statusUntil = juce::Time::getMillisecondCounterHiRes() + 3000;
    };
    zUp.setClickingTogglesState (true);
    zUp.setTooltip (U ("Das Modell hat Z als Hochachse (sonst Y, wie meist bei OBJ)"));
    zUp.onClick = [this] { if (proc.walkModelFile.existsAsFile()) { proc.walkZUp = zUp.getToggleState(); loadModel (proc.walkModelFile); } else proc.walkZUp = zUp.getToggleState(); };
    home.setTooltip (U ("Zur\xc3\xbc" "ck an den H\xc3\xb6rerplatz des Saals, Blick nach vorn"));
    home.onClick = [this] { px = py = yaw = pitch = 0.0f; pushPose(); repaint(); };
    units.addItemList ({ "Meter", "Zentimeter", "Millimeter", "Zoll", U ("Fu\xc3\x9f") }, 1);
    units.setTooltip (U ("Einheit des Modells"));
    units.setWantsKeyboardFocus (false);
    units.onChange = [this]
    {
        proc.walkScale = kUnitScale[juce::jlimit (0, 4, units.getSelectedItemIndex())];
        if (proc.walkModelFile.existsAsFile()) loadModel (proc.walkModelFile);
    };
    addAndMakeVisible (units);
    updateButtons();
}

WalkView::~WalkView() { proc.engine.viewWalkOn = false; }

void WalkView::updateButtons()
{
    const bool have = proc.getWalkMesh() != nullptr;
    clear.setEnabled (have);
    fit.setEnabled (have);
    zUp.setToggleState (proc.walkZUp, juce::dontSendNotification);
    int u = 0;
    for (int i = 0; i < 5; ++i) if (std::abs (kUnitScale[i] - proc.walkScale) < 1.0e-6f) u = i;
    units.setSelectedItemIndex (u, juce::dontSendNotification);
}

void WalkView::resized()
{
    auto r = getLocalBounds().reduced (16, 10).removeFromTop (26);
    back.setBounds (r.removeFromLeft (70));
    home.setBounds (r.removeFromRight (82));
    r.removeFromRight (6);
    fit.setBounds (r.removeFromRight (122));
    r.removeFromRight (6);
    zUp.setBounds (r.removeFromRight (62));
    r.removeFromRight (6);
    units.setBounds (r.removeFromRight (104));
    r.removeFromRight (6);
    clear.setBounds (r.removeFromRight (124));
    r.removeFromRight (6);
    load.setBounds (r.removeFromRight (150));
}

void WalkView::visibilityChanged()
{
    if (isVisible())
    {
        px = proc.walkPose[0].load(); py = proc.walkPose[1].load(); yaw = proc.walkPose[2].load(); pitch = proc.walkPose[3].load();
        proc.engine.viewWalkOn = true;
        pushPose();
        updateButtons();
        lastTime = juce::Time::getMillisecondCounterHiRes();
        startTimerHz (30);          // (the message thread is the host's too: a full model takes ~14 ms a frame)
        if (isShowing()) grabKeyboardFocus();
    }
    else
    {
        stopTimer();
        looking = false;
        proc.engine.viewWalkOn = false;
    }
}

void WalkView::pushPose()
{
    proc.walkPose[0] = px; proc.walkPose[1] = py; proc.walkPose[2] = yaw; proc.walkPose[3] = pitch;
    proc.engine.viewX = px; proc.engine.viewY = py; proc.engine.viewYaw = yaw;
}

RoomParams WalkView::roomNow() const
{
    RoomParams now = proc.readParams().roomAt (0);
    const auto& h = proc.engine.heard[0];
    now.width = h.W.load(); now.length = h.L.load(); now.height = h.H.load(); now.size = 1.0f;
    now.listenerX = h.Lx.load(); now.listenerY = h.Ly.load();
    now.shape = h.shape.load(); now.shapeAmt = h.shapeAmt.load();
    finishPlan (now, proc.getDrawnPlan (0));
    return now;
}

void WalkView::step (double dt, bool fwd, bool bwd, bool left, bool right, float turn, bool fast)
{
    yaw = wrapAz (yaw + turn);
    const float v = (fast ? 4.2f : 1.6f) * (float) dt;
    const float ya = juce::degreesToRadians (yaw);
    const float fx = -std::sin (ya), fy = std::cos (ya), rx = std::cos (ya), ry = std::sin (ya);
    float dx = 0.0f, dy = 0.0f;
    if (fwd)   { dx += fx; dy += fy; }
    if (bwd)   { dx -= fx; dy -= fy; }
    if (right) { dx += rx; dy += ry; }
    if (left)  { dx -= rx; dy -= ry; }
    const float l = std::sqrt (dx * dx + dy * dy);
    if (l > 0.0f)
    {
        const auto r = roomNow();
        const auto homeP = listenerPos (r);
        float x = homeP.x + px + dx / l * v, y = homeP.y + py + dy / l * v;
        // the walls hold you (as they hold the listener of the room simulation)
        if (r.box()) { x = juce::jlimit (0.3f, r.W() - 0.3f, x); y = juce::jlimit (0.3f, r.L() - 0.3f, y); }
        else keepInsidePlan (planGeo (r), x, y, 0.3f);
        px = x - homeP.x; py = y - homeP.y;
    }
    pushPose();
}

void WalkView::stepForTest (double dt, bool f, bool b, bool l, bool r, float turnDeg) { step (dt, f, b, l, r, turnDeg, false); }

void WalkView::timerCallback()
{
    const double t = juce::Time::getMillisecondCounterHiRes();
    const double dt = juce::jlimit (0.0, 0.1, (t - lastTime) * 0.001);
    lastTime = t;
    if (hasKeyboardFocus (false))
    {
        auto down = [] (int code) { return juce::KeyPress::isKeyCurrentlyDown (code); };
        const bool f = down ('W') || down ('w') || down (juce::KeyPress::upKey);
        const bool b = down ('S') || down ('s') || down (juce::KeyPress::downKey);
        const bool l = down ('A') || down ('a');
        const bool r = down ('D') || down ('d');
        float turn = 0.0f;
        if (down (juce::KeyPress::leftKey)) turn += 90.0f * (float) dt;
        if (down (juce::KeyPress::rightKey)) turn -= 90.0f * (float) dt;
        if (down ('Q') || down ('q')) turn += 90.0f * (float) dt;
        if (down ('E') || down ('e')) turn -= 90.0f * (float) dt;
        const bool fast = juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
        if (f || b || l || r || turn != 0.0f) step (dt, f, b, l, r, turn, fast);
    }
    repaint();
}

bool WalkView::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { if (onBack) onBack(); return true; }
    const auto c = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());
    if (c == 'w' || c == 'a' || c == 's' || c == 'd' || c == 'q' || c == 'e') return true;
    const int code = k.getKeyCode();
    return code == juce::KeyPress::upKey || code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey;
}

// (the walking keys are also the computer keyboard's notes: here they walk, the notes come from MIDI or "Halten")
bool WalkView::keyStateChanged (bool) { return true; }

void WalkView::focusLost (FocusChangeType) { looking = false; }

void WalkView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    looking = true;
    lastMouse = e.position;
    e.source.enableUnboundedMouseMovement (true);
}

void WalkView::mouseDrag (const juce::MouseEvent& e)
{
    if (! looking) return;
    const auto d = e.position - lastMouse;
    lastMouse = e.position;
    yaw = wrapAz (yaw - d.x * 0.22f);
    pitch = juce::jlimit (-85.0f, 85.0f, pitch - d.y * 0.22f);
    pushPose();
    repaint();
}

void WalkView::mouseUp (const juce::MouseEvent& e)
{
    looking = false;
    e.source.enableUnboundedMouseMovement (false);
}

void WalkView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    fov = juce::jlimit (40.0f, 110.0f, fov - w.deltaY * 30.0f);
    repaint();
}

void WalkView::chooseModel()
{
    chooser = std::make_unique<juce::FileChooser> (U ("Raummodell (OBJ)"), proc.walkModelFile.existsAsFile() ? proc.walkModelFile.getParentDirectory() : juce::File(), "*.obj");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f.existsAsFile()) loadModel (f);
        grabKeyboardFocus();
    });
}

void WalkView::loadModel (const juce::File& f)
{
    const auto err = proc.loadWalkModel (f, proc.walkZUp, proc.walkScale);
    auto m = proc.getWalkMesh();
    if (err.isNotEmpty()) status = err;
    else if (m != nullptr)
        status = m->name + ": " + juce::String ((int) m->tris.size()) + U (" Dreiecke, ") + juce::String (m->size.x, 1) + U (" \xc3\x97 ")
               + juce::String (m->size.y, 1) + U (" \xc3\x97 ") + juce::String (m->size.z, 1) + " m" + (m->message.isNotEmpty() ? " - " + m->message : juce::String());
    if (m != nullptr && err.isEmpty())
    {
        // you stand and are stopped by the Saal's walls, not the model's: say so when they differ
        const auto r = roomNow();
        if (std::abs (m->size.x - r.W()) > 1.0f || std::abs (m->size.y - r.L()) > 1.0f || std::abs (m->size.z - r.H()) > 1.0f)
            status << U ("  \xc2\xb7  der Saal ist ") << juce::String (r.W(), 1) << U (" \xc3\x97 ") << juce::String (r.L(), 1) << U (" \xc3\x97 ")
                   << juce::String (r.H(), 1) << U (" m: \xe2\x80\x9eMa\xc3\x9f" "e \xc3\xbc" "bernehmen\xe2\x80\x9c passt ihn an");
    }
    statusUntil = juce::Time::getMillisecondCounterHiRes() + 5000;
    updateButtons();
    repaint();
}

void WalkView::buildRoomTris (const RoomParams& r, std::vector<RTri>& out) const
{
    out.clear();
    const float W = r.W(), L = r.L(), H = r.H();
    const bool box = r.box() || r.plan.n < 3;
    auto inside = [&] (float x, float y) { return box ? (x >= 0 && x <= W && y >= 0 && y <= L) : insidePlan (r.plan, W, L, x, y); };
    auto quad = [&] (Raster::Vert a, Raster::Vert b, Raster::Vert c, Raster::Vert d, int slot, float tone)
    {
        out.push_back ({ a, b, c, slot, tone });
        out.push_back ({ a, c, d, slot, tone });
    };
    auto vt = [] (V3 p, V3 n, float u, float v) { Raster::Vert q; q.p = p; q.n = n; q.u = u; q.v = v; return q; };
    // floor and ceiling in fields of a metre or more (their texture: slabs, boards, carpet, plaster)
    const float tile = juce::jmax (1.0f, std::ceil (juce::jmax (W, L) / 30.0f));
    float x0 = 0, x1 = W, y0 = 0, y1 = L;
    if (! box)
    {
        x0 = y0 = 1.0e9f; x1 = y1 = -1.0e9f;
        for (int i = 0; i < r.plan.n; ++i)
        {
            x0 = juce::jmin (x0, r.plan.x[(size_t) i] * W); x1 = juce::jmax (x1, r.plan.x[(size_t) i] * W);
            y0 = juce::jmin (y0, r.plan.y[(size_t) i] * L); y1 = juce::jmax (y1, r.plan.y[(size_t) i] * L);
        }
    }
    const V3 nz { 0, 0, 1 };
    for (float x = x0; x < x1 - 1.0e-3f; x += tile)
        for (float y = y0; y < y1 - 1.0e-3f; y += tile)
        {
            const float xe = juce::jmin (x1, x + tile), ye = juce::jmin (y1, y + tile);
            if (! inside (0.5f * (x + xe), 0.5f * (y + ye))) continue;
            quad (vt ({ x, y, 0 }, nz, x, y), vt ({ xe, y, 0 }, nz, xe, y), vt ({ xe, ye, 0 }, nz, xe, ye), vt ({ x, ye, 0 }, nz, x, ye), 1, 1.0f);
            quad (vt ({ x, y, H }, nz, x, y), vt ({ xe, y, H }, nz, xe, y), vt ({ xe, ye, H }, nz, xe, ye), vt ({ x, ye, H }, nz, x, ye), 2, 1.0f);
        }
    // walls: the material from the absorption; a darker plinth of the same stuff
    float along = 0.0f;
    auto wall = [&] (float ax, float ay, float bx, float by)
    {
        const float len = std::hypot (bx - ax, by - ay);
        if (len < 1.0e-3f) return;
        const V3 n = normalised ({ -(by - ay), bx - ax, 0.0f });
        const int nSeg = juce::jmax (1, (int) std::ceil (len / 2.5f));
        const float plinth = juce::jmin (0.9f, H * 0.2f);
        for (int i = 0; i < nSeg; ++i)
        {
            const float t0 = (float) i / (float) nSeg, t1 = (float) (i + 1) / (float) nSeg;
            const V3 p0 { ax + (bx - ax) * t0, ay + (by - ay) * t0, 0 }, p1 { ax + (bx - ax) * t1, ay + (by - ay) * t1, 0 };
            const float u0 = along + len * t0, u1 = along + len * t1;
            quad (vt (p0, n, u0, 0), vt (p1, n, u1, 0), vt (p1 + V3 { 0, 0, plinth }, n, u1, plinth), vt (p0 + V3 { 0, 0, plinth }, n, u0, plinth), 0, 0.55f);
            const int bands = juce::jmax (1, (int) std::ceil ((H - plinth) / 3.0f));
            for (int b = 0; b < bands; ++b)
            {
                const float z0 = plinth + (H - plinth) * (float) b / (float) bands, z1 = plinth + (H - plinth) * (float) (b + 1) / (float) bands;
                quad (vt (p0 + V3 { 0, 0, z0 }, n, u0, z0), vt (p1 + V3 { 0, 0, z0 }, n, u1, z0),
                      vt (p1 + V3 { 0, 0, z1 }, n, u1, z1), vt (p0 + V3 { 0, 0, z1 }, n, u0, z1), 0, 1.0f - 0.06f * (float) b / (float) bands);
            }
        }
        along += len;
    };
    if (box)
    {
        wall (0, 0, W, 0); wall (W, 0, W, L); wall (W, L, 0, L); wall (0, L, 0, 0);
    }
    else
        for (int i = 0; i < r.plan.n; ++i)
        {
            const int j = (i + 1) % r.plan.n;
            wall (r.plan.x[(size_t) i] * W, r.plan.y[(size_t) i] * L, r.plan.x[(size_t) j] * W, r.plan.y[(size_t) j] * L);
        }
}

void WalkView::addPipe (V3 bottom, float len, float rad, float faceAngle, float tone, float glowAmt)
{
    // a real pipe: a conical foot (to 16 % of its length), the round body, an open dark top; the mouth (in the
    // texture) turned towards the listener's place. Fewer sides when it is small on screen.
    const float depth = rast.viewDepth (bottom + V3 { 0, 0, 0.5f * len });
    if (depth < -len) return;
    const float px = rad / juce::jmax (0.1f, depth) * 0.5f * (float) rast.width();
    const int segs = px < 1.5f ? 5 : (px < 5.0f ? 8 : (px < 14.0f ? 12 : 18));
    const float rFoot = rad * 0.22f, zFoot = 0.16f * len;
    const float slope = (rad - rFoot) / juce::jmax (1.0e-4f, zFoot);
    const float em = 0.75f * glowAmt;
    Raster::Vert ring[3][20];
    for (int i = 0; i <= segs; ++i)
    {
        const float a = faceAngle + juce::MathConstants<float>::twoPi * (float) i / (float) segs;
        const float ca = std::cos (a), sa = std::sin (a);
        const float u = (float) i / (float) segs;
        const V3 nb { ca, sa, 0.0f };
        const V3 nf = normalised ({ ca, sa, -slope });
        auto& r0 = ring[0][i]; r0.p = bottom + V3 { ca * rFoot, sa * rFoot, 0.0f }; r0.n = nf; r0.u = u; r0.v = 0.0f;
        auto& r1 = ring[1][i]; r1.p = bottom + V3 { ca * rad, sa * rad, zFoot }; r1.n = nb; r1.u = u; r1.v = 0.16f;
        auto& r2 = ring[2][i]; r2.p = bottom + V3 { ca * rad, sa * rad, len }; r2.n = nb; r2.u = u; r2.v = 1.0f;
    }
    for (int i = 0; i < segs; ++i)
    {
        // the foot (its own normals, so the cone catches the light differently)
        Raster::Vert f1 = ring[1][i], f2 = ring[1][i + 1];
        f1.n = ring[0][i].n; f2.n = ring[0][i + 1].n;
        rast.triangle (ring[0][i], ring[0][i + 1], f2, kSlotMetal, tone, em);
        rast.triangle (ring[0][i], f2, f1, kSlotMetal, tone, em);
        rast.triangle (ring[1][i], ring[1][i + 1], ring[2][i + 1], kSlotMetal, tone, em);
        rast.triangle (ring[1][i], ring[2][i + 1], ring[2][i], kSlotMetal, tone, em);
    }
    // looking into the open top (only seen from above): dark
    if (rast.eyePos().z > bottom.z + len)
    {
        Raster::Vert c; c.p = bottom + V3 { 0, 0, len - 0.3f * rad }; c.n = { 0, 0, 1 };
        for (int i = 0; i < segs; ++i)
        {
            Raster::Vert a = ring[2][i], b = ring[2][i + 1];
            a.n = b.n = c.n;
            rast.triangle (a, b, c, kSlotDark, 1.0f, 0.3f * em);
        }
    }
}

void WalkView::paint (juce::Graphics& g)
{
    auto& A = Assets::get();
    const auto params = proc.readParams();
    const auto r = roomNow();
    const V3 homeP { listenerPos (r).x, listenerPos (r).y, listenerPos (r).z };
    const V3 eye { homeP.x + px, homeP.y + py, homeP.z };
    const auto area = getLocalBounds();

    // as sharp as the screen allows (in full screen the window is scaled up), less when a frame takes too long
    const float phys = juce::jlimit (1.0f, 2.5f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const float sc = juce::jlimit (0.4f, 2.5f, phys * resScale);
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    rast.setSize (juce::roundToInt ((float) area.getWidth() * sc), juce::roundToInt ((float) area.getHeight() * sc));
    rast.setCamera (eye, yaw, pitch, fov);
    const float fogDist = juce::jmax (8.0f, 0.9f * juce::jmax (r.W(), r.L()));
    rast.begin (0.07f, fogDist, 0.12f, ++frame);

    // the surfaces from the Saal's absorption, scatter and high damping
    const auto wm = wallMaterial (r.absWalls, r.highDamp, r.scatter);
    rast.setMaterial (kSlotWall, wm);
    rast.setMaterial (kSlotFloor, floorMaterial (r.absFloor, r.highDamp, r.scatter, false));
    rast.setMaterial (kSlotCeiling, floorMaterial (r.absFloor, r.highDamp, r.scatter, true));
    rast.setMaterial (kSlotModel, wm);
    { Material m; m.kind = Material::metal; m.tone = 1.0f; m.gloss = 1.0f; m.rough = 0.1f; rast.setMaterial (kSlotMetal, m); }
    { Material m; m.kind = Material::dark; m.tone = 1.0f; m.gloss = 0.0f; rast.setMaterial (kSlotDark, m); }
    wallLook = wm.kind;

    const auto mesh = proc.getWalkMesh();
    if (mesh != nullptr)
    {
        // a model in greys: its colours' brightness, the walls' material on top
        for (const auto& t : mesh->tris)
        {
            const auto c = t.colour;
            const float lum = 0.3f + 0.9f * (0.3f * c.getFloatRed() + 0.59f * c.getFloatGreen() + 0.11f * c.getFloatBlue());
            rast.flatTriangle (t.a, t.b, t.c, kSlotModel, lum);
        }
    }
    else
    {
        buildRoomTris (r, roomTris);
        for (const auto& t : roomTris) rast.triangle (t.a, t.b, t.c, t.slot, t.tone);
    }

    // the pipes, where Ambisonics puts them (as heard from the listener's place; you walk among them)
    const double now = proc.engine.timeSeconds.load();
    struct Glow { V3 at; float amount, size; };
    std::vector<Glow> glows;
    for (int s = 0; s < kNumStops; ++s)
    {
        if (! params.stopOn[(size_t) s]) { for (auto& v : glow[(size_t) s]) v = 0.0f; continue; }
        const uint64_t sounding = proc.engine.soundingMask[(size_t) s].load();
        const float metalTone = kMetal[s].getPerceivedBrightness() * 1.05f;
        for (int k = 0; k < kNumKeys; ++k)
        {
            if (proc.engine.muted[(size_t) s][(size_t) k].load()) continue;
            auto& gl = glow[(size_t) s][(size_t) k];
            gl = ((sounding >> k) & 1) ? juce::jmin (1.0f, gl + 0.35f) : gl * 0.9f;
            const Pos pos = proc.engine.displayPosition (s, k, params);
            const float a = juce::degreesToRadians (pos.az), e = juce::degreesToRadians (pos.el);
            const V3 c { homeP.x - std::sin (a) * std::cos (e) * pos.dist, homeP.y + std::cos (a) * std::cos (e) * pos.dist, homeP.z + std::sin (e) * pos.dist };
            float len, rad;
            pipeSize (s, k, len, rad);
            // floating: a slow breath up and down, each pipe its own
            const float bob = 0.06f * (float) std::sin (now * 0.7 + (double) (s * 41 + k) * 1.37);
            const V3 bottom { c.x, c.y, c.z - 0.5f * len + bob };
            const float face = std::atan2 (homeP.y - c.y, homeP.x - c.x);       // the mouth towards the listener's place
            addPipe (bottom, len, rad, face, metalTone, gl);
            if (gl > 0.02f) glows.push_back ({ { c.x, c.y, c.z + bob }, gl, len });
        }
    }
    geoMs = (float) (juce::Time::getMillisecondCounterHiRes() - t0);
    rast.finish();
    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
    lastFrameMs = (float) ms;
    if (! testMode)
    {
        if (ms > 16.0) resScale = juce::jmax (0.4f / phys, resScale * 0.9f);         // (the host shares this thread)
        else if (ms < 9.0) resScale = juce::jmin (1.0f, resScale * 1.03f);
    }

    {
        juce::Graphics::ScopedSaveState ss (g);
        g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
        g.drawImage (rast.image(), area.toFloat());
    }
    const float isc = (float) area.getWidth() / (float) rast.width();

    // the sounding pipes shine (where they are not hidden behind a wall)
    for (const auto& gw : glows)
    {
        float sx, sy, d;
        if (! rast.project (gw.at, sx, sy, d)) continue;
        const float zd = rast.depthAt ((int) sx, (int) sy);
        if (zd > 0.0f && zd < d * 0.95f - 0.4f) continue;         // (the pipe itself is drawn a little nearer than its axis)
        const float rad = juce::jlimit (6.0f, 260.0f, (0.6f + 0.35f * gw.size) / d * 0.5f * (float) area.getWidth());
        const juce::Point<float> c (sx * isc, sy * isc);
        // (a glow drawn once, only stretched here: gradients per frame cost several milliseconds)
        if (! glowSprite.isValid())
        {
            glowSprite = juce::Image (juce::Image::ARGB, 128, 128, true);
            juce::Graphics gg (glowSprite);
            juce::ColourGradient grad (juce::Colour (0xfffff1dc).withAlpha (0.5f), 64.0f, 64.0f, col::rubric.withAlpha (0.0f), 128.0f, 64.0f, true);
            grad.addColour (0.3, juce::Colour (0xffe9c9a8).withAlpha (0.2f));
            grad.addColour (0.65, col::rubric.withAlpha (0.07f));
            gg.setGradientFill (grad);
            gg.fillEllipse (0.0f, 0.0f, 128.0f, 128.0f);
        }
        juce::Graphics::ScopedSaveState ss (g);
        g.setOpacity (juce::jlimit (0.0f, 1.0f, gw.amount));
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImage (glowSprite, juce::Rectangle<float> (2.0f * rad, 2.0f * rad).withCentre (c));
    }

    // with a model: the room the sound is computed in, as a faint outline
    if (mesh != nullptr)
    {
        g.setColour (col::rubric.withAlpha (0.55f));
        const float W = r.W(), L = r.L(), H = r.H();
        const V3 cn[8] { { 0, 0, 0 }, { W, 0, 0 }, { W, L, 0 }, { 0, L, 0 }, { 0, 0, H }, { W, 0, H }, { W, L, H }, { 0, L, H } };
        const int ed[12][2] { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        for (auto& e : ed)
        {
            juce::Point<float> a, b;
            if (rast.projectLine (cn[e[0]], cn[e[1]], a, b)) g.drawLine ({ a * isc, b * isc }, 1.0f);
        }
    }

    // a small cross in the middle
    {
        const auto c = area.getCentre().toFloat();
        g.setColour (col::ink.withAlpha (0.45f));
        g.drawLine (c.x - 6, c.y, c.x + 6, c.y, 1.0f);
        g.drawLine (c.x, c.y - 6, c.x, c.y + 6, 1.0f);
    }

    // heading, hints, status (on dark bands, so they stay readable over a bright model)
    {
        const auto top = area.toFloat().removeFromTop (84.0f), bottom = area.toFloat().removeFromBottom (54.0f);
        g.setGradientFill (juce::ColourGradient (col::bg.withAlpha (0.72f), 0.0f, top.getY(), col::bg.withAlpha (0.0f), 0.0f, top.getBottom(), false));
        g.fillRect (top);
        g.setGradientFill (juce::ColourGradient (col::bg.withAlpha (0.0f), 0.0f, bottom.getY(), col::bg.withAlpha (0.78f), 0.0f, bottom.getBottom(), false));
        g.fillRect (bottom);
    }
    g.setFont (A.monoFont (11.0f));
    g.setColour (col::mute);
    const auto info = U ("Begehen \xc2\xb7 ") + (mesh != nullptr ? mesh->name : U ("Saal ") + juce::String (r.W(), 1) + U (" \xc3\x97 ") + juce::String (r.L(), 1) + U (" \xc3\x97 ") + juce::String (r.H(), 1) + " m")
                    + U ("   \xc2\xb7   Platz ") + juce::String (eye.x, 1) + " / " + juce::String (eye.y, 1) + U (" m   Blick ") + juce::String (juce::roundToInt (-yaw)) + U ("\xc2\xb0")
                    + U ("   \xc2\xb7   W\xc3\xa4nde: ") + materialName (wallLook)
                    + (proc.engine.hiddenNow.load() > 0 ? U ("   \xc2\xb7   ") + juce::String (proc.engine.hiddenNow.load()) + U (" Pfeifen um die Ecke geh\xc3\xb6rt") : juce::String());
    g.drawText (info, area.reduced (18, 0).withTrimmedTop (44).removeFromTop (16), juce::Justification::centredLeft);
    g.setColour (col::faint);
    g.drawText (U ("W A S D gehen  \xc2\xb7  Maus ziehen: schauen  \xc2\xb7  Q E / \xe2\x86\x90 \xe2\x86\x92 drehen  \xc2\xb7  Shift schneller  \xc2\xb7  Rad: Blickwinkel  \xc2\xb7  Esc zur\xc3\xbc" "ck"
                  "   \xe2\x80\x94   gespielt wird hier \xc3\xbc" "ber MIDI oder \xe2\x80\x9eHalten\xe2\x80\x9c"),
                area.reduced (18, 0).removeFromBottom (24), juce::Justification::centredLeft);
    if (! proc.isBinaural())
    {
        g.setColour (col::rubric.brighter (0.2f));
        g.drawText (U ("Die Kopfdrehung ist nur bei Kopfh\xc3\xb6rer-Ausgabe (binaural) zu h\xc3\xb6ren; Gehen \xc3\xbc" "berall."),
                    area.reduced (18, 0).withTrimmedBottom (24).removeFromBottom (16), juce::Justification::centredLeft);
    }
    if (juce::Time::getMillisecondCounterHiRes() < statusUntil && status.isNotEmpty())
    {
        g.setColour (col::ink);
        g.drawText (status, area.reduced (18, 0).withTrimmedTop (62).removeFromTop (16), juce::Justification::centredLeft);
    }

    drawMap (g, juce::Rectangle<float> (190.0f, 150.0f).withPosition ((float) area.getRight() - 206.0f, (float) area.getBottom() - 186.0f), r, params, homeP);
}

void WalkView::drawMap (juce::Graphics& g, juce::Rectangle<float> box, const RoomParams& r, const EngineParams& params, V3 homeP)
{
    g.setColour (col::bg.withAlpha (0.72f));
    g.fillRect (box);
    g.setColour (col::border);
    g.drawRect (box, 1.0f);
    const float W = r.W(), L = r.L();
    // the map is turned with the view: ahead is up
    const auto c = box.getCentre();
    const float scale = juce::jmin (box.getWidth(), box.getHeight()) * 0.9f / juce::jmax (W, L) * 1.4f;
    const float ex = homeP.x + px, ey = homeP.y + py;
    const float ya = juce::degreesToRadians (yaw);
    auto P = [&] (float x, float y)
    {
        const float dx = x - ex, dy = y - ey;
        const float rx = dx * std::cos (ya) + dy * std::sin (ya), fy = -dx * std::sin (ya) + dy * std::cos (ya);
        return juce::Point<float> (c.x + rx * scale, c.y + 18.0f - fy * scale);
    };
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (box.toNearestInt().reduced (1));
    juce::Path walls;
    const bool box_ = r.box() || r.plan.n < 3;
    if (box_) { walls.startNewSubPath (P (0, 0)); walls.lineTo (P (W, 0)); walls.lineTo (P (W, L)); walls.lineTo (P (0, L)); }
    else
        for (int i = 0; i < r.plan.n; ++i)
        {
            const auto q = P (r.plan.x[(size_t) i] * W, r.plan.y[(size_t) i] * L);
            if (i == 0) walls.startNewSubPath (q); else walls.lineTo (q);
        }
    walls.closeSubPath();
    g.setColour (col::ink.withAlpha (0.7f));
    g.strokePath (walls, juce::PathStrokeType (1.2f));
    for (int s = 0; s < kNumStops; ++s)
    {
        if (! params.stopOn[(size_t) s]) continue;
        for (int k = 0; k < kNumKeys; ++k)
        {
            if (proc.engine.muted[(size_t) s][(size_t) k].load()) continue;
            const Pos pos = proc.engine.displayPosition (s, k, params);
            const float a = juce::degreesToRadians (pos.az), e = juce::degreesToRadians (pos.el);
            const auto q = P (homeP.x - std::sin (a) * std::cos (e) * pos.dist, homeP.y + std::cos (a) * std::cos (e) * pos.dist);
            const float gl = glow[(size_t) s][(size_t) k];
            g.setColour (gl > 0.05f ? juce::Colour (0xffffd9a0).withAlpha (0.5f + 0.5f * gl) : col::mute.withAlpha (0.6f));
            g.fillEllipse (juce::Rectangle<float> (gl > 0.05f ? 3.5f : 2.2f, gl > 0.05f ? 3.5f : 2.2f).withCentre (q));
        }
    }
    // the place the pipes are placed from, and you (looking up = ahead)
    g.setColour (col::faint);
    g.drawEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (P (homeP.x, homeP.y)), 1.0f);
    const auto me = P (ex, ey);
    const float half = juce::degreesToRadians (0.5f * fov);
    juce::Path cone;
    cone.startNewSubPath (me);
    cone.lineTo (me.x - 40.0f * std::sin (half), me.y - 40.0f * std::cos (half));
    cone.lineTo (me.x + 40.0f * std::sin (half), me.y - 40.0f * std::cos (half));
    cone.closeSubPath();
    g.setColour (col::rubric.withAlpha (0.25f));
    g.fillPath (cone);
    g.setColour (col::rubric.brighter (0.3f));
    g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (me));
}
} // namespace rysum::ui
