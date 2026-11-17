#include "WalkView.h"
#include <algorithm>

// The 3D view: more hands and the room. Setting a pipe down (the right mouse button while one is held), pinning
// (the middle button), sounding a pipe as long as the mouse is held on it, the whirlwind (T), pause (P) and slow
// motion (Z held), the Saal larger or smaller (Ctrl+wheel), every surface at once (Alt+M), lanterns (K), the ears
// left behind (H), and the controls of this computer (mouse speed, upside-down mouse, walking pace).
namespace rysum::ui
{
namespace
{
juce::String U (const char* s) { return juce::String::fromUTF8 (s); }
juce::File controlsFile() { return RysumProcessor::getPatchFolder().getParentDirectory().getChildFile ("Steuerung.xml"); }
WalkView::Controls& controlsNow()
{
    static WalkView::Controls c;
    static bool read = false;
    if (! read)
    {
        read = true;
        if (auto xml = juce::parseXML (controlsFile()))
        {
            c.mouse = juce::jlimit (0.25f, 3.0f, (float) xml->getDoubleAttribute ("maus", 1.0));
            c.walk = juce::jlimit (0.4f, 2.5f, (float) xml->getDoubleAttribute ("gehen", 1.0));
            c.invertY = xml->getBoolAttribute ("umgekehrt", false);
        }
    }
    return c;
}
constexpr int kMaxLanterns = 16;
} // namespace

WalkView::Controls WalkView::controls() { return controlsNow(); }

void WalkView::setControls (const Controls& c)
{
    auto& now = controlsNow();
    now.mouse = juce::jlimit (0.25f, 3.0f, c.mouse);
    now.walk = juce::jlimit (0.4f, 2.5f, c.walk);
    now.invertY = c.invertY;
    juce::XmlElement xml ("STEUERUNG");
    xml.setAttribute ("maus", now.mouse);
    xml.setAttribute ("gehen", now.walk);
    xml.setAttribute ("umgekehrt", now.invertY);
    xml.writeTo (controlsFile());
}

void WalkView::showControlsMenu()
{
    const auto c = controls();
    juce::PopupMenu m, mouse, walk;
    const float speeds[] { 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
    const char* names[] { "sehr langsam", "langsam", "normal", "schnell", "sehr schnell" };
    for (int i = 0; i < 5; ++i)
    {
        mouse.addItem (10 + i, U (names[i]), true, std::abs (c.mouse - speeds[i]) < 0.01f);
        walk.addItem (20 + i, U (names[i]), true, std::abs (c.walk - speeds[i]) < 0.01f);
    }
    m.addSectionHeader (U ("Steuerung (auf diesem Computer)"));
    m.addSubMenu (U ("Maus: Tempo des Schauens"), mouse);
    m.addItem (1, U ("Maus umgekehrt (hoch = nach unten schauen)"), true, c.invertY);
    m.addSubMenu (U ("Gehtempo (auch Shift+Rad)"), walk);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&controlsButton),
                     [safe = juce::Component::SafePointer<WalkView> (this), speeds] (int r)
                     {
                         if (safe == nullptr || r == 0) return;
                         auto c2 = controls();
                         if (r == 1) c2.invertY = ! c2.invertY;
                         else if (r >= 10 && r < 15) c2.mouse = speeds[r - 10];
                         else if (r >= 20 && r < 25) c2.walk = speeds[r - 20];
                         setControls (c2);
                         safe->grabKeyboardFocus();
                     });
}

// ---------------------------------------------------------------------------
// where the cross meets the room
// ---------------------------------------------------------------------------
V3 WalkView::crossPoint (float fallback) const
{
    float d = 0.0f;
    if (rast.width() > 0) d = rast.depthAt (rast.width() / 2, rast.height() / 2);
    const float dist = d > 0.2f && d < 40.0f ? d : fallback;
    return eyeNow() + lookDir() * dist;
}

// ---------------------------------------------------------------------------
// setting down, pinning, sounding
// ---------------------------------------------------------------------------
bool WalkView::setDown()
{
    auto& en = proc.engine;
    if (heldStop < 0) return false;
    const int s = heldStop, k = heldKey;
    // (a little short of the surface: the pipe stands on the floor or leans at the wall, not in it)
    const V3 eye = eyeNow(), at = crossPoint (holdDist);
    const float dist = std::sqrt (dot (at - eye, at - eye));
    const V3 p = eye + lookDir() * juce::jmax (0.6f, dist - 0.3f);
    pushUndo();
    en.grabStop = -1; en.grabKey = -1;
    heldStop = heldKey = -1;
    RysumProcessor::WalkLayout one;
    one.has[(size_t) s] = (uint64_t) 1 << k;
    one.at[(size_t) s][(size_t) k] = { p.x, p.y, juce::jmax (0.2f, p.z) };
    applyLayout (one);
    say (U ("Abgestellt \xe2\x80\x93 die Pfeife bleibt dort"));
    return true;
}

bool WalkView::isPinned (int stop, int key) const noexcept
{
    return stop >= 0 && stop < kNumStops && key >= 0 && key < kNumKeys && ((proc.engine.pinMask[(size_t) stop].load() >> key) & 1u) != 0u;
}

bool WalkView::togglePin (int stop, int key)
{
    if (stop < 0 || stop >= kNumStops || key < 0 || key >= kNumKeys) return false;
    const uint64_t bit = (uint64_t) 1 << key;
    const bool now = (proc.engine.pinMask[(size_t) stop].fetch_xor (bit) & bit) == 0;
    say (now ? U ("Festgesteckt: diese Pfeife bleibt, wo sie ist (mittlere Maustaste: wieder frei)") : U ("Wieder frei"));
    repaint();
    return now;
}

bool WalkView::bowStart (int stop, int key)
{
    if (stop < 0 || stop >= kNumStops || key < 0 || key >= kNumKeys) return false;
    if (! handsMay (2)) return false;      // (listening: only with the player's leave)
    bowStop();
    proc.engine.playerAct (OrganEngine::Act::BowOn, stop, key);
    bowStop_ = stop; bowKey_ = key;
    glow[(size_t) stop][(size_t) key] = 1.0f;
    return true;
}

void WalkView::bowStop()
{
    if (bowStop_ < 0) return;
    proc.engine.playerAct (OrganEngine::Act::BowOff, bowStop_, bowKey_);
    bowStop_ = bowKey_ = -1;
}

// ---------------------------------------------------------------------------
// the whirlwind, pause, slow motion
// ---------------------------------------------------------------------------
void WalkView::whirl (float strength)
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - whirlFx < 500.0) return;
    // (the pulled stops that stand still start to hover, so there is something to spin)
    const auto params = proc.readParams();
    int woken = 0;
    for (int s = 0; s < kNumStops; ++s)
        if (params.stopOn[(size_t) s] && ! isRoomMotion (params.motionMode[(size_t) s]) && makeHover (s, false)) ++woken;
    pushUndo();
    {
        // (where the cross meets the room, but never further than five metres: a far wall would leave the pipes out)
        const V3 eye = eyeNow(), at = crossPoint (4.0f);
        const float dist = std::sqrt (dot (at - eye, at - eye));
        whirlPoint = eye + lookDir() * juce::jmin (dist, 5.0f);
    }
    whirlStrength = strength;
    whirlFx = now;
    proc.engine.playerAct (OrganEngine::Act::Whirl, -1, -1, { whirlPoint.x, whirlPoint.y, whirlPoint.z }, strength);
    if (woken > 0) whirlAgain = now + 120.0;      // (once their pipes are bodies)
    say (U ("Wirbelwind \xe2\x80\x93 die Pfeifen in seiner N\xc3\xa4he drehen sich um ihn"));
}

void WalkView::setPause (bool on)
{
    paused = on;
    proc.engine.actPause = on;
    say (on ? U ("Pause: alles steht still \xe2\x80\x93 P l\xc3\xa4sst es weiterlaufen") : U ("Weiter"));
}

void WalkView::setSlow (bool on)
{
    if (on == slow) return;
    slow = on;
    proc.engine.bodyPace = on ? 0.25f : 1.0f;
}

// ---------------------------------------------------------------------------
// the room
// ---------------------------------------------------------------------------
bool WalkView::scaleRoom (float factor)
{
    if (proc.getWalkMesh() != nullptr)
    {
        say (U ("Ein Raummodell hat seine eigene Gr\xc3\xb6\xc3\x9f" "e (\xe2\x80\x9eModell entfernen\xe2\x80\x9c: der Saal l\xc3\xa4sst sich gr\xc3\xb6\xc3\x9f" "er und kleiner ziehen)"), 3500.0);
        return false;
    }
    auto* prm = proc.apvts.getParameter (RysumProcessor::roomParamId (0, "Size"));
    if (prm == nullptr) return false;
    const float was = prm->convertFrom0to1 (prm->getValue());
    const float now = juce::jlimit (0.25f, 3.0f, was * factor);
    prm->beginChangeGesture();
    prm->setValueNotifyingHost (prm->convertTo0to1 (now));
    prm->endChangeGesture();
    const auto r = roomNow();
    say (U ("Saal \xc3\x97 ") + juce::String (now, 2) + U ("  \xc2\xb7  ") + juce::String (r.W() * now / juce::jmax (0.01f, was), 1) + U (" \xc3\x97 ")
         + juce::String (r.L() * now / juce::jmax (0.01f, was), 1) + U (" \xc3\x97 ") + juce::String (r.H() * now / juce::jmax (0.01f, was), 1)
         + U (" m  \xc2\xb7  der Nachhall w\xc3\xa4" "chst mit"));
    return true;
}

bool WalkView::materialAll()
{
    if (aimSurf < 0)
    {
        say (proc.getWalkMesh() != nullptr ? U ("Material: nur f\xc3\xbcr die Fl\xc3\xa4" "chen des Saals (ohne Modell)") : U ("Keine Fl\xc3\xa4" "che im Blick"));
        return false;
    }
    const int cur = proc.getSurfaceMaterial (0, aimSurf);
    const int next = cur + 1 >= kNumSurfMaterials ? -1 : cur + 1;
    for (int s = 0; s < kMaxSurf; ++s) proc.setSurfaceMaterial (0, s, next);
    say (U ("Alle Fl\xc3\xa4" "chen: ") + surfaceName (roomNow(), aimSurf).fromFirstOccurrenceOf (": ", false, false));
    repaint();
    return true;
}

bool WalkView::placeLantern()
{
    auto& ls = proc.walkLanterns;
    if ((int) ls.size() >= kMaxLanterns) { say (U ("Schon 16 Laternen \xe2\x80\x93 Shift+K nimmt die n\xc3\xa4" "chste weg")); return false; }
    // (a hand's breadth short of the surface, not lower than a candle on the floor)
    const V3 eye = eyeNow(), at = crossPoint (2.5f);
    const float dist = std::sqrt (dot (at - eye, at - eye));
    V3 p = eye + lookDir() * juce::jmax (0.5f, dist - 0.15f);
    p.z = juce::jmax (0.15f, p.z);
    ls.push_back ({ p.x, p.y, p.z });
    say (U ("Laterne ") + juce::String ((int) ls.size()) + (proc.walkDark ? U (" \xe2\x80\x93 sie leuchtet den Raum aus") : U (" \xe2\x80\x93 L: Licht aus, dann leuchtet sie den Raum aus")));
    repaint();
    return true;
}

bool WalkView::removeLantern()
{
    auto& ls = proc.walkLanterns;
    if (ls.empty()) { say (U ("Keine Laterne da")); return false; }
    const V3 at = crossPoint (2.5f);
    size_t best = 0;
    float bestD = 1.0e9f;
    for (size_t i = 0; i < ls.size(); ++i)
    {
        const V3 l { ls[i].x, ls[i].y, ls[i].z };
        const float d = dot (l - at, l - at);
        if (d < bestD) { bestD = d; best = i; }
    }
    ls.erase (ls.begin() + (ptrdiff_t) best);
    say (U ("Laterne weggenommen (noch ") + juce::String ((int) ls.size()) + ")");
    repaint();
    return true;
}

void WalkView::clearLanterns()
{
    proc.walkLanterns.clear();
    say (U ("Alle Laternen weggenommen"));
    repaint();
}

void WalkView::leaveEars (bool on)
{
    if (on == earsAway) return;
    earsAway = on;
    if (on)
    {
        earEye = eyeNow();
        earPose[0] = px; earPose[1] = py; earPose[2] = yaw; earPose[3] = fz;
        say (U ("Die Ohren bleiben hier \xe2\x80\x93 geh und sieh dir an, was du h\xc3\xb6rst (H: zur\xc3\xbc" "ck)"), 4000.0);
    }
    else
        say (U ("Die Ohren sind wieder bei dir"));
    pushPose();
    repaint();
}

// ---------------------------------------------------------------------------
// drawn over the picture: pins, the whirlwind, the ears left behind
// ---------------------------------------------------------------------------
void WalkView::drawMoreHands (juce::Graphics& g, juce::Rectangle<int> area)
{
    const float isc = rast.width() > 0 ? (float) area.getWidth() / (float) rast.width() : 1.0f;
    auto at = [&] (V3 p, juce::Point<float>& out, float& depth)
    {
        float sx, sy;
        if (! rast.project (p, sx, sy, depth)) return false;
        out = { sx * isc, sy * isc };
        return true;
    };
    // pinned pipes: a nail head at the top
    for (int s = 0; s < kNumStops; ++s)
    {
        const uint64_t m = proc.engine.pinMask[(size_t) s].load();
        if (m == 0) continue;
        for (int k = 0; k < kNumKeys; ++k)
        {
            if (((m >> k) & 1u) == 0u) continue;
            const auto& sh = shown[(size_t) s][(size_t) k];
            juce::Point<float> c;
            float d;
            if (! sh.on || ! at (sh.c + V3 { 0, 0, 0.5f * sh.len }, c, d)) continue;
            const float r = juce::jlimit (3.0f, 7.0f, 5.0f / juce::jmax (0.5f, d) * 2.0f);
            g.setColour (col::bg.withAlpha (0.8f));
            g.fillEllipse (juce::Rectangle<float> (2.0f * r + 3.0f, 2.0f * r + 3.0f).withCentre (c));
            g.setColour (col::ink);
            g.drawEllipse (juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (c), 1.2f);
            g.fillEllipse (juce::Rectangle<float> (2.5f, 2.5f).withCentre (c));
        }
    }
    // the lanterns: a small flame each, so they can be found (and taken away) in the light too
    for (const auto& l : proc.walkLanterns)
    {
        juce::Point<float> c;
        float d;
        if (! at ({ l.x, l.y, l.z }, c, d)) continue;
        const float r = juce::jlimit (2.5f, 9.0f, 0.06f / juce::jmax (0.3f, d) * 0.5f * (float) area.getWidth());
        g.setColour (juce::Colour (0xfff6e7c8).withAlpha (0.25f));
        g.fillEllipse (juce::Rectangle<float> (5.0f * r, 5.0f * r).withCentre (c));
        g.setColour (juce::Colour (0xfffff4e0));
        drawLozenge (g, c.translated (0.0f, -0.3f * r), 0.7f * r, 1.5f * r);
        g.setColour (col::ink.withAlpha (0.7f));
        g.fillRect (juce::Rectangle<float> (0.9f * r, 0.5f * r).withCentre (c.translated (0.0f, 1.4f * r)));
    }
    // the whirlwind: rings turning around its axis, fading
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float age = (float) ((now - whirlFx) / 4000.0);
    if (whirlFx > 0.0 && age >= 0.0f && age < 1.0f)
    {
        const float spin = (float) (now / 1000.0) * 3.0f;
        for (int ring = 0; ring < 3; ++ring)
        {
            const float rad = 1.2f + 1.6f * (float) ring, z = 0.4f + 0.9f * (float) ring;
            juce::Path p;
            bool started = false;
            for (int i = 0; i <= 20; ++i)
            {
                const float a = spin * (1.0f - 0.25f * (float) ring) + (float) i / 20.0f * 4.2f;
                juce::Point<float> q;
                float d;
                if (! at ({ whirlPoint.x + rad * std::cos (a), whirlPoint.y + rad * std::sin (a), z }, q, d)) { started = false; continue; }
                if (! started) { p.startNewSubPath (q); started = true; }
                else p.lineTo (q);
            }
            g.setColour (col::ink.withAlpha (0.5f * (1.0f - age)));
            g.strokePath (p, juce::PathStrokeType (1.3f));
        }
    }
    else if (whirlFx > 0.0 && age >= 1.0f)
        whirlFx = 0.0;
    // the ears left behind: a red ring where they are
    if (earsAway)
    {
        juce::Point<float> c;
        float d;
        if (at (earEye, c, d))
        {
            const float r = juce::jlimit (6.0f, 26.0f, 0.25f / juce::jmax (0.3f, d) * 0.5f * (float) area.getWidth());
            g.setColour (col::rubric.brighter (0.3f));
            g.drawEllipse (juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (c), 1.6f);
            g.drawEllipse (juce::Rectangle<float> (2.0f * r + 7.0f, 2.0f * r + 7.0f).withCentre (c), 0.7f);
            g.setFont (Assets::get().monoFont (10.0f));
            g.drawText (U ("deine Ohren"), juce::Rectangle<float> (120.0f, 14.0f).withCentre (c.translated (0.0f, r + 12.0f)), juce::Justification::centred);
        }
    }
}
} // namespace rysum::ui
