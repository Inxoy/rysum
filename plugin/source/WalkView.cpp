#include "WalkView.h"
#include "MeshAcoustics.h"
#include "RoomSim.h"
#include "RoomModels.h"
#include "SpatialMeta.h"
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
// where 360° recordings go: chosen once per computer (Documents/Rysum/360.xml), else Documents/Rysum/360
juce::File panoDefaultFolder() { return RysumProcessor::getPatchFolder().getParentDirectory().getChildFile ("360"); }
juce::File panoSettingsFile() { return RysumProcessor::getPatchFolder().getParentDirectory().getChildFile ("360.xml"); }
juce::File panoBaseFolder()
{
    if (auto xml = juce::parseXML (panoSettingsFile()))
    {
        const auto path = xml->getStringAttribute ("ordner");
        if (path.isNotEmpty() && juce::File::isAbsolutePath (path)) return juce::File (path);
    }
    return panoDefaultFolder();
}
void setPanoBaseFolder (const juce::File& f)
{
    juce::XmlElement xml ("AUFNAHME360");
    xml.setAttribute ("ordner", f == panoDefaultFolder() ? juce::String() : f.getFullPathName());
    xml.writeTo (panoSettingsFile());
}

juce::String panoTooltip()
{
    return rysum::tr("Ein 360\xc2\xb0-Video aufnehmen: das Bild rundum (equirektangular) und der Raumklang als Ambisonics 1. Ordnung (AmbiX), "
                           "beide fest im Saal. Der Ausgang muss Ambisonics sein (AmbiX, Kopfh\xc3\xb6rer oder Lautsprecher). Noch einmal klicken: Ende. "
                           "Dann werden die Bilder gezeichnet (von der Grafikkarte, wenn sie es kann) und, wenn ffmpeg installiert ist, wird daraus "
                           "rysum360.mov \xe2\x80\x93 schon mit den Metadaten f\xc3\xbcr 360\xc2\xb0 und den Raumklang, fertig f\xc3\xbcr YouTube. Der Ordner \xc3\xb6" "ffnet sich.");
}
} // namespace

WalkView::WalkView (RysumProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
    for (juce::Button* b : std::initializer_list<juce::Button*> { &back, &load, &clear, &fit, &zUp, &home, &hear, &immerse, &actShock, &actGrab, &actFreeze, &actHome, &actClap, &actMat, &actJump, &actLight, &actAudience, &actPano, &controlsButton, &actLook })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    back.onClick = [this] { if (onBack) onBack(); };
    immerse.setTooltip (rysum::tr("Nur das Bild, \xc3\xbc" "ber das ganze Fenster: keine Kn\xc3\xb6pfe, keine Schrift, keine Karte. Die Maus schaut sich um, "
                           "ohne dass eine Taste gehalten wird (der Zeiger ist versteckt). Esc oder I: zur\xc3\xbc" "ck."));
    immerse.onClick = [this] { toggleImmersive(); };
    actShock.setTooltip (rysum::tr("Eine Druckwelle st\xc3\xb6\xc3\x9ft alle Pfeifen von dir weg (Taste E, mit Shift st\xc3\xa4rker)"));
    actShock.onClick = [this] { shock (juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown() ? 1.7f : 1.0f); };
    actGrab.setTooltip (rysum::tr("Gedr\xc3\xbc" "ckt halten: die Pfeife unter dem Kreuz greifen und tragen; loslassen: werfen (Taste Q)"));
    actFreeze.setTooltip (rysum::tr("Gedr\xc3\xbc" "ckt halten: alles, was sich bewegt, steht still (Taste F)"));
    actHome.setTooltip (rysum::tr("Alle Pfeifen fliegen an ihren Platz in der Orgel zur\xc3\xbc" "ck (Taste R)"));
    actHome.onClick = [this] { callHome(); };
    actClap.setTooltip (rysum::tr("In die H\xc3\xa4nde klatschen, wo du stehst (Taste C)"));
    actClap.onClick = [this] { clap(); };
    actMat.setTooltip (rysum::tr("Das Material der Fl\xc3\xa4" "che unter dem Kreuz wechseln (Taste M)"));
    actMat.onClick = [this] { cycleMaterial(); };
    actJump.setTooltip (rysum::tr("Springen (Leertaste)"));
    actLight.setClickingTogglesState (true);
    actLight.setToggleState (proc.walkDark, juce::dontSendNotification);
    actLight.setTooltip (rysum::tr("Licht aus: nur die klingenden Pfeifen (und die Funken ihrer St\xc3\xb6\xc3\x9f" "e) erhellen den Raum (Taste L)"));
    actLight.onClick = [this] { setDark (actLight.getToggleState()); };
    actAudience.setClickingTogglesState (true);
    actAudience.setTooltip (rysum::tr("Das Publikum spielt mit: ein QR-Code f\xc3\xbchrt die Handys im selben WLAN zu einer kleinen Seite. Jedes Handy bekommt "
                               "eine Pfeife in eigener Farbe: Tippen schl\xc3\xa4gt sie an, schnell Wischen schickt eine Druckwelle, Halten und Ziehen "
                               "bl\xc3\xa4st die Pfeifen wie Wind (nach oben = wohin der Blick geht). Nur Pfeifen, die frei im Saal sind, spielen mit; "
                               "beim Einschalten schweben alle gezogenen Register, die stillstehen. Wird nicht gespeichert."));
    actAudience.onClick = [this] { setAudience (actAudience.getToggleState()); };
    actPano.onClick = [this] { panoClicked(); };
    actPano.setTooltip (panoTooltip());
    panoFinishedSeen = proc.pano360.finishedCount();
    // (test, RYSUM_360_TEST, on the standalone with this page open: a recording of 2.5 s while the view goes on
    //  drawing, then Documents/Rysum/360-test.txt - who drew the frames, the video, the view's card)
    if (juce::SystemStats::getEnvironmentVariable ("RYSUM_360_TEST", {}).isNotEmpty())
        juce::Timer::callAfterDelay (3000, [safe = juce::Component::SafePointer<WalkView> (this)] { if (safe != nullptr) safe->run360Test (0); });
    controlsButton.setTooltip (rysum::tr("Wie sich Maus und Gehen anf\xc3\xbchlen (auf diesem Computer): Tempo des Schauens, Maus umgekehrt, Gehtempo. Tab gehalten: alle Tasten."));
    controlsButton.onClick = [this] { showControlsMenu(); };
    back.setTooltip (rysum::tr("Zur\xc3\xbc" "ck zum Saal (Esc)"));
    load.onClick = [this]
    {
        // the rooms that come with the plugin, or a file of one's own
        juce::PopupMenu m;
        m.addSectionHeader (rysum::tr("R\xc3\xa4ume, die dabei sind"));
        const auto& rooms = builtInRooms();
        for (int i = 0; i < (int) rooms.size(); ++i)
            m.addItem (i + 1, juce::String::fromUTF8 (rooms[(size_t) i].name) + U ("  \xe2\x80\x93  ") + juce::String::fromUTF8 (rooms[(size_t) i].about),
                       true, builtInRoomIndex (proc.walkModelFile.getFileName()) == i && proc.getWalkMesh() != nullptr);
        m.addSeparator();
        m.addItem (100, rysum::tr("Eigene Datei (OBJ) \xe2\x80\xa6"));
        juce::Component::SafePointer<WalkView> safe (static_cast<WalkView*> (this));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&load), [safe] (int r)
        {
            if (safe == nullptr || r == 0) return;
            if (r == 100) safe->chooseModel();
            else safe->loadBuiltInRoom (r - 1);
        });
    };
    load.setTooltip (rysum::tr("Ein 3D-Modell (OBJ, mit MTL-Farben) als Raum: zu sehen, zu begehen (seine W\xc3\xa4nde halten dich auf) und \xe2\x80\x93 mit \xe2\x80\x9eModell h\xc3\xb6rbar\xe2\x80\x9c \xe2\x80\x93 zu h\xc3\xb6ren. Der Quader des Saals liegt darunter."));
    clear.onClick = [this] { proc.clearWalkModel(); status = rysum::tr("Modell entfernt"); statusUntil = juce::Time::getMillisecondCounterHiRes() + 2500; updateButtons(); repaint(); };
    fit.setTooltip (rysum::tr("Der Saal bekommt die Ma\xc3\x9f" "e des Modells (rechteckig, Breite \xc3\x97 L\xc3\xa4nge \xc3\x97 H\xc3\xb6he)"));
    fit.onClick = [this]
    {
        auto m = proc.getWalkMesh();
        if (m == nullptr || ! proc.fitSaalToWalkModel()) return;
        status = U ("Saal: ") + juce::String (m->size.x, 1) + U (" \xc3\x97 ") + juce::String (m->size.y, 1) + U (" \xc3\x97 ") + juce::String (m->size.z, 1) + " m";
        statusUntil = juce::Time::getMillisecondCounterHiRes() + 3000;
    };
    zUp.setClickingTogglesState (true);
    zUp.setTooltip (rysum::tr("Das Modell hat Z als Hochachse (sonst Y, wie meist bei OBJ)"));
    zUp.onClick = [this] { if (proc.walkModelFile.existsAsFile()) { proc.walkZUp = zUp.getToggleState(); loadModel (proc.walkModelFile); } else proc.walkZUp = zUp.getToggleState(); };
    home.setTooltip (rysum::tr("Zur\xc3\xbc" "ck an den H\xc3\xb6rerplatz des Saals, Blick nach vorn"));
    home.onClick = [this] { px = py = yaw = pitch = yawT = pitchT = vx = vy = vz = fz = turnV = 0.0f; onGround = true; pushPose(); repaint(); };
    units.addItemList ({ "Meter", rysum::tr("Zentimeter"), rysum::tr("Millimeter"), rysum::tr("Zoll"), rysum::tr("Fu\xc3\x9f") }, 1);
    units.setTooltip (rysum::tr("Einheit des Modells"));
    units.setWantsKeyboardFocus (false);
    units.onChange = [this]
    {
        proc.walkScale = kUnitScale[juce::jlimit (0, 4, units.getSelectedItemIndex())];
        if (proc.walkModelFile.existsAsFile()) loadModel (proc.walkModelFile);
    };
    addAndMakeVisible (units);
    hear.setClickingTogglesState (true);
    hear.setTooltip (rysum::tr("Das Modell ist auch zu h\xc3\xb6ren: Pfeifen hinter seinen W\xc3\xa4nden kommen um die Ecke, der Nachhall wird per "
                        "Raytracing durch das Modell berechnet. Der Quader des Saals liegt darunter (fr\xc3\xbche Reflexionen, Gr\xc3\xb6\xc3\x9f" "e). "
                        "Aus: das Modell ist nur zu sehen."));
    hear.onClick = [this] { proc.setWalkHear (hear.getToggleState()); };
    // the player and the pipes ("Spieler": the pipes that are bodies in the Saal follow, flee or circle the walker)
    for (int i = 0; i < 5; ++i)
        playerMode.addItem (juce::StringArray { U ("Pfeifen: ruhig"), rysum::tr("Pfeifen: ich ziehe an"), rysum::tr("Pfeifen: ich sto\xc3\x9f" "e ab"), rysum::tr("Pfeifen: umkreisen mich"), rysum::tr("Pfeifen: Sog") }[i], i + 1);
    playerMode.setWantsKeyboardFocus (false);
    playerMode.setTooltip (rysum::tr("Wie du die Pfeifen bewegst, w\xc3\xa4hrend du gehst: wie eine Masse, zu der sie fallen (anziehen), vor der sie fliehen "
                              "(absto\xc3\x9f" "en), um die sie kreisen (Umlaufbahn) oder die sie im Vorbeigehen mitrei\xc3\x9ft (Sog). "
                              "Wirkt auf Register, deren Bewegung (Raum-Seite) im Saal stattfindet: Flug, Fall, Schwarm, Str\xc3\xb6mung - und "
                              "Schweben, das nur auf dich reagiert."));
    playerModeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "lvPlayerMode", playerMode);
    playerMode.onChange = [this] { if (isShowing()) grabKeyboardFocus(); };
    addAndMakeVisible (playerMode);
    playerForce.setSliderStyle (juce::Slider::LinearBar);
    playerForce.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    playerForce.setWantsKeyboardFocus (false);
    playerForce.setTooltip (rysum::tr("Kraft, mit der du die Pfeifen bewegst"));
    playerForceAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "lvPlayerForce", playerForce);
    addAndMakeVisible (playerForce);
    {
        const char* ids[] { "lvSelfPlay", "lvGrow", "lvWhisper", "lvTimeDepth", "lvRoomHear", "lvWallPull" };
        const juce::String names[] { "Selbstspiel", rysum::tr("Wachsen & Brechen"), rysum::tr("Fl\xc3\xbcstern"), rysum::tr("Zeit als Entfernung"), rysum::tr("H\xc3\xb6rraum"), rysum::tr("Wandn\xc3\xa4he") };
        const juce::String tips[] {
            U ("Der Raum spielt sich selbst: jede Pfeife, die gegen eine Wand, den Boden oder die Decke st\xc3\xb6\xc3\x9ft, klingt mit ihrem eigenen Ton \xe2\x80\x93 "
               "je h\xc3\xa4rter der Sto\xc3\x9f, desto l\xc3\xa4nger. Braucht Register, deren Bewegung im Saal stattfindet (Flug, Fall, Schwarm, Str\xc3\xb6mung)."),
            rysum::tr("Pfeifen, die nicht klingen, wachsen langsam und werden tiefer (bis zur doppelten L\xc3\xa4nge, eine Oktave). Ein harter Sto\xc3\x9f bricht eine Pfeife "
               "entzwei: halb so lang, eine Oktave h\xc3\xb6her. Mehr = schnelleres Wachsen, leichteres Brechen. Aus: jede Pfeife findet zur\xc3\xbc" "ck; R heilt alle sofort."),
            rysum::tr("Eine Pfeife ganz nah am Kopf (unter 1,4 m) ist auch direkt im Ohr zu h\xc3\xb6ren: das nahe Ohr viel lauter, das ferne sp\xc3\xa4ter und dumpfer, der Klang w\xc3\xa4rmer. "
               "Pfeife greifen (Q) und mit dem Mausrad heranholen. Nur \xc3\xbc" "ber Kopfh\xc3\xb6rer (binaural) und Stereo."),
            rysum::tr("Das vordere Ende des Saals spielt die Vergangenheit: am gewohnten Platz h\xc3\xb6rst du die Gegenwart, an der Stirnwand das, was vor so vielen Sekunden "
               "erklang. Schnell nach vorn gehen: die Zeit l\xc3\xa4uft r\xc3\xbc" "ckw\xc3\xa4rts; zur\xc3\xbc" "ckgehen: sie holt auf."),
            rysum::tr("Der Nachhall geh\xc3\xb6rt zu einem Raum anderer Gr\xc3\xb6\xc3\x9f" "e als der, den du siehst und begehst: eine Kapelle sehen, eine Kathedrale h\xc3\xb6ren (oder umgekehrt). "
               "Braucht \xe2\x80\x9eSaal an\xe2\x80\x9c."),
            rysum::tr("Je n\xc3\xa4her du einer Wand kommst (ab 3 m), desto gr\xc3\xb6\xc3\x9f" "er und lauter wird der Nachhall \xe2\x80\x93 als \xc3\xb6" "ffne sich hinter der Wand ein weiterer Raum. Braucht \xe2\x80\x9eSaal an\xe2\x80\x9c.") };
        for (int i = 0; i < 6; ++i)
        {
            illusions[(size_t) i] = std::make_unique<ParamRow> (proc.apvts, ids[i], names[i], true);
            illusions[(size_t) i]->setTooltip (tips[i]);
            illusions[(size_t) i]->setWantsKeyboardFocus (false);
            addAndMakeVisible (*illusions[(size_t) i]);
        }
    }
    {
        // "Stimmung": every part of the look its own control (each can be modulated and drawn in the Verlauf)
        struct Row { const char* id; const char* name; const char* tip; };
        static const Row rows[kLookRows] {
            { "lookDay", "Tageslicht", "Wie sehr die Tageszeit das Licht bestimmt: woher es kommt, seine Farbe, wie hell. 0 = das gewohnte graue Licht." },
            { "lookHour", "Uhrzeit", "Die Tageszeit: Morgen (goldenes, flaches Licht von rechts), Mittag (hell, von oben), Abend (orange, von links), Nacht (dunkelblaues Mondlicht). Wirkt mit \xe2\x80\x9eTageslicht\xe2\x80\x9c; die Schatten der Pfeifen werden mit tiefer Sonne l\xc3\xa4nger." },
            { "lookRays", "Lichtstrahlen", "Lichtb\xc3\xbcndel, die schr\xc3\xa4g durch den Saal fallen (aus der Richtung der Sonne, mit \xe2\x80\x9eTageslicht\xe2\x80\x9c), im Dunkeln von den klingenden Pfeifen." },
            { "lookRayWidth", "Strahlbreite", "Wie breit jedes Lichtb\xc3\xbcndel ist." },
            { "lookDust", "Staub", "Staub, der in der Luft treibt und im Licht der Strahlen und Pfeifen aufleuchtet." },
            { "lookDustStir", "Staub aufwirbeln", "Wie sehr der Staub aufgewirbelt wird, wenn der Kalkant tritt, ein lauter Akkord einsetzt oder eine Pfeife aufschl\xc3\xa4gt." },
            { "lookPipeLight", "Pfeifenlicht", "Die klingenden Pfeifen werfen farbiges Licht auf Boden und W\xc3\xa4nde \xe2\x80\x93 auch bei Licht, im Dunkeln (L) st\xc3\xa4rker." },
            { "lookPipeHue", "Lichtfarbe", "Von warmem Wei\xc3\x9f bis zur eigenen Farbe jedes Registers (Praestant gold, Gedackt bernstein, Trompete rot, Mixtur eisblau \xe2\x80\xa6)." },
            { "lookPipeReach", "Reichweite", "Wie weit das Licht einer Pfeife reicht." },
            { "lookTrails", "Leuchtspuren", "Pfeifen, die durch den Saal fliegen, fallen oder schw\xc3\xa4rmen, ziehen Lichtspuren hinter sich her \xe2\x80\x93 wie bei langer Belichtung." },
            { "lookTrailLen", "Spurl\xc3\xa4nge", "Wie lange eine Lichtspur nachleuchtet." },
            { "lookFog", "Hallnebel", "Ein Nebel, so dicht wie der Nachhall laut ist: nach einem Akkord h\xc3\xa4ngt er so lange in der Luft, wie der Saal nachklingt, dann klart es auf." },
            { "lookFogFollow", "Nebel folgt Hall", "Wie sehr der Nebel dem Nachhall folgt: 0 = er steht immer im Saal, 100 % = nur so viel, wie gerade nachklingt." },
            { "lookDoors", "T\xc3\xbcren sichtbar", "Die T\xc3\xbcr zu einem anderen Saal (Saal-Seite) ist zu sehen: ein T\xc3\xbcrfl\xc3\xbcgel, der so weit offen steht wie die T\xc3\xbcr, und dahinter der andere Saal." },
            { "lookDoorDim", "Nachbarsaal hell", "Wie hell der Saal hinter der T\xc3\xbcr ist." },
            { "lookWalls", "W\xc3\xa4nde vergehen", "W\xc3\xa4nde, Boden und Decke, die die Raumtastatur wegnimmt, l\xc3\xb6sen sich auf und kommen zur\xc3\xbc" "ck, wenn die Taste losgelassen wird." },
            { "lookWallGrain", "Zerfall in St\xc3\xbc" "cke", "Wie eine Wand vergeht: 0 = sie verblasst Punkt f\xc3\xbcr Punkt, mehr = sie zerf\xc3\xa4llt in immer gr\xc3\xb6\xc3\x9f" "ere St\xc3\xbc" "cke." },
            { "lookWear", "Gebrauchsspuren", "Wasserflecken auf Stein und Putz, deutlichere Maserung im Holz, angelaufenes Metall." },
            { "lookShine", "Glanz", "Glas, Spiegel und Metall fangen das Licht: hellere Glanzlichter." },
            { "lookRipple", "Wellen", "Wo eine Pfeife aufschl\xc3\xa4gt, l\xc3\xa4uft eine kurze Welle \xc3\xbc" "ber die Fl\xc3\xa4" "chen." },
            { "lookSway", "Kamera schwankt", "Der Blick schwankt beim Gehen st\xc3\xa4rker, und im Stehen atmet er ein wenig." },
            { "lookShake", "Kamera bebt", "Ein harter Aufprall in der N\xc3\xa4he ersch\xc3\xbcttert den Blick." },
            { "lookFocus", "Sch\xc3\xa4rfentiefe", "Scharf ist, worauf du schaust (die Pfeife unter dem Kreuz); davor und dahinter verschwimmt es." },
            { "choreoPattern", "Choreografie", "Eine Figur, die alle Pfeifen einmal tanzen: Spirale aufw\xc3\xa4rts, Einsturz, Erbl\xc3\xbchen, Wirbel, Welle, Zerstreuen \xe2\x80\x93 jede Pfeife etwas sp\xc3\xa4ter als die unter ihr. Los mit J, dem Knopf, dem Schalter \xe2\x80\x9e" "Choreografie los\xe2\x80\x9c (MIDI, Verlauf) oder auf dem Kanal der Raumtastatur mit C1 bis H1 (jede Taste eine Figur, der Anschlag wie weit)." },
            { "choreoTime", "Dauer", "Wie lange die Figur dauert." },
            { "choreoAmt", "Weite", "Wie weit die Pfeifen sich bewegen." } };
        for (int i = 0; i < kLookRows; ++i)
        {
            auto row = std::make_unique<ParamRow> (proc.apvts, rows[i].id, rysum::tr (rows[i].name), true);
            row->setTooltip (rysum::tr (rows[i].tip));
            row->setWantsKeyboardFocus (false);
            addChildComponent (*row);
            lookRows[(size_t) i] = std::move (row);
        }
        actLook.setClickingTogglesState (true);
        actLook.setTooltip (rysum::tr ("Die Stimmung des Saals: Tageslicht, Lichtstrahlen und Staub, das Licht der Pfeifen, Leuchtspuren, Hallnebel, "
                                       "sichtbare T\xc3\xbcren, vergehende W\xc3\xa4nde, Gebrauchsspuren, Glanz, Wellen, die Kamera. Alles l\xc3\xa4sst sich "
                                       "modulieren (Rechtsklick) und im Verlauf zeichnen."));
        actLook.onClick = [this] { showLook (actLook.getToggleState()); };
        // the named looks
        lookPreset.setTextWhenNothingSelected (rysum::tr ("Stimmung laden \xe2\x80\xa6"));
        const auto names = lookPresetNames();
        for (int i = 0; i < names.size(); ++i) lookPreset.addItem (rysum::tr (names[i]), i + 1);
        lookPreset.setWantsKeyboardFocus (false);
        lookPreset.setTooltip (rysum::tr ("Eine fertige Stimmung: alle Regler der Atmosph\xc3\xa4re auf einmal (was sie nicht nennt, wie am Anfang), "
                                          "ob der Saal offen ist und das Licht. Danach frei weiter einstellbar."));
        lookPreset.onChange = [this]
        {
            const int i = lookPreset.getSelectedItemIndex();
            if (i >= 0) applyLookPreset (i);
            lookPreset.setSelectedItemIndex (-1, juce::dontSendNotification);
            if (isShowing()) grabKeyboardFocus();
        };
        addChildComponent (lookPreset);
        actChoreo.setTooltip (rysum::tr ("Die Pfeifen tanzen die Figur (J)"));
        actChoreo.setWantsKeyboardFocus (false);
        actChoreo.onClick = [this] { ++proc.engine.choreoUiCount; };
        addChildComponent (actChoreo);
    }
    updateButtons();
}

juce::StringArray WalkView::lookPresetNames()
{
    return { "Klarer Blick", "Morgenandacht", "Mittagslicht", "Abendrot", "Mitternachtsmesse", "Ruine im Nebel",
             "Hof unter Sternen", "Verlassene Kirche" };
}

void WalkView::applyLookPreset (int index)
{
    struct P { std::vector<std::pair<const char*, float>> v; int open; bool dark; };
    static const P presets[] {
        { {}, 0, false },
        { { { "lookDay", 1.0f }, { "lookHour", 7.5f }, { "lookRays", 0.8f }, { "lookRayWidth", 1.5f }, { "lookDust", 0.6f },
            { "lookDustStir", 0.4f }, { "lookPipeLight", 0.3f }, { "lookPipeHue", 0.3f }, { "lookFog", 0.2f }, { "lookShine", 0.3f },
            { "lookSway", 0.2f } }, 0, false },
        { { { "lookDay", 1.0f }, { "lookHour", 12.5f }, { "lookRays", 0.4f }, { "lookDust", 0.3f }, { "lookShine", 0.5f } }, 0, false },
        { { { "lookDay", 1.0f }, { "lookHour", 19.8f }, { "lookRays", 1.0f }, { "lookRayWidth", 2.0f }, { "lookDust", 0.5f },
            { "lookPipeLight", 0.5f }, { "lookPipeHue", 0.6f }, { "lookFog", 0.3f }, { "lookWear", 0.3f } }, 0, false },
        { { { "lookPipeLight", 1.0f }, { "lookPipeHue", 0.8f }, { "lookPipeReach", 5.0f }, { "lookRays", 0.6f }, { "lookDust", 0.4f },
            { "lookTrails", 0.4f }, { "lookFog", 0.4f }, { "lookFocus", 0.5f } }, 0, true },
        { { { "lookDay", 1.0f }, { "lookHour", 9.0f }, { "lookFog", 0.8f }, { "lookFogFollow", 0.3f }, { "lookWear", 1.0f },
            { "lookWalls", 1.0f }, { "lookWallGrain", 0.6f }, { "lookDust", 0.3f }, { "lookSway", 0.3f } }, 2, false },
        { { { "lookDay", 1.0f }, { "lookHour", 23.0f }, { "lookPipeLight", 0.8f }, { "lookPipeHue", 0.6f }, { "lookTrails", 0.6f },
            { "lookDust", 0.2f } }, 1, false },
        { { { "lookDay", 0.8f }, { "lookHour", 15.0f }, { "lookWear", 1.0f }, { "lookDust", 0.8f }, { "lookRays", 0.7f },
            { "lookShine", 0.2f }, { "lookRipple", 0.6f }, { "lookShake", 0.5f } }, 0, false } };
    if (index < 0 || index >= (int) (sizeof (presets) / sizeof (presets[0]))) return;
    const auto& pr = presets[index];
    auto set = [this] (const juce::String& id, float real, bool asDefault)
    {
        auto* prm = proc.apvts.getParameter (id);
        if (prm == nullptr) return;
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (asDefault ? prm->getDefaultValue() : prm->convertTo0to1 (real));
        prm->endChangeGesture();
    };
    // every look control: what the preset names, else as at first
    for (const auto& row : lookRows)
    {
        if (row == nullptr) continue;
        const auto id = row->getParamId();
        if (id.startsWith ("choreo")) continue;
        auto it = std::find_if (pr.v.begin(), pr.v.end(), [&] (const auto& kv) { return id == kv.first; });
        set (id, it != pr.v.end() ? it->second : 0.0f, it == pr.v.end());
    }
    set ("simOpen", (float) pr.open, false);
    if (pr.dark != proc.walkDark) setDark (pr.dark);
    say (rysum::tr ("Stimmung: ") + rysum::tr (lookPresetNames()[index]));
}

void WalkView::showLook (bool on)
{
    actLook.setToggleState (on, juce::dontSendNotification);
    for (auto& r : lookRows) if (r != nullptr) r->setVisible (on);
    actChoreo.setVisible (on);
    lookPreset.setVisible (on);
    resized();
    repaint();
}

WalkView::~WalkView()
{
    markStop = true;                       // (a video being marked: stopped, its half-written copy removed)
    if (markThread.joinable()) markThread.join();
    proc.pano360.stop();                   // (a 360 recording ends with the view: the frames are drawn on their own)
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
    r.removeFromLeft (6);
    actPano.setBounds (r.removeFromLeft (112));
    {
        // the hands: a row above the hints at the bottom
        auto h = getLocalBounds().reduced (18, 0).withTrimmedBottom (26).removeFromBottom (22);
        handsArea = h;
        const int widths[] { 104, 88, 92, 104, 96, 92, 78, 96, 80, 92, 84 };
        juce::Button* hands[] { &actShock, &actGrab, &actFreeze, &actHome, &actClap, &actMat, &actJump, &actLight, &actAudience, &controlsButton, &actLook };
        const bool sp = isSpectator();
        for (int i = 0; i < 11; ++i)
        {
            // (listening: only jumping, the light and the controls - packed together)
            if (sp && ! shownWhileListening (hands[i])) continue;
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
    {
        // "Stimmung": four columns under the illusions
        auto area = getLocalBounds().reduced (18, 0).withTrimmedTop (104).removeFromTop (7 * 34 + 12);
        lookArea = area;
        area = area.reduced (10, 6);
        const int cw = (area.getWidth() - 3 * 20) / 4;
        for (int i = 0; i < kLookRows; ++i)
        {
            const int col = i / 7, rw = i % 7;
            if (lookRows[(size_t) i] != nullptr)
                lookRows[(size_t) i]->setBounds (area.getX() + col * (cw + 20), area.getY() + rw * 34, cw, 32);
        }
        actChoreo.setBounds (area.getX() + 3 * (cw + 20), area.getY() + 5 * 34 + 4, 90, 24);
        lookPreset.setBounds (area.getX() + 3 * (cw + 20), area.getY() + 6 * 34 + 4, juce::jmin (cw, 240), 24);
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
        stopPano();                            // (the 360 recording ends with the view)
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
    if (isSpectator() != spectatorShown || handLevel() != handsShown) refreshSpectator();      // (Konzert: listening began or ended)
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
        if (isSpectator()) updateActions (handsMay (2) && (down ('Q') || down ('q') || grabHeld), false);
        else updateActions (down ('Q') || down ('q') || grabHeld, down ('F') || down ('f') || freezeHeld);
        { const bool tab = down (juce::KeyPress::tabKey); if (tab != helpHeld) { helpHeld = tab; repaint(); } }
        {
            const auto m = juce::ModifierKeys::getCurrentModifiersRealtime();
            setSlow ((down ('Z') || down ('z')) && ! m.isCtrlDown() && ! m.isCommandDown() && ! isSpectator());
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
    if (proc.pano360.state() != Pano360Recorder::State::idle || actPano.getToggleState() || proc.pano360.finishedCount() != panoFinishedSeen) updatePanoLabel();
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
        gpuNote = rysum::tr("kein OpenGL 3.2");
        gpuGaveUp = true;
        gpu.reset();
    }
}

juce::String WalkView::pictureInfo() const
{
    if (gpuOn()) return rysum::tr("Grafikkarte (") + gpu->cardName() + "), " + juce::String (gpu->framesPerSecond(), 1) + rysum::tr(" Bilder/s");
    return rysum::tr("Prozessor") + (gpuNote.isNotEmpty() ? rysum::tr(" (Grafikkarte: ") + gpuNote + ")" : juce::String());
}

bool WalkView::isSpectator() const noexcept
{
    return alwaysSpectator || proc.concert.role() == ConcertLink::Role::listener;
}

int WalkView::handLevel() const noexcept
{
    return isSpectator() ? proc.concert.allowedHands() : 2;
}

bool WalkView::shownWhileListening (juce::Component* c) const
{
    // walking and looking always; the hands the player allows
    if (c == &back) return ! alwaysSpectator;
    if (c == &immerse || c == &home || c == &actJump || c == &controlsButton || c == &actLight || c == &actPano || c == &actLook) return true;
    if (c == &actShock || c == &actClap) return handsMay (1);
    if (c == &actGrab || c == &actHome) return handsMay (2);
    return false;
}

void WalkView::refreshSpectator()
{
    // listening: only what walks and looks stays (the rest is the player's)
    const bool sp = isSpectator();
    spectatorShown = sp;
    if (immersive) return;
    resized();
    handsShown = handLevel();
    for (auto* c : getChildren())
        c->setVisible (! sp || shownWhileListening (c));
    if (sp && ! handsMay (2))
    {
        heldStop = heldKey = -1;
        proc.engine.grabStop = -1; proc.engine.grabKey = -1;
        clearSelection();
        if (magnet != 0) setMagnet (0);
        bowStop();
        setFreeze (false);
    }
    repaint();
}

bool WalkView::keyPressed (const juce::KeyPress& k)
{
    if (isSpectator())
    {
        // walking, looking, the picture - nothing that touches the organ
        if (k == juce::KeyPress::escapeKey)
        {
            if (immersive) setImmersive (false);
            else if (onBack && ! alwaysSpectator) onBack();
            return true;
        }
        const auto c = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());
        const bool ctrlKey = k.getModifiers().isCtrlDown() || k.getModifiers().isCommandDown();
        if (c == 'i' && ! ctrlKey) { toggleImmersive(); return true; }
        if (c == 'l') { setDark (! proc.walkDark); return true; }
        if (c == 'h' && ! ctrlKey) { leaveEars (! earsAway); return true; }
        if (c == 'g' && ! ctrlKey) { setGpuWanted (! gpuWanted()); gpuGaveUp = false; gpuNote = {}; return true; }
        // the hands, as far as the player allows
        const bool hard = juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
        if (c == 'e' && handsMay (1)) { shock (hard ? 1.7f : 1.0f); return true; }
        if (c == 'c' && handsMay (1)) { clap(); return true; }
        if (c == 'r' && ! ctrlKey && handsMay (2)) { callHome(); return true; }
        if (c == 't' && ! ctrlKey && handsMay (2)) { whirl (hard ? 1.7f : 1.0f); return true; }
        // (the other keys are taken, so none of them plays the computer keyboard's notes)
        return ! ctrlKey;
    }
    if (k == juce::KeyPress::escapeKey)
    {
        if (selectedCount() > 0) { clearSelection(); say (rysum::tr("Auswahl leer")); return true; }
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
        if (! ctrlKey && (kc == 'J' || kc == 'j')) { ++proc.engine.choreoUiCount; return true; }      // (the figure)
        if (kc == 'B' || kc == 'b')
        {
            // painting: B paints the cell under the cross, Shift+B wipes it off, Ctrl+B the next material
            if (ctrlKey)
            {
                brush = (brush + 1) % kNumSurfMaterials;
                say (rysum::tr ("Pinsel: ") + rysum::trText (juce::String::fromUTF8 (surfMaterial (brush).name)));
            }
            else paintHere (mods.isShiftDown());
            return true;
        }
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
    status = on ? rysum::tr("Licht aus: die Pfeifen leuchten") : rysum::tr("Licht an");
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
    if (! on) refreshSpectator();
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
    const bool guestHand = isSpectator() && handsMay (1) && e.mods.isLeftButtonDown() && ! e.mods.isCtrlDown() && ! e.mods.isCommandDown();
    if (isSpectator() && ! guestHand)
    {
        // listening: a drag looks around (with the player's leave the left button also knocks)
        if (immersive || e.mods.isMiddleButtonDown()) return;
        looking = true;
        lastMouse = downPos = e.position;
        dragged = 0.0f;
        e.source.enableUnboundedMouseMovement (true);
        return;
    }
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
    if (isSpectator() && ! (handsMay (1) && ! e.mods.isPopupMenu() && ! e.mods.isMiddleButtonDown()))
    {
        if (looking) e.source.enableUnboundedMouseMovement (false);
        looking = false;
        return;
    }
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
        say (juce::String (added) + rysum::tr(" Pfeifen dazu, ") + juce::String (selectedCount()) + rysum::tr(" gew\xc3\xa4hlt  \xc2\xb7  1 bis 5: aufstellen, rechte Maustaste: Magnet, Esc: Auswahl leeren"), 3500.0);
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
    if (isSpectator())
    {
        // listening: the stop is the player's - ask (with leave to carry) and take it once it hovers
        if (m == (int) Motion::Static && handsMay (2))
        {
            proc.concert.requestHover (stop);
            if (tell) say (rysum::tr("Register ") + juce::String (stop + 1) + rysum::tr(" schwebt gleich \xe2\x80\x93 dann noch einmal greifen"), 3000.0);
        }
        return false;
    }
    if (m != (int) Motion::Static)
    {
        if (tell) say (rysum::tr("Dieses Register bewegt sich anders (") + rysum::tr(motionNames()[m]) + rysum::tr("): zum Greifen die Bewegung \xe2\x80\x9eSchweben\xe2\x80\x9c w\xc3\xa4hlen (Raum-Seite)"), 4000.0);
        return false;
    }
    if (auto* prm = proc.apvts.getParameter (RysumProcessor::motionParamId (stop)))
    {
        prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (int) Motion::Hover));
        if (tell) say (rysum::tr("Register ") + juce::String (stop + 1) + rysum::tr(": Bewegung jetzt \xe2\x80\x9eSchweben\xe2\x80\x9c"));
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
        say (juce::String (woken) + rysum::tr(" Register schweben jetzt (Bewegung \xe2\x80\x9eSchweben\xe2\x80\x9c) \xe2\x80\x93 die Druckwelle treibt sie fort, sie kehren zur\xc3\xbc" "ck"), 4000.0);
    }
    else
    {
        proc.engine.playerAct (OrganEngine::Act::Shock, -1, -1, {}, strength);
        if (bodies == 0) say (rysum::tr("Druckwelle: kein Register, dessen Pfeifen frei im Saal sind (Bewegung Schweben, Flug, Fall, Schwarm, Str\xc3\xb6mung)"), 4000.0);
    }
}

void WalkView::callHome()
{
    pushUndo();
    proc.engine.playerAct (OrganEngine::Act::Home);
    proc.engine.pipeLenReset = true;      // (and every broken or grown pipe is whole again)
    say (rysum::tr("Alle Pfeifen fliegen an ihren Platz zur\xc3\xbc" "ck"));
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
    say (rysum::tr("Alles zur\xc3\xbc" "ckgesetzt"));
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
        if (aimStop < 0) { say (rysum::tr("Keine Pfeife im Blick (das Kreuz auf eine Pfeife richten, h\xc3\xb6" "chstens 9 m entfernt)")); return; }
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
    if (frozen) say (rysum::tr("Angehalten \xe2\x80\x93 solange F gehalten wird"), 250.0);
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
    if (paused) say (rysum::tr("Pause \xe2\x80\x93 P: weiter"), 250.0);
    else if (slow) say (rysum::tr("Zeitlupe \xe2\x80\x93 solange Z gehalten wird"), 250.0);
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
    const bool sp = isSpectator();
    if (heldStop >= 0 && ! sp) { holdDist = juce::jlimit (0.2f, 4.0f, holdDist * std::exp (w.deltaY * 1.2f)); return; }
    if (magnet != 0 && ! sp) { magnetDist = juce::jlimit (0.8f, 14.0f, magnetDist * std::exp (w.deltaY * 1.2f)); return; }
    {
        const float dw = w.deltaY != 0.0f ? w.deltaY : w.deltaX;
        const auto mods = juce::ModifierKeys::getCurrentModifiersRealtime();
        if (mods.isCtrlDown() || mods.isCommandDown()) { if (! sp) scaleRoom (std::exp (dw * 0.6f)); return; }
        if (mods.isShiftDown())
        {
            auto cs = controls();
            cs.walk = juce::jlimit (0.4f, 8.0f, cs.walk * std::exp (dw * 0.8f));
            setControls (cs);
            say (rysum::tr("Gehtempo \xc3\x97 ") + juce::String (cs.walk, 2));
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
        status = rysum::tr("Konnte das Modell nicht schreiben: ") + f.getFullPathName();
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
    chooser = std::make_unique<juce::FileChooser> (rysum::tr("Raummodell (OBJ)"), proc.walkModelFile.existsAsFile() ? proc.walkModelFile.getParentDirectory() : juce::File(), "*.obj");
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
        status = m->name + ": " + juce::String ((int) m->tris.size()) + rysum::tr(" Dreiecke, ") + juce::String (m->size.x, 1) + U (" \xc3\x97 ")
               + juce::String (m->size.y, 1) + U (" \xc3\x97 ") + juce::String (m->size.z, 1) + " m" + (m->message.isNotEmpty() ? " - " + m->message : juce::String());
    if (m != nullptr && err.isEmpty())
    {
        // the Saal's shoebox lies underneath (early reflections, the outer limit): say so when the sizes differ
        const auto r = roomNow();
        if (std::abs (m->size.x - r.W()) > 1.0f || std::abs (m->size.y - r.L()) > 1.0f || std::abs (m->size.z - r.H()) > 1.0f)
            status << rysum::tr("  \xc2\xb7  der Saal ist ") << juce::String (r.W(), 1) << U (" \xc3\x97 ") << juce::String (r.L(), 1) << U (" \xc3\x97 ")
                   << juce::String (r.H(), 1) << rysum::tr(" m: \xe2\x80\x9eMa\xc3\x9f" "e \xc3\xbc" "bernehmen\xe2\x80\x9c passt ihn an");
    }
    statusUntil = juce::Time::getMillisecondCounterHiRes() + 5000;
    updateButtons();
    repaint();
}

void WalkView::buildRoomTris (const RoomParams& r, std::vector<RTri>& out, const DoorCut& door) const
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
        const float plinth = juce::jmin (0.9f, H * 0.2f);
        // a stretch of the wall from t0 to t1 (0 .. 1 along it), from the height zFrom up (the plinth below 'plinth')
        auto stretch = [&] (float ta, float tb, float zFrom)
        {
            const int nSeg = juce::jmax (1, (int) std::ceil (len * (tb - ta) / 2.5f));
            for (int i = 0; i < nSeg; ++i)
            {
                const float t0 = ta + (tb - ta) * (float) i / (float) nSeg, t1 = ta + (tb - ta) * (float) (i + 1) / (float) nSeg;
                const V3 p0 { ax + (bx - ax) * t0, ay + (by - ay) * t0, 0 }, p1 { ax + (bx - ax) * t1, ay + (by - ay) * t1, 0 };
                const float u0 = along + len * t0, u1 = along + len * t1;
                if (zFrom < plinth)
                    quad (vt (p0, n, u0, 0), vt (p1, n, u1, 0), vt (p1 + V3 { 0, 0, plinth }, n, u1, plinth), vt (p0 + V3 { 0, 0, plinth }, n, u0, plinth), slot, 0.55f);
                const float zb = juce::jmax (plinth, zFrom);
                const int bands = juce::jmax (1, (int) std::ceil ((H - zb) / 3.0f));
                for (int b = 0; b < bands; ++b)
                {
                    const float z0 = zb + (H - zb) * (float) b / (float) bands, z1 = zb + (H - zb) * (float) (b + 1) / (float) bands;
                    quad (vt (p0 + V3 { 0, 0, z0 }, n, u0, z0), vt (p1 + V3 { 0, 0, z0 }, n, u1, z0),
                          vt (p1 + V3 { 0, 0, z1 }, n, u1, z1), vt (p0 + V3 { 0, 0, z1 }, n, u0, z1), slot, 1.0f - 0.06f * (float) b / (float) bands);
                }
            }
        };
        if (index == door.wall && door.height < H - 0.05f && 2.0f * door.halfW < len - 0.2f)
        {
            // the door: an opening in the middle (where the engine hears it), the lintel above it
            const float ta = 0.5f - door.halfW / len, tb = 0.5f + door.halfW / len;
            stretch (0.0f, ta, 0.0f);
            stretch (tb, 1.0f, 0.0f);
            stretch (ta, tb, door.height);
        }
        else stretch (0.0f, 1.0f, 0.0f);
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

void WalkView::addDoorView (const RoomParams& r, const DoorCut& door, float open, const RoomParams& next, float dim)
{
    const float W = r.W(), L = r.L();
    const bool box = r.box() || r.plan.n < 3;
    float ax, ay, bx, by;
    if (box)
    {
        const float c[4][4] { { 0, 0, W, 0 }, { W, 0, W, L }, { W, L, 0, L }, { 0, L, 0, 0 } };
        const int w = ((door.wall % 4) + 4) % 4;
        ax = c[w][0]; ay = c[w][1]; bx = c[w][2]; by = c[w][3];
    }
    else
    {
        const int i = ((door.wall % r.plan.n) + r.plan.n) % r.plan.n, j = (i + 1) % r.plan.n;
        ax = r.plan.x[(size_t) i] * W; ay = r.plan.y[(size_t) i] * L; bx = r.plan.x[(size_t) j] * W; by = r.plan.y[(size_t) j] * L;
    }
    const float len = std::hypot (bx - ax, by - ay);
    if (len < 2.0f * door.halfW + 0.2f) return;
    const V3 t { (bx - ax) / len, (by - ay) / len, 0.0f }, in { -t.y, t.x, 0.0f }, out = in * -1.0f;
    const V3 mid { 0.5f * (ax + bx), 0.5f * (ay + by), 0.0f };
    auto vt = [] (V3 p, V3 n, float u, float v) { Raster::Vert q; q.p = p; q.n = n; q.u = u; q.v = v; return q; };
    auto quad = [&] (V3 a, V3 b, V3 c, V3 d, V3 n, int slot, float tone, bool level)
    {
        // (texture coordinates from the world, as the Saal's own surfaces have them)
        auto uv = [&] (V3 p) { return level ? std::make_pair (p.x, p.y) : std::make_pair (dot (p, t), p.z); };
        const auto ua = uv (a), ub = uv (b), uc = uv (c), ud = uv (d);
        rast.triangle (vt (a, n, ua.first, ua.second), vt (b, n, ub.first, ub.second), vt (c, n, uc.first, uc.second), slot, tone);
        rast.triangle (vt (a, n, ua.first, ua.second), vt (c, n, uc.first, uc.second), vt (d, n, ud.first, ud.second), slot, tone);
    };
    // the Saal behind: as wide as it is, as deep as it is long, its floor at ours; dimmer, as it is lit less
    {
        const float hw = juce::jlimit (door.halfW + 0.5f, 40.0f, 0.5f * next.W()), D = juce::jlimit (2.0f, 80.0f, next.L()),
                    H2 = juce::jlimit (door.height + 0.2f, 40.0f, next.H());
        const float tone = 0.15f + 0.85f * dim;
        const V3 p0 = mid - t * hw, p1 = mid + t * hw, q0 = p0 + out * D, q1 = p1 + out * D, up { 0, 0, H2 };
        quad (p0, p1, q1, q0, { 0, 0, 1 }, kSlotFloor, tone, true);                       // floor
        quad (p0 + up, q0 + up, q1 + up, p1 + up, { 0, 0, -1 }, kSlotCeiling, tone * 0.85f, true);
        quad (q0, q1, q1 + up, q0 + up, in, kSlotWall, tone * 0.9f, false);              // the far wall
        quad (p0, q0, q0 + up, p0 + up, t, kSlotWall, tone, false);                       // the sides
        quad (q1, p1, p1 + up, q1 + up, t * -1.0f, kSlotWall, tone, false);
        // its own wall around the door, seen from behind ours (where our wall is thinner than its)
    }
    // the leaf: hinged at one side of the opening, swung into this Saal as far as the door is open
    {
        const float ang = juce::jlimit (0.0f, 1.0f, open) * 0.5f * juce::MathConstants<float>::pi * 0.95f;
        const V3 hinge = mid - t * door.halfW;
        const V3 dir = t * std::cos (ang) + in * std::sin (ang), side { -dir.y, dir.x, 0.0f };
        const float lw = 2.0f * door.halfW, th = 0.05f;
        const V3 up { 0, 0, door.height - 0.02f };
        const V3 a = hinge + side * (0.5f * th), b = a + dir * lw, a2 = hinge - side * (0.5f * th), b2 = a2 + dir * lw;
        quad (a, b, b + up, a + up, side, kSlotWood, 0.8f, false);
        quad (b2, a2, a2 + up, b2 + up, side * -1.0f, kSlotWood, 0.8f, false);
        quad (b, b2, b2 + up, b + up, dir, kSlotWood, 0.6f, false);                    // its edge
        quad (a + up, b + up, b2 + up, a2 + up, { 0, 0, 1 }, kSlotWood, 0.6f, true);
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
    if (surf == kSurfFloor) n = rysum::tr("Boden");
    else if (surf == kSurfCeiling) n = rysum::tr("Decke");
    else if (r.box() || r.plan.n < 3) n = juce::StringArray { U ("R\xc3\xbc" "ckwand"), rysum::tr("rechte Wand"), rysum::tr("Vorderwand"), rysum::tr("linke Wand") }[juce::jlimit (0, 3, surf - kSurfWall0)];
    else n = rysum::tr("Wand ") + juce::String (surf - kSurfWall0 + 1);
    const int m = r.plan.surf[(size_t) juce::jlimit (0, kMaxSurf - 1, surf)];
    return n + ": " + (m < 0 ? rysum::tr("wie Regler") : juce::String (surfMaterial (m).name));
}

/** M: the surface under the cross gets the next material (after the last: as the sliders say again) */
/** where the view meets a surface of the Saal, and where on it: u along a wall from its first corner (x on the floor
    and ceiling), v up (y) */
bool WalkView::surfaceHit (const RoomParams& r, V3 eye, int& surf, float& u, float& v) const
{
    const float ya = juce::degreesToRadians (yaw), pa = juce::degreesToRadians (juce::jlimit (-89.0f, 89.0f, pitch));
    const V3 d { -std::sin (ya) * std::cos (pa), std::cos (ya) * std::cos (pa), std::sin (pa) };
    float best = 1.0e30f;
    surf = -1;
    if (d.z < -1.0e-5f) { best = -eye.z / d.z; surf = kSurfFloor; }
    else if (d.z > 1.0e-5f) { best = (r.H() - eye.z) / d.z; surf = kSurfCeiling; }
    if (surf >= 0) { u = eye.x + d.x * best; v = eye.y + d.y * best; }
    auto wall = [&] (float ax, float ay, float bx, float by, int index)
    {
        const float ex = bx - ax, ey = by - ay, len = std::sqrt (ex * ex + ey * ey);
        if (len < 1.0e-3f) return;
        const float nx = -ey / len, ny = ex / len;
        const float dn = nx * d.x + ny * d.y;
        if (dn > -1.0e-6f) return;
        const float t = (nx * (eye.x - ax) + ny * (eye.y - ay)) / -dn;
        if (t <= 0.0f || t >= best) return;
        const float a = ((eye.x + d.x * t - ax) * ex + (eye.y + d.y * t - ay) * ey) / (len * len);
        if (a < 0.0f || a > 1.0f) return;
        best = t; surf = kSurfWall0 + index; u = a * len; v = eye.z + d.z * t;
    };
    const float W = r.W(), L = r.L();
    if (r.box() || r.plan.n < 3) { wall (0, 0, W, 0, 0); wall (W, 0, W, L, 1); wall (W, L, 0, L, 2); wall (0, L, 0, 0, 3); }
    else
        for (int i = 0; i < r.plan.n; ++i)
        {
            const int j = (i + 1) % r.plan.n;
            wall (r.plan.x[(size_t) i] * W, r.plan.y[(size_t) i] * L, r.plan.x[(size_t) j] * W, r.plan.y[(size_t) j] * L, i);
        }
    return surf >= 0;
}

void WalkView::paintHere (bool wipe)
{
    if (proc.getWalkMesh() != nullptr)
    {
        say (rysum::tr ("Malen: nur auf den Fl\xc3\xa4" "chen des Saals (ohne Modell)"));
        return;
    }
    const auto r = roomNow();
    int surf; float u, v;
    if (! surfaceHit (r, eyeNow(), surf, u, v)) { say (rysum::tr ("Keine Fl\xc3\xa4" "che im Blick")); return; }
    proc.paintCell (0, surf, (int) std::floor (u), (int) std::floor (v), wipe ? -1 : brush);
    say ((wipe ? rysum::tr ("Abgewischt") : rysum::tr ("Gemalt: ") + rysum::trText (juce::String::fromUTF8 (surfMaterial (brush).name)))
         + juce::String::fromUTF8 (" \xc2\xb7 ") + surfaceName (r, surf));
    repaint();
}

/** shadows: the pipes' axes cast onto the floor and the walls - by the sun of the time of day (along its direction)
    and by the strongest lamps (the lanterns, the sounding pipes in the dark or with "Pfeifenlicht": from their place).
    Each a darker patch of the surface it falls on; no more than 1500 of them. */
void WalkView::castShadows (const RoomParams& r, bool dark)
{
    shadowQuads = 0;
    struct Light { V3 at; bool point; float strength, reach; };
    std::vector<Light> lights;
    if (shadowAmt > 0.0f && ! dark) lights.push_back ({ rast.lightDirNow(), false, shadowAmt, 1.0e9f });
    {
        std::vector<const Raster::Lamp*> strong;
        for (const auto& l : lamps) if (l.power > 0.08f) strong.push_back (&l);
        std::sort (strong.begin(), strong.end(), [] (const Raster::Lamp* a, const Raster::Lamp* b) { return a->power > b->power; });
        for (size_t i = 0; i < strong.size() && i < 4; ++i)
            lights.push_back ({ strong[i]->at, true, juce::jlimit (0.0f, 0.6f, (dark ? 0.9f : 0.5f) * strong[i]->power), 2.5f * strong[i]->reach });
    }
    if (lights.empty() || casters.empty()) return;
    const float W = r.W(), L = r.L(), H = r.H();
    const bool box = r.box() || r.plan.n < 3;
    // the walls: their ends and how far the texture has run before them
    struct Wall { float ax, ay, bx, by, len, along; V3 t, n; };
    std::vector<Wall> walls;
    {
        float along = 0.0f;
        const int nw = box ? 4 : r.plan.n;
        for (int w = 0; w < nw; ++w)
        {
            float ax, ay, bx, by;
            if (box)
            {
                const float c[4][4] { { 0, 0, W, 0 }, { W, 0, W, L }, { W, L, 0, L }, { 0, L, 0, 0 } };
                ax = c[w][0]; ay = c[w][1]; bx = c[w][2]; by = c[w][3];
            }
            else
            {
                const int j = (w + 1) % r.plan.n;
                ax = r.plan.x[(size_t) w] * W; ay = r.plan.y[(size_t) w] * L; bx = r.plan.x[(size_t) j] * W; by = r.plan.y[(size_t) j] * L;
            }
            const float len = std::hypot (bx - ax, by - ay);
            const V3 t = len > 1.0e-4f ? V3 { (bx - ax) / len, (by - ay) / len, 0.0f } : V3 { 1, 0, 0 };
            walls.push_back ({ ax, ay, bx, by, len, along, t, { -t.y, t.x, 0.0f } });
            along += len;
        }
    }
    // the first surface a ray from p along d meets beyond p: -1 floor, else the wall's index; the point
    auto hit = [&] (V3 p, V3 d, int& surf, V3& q)
    {
        float best = 1.0e30f;
        surf = -2;
        if (d.z < -1.0e-5f) { best = -p.z / d.z; surf = -1; }
        for (int w = 0; w < (int) walls.size(); ++w)
        {
            const auto& wl = walls[(size_t) w];
            const float dn = dot (wl.n, d);
            if (dn > -1.0e-6f) continue;
            const float t = (dot (wl.n, p) - (wl.n.x * wl.ax + wl.n.y * wl.ay)) / -dn;
            if (t <= 0.0f || t >= best) continue;
            const V3 x = p + d * t;
            const float u = (x.x - wl.ax) * wl.t.x + (x.y - wl.ay) * wl.t.y;
            if (u < 0.0f || u > wl.len || x.z < 0.0f || x.z > H) continue;
            best = t; surf = w;
        }
        if (surf == -2) return false;
        q = p + d * best;
        return true;
    };
    auto vt = [] (V3 p, V3 n, float u, float v) { Raster::Vert q; q.p = p; q.n = n; q.u = u; q.v = v; return q; };
    for (const auto& li : lights)
        for (const auto& c : casters)
        {
            if (shadowQuads >= 1500) return;
            const V3 top { c.bottom.x, c.bottom.y, c.bottom.z + c.len }, mid { c.bottom.x, c.bottom.y, c.bottom.z + 0.5f * c.len };
            V3 dirB, dirT;
            float spread = 1.0f;          // (a near lamp throws a wider shadow)
            if (li.point)
            {
                const V3 e = mid - li.at;
                const float dl = std::sqrt (dot (e, e));
                if (dl < 0.35f || dl > li.reach) continue;           // (its own pipe, or too far to cast)
                dirB = normalised (c.bottom - li.at); dirT = normalised (top - li.at);
                spread = 1.0f;
            }
            else dirB = dirT = li.at * -1.0f;
            int sb, st; V3 qb, qt;
            if (! hit (c.bottom, dirB, sb, qb) || ! hit (top, dirT, st, qt) || sb != st) continue;
            if (li.point)
            {
                const float d0 = std::sqrt (dot (c.bottom - li.at, c.bottom - li.at)), d1 = std::sqrt (dot (qb - li.at, qb - li.at));
                spread = juce::jlimit (1.0f, 6.0f, d1 / juce::jmax (0.05f, d0));
            }
            const float tone = 1.0f - li.strength;
            if (sb == -1)
            {
                // on the floor, a little above it
                V3 sd { -(qt.y - qb.y), qt.x - qb.x, 0.0f };
                const float sl = std::sqrt (sd.x * sd.x + sd.y * sd.y);
                if (sl > 40.0f) continue;
                sd = sl > 1.0e-4f ? sd * (c.rad * spread / sl) : V3 { c.rad * spread, 0, 0 };
                const V3 nz { 0, 0, 1 }, lift { 0, 0, 0.004f };
                auto fv = [&] (V3 p) { p = p + lift; return vt (p, nz, p.x, p.y); };
                rast.triangle (fv (qb - sd), fv (qb + sd), fv (qt + sd), kSlotFloor, tone);
                rast.triangle (fv (qb - sd), fv (qt + sd), fv (qt - sd), kSlotFloor, tone);
            }
            else
            {
                // on a wall, a little in front of it (not over the door's opening)
                const auto& wl = walls[(size_t) sb];
                auto uOf = [&] (V3 p) { return (p.x - wl.ax) * wl.t.x + (p.y - wl.ay) * wl.t.y; };
                const float ub = uOf (qb), ut = uOf (qt);
                if (sb == cutNow.wall)
                {
                    const float d0 = 0.5f * wl.len - cutNow.halfW, d1 = 0.5f * wl.len + cutNow.halfW;
                    if (juce::jmax (ub, ut) > d0 && juce::jmin (ub, ut) < d1 && juce::jmin (qb.z, qt.z) < cutNow.height) continue;
                }
                // (the axis on the wall, widened across it)
                const float du = ut - ub, dz = qt.z - qb.z, sl = std::sqrt (du * du + dz * dz);
                if (sl > 40.0f) continue;
                const float w = c.rad * spread;
                const float pu = sl > 1.0e-4f ? -dz / sl * w : w, pz = sl > 1.0e-4f ? du / sl * w : 0.0f;
                const V3 o = V3 { wl.ax, wl.ay, 0.0f } + wl.n * 0.012f;
                auto at = [&] (float u, float z)
                {
                    u = juce::jlimit (0.0f, wl.len, u); z = juce::jlimit (0.0f, H, z);
                    return vt (o + wl.t * u + V3 { 0, 0, z }, wl.n, wl.along + u, z);
                };
                const int slot = kSlotWall0 + juce::jlimit (0, kMaxPlanVerts - 1, sb);
                rast.triangle (at (ub - pu, qb.z - pz), at (ub + pu, qb.z + pz), at (ut + pu, qt.z + pz), slot, tone);
                rast.triangle (at (ub - pu, qb.z - pz), at (ut + pu, qt.z + pz), at (ut - pu, qt.z - pz), slot, tone);
            }
            ++shadowQuads;
        }
}

/** the painted cells: each a patch of its material, a little in front of its surface */
void WalkView::addPaint (const RoomParams& r)
{
    const auto& cells = proc.getPaint (0);
    if (cells.empty()) return;
    const float W = r.W(), L = r.L(), H = r.H();
    const bool box = r.box() || r.plan.n < 3;
    auto vt = [] (V3 p, V3 n, float u, float v) { Raster::Vert q; q.p = p; q.n = n; q.u = u; q.v = v; return q; };
    // (the walls' lengths before this one: the texture runs on along the walls, as the Saal's own)
    std::array<float, kMaxPlanVerts + 1> along {};
    auto wallEnds = [&] (int w, float& ax, float& ay, float& bx, float& by)
    {
        if (box)
        {
            const float c[4][4] { { 0, 0, W, 0 }, { W, 0, W, L }, { W, L, 0, L }, { 0, L, 0, 0 } };
            const int k = ((w % 4) + 4) % 4;
            ax = c[k][0]; ay = c[k][1]; bx = c[k][2]; by = c[k][3];
            return w < 4;
        }
        if (w >= r.plan.n) return false;
        const int j = (w + 1) % r.plan.n;
        ax = r.plan.x[(size_t) w] * W; ay = r.plan.y[(size_t) w] * L; bx = r.plan.x[(size_t) j] * W; by = r.plan.y[(size_t) j] * L;
        return true;
    };
    {
        const int nw = box ? 4 : r.plan.n;
        for (int w = 0; w < nw; ++w)
        {
            float ax, ay, bx, by;
            wallEnds (w, ax, ay, bx, by);
            along[(size_t) w + 1] = along[(size_t) w] + std::hypot (bx - ax, by - ay);
        }
    }
    for (const auto& c : cells)
    {
        const int slot = kSlotPaint0 + juce::jlimit (0, kNumSurfMaterials - 1, c.mat);
        if (c.surf < kSurfWall0)
        {
            const bool top = c.surf == kSurfCeiling;
            const float z = top ? H - 0.02f : 0.02f;
            const float x0 = juce::jmax (0.0f, (float) c.iu), x1 = juce::jmin (W, (float) c.iu + 1.0f);
            const float y0 = juce::jmax (0.0f, (float) c.iv), y1 = juce::jmin (L, (float) c.iv + 1.0f);
            if (x1 <= x0 || y1 <= y0) continue;
            const V3 n { 0, 0, top ? -1.0f : 1.0f };
            const auto a = vt ({ x0, y0, z }, n, x0, y0), b = vt ({ x1, y0, z }, n, x1, y0), cc = vt ({ x1, y1, z }, n, x1, y1), d = vt ({ x0, y1, z }, n, x0, y1);
            rast.triangle (a, b, cc, slot, 1.0f);
            rast.triangle (a, cc, d, slot, 1.0f);
            continue;
        }
        const int w = c.surf - kSurfWall0;
        float ax, ay, bx, by;
        if (! wallEnds (w, ax, ay, bx, by)) continue;
        const float len = std::hypot (bx - ax, by - ay);
        if (len < 1.0e-3f) continue;
        const V3 t { (bx - ax) / len, (by - ay) / len, 0.0f }, n { -t.y, t.x, 0.0f };
        const float u0 = juce::jmax (0.0f, (float) c.iu), u1 = juce::jmin (len, (float) c.iu + 1.0f);
        const float v0 = juce::jmax (0.0f, (float) c.iv), v1 = juce::jmin (H, (float) c.iv + 1.0f);
        if (u1 <= u0 || v1 <= v0) continue;
        const V3 o = V3 { ax, ay, 0.0f } + n * 0.02f;
        const float ua = along[(size_t) w];
        auto at = [&] (float uu, float vv) { return vt (o + t * uu + V3 { 0, 0, vv }, n, ua + uu, vv); };
        rast.triangle (at (u0, v0), at (u1, v0), at (u1, v1), slot, 1.0f);
        rast.triangle (at (u0, v0), at (u1, v1), at (u0, v1), slot, 1.0f);
    }
}

void WalkView::cycleMaterial()
{
    if (aimSurf < 0)
    {
        status = proc.getWalkMesh() != nullptr ? rysum::tr("Material: nur f\xc3\xbcr die Fl\xc3\xa4" "chen des Saals (ohne Modell)") : rysum::tr("Keine Fl\xc3\xa4" "che im Blick");
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

void WalkView::addFigure (V3 feet, float height, float yawDeg)
{
    // someone else in the Saal (Konzert): a plain turned figure - a body narrowing to the feet, a head,
    // a dark band where the eyes are, so one sees where it looks
    const float ya = juce::degreesToRadians (yawDeg);
    const V3 fwd { -std::sin (ya), std::cos (ya), 0.0f };
    const int segs = 12;
    const float zs = feet.z + 0.80f * height, zh = feet.z + height - 0.12f, rh = 0.12f;
    auto ringAt = [&] (float z, float rad, float v, Raster::Vert* out, float nz)
    {
        for (int i = 0; i <= segs; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / (float) segs;
            const float ca = std::cos (a), sa = std::sin (a);
            out[i].p = { feet.x + ca * rad, feet.y + sa * rad, z };
            out[i].n = normalised ({ ca, sa, nz });
            out[i].u = (float) i / (float) segs; out[i].v = v;
        }
    };
    Raster::Vert body[4][segs + 1];
    ringAt (feet.z, 0.12f, 0.0f, body[0], -0.1f);
    ringAt (feet.z + 0.45f * height, 0.17f, 0.4f, body[1], 0.0f);
    ringAt (zs, 0.22f, 0.8f, body[2], 0.25f);
    ringAt (zs + 0.06f, 0.07f, 0.85f, body[3], 0.9f);      // the shoulders close in towards the neck
    const float tone = 0.55f, em = 0.08f;
    for (int r = 0; r < 3; ++r)
        for (int i = 0; i < segs; ++i)
        {
            rast.triangle (body[r][i], body[r][i + 1], body[r + 1][i + 1], kSlotWood, tone, em);
            rast.triangle (body[r][i], body[r + 1][i + 1], body[r + 1][i], kSlotWood, tone, em);
        }
    // the head: a sphere of a few rings
    const int lat = 6;
    Raster::Vert head[lat + 1][segs + 1];
    for (int j = 0; j <= lat; ++j)
    {
        const float th = juce::MathConstants<float>::pi * (float) j / (float) lat;
        const float rz = std::cos (th), rr = std::sin (th);
        for (int i = 0; i <= segs; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / (float) segs;
            const V3 n { std::cos (a) * rr, std::sin (a) * rr, rz };
            head[j][i].p = { feet.x + n.x * rh, feet.y + n.y * rh, zh + n.z * rh };
            head[j][i].n = n;
            head[j][i].u = (float) i / (float) segs; head[j][i].v = (float) j / (float) lat;
        }
    }
    for (int j = 0; j < lat; ++j)
        for (int i = 0; i < segs; ++i)
        {
            // (the band of the eyes: the faces of the middle ring turned the way it looks)
            const float a = juce::MathConstants<float>::twoPi * ((float) i + 0.5f) / (float) segs;
            const bool eyes = (j == 2 || j == 3) && std::cos (a) * fwd.x + std::sin (a) * fwd.y > 0.55f;
            const int slot = eyes ? kSlotDark : kSlotWood;
            rast.triangle (head[j][i], head[j][i + 1], head[j + 1][i + 1], slot, tone, eyes ? 0.0f : em);
            rast.triangle (head[j][i], head[j + 1][i + 1], head[j + 1][i], slot, tone, eyes ? 0.0f : em);
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

void WalkView::addVoxPipe (V3 bottom, float len, float rad, float faceAngle, float glowAmt, const float* rings)
{
    // a smooth glass pipe without foot or mouth; around it, at eight heights, a flat ring each - as wide as
    // that band of its harmonics is strong (16' lowest, the highest partials at the top), lit while it sings
    const float depth = rast.viewDepth (bottom + V3 { 0, 0, 0.5f * len });
    if (depth < -len) return;
    const float px = rad / juce::jmax (0.1f, depth) * 0.5f * (float) rast.width();
    const int segs = px < 1.5f ? 6 : (px < 5.0f ? 8 : (px < 14.0f ? 12 : 18));
    const float em = 0.05f + 0.45f * glowAmt;     // glass holds a little light of its own (the rings carry the rest)
    Raster::Vert ring[2][20];
    for (int i = 0; i <= segs; ++i)
    {
        const float a = faceAngle + juce::MathConstants<float>::twoPi * (float) i / (float) segs;
        const float ca = std::cos (a), sa = std::sin (a);
        const float u = (float) i / (float) segs;
        auto& r0 = ring[0][i]; r0.p = bottom + V3 { ca * rad, sa * rad, 0.0f }; r0.n = { ca, sa, 0.0f }; r0.u = u; r0.v = 0.0f;
        auto& r1 = ring[1][i]; r1.p = bottom + V3 { ca * rad, sa * rad, len }; r1.n = { ca, sa, 0.0f }; r1.u = u; r1.v = 1.0f;
    }
    for (int i = 0; i < segs; ++i)
    {
        rast.triangle (ring[0][i], ring[0][i + 1], ring[1][i + 1], kSlotGlass, 0.95f, em);
        rast.triangle (ring[0][i], ring[1][i + 1], ring[1][i], kSlotGlass, 0.95f, em);
    }
    // the closed, rounded-off top: a flat cap (glass is not open)
    {
        Raster::Vert c; c.p = bottom + V3 { 0, 0, len }; c.n = { 0, 0, 1 };
        for (int i = 0; i < segs; ++i)
        {
            Raster::Vert a = ring[1][i], b = ring[1][i + 1];
            a.n = b.n = c.n;
            rast.triangle (a, b, c, kSlotGlass, 0.95f, em);
        }
    }
    // the rings of the harmonics: a collar each - the stronger that band, the wider and brighter it is
    // (a collar, not a flat ring, so it is seen from the side as well)
    for (int j = 0; j < 8; ++j)
    {
        const float amt = juce::jlimit (0.0f, 1.0f, rings[j]);
        if (amt < 0.04f) continue;
        const float z0 = len * (0.1f + 0.105f * (float) j), h = juce::jmax (0.02f, 0.045f * len);
        const float rOut = rad * (1.15f + 1.7f * amt);
        const float rem = (0.08f + 0.95f * glowAmt) * amt;
        auto vtx = [&] (float a, float rr, float z, V3 n) { Raster::Vert q; q.p = bottom + V3 { std::cos (a) * rr, std::sin (a) * rr, z }; q.n = n; q.u = a; q.v = z; return q; };
        for (int i = 0; i < segs; ++i)
        {
            const float a0 = faceAngle + juce::MathConstants<float>::twoPi * (float) i / (float) segs;
            const float a1 = faceAngle + juce::MathConstants<float>::twoPi * (float) (i + 1) / (float) segs;
            const V3 n0 { std::cos (a0), std::sin (a0), 0.0f }, n1 { std::cos (a1), std::sin (a1), 0.0f };
            // the outside
            const auto o00 = vtx (a0, rOut, z0, n0), o10 = vtx (a1, rOut, z0, n1), o01 = vtx (a0, rOut, z0 + h, n0), o11 = vtx (a1, rOut, z0 + h, n1);
            rast.triangle (o00, o10, o11, kSlotGlass, 1.5f, rem);
            rast.triangle (o00, o11, o01, kSlotGlass, 1.5f, rem);
            // its top and its underside (seen from above / below)
            for (int side = 0; side < 2; ++side)
            {
                const float z = side == 0 ? z0 + h : z0;
                const V3 n { 0, 0, side == 0 ? 1.0f : -1.0f };
                const auto p0 = vtx (a0, rad, z, n), p1 = vtx (a1, rad, z, n), p2 = vtx (a1, rOut, z, n), p3 = vtx (a0, rOut, z, n);
                if (side == 0) { rast.triangle (p0, p1, p2, kSlotGlass, 1.5f, rem); rast.triangle (p0, p2, p3, kSlotGlass, 1.5f, rem); }
                else           { rast.triangle (p0, p2, p1, kSlotGlass, 1.5f, rem); rast.triangle (p0, p3, p2, kSlotGlass, 1.5f, rem); }
            }
        }
    }
}

void WalkView::addVoxHumana (const RoomParams& r, V3 homeP, bool dark)
{
    auto raw = [this] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
    if (raw ("voxOn") < 0.5f)
    {
        for (auto& v : voxShown) v.glow = 0.0f;
        return;
    }
    // every pipe stands where its sound is (each is a source of its own; with a motion they move), as heard
    // - in a shaped Saal inside the walls, like the pipes of the other stops
    const Pos centre { raw ("voxAz"), raw ("voxEl"), raw ("voxDist") };
    const bool live = proc.engine.voxShowDist[0].load() > 0.0f;
    const float width = raw ("voxWidth");
    auto pointOf = [&] (Pos pos)
    {
        if (pipesInRoom) { const auto q = roomPoint (r, pos); return V3 { q.x, q.y, q.z }; }
        const float a = juce::degreesToRadians (pos.az), e = juce::degreesToRadians (pos.el);
        return V3 { homeP.x - std::sin (a) * std::cos (e) * pos.dist, homeP.y + std::cos (a) * std::cos (e) * pos.dist, homeP.z + std::sin (e) * pos.dist };
    };
    float levelTop = 0.0f;
    for (int i = 0; i < 8; ++i)
    {
        // a pipe shows the louder of its tracked voice (the input) and its played voice (the keys)
        const auto& a = proc.voxVoice (i);
        const auto& b = proc.voxVoice (i + 8);
        const auto& d = a.level.load() >= b.level.load() ? a : b;
        const float level = d.level.load();
        levelTop = juce::jmax (levelTop, level);
        auto& sh = voxShown[(size_t) i];
        if (level > 1.0e-6f)
        {
            // as long as an open 8' pipe of the pitch it sings - it glides with the voice
            const float tgt = juce::jlimit (0.12f, 4.0f, 2.44f * std::exp2 (-(d.key.load() - 36.0f) / 12.0f));
            sh.len += 0.35f * (tgt - sh.len);
            const float g = juce::jlimit (0.15f, 1.0f, std::sqrt (level / voxLevelMax));
            sh.glow = sh.glow < g ? juce::jmin (g, sh.glow + 0.35f) : juce::jmax (g, sh.glow * 0.9f);
            for (size_t j = 0; j < 8; ++j)
                sh.rings[j] += 0.4f * (d.rings[j].load() - sh.rings[j]);
        }
        else
        {
            sh.glow *= 0.9f;
            for (auto& v : sh.rings) v *= 0.95f;
        }
        const float rad = juce::jlimit (0.035f, 0.1f, sh.len * 0.05f);
        const Pos pp = live ? Pos { proc.engine.voxShowAz[(size_t) i].load(), proc.engine.voxShowEl[(size_t) i].load(),
                                    proc.engine.voxShowDist[(size_t) i].load() }
                            : Pos { wrapAz (centre.az + width * ((float) i - 3.5f) / 7.0f), centre.el, centre.dist };
        const V3 at = pointOf (pp);
        const V3 bottom { at.x, at.y, at.z - 0.5f * sh.len };
        const float face = std::atan2 (homeP.y - at.y, homeP.x - at.x);
        addVoxPipe (bottom, sh.len, rad, face, dark ? sh.glow + 0.025f : sh.glow, sh.rings.data());
        if (sh.glow > 0.02f) scene.glows.push_back ({ at, sh.glow, sh.len });
    }
    // (the brightest voice of the last seconds sets what "full" is)
    voxLevelMax = juce::jmax (1.0e-4f, juce::jmax (levelTop, voxLevelMax * 0.995f));
}

WalkView::LookNow WalkView::readLook() const
{
    // (as the modulation and the Verlauf have them right now, else as the controls stand)
    auto v = [this] (const char* id, float def)
    {
        auto* prm = proc.apvts.getParameter (id);
        if (prm == nullptr) return def;
        const int i = proc.getParamIndex (id);
        const float mn = proc.getModulatedNorm (i);
        if (! std::isnan (mn)) return prm->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, mn));
        if (auto* raw = proc.apvts.getRawParameterValue (id)) return raw->load();
        return def;
    };
    LookNow l;
    l.day = v ("lookDay", 0); l.hour = v ("lookHour", 12); l.rays = v ("lookRays", 0); l.rayWidth = v ("lookRayWidth", 1.2f);
    l.dust = v ("lookDust", 0); l.dustStir = v ("lookDustStir", 0.5f); l.pipeLight = v ("lookPipeLight", 0);
    l.pipeHue = v ("lookPipeHue", 0.6f); l.pipeReach = v ("lookPipeReach", 3); l.trails = v ("lookTrails", 0);
    l.trailLen = v ("lookTrailLen", 2); l.fog = v ("lookFog", 0); l.fogFollow = v ("lookFogFollow", 1);
    l.doors = v ("lookDoors", 0); l.doorDim = v ("lookDoorDim", 0.6f); l.walls = v ("lookWalls", 0);
    l.wallGrain = v ("lookWallGrain", 0.5f); l.wear = v ("lookWear", 0); l.shine = v ("lookShine", 0);
    l.ripple = v ("lookRipple", 0); l.sway = v ("lookSway", 0); l.shake = v ("lookShake", 0); l.focus = v ("lookFocus", 0);
    return l;
}

namespace
{
/** the colour each stop's pipes light the room with (Pfeifenlicht), at full colour */
juce::Colour stopLightColour (int s)
{
    static const juce::uint32 c[] { 0xffffc46b,      // Praestant 8': gold
                                    0xffff8a3d,      // Gedackt: amber (wood)
                                    0xffa8ffb0,      // Octave 4': pale green
                                    0xff8fd8ff,      // Octave 2': sky blue
                                    0xffd29bff,      // Sesquialtera: violet
                                    0xffbfe8ff,      // Mixtur: ice
                                    0xffff5a4a,      // Trompete: red
                                    0xffffe0a0 };    // Praestant 16': warm white
    return juce::Colour (c[(size_t) juce::jlimit (0, 7, s)]);
}
/** the time of day: where the light comes from, its colour and strength (hour 0 .. 24) */
void dayLight (float hour, V3& dir, float* tint)
{
    const float pi = juce::MathConstants<float>::pi;
    const float t = (hour - 5.0f) / 16.0f;                            // 0 = sunrise (5:00), 1 = sunset (21:00)
    const float sunEl = std::sin (pi * juce::jlimit (-0.25f, 1.25f, t));      // (below 0: night)
    const float day = juce::jlimit (0.0f, 1.0f, sunEl * 3.0f + 0.2f);         // 0 = night .. 1 = day
    // the sun from the east (right) over the front to the west; at night the moon, high in the south-west
    const float az = pi * juce::jlimit (0.0f, 1.0f, t);
    const float el = juce::jmax (0.08f, sunEl) * 1.1f;
    V3 sun { std::cos (az) * std::cos (el), 0.45f * std::cos (el), std::sin (el) };
    V3 moon { -0.4f, 0.5f, 0.75f };
    dir = normalised (sun * day + moon * (1.0f - day));
    // low sun: gold to orange; high: neutral; night: a dim blue
    const float low = juce::jlimit (0.0f, 1.0f, 1.0f - sunEl * 2.2f) * day;
    float r = 1.02f, g = 1.0f, b = 0.96f;
    r += 0.18f * low; g -= 0.12f * low; b -= 0.36f * low;
    const float bright = 0.32f + 0.68f * day * (0.82f + 0.18f * juce::jmax (0.0f, sunEl));
    tint[0] = (r * day + 0.62f * (1.0f - day)) * bright;
    tint[1] = (g * day + 0.74f * (1.0f - day)) * bright;
    tint[2] = (b * day + 1.18f * (1.0f - day)) * bright;
}
} // namespace

/** the air of the Saal this frame: rays of light (the sun through the wall on its side; in the dark from the sounding
    pipes down to the floor), dust drifting in it (stirred by the Kalkant, loud chords, knocks), the trails the flying
    pipes leave */
void WalkView::updateAir (const RoomParams& r, const EngineParams& params, std::vector<Glow>& glows)
{
    const auto& lk = lookNow;
    const float W = r.W(), L = r.L(), H = r.H();
    const float dt = lookDt;
    juce::ignoreUnused (params);
    // ---- the rays
    beams.clear();
    if (lk.rays > 0.0f)
    {
        const V3 Ls = rast.lightDirNow();
        const bool sun = lk.day > 0.0f && Ls.z > 0.05f && ! proc.walkDark;
        if (sun)
        {
            // the wall on the sun's side: the light comes in high up on it and falls through the room
            const V3 hz = normalised ({ Ls.x, Ls.y, 0.0f });
            const V3 ns[4] { { 0, 1, 0 }, { -1, 0, 0 }, { 0, -1, 0 }, { 1, 0, 0 } };
            int wi = 0; float best = 1.0e9f;
            for (int i = 0; i < 4; ++i) if (dot (ns[i], hz) < best) { best = dot (ns[i], hz); wi = i; }
            float tint[3] { 1, 1, 1 }; V3 dd;
            dayLight (lk.hour, dd, tint);
            const juce::Colour c = juce::Colour::fromFloatRGBA (juce::jlimit (0.0f, 1.0f, 0.95f * tint[0] / juce::jmax (0.3f, tint[1])),
                                                                0.93f, juce::jlimit (0.0f, 1.0f, 0.88f * tint[2] / juce::jmax (0.3f, tint[1])), 1.0f);
            const float strength = lk.rays * juce::jlimit (0.2f, 1.0f, (tint[0] + tint[1] + tint[2]) / 3.0f) * juce::jmin (1.0f, lk.day * 1.5f);
            constexpr int n = 4;
            for (int i = 0; i < n; ++i)
            {
                const float f = ((float) i + 0.5f + 0.3f * (std::sin ((float) i * 2.3f))) / (float) n;
                V3 a;
                switch (wi)
                {
                    case 0:  a = { f * W, 0.05f, 0.82f * H }; break;
                    case 1:  a = { W - 0.05f, f * L, 0.82f * H }; break;
                    case 2:  a = { f * W, L - 0.05f, 0.82f * H }; break;
                    default: a = { 0.05f, f * L, 0.82f * H }; break;
                }
                // down along the light until the floor or a wall
                const V3 d = Ls * -1.0f;
                float t = a.z / juce::jmax (0.05f, -d.z);
                if (d.x > 1.0e-4f) t = juce::jmin (t, (W - a.x) / d.x); else if (d.x < -1.0e-4f) t = juce::jmin (t, -a.x / d.x);
                if (d.y > 1.0e-4f) t = juce::jmin (t, (L - a.y) / d.y); else if (d.y < -1.0e-4f) t = juce::jmin (t, -a.y / d.y);
                beams.push_back ({ a, a + d * juce::jmax (0.5f, t), lk.rayWidth, strength, c });
            }
        }
        else
        {
            // in the dark (or without the sun): the brightest sounding pipes shine down to the floor
            std::vector<const Glow*> lit;
            for (const auto& gw : glows) if (gw.stop >= 0 && gw.amount > 0.1f) lit.push_back (&gw);
            std::sort (lit.begin(), lit.end(), [] (const Glow* a, const Glow* b) { return a->amount > b->amount; });
            for (size_t i = 0; i < lit.size() && i < 6; ++i)
            {
                const auto c = stopLightColour (lit[i]->stop).interpolatedWith (juce::Colours::white, 1.0f - lk.pipeHue);
                beams.push_back ({ lit[i]->at, { lit[i]->at.x, lit[i]->at.y, 0.0f }, lk.rayWidth * 0.6f, lk.rays * lit[i]->amount, c });
            }
        }
    }
    // ---- the dust
    knocksNow.erase (knocksNow.begin(), knocksNow.end() - (ptrdiff_t) juce::jmin ((size_t) 16, knocksNow.size()));
    if (lk.dust > 0.0f)
    {
        juce::Random rnd ((juce::int64) (lookClock * 1000.0));
        if (motes.size() != 260)
        {
            motes.clear();
            for (int i = 0; i < 260; ++i)
                motes.push_back ({ { rnd.nextFloat() * W, rnd.nextFloat() * L, rnd.nextFloat() * H }, {}, rnd.nextFloat() });
        }
        // what stirs it: a tread of the Kalkant, a loud chord setting in, a knock
        float kick = 0.0f;
        const int tread = proc.engine.treadCount.load();
        if (tread != treadSeen) { treadSeen = tread; kick = juce::jmax (kick, 0.6f); }
        const float meter = proc.engine.meterL.load();
        if (meter > 0.08f && meter > meterPrev * 1.8f) kick = juce::jmax (kick, juce::jmin (1.0f, meter * 2.0f));
        meterPrev = meterPrev * 0.9f + meter * 0.1f;
        kick *= lk.dustStir;
        const float t = (float) lookClock;
        for (auto& m : motes)
        {
            // a slow wandering current, its own for every mote
            const V3 flow { 0.03f * std::sin (0.21f * t + 7.0f * m.seed + 0.4f * m.p.y), 0.03f * std::cos (0.17f * t + 5.0f * m.seed + 0.3f * m.p.x),
                            0.012f * std::sin (0.13f * t + 11.0f * m.seed) - 0.004f };
            if (kick > 0.0f)
                m.v = m.v + V3 { (rnd.nextFloat() - 0.5f) * 0.8f, (rnd.nextFloat() - 0.5f) * 0.8f, rnd.nextFloat() * 0.6f } * kick;
            for (const auto& kp : knocksNow)
            {
                const V3 e = m.p - kp;
                const float d2 = dot (e, e);
                if (d2 < 9.0f) m.v = m.v + normalised (e) * (lk.dustStir * 1.2f / (1.0f + 3.0f * d2));
            }
            m.v = m.v * std::exp (-1.5f * dt);
            m.p = m.p + (flow + m.v) * dt;
            // (it stays in the Saal: what leaves comes in again at the other side)
            if (m.p.x < 0) m.p.x += W; if (m.p.x > W) m.p.x -= W;
            if (m.p.y < 0) m.p.y += L; if (m.p.y > L) m.p.y -= L;
            if (m.p.z < 0.05f) { m.p.z = 0.05f; m.v.z = std::abs (m.v.z) * 0.3f; }
            if (m.p.z > H) m.p.z -= H * 0.9f;
            // how bright: the room's light, the rays it floats in, the pipes near it
            float b = proc.walkDark ? 0.03f : 0.14f;
            for (const auto& bm : beams)
            {
                const V3 ab = bm.b - bm.a;
                const float tt = juce::jlimit (0.0f, 1.0f, dot (m.p - bm.a, ab) / juce::jmax (1.0e-4f, dot (ab, ab)));
                const V3 q = bm.a + ab * tt - m.p;
                if (dot (q, q) < 0.25f * bm.width * bm.width) b += 1.1f * bm.strength;
            }
            for (const auto& gw : glows)
            {
                if (gw.stop < 0) continue;
                const V3 e = m.p - gw.at;
                const float d2 = dot (e, e);
                if (d2 < 4.0f) b += gw.amount * (1.0f - std::sqrt (d2) * 0.5f);
            }
            const float amt = lk.dust * juce::jmin (1.0f, b) * (0.6f + 0.4f * std::sin (3.0f * t + 20.0f * m.seed));      // (it glitters)
            if (amt > 0.01f) glows.push_back ({ m.p, amt, 0.0f, -1, 1.0f + 0.8f * m.seed });
        }
    }
    else motes.clear();
    knocksNow.clear();
    // ---- the trails: every pipe flying as a body remembers where it was
    if (lk.trails > 0.0f)
    {
        if (trails.size() != (size_t) (kNumStops * kNumKeys)) trails.assign ((size_t) (kNumStops * kNumKeys), {});
        const double every = juce::jmax (0.02, (double) lk.trailLen / 32.0);
        for (int s = 0; s < kNumStops; ++s)
            for (int k = 0; k < kNumKeys; ++k)
            {
                auto& tr = trails[(size_t) (s * kNumKeys + k)];
                Vec3f b;
                if (! params.stopOn[(size_t) s] || ! proc.engine.bodyAt (s, k, b)) { tr.n = 0; continue; }
                if (tr.n > 0 && lookClock - tr.last < every) continue;
                tr.p[(size_t) tr.head] = { b.x, b.y, b.z };
                tr.t[(size_t) tr.head] = lookClock;
                tr.head = (tr.head + 1) % 32;
                tr.n = juce::jmin (32, tr.n + 1);
                tr.last = lookClock;
            }
    }
    else trails.clear();
}

/** the rays and trails as bands of light in the view's pixels (after the depths are known: hidden ones are left out) */
void WalkView::buildVeils (float isc)
{
    veils.clear();
    const auto& lk = lookNow;
    const float focalPx = rast.focalPerWidth() * (float) rast.width() * isc;
    auto toScreen = [&] (V3 p, juce::Point<float>& s, float& d)
    {
        float sx, sy;
        if (! rast.project (p, sx, sy, d)) return false;
        s = { sx * isc, sy * isc };
        return true;
    };
    // a band from a to b, so wide (m) at each end, coloured ca .. cb
    auto band = [&] (V3 a, V3 b, float wa, float wb, juce::Colour ca, juce::Colour cb)
    {
        // (pulled in front of the eye where it reaches behind it)
        const float da0 = rast.viewDepth (a), db0 = rast.viewDepth (b);
        if (da0 < 0.1f && db0 < 0.1f) return;
        if (da0 < 0.1f) a = a + (b - a) * ((0.1f - da0) / (db0 - da0));
        if (db0 < 0.1f) b = b + (a - b) * ((0.1f - db0) / (da0 - db0));
        juce::Point<float> sa, sb; float da, db;
        if (! toScreen (a, sa, da) || ! toScreen (b, sb, db)) return;
        const auto dir = sb - sa;
        const float len = dir.getDistanceFromOrigin();
        juce::Point<float> nrm = len > 1.0e-3f ? juce::Point<float> (-dir.y / len, dir.x / len) : juce::Point<float> (1.0f, 0.0f);
        const float pa = juce::jlimit (0.6f, 400.0f, 0.5f * wa / da * focalPx), pb = juce::jlimit (0.6f, 400.0f, 0.5f * wb / db * focalPx);
        veils.push_back ({ sa - nrm * pa, sa + nrm * pa, sb - nrm * pb, sb + nrm * pb, ca, cb });
    };
    for (const auto& bm : beams)
    {
        // a core and a wider, fainter halo
        const float s = juce::jlimit (0.0f, 1.0f, bm.strength);
        band (bm.a, bm.b, bm.width, bm.width * 1.25f, bm.colour.withAlpha (0.16f * s), bm.colour.withAlpha (0.05f * s));
        band (bm.a, bm.b, bm.width * 2.2f, bm.width * 2.6f, bm.colour.withAlpha (0.06f * s), bm.colour.withAlpha (0.015f * s));
    }
    if (lk.trails > 0.0f && ! trails.empty())
    {
        const double len = juce::jmax (0.05, (double) lk.trailLen);
        for (int s = 0; s < kNumStops; ++s)
        {
            const auto c = stopLightColour (s).interpolatedWith (juce::Colours::white, 0.55f + 0.45f * (1.0f - lk.pipeHue));
            for (int k = 0; k < kNumKeys; ++k)
            {
                const auto& tr = trails[(size_t) (s * kNumKeys + k)];
                if (tr.n < 2) continue;
                const float glowNow = juce::jmin (1.0f, glow[(size_t) s][(size_t) k]);
                const float rad = juce::jmax (0.02f, shown[(size_t) s][(size_t) k].rad);
                for (int i = 1; i < tr.n; ++i)
                {
                    const int i0 = (tr.head - i + 64) % 32, i1 = (tr.head - i - 1 + 64) % 32;      // (newest first)
                    const float age0 = (float) ((lookClock - tr.t[(size_t) i0]) / len), age1 = (float) ((lookClock - tr.t[(size_t) i1]) / len);
                    if (age1 >= 1.0f) break;
                    const V3 p0 = tr.p[(size_t) i0], p1 = tr.p[(size_t) i1];
                    if (dot (p1 - p0, p1 - p0) > 9.0f) continue;          // (a jump, not a path)
                    // (hidden behind a wall: left out)
                    float sx, sy, d;
                    const V3 mid = (p0 + p1) * 0.5f;
                    if (! rast.project (mid, sx, sy, d)) continue;
                    const float zd = rast.depthAt ((int) sx, (int) sy);
                    if (zd > 0.0f && zd < d * 0.95f - 0.4f) continue;
                    const float a0 = lk.trails * (1.0f - age0) * (0.25f + 0.75f * glowNow), a1 = lk.trails * (1.0f - age1) * (0.25f + 0.75f * glowNow);
                    band (p0, p1, 2.0f * rad, 2.0f * rad, c.withAlpha (0.55f * a0), c.withAlpha (0.55f * a1));
                }
            }
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
    // "Stimmung": read once a frame; the time since the last frame for what moves with it
    lookNow = readLook();
    {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        lookDt = lookLastMs > 0.0 ? (float) juce::jlimit (0.0, 0.1, (nowMs - lookLastMs) / 1000.0) : 1.0f / 60.0f;
        lookLastMs = nowMs;
        lookClock += lookDt;
    }
    const auto& lk = lookNow;
    // (Kamera schwankt: the steps sway more, and standing still the body breathes a little)
    const float swayMore = 1.0f + 3.0f * lk.sway;
    const float breath = lk.sway * 0.012f * (float) std::sin (lookClock * 1.1);
    const float sway = 0.022f * bobAmt * swayMore * std::sin (bobPhase) + breath - landDip;
    const V3 eye { homeP.x + px, homeP.y + py, juce::jmax (0.25f, homeP.z + fz + sway) };
    // (Kamera bebt: a hard knock shakes the view, dying away fast)
    shakeEnv *= std::exp (-7.0f * lookDt);
    const float shakeYaw = shakeEnv * 1.6f * (float) (std::sin (lookClock * 57.0) + 0.6 * std::sin (lookClock * 91.0 + 1.3));
    const float shakePitch = shakeEnv * 1.2f * (float) (std::sin (lookClock * 63.0 + 0.7) + 0.5 * std::sin (lookClock * 103.0));
    const float swayYaw = lk.sway * (0.5f * bobAmt * (float) std::sin (0.5f * bobPhase) + 0.35f * (float) std::sin (lookClock * 0.37));
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
        if (gpuFrames >= 90 && gpuFrames <= 90 + 60 * 12)
        {
            testStep = (gpuFrames - 60) / 60;          // 0 = as it is, 1 .. 9 = the kinds, 10 = the model, 11 = the model with the light off,
                                                       // 12 = the look (light's colour, wear, dissolving, a ripple, a coloured lamp)
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
    rast.setCamera (eye, yaw + shakeYaw + swayYaw, pitch + shakePitch, fov);
    // the time of day: where the light comes from (before the triangles: it shapes their light) and its colour
    Look look;
    {
        V3 sunDir { 0.35f, -0.25f, 0.9f };          // (as before; normalised by the renderer)
        if (lk.day > 0.0f)
        {
            V3 d; float tint[3];
            dayLight (lk.hour, d, tint);
            sunDir = normalised (sunDir * (1.0f - lk.day) + d * lk.day);
            for (int c = 0; c < 3; ++c) look.tint[c] = 1.0f + (tint[c] - 1.0f) * lk.day;
        }
        rast.setLightDir (sunDir);
        // (the pipes' shadows on the floor: with the time of day's light, the lower the sun the longer)
        shadowAmt = 0.0f;
        if (lk.day > 0.0f && sunDir.z > 0.05f)
        {
            float tint[3]; V3 d;
            dayLight (lk.hour, d, tint);
            shadowAmt = lk.day * juce::jlimit (0.0f, 1.0f, (tint[0] + tint[1] + tint[2]) / 3.0f - 0.3f) * 0.6f;
        }
    }
    look.wear = lk.wear;
    look.cell = lk.wallGrain < 0.05f ? 0.0f : 0.05f + 0.6f * lk.wallGrain;
    look.rippleAmt = lk.ripple;
    // (the sky of an open Saal: as bright as the time of day says - the light off: a night sky)
    viewScale = (float) area.getWidth() / (float) juce::jmax (1, rast.width());
    const float fogDist = juce::jmax (12.0f, 1.5f * juce::jmax (r.W(), r.L()));      // (floor and ceiling stay readable to the far end)
    // (light off: what is far away sinks into the dark, not into a grey haze)
    const bool dark = (proc.walkDark && ! compareNow) || testStep == 11;      // (the test: one lamp, below)
    // (a comparison: without the film grain; in the dark less of it, so nothing glitters where no lamp shines)
    const float grainAmt = compareNow ? 0.0f : (dark ? 0.05f : 0.12f);
    // "Hallnebel": a mist as dense as the late reverb is loud - after a chord it hangs for the reverb's length
    {
        const float tailDb = proc.engine.tailLevelNow.load (std::memory_order_relaxed);
        const float tail = juce::jlimit (0.0f, 1.0f, (tailDb + 54.0f) / 48.0f);
        const float want = lk.fog * ((1.0f - lk.fogFollow) + lk.fogFollow * tail);
        fogDensNow += (want - fogDensNow) * (1.0f - std::exp (-lookDt / 0.15f));
    }
    const float fogTone0 = dark ? 0.0f : 0.07f;
    const float fogToneNow = fogTone0 + (dark ? 0.12f : 0.62f) * fogDensNow;
    const float fogDistNow = fogDist / (1.0f + 16.0f * fogDensNow);
    rast.begin (fogToneNow, fogDistNow, grainAmt, ++frame);
    // the 360° recording: this frame's triangles are kept (all around the eye: nothing left out for lying
    // behind the view) when the next video frame is due and the renderer is free
    {
        auto& rec = proc.pano360;
        const bool on = rec.isRecording() || panoForce;
        rast.setAllAround (on);
        panoCapturing = panoForce || (on && ! compareNow && rec.wantsScene());
        panoCalls.clear();
        panoStatic = 0;
        rast.setRecording (panoCapturing ? &panoCalls : nullptr);
    }

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
    { Material m; m.kind = Material::plain; m.tone = 1.3f; m.gloss = 1.0f; m.rough = 0.0f; rast.setMaterial (kSlotGlass, m); }      // (the Vox humana: smooth, light, no mouth)
    {
        // "Glanz": glass, mirrors and metal catch the light; "Waende vergehen": the surfaces the room keyboard takes
        // away dissolve (as far as it has taken them, gliding)
        auto mats = rast.materials();
        for (auto& m : mats)
            if (m.kind == Material::glass || m.kind == Material::mirror || m.kind == Material::metal || (m.kind == Material::plain && m.gloss >= 1.0f))
                m.gloss = juce::jmin (4.0f, m.gloss * (1.0f + 3.0f * lk.shine) + 0.6f * lk.shine);
        // (what is open of the Saal for good - a courtyard, a ruin, a field - is always gone; what the room keyboard
        //  takes away as far as "Waende vergehen" says)
        const auto& op = params.opens[0];
        skyOpen = 0.0f;
        if (lk.walls > 0.0f || op.shape > 0)
        {
            auto gone = [&] (int surf)
            {
                const float g = proc.engine.wallGoneNow[0][(size_t) surf].load (std::memory_order_relaxed);
                return juce::jlimit (0.0f, 1.0f, juce::jmax (juce::jmin (g, openGone (op.shape, op.amount, surf)), g * lk.walls));
            };
            mats[(size_t) kSlotFloor].gone = gone (kSurfFloor);
            mats[(size_t) kSlotCeiling].gone = gone (kSurfCeiling);
            for (int i = 0; i < kMaxPlanVerts; ++i)
            {
                mats[(size_t) (kSlotWall0 + i)].gone = gone (kSurfWall0 + i);
                skyOpen = juce::jmax (skyOpen, 0.8f * mats[(size_t) (kSlotWall0 + i)].gone);
            }
            skyOpen = juce::jmax (skyOpen, mats[(size_t) kSlotCeiling].gone);
        }
        if (testStep == 12)
            for (int sl : { (int) kSlotWall, (int) kSlotFloor, (int) kSlotCeiling, (int) kSlotModel })
                mats[(size_t) sl].gone = 0.35f;
        rast.setMaterials (mats);
    }
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
        // "Tueren sichtbar": the door of Saal 1 to another Saal, an opening with the other Saal behind it
        DoorCut cut;
        cutNow = {};
        const auto& dr = params.doors[0];
        const bool showDoor = lk.doors > 0.01f && dr.to > 0 && dr.to < kNumRooms;
        if (showDoor)
        {
            cut.wall = dr.wall;
            cut.halfW = 0.7f;
            cut.height = juce::jmin (2.6f, 0.8f * r.H());
        }
        buildRoomTris (r, roomTris, cut);
        cutNow = cut;
        // (the surface the cross points at - the one M gives another material - a little lighter)
        aimSurf = surfaceAhead (r, eye);
        const int aimSlot = aimSurf < 0 ? -1 : (aimSurf == kSurfFloor ? (int) kSlotFloor : (aimSurf == kSurfCeiling ? (int) kSlotCeiling : kSlotWall0 + aimSurf - kSurfWall0));
        for (const auto& t : roomTris) rast.triangle (t.a, t.b, t.c, t.slot, t.slot == aimSlot && ! panoCapturing ? t.tone * 1.1f : t.tone);
        {
            // the paint: every material its own slot, looking as it would as the whole wall
            for (int m = 0; m < kNumSurfMaterials; ++m)
            {
                const auto& sm = surfMaterial (m);
                rast.setMaterial (kSlotPaint0 + m, wallMaterial (sm.aw, sm.hi, r.scatter));
            }
            addPaint (r);
        }
        if (showDoor)
        {
            RoomParams next = params.roomAt (dr.to);
            addDoorView (r, cut, dr.open * juce::jmin (1.0f, lk.doors * 2.0f), next, lk.doorDim);
        }
    }

    panoStatic = panoCalls.size();          // (the 360° recording: up to here the room, kept only when it changes)
    casters.clear();
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
            // a sounding pipe shines as strongly as the wind it gets (a half-drawn stop, an empty
            // bellows: dimmer); it rises quickly to that and fades as before
            if ((sounding >> k) & 1)
            {
                const float tgt = proc.engine.windShown (s, k);
                gl = gl < tgt ? juce::jmin (tgt, gl + 0.35f) : juce::jmax (tgt, gl * 0.9f);
            }
            else gl *= 0.9f;
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
            // the mouth towards the listener's place, turned by the pipe's Ausrichtung and Drehung (counter-clockwise from above)
            const float face = std::atan2 (homeP.y - c.y, homeP.x - c.x) + juce::degreesToRadians (proc.engine.facingShown (s, k));
            addPipe (bottom, len, rad, face, metalTone, dark ? gl + 0.025f : gl, s);    // (light off: the barest shimmer of their own)
            if (bottom.z > 0.02f) casters.push_back ({ bottom, len, rad });     // (its shadow: below, once the lights are known)
            shown[(size_t) s][(size_t) k] = { { c.x, c.y, c.z + bob }, len, rad, true };
            if (gl > 0.02f) glows.push_back ({ { c.x, c.y, c.z + bob }, gl, len, s });
        }
    }
    addVoxHumana (r, homeP, dark);
    // Konzert: the others in the Saal (the player, the other listeners), eased between their places (they come
    // ten times a second)
    {
        scene.figureLabels.clear();
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const float k = figSmoothMs > 0.0 ? 1.0f - std::exp (-(float) (nowMs - figSmoothMs) / 90.0f) : 1.0f;
        figSmoothMs = nowMs;
        std::map<int, std::array<float, 4>> next;
        for (const auto& f : proc.concert.figures())
        {
            std::array<float, 4> s { f.x, f.y, f.z, f.yaw };
            if (auto it = figSmooth.find (f.id); it != figSmooth.end())
            {
                const auto& o = it->second;
                const float far = std::abs (f.x - o[0]) + std::abs (f.y - o[1]);
                if (far < 6.0f)
                {
                    for (int i = 0; i < 3; ++i) s[(size_t) i] = o[(size_t) i] + k * (s[(size_t) i] - o[(size_t) i]);
                    float dy = std::fmod (f.yaw - o[3] + 540.0f, 360.0f) - 180.0f;
                    s[3] = o[3] + k * dy;
                }
            }
            next[f.id] = s;
            const V3 feet { homeP.x + s[0], homeP.y + s[1], juce::jmax (0.0f, s[2]) };
            const float h = juce::jlimit (1.2f, 2.0f, homeP.z + 0.12f);
            addFigure (feet, h, s[3]);
            scene.figureLabels.push_back ({ { feet.x, feet.y, feet.z + h + 0.18f }, f.name });
        }
        figSmooth = std::move (next);
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
            ripples[(size_t) rippleNext] = { f.at, 0.0f, f.strength };
            knocksNow.push_back (f.at);
            rippleNext = (rippleNext + 1) % Look::kMaxRipples;
            // (the nearer and harder the knock, the more the view shakes)
            const float dd = std::sqrt (dot (f.at - eye, f.at - eye));
            shakeEnv = juce::jmax (shakeEnv, lk.shake * f.strength * juce::jlimit (0.15f, 1.0f, 3.0f / (1.0f + dd)));
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
    // light off: the sounding pipes (and the flashes of knocks) are the lamps of the room; "Pfeifenlicht": they
    // light it also with the light on, each stop in its colour, as far as it reaches
    {
        for (auto& rp : ripples) rp.age = juce::jmin (1.0f, rp.age + lookDt / 1.6f);
        look.ripples = ripples;
        if (testStep == 12)
        {
            // (the test of the look: everything the renderer does with it at once)
            const float ya = juce::degreesToRadians (yaw);
            look.tint[0] = 1.12f; look.tint[1] = 0.9f; look.tint[2] = 0.7f;
            look.wear = 1.0f; look.cell = 0.3f; look.rippleAmt = 1.0f;
            look.ripples = {};
            look.ripples[0] = { eye + V3 { -std::sin (ya) * 4.0f, std::cos (ya) * 4.0f, -1.0f }, 0.3f, 1.0f };
        }
        if (skyOpen > 0.0f)
        {
            float sb = 0.78f;
            if (lk.day > 0.0f)
            {
                float tint[3]; V3 d;
                dayLight (lk.hour, d, tint);
                sb = sb * (1.0f - lk.day) + lk.day * juce::jlimit (0.08f, 0.95f, 0.85f * (tint[0] + tint[1] + tint[2]) / 3.0f);
            }
            look.sky = skyOpen * (dark ? 0.05f : sb);
        }
        rast.setLook (look);
        lamps.clear();
        const float pl = lk.pipeLight;
        if (dark || pl > 0.0f)
        {
            // (each a small light; a full chord of many stops is brighter than one pipe, but not without end)
            float sum = 0.0f;
            for (const auto& gw : glows)
            {
                if (! dark && gw.stop < 0) continue;          // (with the light on only the pipes)
                Raster::Lamp lp { gw.at, (dark ? 0.8f * (1.0f + pl) : 0.9f * pl) * juce::jmin (1.0f, gw.amount),
                                  pl > 0.0f ? lk.pipeReach * (0.75f + 0.12f * juce::jmin (3.0f, gw.size)) : 1.8f + 0.45f * juce::jmin (3.0f, gw.size) };
                if (pl > 0.0f && gw.stop >= 0)
                {
                    // (from a warm white to the stop's own colour)
                    const auto c = stopLightColour (gw.stop);
                    const float hue = lk.pipeHue;
                    lp.r = 1.0f + (c.getFloatRed() - 1.0f) * hue;
                    lp.g = 0.92f + (c.getFloatGreen() - 0.92f) * hue;
                    lp.b = 0.78f + (c.getFloatBlue() - 0.78f) * hue;
                }
                lamps.push_back (lp);
                sum += lamps.back().power;
            }
            const float cap = dark ? 3.0f * (1.0f + pl) : 2.5f * pl;
            if (sum > cap)
                for (auto& l : lamps) l.power *= cap / sum;
        }
        if (testStep == 11 || testStep == 12)
        {
            // (the test of the light off: one lamp three metres ahead, a little above the eyes; the look's: a coloured one)
            const float ya = juce::degreesToRadians (yaw);
            lamps.push_back ({ eye + V3 { -std::sin (ya) * 3.0f, std::cos (ya) * 3.0f, 0.3f }, 1.5f, 3.0f });
            if (testStep == 12) { lamps.back().r = 1.0f; lamps.back().g = 0.4f; lamps.back().b = 0.2f; }
        }
        rast.setLamps (lamps, dark ? kDarkRest : 1.0f);
    }
    if (mesh == nullptr) castShadows (r, dark);
    updateAir (r, params, glows);
    {
        // "Schaerfentiefe": sharp is what you look at - the pipe under the cross, else what lies in the middle of the
        // view (as the last frame had it); the focus follows in a quarter of a second
        float want = focusNow;
        if (aimStop >= 0 && aimKey >= 0 && shown[(size_t) aimStop][(size_t) aimKey].on)
            want = rast.viewDepth (shown[(size_t) aimStop][(size_t) aimKey].c);
        else if (const float dc = rast.depthAt (rast.width() / 2, rast.height() / 2); dc > 0.0f)
            want = dc;
        focusNow += (juce::jlimit (0.3f, 200.0f, want) - focusNow) * (1.0f - std::exp (-lookDt / 0.25f));
        rast.setFocus (focusNow, compareNow ? (testStep == 12 ? 1.0f : 0.0f) : lk.focus);
    }
    if (panoCapturing)
    {
        // the 360° frame: its triangles, materials, light and glows to the renderer's thread
        rast.setRecording (nullptr);
        PanoJob job;
        job.calls = std::move (panoCalls);
        panoCalls = {};
        job.staticCount = panoStatic;
        job.mats = rast.materials();
        job.lamps = lamps;
        job.ambient = dark ? kDarkRest : 1.0f;
        job.eye = earsAway ? earEye : eye;          // (the ears left behind: the film is shot where they listen)
        job.fogTone = rast.fogToneNow();
        job.fogDistance = fogDistNow;
        job.grain = grainAmt;
        job.frameNo = frame;
        for (const auto& gw : glows) if (gw.px <= 0.0f) job.glows.push_back ({ gw.at, gw.amount, gw.size });
        job.upTo = proc.pano360.frameNow();
        if (panoForce) panoTestJob = std::move (job);
        else proc.pano360.addScene (std::move (job));
        panoCapturing = false;
    }
    geoMs = (float) (juce::Time::getMillisecondCounterHiRes() - t0);
    if (card)
    {
        gpuFrame.dark = rast.darkNow();
        gpuFrame.ambient = rast.ambientNow();
        gpuFrame.focalPerWidth = rast.focalPerWidth();
        gpuFrame.lamps = rast.lampsInView();
        rast.exportGpu (gpuFrame.verts, gpuFrame.mats);
        rast.lookForGpu (gpuFrame.tint, gpuFrame.look, gpuFrame.ripples.data());
        gpuFrame.focus[0] = 1.0f / juce::jmax (0.2f, focusNow);
        gpuFrame.sky = rast.skyNow();
        gpuFrame.focus[1] = compareNow ? (testStep == 12 ? 1.0f : 0.0f) : lk.focus;
        gpuFrame.fogTone = rast.fogToneNow(); gpuFrame.fogInv = rast.fogInvNow();
        gpuFrame.grain = grainAmt; gpuFrame.frameNo = frame;
        if (compareNow)
        {
            // (test) the same frame from the processor, to be compared with what the card draws
            rast.finish();
            gpuFrame.compareWith = rast.image().createCopy();
            static const char* const stepNames[] { "0_scene", "1_stone", "2_plaster", "3_wood", "4_glass", "5_curtain", "6_mirror", "7_slabs", "8_boards", "9_carpet", "10_model", "11_dark", "12_look" };
            gpuFrame.compareDir = RysumProcessor::getPatchFolder().getParentDirectory().getChildFile ("gpu-test").getChildFile (stepNames[juce::jlimit (0, 12, testStep)]);
        }
        else
        {
            rast.finishDepth();
            // (a comparison frame the card had not drawn yet comes back here when the next frame takes its place:
            //  it must not ride along with this one, which has grain)
            gpuFrame.compareWith = {};
        }
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
                const float rad = gw.px > 0.0f ? juce::jlimit (0.8f, 5.0f, gw.px * 4.0f / d)
                                               : juce::jlimit (6.0f, 260.0f, (0.6f + 0.35f * gw.size) / d * 0.5f * (float) area.getWidth());
                gpuFrame.addGlow (sx / rw * 2.0f - 1.0f, 1.0f - sy / rh * 2.0f, 2.0f * rad / (float) area.getWidth(), 2.0f * rad / (float) area.getHeight(),
                                  juce::jlimit (0.0f, 1.0f, gw.amount));
            }
        buildVeils ((float) area.getWidth() / rw);
        gpuFrame.veils.clear();
        if (! compareNow)
        {
            const float aw = (float) area.getWidth(), ah = (float) area.getHeight();
            auto put = [&] (juce::Point<float> p, juce::Colour c)
            {
                gpuFrame.veils.insert (gpuFrame.veils.end(), { p.x / aw * 2.0f - 1.0f, 1.0f - p.y / ah * 2.0f,
                                                               c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue(), c.getFloatAlpha() });
            };
            for (const auto& v : veils)
            {
                put (v.a0, v.ca); put (v.a1, v.ca); put (v.b1, v.cb);
                put (v.a0, v.ca); put (v.b1, v.cb); put (v.b0, v.cb);
            }
        }
        gpu->submit (gpuFrame);
    }
    else
    {
        rast.finish();
        buildVeils ((float) area.getWidth() / (float) rast.width());
    }
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

    // bands of light: the rays and the trails (added to the light)
    if (! gpuOn() && ! veils.empty())
    {
        for (const auto& v : veils)
        {
            juce::Path p;
            p.startNewSubPath (v.a0); p.lineTo (v.a1); p.lineTo (v.b1); p.lineTo (v.b0); p.closeSubPath();
            const auto ma = (v.a0 + v.a1) * 0.5f, mb = (v.b0 + v.b1) * 0.5f;
            if (v.ca == v.cb || ma.getDistanceFrom (mb) < 2.0f) g.setColour (v.ca);
            else g.setGradientFill (juce::ColourGradient (v.ca, ma, v.cb, mb, false));
            g.fillPath (p);
        }
    }
    // the sounding pipes shine (where they are not hidden behind a wall)
    for (const auto& gw : glows)
    {
        if (gpuOn()) break;          // (the card draws them)
        float sx, sy, d;
        if (! rast.project (gw.at, sx, sy, d)) continue;
        const float zd = rast.depthAt ((int) sx, (int) sy);
        if (zd > 0.0f && zd < d * 0.95f - 0.4f) continue;         // (the pipe itself is drawn a little nearer than its axis)
        const float rad = gw.px > 0.0f ? juce::jlimit (0.8f, 5.0f, gw.px * 4.0f / d)
                                       : juce::jlimit (6.0f, 260.0f, (0.6f + 0.35f * gw.size) / d * 0.5f * (float) area.getWidth());
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
    if (actLook.getToggleState() && ! immersive)
    {
        g.setColour (col::bg.withAlpha (0.82f));
        g.fillRect (lookArea);
        g.setColour (col::border);
        g.drawRect (lookArea, 1);
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
    const auto info = rysum::tr("Begehen \xc2\xb7 ") + (mesh != nullptr ? mesh->name : U ("Saal ") + juce::String (r.W(), 1) + U (" \xc3\x97 ") + juce::String (r.L(), 1) + U (" \xc3\x97 ") + juce::String (r.H(), 1) + " m")
                    + rysum::tr("   \xc2\xb7   Platz ") + juce::String (eye.x, 1) + " / " + juce::String (eye.y, 1) + rysum::tr(" m   Blick ") + juce::String (juce::roundToInt (-yaw)) + U ("\xc2\xb0")
                    + rysum::tr("   \xc2\xb7   W\xc3\xa4nde: ") + rysum::tr (materialName (wallLook))
                    + (proc.engine.hiddenNow.load() > 0 ? U ("   \xc2\xb7   ") + juce::String (proc.engine.hiddenNow.load()) + rysum::tr(" Pfeifen um die Ecke geh\xc3\xb6rt") : juce::String())
                    + (aimSurf >= 0 && ! isSpectator() ? U ("   \xc2\xb7   ") + rysum::trText (surfaceName (r, aimSurf)) + rysum::tr("  [M: Material]") : juce::String());
    g.drawText (info + (proc.timeBackNow.load() > 0.05f ? rysum::tr("   \xc2\xb7   du h\xc3\xb6rst, was vor ") + juce::String (proc.timeBackNow.load(), 1) + rysum::tr(" s erklang") : juce::String())
                     + (proc.engine.breakCount.load() > 0 ? U ("   \xc2\xb7   ") + juce::String ((int) proc.engine.breakCount.load()) + rysum::tr(" Pfeifen gebrochen") : juce::String()),
                area.reduced (18, 0).withTrimmedTop (84).removeFromTop (16), juce::Justification::centredLeft);
    g.setColour (col::faint);
    if (isSpectator())
        g.drawText (rysum::tr("W A S D gehen  \xc2\xb7  Leertaste springen  \xc2\xb7  Maus ziehen: schauen  \xc2\xb7  \xe2\x86\x90 \xe2\x86\x92 drehen  \xc2\xb7  Shift schneller  \xc2\xb7  Rad: Blickwinkel  \xc2\xb7  "
                       "I immersiv  \xc2\xb7  L Licht  \xc2\xb7  H Ohren hierlassen")
                       + (handsMay (2) ? rysum::tr("  \xc2\xb7  E Druckwelle  \xc2\xb7  C klatschen  \xc2\xb7  Klick: anklopfen  \xc2\xb7  Q tragen und werfen  \xc2\xb7  R zur\xc3\xbc" "ckrufen  \xc2\xb7  T Wirbel")
                          : handsMay (1) ? rysum::tr("  \xc2\xb7  E Druckwelle  \xc2\xb7  C klatschen  \xc2\xb7  Klick: anklopfen")
                                         : rysum::tr("   \xe2\x80\x94   du h\xc3\xb6rst zu: gespielt wird beim Spieler")),
                    area.reduced (18, 0).removeFromBottom (24), juce::Justification::centredLeft);
    else
        g.drawText (rysum::tr("W A S D gehen  \xc2\xb7  Leertaste springen  \xc2\xb7  Maus ziehen: schauen  \xc2\xb7  \xe2\x86\x90 \xe2\x86\x92 drehen  \xc2\xb7  Shift schneller  \xc2\xb7  M: Material der Fl\xc3\xa4" "che  \xc2\xb7  Rad: Blickwinkel  \xc2\xb7  Tab: alle Tasten  \xc2\xb7  Esc zur\xc3\xbc" "ck"
                      "   \xe2\x80\x94   gespielt wird hier \xc3\xbc" "ber MIDI oder \xe2\x80\x9eHalten\xe2\x80\x9c"),
                    area.reduced (18, 0).removeFromBottom (24), juce::Justification::centredLeft);
    g.drawText (rysum::tr("Tab: alle Tasten  \xc2\xb7  G: Bild ") + (gpuOn() ? rysum::tr("Grafikkarte") : (gpuWanted() && gpuNote.isNotEmpty() ? rysum::tr("Prozessor (") + gpuNote + ")" : rysum::tr("Prozessor"))), handsArea.withTrimmedLeft (8), juce::Justification::centredLeft);
    {
        // the player moves pipes, but no stop has pipes that can be moved: say how
        bool anyBodies = false;
        for (int s = 0; s < kNumStops; ++s) anyBodies = anyBodies || isRoomMotion (params.motionMode[(size_t) s]);
        if (params.live.playerMode > 0 && ! anyBodies && juce::Time::getMillisecondCounterHiRes() >= statusUntil)
        {
            g.setColour (col::rubric.brighter (0.2f));
            g.drawText (rysum::tr("Die Pfeifen reagieren auf dich, wenn die Bewegung eines Registers (Raum-Seite) im Saal stattfindet: Schweben, Flug, Fall, Schwarm oder Str\xc3\xb6mung."),
                        area.reduced (18, 0).withTrimmedTop (102).removeFromTop (16), juce::Justification::centredLeft);
        }
    }
    if (! proc.isBinaural())
    {
        g.setColour (col::rubric.brighter (0.2f));
        g.drawText (rysum::tr("Die Kopfdrehung ist nur bei Kopfh\xc3\xb6rer-Ausgabe (binaural) zu h\xc3\xb6ren; Gehen \xc3\xbc" "berall."),
                    area.reduced (18, 0).withTrimmedBottom (52).removeFromBottom (16), juce::Justification::centredLeft);
    }
    if (juce::Time::getMillisecondCounterHiRes() < statusUntil && status.isNotEmpty())
    {
        g.setColour (col::ink);
        g.drawText (rysum::trText (status), area.reduced (18, 0).withTrimmedTop (102).removeFromTop (16), juce::Justification::centredLeft);
    }

    // Konzert: a name over everyone else in the Saal
    if (! scene.figureLabels.empty())
    {
        const float isc = rast.width() > 0 ? (float) area.getWidth() / (float) rast.width() : 1.0f;
        g.setFont (A.monoFont (12.0f));
        for (const auto& fl : scene.figureLabels)
        {
            float sx, sy, d;
            if (! rast.project (fl.head, sx, sy, d) || d < 0.3f) continue;
            const juce::Rectangle<float> box (sx * isc - 80.0f, sy * isc - 18.0f, 160.0f, 16.0f);
            g.setColour (col::bg.withAlpha (0.55f));
            g.fillRoundedRectangle (box.withSizeKeepingCentre ((float) juce::GlyphArrangement::getStringWidth (A.monoFont (12.0f), fl.name) + 12.0f, 16.0f), 3.0f);
            g.setColour (col::ink);
            g.drawText (fl.name, box, juce::Justification::centred, false);
        }
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
    // Konzert: the others, a red dot with a stroke the way they look
    for (const auto& [id, s] : figSmooth)
    {
        juce::ignoreUnused (id);
        const auto q = P (homeP.x + s[0], homeP.y + s[1]);
        const float fa = juce::degreesToRadians (s[3]);
        const auto ahead = P (homeP.x + s[0] - std::sin (fa) * 1.2f, homeP.y + s[1] + std::cos (fa) * 1.2f);
        g.setColour (col::rubric.brighter (0.3f));
        g.drawLine ({ q, ahead }, 1.5f);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (q));
    }
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
        say (rysum::tr("Publikum aus: die Seite f\xc3\xbcr die Handys ist geschlossen"));
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
    if (! ok) { say (rysum::tr("Publikum: kein freier Port (8787 bis 8796) \xe2\x80\x93 l\xc3\xa4uft Rysum schon mehrmals?"), 5000.0); return; }
    say (rysum::tr("Publikum an: ") + proc.getAudience()->url() + (woken > 0 ? U ("  \xc2\xb7  ") + juce::String (woken) + rysum::tr(" Register schweben jetzt") : juce::String())
         + rysum::tr("  \xc2\xb7  Windows fragt beim ersten Mal, ob Rysum ins Netz darf"), 6000.0);
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
    g.drawText (phones == 0 ? rysum::tr("Handy: QR-Code scannen") : juce::String (phones) + (phones == 1 ? rysum::tr(" Handy spielt mit") : rysum::tr(" Handys spielen mit")),
                box.getX(), box.getBottom() + 12, side, 16, juce::Justification::centred);
    if (hint)
    {
        g.setColour (juce::Colour (0xffa03010));
        g.drawFittedText (rysum::tr("Handy l\xc3\xa4" "dt nicht? Windows-Firewall:\nRysum im privaten Netz zulassen"), box.getX() + 2, box.getBottom() + 28, side - 4, 28, juce::Justification::centred, 2, 0.7f);
    }
}

// ---------------------------------------------------------------------------
// the 360° recording
// ---------------------------------------------------------------------------
void WalkView::panoClicked()
{
    auto& rec = proc.pano360;
    if (rec.isRecording()) { stopPano(); return; }
    actPano.setToggleState (rec.state() != Pano360Recorder::State::idle, juce::dontSendNotification);
    juce::PopupMenu m;
    juce::Component::SafePointer<WalkView> safe (this);
    if (rec.state() == Pano360Recorder::State::drawing)
    {
        m.addSectionHeader (rysum::tr("Die 360\xc2\xb0-Aufnahme wird gezeichnet"));
        m.addItem (8, rysum::tr("Zeichnen abbrechen"));
        m.addItem (9, rysum::tr("Ordner der Aufnahmen \xc3\xb6" "ffnen"));
        m.addItem (12, rysum::tr("360\xc2\xb0-Metadaten in ein Video schreiben \xe2\x80\xa6"));
    }
    else
    {
        m.addSectionHeader (rysum::tr("360\xc2\xb0-Video aufnehmen"));
        m.addItem (1, "3840 \xc3\x97 1920 (4K), 30 " + rysum::tr("Bilder/s"));
        m.addItem (2, "3840 \xc3\x97 1920 (4K), 25 " + rysum::tr("Bilder/s"));
        m.addItem (3, "2560 \xc3\x97 1280, 30 " + rysum::tr("Bilder/s"));
        m.addItem (4, "2048 \xc3\x97 1024 (2K), 30 " + rysum::tr("Bilder/s"));
        m.addItem (5, "5760 \xc3\x97 2880 (5.7K), 30 " + rysum::tr("Bilder/s"));
        m.addSeparator();
        // where it goes (and how much room is left there: 4K frames take gigabytes per minute)
        const auto base = panoBaseFolder();
        auto probe = base;
        while (! probe.exists() && probe.getParentDirectory() != probe) probe = probe.getParentDirectory();
        const auto freeBytes = probe.exists() ? probe.getBytesFreeOnVolume() : (juce::int64) 0;
        m.addSectionHeader (rysum::tr("Speicherort: ") + base.getFullPathName() + "  (" + juce::String ((double) freeBytes / 1.0e9, 1) + rysum::tr(" GB frei") + ")");
        m.addItem (10, rysum::tr("Speicherort w\xc3\xa4hlen \xe2\x80\xa6"));
        if (base != panoDefaultFolder()) m.addItem (11, rysum::tr("Speicherort: wieder Dokumente/Rysum/360"));
        m.addItem (9, rysum::tr("Ordner der Aufnahmen \xc3\xb6" "ffnen"));
        m.addItem (12, rysum::tr("360\xc2\xb0-Metadaten in ein Video schreiben \xe2\x80\xa6"));
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&actPano), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r == 8) { safe->proc.pano360.abort(); safe->updatePanoLabel(); return; }
        if (r == 9)
        {
            auto d = panoBaseFolder();
            d.createDirectory();
            d.startAsProcess();
            return;
        }
        if (r == 10) { safe->choosePanoFolder(); return; }
        if (r == 12) { safe->markVideo(); return; }
        if (r == 11) { setPanoBaseFolder (panoDefaultFolder()); safe->say (rysum::tr("Speicherort: ") + panoDefaultFolder().getFullPathName(), 4000.0); return; }
        const int widths[] { 3840, 3840, 2560, 2048, 5760 }, rates[] { 30, 25, 30, 30, 30 };
        safe->startPano (widths[r - 1], rates[r - 1]);
    });
}

bool WalkView::startPanoForTest (const juce::File& folder, int width, int fps)
{
    juce::String err;
    panoFinishedSeen = proc.pano360.finishedCount();
    return proc.pano360.start (folder, { width, fps, 92 }, proc.getSampleRate(), err);
}

void WalkView::choosePanoFolder()
{
    chooser = std::make_unique<juce::FileChooser> (rysum::tr("Wohin die 360\xc2\xb0-Aufnahmen kommen"), panoBaseFolder().exists() ? panoBaseFolder() : juce::File());
    juce::Component::SafePointer<WalkView> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (safe == nullptr || f == juce::File()) return;
        if (! f.isDirectory() && ! f.createDirectory()) { safe->say (rysum::tr("Kann den Ordner nicht anlegen: ") + f.getFullPathName(), 5000.0); return; }
        setPanoBaseFolder (f);
        safe->say (rysum::tr("Speicherort: ") + f.getFullPathName() + "  (" + juce::String ((double) f.getBytesFreeOnVolume() / 1.0e9, 1) + rysum::tr(" GB frei") + ")", 5000.0);
    });
}

void WalkView::markVideo()
{
    // a video made by hand (make_video.bat): marked as 360 degrees with spatial sound, next to it as ..._360
    chooser = std::make_unique<juce::FileChooser> (rysum::tr("Welches Video (360\xc2\xb0, mit Ambisonics 1. Ordnung)?"),
                                                   panoBaseFolder().exists() ? panoBaseFolder() : juce::File(), "*.mov;*.mp4");
    juce::Component::SafePointer<WalkView> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
    {
        const auto in = fc.getResult();
        if (safe == nullptr || ! in.existsAsFile()) return;
        if (safe->markBusy.load()) { safe->say (rysum::tr("Ein Video wird noch markiert"), 4000.0); return; }
        const auto out = in.getSiblingFile (in.getFileNameWithoutExtension() + "_360" + in.getFileExtension());
        safe->say (rysum::tr("360\xc2\xb0-Metadaten werden geschrieben: ") + out.getFullPathName(), 4000.0);
        // (a copy of the whole file: on a thread of the view's, stopped and waited for when the view goes)
        if (safe->markThread.joinable()) safe->markThread.join();
        safe->markStop = false;
        safe->markBusy = true;
        auto* view = safe.getComponent();
        safe->markThread = std::thread ([view, safe, in, out]
        {
            juce::String error;
            const bool ok = rysum::injectSpatialMetadata (in, out, error, [view] { return view->markStop.load(); });
            view->markBusy = false;
            juce::MessageManager::callAsync ([safe, ok, out, error]
            {
                if (safe == nullptr) return;
                if (ok) safe->say (rysum::tr("Fertig f\xc3\xbcr YouTube: ") + out.getFullPathName(), 7000.0);
                else safe->say (rysum::tr("Die 360\xc2\xb0-Metadaten lie\xc3\x9f" "en sich nicht schreiben: ") + error, 8000.0);
            });
        });
    });
}

void WalkView::run360Test (int step)
{
    juce::Component::SafePointer<WalkView> safe (this);
    auto& rec = proc.pano360;
    const auto root = RysumProcessor::getPatchFolder().getParentDirectory();
    if (step == 0)
    {
        const auto dir = root.getChildFile ("360-test").getChildFile (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"));
        juce::String err;
        panoFinishedSeen = rec.finishedCount();
        testStarted = rec.start (dir, { 2048, 30, 92 }, proc.getSampleRate(), err);
        testFolder = dir;
        testNote = err;
        juce::Timer::callAfterDelay (2500, [safe] { if (safe != nullptr) safe->run360Test (1); });
        return;
    }
    if (step == 1) { stopPano(); juce::Timer::callAfterDelay (500, [safe] { if (safe != nullptr) safe->run360Test (2); }); return; }
    if (rec.state() != Pano360Recorder::State::idle && step < 600)
    {
        juce::Timer::callAfterDelay (500, [safe, step] { if (safe != nullptr) safe->run360Test (step + 1); });
        return;
    }
    juce::String t;
    t << "started: " << (testStarted ? "yes" : "no " + testNote) << "\n"
      << "frames due " << rec.framesTotal() << ", scenes " << rec.scenesKept() << ", frames drawn " << rec.framesDrawn() << ", last "
      << juce::String (rec.lastFrameMs(), 1) << " ms\n"
      << "drawn on: " << (rec.drawingOn().isNotEmpty() ? rec.drawingOn() : "the processor (" + rec.whyNotOnCard() + ")") << "\n"
      << "video: " << (testFolder.getChildFile ("rysum360.mov").existsAsFile() ? juce::String (testFolder.getChildFile ("rysum360.mov").getSize()) + " bytes" : "none " + rec.videoProblem()) << "\n"
      << "the view's card: " << (gpu != nullptr ? juce::String (gpu->isReady() ? "ready" : "not ready") + (gpu->hasFailed() ? ", FAILED " + gpu->failure() : juce::String())
                                                  + ", " + juce::String (gpu->framesPerSecond(), 1) + " pictures/s, " + gpu->cardName()
                                                : juce::String ("none")) << "\n"
      << "note: " << gpuNote << "\n\n"
      << testFolder.getChildFile (rysum::german() ? "LIESMICH.txt" : "README.txt").loadFileAsString();
    root.getChildFile ("360-test.txt").replaceWithText (t);
}

void WalkView::startPano (int width, int fps)
{
    const auto base = panoBaseFolder();
    if (! base.isDirectory() && ! base.createDirectory())
    {
        // (a drive that is not there now: say so instead of writing somewhere else)
        say (rysum::tr("Kann den Ordner nicht anlegen: ") + base.getFullPathName(), 6000.0);
        return;
    }
    const auto dir = base.getChildFile (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"));
    juce::String err;
    panoFinishedSeen = proc.pano360.finishedCount();
    if (! proc.pano360.start (dir, { width, fps, 92 }, proc.getSampleRate(), err)) { say (err, 5000.0); return; }
    panoWarned = false;
    actPano.setToggleState (true, juce::dontSendNotification);
    // (little room left: a bright 4K frame is about 1.6 MB, some 3 GB a minute at 30 frames/s - a dark one far less)
    const double gbPerMinute = 3.0 * (double) width * (double) width / (3840.0 * 3840.0) * (double) fps / 30.0;
    if ((double) base.getBytesFreeOnVolume() / 1.0e9 < 5.0 * gbPerMinute)
        say (rysum::tr("360\xc2\xb0: wenig Platz auf dem Laufwerk \xe2\x80\x93 ") + juce::String ((double) base.getBytesFreeOnVolume() / 1.0e9, 1)
             + rysum::tr(" GB frei, etwa ") + juce::String (gbPerMinute, 1) + rysum::tr(" GB je Minute"), 7000.0);
    else
        say (rysum::tr("360\xc2\xb0-Aufnahme l\xc3\xa4uft: ") + dir.getFullPathName(), 4000.0);
    panoLabelMs = 0.0;
    updatePanoLabel();
}

void WalkView::stopPano()
{
    if (! proc.pano360.isRecording()) return;
    proc.pano360.stop();
    rast.setAllAround (false);
    say (rysum::tr("360\xc2\xb0: die Bilder werden jetzt gezeichnet \xe2\x80\x93 der Ordner \xc3\xb6" "ffnet sich, wenn sie fertig sind"), 5000.0);
    panoLabelMs = 0.0;
    updatePanoLabel();
}

void WalkView::updatePanoLabel()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - panoLabelMs < 250.0) return;
    panoLabelMs = now;
    auto& rec = proc.pano360;
    const auto state = rec.state();
    if (rec.finishedCount() != panoFinishedSeen)
    {
        // all frames drawn (or the drawing stopped): the folder
        panoFinishedSeen = rec.finishedCount();
        const auto problem = rec.videoProblem();
        if (rec.videoStepNow() == 3)
            say (rysum::tr("360\xc2\xb0-Video fertig f\xc3\xbcr YouTube: ") + rec.folder().getChildFile ("rysum360.mov").getFullPathName(), 7000.0);
        else if (problem.isNotEmpty())
            say (problem, 8000.0);
        else
            say (rysum::tr("360\xc2\xb0-Aufnahme fertig: ") + rec.folder().getFullPathName(), 5000.0);
        if (! testMode) rec.folder().startAsProcess();
    }
    if (state == Pano360Recorder::State::idle)
    {
        actPano.setToggleState (false, juce::dontSendNotification);
        actPano.setButtonText (juce::String::fromUTF8 ("360\xc2\xb0 Aufnahme"));
        actPano.setTooltip (panoTooltip());
        return;
    }
    actPano.setToggleState (true, juce::dontSendNotification);
    if (state == Pano360Recorder::State::drawing)
    {
        // after the recording: how far the drawing is
        const auto total = juce::jmax<int64_t> (1, rec.framesTotal());
        const int step = rec.videoStepNow();
        if (step == 1)
        {
            // ffmpeg makes the video
            actPano.setButtonText (rysum::tr("Video ") + juce::String ((int) (100 * rec.videoFramesDone() / total)) + " %");
            actPano.setTooltip (rysum::tr("ffmpeg macht das Video: ") + juce::String (rec.videoFramesDone()) + " / " + juce::String (total) + rysum::tr(" Bilder"));
            return;
        }
        if (step == 2)
        {
            actPano.setButtonText (rysum::tr("360\xc2\xb0 Metadaten"));
            actPano.setTooltip (rysum::tr("Die Metadaten f\xc3\xbcr 360\xc2\xb0 und den Raumklang werden geschrieben"));
            return;
        }
        const auto card = rec.drawingOn();
        actPano.setButtonText (juce::String::fromUTF8 ("360\xc2\xb0 ") + juce::String ((int) (100 * rec.framesDrawn() / total)) + " %");
        actPano.setTooltip (juce::String (rec.framesDrawn()) + " / " + juce::String (total) + rysum::tr(" Bilder gezeichnet, ")
                            + juce::String (rec.lastFrameMs(), 0) + rysum::tr(" ms je Bild") + " ("
                            + (card.isNotEmpty() ? rysum::tr("Grafikkarte: ") + card : rysum::tr("vom Prozessor")) + ")");
        return;
    }
    const int s = (int) rec.secondsRecorded();
    actPano.setButtonText (juce::String::fromUTF8 ("\xe2\x97\x8f 360\xc2\xb0 ") + juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2));
    if (rec.noScene() && ! panoWarned)
    {
        panoWarned = true;
        say (rysum::tr("360\xc2\xb0: der Ausgang ist Stereo \xe2\x80\x93 f\xc3\xbcr den Raumklang AmbiX, Kopfh\xc3\xb6rer oder Lautsprecher w\xc3\xa4hlen"), 6000.0);
    }
}

juce::Image WalkView::panoramaForTest (int width)
{
    panoForce = true;
    buildScene (1.0f);
    panoForce = false;
    rast.setAllAround (proc.pano360.isRecording());
    Raster face;
    return renderPanorama (panoTestJob, width, face);
}
} // namespace rysum::ui
