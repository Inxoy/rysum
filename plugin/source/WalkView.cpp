#include "WalkView.h"
#include "MeshAcoustics.h"
#include "RoomSim.h"
#include "RoomModels.h"
#include <algorithm>

namespace rysum::ui
{
namespace
{
juce::String U (const char* s) { return juce::String::fromUTF8 (s); }

const juce::Colour kMetal[kNumStops] { juce::Colour (0xffd9d6cd), juce::Colour (0xff8e877a), juce::Colour (0xffc9c6bd), juce::Colour (0xffbdbab2),
                                       juce::Colour (0xffb5afa2), juce::Colour (0xffaaa69c), juce::Colour (0xffc8a15a), juce::Colour (0xffe4e0d6) };
constexpr float kUnitScale[] { 1.0f, 0.01f, 0.001f, 0.0254f, 0.3048f };
// "Licht aus": what is left of the usual light where no pipe shines - the room barely to be made out
constexpr float kDarkRest = 0.018f;


// the floor and ceiling follow the outline exactly: the outline cut into triangles (ear clipping; it may be
// concave: L, cross), each triangle cut to the fields of the floor
struct P2 { float x = 0, y = 0; };
inline float cross2 (P2 a, P2 b, P2 c) noexcept { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

void triangulatePlan (const FloorPlan& f, float W, float L, std::vector<std::array<P2, 3>>& out)
{
    out.clear();
    std::vector<P2> p;
    for (int i = 0; i < f.n; ++i) p.push_back ({ f.x[(size_t) i] * W, f.y[(size_t) i] * L });
    if (p.size() < 3) return;
    float area2 = 0.0f;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) area2 += p[j].x * p[i].y - p[i].x * p[j].y;
    if (area2 < 0.0f) std::reverse (p.begin(), p.end());          // counter-clockwise from here on
    const float eps = 1.0e-7f * juce::jmax (1.0f, W * L);
    int guard = 0;
    while (p.size() > 3 && ++guard < 200)
    {
        bool cut = false;
        const size_t n = p.size();
        for (size_t i = 0; i < n && ! cut; ++i)
        {
            const P2 a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
            const float cr = cross2 (a, b, c);
            if (cr < -eps) continue;                               // a corner pointing inwards: no ear
            bool ear = true;
            if (cr > eps)                                          // (a corner on a straight line is simply dropped)
                for (size_t k = 0; k < n && ear; ++k)
                {
                    if (k == i || k == (i + n - 1) % n || k == (i + 1) % n) continue;
                    const P2 q = p[k];
                    if (cross2 (a, b, q) > -eps && cross2 (b, c, q) > -eps && cross2 (c, a, q) > -eps) ear = false;
                }
            if (! ear) continue;
            if (cr > eps) out.push_back ({ a, b, c });
            p.erase (p.begin() + (ptrdiff_t) i);
            cut = true;
        }
        if (! cut) break;                                          // (a crossing outline: what is left as a fan)
    }
    for (size_t k = 2; k < p.size(); ++k) out.push_back ({ p[0], p[k - 1], p[k] });
}

/** a triangle cut to a rectangle: the corners of what is left (convex, at most 7) */
int clipToField (const std::array<P2, 3>& t, float x0, float y0, float x1, float y1, P2* out)
{
    P2 a[8], b[8];
    int n = 3;
    a[0] = t[0]; a[1] = t[1]; a[2] = t[2];
    for (int side = 0; side < 4 && n >= 3; ++side)
    {
        auto dist = [&] (P2 q) { return side == 0 ? q.x - x0 : (side == 1 ? x1 - q.x : (side == 2 ? q.y - y0 : y1 - q.y)); };
        int m = 0;
        for (int i = 0; i < n; ++i)
        {
            const P2 p = a[i], q = a[(i + 1) % n];
            const float dp = dist (p), dq = dist (q);
            if (dp >= 0.0f) b[m++] = p;
            if ((dp >= 0.0f) != (dq >= 0.0f))
            {
                // (always worked out from the same end, and exactly on the line: neighbouring pieces share the point)
                const bool swap = q.x < p.x || (q.x == p.x && q.y < p.y);
                const P2 e0 = swap ? q : p, e1 = swap ? p : q;
                const float d0 = swap ? dq : dp, d1 = swap ? dp : dq;
                const float s = d0 / (d0 - d1);
                P2 c { e0.x + (e1.x - e0.x) * s, e0.y + (e1.y - e0.y) * s };
                if (side == 0) c.x = x0; else if (side == 1) c.x = x1; else if (side == 2) c.y = y0; else c.y = y1;
                b[m++] = c;
            }
        }
        n = juce::jmin (m, 8);
        for (int i = 0; i < n; ++i) a[i] = b[i];
    }
    if (n < 3) return 0;
    for (int i = 0; i < n; ++i) out[i] = a[i];
    return n;
}
} // namespace

WalkView::WalkView (RysumProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
    for (juce::Button* b : std::initializer_list<juce::Button*> { &back, &load, &clear, &fit, &zUp, &home, &hear, &immerse, &actShock, &actGrab, &actFreeze, &actHome, &actClap, &actMat, &actJump, &actLight, &actAudience, &controlsButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    back.onClick = [this] { if (onBack) onBack(); };
    immerse.setTooltip (U ("Nur das Bild, \xc3\xbc" "ber das ganze Fenster: keine Kn\xc3\xb6pfe, keine Schrift, keine Karte. Die Maus schaut sich um, "
                           "ohne dass eine Taste gehalten wird (der Zeiger ist versteckt). Esc oder I: zur\xc3\xbc" "ck."));
    immerse.onClick = [this] { toggleImmersive(); };
    actShock.setTooltip (U ("Eine Druckwelle st\xc3\xb6\xc3\x9ft alle Pfeifen von dir weg (Taste E, mit Shift st\xc3\xa4rker)"));
    actShock.onClick = [this] { shock (juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown() ? 1.7f : 1.0f); };
    actGrab.setTooltip (U ("Gedr\xc3\xbc" "ckt halten: die Pfeife unter dem Kreuz greifen und tragen; loslassen: werfen (Taste Q)"));
    actFreeze.setTooltip (U ("Gedr\xc3\xbc" "ckt halten: alles, was sich bewegt, steht still (Taste F)"));
    actHome.setTooltip (U ("Alle Pfeifen fliegen an ihren Platz in der Orgel zur\xc3\xbc" "ck (Taste R)"));
    actHome.onClick = [this] { callHome(); };
    actClap.setTooltip (U ("In die H\xc3\xa4nde klatschen, wo du stehst (Taste C)"));
    actClap.onClick = [this] { clap(); };
    actMat.setTooltip (U ("Das Material der Fl\xc3\xa4" "che unter dem Kreuz wechseln (Taste M)"));
    actMat.onClick = [this] { cycleMaterial(); };
    actJump.setTooltip (U ("Springen (Leertaste)"));
    actLight.setClickingTogglesState (true);
    actLight.setToggleState (proc.walkDark, juce::dontSendNotification);
    actLight.setTooltip (U ("Licht aus: nur die klingenden Pfeifen (und die Funken ihrer St\xc3\xb6\xc3\x9f" "e) erhellen den Raum (Taste L)"));
    actLight.onClick = [this] { setDark (actLight.getToggleState()); };
    actAudience.setClickingTogglesState (true);
    actAudience.setTooltip (U ("Das Publikum spielt mit: ein QR-Code f\xc3\xbchrt die Handys im selben WLAN zu einer kleinen Seite. Jedes Handy bekommt "
                               "eine Pfeife in eigener Farbe: Tippen schl\xc3\xa4gt sie an, schnell Wischen schickt eine Druckwelle, Halten und Ziehen "
                               "bl\xc3\xa4st die Pfeifen wie Wind (nach oben = wohin der Blick geht). Nur Pfeifen, die frei im Saal sind, spielen mit; "
                               "beim Einschalten schweben alle gezogenen Register, die stillstehen. Wird nicht gespeichert."));
    actAudience.onClick = [this] { setAudience (actAudience.getToggleState()); };
    controlsButton.setTooltip (U ("Wie sich Maus und Gehen anf\xc3\xbchlen (auf diesem Computer): Tempo des Schauens, Maus umgekehrt, Gehtempo. Tab gehalten: alle Tasten."));
    controlsButton.onClick = [this] { showControlsMenu(); };
    back.setTooltip (U ("Zur\xc3\xbc" "ck zum Saal (Esc)"));
    load.onClick = [this]
    {
        // the rooms that come with the plugin, or a file of one's own
        juce::PopupMenu m;
        m.addSectionHeader (U ("R\xc3\xa4ume, die dabei sind"));
        const auto& rooms = builtInRooms();
        for (int i = 0; i < (int) rooms.size(); ++i)
            m.addItem (i + 1, juce::String::fromUTF8 (rooms[(size_t) i].name) + U ("  \xe2\x80\x93  ") + juce::String::fromUTF8 (rooms[(size_t) i].about),
                       true, builtInRoomIndex (proc.walkModelFile.getFileName()) == i && proc.getWalkMesh() != nullptr);
        m.addSeparator();
        m.addItem (100, U ("Eigene Datei (OBJ) \xe2\x80\xa6"));
        juce::Component::SafePointer<WalkView> safe (static_cast<WalkView*> (this));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&load), [safe] (int r)
        {
            if (safe == nullptr || r == 0) return;
            if (r == 100) safe->chooseModel();
            else safe->loadBuiltInRoom (r - 1);
        });
    };
    load.setTooltip (U ("Ein 3D-Modell (OBJ, mit MTL-Farben) als Raum: zu sehen, zu begehen (seine W\xc3\xa4nde halten dich auf) und \xe2\x80\x93 mit \xe2\x80\x9eModell h\xc3\xb6rbar\xe2\x80\x9c \xe2\x80\x93 zu h\xc3\xb6ren. Der Quader des Saals liegt darunter."));
    clear.onClick = [this] { proc.clearWalkModel(); status = U ("Modell entfernt"); statusUntil = juce::Time::getMillisecondCounterHiRes() + 2500; updateButtons(); repaint(); };
    fit.setTooltip (U ("Der Saal bekommt die Ma\xc3\x9f" "e des Modells (rechteckig, Breite \xc3\x97 L\xc3\xa4nge \xc3\x97 H\xc3\xb6he)"));
    fit.onClick = [this]
    {
        auto m = proc.getWalkMesh();
        if (m == nullptr || ! proc.fitSaalToWalkModel()) return;
        status = U ("Saal: ") + juce::String (m->size.x, 1) + U (" \xc3\x97 ") + juce::String (m->size.y, 1) + U (" \xc3\x97 ") + juce::String (m->size.z, 1) + " m";
        statusUntil = juce::Time::getMillisecondCounterHiRes() + 3000;
    };
    zUp.setClickingTogglesState (true);
    zUp.setTooltip (U ("Das Modell hat Z als Hochachse (sonst Y, wie meist bei OBJ)"));
    zUp.onClick = [this] { if (proc.walkModelFile.existsAsFile()) { proc.walkZUp = zUp.getToggleState(); loadModel (proc.walkModelFile); } else proc.walkZUp = zUp.getToggleState(); };
    home.setTooltip (U ("Zur\xc3\xbc" "ck an den H\xc3\xb6rerplatz des Saals, Blick nach vorn"));
    home.onClick = [this] { px = py = yaw = pitch = yawT = pitchT = vx = vy = vz = fz = turnV = 0.0f; onGround = true; pushPose(); repaint(); };
    units.addItemList ({ "Meter", "Zentimeter", "Millimeter", "Zoll", U ("Fu\xc3\x9f") }, 1);
    units.setTooltip (U ("Einheit des Modells"));
    units.setWantsKeyboardFocus (false);
    units.onChange = [this]
    {
        proc.walkScale = kUnitScale[juce::jlimit (0, 4, units.getSelectedItemIndex())];
        if (proc.walkModelFile.existsAsFile()) loadModel (proc.walkModelFile);
    };
    addAndMakeVisible (units);
    hear.setClickingTogglesState (true);
    hear.setTooltip (U ("Das Modell ist auch zu h\xc3\xb6ren: Pfeifen hinter seinen W\xc3\xa4nden kommen um die Ecke, der Nachhall wird per "
                        "Raytracing durch das Modell berechnet. Der Quader des Saals liegt darunter (fr\xc3\xbche Reflexionen, Gr\xc3\xb6\xc3\x9f" "e). "
                        "Aus: das Modell ist nur zu sehen."));
    hear.onClick = [this] { proc.setWalkHear (hear.getToggleState()); };
    // the player and the pipes ("Spieler": the pipes that are bodies in the Saal follow, flee or circle the walker)
    for (int i = 0; i < 5; ++i)
        playerMode.addItem (juce::StringArray { U ("Pfeifen: ruhig"), U ("Pfeifen: ich ziehe an"), U ("Pfeifen: ich sto\xc3\x9f" "e ab"), U ("Pfeifen: umkreisen mich"), U ("Pfeifen: Sog") }[i], i + 1);
    playerMode.setWantsKeyboardFocus (false);
    playerMode.setTooltip (U ("Wie du die Pfeifen bewegst, w\xc3\xa4hrend du gehst: wie eine Masse, zu der sie fallen (anziehen), vor der sie fliehen "
                              "(absto\xc3\x9f" "en), um die sie kreisen (Umlaufbahn) oder die sie im Vorbeigehen mitrei\xc3\x9ft (Sog). "
                              "Wirkt auf Register, deren Bewegung (Raum-Seite) im Saal stattfindet: Flug, Fall, Schwarm, Str\xc3\xb6mung - und "
                              "Schweben, das nur auf dich reagiert."));
    playerModeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "lvPlayerMode", playerMode);
    playerMode.onChange = [this] { if (isShowing()) grabKeyboardFocus(); };
    addAndMakeVisible (playerMode);
    playerForce.setSliderStyle (juce::Slider::LinearBar);
    playerForce.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    playerForce.setWantsKeyboardFocus (false);
    playerForce.setTooltip (U ("Kraft, mit der du die Pfeifen bewegst"));
    playerForceAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "lvPlayerForce", playerForce);
    addAndMakeVisible (playerForce);
    {
        const char* ids[] { "lvSelfPlay", "lvGrow", "lvWhisper", "lvTimeDepth", "lvRoomHear", "lvWallPull" };
        const juce::String names[] { "Selbstspiel", "Wachsen & Brechen", U ("Fl\xc3\xbcstern"), "Zeit als Entfernung", U ("H\xc3\xb6rraum"), U ("Wandn\xc3\xa4he") };
        const juce::String tips[] {
            U ("Der Raum spielt sich selbst: jede Pfeife, die gegen eine Wand, den Boden oder die Decke st\xc3\xb6\xc3\x9ft, klingt mit ihrem eigenen Ton \xe2\x80\x93 "
               "je h\xc3\xa4rter der Sto\xc3\x9f, desto l\xc3\xa4nger. Braucht Register, deren Bewegung im Saal stattfindet (Flug, Fall, Schwarm, Str\xc3\xb6mung)."),
            U ("Pfeifen, die nicht klingen, wachsen langsam und werden tiefer (bis zur doppelten L\xc3\xa4nge, eine Oktave). Ein harter Sto\xc3\x9f bricht eine Pfeife "
               "entzwei: halb so lang, eine Oktave h\xc3\xb6her. Mehr = schnelleres Wachsen, leichteres Brechen. Aus: jede Pfeife findet zur\xc3\xbc" "ck; R heilt alle sofort."),
            U ("Eine Pfeife ganz nah am Kopf (unter 1,4 m) ist auch direkt im Ohr zu h\xc3\xb6ren: das nahe Ohr viel lauter, das ferne sp\xc3\xa4ter und dumpfer, der Klang w\xc3\xa4rmer. "
               "Pfeife greifen (Q) und mit dem Mausrad heranholen. Nur \xc3\xbc" "ber Kopfh\xc3\xb6rer (binaural) und Stereo."),
            U ("Das vordere Ende des Saals spielt die Vergangenheit: am gewohnten Platz h\xc3\xb6rst du die Gegenwart, an der Stirnwand das, was vor so vielen Sekunden "
               "erklang. Schnell nach vorn gehen: die Zeit l\xc3\xa4uft r\xc3\xbc" "ckw\xc3\xa4rts; zur\xc3\xbc" "ckgehen: sie holt auf."),
            U ("Der Nachhall geh\xc3\xb6rt zu einem Raum anderer Gr\xc3\xb6\xc3\x9f" "e als der, den du siehst und begehst: eine Kapelle sehen, eine Kathedrale h\xc3\xb6ren (oder umgekehrt). "
               "Braucht \xe2\x80\x9eSaal an\xe2\x80\x9c."),
            U ("Je n\xc3\xa4her du einer Wand kommst (ab 3 m), desto gr\xc3\xb6\xc3\x9f" "er und lauter wird der Nachhall \xe2\x80\x93 als \xc3\xb6" "ffne sich hinter der Wand ein weiterer Raum. Braucht \xe2\x80\x9eSaal an\xe2\x80\x9c.") };
        for (int i = 0; i < 6; ++i)
        {
            illusions[(size_t) i] = std::make_unique<ParamRow> (proc.apvts, ids[i], names[i], true);
            illusions[(size_t) i]->setTooltip (tips[i]);
            illusions[(size_t) i]->setWantsKeyboardFocus (false);
            addAndMakeVisible (*illusions[(size_t) i]);
        }
    }
    updateButtons();
}

WalkView::~WalkView()
{
    gpu.reset();
    proc.engine.viewWalkOn = false; proc.engine.viewZ = 0.0f;
    proc.engine.grabStop = -1; proc.engine.actFreeze = false;
}

void WalkView::updateButtons()
{
    const bool have = proc.getWalkMesh() != nullptr;
    clear.setEnabled (have);
    fit.setEnabled (have);
    hear.setEnabled (have);
    hear.setToggleState (proc.getWalkHear(), juce::dontSendNotification);
    zUp.setToggleState (proc.walkZUp, juce::dontSendNotification);
    int u = 0;
    for (int i = 0; i < 5; ++i) if (std::abs (kUnitScale[i] - proc.walkScale) < 1.0e-6f) u = i;
    units.setSelectedItemIndex (u, juce::dontSendNotification);
}

void WalkView::resized()
{
    auto r = getLocalBounds().reduced (16, 10).removeFromTop (26);
    back.setBounds (r.removeFromLeft (70));
    r.removeFromLeft (6);
    immerse.setBounds (r.removeFromLeft (84));
    {
        // the hands: a row above the hints at the bottom
        auto h = getLocalBounds().reduced (18, 0).withTrimmedBottom (26).removeFromBottom (22);
        handsArea = h;
        const int widths[] { 104, 88, 92, 104, 96, 92, 78, 96, 80, 92 };
        juce::Button* hands[] { &actShock, &actGrab, &actFreeze, &actHome, &actClap, &actMat, &actJump, &actLight, &actAudience, &controlsButton };
        for (int i = 0; i < 10; ++i)
        {
            hands[i]->setBounds (h.removeFromLeft (widths[i]));
            h.removeFromLeft (5);
        }
        handsArea = h;
    }
    {
        // the illusions: a row of sliders under the buttons
        auto row = getLocalBounds().reduced (18, 0).withTrimmedTop (42).removeFromTop (36);
        const int w = (row.getWidth() - 5 * 16) / 6;
        for (auto& il : illusions)
        {
            if (il != nullptr) il->setBounds (row.removeFromLeft (w));
            row.removeFromLeft (16);
        }
    }
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
    r.removeFromRight (6);
    hear.setBounds (r.removeFromRight (104));
    r.removeFromRight (14);
    playerForce.setBounds (r.removeFromRight (juce::jmin (84, juce::jmax (0, r.getWidth() - 160))).reduced (0, 5));
    r.removeFromRight (4);
    playerMode.setBounds (r.removeFromRight (juce::jmin (176, juce::jmax (0, r.getWidth() - 8))));
}

void WalkView::visibilityChanged()
{
    if (isVisible())
    {
        seenWalkSerial = proc.walkSerial.load();
        actLight.setToggleState (proc.walkDark, juce::dontSendNotification);
        actAudience.setToggleState (proc.getAudience() != nullptr, juce::dontSendNotification);
        px = proc.walkPose[0].load(); py = proc.walkPose[1].load(); yaw = proc.walkPose[2].load(); pitch = proc.walkPose[3].load();
        yawT = yaw; pitchT = pitch; vx = vy = vz = turnV = 0.0f; fz = 0.0f; onGround = true;
        proc.engine.viewWalkOn = true;
        pushPose();
        updateButtons();
        lastTime = juce::Time::getMillisecondCounterHiRes();
        startTimerHz (60);          // (smooth looking around; the resolution adapts so a frame stays within ~8 ms of drawing)
        if (isShowing()) grabKeyboardFocus();
    }
    else
    {
        stopTimer();
        gpu.reset();                           // (the card's window goes with the page)
        looking = false;
        setImmersive (false);
        actGrab.midiHeld = actFreeze.midiHeld = actJump.midiHeld = false;
        updateActions (false, false);          // (nothing stays in the hand, nothing stays frozen)
        proc.engine.viewWalkOn = false;
        proc.engine.viewZ = 0.0f;
    }
}

void WalkView::pushPose()
{
    proc.walkPose[0] = px; proc.walkPose[1] = py; proc.walkPose[2] = yaw; proc.walkPose[3] = pitch;
    // (the ears left behind with H: the engine hears from there while you walk on)
    proc.engine.viewX = earsAway ? earPose[0] : px; proc.engine.viewY = earsAway ? earPose[1] : py;
    proc.engine.viewYaw = earsAway ? earPose[2] : yaw;
    proc.engine.viewZ = earsAway ? earPose[3] : fz;          // (a jump, a step of the model: the ears go up with it)
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

void WalkView::moveBy (float dx, float dy)
{
    if (dx == 0.0f && dy == 0.0f) return;
    const auto r = roomNow();
    const auto homeP = listenerPos (r);
    float x = homeP.x + px, y = homeP.y + py;
    float tx = x + dx, ty = y + dy;
    // a loaded model: its walls stop you (what is lower than a step - 45 cm - is climbed, see groundAt). Two
    // rays a body's width ahead, at the height of the shins and of the chest; blocked: along the wall instead.
    if (const auto ac = proc.getWalkAcoustics())
    {
        auto blockedTo = [&] (float bx, float by)
        {
            const float ddx = bx - x, ddy = by - y, l = std::sqrt (ddx * ddx + ddy * ddy);
            if (l < 1.0e-6f) return false;
            const float ext = (l + 0.3f) / l;
            for (float hgt : { 0.5f, 1.3f })
                if (ac->blocked ({ x, y, fz + hgt }, { x + ddx * ext, y + ddy * ext, fz + hgt })) return true;
            return false;
        };
        if (blockedTo (tx, ty))
        {
            if (! blockedTo (tx, y)) ty = y;
            else if (! blockedTo (x, ty)) tx = x;
            else { tx = x; ty = y; }
        }
    }
    // the Saal's walls hold you and you slide along them (as the listener of the room simulation does); never a jump
    if (r.box() || r.plan.n < 3) { x = juce::jlimit (0.3f, r.W() - 0.3f, tx); y = juce::jlimit (0.3f, r.L() - 0.3f, ty); }
    else
    {
        const auto geo = planGeo (r);
        if (! planSlide (geo, x, y, tx, ty, 0.3f)) { x = tx; y = ty; keepInsidePlan (geo, x, y, 0.3f); }   // (not inside yet: the room changed)
    }
    px = x - homeP.x; py = y - homeP.y;
}

/** what you stand on: the Saal's floor, or a floor / step of the model below your knees */
float WalkView::groundAt (float x, float y) const
{
    const auto ac = proc.getWalkAcoustics();
    if (ac == nullptr) return 0.0f;
    const float from = fz + 0.5f;                       // (the height a step may have)
    float t;
    if (ac->cast ({ x, y, from }, { 0.0f, 0.0f, -1.0f }, from + 0.5f, t) >= 0) return juce::jmax (0.0f, from - t);
    return 0.0f;                                        // (nothing of the model below: the Saal's floor underneath)
}

void WalkView::step (double dt, bool fwd, bool bwd, bool left, bool right, float turn, bool fast)
{
    yaw = yawT = wrapAz (yaw + turn);
    const float v = (fast ? 4.2f : 1.6f) * (float) dt;
    const float ya = juce::degreesToRadians (yaw);
    const float fx = -std::sin (ya), fy = std::cos (ya), rx = std::cos (ya), ry = std::sin (ya);
    float dx = 0.0f, dy = 0.0f;
    if (fwd)   { dx += fx; dy += fy; }
    if (bwd)   { dx -= fx; dy -= fy; }
    if (right) { dx += rx; dy += ry; }
    if (left)  { dx -= rx; dy -= ry; }
    const float l = std::sqrt (dx * dx + dy * dy);
    if (l > 0.0f) moveBy (dx / l * v, dy / l * v);
    pushPose();
}

void WalkView::advance (double dtD, bool fwd, bool bwd, bool left, bool right, float turnKeys, bool fast, bool jump)
{
    const float dt = (float) dtD;
    if (dt <= 0.0f) return;
    auto ease = [dt] (float tau) { return 1.0f - std::exp (-dt / tau); };

    // ---- looking: the view follows where the mouse points, quickly but without the jerk of single mouse steps;
    //      the turning keys build up a turning speed
    turnV += (90.0f * turnKeys - turnV) * ease (0.09f);
    if (std::abs (turnV) < 0.05f && turnKeys == 0.0f) turnV = 0.0f;
    yawT = wrapAz (yawT + turnV * dt);
    yaw = wrapAz (yaw + turnV * dt);
    yaw = wrapAz (yaw + wrapAz (yawT - yaw) * ease (0.035f));
    pitch += (pitchT - pitch) * ease (0.035f);

    // ---- walking: the speed builds up and dies away (in the air there is little to push against)
    const float ya = juce::degreesToRadians (yaw);
    const float fx = -std::sin (ya), fy = std::cos (ya), rx = std::cos (ya), ry = std::sin (ya);
    float dx = 0.0f, dy = 0.0f;
    if (fwd)   { dx += fx; dy += fy; }
    if (bwd)   { dx -= fx; dy -= fy; }
    if (right) { dx += rx; dy += ry; }
    if (left)  { dx -= rx; dy -= ry; }
    const float l = std::sqrt (dx * dx + dy * dy);
    const float speed = (fast ? 4.2f : 1.6f) * controls().walk;
    const float tvx = l > 0.0f ? dx / l * speed : 0.0f, tvy = l > 0.0f ? dy / l * speed : 0.0f;
    const float k = ease (onGround ? (l > 0.0f ? 0.12f : 0.16f) : 0.7f);
    vx += (tvx - vx) * k; vy += (tvy - vy) * k;
    if (l == 0.0f && vx * vx + vy * vy < 0.0004f) vx = vy = 0.0f;
    if (vx != 0.0f || vy != 0.0f)
    {
        const float x0 = px, y0 = py;
        moveBy (vx * dt, vy * dt);
        // (against a wall: the speed into it is gone, the speed along it stays)
        const float ax = (px - x0) / dt, ay = (py - y0) / dt;
        if (std::abs (ax - vx) > 0.02f || std::abs (ay - vy) > 0.02f) { vx = ax; vy = ay; }
    }

    // ---- up and down: a jump, falling, the steps of a model
    const auto r = roomNow();
    const auto ac = proc.getWalkAcoustics();
    const float homeX = listenerPos (r).x + px, homeY = listenerPos (r).y + py;
    const float ground = groundAt (homeX, homeY);
    if (jump && onGround) { vz = 4.6f; onGround = false; }
    if (! onGround || fz > ground + 0.02f)
    {
        vz -= 12.5f * dt;                     // (a little heavier than the earth: a jump that does not float)
        fz += vz * dt;
        onGround = false;
        if (fz <= ground)
        {
            landDip = juce::jlimit (0.0f, 0.12f, -vz * 0.016f);
            fz = ground; vz = 0.0f; onGround = true;
        }
    }
    else fz += (ground - fz) * ease (0.05f);  // (a step up: lifted, not teleported)
    {
        // the head stays below the ceiling (the Saal's, or the higher model's) - and below what the model has there
        const float ear = listenerHeight (r);
        float top = juce::jmax (r.H(), ac != nullptr ? ac->size.z : 0.0f) - 0.2f;
        if (ac != nullptr && vz > 0.0f)
        {
            float t;
            if (ac->cast ({ homeX, homeY, fz + ear }, { 0.0f, 0.0f, 1.0f }, 0.3f + vz * dt, t) >= 0) top = juce::jmin (top, fz + ear + t - 0.2f);
        }
        if (fz + ear > top) { fz = juce::jmax (0.0f, top - ear); vz = juce::jmin (vz, 0.0f); }
    }
    landDip *= std::exp (-dt / 0.09f);

    // ---- the steps: a slight sway of the view while walking on the ground
    const float sp = std::sqrt (vx * vx + vy * vy);
    bobAmt += ((onGround && sp > 0.3f ? juce::jmin (1.0f, sp / 1.6f) : 0.0f) - bobAmt) * ease (0.15f);
    if (onGround) bobPhase += sp * dt * 4.4f;             // (a step every 70 cm or so)
    if (bobPhase > 1000.0f) bobPhase -= 1000.0f;
    pushPose();
}

void WalkView::keepInside()
{
    const auto r = roomNow();
    const auto homeP = listenerPos (r);
    float x = homeP.x + px, y = homeP.y + py;
    if (r.box()) { x = juce::jlimit (0.3f, r.W() - 0.3f, x); y = juce::jlimit (0.3f, r.L() - 0.3f, y); }
    else keepInsidePlan (planGeo (r), x, y, 0.3f);
    px = x - homeP.x; py = y - homeP.y;
}

void WalkView::stepForTest (double dt, bool f, bool b, bool l, bool r, float turnDeg) { step (dt, f, b, l, r, turnDeg, false); }

void WalkView::timerCallback()
{
    const double t = juce::Time::getMillisecondCounterHiRes();
    const double dt = juce::jlimit (0.0, 0.1, (t - lastTime) * 0.001);
    lastTime = t;
    if (proc.walkSerial.load() != seenWalkSerial)
    {
        // a patch brought a room (or the light): stand where it says, show what it set
        seenWalkSerial = proc.walkSerial.load();
        px = proc.walkPose[0].load(); py = proc.walkPose[1].load(); yaw = proc.walkPose[2].load(); pitch = proc.walkPose[3].load();
        yawT = yaw; pitchT = pitch; vx = vy = vz = turnV = 0.0f; fz = 0.0f; onGround = true;
        pushPose();
        actLight.setToggleState (proc.walkDark, juce::dontSendNotification);
        updateButtons();
        if (auto m = proc.getWalkMesh()) { status = m->name; statusUntil = t + 3000; }
    }
    const bool grabHeld = actGrab.held(), freezeHeld = actFreeze.held(), jumpHeld = actJump.held();
    if (immersive && ! testMode)
    {
        // without the keyboard the mouse is let go at once, and the view is left a moment later
        if (hasKeyboardFocus (false)) { unfocused = 0; pollLockedMouse(); }
        else if (++unfocused > 3) setImmersive (false);
    }
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
        const bool fast = juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
        advance (dt, f, b, l, r, turn / (90.0f * (float) juce::jmax (1.0e-4, dt)), fast, down (juce::KeyPress::spaceKey) || jumpHeld);
        updateActions (down ('Q') || down ('q') || grabHeld, down ('F') || down ('f') || freezeHeld);
        { const bool tab = down (juce::KeyPress::tabKey); if (tab != helpHeld) { helpHeld = tab; repaint(); } }
        {
            const auto m = juce::ModifierKeys::getCurrentModifiersRealtime();
            setSlow ((down ('Z') || down ('z')) && ! m.isCtrlDown() && ! m.isCommandDown());
        }

    }
    else
    {
        advance (dt, false, false, false, false, 0.0f, false, jumpHeld);      // (keys let go by losing the focus: coming to rest, landing)
        updateActions (grabHeld, freezeHeld);
        if (helpHeld) { helpHeld = false; repaint(); }
        setSlow (false);
        if (magnet != 0) setMagnet (0);
    }
    updateGpu();
    if (! gpuOn()) { overWasCard = false; repaint(); return; }
    buildScene ((float) gpu->renderingScale());
    // The card draws the picture every frame; what JUCE paints over it (writing, controls, the map) is kept as
    // a picture of its own and only painted again where it changes - painting all of it 60 times a second would
    // keep one core busy.
    ++overTick;
    const bool shockNow = shockFx > 0.0 && t - shockFx < 600.0;
    const int cross = (aimStop >= 0 || heldStop >= 0) ? 1 : 0;
    if (! overWasCard || shockNow || overShock || overTick % 120 == 0 || overlayBusy() || overText.isEmpty() != overTextWasEmpty)
        repaint();
    else
    {
        const auto area = getLocalBounds();
        if (cross != overCross) repaint (juce::Rectangle<int> (48, 48).withCentre (area.getCentre()));
        if (overTick % 6 == 0 && ! immersive)
        {
            repaint (area.withTrimmedTop (80).removeFromTop (48));                                     // the lines that say where you are and what happened
            repaint (area.withTrimmedBottom (56).removeFromBottom (22));                               // the hint about what is heard
            repaint (juce::Rectangle<int> (198, 158).withPosition (area.getRight() - 210, area.getBottom() - 190));   // the map
        }
        if (overTick % 30 == 0 && overText.isNotEmpty()) repaint (area.withTrimmedLeft (area.getWidth() - 560).withTrimmedTop (150).removeFromTop (136));
    }
    overWasCard = true; overShock = shockNow; overCross = cross; overTextWasEmpty = overText.isEmpty();
}

// ---------------------------------------------------------------------------
// the picture from the graphics card
// ---------------------------------------------------------------------------
namespace
{
juce::File gpuChoiceFile() { return RysumProcessor::getPatchFolder().getParentDirectory().getChildFile ("Grafik.xml"); }
int gpuChoice = -1;          // -1 = not read yet
}

bool WalkView::gpuWanted()
{
    const auto forced = juce::SystemStats::getEnvironmentVariable ("RYSUM_GPU", {});
    if (forced.isNotEmpty()) return forced.getIntValue() != 0;
    if (gpuChoice < 0)
    {
        gpuChoice = 1;
        if (auto xml = juce::parseXML (gpuChoiceFile())) gpuChoice = xml->getBoolAttribute ("grafikkarte", true) ? 1 : 0;
    }
    return gpuChoice != 0;
}

void WalkView::setGpuWanted (bool on)
{
    gpuChoice = on ? 1 : 0;
    juce::XmlElement xml ("GRAFIK");
    xml.setAttribute ("grafikkarte", on);
    xml.writeTo (gpuChoiceFile());
}

void WalkView::updateGpu()
{
    if (gpu != nullptr && gpu->hasFailed())
    {
        // the card or its driver cannot do it: the processor draws from now on
        gpuNote = gpu->failure().upToFirstOccurrenceOf (juce::String::charToString ((juce::juce_wchar) 10), false, false).substring (0, 70).trim();
        gpuGaveUp = true;
        gpu.reset();
        return;
    }
    const bool want = gpuWanted() && ! testMode && ! gpuBlocked && ! gpuGaveUp && isShowing() && getPeer() != nullptr;
    if (! want) { gpu.reset(); gpuWaiting = 0; return; }
    if (gpu == nullptr) { gpu = std::make_unique<GpuPicture>(); gpuWaiting = 0; gpuFrames = 0; }
    gpu->attachTo (*this);
    GpuPicture::letInputThrough (*this);
    if (! gpu->isReady() && ++gpuWaiting > 240)
    {
        // no context after four seconds (no OpenGL 3.2 here)
        gpuNote = "kein OpenGL 3.2";
        gpuGaveUp = true;
        gpu.reset();
    }
}

juce::String WalkView::pictureInfo() const
{
    if (gpuOn()) return "Grafikkarte (" + gpu->cardName() + "), " + juce::String (gpu->framesPerSecond(), 1) + " Bilder/s";
    return "Prozessor" + (gpuNote.isNotEmpty() ? " (Grafikkarte: " + gpuNote + ")" : juce::String());
}

bool WalkView::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        if (selectedCount() > 0) { clearSelection(); say (U ("Auswahl leer")); return true; }
        if (immersive) setImmersive (false);
        else if (onBack) onBack();
        return true;
    }
    {
        // arranging: layouts on F1 to F8 (Ctrl: keep), undo, the formations, the list of the keys
        const auto mods = k.getModifiers();
        const bool ctrlKey = mods.isCtrlDown() || mods.isCommandDown();
        const int kc = k.getKeyCode();
        for (int i = 0; i < 8; ++i)
            if (kc == juce::KeyPress::F1Key + i) { if (ctrlKey) storeLayout (i); else recallLayout (i); return true; }
        if (ctrlKey && (kc == 'Z' || kc == 'z')) { undo(); return true; }
        if (kc == juce::KeyPress::tabKey) return true;      // (held: see the timer)
        if (! ctrlKey && ! mods.isAltDown() && kc >= '1' && kc <= '5') { formation (kc - '0'); return true; }
        if (kc == 'K' || kc == 'k')
        {
            if (ctrlKey) clearLanterns(); else if (mods.isShiftDown()) removeLantern(); else placeLantern();
            return true;
        }
        if ((kc == 'M' || kc == 'm') && mods.isAltDown()) { materialAll(); return true; }
        if (! ctrlKey && (kc == 'T' || kc == 't')) { whirl (mods.isShiftDown() ? 1.7f : 1.0f); return true; }
        if (! ctrlKey && (kc == 'P' || kc == 'p')) { setPause (! paused); return true; }
        if (! ctrlKey && (kc == 'H' || kc == 'h')) { leaveEars (! earsAway); return true; }
        if (! ctrlKey && (kc == 'Z' || kc == 'z')) return true;      // (held: slow motion, see the timer)
    }
    const auto c = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());
    if (c == 'i' && ! k.getModifiers().isCtrlDown() && ! k.getModifiers().isCommandDown()) { toggleImmersive(); return true; }
    if (c == 'm') { cycleMaterial(); return true; }
    if (c == 'l') { setDark (! proc.walkDark); return true; }
    if (c == 'g' && ! k.getModifiers().isCtrlDown() && ! k.getModifiers().isCommandDown())
    {
        // the picture from the graphics card or from the processor
        setGpuWanted (! gpuWanted());
        gpuGaveUp = false; gpuNote = {};
        return true;
    }
    const bool hard = juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
    if (c == 'e') { shock (hard ? 1.7f : 1.0f); return true; }
    if (c == 'r') { callHome(); return true; }
    if (c == 'c') { clap(); return true; }
    if (c == 'w' || c == 'a' || c == 's' || c == 'd' || c == 'q' || c == 'f' || c == ' ') return true;
    const int code = k.getKeyCode();
    if (code == juce::KeyPress::spaceKey) return true;
    return code == juce::KeyPress::upKey || code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey;
}

// (the walking keys are also the computer keyboard's notes: here they walk, the notes come from MIDI or "Halten")
bool WalkView::keyStateChanged (bool) { return true; }

void WalkView::focusLost (FocusChangeType) { looking = false; bowArmed = false; bowStop(); setSlow (false); }

void WalkView::setDark (bool on)
{
    proc.walkDark = on;
    actLight.setToggleState (on, juce::dontSendNotification);
    status = on ? U ("Licht aus: die Pfeifen leuchten") : U ("Licht an");
    statusUntil = juce::Time::getMillisecondCounterHiRes() + 2000;
    repaint();
}

// ---------------------------------------------------------------------------
// "Immersiv": only the picture, the mouse looks around by itself
// ---------------------------------------------------------------------------
void WalkView::toggleImmersive()
{
    if (immersive) { setImmersive (false); return; }
    if (! isShowing() && ! testMode) return;      // (a MIDI button pressed while another page is shown: nothing)
    // (the window may go to the full screen first: the keyboard wanders while it does, so the view starts after it)
    if (prepareImmersive && prepareImmersive())
        juce::Timer::callAfterDelay (160, [safe = juce::Component::SafePointer<WalkView> (this)] {
            if (safe == nullptr) return;
            if (safe->isShowing()) safe->setImmersive (true);
            else if (safe->onImmersive) safe->onImmersive (false);      // (the page was left meanwhile: the window goes back)
        });
    else
        setImmersive (true);
}

void WalkView::setImmersive (bool on)
{
    if (on == immersive) return;
    if (on && ! isShowing() && ! testMode) return;      // (never on a page that is not shown: its controls would stay hidden)
    immersive = on;
    looking = false;
    for (auto* c : getChildren()) c->setVisible (! on);
    setMouseCursor (on && ! testMode ? juce::MouseCursor::NoCursor : juce::MouseCursor::CrosshairCursor);
    lockFresh = true;
    unfocused = 0;
    if (onImmersive) onImmersive (on);
    if (on && isShowing()) grabKeyboardFocus();
    repaint();
}

void WalkView::pollLockedMouse()
{
    if (! immersive || testMode || ! hasKeyboardFocus (false)) return;
    // the pointer's way since the last look, on the screen (so the full screen's enlargement does not change the
    // speed); it is hidden, and put back to the middle before it could leave the view
    auto mouse = juce::Desktop::getInstance().getMainMouseSource();
    const auto now = mouse.getScreenPosition();
    if (! lockFresh)
    {
        const auto d = now - lockRef;
        if (std::abs (d.x) + std::abs (d.y) > 0.01f)
        {
            const auto cs = controls();
            yawT = wrapAz (yawT - d.x * 0.15f * cs.mouse);
            pitchT = juce::jlimit (-85.0f, 85.0f, pitchT - d.y * 0.15f * cs.mouse * (cs.invertY ? -1.0f : 1.0f));
        }
    }
    lockFresh = false;
    lockRef = now;
    const auto centre = getScreenBounds().getCentre().toFloat();
    if (now.getDistanceFrom (centre) > 60.0f)
    {
        mouse.setScreenPosition (centre);
        lockRef = mouse.getScreenPosition();      // (where it really landed: no drift from rounding)
    }
}

void WalkView::mouseMove (const juce::MouseEvent&) { pollLockedMouse(); }

void WalkView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    const bool ctrl = e.mods.isCtrlDown() || e.mods.isCommandDown();
    const auto centre = getLocalBounds().getCentre().toFloat();
    if (e.mods.isMiddleButtonDown())
    {
        // the middle button: the pipe there is pinned (or free again)
        int s = -1, k = -1;
        if (pickPipe (immersive ? centre : e.position, 40.0f, s, k)) togglePin (s, k);
        return;
    }
    if (e.mods.isPopupMenu() && heldStop >= 0) { setDown(); return; }      // (a pipe in the hand: set down there)
    if (e.mods.isPopupMenu())
    {
        // the right button held: the magnet (moving the mouse meanwhile still looks around)
        setMagnet (e.mods.isShiftDown() ? -1 : 1);
        if (immersive) return;
        looking = true;
        lastMouse = downPos = e.position;
        dragged = 0.0f;
        e.source.enableUnboundedMouseMovement (true);
        return;
    }
    if (immersive)
    {
        // no pointer: everything happens at the cross
        int s = -1, k = -1;
        if (ctrl) { if (pickPipe (centre, 40.0f, s, k)) selectPipe (s, k); return; }
        if (e.mods.isAltDown()) { sweeping = true; sweepTo (centre, centre, 1.0f); return; }
        // (a knock when it is let go soon; held on the pipe, it sounds: see the timer)
        bowArmed = true; bowFrom = centre; bowDownMs = juce::Time::getMillisecondCounterHiRes(); dragged = 0.0f;
        return;
    }
    lastMouse = downPos = e.position;
    dragged = 0.0f;
    if (e.mods.isAltDown())
    {
        // Alt+drag: over the pipes like a hand over strings
        sweeping = true;
        sweepLast = e.position;
        sweepLastMs = juce::Time::getMillisecondCounterHiRes();
        sweepTo (e.position, e.position, 1.0f);
        return;
    }
    if (ctrl) { boxing = true; boxFrom = boxTo = e.position; return; }      // (a click chooses one pipe, a drag a frame)
    looking = true;
    bowArmed = true; bowFrom = e.position; bowDownMs = juce::Time::getMillisecondCounterHiRes();
    e.source.enableUnboundedMouseMovement (true);
}

void WalkView::mouseDrag (const juce::MouseEvent& e)
{
    if (immersive)
    {
        pollLockedMouse();
        if (sweeping) { const auto c = getLocalBounds().getCentre().toFloat(); sweepTo (c, c, 1.0f); }
        return;
    }
    if (sweeping)
    {
        // as hard as the pointer is fast
        const double now = juce::Time::getMillisecondCounterHiRes();
        const float speed = e.position.getDistanceFrom (sweepLast) / (float) juce::jmax (1.0, now - sweepLastMs);
        sweepTo (sweepLast, e.position, juce::jlimit (0.4f, 1.7f, 0.35f + 0.5f * speed));
        sweepLast = e.position;
        sweepLastMs = now;
        return;
    }
    if (boxing)
    {
        dragged += std::abs (e.position.x - boxTo.x) + std::abs (e.position.y - boxTo.y);
        boxTo = e.position;
        repaint();
        return;
    }
    if (! looking) return;
    const auto d = e.position - lastMouse;
    lastMouse = e.position;
    dragged += std::abs (d.x) + std::abs (d.y);
    // (where to look; the view gets there within a few hundredths of a second: see advance)
    const auto cs = controls();
    yawT = wrapAz (yawT - d.x * 0.22f * cs.mouse);
    pitchT = juce::jlimit (-85.0f, 85.0f, pitchT - d.y * 0.22f * cs.mouse * (cs.invertY ? -1.0f : 1.0f));
}

void WalkView::mouseUp (const juce::MouseEvent& e)
{
    bowArmed = false;
    if (bowStop_ >= 0 && ! e.mods.isPopupMenu() && ! e.mods.isMiddleButtonDown())
    {
        bowStop();
        if (looking) e.source.enableUnboundedMouseMovement (false);
        looking = false;
        return;
    }
    if (e.mods.isPopupMenu())
    {
        setMagnet (0);
        if (looking) e.source.enableUnboundedMouseMovement (false);
        looking = false;
        return;
    }
    if (sweeping) { sweeping = false; return; }
    if (boxing)
    {
        boxing = false;
        int s = -1, k = -1;
        if (dragged < 5.0f) { if (pickPipe (downPos, 40.0f, s, k)) selectPipe (s, k); repaint(); return; }
        // every pipe drawn inside the frame is added to the choice
        const auto box = juce::Rectangle<float> (boxFrom, boxTo);
        const float isc = rast.width() > 0 ? (float) getWidth() / (float) rast.width() : 1.0f;
        int added = 0;
        for (int st = 0; st < kNumStops; ++st)
            for (int ky = 0; ky < kNumKeys; ++ky)
            {
                const auto& sh = shown[(size_t) st][(size_t) ky];
                float sx, sy, d;
                if (! sh.on || ! rast.project (sh.c, sx, sy, d) || ! box.contains (sx * isc, sy * isc)) continue;
                if (! isSelected (st, ky)) ++added;
                sel[(size_t) st] |= (uint64_t) 1 << ky;
            }
        say (juce::String (added) + U (" Pfeifen dazu, ") + juce::String (selectedCount()) + U (" gew\xc3\xa4hlt  \xc2\xb7  1 bis 5: aufstellen, rechte Maustaste: Magnet, Esc: Auswahl leeren"), 3500.0);
        repaint();
        return;
    }
    if (immersive)
    {
        looking = false;
        if (! e.mods.isMiddleButtonDown()) strikeAt (getLocalBounds().getCentre().toFloat(), e.mods.isShiftDown() ? 1.7f : 1.0f);
        return;
    }
    if (! looking) return;
    looking = false;
    e.source.enableUnboundedMouseMovement (false);
    // a click (not a look around): a knock on the pipe there
    if (dragged < 5.0f) strikeAt (downPos, e.mods.isShiftDown() ? 1.7f : 1.0f);
}

// ---------------------------------------------------------------------------
// the player's hands
// ---------------------------------------------------------------------------
void WalkView::say (const juce::String& text, double ms)
{
    status = text;
    statusUntil = juce::Time::getMillisecondCounterHiRes() + ms;
}

V3 WalkView::eyeNow() const
{
    const auto r = roomNow();
    const auto hp = listenerPos (r);
    return { hp.x + px, hp.y + py, hp.z + fz };
}

V3 WalkView::lookDir() const
{
    const float ya = juce::degreesToRadians (yaw), pa = juce::degreesToRadians (juce::jlimit (-89.0f, 89.0f, pitch));
    return { -std::sin (ya) * std::cos (pa), std::cos (ya) * std::cos (pa), std::sin (pa) };
}

/** the pipe drawn at this point of the view (the nearest one, not hidden behind a wall) */
bool WalkView::pickPipe (juce::Point<float> at, float maxDist, int& stop, int& key) const
{
    if (rast.width() <= 0 || viewScale <= 0.0f) return false;
    const float qx = at.x / viewScale, qy = at.y / viewScale;
    float best = 1.0e30f;
    bool found = false;
    for (int s = 0; s < kNumStops; ++s)
        for (int k = 0; k < kNumKeys; ++k)
        {
            const auto& sh = shown[(size_t) s][(size_t) k];
            if (! sh.on) continue;
            float cx, cy, cd;
            if (! rast.project (sh.c, cx, cy, cd) || cd > maxDist || cd >= best) continue;
            float ax = cx, ay = cy, ad, bx = cx, by = cy, bd;
            const V3 half { 0.0f, 0.0f, 0.5f * sh.len };
            float tx, ty;
            if (rast.project (sh.c - half, tx, ty, ad)) { ax = tx; ay = ty; }
            if (rast.project (sh.c + half, tx, ty, bd)) { bx = tx; by = ty; }
            // how far the point is from the pipe's axis on the screen
            const float ex = bx - ax, ey = by - ay, l2 = ex * ex + ey * ey;
            const float u = l2 > 1.0e-6f ? juce::jlimit (0.0f, 1.0f, ((qx - ax) * ex + (qy - ay) * ey) / l2) : 0.0f;
            const float dx = qx - (ax + u * ex), dy = qy - (ay + u * ey);
            const float radPx = sh.rad / juce::jmax (0.1f, cd) * 0.5f * (float) rast.width();
            const float tol = juce::jmax (1.5f * radPx, 7.0f / viewScale);
            if (dx * dx + dy * dy > tol * tol) continue;
            if (cx >= 0.0f && cy >= 0.0f && cx < (float) rast.width() && cy < (float) rast.height())
            {
                const float zd = rast.depthAt ((int) cx, (int) cy);
                if (zd > 0.0f && zd < cd * 0.95f - 0.4f) continue;         // (behind a wall)
            }
            best = cd; stop = s; key = k; found = true;
        }
    return found;
}

/** the pipes of this stop become bodies in the Saal (so they can be pushed, carried, thrown): a stop that
    stands still gets the motion "Schweben"; one that moves in another way is left as it is */
bool WalkView::makeHover (int stop, bool tell)
{
    if (stop < 0 || stop >= kNumStops) return false;
    const int m = proc.readParams().motionMode[(size_t) stop];
    if (isRoomMotion (m)) return true;
    if (m != (int) Motion::Static)
    {
        if (tell) say (U ("Dieses Register bewegt sich anders (") + motionNames()[m] + U ("): zum Greifen die Bewegung \xe2\x80\x9eSchweben\xe2\x80\x9c w\xc3\xa4hlen (Raum-Seite)"), 4000.0);
        return false;
    }
    if (auto* prm = proc.apvts.getParameter (RysumProcessor::motionParamId (stop)))
    {
        prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (int) Motion::Hover));
        if (tell) say (U ("Register ") + juce::String (stop + 1) + U (": Bewegung jetzt \xe2\x80\x9eSchweben\xe2\x80\x9c"));
        return true;
    }
    return false;
}

void WalkView::shock (float strength)
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastShock < 300.0) return;                 // (a held key: three bursts a second, not thirty)
    pushUndo();
    lastShock = now;
    // the stops that stand still start to hover, so there is something to push
    const auto params = proc.readParams();
    int bodies = 0, woken = 0;
    for (int s = 0; s < kNumStops; ++s)
    {
        if (! params.stopOn[(size_t) s]) continue;
        if (isRoomMotion (params.motionMode[(size_t) s])) ++bodies;
        else if (makeHover (s, false)) ++woken;
    }
    shockAmt = strength;
    shockFx = now;
    if (woken > 0)
    {
        shockAt = now + 90.0;                            // (once their pipes are bodies: a few blocks from now)
        say (juce::String (woken) + U (" Register schweben jetzt (Bewegung \xe2\x80\x9eSchweben\xe2\x80\x9c) \xe2\x80\x93 die Druckwelle treibt sie fort, sie kehren zur\xc3\xbc" "ck"), 4000.0);
    }
    else
    {
        proc.engine.playerAct (OrganEngine::Act::Shock, -1, -1, {}, strength);
        if (bodies == 0) say (U ("Druckwelle: kein Register, dessen Pfeifen frei im Saal sind (Bewegung Schweben, Flug, Fall, Schwarm, Str\xc3\xb6mung)"), 4000.0);
    }
}

void WalkView::callHome()
{
    pushUndo();
    proc.engine.playerAct (OrganEngine::Act::Home);
    proc.engine.pipeLenReset = true;      // (and every broken or grown pipe is whole again)
    say (U ("Alle Pfeifen fliegen an ihren Platz zur\xc3\xbc" "ck"));
}

void WalkView::resetView()
{
    setImmersive (false);
    clearSelection();
    setMagnet (0);
    for (auto& m : proc.engine.pinMask) m = 0;
    if (paused) setPause (false);
    setSlow (false);
    leaveEars (false);
    bowStop();
    fov = 80.0f;
    proc.engine.playerAct (OrganEngine::Act::Home);
    proc.engine.pipeLenReset = true;
    updateButtons();
    say (U ("Alles zur\xc3\xbc" "ckgesetzt"));
    repaint();
}

void WalkView::clap()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastClap < 160.0) return;
    lastClap = now;
    proc.engine.playerAct (OrganEngine::Act::Clap);
}

bool WalkView::strikeAt (juce::Point<float> at, float strength)
{
    int s = -1, k = -1;
    if (! pickPipe (at, 40.0f, s, k)) return false;
    const auto& sh = shown[(size_t) s][(size_t) k];
    proc.engine.playerAct (OrganEngine::Act::Strike, s, k, { sh.c.x, sh.c.y, sh.c.z }, strength);
    glow[(size_t) s][(size_t) k] = 1.0f;
    return true;
}

void WalkView::setGrab (bool on, bool fast)
{
    auto& en = proc.engine;
    if (on)
    {
        if (heldStop >= 0) return;
        if (aimStop < 0) { say (U ("Keine Pfeife im Blick (das Kreuz auf eine Pfeife richten, h\xc3\xb6" "chstens 9 m entfernt)")); return; }
        if (! makeHover (aimStop, true)) return;
        heldStop = aimStop; heldKey = aimKey;
        const V3 eye = eyeNow();
        const auto& sh = shown[(size_t) heldStop][(size_t) heldKey];
        holdDist = juce::jlimit (1.2f, 3.0f, std::sqrt (dot (sh.c - eye, sh.c - eye)));
        updateActions (true, frozen);
        return;
    }
    if (heldStop < 0) return;
    if (en.grabStop.load() >= 0)
    {
        // let go: it flies on with the hand's speed, your own, and a push ahead
        pushUndo();
        const V3 d = lookDir();
        const float push = fast ? 9.0f : 4.5f;
        en.playerAct (OrganEngine::Act::Throw, heldStop, heldKey, { d.x * push + vx, d.y * push + vy, d.z * push + juce::jmax (0.0f, vz) });
        en.grabStop = -1; en.grabKey = -1;
    }
    heldStop = heldKey = -1;
}

void WalkView::setFreeze (bool on)
{
    frozen = on;
    proc.engine.actFreeze = on;
}

void WalkView::updateActions (bool qDown, bool fDown)
{
    auto& en = proc.engine;
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (fDown != frozen) setFreeze (fDown);
    if (frozen) say (U ("Angehalten \xe2\x80\x93 solange F gehalten wird"), 250.0);
    if (qDown != qWas)
    {
        qWas = qDown;
        setGrab (qDown, juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown());
    }
    if (heldStop >= 0)
    {
        // the hand: ahead of the eyes (the pipe is taken as soon as it is a body in the Saal)
        const V3 eye = eyeNow(), d = lookDir();
        en.grabX = eye.x + d.x * holdDist; en.grabY = eye.y + d.y * holdDist; en.grabZ = juce::jmax (0.3f, eye.z + d.z * holdDist);
        Vec3f b;
        if (en.grabStop.load() < 0 && en.bodyAt (heldStop, heldKey, b)) { en.grabKey = heldKey; en.grabStop = heldStop; }
    }
    if (bowArmed && bowStop_ < 0 && dragged < 5.0f && now - bowDownMs > 280.0)
    {
        // held still on a pipe: it sounds until the button is let go
        bowArmed = false;
        int s = -1, k = -1;
        if (pickPipe (bowFrom, 40.0f, s, k)) bowStart (s, k);
    }
    if (whirlAgain > 0.0 && now >= whirlAgain)
    {
        whirlAgain = 0.0;
        en.playerAct (OrganEngine::Act::Whirl, -1, -1, { whirlPoint.x, whirlPoint.y, whirlPoint.z }, whirlStrength);
    }
    if (paused) say (U ("Pause \xe2\x80\x93 P: weiter"), 250.0);
    else if (slow) say (U ("Zeitlupe \xe2\x80\x93 solange Z gehalten wird"), 250.0);
    if (magnet != 0)
    {
        const V3 p = eyeNow() + lookDir() * magnetDist;
        en.magnetX = p.x; en.magnetY = p.y; en.magnetZ = juce::jmax (0.3f, p.z);
    }
    if (arrangeUntil > 0.0 && now - arrangeLast > 60.0)
    {
        bool waiting = false;
        for (const auto& m : en.goMask) waiting = waiting || m.load() != 0;
        if (waiting && now < arrangeUntil) { en.playerAct (OrganEngine::Act::Arrange); arrangeLast = now; }
        else arrangeUntil = 0.0;
    }
    if (shockAt > 0.0 && now >= shockAt)
    {
        shockAt = 0.0;
        en.playerAct (OrganEngine::Act::Shock, -1, -1, {}, shockAmt);
    }
}

void WalkView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    // a pipe in the hand: the wheel brings it nearer (right up to the ear: "Fluestern") or holds it further away
    if (heldStop >= 0) { holdDist = juce::jlimit (0.2f, 4.0f, holdDist * std::exp (w.deltaY * 1.2f)); return; }
    if (magnet != 0) { magnetDist = juce::jlimit (0.8f, 14.0f, magnetDist * std::exp (w.deltaY * 1.2f)); return; }
    {
        const float dw = w.deltaY != 0.0f ? w.deltaY : w.deltaX;
        const auto mods = juce::ModifierKeys::getCurrentModifiersRealtime();
        if (mods.isCtrlDown() || mods.isCommandDown()) { scaleRoom (std::exp (dw * 0.6f)); return; }
        if (mods.isShiftDown())
        {
            auto cs = controls();
            cs.walk = juce::jlimit (0.4f, 2.5f, cs.walk * std::exp (dw * 0.8f));
            setControls (cs);
            say (U ("Gehtempo \xc3\x97 ") + juce::String (cs.walk, 2));
            return;
        }
    }
    fov = juce::jlimit (40.0f, 110.0f, fov - w.deltaY * 30.0f);
    repaint();
}

void WalkView::loadBuiltInRoom (int index)
{
    const auto& rooms = builtInRooms();
    if (index < 0 || index >= (int) rooms.size()) return;
    const auto f = writeBuiltInRoom (index, RysumProcessor::getPatchFolder().getParentDirectory().getChildFile ("Modelle"));
    if (! f.existsAsFile())
    {
        status = U ("Konnte das Modell nicht schreiben: ") + f.getFullPathName();
        statusUntil = juce::Time::getMillisecondCounterHiRes() + 5000;
        repaint();
        return;
    }
    // they are made z up, in metres; the Saal takes their size, the listener their good place
    proc.walkZUp = true;
    zUp.setToggleState (true, juce::dontSendNotification);
    proc.walkScale = 1.0f;
    units.setSelectedItemIndex (0, juce::dontSendNotification);
    loadModel (f);
    if (proc.getWalkMesh() == nullptr) return;
    const auto keep = status;
    if (fit.onClick) fit.onClick();
    auto set = [this] (const char* key, float v)
    {
        if (auto* prm = proc.apvts.getParameter (RysumProcessor::roomParamId (0, key))) prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    };
    set ("Lx", rooms[(size_t) index].listenX);
    set ("Ly", rooms[(size_t) index].listenY);
    status = keep.upToFirstOccurrenceOf (U ("  \xc2\xb7  "), false, false);
    statusUntil = juce::Time::getMillisecondCounterHiRes() + 5000;
    if (home.onClick) home.onClick();
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
        // the Saal's shoebox lies underneath (early reflections, the outer limit): say so when the sizes differ
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
    std::vector<std::array<P2, 3>> planTris;
    if (! box) triangulatePlan (r.plan, W, L, planTris);
    for (float x = x0; x < x1 - 1.0e-3f; x += tile)
        for (float y = y0; y < y1 - 1.0e-3f; y += tile)
        {
            const float xe = juce::jmin (x1, x + tile), ye = juce::jmin (y1, y + tile);
            if (box)
            {
                quad (vt ({ x, y, 0 }, nz, x, y), vt ({ xe, y, 0 }, nz, xe, y), vt ({ xe, ye, 0 }, nz, xe, ye), vt ({ x, ye, 0 }, nz, x, ye), 1, 1.0f);
                quad (vt ({ x, y, H }, nz, x, y), vt ({ xe, y, H }, nz, xe, y), vt ({ xe, ye, H }, nz, xe, ye), vt ({ x, ye, H }, nz, x, ye), 2, 1.0f);
                continue;
            }
            // a shaped Saal: every field cut to the outline, so floor and ceiling reach every wall (whole fields
            // kept or left out by their centre left holes along slanted walls, and stuck out behind others)
            for (const auto& t : planTris)
            {
                if (juce::jmax (t[0].x, t[1].x, t[2].x) <= x || juce::jmin (t[0].x, t[1].x, t[2].x) >= xe
                    || juce::jmax (t[0].y, t[1].y, t[2].y) <= y || juce::jmin (t[0].y, t[1].y, t[2].y) >= ye) continue;
                P2 c[8];
                const int nc = clipToField (t, x, y, xe, ye, c);
                for (int k = 2; k < nc; ++k)
                {
                    if (std::abs (cross2 (c[0], c[k - 1], c[k])) < 1.0e-8f) continue;
                    for (int top = 0; top < 2; ++top)
                    {
                        const float z = top ? H : 0.0f;
                        out.push_back ({ vt ({ c[0].x, c[0].y, z }, nz, c[0].x, c[0].y), vt ({ c[k - 1].x, c[k - 1].y, z }, nz, c[k - 1].x, c[k - 1].y),
                                         vt ({ c[k].x, c[k].y, z }, nz, c[k].x, c[k].y), top ? 2 : 1, 1.0f });
                    }
                }
            }
        }
    // walls: the material from the absorption; a darker plinth of the same stuff
    float along = 0.0f;
    auto wall = [&] (float ax, float ay, float bx, float by, int index)
    {
        const int slot = kSlotWall0 + juce::jlimit (0, kMaxPlanVerts - 1, index);        // (every wall its own material)
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
            quad (vt (p0, n, u0, 0), vt (p1, n, u1, 0), vt (p1 + V3 { 0, 0, plinth }, n, u1, plinth), vt (p0 + V3 { 0, 0, plinth }, n, u0, plinth), slot, 0.55f);
            const int bands = juce::jmax (1, (int) std::ceil ((H - plinth) / 3.0f));
            for (int b = 0; b < bands; ++b)
            {
                const float z0 = plinth + (H - plinth) * (float) b / (float) bands, z1 = plinth + (H - plinth) * (float) (b + 1) / (float) bands;
                quad (vt (p0 + V3 { 0, 0, z0 }, n, u0, z0), vt (p1 + V3 { 0, 0, z0 }, n, u1, z0),
                      vt (p1 + V3 { 0, 0, z1 }, n, u1, z1), vt (p0 + V3 { 0, 0, z1 }, n, u0, z1), slot, 1.0f - 0.06f * (float) b / (float) bands);
            }
        }
        along += len;
    };
    if (box)
    {
        wall (0, 0, W, 0, 0); wall (W, 0, W, L, 1); wall (W, L, 0, L, 2); wall (0, L, 0, 0, 3);       // (back, right, front, left: as kSurfWall0 + i)
    }
    else
        for (int i = 0; i < r.plan.n; ++i)
        {
            const int j = (i + 1) % r.plan.n;
            wall (r.plan.x[(size_t) i] * W, r.plan.y[(size_t) i] * L, r.plan.x[(size_t) j] * W, r.plan.y[(size_t) j] * L, i);
        }
}

/** the Saal's surface the middle of the view points at (kSurfFloor, kSurfCeiling, kSurfWall0 + wall), -1 = none */
int WalkView::surfaceAhead (const RoomParams& r, V3 eye) const
{
    const float ya = juce::degreesToRadians (yaw), pa = juce::degreesToRadians (juce::jlimit (-89.0f, 89.0f, pitch));
    const V3 d { -std::sin (ya) * std::cos (pa), std::cos (ya) * std::cos (pa), std::sin (pa) };
    float best = 1.0e30f;
    int surf = -1;
    if (d.z < -1.0e-5f) { best = -eye.z / d.z; surf = kSurfFloor; }
    else if (d.z > 1.0e-5f) { best = (r.H() - eye.z) / d.z; surf = kSurfCeiling; }
    auto wall = [&] (float ax, float ay, float bx, float by, int index)
    {
        // (the wall's inward normal is to the left of a -> b; only a wall the view runs towards)
        const float ex = bx - ax, ey = by - ay, len = std::sqrt (ex * ex + ey * ey);
        if (len < 1.0e-3f) return;
        const float nx = -ey / len, ny = ex / len;
        const float dn = nx * d.x + ny * d.y;
        if (dn > -1.0e-6f) return;
        const float t = (nx * (eye.x - ax) + ny * (eye.y - ay)) / -dn;
        if (t <= 0.0f || t >= best) return;
        const float u = ((eye.x + d.x * t - ax) * ex + (eye.y + d.y * t - ay) * ey) / (len * len);
        if (u < 0.0f || u > 1.0f) return;
        best = t; surf = kSurfWall0 + index;
    };
    const float W = r.W(), L = r.L();
    if (r.box() || r.plan.n < 3) { wall (0, 0, W, 0, 0); wall (W, 0, W, L, 1); wall (W, L, 0, L, 2); wall (0, L, 0, 0, 3); }
    else
        for (int i = 0; i < r.plan.n; ++i)
        {
            const int j = (i + 1) % r.plan.n;
            wall (r.plan.x[(size_t) i] * W, r.plan.y[(size_t) i] * L, r.plan.x[(size_t) j] * W, r.plan.y[(size_t) j] * L, i);
        }
    return surf;
}

juce::String WalkView::surfaceName (const RoomParams& r, int surf) const
{
    juce::String n;
    if (surf == kSurfFloor) n = "Boden";
    else if (surf == kSurfCeiling) n = "Decke";
    else if (r.box() || r.plan.n < 3) n = juce::StringArray { U ("R\xc3\xbc" "ckwand"), "rechte Wand", "Vorderwand", "linke Wand" }[juce::jlimit (0, 3, surf - kSurfWall0)];
    else n = "Wand " + juce::String (surf - kSurfWall0 + 1);
    const int m = r.plan.surf[(size_t) juce::jlimit (0, kMaxSurf - 1, surf)];
    return n + ": " + (m < 0 ? juce::String ("wie Regler") : juce::String (surfMaterial (m).name));
}

/** M: the surface under the cross gets the next material (after the last: as the sliders say again) */
void WalkView::cycleMaterial()
{
    if (aimSurf < 0)
    {
        status = proc.getWalkMesh() != nullptr ? U ("Material: nur f\xc3\xbcr die Fl\xc3\xa4" "chen des Saals (ohne Modell)") : U ("Keine Fl\xc3\xa4" "che im Blick");
        statusUntil = juce::Time::getMillisecondCounterHiRes() + 2500;
        return;
    }
    const int cur = proc.getSurfaceMaterial (0, aimSurf);
    proc.setSurfaceMaterial (0, aimSurf, cur + 1 >= kNumSurfMaterials ? -1 : cur + 1);
    status = surfaceName (roomNow(), aimSurf);
    statusUntil = juce::Time::getMillisecondCounterHiRes() + 2500;
    repaint();
}

void WalkView::addPipe (V3 bottom, float len, float rad, float faceAngle, float tone, float glowAmt, int stop)
{
    const float depth = rast.viewDepth (bottom + V3 { 0, 0, 0.5f * len });
    if (depth < -len) return;
    const float px = rad / juce::jmax (0.1f, depth) * 0.5f * (float) rast.width();
    if (stop == 1) { addWoodPipe (bottom, len, rad, faceAngle, glowAmt); return; }
    if (stop == 6) { addReedPipe (bottom, len, rad, faceAngle, tone, glowAmt, px); return; }
    // a real pipe: a conical foot (to 16 % of its length), the round body, an open dark top; the mouth (in the
    // texture) turned towards the listener's place. Fewer sides when it is small on screen.
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

void WalkView::addWoodPipe (V3 bottom, float len, float rad, float faceAngle, float glowAmt)
{
    // the Gedackt: a wooden pipe, square - a narrower block as its foot, the body of boards (deeper than it is
    // wide), the mouth a dark slot in its front turned to the listener's place, and on top the stopper with its
    // handle (it is stopped: no open top)
    const float em = 0.75f * glowAmt;
    const float ca = std::cos (faceAngle), sa = std::sin (faceAngle);
    const V3 f { ca, sa, 0.0f }, s { -sa, ca, 0.0f };          // front (towards the listener), side
    auto block = [&] (float halfS, float halfF, float z0, float z1, float tone, bool top)
    {
        const V3 c = bottom;
        // four sides: front, back, left, right - each a flat face, boards running up it
        struct Face { V3 n, a; float halfU; V3 u; };
        const Face faces[4] { { f, f * halfF, halfS, s }, { f * -1.0f, f * -halfF, halfS, s * -1.0f },
                              { s, s * halfS, halfF, f * -1.0f }, { s * -1.0f, s * -halfS, halfF, f } };
        for (const auto& fc : faces)
        {
            Raster::Vert q[4];
            const float us[4] { -1, 1, 1, -1 }, zs[4] { z0, z0, z1, z1 };
            for (int i = 0; i < 4; ++i)
            {
                q[i].p = c + fc.a + fc.u * (us[i] * fc.halfU) + V3 { 0, 0, zs[i] };
                q[i].n = fc.n; q[i].u = (us[i] + 1.0f) * fc.halfU; q[i].v = zs[i];
            }
            rast.triangle (q[0], q[1], q[2], kSlotWood, tone, em);
            rast.triangle (q[0], q[2], q[3], kSlotWood, tone, em);
        }
        if (top)
        {
            Raster::Vert q[4];
            const float us[4] { -1, 1, 1, -1 }, vs[4] { -1, -1, 1, 1 };
            for (int i = 0; i < 4; ++i)
            {
                q[i].p = c + s * (us[i] * halfS) + f * (vs[i] * halfF) + V3 { 0, 0, z1 };
                q[i].n = { 0, 0, 1 }; q[i].u = (us[i] + 1.0f) * halfS; q[i].v = (vs[i] + 1.0f) * halfF;
            }
            rast.triangle (q[0], q[1], q[2], kSlotWood, tone, em);
            rast.triangle (q[0], q[2], q[3], kSlotWood, tone, em);
        }
    };
    const float hs = 0.85f * rad, hf = 1.1f * rad, zFoot = 0.12f * len;
    block (0.5f * hs, 0.5f * hf, 0.0f, zFoot, 0.8f, false);                       // the foot
    block (hs, hf, zFoot, len, 1.0f, false);                                      // the body
    const float cap = juce::jmax (0.02f, 0.5f * hs);
    block (1.12f * hs, 1.12f * hf, len, len + cap, 0.75f, true);                  // the stopper
    block (0.3f * hs, 0.18f * hf, len + cap, len + cap + 1.3f * cap, 0.7f, true); // its handle
    // the mouth: a dark slot just above the foot, in the front
    {
        const float w = 0.75f * hs, z0 = zFoot + 0.25f * hs, z1 = z0 + 0.55f * hs;
        const V3 o = bottom + f * (hf + 0.003f);
        Raster::Vert q[4];
        const float us[4] { -1, 1, 1, -1 }, zs[4] { z0, z0, z1, z1 };
        for (int i = 0; i < 4; ++i) { q[i].p = o + s * (us[i] * w) + V3 { 0, 0, zs[i] }; q[i].n = f; }
        rast.triangle (q[0], q[1], q[2], kSlotDark, 1.0f, 0.3f * em);
        rast.triangle (q[0], q[2], q[3], kSlotDark, 1.0f, 0.3f * em);
    }
}

void WalkView::addReedPipe (V3 bottom, float len, float rad, float faceAngle, float tone, float glowAmt, float px)
{
    // the Trompete: a reed pipe - a round boot below (the reed sits in it), on it a long funnel that widens
    // all the way up to its open top; no mouth
    const float em = 0.75f * glowAmt;
    const int segs = px < 1.5f ? 5 : (px < 5.0f ? 8 : (px < 14.0f ? 12 : 18));
    const float rTip = 0.2f * rad, rBoot = 0.75f * rad, zTip = 0.04f * len, zBoot = 0.2f * len;
    const float rNeck = 0.16f * rad, rTop = 1.25f * rad;
    // the rings from the bottom up: radius, height, texture v (the boot in the foot's shade, the funnel above
    // the band where a flue pipe has its mouth)
    struct Ring { float r, z, v; };
    const Ring rings[6] { { rTip, 0.0f, 0.0f }, { rBoot, zTip, 0.04f }, { rBoot, zBoot, 0.15f }, { rNeck, zBoot, 0.15f },
                          { rNeck, zBoot, 0.24f }, { rTop, len, 1.0f } };
    Raster::Vert ring[6][20];
    for (int i = 0; i <= segs; ++i)
    {
        const float a = faceAngle + juce::MathConstants<float>::twoPi * (float) i / (float) segs;
        const float ca = std::cos (a), sa = std::sin (a);
        for (int k = 0; k < 6; ++k)
        {
            auto& q = ring[k][i];
            q.p = bottom + V3 { ca * rings[k].r, sa * rings[k].r, rings[k].z };
            q.u = (float) i / (float) segs; q.v = rings[k].v;
        }
        // (each band's own slope: the cone of the tip, the boot, its lid, the funnel)
        ring[0][i].n = normalised ({ ca, sa, -(rBoot - rTip) / zTip });
        ring[1][i].n = ring[2][i].n = V3 { ca, sa, 0.0f };
        ring[3][i].n = { 0, 0, 1 };
        ring[4][i].n = ring[5][i].n = normalised ({ ca, sa, -(rTop - rNeck) / (len - zBoot) });
    }
    auto band = [&] (int k0, int k1, int i)
    {
        Raster::Vert a = ring[k0][i], b = ring[k0][i + 1], c = ring[k1][i + 1], d = ring[k1][i];
        if (k0 == 0) { d.n = ring[0][i].n; c.n = ring[0][i + 1].n; }
        if (k0 == 2) { a.n = b.n = c.n = d.n = V3 { 0, 0, 1 }; }
        rast.triangle (a, b, c, kSlotMetal, tone, em);
        rast.triangle (a, c, d, kSlotMetal, tone, em);
    };
    for (int i = 0; i < segs; ++i)
    {
        band (0, 1, i);      // the tip of the boot
        band (1, 2, i);      // the boot
        band (2, 3, i);      // its lid
        band (4, 5, i);      // the funnel
    }
    if (rast.eyePos().z > bottom.z + len)
    {
        Raster::Vert c; c.p = bottom + V3 { 0, 0, len - 0.5f * rTop }; c.n = { 0, 0, 1 };
        for (int i = 0; i < segs; ++i)
        {
            Raster::Vert a = ring[5][i], b = ring[5][i + 1];
            a.n = b.n = c.n;
            rast.triangle (a, b, c, kSlotDark, 1.0f, 0.3f * em);
        }
    }
}

V3 WalkView::pipeAt (const RoomParams& r, V3 homeP, Pos pos, int stop, int key) const
{
    // a pipe flying in the room (Flug, Fall, Schwarm, Strömung) is where its body is
    Vec3f b;
    if (proc.engine.bodyAt (stop, key, b)) return { b.x, b.y, b.z };
    // walking in a shaped Saal the engine hears every pipe where the room simulation has it, inside the walls
    // (so what you see hidden behind a wall is what you hear around the corner); otherwise where Ambisonics puts it
    if (pipesInRoom)
    {
        const auto q = roomPoint (r, pos);
        return { q.x, q.y, q.z };
    }
    const float a = juce::degreesToRadians (pos.az), e = juce::degreesToRadians (pos.el);
    return { homeP.x - std::sin (a) * std::cos (e) * pos.dist, homeP.y + std::cos (a) * std::cos (e) * pos.dist, homeP.z + std::sin (e) * pos.dist };
}

void WalkView::paint (juce::Graphics& g)
{
    if (! gpuOn()) buildScene (juce::jlimit (1.0f, 2.5f, g.getInternalContext().getPhysicalPixelScaleFactor()));
    drawOver (g);
}

/** the frame's picture: the room and the pipes as triangles, drawn into the renderer's image */
void WalkView::buildScene (float phys)
{
    scene.params = proc.readParams();
    scene.r = roomNow();
    const auto& params = scene.params;
    const auto& r = scene.r;
    // (walking in a shaped Saal, or with a model that is heard: the pipes are where the room simulation has them)
    pipesInRoom = params.room.on && ((! r.box() && r.plan.n >= 3 && (proc.engine.listenerXNow.load() != 0.0f || proc.engine.listenerYNow.load() != 0.0f))
                                     || proc.engine.meshActive.load());
    const V3 homeP { listenerPos (r).x, listenerPos (r).y, listenerPos (r).z };
    // the eyes: where you stand (or jump), swaying a little with the steps, giving in the knees after a jump
    const float sway = 0.022f * bobAmt * std::sin (bobPhase) - landDip;
    const V3 eye { homeP.x + px, homeP.y + py, juce::jmax (0.25f, homeP.z + fz + sway) };
    const auto area = getLocalBounds();

    // as sharp as the screen allows (in full screen the window is scaled up), less when a frame takes too long
    // (the graphics card draws at the screen's full sharpness whatever this is: the processor then only keeps
    //  the depths, for the cross and the glows, at half the view's size)
    const bool card = gpuOn();
    // (test, RYSUM_GPU_TEST: every 60 frames one frame is drawn both ways and compared - first the scene as it
    //  is, then every surface of the Saal as each kind of material in turn, at last with the test model loaded)
    static const bool gpuTest = juce::SystemStats::getEnvironmentVariable ("RYSUM_GPU_TEST", {}).isNotEmpty();
    int testStep = -1;
    if (card && gpuTest)
    {
        ++gpuFrames;
        if (gpuFrames >= 90 && gpuFrames <= 90 + 60 * 11)
        {
            testStep = (gpuFrames - 60) / 60;          // 0 = as it is, 1 .. 9 = the kinds, 10 = the model, 11 = the model with the light off
            if (gpuFrames == 60 + 60 * 10)
            {
                auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rysum_gpu_test.obj");
                f.replaceWithText (makeTestObj());
                loadModel (f);
            }
        }
    }
    const bool compareNow = testStep >= 0 && (gpuFrames - 90) % 60 == 0;
    const float sc = compareNow ? phys : (card ? 0.5f : juce::jlimit (0.4f, 2.5f, phys * resScale));
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    rast.setSize (juce::roundToInt ((float) area.getWidth() * sc), juce::roundToInt ((float) area.getHeight() * sc));
    rast.setCamera (eye, yaw, pitch, fov);
    viewScale = (float) area.getWidth() / (float) juce::jmax (1, rast.width());
    const float fogDist = juce::jmax (12.0f, 1.5f * juce::jmax (r.W(), r.L()));      // (floor and ceiling stay readable to the far end)
    // (light off: what is far away sinks into the dark, not into a grey haze)
    const bool dark = (proc.walkDark && ! compareNow) || testStep == 11;      // (the test: one lamp, below)
    // (a comparison: without the film grain; in the dark less of it, so nothing glitters where no lamp shines)
    const float grainAmt = compareNow ? 0.0f : (dark ? 0.05f : 0.12f);
    rast.begin (dark ? 0.0f : 0.07f, fogDist, grainAmt, ++frame);

    // the surfaces from the Saal's absorption, scatter and high damping
    const auto wm = wallMaterial (r.absWalls, r.highDamp, r.scatter);
    rast.setMaterial (kSlotWall, wm);
    {
        // every surface as what it is made of: its own material (Saal page, or M here), else what the sliders say
        const auto sa = SurfAbs::from (r);
        auto look = [&] (int surf)
        {
            const int own = r.plan.surf[(size_t) surf];
            const bool level = surf < kSurfWall0;
            if (own < 0) return level ? floorMaterial (r.absFloor, r.highDamp, r.scatter, surf == kSurfCeiling) : wm;
            const auto& sm = surfMaterial (own);
            Material m = wallMaterial (sm.aw, sm.hi, r.scatter);
            m.tone = juce::jlimit (0.12f, 1.0f, m.tone * (1.0f - 0.5f * (sa.low[(size_t) surf] - sm.aw)));
            if (level)
            {
                // lying down: ashlar becomes slabs, panelling boards, a curtain a carpet
                if (m.kind == Material::stone && surf == kSurfFloor) m.kind = Material::slabs;
                else if (m.kind == Material::wood) m.kind = Material::boards;
                else if (m.kind == Material::curtain) m.kind = Material::carpet;
                if (surf == kSurfCeiling) m.tone *= 0.78f;
            }
            return m;
        };
        rast.setMaterial (kSlotFloor, look (kSurfFloor));
        rast.setMaterial (kSlotCeiling, look (kSurfCeiling));
        for (int i = 0; i < kMaxPlanVerts; ++i) rast.setMaterial (kSlotWall0 + i, look (kSurfWall0 + i));
    }
    rast.setMaterial (kSlotModel, wm);
    if (testStep >= 1 && testStep <= 9)
    {
        Material m;
        m.kind = (Material::Kind) testStep; m.tone = 0.8f; m.rough = 0.3f; m.gloss = 0.3f;
        for (int slot : { (int) kSlotWall, (int) kSlotFloor, (int) kSlotCeiling, (int) kSlotModel }) rast.setMaterial (slot, m);
        for (int i = 0; i < kMaxPlanVerts; ++i) rast.setMaterial (kSlotWall0 + i, m);
    }
    { Material m; m.kind = Material::metal; m.tone = 1.0f; m.gloss = 1.0f; m.rough = 0.1f; rast.setMaterial (kSlotMetal, m); }
    { Material m; m.kind = Material::dark; m.tone = 1.0f; m.gloss = 0.0f; rast.setMaterial (kSlotDark, m); }
    { Material m; m.kind = Material::wood; m.tone = 0.62f; m.gloss = 0.12f; m.rough = 0.35f; rast.setMaterial (kSlotWood, m); }      // (the Gedackt)
    wallLook = wm.kind;

    scene.mesh = proc.getWalkMesh();
    scene.homeP = homeP;
    scene.eye = eye;
    const auto& mesh = scene.mesh;
    if (mesh != nullptr)
    {
        // a model in greys (its colours' brightness): level faces low down get the floor's material, level faces
        // high up the ceiling's, everything else the walls' (worked out once per model)
        if (slotMesh != mesh.get() || meshSlots.size() != mesh->tris.size())
        {
            slotMesh = mesh.get();
            meshSlots.resize (mesh->tris.size());
            for (size_t i = 0; i < mesh->tris.size(); ++i)
            {
                const auto& t = mesh->tris[i];
                const V3 n = normalised (cross (t.b - t.a, t.c - t.a));
                const float zc = (t.a.z + t.b.z + t.c.z) / 3.0f;
                meshSlots[i] = (uint8_t) (std::abs (n.z) < 0.8f ? kSlotModel : (zc < 0.5f * mesh->size.z ? kSlotFloor : kSlotCeiling));
            }
        }
        for (size_t i = 0; i < mesh->tris.size(); ++i)
        {
            const auto& t = mesh->tris[i];
            const auto c = t.colour;
            const float lum = 0.3f + 0.9f * (0.3f * c.getFloatRed() + 0.59f * c.getFloatGreen() + 0.11f * c.getFloatBlue());
            rast.flatTriangle (t.a, t.b, t.c, meshSlots[i], lum);
        }
        // the Saal's shoebox underneath: its floor and its ceiling, far beyond the model - a model without a floor
        // or a roof, or open to one side, never shows a black void (a hair below / above the model's own, so the
        // model's surfaces lie in front)
        {
            const float mx = juce::jmax (r.W(), mesh->size.x), my = juce::jmax (r.L(), mesh->size.y);
            const float ext = juce::jmax (48.0f, 2.0f * juce::jmax (mx, my)), tile = 8.0f;
            const float zTop = juce::jmax (r.H(), mesh->size.z) + 0.04f;
            auto vt = [] (float x, float y, float z) { Raster::Vert q; q.p = { x, y, z }; q.n = { 0, 0, 1 }; q.u = x; q.v = y; return q; };
            for (float x = -ext; x < mx + ext; x += tile)
                for (float y = -ext; y < my + ext; y += tile)
                    for (int top = 0; top < 2; ++top)
                    {
                        const float z = top ? zTop : -0.04f;
                        const auto a = vt (x, y, z), b = vt (x + tile, y, z), cc = vt (x + tile, y + tile, z), d = vt (x, y + tile, z);
                        rast.triangle (a, b, cc, top ? kSlotCeiling : kSlotFloor, 1.0f);
                        rast.triangle (a, cc, d, top ? kSlotCeiling : kSlotFloor, 1.0f);
                    }
        }
        aimSurf = -1;
    }
    else
    {
        buildRoomTris (r, roomTris);
        // (the surface the cross points at - the one M gives another material - a little lighter)
        aimSurf = surfaceAhead (r, eye);
        const int aimSlot = aimSurf < 0 ? -1 : (aimSurf == kSurfFloor ? (int) kSlotFloor : (aimSurf == kSurfCeiling ? (int) kSlotCeiling : kSlotWall0 + aimSurf - kSurfWall0));
        for (const auto& t : roomTris) rast.triangle (t.a, t.b, t.c, t.slot, t.slot == aimSlot ? t.tone * 1.1f : t.tone);
    }

    // the pipes, where Ambisonics puts them (as heard from the listener's place; you walk among them)
    const double now = proc.engine.timeSeconds.load();
    auto& glows = scene.glows;
    glows.clear();
    for (int s = 0; s < kNumStops; ++s)
    {
        for (auto& sh : shown[(size_t) s]) sh.on = false;
        if (! params.stopOn[(size_t) s]) { for (auto& v : glow[(size_t) s]) v = 0.0f; continue; }
        const uint64_t sounding = proc.engine.soundingMask[(size_t) s].load();
        const float metalTone = kMetal[s].getPerceivedBrightness() * 1.05f;
        for (int k = 0; k < kNumKeys; ++k)
        {
            if (proc.engine.muted[(size_t) s][(size_t) k].load()) continue;
            auto& gl = glow[(size_t) s][(size_t) k];
            gl = ((sounding >> k) & 1) ? juce::jmin (1.0f, gl + 0.35f) : gl * 0.9f;
            if (s == heldStop && k == heldKey) gl = juce::jmax (gl, 0.3f);        // (in your hand)
            const Pos pos = proc.engine.displayPosition (s, k, params);
            const V3 c = pipeAt (r, homeP, pos, s, k);
            float len, rad;
            pipeSizeOf (s, k, len, rad);
            len *= juce::jlimit (0.2f, 4.0f, proc.engine.pipeLen[(size_t) s][(size_t) k].load());      // (Wachsen & Brechen)
            // floating: a slow breath up and down, each pipe its own (not the ones flying as bodies: they move as they move)
            Vec3f bodyNow;
            const float bob = proc.engine.bodyAt (s, k, bodyNow) ? 0.0f : 0.06f * (float) std::sin (now * 0.7 + (double) (s * 41 + k) * 1.37);
            const V3 bottom { c.x, c.y, c.z - 0.5f * len + bob };
            const float face = std::atan2 (homeP.y - c.y, homeP.x - c.x);       // the mouth towards the listener's place
            addPipe (bottom, len, rad, face, metalTone, dark ? gl + 0.025f : gl, s);    // (light off: the barest shimmer of their own)
            shown[(size_t) s][(size_t) k] = { { c.x, c.y, c.z + bob }, len, rad, true };
            if (gl > 0.02f) glows.push_back ({ { c.x, c.y, c.z + bob }, gl, len });
        }
    }
    // impacts: a short white flash where a pipe hit (new ones from the engine's ring of the last 16)
    {
        const uint32_t n = proc.engine.impactCount.load();
        if (n - flashSeen > 16u) flashSeen = n - 16u;
        for (; flashSeen != n; ++flashSeen)
        {
            const auto& sh = proc.engine.impactShow[(size_t) (flashSeen & 15u)];
            auto& f = flashes[(size_t) (flashSeen & 15u)];
            f.at = { sh.x.load(), sh.y.load(), sh.z.load() };
            f.strength = 0.4f + 0.6f * sh.strength.load();
            f.life = 1.0f;
        }
        for (auto& f : flashes)
            if (f.life > 0.0f)
            {
                glows.push_back ({ f.at, f.strength * f.life, 0.4f + 1.6f * (1.0f - f.life) });   // grows as it fades
                f.life -= 0.12f;
            }
    }
    {
        // the lanterns (K): a small flickering light each
        const double tl = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        for (size_t i = 0; i < proc.walkLanterns.size(); ++i)
        {
            const auto& l = proc.walkLanterns[i];
            const float flicker = 0.85f + 0.08f * (float) std::sin (tl * 7.3 + (double) i * 2.1) + 0.05f * (float) std::sin (tl * 13.1 + (double) i);
            glows.push_back ({ { l.x, l.y, l.z }, flicker, 0.6f });
        }
    }
    // light off: the sounding pipes (and the flashes of knocks) are the lamps of the room
    {
        lamps.clear();
        if (dark)
        {
            // (each a small light; a full chord of many stops is brighter than one pipe, but not without end)
            float sum = 0.0f;
            for (const auto& gw : glows)
            {
                lamps.push_back ({ gw.at, 0.8f * juce::jmin (1.0f, gw.amount), 1.8f + 0.45f * juce::jmin (3.0f, gw.size) });
                sum += lamps.back().power;
            }
            if (sum > 3.0f)
                for (auto& l : lamps) l.power *= 3.0f / sum;
        }
        if (testStep == 11)
        {
            // (the test of the light off: one lamp three metres ahead, a little above the eyes)
            const float ya = juce::degreesToRadians (yaw);
            lamps.push_back ({ eye + V3 { -std::sin (ya) * 3.0f, std::cos (ya) * 3.0f, 0.3f }, 1.5f, 3.0f });
        }
        rast.setLamps (lamps, dark ? kDarkRest : 1.0f);
    }
    geoMs = (float) (juce::Time::getMillisecondCounterHiRes() - t0);
    if (card)
    {
        gpuFrame.dark = rast.darkNow();
        gpuFrame.ambient = rast.ambientNow();
        gpuFrame.focalPerWidth = rast.focalPerWidth();
        gpuFrame.lamps = rast.lampsInView();
        rast.exportGpu (gpuFrame.verts, gpuFrame.mats);
        gpuFrame.fogTone = rast.fogToneNow(); gpuFrame.fogInv = rast.fogInvNow();
        gpuFrame.grain = grainAmt; gpuFrame.frameNo = frame;
        if (compareNow)
        {
            // (test) the same frame from the processor, to be compared with what the card draws
            rast.finish();
            gpuFrame.compareWith = rast.image().createCopy();
            static const char* const stepNames[] { "0_scene", "1_stone", "2_plaster", "3_wood", "4_glass", "5_curtain", "6_mirror", "7_slabs", "8_boards", "9_carpet", "10_model", "11_dark" };
            gpuFrame.compareDir = RysumProcessor::getPatchFolder().getParentDirectory().getChildFile ("gpu-test").getChildFile (stepNames[juce::jlimit (0, 11, testStep)]);
        }
        else
            rast.finishDepth();
        // the glow of the sounding pipes (where no wall hides them) and, with a model, the outline of the room
        // the sound is computed in: drawn by the card as well
        gpuFrame.glows.clear();
        gpuFrame.lines.clear();
        const float rw = (float) rast.width(), rh = (float) rast.height();
        if (! compareNow)
            for (const auto& gw : glows)
            {
                float sx, sy, d;
                if (! rast.project (gw.at, sx, sy, d)) continue;
                const float zd = rast.depthAt ((int) sx, (int) sy);
                if (zd > 0.0f && zd < d * 0.95f - 0.4f) continue;
                const float rad = juce::jlimit (6.0f, 260.0f, (0.6f + 0.35f * gw.size) / d * 0.5f * (float) area.getWidth());
                gpuFrame.addGlow (sx / rw * 2.0f - 1.0f, 1.0f - sy / rh * 2.0f, 2.0f * rad / (float) area.getWidth(), 2.0f * rad / (float) area.getHeight(),
                                  juce::jlimit (0.0f, 1.0f, gw.amount));
            }
        gpu->submit (gpuFrame);
    }
    else
        rast.finish();
    // the pipe under the cross (Q takes it)
    if (! pickPipe (area.getCentre().toFloat(), 9.0f, aimStop, aimKey)) aimStop = aimKey = -1;
    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
    lastFrameMs = (float) ms;
    if (! testMode && ! card)
    {
        // the resolution in fixed steps, decided by the part that depends on it (drawing the pixels, not the
        // geometry): down after a few slow frames, up when the next step would still be quick, a while later
        static constexpr float kSteps[] { 1.0f, 0.85f, 0.7f, 0.55f, 0.42f };
        const float pixMs = rast.rasterMs;
        if (pixMs > 8.0f) { ++slowFrames; quickFrames = 0; }
        else
        {
            slowFrames = 0;
            const float up = resLevel > 0 ? kSteps[resLevel - 1] / kSteps[resLevel] : 1.0f;
            if (resLevel > 0 && pixMs * up * up < 6.5f) ++quickFrames; else quickFrames = 0;
        }
        if (slowFrames >= 10 && resLevel < 4) { ++resLevel; slowFrames = 0; }
        if (quickFrames >= 90 && resLevel > 0) { --resLevel; quickFrames = 0; }
        resScale = kSteps[resLevel];
    }
}

/** what lies over the picture: the glow of the sounding pipes, the cross, the writing, the map */
void WalkView::drawOver (juce::Graphics& g)
{
    auto& A = Assets::get();
    const auto& params = scene.params;
    const auto& r = scene.r;
    const auto& mesh = scene.mesh;
    const auto& glows = scene.glows;
    const V3 homeP = scene.homeP, eye = scene.eye;
    const auto area = getLocalBounds();
    juce::ignoreUnused (A, params, homeP, eye);

    if (! gpuOn())          // (the card's picture lies under this component already)
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
        g.drawImage (rast.image(), area.toFloat());
    }
    const float isc = (float) area.getWidth() / (float) rast.width();

    // the sounding pipes shine (where they are not hidden behind a wall)
    for (const auto& gw : glows)
    {
        if (gpuOn()) break;          // (the card draws them)
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
            juce::ColourGradient grad (juce::Colour (0xfff6f4f0).withAlpha (0.5f), 64.0f, 64.0f, col::ink.withAlpha (0.0f), 128.0f, 64.0f, true);      // (light, no colour: the room stays grey)
            grad.addColour (0.3, juce::Colour (0xffdcdad4).withAlpha (0.2f));
            grad.addColour (0.65, col::dim.withAlpha (0.06f));
            gg.setGradientFill (grad);
            gg.fillEllipse (0.0f, 0.0f, 128.0f, 128.0f);
        }
        juce::Graphics::ScopedSaveState ss (g);
        g.setOpacity (juce::jlimit (0.0f, 1.0f, gw.amount));
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImage (glowSprite, juce::Rectangle<float> (2.0f * rad, 2.0f * rad).withCentre (c));
    }

    // a small cross in the middle
    {
        const auto c = area.getCentre().toFloat();
        const bool onPipe = aimStop >= 0 || heldStop >= 0;
        g.setColour (col::ink.withAlpha (onPipe ? 0.9f : 0.45f));
        g.drawLine (c.x - 6, c.y, c.x + 6, c.y, 1.0f);
        g.drawLine (c.x, c.y - 6, c.x, c.y + 6, 1.0f);
        if (onPipe) g.drawEllipse (juce::Rectangle<float> (18.0f, 18.0f).withCentre (c), 1.0f);     // (a pipe to take, or taken)
        // the shockwave: a ring running outwards
        const float age = (float) ((juce::Time::getMillisecondCounterHiRes() - shockFx) / 450.0);
        if (shockFx > 0.0 && age >= 0.0f && age < 1.0f)
        {
            const float rad = (0.04f + 0.6f * std::sqrt (age)) * (float) area.getWidth();
            g.setColour (col::ink.withAlpha (0.5f * (1.0f - age) * (1.0f - age)));
            g.drawEllipse (juce::Rectangle<float> (2.0f * rad, 2.0f * rad).withCentre (c), 1.0f + 5.0f * (1.0f - age));
        }
    }

    if (gpuOn() && overText.isNotEmpty())
    {
        // (the window's readout: it lies under the card's picture, so it is written here)
        auto corner = area;
        const auto box = corner.removeFromRight (560).withTrimmedTop (150).removeFromTop (136).reduced (8);
        g.setColour (col::bg.withAlpha (0.88f));
        g.fillRect (box);
        g.setColour (col::ink);
        g.setFont (A.monoFont (10.0f));
        g.drawFittedText (overText, box.reduced (6, 3), juce::Justification::topLeft, 9);
    }
    drawAudience (g, area);     // (also in the immersive view: it is what the room is shown)
    drawArranging (g, area);
    drawMoreHands (g, area);
    if (immersive) { if (helpHeld) drawHelp (g, area); return; }      // (only the picture and the cross)

    // heading, hints, status (on dark bands, so they stay readable over a bright model)
    {
        const auto top = area.toFloat().removeFromTop (124.0f), bottom = area.toFloat().removeFromBottom (84.0f);
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
                    + (proc.engine.hiddenNow.load() > 0 ? U ("   \xc2\xb7   ") + juce::String (proc.engine.hiddenNow.load()) + U (" Pfeifen um die Ecke geh\xc3\xb6rt") : juce::String())
                    + (aimSurf >= 0 ? U ("   \xc2\xb7   ") + surfaceName (r, aimSurf) + U ("  [M: Material]") : juce::String());
    g.drawText (info + (proc.timeBackNow.load() > 0.05f ? U ("   \xc2\xb7   du h\xc3\xb6rst, was vor ") + juce::String (proc.timeBackNow.load(), 1) + U (" s erklang") : juce::String())
                     + (proc.engine.breakCount.load() > 0 ? U ("   \xc2\xb7   ") + juce::String ((int) proc.engine.breakCount.load()) + U (" Pfeifen gebrochen") : juce::String()),
                area.reduced (18, 0).withTrimmedTop (84).removeFromTop (16), juce::Justification::centredLeft);
    g.setColour (col::faint);
    g.drawText (U ("W A S D gehen  \xc2\xb7  Leertaste springen  \xc2\xb7  Maus ziehen: schauen  \xc2\xb7  \xe2\x86\x90 \xe2\x86\x92 drehen  \xc2\xb7  Shift schneller  \xc2\xb7  M: Material der Fl\xc3\xa4" "che  \xc2\xb7  Rad: Blickwinkel  \xc2\xb7  Tab: alle Tasten  \xc2\xb7  Esc zur\xc3\xbc" "ck"
                  "   \xe2\x80\x94   gespielt wird hier \xc3\xbc" "ber MIDI oder \xe2\x80\x9eHalten\xe2\x80\x9c"),
                area.reduced (18, 0).removeFromBottom (24), juce::Justification::centredLeft);
    g.drawText (U ("Tab: alle Tasten  \xc2\xb7  G: Bild ") + (gpuOn() ? juce::String ("Grafikkarte") : (gpuWanted() && gpuNote.isNotEmpty() ? "Prozessor (" + gpuNote + ")" : juce::String ("Prozessor"))), handsArea.withTrimmedLeft (8), juce::Justification::centredLeft);
    {
        // the player moves pipes, but no stop has pipes that can be moved: say how
        bool anyBodies = false;
        for (int s = 0; s < kNumStops; ++s) anyBodies = anyBodies || isRoomMotion (params.motionMode[(size_t) s]);
        if (params.live.playerMode > 0 && ! anyBodies && juce::Time::getMillisecondCounterHiRes() >= statusUntil)
        {
            g.setColour (col::rubric.brighter (0.2f));
            g.drawText (U ("Die Pfeifen reagieren auf dich, wenn die Bewegung eines Registers (Raum-Seite) im Saal stattfindet: Schweben, Flug, Fall, Schwarm oder Str\xc3\xb6mung."),
                        area.reduced (18, 0).withTrimmedTop (102).removeFromTop (16), juce::Justification::centredLeft);
        }
    }
    if (! proc.isBinaural())
    {
        g.setColour (col::rubric.brighter (0.2f));
        g.drawText (U ("Die Kopfdrehung ist nur bei Kopfh\xc3\xb6rer-Ausgabe (binaural) zu h\xc3\xb6ren; Gehen \xc3\xbc" "berall."),
                    area.reduced (18, 0).withTrimmedBottom (52).removeFromBottom (16), juce::Justification::centredLeft);
    }
    if (juce::Time::getMillisecondCounterHiRes() < statusUntil && status.isNotEmpty())
    {
        g.setColour (col::ink);
        g.drawText (status, area.reduced (18, 0).withTrimmedTop (102).removeFromTop (16), juce::Justification::centredLeft);
    }

    drawMap (g, juce::Rectangle<float> (190.0f, 150.0f).withPosition ((float) area.getRight() - 206.0f, (float) area.getBottom() - 186.0f), r, params, homeP);
    if (helpHeld) drawHelp (g, area);
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
            const V3 pw = pipeAt (r, homeP, pos, s, k);
            const auto q = P (pw.x, pw.y);
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

void WalkView::setAudience (bool on)
{
    if (! on)
    {
        proc.setAudience (false);
        actAudience.setToggleState (false, juce::dontSendNotification);
        say (U ("Publikum aus: die Seite f\xc3\xbcr die Handys ist geschlossen"));
        return;
    }
    // (the phones knock and blow pipes that are bodies in the Saal: the pulled stops that stand still start
    //  to hover now, once, at the player's click - never because a phone asked)
    const auto params = proc.readParams();
    int woken = 0;
    for (int s = 0; s < kNumStops; ++s)
        if (params.stopOn[(size_t) s] && ! isRoomMotion (params.motionMode[(size_t) s]) && makeHover (s, false)) ++woken;
    const bool ok = proc.setAudience (true, audienceLoopback);
    actAudience.setToggleState (ok, juce::dontSendNotification);
    if (! ok) { say (U ("Publikum: kein freier Port (8787 bis 8796) \xe2\x80\x93 l\xc3\xa4uft Rysum schon mehrmals?"), 5000.0); return; }
    say (U ("Publikum an: ") + proc.getAudience()->url() + (woken > 0 ? U ("  \xc2\xb7  ") + juce::String (woken) + U (" Register schweben jetzt") : juce::String())
         + U ("  \xc2\xb7  Windows fragt beim ersten Mal, ob Rysum ins Netz darf"), 6000.0);
}

void WalkView::drawAudience (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto* au = proc.getAudience();
    if (au == nullptr) return;
    auto& A = Assets::get();
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float isc = rast.width() > 0 ? (float) area.getWidth() / (float) rast.width() : 1.0f;

    // every phone's pipe: a ring in its colour (bright for a moment after it knocked)
    for (const auto& h : au->hands())
    {
        if (h.stop < 0) continue;
        const auto& sh = shown[(size_t) h.stop][(size_t) h.key];
        float sx, sy, d;
        if (! sh.on || ! rast.project (sh.c, sx, sy, d)) continue;
        const float hit = (float) juce::jlimit (0.0, 1.0, 1.0 - (now - h.lastAct) / 500.0);
        if (hit > 0.0f) glow[(size_t) h.stop][(size_t) h.key] = juce::jmax (glow[(size_t) h.stop][(size_t) h.key], hit);
        const juce::Point<float> c (sx * isc, sy * isc);
        const float rad = juce::jlimit (10.0f, 70.0f, 0.5f / juce::jmax (0.3f, d) * 0.5f * (float) area.getWidth() * 0.3f) * (1.0f + 0.8f * hit);
        const auto colr = AudienceHands::colour (h.colour);
        g.setColour (colr.withAlpha (0.55f + 0.45f * hit));
        g.drawEllipse (juce::Rectangle<float> (2.0f * rad, 2.0f * rad).withCentre (c), 2.0f + 3.0f * hit);
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (c));
    }
    // a phone's shockwave: the ring around the cross, as for E
    if (au->lastShockMs() > audienceShockSeen) { audienceShockSeen = au->lastShockMs(); shockFx = audienceShockSeen; }

    // the way in: the QR code (dark on white, with its quiet zone) and the address; bigger in the immersive view
    const auto& q = au->qr();
    if (q.size <= 0) return;
    // (top right, under the row of sliders - the map has the bottom right; immersive: bottom right, bigger)
    const int mod = juce::jlimit (3, 10, (immersive ? area.getHeight() / 3 : area.getHeight() / 4) / (q.size + 8));
    const int side = (q.size + 8) * mod;
    auto box = juce::Rectangle<int> (area.getRight() - side - 18, immersive ? area.getBottom() - side - 52 : area.getY() + 128, side, side);
    const int phones = au->phoneCount();
    if (phones > 0) audienceAlone = 0.0; else if (audienceAlone <= 0.0) audienceAlone = now;
    // (nobody after a while: most often the firewall drops the phones, the PC itself still gets in)
    const bool hint = phones == 0 && now - audienceAlone > 20000.0;
    g.setColour (juce::Colours::white);
    g.fillRect (box.withHeight (side + (hint ? 62 : 34)));
    g.setColour (juce::Colours::black);
    for (int y = 0; y < q.size; ++y)
        for (int x = 0; x < q.size; ++x)
            if (q.at (x, y)) g.fillRect (box.getX() + (x + 4) * mod, box.getY() + (y + 4) * mod, mod, mod);
    g.setFont (A.monoFont (10.0f));
    g.drawText (au->url().fromFirstOccurrenceOf ("//", false, false).trimCharactersAtEnd ("/"), box.getX(), box.getBottom() - 4, side, 16, juce::Justification::centred);
    g.drawText (phones == 0 ? U ("Handy: QR-Code scannen") : juce::String (phones) + (phones == 1 ? U (" Handy spielt mit") : U (" Handys spielen mit")),
                box.getX(), box.getBottom() + 12, side, 16, juce::Justification::centred);
    if (hint)
    {
        g.setColour (juce::Colour (0xffa03010));
        g.drawFittedText (U ("Handy l\xc3\xa4" "dt nicht? Windows-Firewall:\nRysum im privaten Netz zulassen"), box.getX() + 2, box.getBottom() + 28, side - 4, 28, juce::Justification::centred, 2, 0.7f);
    }
}
} // namespace rysum::ui
