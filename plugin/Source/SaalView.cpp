#include "SaalView.h"
#include "RoomSim.h"
#include <algorithm>

namespace rysum::ui
{
static juce::String U (const char* s) { return juce::String::fromUTF8 (s); }

// ---------------------------------------------------------------------------
// floor plan
// ---------------------------------------------------------------------------
RoomParams SaalPlan::heardNow() const
{
    RoomParams now = proc.readParams().roomAt (shownRoom);
    const auto& h = proc.engine.heard[(size_t) shownRoom];
    now.width = h.W.load(); now.length = h.L.load(); now.height = h.H.load(); now.size = 1.0f;
    now.listenerX = h.Lx.load(); now.listenerY = h.Ly.load();
    return now;
}

SaalPlan::Frame SaalPlan::frame() const
{
    const auto base = proc.readParams().roomAt (shownRoom);  // the knobs (the plan does not jump with modulation)
    const auto r = getLocalBounds().toFloat().reduced (30.0f, 34.0f);
    Frame f;
    f.scale = juce::jmin (r.getWidth() / (base.W() * 1.35f), r.getHeight() / (base.L() * 1.35f));
    f.origin = { r.getCentreX() - base.W() * f.scale * 0.5f, r.getCentreY() + base.L() * f.scale * 0.5f };
    return f;
}

void SaalPlan::paint (juce::Graphics& g)
{
    auto& A = Assets::get();
    const auto params = proc.readParams();
    RoomParams now = heardNow();                        // as heard (modulated), from the engine
    const bool on = params.room.on;
    const auto f = frame();
    const float W = now.W(), L = now.L();

    // grid: 1 m faint, 5 m a little stronger
    {
        juce::Graphics::ScopedSaveState ss (g);
        const auto vis = getLocalBounds().toFloat();
        for (int m = -200; m <= 300; ++m)
        {
            if (f.scale * 1.0f < 5.0f && m % 5 != 0) continue;
            const float x = f.origin.x + (float) m * f.scale, y = f.origin.y - (float) m * f.scale;
            g.setColour (m % 5 == 0 ? juce::Colour (0xff1e1e1b) : juce::Colour (0xff161614));
            if (x >= vis.getX() && x <= vis.getRight()) g.drawVerticalLine (juce::roundToInt (x), vis.getY(), vis.getBottom());
            if (y >= vis.getY() && y <= vis.getBottom()) g.drawHorizontalLine (juce::roundToInt (y), vis.getX(), vis.getRight());
        }
    }
    auto P = [&] (float x, float y) { return toScreen (f, x, y); };
    const auto room = juce::Rectangle<float>::leftTopRightBottom (P (0, L).x, P (0, L).y, P (W, 0).x, P (W, 0).y);

    // mirror rooms (the image-source method: every reflection is a source in a mirrored room)
    {
        const float dash[] { 3.0f, 4.0f };
        g.setColour (col::border.withAlpha (on ? 0.8f : 0.4f));
        for (auto off : { juce::Point<float> (-room.getWidth(), 0), juce::Point<float> (room.getWidth(), 0),
                          juce::Point<float> (0, -room.getHeight()), juce::Point<float> (0, room.getHeight()) })
        {
            const auto rr = room.translated (off.x, off.y);
            juce::Path p; p.addRectangle (rr);
            juce::Path d; juce::PathStrokeType (0.8f).createDashedStroke (d, p, dash, 2);
            g.fillPath (d);
        }
    }
    // walls: a double line like an engraved plan
    g.setColour (col::ink.withAlpha (on ? 0.95f : 0.45f));
    g.drawRect (room, 2.0f);
    g.setColour (col::ink.withAlpha (on ? 0.35f : 0.15f));
    g.drawRect (room.expanded (4.0f), 0.8f);
    g.setFont (A.monoFont (10.0f));
    g.setColour (col::mute);
    g.drawText ("vorn", juce::Rectangle<float> (60, 12).withCentre ({ room.getCentreX(), room.getY() - 14 }), juce::Justification::centred);
    g.drawText (juce::String (W, 1) + U (" \xc3\x97 ") + juce::String (L, 1) + U (" \xc3\x97 ") + juce::String (now.H(), 1) + " m",
                juce::Rectangle<float> (200, 12).withCentre ({ room.getCentreX(), room.getBottom() + 14 }), juce::Justification::centred);

    const Vec3f lis = listenerPos (now);
    const auto ls = P (lis.x, lis.y);

    // pipes (top view, kept inside the walls)
    for (int s = 0; s < kNumStops; ++s)
    {
        if (! params.stopOn[(size_t) s] || params.stopRoom[(size_t) s] != shownRoom) continue;      // only the stops in this room
        const uint64_t sounding = proc.engine.soundingMask[(size_t) s].load();
        for (int k = 0; k < kNumKeys; ++k)
        {
            const auto q = roomPoint (now, proc.engine.displayPosition (s, k, params));
            const auto pt = P (q.x, q.y);
            const bool snd = (sounding >> k) & 1;
            juce::Graphics::ScopedSaveState ss (g);
            g.setOpacity (on ? (snd ? 1.0f : 0.55f) : 0.3f);
            if (snd) { g.setColour (col::white.withAlpha (0.8f)); g.drawEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (pt), 1.0f); }
            drawStopDot (g, s, pt, juce::jlimit (3.0f, 7.0f, 3.0f + q.z * 0.25f), false);
        }
    }

    // reflections of the last struck pipe: mirror images and the paths over the walls
    const int ls_ = proc.engine.lastStruckStop.load(), lk = proc.engine.lastStruckKey.load();
    if (on && ls_ >= 0 && lk >= 0 && params.stopRoom[(size_t) ls_] == shownRoom)
    {
        const auto src = roomPoint (now, proc.engine.displayPosition (ls_, lk, params));
        std::array<ImageSource, 62> ims;
        const int n = imageSources (now, src, juce::jmin (2, now.order), ims.data(), 62);
        const auto ab = BandAbsorption::from (now);
        const auto sp = P (src.x, src.y);
        for (int i = 0; i < n; ++i)
        {
            const auto& im = ims[(size_t) i];
            if (im.mz != 0) continue;                              // floor / ceiling: not visible from above
            const auto ip = P (im.pos.x, im.pos.y);
            const int hits = im.hitsWalls;
            const float gain = std::pow (1.0f - ab.wallLow, 0.5f * (float) hits) * std::pow (1.0f - now.scatter, 0.5f * (float) hits);
            g.setColour (col::rubric.brighter (0.3f).withAlpha (juce::jlimit (0.15f, 0.9f, gain * (hits == 1 ? 0.9f : 0.45f))));
            g.drawEllipse (juce::Rectangle<float> (hits == 1 ? 7.0f : 5.0f, hits == 1 ? 7.0f : 5.0f).withCentre (ip), 1.0f);
            if (hits != 1) continue;
            // first-order path: pipe -> wall -> listener (the wall point lies on the line listener -> image)
            float t = 1.0f;
            if (im.mx != 0) t = ((im.mx > 0 ? W : 0.0f) - lis.x) / (im.pos.x - lis.x);
            else            t = ((im.my > 0 ? L : 0.0f) - lis.y) / (im.pos.y - lis.y);
            const auto wp = P (lis.x + t * (im.pos.x - lis.x), lis.y + t * (im.pos.y - lis.y));
            g.setColour (col::rubric.brighter (0.2f).withAlpha (juce::jlimit (0.15f, 0.8f, gain * 0.8f)));
            g.drawLine ({ sp, wp }, 1.0f);
            g.drawLine ({ wp, ls }, 1.0f);
            const float dash[] { 2.0f, 3.0f };
            g.setColour (col::rubric.withAlpha (0.25f));
            g.drawDashedLine ({ wp, ip }, dash, 2, 0.8f);
        }
        g.setColour (col::ink.withAlpha (0.5f));
        g.drawLine ({ sp, ls }, 0.8f);
    }

    // listener: a head seen from above, facing the front
    g.setColour (col::ink);
    g.drawEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (ls), 1.4f);
    juce::Path nose;
    nose.addTriangle (ls.x - 4.0f, ls.y - 6.5f, ls.x + 4.0f, ls.y - 6.5f, ls.x, ls.y - 12.0f);
    g.fillPath (nose);

    // numbers
    g.setFont (A.monoFont (10.5f));
    g.setColour (on ? col::dim : col::faint);
    const float V = roomVolume (now);
    auto info = getLocalBounds().removeFromBottom (48).removeFromTop (32).reduced (4, 0);
    if (on && shownRoom == 0)
        g.drawFittedText ("V " + juce::String (juce::roundToInt (V)) + U (" m\xc2\xb3 \xc2\xb7 Nachhall ") + juce::String (proc.engine.roomRtLow.load(), 2)
                    + U (" s tief / ") + juce::String (proc.engine.roomRtHigh.load(), 2) + U (" s hoch \xc2\xb7 Hallradius ")
                    + juce::String (proc.engine.roomCritDist.load(), 1) + U (" m \xc2\xb7 ") + juce::String (proc.engine.erActive.load()) + U (" Kl\xc3\xa4nge mit eigenen Reflexionen"),
                    info, juce::Justification::bottomLeft, 2);
    else if (on)
    {
        const bool used = (proc.engine.roomsUsed.load() >> shownRoom) & 1;
        g.drawFittedText ("Saal " + juce::String (shownRoom + 1) + U (" \xc2\xb7 V ") + juce::String (juce::roundToInt (V)) + U (" m\xc2\xb3 \xc2\xb7 Nachhall ")
                          + juce::String (proc.engine.heard[(size_t) shownRoom].rtLow.load(), 2) + U (" s tief / ") + juce::String (proc.engine.heard[(size_t) shownRoom].rtHigh.load(), 2)
                          + U (" s hoch") + (used ? juce::String() : U (" \xc2\xb7 kein Register in diesem Saal")),
                          info, juce::Justification::bottomLeft, 2);
    }
    else
        g.drawFittedText (U ("Saal aus \xe2\x80\x93 der Hall kommt von der Impulsantwort (Seite Raum)"), info, juce::Justification::bottomLeft, 2);
    g.setFont (A.monoFont (9.5f));
    g.setColour (col::faint);
    if (on && shownRoom == 0) g.drawFittedText (proc.getRoomStatus(), getLocalBounds().removeFromBottom (15).reduced (4, 0), juce::Justification::centredLeft, 1, 0.7f);
    g.setFont (A.monoFont (10.5f));
    g.drawFittedText (U ("Draufsicht \xc2\xb7 H\xc3\xb6rer ziehen \xc2\xb7 rot: Reflexionen der zuletzt gespielten Pfeife"),
                      getLocalBounds().removeFromTop (30).reduced (4, 0), juce::Justification::topLeft, 2);
}

void SaalPlan::setListener (juce::Point<float> screen)
{
    const auto f = frame();
    const auto now = heardNow();
    const float W = now.W(), L = now.L();
    const float x = (screen.x - f.origin.x) / f.scale, y = (f.origin.y - screen.y) / f.scale;
    for (auto [key, v] : { std::pair<const char*, float> { "Lx", x / W }, { "Ly", y / L } })
        if (auto* prm = proc.apvts.getParameter (RysumProcessor::roomParamId (shownRoom, key))) prm->setValueNotifyingHost (prm->convertTo0to1 (juce::jlimit (0.0f, 1.0f, v)));
}

void SaalPlan::mouseDown (const juce::MouseEvent& e)
{
    const auto l = listenerPos (heardNow());
    const auto ls = toScreen (frame(), l.x, l.y);
    dragging = ls.getDistanceFrom (e.position) < 16.0f;
    if (dragging)
        for (auto* key : { "Lx", "Ly" }) if (auto* prm = proc.apvts.getParameter (RysumProcessor::roomParamId (shownRoom, key))) prm->beginChangeGesture();
}

void SaalPlan::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging) { setListener (e.position); repaint(); }
}

void SaalPlan::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
        for (auto* key : { "Lx", "Ly" }) if (auto* prm = proc.apvts.getParameter (RysumProcessor::roomParamId (shownRoom, key))) prm->endChangeGesture();
    dragging = false;
}

// ---------------------------------------------------------------------------
// page
// ---------------------------------------------------------------------------
ParamRow& SaalView::row (const char* id, const juce::String& name)
{
    rows.push_back (std::make_unique<ParamRow> (proc.apvts, id, name, true));
    addAndMakeVisible (*rows.back());
    return *rows.back();
}

void SaalView::setParams (std::initializer_list<std::pair<const char*, float>> values)
{
    for (auto& [id, v] : values)
        if (auto* prm = proc.apvts.getParameter (id))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 (v));
            prm->endChangeGesture();
        }
}

SaalView::SaalView (RysumProcessor& p) : proc (p)
{
    addAndMakeVisible (plan);
    onButton.setClickingTogglesState (true);
    onButton.setTooltip (U ("Simulierter Saal statt Impulsantwort: fr\xc3\xbche Reflexionen jeder Pfeife (Spiegelquellen) und Nachhall (Strahlenverfolgung). "
                            "Alles modulierbar \xe2\x80\x93 der Raum ist spielbar."));
    onAtt = std::make_unique<ButtonAtt> (proc.apvts, "simOn", onButton);
    addAndMakeVisible (onButton);
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("simOrder")))
        order.addItemList (choice->choices, 1);
    orderAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "simOrder", order);
    order.setTooltip (U ("Fr\xc3\xbche Reflexionen bis zu dieser Ordnung (1: 6 Spiegelquellen, 2: 24, 3: 62 \xe2\x80\x93 rechenintensiv)"));
    addAndMakeVisible (order);

    struct Preset { const char* name; float w, l, h, aw, af, hi, sc; };
    static const Preset rooms[] {
        { "Kapelle",     8, 14, 7,   0.05f, 0.12f, 0.4f, 0.4f },
        { "Dorfkirche", 12, 28, 11,  0.06f, 0.20f, 0.4f, 0.35f },
        { "Kathedrale", 30, 90, 32,  0.04f, 0.10f, 0.35f, 0.5f },
        { "Flur",       2.5f, 60, 3, 0.02f, 0.08f, 0.3f, 0.05f },
        { "Wuerfel", 6, 6, 6, 0.01f, 0.01f, 0.1f, 0.1f },
        { "Halle",      40, 80, 20,  0.05f, 0.05f, 0.5f, 0.2f } };
    for (auto& r : rooms)
    {
        roomPresets.push_back (std::make_unique<juce::TextButton> (juce::String::fromUTF8 (r.name)));
        auto& b = *roomPresets.back();
        const Preset pr = { nullptr, r.w, r.l, r.h, r.aw, r.af, r.hi, r.sc };
        b.onClick = [this, pr]
        {
            const int r = editRoom;
            auto id = [r] (const char* k) { return RysumProcessor::roomParamId (r, k); };
            const std::pair<juce::String, float> vals[] { { id ("W"), pr.w }, { id ("L"), pr.l }, { id ("H"), pr.h }, { id ("Size"), 1.0f }, { id ("Abs"), pr.aw },
                                                          { id ("AbsFloor"), pr.af }, { id ("High"), pr.hi }, { id ("Scatter"), pr.sc } };
            for (auto& [pid, v] : vals)
                if (auto* prm = proc.apvts.getParameter (pid)) { prm->beginChangeGesture(); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); prm->endChangeGesture(); }
        };
        addAndMakeVisible (b);
    }
    roomPresets[4]->setButtonText (U ("W\xc3\xbcrfel"));
    roomPresets[3]->setTooltip (U ("Lang und schmal, harte W\xc3\xa4nde: Flatterecho"));
    roomPresets[4]->setTooltip (U ("Kleiner, fast verlustfreier W\xc3\xbcrfel: klingt wie ein Instrument"));

    struct Mat { const char* name; float aw, af, hi; };
    static const Mat mats[] { { "Stein", 0.02f, 0.04f, 0.3f }, { "Putz", 0.05f, 0.10f, 0.4f }, { "Holz", 0.12f, 0.15f, 0.55f },
                              { "Glas", 0.04f, 0.06f, 0.1f }, { "Vorhang", 0.5f, 0.35f, 0.85f }, { "Spiegel", 0.0f, 0.0f, 0.0f } };
    for (auto& m : mats)
    {
        materials.push_back (std::make_unique<juce::TextButton> (m.name));
        auto& b = *materials.back();
        const Mat mm = m;
        b.onClick = [this, mm]
        {
            const std::pair<juce::String, float> vals[] { { RysumProcessor::roomParamId (editRoom, "Abs"), mm.aw }, { RysumProcessor::roomParamId (editRoom, "AbsFloor"), mm.af },
                                                          { RysumProcessor::roomParamId (editRoom, "High"), mm.hi } };
            for (auto& [pid, v] : vals)
                if (auto* prm = proc.apvts.getParameter (pid)) { prm->beginChangeGesture(); prm->setValueNotifyingHost (prm->convertTo0to1 (v)); prm->endChangeGesture(); }
        };
        addAndMakeVisible (b);
    }
    materials[5]->setTooltip (U ("Ein unm\xc3\xb6glicher Raum: nichts wird geschluckt (nur die Luft d\xc3\xa4mpft)"));

    for (int r = 0; r < rysum::kNumRooms; ++r)
    {
        auto id = [r] (const char* k) { return RysumProcessor::roomParamId (r, k); };
        dimsR[(size_t) r] = { &row (id ("W").toRawUTF8(), "Breite"), &row (id ("L").toRawUTF8(), U ("L\xc3\xa4nge")), &row (id ("H").toRawUTF8(), U ("H\xc3\xb6he")),
                              &row (id ("Size").toRawUTF8(), U ("Gr\xc3\xb6\xc3\x9f" "e (alle Ma\xc3\x9f" "e)")) };
        surfR[(size_t) r] = { &row (id ("Abs").toRawUTF8(), U ("Absorption W\xc3\xa4nde")), &row (id ("AbsFloor").toRawUTF8(), "Absorption Boden/Decke"),
                              &row (id ("High").toRawUTF8(), U ("H\xc3\xb6hend\xc3\xa4mpfung")), &row (id ("Scatter").toRawUTF8(), "Streuung") };
        lisR[(size_t) r] = { &row (id ("Lx").toRawUTF8(), U ("H\xc3\xb6rer quer (links \xe2\x86\x92 rechts)")), &row (id ("Ly").toRawUTF8(), U ("H\xc3\xb6rer l\xc3\xa4ngs (hinten \xe2\x86\x92 vorn)")) };
    }
    gainRows = { &row ("simEr", U ("Fr\xc3\xbche Reflexionen")), &row ("simTail", "Nachhall") };
    // rooms: tabs to edit one, and which stop sounds where
    for (int r = 0; r < rysum::kNumRooms; ++r)
    {
        auto& b = roomTabs[(size_t) r];
        b.setButtonText ("Saal " + juce::String (r + 1));
        b.setTooltip (U ("Diesen Saal bearbeiten und anzeigen (Grundriss, Ma\xc3\x9f" "e, Oberfl\xc3\xa4" "chen, H\xc3\xb6rer)"));
        b.onClick = [this, r] { selectRoom (r); };
        addAndMakeVisible (b);
    }
    for (int slot = 0; slot <= kNumStops; ++slot)
    {
        auto& b = assign[(size_t) slot];
        b.setTooltip (slot < kNumStops ? prettyLabel (kStops[(size_t) kDisplayOrder[(size_t) slot]].label) + U (": in welchem Saal (Klick: n\xc3\xa4" "chster, Rechtsklick: w\xc3\xa4hlen)")
                                       : U ("Eingang (Audio): in welchem Saal"));
        b.onClick = [this, slot]
        {
            if (juce::ModifierKeys::currentModifiers.isPopupMenu()) { showAssignMenu (slot); return; }
            if (auto* prm = proc.apvts.getParameter (assignId (slot)))
            {
                const int cur = juce::roundToInt (prm->convertFrom0to1 (prm->getValue()));
                prm->beginChangeGesture(); prm->setValueNotifyingHost (prm->convertTo0to1 ((float) ((cur + 1) % rysum::kNumRooms))); prm->endChangeGesture();
            }
        };
        addAndMakeVisible (b);
    }
    selectRoom (0);

    if (auto* tm = proc.apvts.getParameter ("simTailModel"))
    {
        tailAtt = std::make_unique<juce::ParameterAttachment> (*tm, [this] (float v)
        {
            tailConv.setToggleState (v < 0.5f, juce::dontSendNotification);
            tailFdn.setToggleState (v >= 0.5f, juce::dontSendNotification);
        });
        tailAtt->sendInitialUpdate();
    }
    tailConv.onClick = [this] { if (tailAtt) tailAtt->setValueAsCompleteGesture (0.0f); };
    tailFdn.onClick = [this] { if (tailAtt) tailAtt->setValueAsCompleteGesture (1.0f); };
    tailConv.setTooltip (U ("Nachhall als Faltung: eine aus der Strahlenverfolgung synthetisierte Raumantwort \xe2\x80\x93 dicht, ohne Eigenmoden."));
    tailFdn.setTooltip (U ("Nachhall als R\xc3\xbc" "ckkopplungsnetz (FDN, 16 Leitungen, langsam moduliert) \xe2\x80\x93 das fr\xc3\xbchere Modell: "
                           "etwas lebendiger, weniger Rechenzeit. Abklingzeit ebenfalls aus der Strahlenverfolgung."));
    addAndMakeVisible (tailConv);
    addAndMakeVisible (tailFdn);

    // ---- Lucier mode
    lucOn.setClickingTogglesState (true);
    lucOn.setTooltip (U ("Alvin Lucier, \xe2\x80\x9eI am sitting in a room\xe2\x80\x9c (1969): eine Aufnahme wird in den Raum gespielt und neu aufgenommen, "
                         "immer wieder, bis nur die Resonanzen des Raums \xc3\xbc" "brig sind. Hier live: der ganze Klang geht n-mal durch den Raum."));
    lucOnAtt = std::make_unique<ButtonAtt> (proc.apvts, "lucOn", lucOn);
    addAndMakeVisible (lucOn);
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("lucSource")))
        lucSource.addItemList (choice->choices, 1);
    lucSourceAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "lucSource", lucSource);
    lucSource.setTooltip (U ("Welcher Raum singt: der simulierte Saal (seine Eigenmoden aus den Ma\xc3\x9f" "en) oder die Impulsantwort von der Seite Raum "
                             "(z. B. eine Aufnahme aus der Kirche in Rysum)."));
    addAndMakeVisible (lucSource);
    lucRows = { &row ("lucGen", "Generation"), &row ("lucGrow", U ("Wachsen (Generationen pro Minute)")),
                &row ("lucTime", "Nachklingen je Durchgang"), &row ("lucMix", U ("Anteil (0 = nur Orgel, 100 = nur Raum)")) };
    lucRows[0]->setTooltip (U ("Wie oft der Klang durch den Raum gegangen ist. Kommazahlen blenden zwischen zwei Durchg\xc3\xa4ngen. Modulierbar."));
    lucRows[1]->setTooltip (U ("Die Generation steigt von selbst, solange Lucier an ist \xe2\x80\x93 \xc3\xbc" "ber ein ganzes St\xc3\xbc" "ck hinweg. 0 = steht."));
    lucRows[2]->setTooltip (U ("Wie lange jeder Durchgang nachklingt. Kurz: die Resonanzen kommen schnell und breiter; lang: langsam und ganz schmal."));
    lucRestart.setTooltip (U ("Das Gewachsene zur\xc3\xbc" "cksetzen: wieder bei der eingestellten Generation anfangen."));
    lucRestart.onClick = [this] { proc.resetLucierGrowth(); };
    addAndMakeVisible (lucRestart);

    // ---- Lucier as a piece
    if (auto* m = proc.apvts.getParameter ("lucMode"))
    {
        lucModeAtt = std::make_unique<juce::ParameterAttachment> (*m, [this] (float) { updateLucierMode(); });
        lucModeAtt->sendInitialUpdate();
    }
    lucModePiece.onClick = [this] { if (lucModeAtt) lucModeAtt->setValueAsCompleteGesture (0.0f); };
    lucModeLive.onClick = [this] { if (lucModeAtt) lucModeAtt->setValueAsCompleteGesture (1.0f); };
    lucModePiece.setTooltip (U ("Wie Lucier: aufnehmen (Stimme, Orgel, beides), dann wird die Aufnahme wieder und wieder in den Raum gespielt "
                                "und neu aufgenommen \xe2\x80\x93 Generation f\xc3\xbcr Generation, jede so lang wie die Aufnahme. Das St\xc3\xbc" "ck spielt sich selbst."));
    lucModeLive.setTooltip (U ("Alles, was gerade klingt, geht sofort n-mal durch den Raum (zum Spielen; klingt aus, wenn du aufh\xc3\xb6rst)."));
    addAndMakeVisible (lucModePiece);
    addAndMakeVisible (lucModeLive);
    lucRec.setTooltip (U ("Aufnahme starten. Nochmal klicken: Aufnahme beenden \xe2\x80\x93 dann beginnt das St\xc3\xbc" "ck: erst die Aufnahme selbst, "
                          "dann Generation 1, 2, 3 \xe2\x80\xa6 (h\xc3\xb6" "chstens 5 Minuten Aufnahme; jede Aufnahme wird in Dokumente/Rysum/Lucier gespeichert)."));
    lucRec.onClick = [this]
    {
        auto& pc = proc.getLucierPiece();
        if (pc.state.load() == rysum::LucierPiece::stRecording) proc.lucierStopRecording();
        else proc.lucierRecord();
    };
    lucPlay.setTooltip (U ("Das St\xc3\xbc" "ck mit der letzten Aufnahme von vorn spielen"));
    lucPlay.onClick = [this] { proc.lucierPlay(); };
    lucStop.setTooltip (U ("Anhalten (Aufnahme bzw. St\xc3\xbc" "ck)"));
    lucStop.onClick = [this] { proc.lucierStop(); };
    lucLoad.setTooltip (U ("Eine eigene Aufnahme (WAV/AIFF/FLAC, bis 20 Minuten) als Ausgang des St\xc3\xbc" "cks"));
    lucLoad.onClick = [this]
    {
        lucChooser = std::make_unique<juce::FileChooser> (U ("Aufnahme f\xc3\xbcr das Lucier-St\xc3\xbc" "ck"),
                                                          RysumProcessor::getPatchFolder().getParentDirectory().getChildFile ("Lucier"), "*.wav;*.aif;*.aiff;*.flac");
        lucChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            if (! fc.getResult().existsAsFile()) return;
            juce::String err;
            if (! proc.loadLucierRecording (fc.getResult(), err))
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Lucier", fc.getResult().getFileName() + ": " + err);
            repaint (lucBox);
        });
    };
    lucExport.setTooltip (U ("Das ganze St\xc3\xbc" "ck (Aufnahme und alle Generationen) in eine WAV-Datei rechnen \xe2\x80\x93 im Hintergrund, schneller als Echtzeit"));
    lucExport.onClick = [this]
    {
        lucChooser = std::make_unique<juce::FileChooser> (U ("St\xc3\xbc" "ck speichern"),
                                                          RysumProcessor::getPatchFolder().getParentDirectory().getChildFile ("Lucier").getChildFile (U ("Lucier-St\xc3\xbc" "ck.wav")), "*.wav");
        lucChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                                 [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            proc.exportLucierPiece (f.withFileExtension ("wav"));
        });
    };
    for (auto* b : { &lucRec, &lucPlay, &lucStop, &lucLoad, &lucExport }) addAndMakeVisible (*b);
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("lucRecSrc")))
        lucRecSrc.addItemList (choice->choices, 1);
    lucRecSrcAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "lucRecSrc", lucRecSrc);
    lucRecSrc.setTooltip (U ("Was aufgenommen wird: alles, was Rysum gerade spielt, oder nur der Audio-Eingang (Seite Raum)"));
    addAndMakeVisible (lucRecSrc);
    pieceRows = { &row ("lucGen", U ("Generationen (dann ist das St\xc3\xbc" "ck aus)")), &row ("lucGap", "Pause zwischen den Generationen"),
                  &row ("lucMix", U ("Anteil (0 = nur live, 100 = nur Wiedergabe)")) };
    updateLucierMode();
}

bool SaalView::pieceMode() const { return proc.isLucierPieceMode(); }

void SaalView::updateLucierMode()
{
    const bool piece = pieceMode();
    lucModePiece.setToggleState (piece, juce::dontSendNotification);
    lucModeLive.setToggleState (! piece, juce::dontSendNotification);
    for (auto* r : lucRows) if (r) r->setVisible (! piece);
    lucRestart.setVisible (! piece);
    for (auto* r : pieceRows) if (r) r->setVisible (piece);
    for (auto* b : { &lucRec, &lucPlay, &lucStop, &lucLoad, &lucExport }) b->setVisible (piece);
    lucRecSrc.setVisible (piece);
    resized();
    repaint (lucBox);
}

juce::String SaalView::pieceStatus() const
{
    auto& pc = const_cast<RysumProcessor&> (proc).getLucierPiece();
    auto mmss = [] (double s) { const int t = juce::jmax (0, (int) s); return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2); };
    const double sr = pc.sampleRate();
    const int st = pc.state.load();
    const double recLen = (double) pc.getRecording().size() / juce::jmax (1.0, pc.recordingRate());
    if (st == rysum::LucierPiece::stRecording) return U ("Aufnahme l\xc3\xa4uft  ") + mmss ((double) pc.recPos.load() / sr);
    juce::String s;
    if (st == rysum::LucierPiece::stPlaying)
    {
        const int g = pc.playingGen.load(), last = pc.lastGen.load();
        const double pos = (double) pc.playPos.load() / sr;
        const double gap = proc.apvts.getRawParameterValue ("lucGap")->load();
        const double left = (recLen + gap) * (double) (last - g) + juce::jmax (0.0, recLen - pos);
        s << mmss (pos) << " / " << mmss (recLen) << U (" \xc2\xb7 noch ") << mmss (left);
        if (pc.computingGen.load() >= 0) s << U (" \xc2\xb7 rechne ") << pc.computingGen.load();
    }
    else if (st == rysum::LucierPiece::stFinished) s = U ("Das St\xc3\xbc" "ck ist zu Ende.");
    else if (recLen > 0.0) s = U ("Aufnahme ") + mmss (recLen) + U (" \xc2\xb7 bereit");
    else s = U ("Noch keine Aufnahme");
    if (pc.exportProgress.load() >= 0.0f) s << U (" \xc2\xb7 WAV ") << juce::roundToInt (pc.exportProgress.load() * 100.0f) << " %";
    return s;
}

void SaalView::resized()
{
    auto r = getLocalBounds().reduced (28, 0).withTrimmedTop (18).withTrimmedBottom (14);
    auto right = r.removeFromRight (404);
    r.removeFromRight (24);
    lucBox = r.removeFromRight (300);
    r.removeFromRight (20);
    {
        // rooms: tabs, and the stops' rooms
        roomsArea = r.removeFromTop (78);
        auto t = roomsArea.withTrimmedTop (4).removeFromTop (24);
        t.removeFromLeft (64);
        const int tw = (t.getWidth() - 3 * 4) / rysum::kNumRooms;
        for (auto& b : roomTabs) { b.setBounds (t.removeFromLeft (tw)); t.removeFromLeft (4); }
        auto a = roomsArea.withTrimmedTop (50).removeFromTop (24);
        a.removeFromLeft (64);
        const int aw = (a.getWidth() - kNumStops * 3) / (kNumStops + 1);
        for (auto& b : assign) { b.setBounds (a.removeFromLeft (aw)); a.removeFromLeft (3); }
    }
    planBox = r;
    plan.setBounds (r);
    {
        auto b = lucBox.withTrimmedTop (32);
        auto t = b.removeFromTop (26);
        lucOn.setBounds (t.removeFromLeft (100));
        t.removeFromLeft (6);
        lucSource.setBounds (t);
        b.removeFromTop (6);
        auto m = b.removeFromTop (24);
        const int mw = (m.getWidth() - 6) / 2;
        lucModePiece.setBounds (m.removeFromLeft (mw));
        m.removeFromLeft (6);
        lucModeLive.setBounds (m);
        b.removeFromTop (8);
        if (pieceMode())
        {
            auto t2 = b.removeFromTop (26);
            lucRec.setBounds (t2.removeFromLeft (120));
            t2.removeFromLeft (6);
            lucPlay.setBounds (t2.removeFromLeft (110));
            t2.removeFromLeft (6);
            lucStop.setBounds (t2);
            b.removeFromTop (4);
            lucGenArea = b.removeFromTop (34);
            lucStatusArea = b.removeFromTop (16);
            lucCurveArea = b.removeFromTop (96);
            lucPeakArea = b.removeFromTop (34);
            for (auto* x : pieceRows) if (x) x->setBounds (b.removeFromTop (30).withTrimmedBottom (2));
            b.removeFromTop (2);
            lucRecSrc.setBounds (b.removeFromTop (24));
            b.removeFromTop (6);
            auto t3 = b.removeFromTop (24);
            lucLoad.setBounds (t3.removeFromLeft ((t3.getWidth() - 6) / 2));
            t3.removeFromLeft (6);
            lucExport.setBounds (t3);
            b.removeFromTop (8);
            lucNoteArea = b;
        }
        else
        {
            lucGenArea = b.removeFromTop (34);
            lucStatusArea = {};
            lucCurveArea = b.removeFromTop (110);
            lucPeakArea = b.removeFromTop (34);
            b.removeFromTop (4);
            for (auto* x : lucRows) x->setBounds (b.removeFromTop (30).withTrimmedBottom (2));
            b.removeFromTop (4);
            lucRestart.setBounds (b.removeFromTop (24).withWidth (180));
            b.removeFromTop (8);
            lucNoteArea = b;
        }
    }

    constexpr int kRow = 28, kTitle = 32;
    auto take = [&] (int h) { return right.removeFromTop (h); };
    auto stack = [&] (juce::Rectangle<int>& b, std::array<ParamRow*, 4>& rs) { for (auto* x : rs) x->setBounds (b.removeFromTop (kRow).withTrimmedBottom (2)); };

    saalBox = take (kTitle + 28 + 4 + 50);
    {
        auto b = saalBox.withTrimmedTop (kTitle);
        auto t = b.removeFromTop (26);
        onButton.setBounds (t.removeFromLeft (120));
        t.removeFromLeft (8);
        order.setBounds (t);
        b.removeFromTop (4);
        for (int rr = 0; rr < 2; ++rr)
        {
            auto line = b.removeFromTop (24);
            const int w = (line.getWidth() - 8) / 3;
            for (int c = 0; c < 3; ++c) { roomPresets[(size_t) (rr * 3 + c)]->setBounds (line.removeFromLeft (w)); line.removeFromLeft (4); }
        }
    }
    right.removeFromTop (8);
    dimBox = take (kTitle + 4 * kRow);
    for (auto& d : dimsR) { auto b = dimBox.withTrimmedTop (kTitle); stack (b, d); }
    right.removeFromTop (8);
    surfBox = take (kTitle + 50 + 4 + 4 * kRow);
    {
        auto b = surfBox.withTrimmedTop (kTitle);
        for (int rr = 0; rr < 2; ++rr)
        {
            auto line = b.removeFromTop (24);
            const int w = (line.getWidth() - 8) / 3;
            for (int c = 0; c < 3; ++c) { materials[(size_t) (rr * 3 + c)]->setBounds (line.removeFromLeft (w)); line.removeFromLeft (4); }
        }
        b.removeFromTop (6);
        for (auto& sf : surfR) { auto bb = b; stack (bb, sf); }
    }
    right.removeFromTop (8);
    levelBox = take (kTitle + 4 * kRow);
    {
        auto b = levelBox.withTrimmedTop (kTitle);
        for (auto& l2 : lisR) { auto bb = b; for (auto* x : l2) x->setBounds (bb.removeFromTop (kRow).withTrimmedBottom (2)); }
        b.removeFromTop (2 * kRow);
        for (auto* x : gainRows) x->setBounds (b.removeFromTop (kRow).withTrimmedBottom (2));
    }
    tailFdn.setBounds (levelBox.getRight() - 52, levelBox.getY() + 9, 52, 20);
    tailConv.setBounds (tailFdn.getX() - 4 - 66, levelBox.getY() + 9, 66, 20);
}

void SaalView::selectRoom (int r)
{
    editRoom = juce::jlimit (0, rysum::kNumRooms - 1, r);
    for (int q = 0; q < rysum::kNumRooms; ++q)
    {
        roomTabs[(size_t) q].setToggleState (q == editRoom, juce::dontSendNotification);
        for (auto* x : dimsR[(size_t) q]) if (x) x->setVisible (q == editRoom);
        for (auto* x : surfR[(size_t) q]) if (x) x->setVisible (q == editRoom);
        for (auto* x : lisR[(size_t) q]) if (x) x->setVisible (q == editRoom);
    }
    plan.repaint();
    repaint();
}

juce::String SaalView::assignId (int slot) const
{
    return slot < kNumStops ? RysumProcessor::stopRoomParamId (kDisplayOrder[(size_t) slot]) : juce::String ("simRoomIn");
}

void SaalView::showAssignMenu (int slot)
{
    juce::PopupMenu m;
    for (int r = 0; r < rysum::kNumRooms; ++r) m.addItem (r + 1, "Saal " + juce::String (r + 1));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&assign[(size_t) slot]), [this, slot] (int res)
    {
        if (res <= 0) return;
        if (auto* prm = proc.apvts.getParameter (assignId (slot)))
        { prm->beginChangeGesture(); prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (res - 1))); prm->endChangeGesture(); }
    });
}

void SaalView::refresh()
{
    for (int slot = 0; slot <= kNumStops; ++slot)
    {
        const int r = juce::roundToInt (proc.apvts.getRawParameterValue (assignId (slot))->load());
        if (r != shownAssign[(size_t) slot] || assign[(size_t) slot].getButtonText().isEmpty())
        {
            shownAssign[(size_t) slot] = r;
            assign[(size_t) slot].setButtonText (juce::String (r + 1));
            assign[(size_t) slot].setToggleState (r == editRoom, juce::dontSendNotification);
            repaint (roomsArea);
        }
        assign[(size_t) slot].setToggleState (r == editRoom, juce::dontSendNotification);
    }
    plan.repaint();
    for (auto& r : rows) r->repaint();
    const auto st = proc.getRoomStatus();
    if (st != shownStatus) { shownStatus = st; repaint (saalBox); }
    const auto& lv = proc.getLucierView();
    const float lvl = proc.engine.lucierLevel.load();
    if (lucModePiece.getToggleState() != pieceMode()) updateLucierMode();      // (changed by a patch, the record button, automation)
    const auto ps = pieceMode() ? pieceStatus() : juce::String();
    {
        auto& pc = proc.getLucierPiece();
        const bool rec = pc.state.load() == rysum::LucierPiece::stRecording;
        lucRec.setToggleState (rec, juce::dontSendNotification);
        lucRec.setButtonText (rec ? U ("Aufnahme fertig") : U ("Aufnehmen"));
        lucPlay.setEnabled (! pc.getRecording().empty() && ! rec);
        lucPlay.setToggleState (pc.state.load() == rysum::LucierPiece::stPlaying, juce::dontSendNotification);
        lucExport.setEnabled (! pc.getRecording().empty() && pc.exportProgress.load() < 0.0f);
    }
    if (ps != shownPieceStatus) { shownPieceStatus = ps; repaint (lucBox); }
    if (lv.version != shownLucVersion || std::abs (proc.getLucierGeneration() - shownLucGen) > 0.004f || std::abs (lvl - shownLucLevel) > 0.01f)
    {
        shownLucVersion = lv.version; shownLucGen = proc.getLucierGeneration(); shownLucLevel = lvl;
        repaint (lucBox);
    }
}

void SaalView::paintLucier (juce::Graphics& g)
{
    auto& A = Assets::get();
    const auto& lv = proc.getLucierView();
    const bool on = proc.apvts.getRawParameterValue ("lucOn")->load() > 0.5f;
    drawSectionTitle (g, lucBox, "Lucier", U ("\xe2\x80\x9eIch sitze in einem Raum\xe2\x80\x9c"));
    const bool piece = pieceMode();
    auto& pc = proc.getLucierPiece();
    if (piece)
    {
        // generation, large: which one is playing, of how many
        auto r = lucGenArea;
        const int st = pc.state.load();
        const bool playingNow = st == rysum::LucierPiece::stPlaying || st == rysum::LucierPiece::stFinished;
        g.setColour (playingNow ? col::ink : col::mute);
        g.setFont (A.romanFont (26.0f));
        const juce::String big = st == rysum::LucierPiece::stRecording ? U ("Aufnahme")
                               : playingNow ? juce::String (pc.playingGen.load()) + " / " + juce::String (pc.lastGen.load()) : U ("\xe2\x80\x93");
        g.drawText (big, r.removeFromLeft (150), juce::Justification::centredLeft);
        g.setFont (A.monoFont (11.0f));
        g.setColour (st == rysum::LucierPiece::stRecording ? col::rubric.brighter (0.5f) : col::dim);
        g.drawText (st == rysum::LucierPiece::stRecording ? U ("sprich, spiel \xe2\x80\xa6") : playingNow && pc.playingGen.load() == 0 ? U ("die Aufnahme selbst") : U ("Generation"),
                    r, juce::Justification::centredLeft);
        g.setFont (A.monoFont (10.5f));
        g.setColour (col::mute);
        g.drawFittedText (shownPieceStatus, lucStatusArea, juce::Justification::centredLeft, 1, 0.8f);
    }
    // generation, large
    if (! piece)
    {
        auto r = lucGenArea;
        g.setColour (on ? col::ink : col::mute);
        g.setFont (A.romanFont (26.0f));
        const float gen = proc.getLucierGeneration();
        g.drawText (juce::String (gen, gen < 10.0f ? 2 : 1).replace (".", ","), r.removeFromLeft (110), juce::Justification::centredLeft);
        g.setFont (A.monoFont (11.0f));
        g.setColour (col::dim);
        g.drawText (gen < 1.5f ? U ("Durchgang \xc2\xb7 der Raum, wie er ist") : U ("Durchg\xc3\xa4nge durch den Raum"), r, juce::Justification::centredLeft);
    }
    // the room's response after n passes (log frequency, 50 dB)
    {
        auto r = lucCurveArea.toFloat().reduced (0.0f, 4.0f);
        g.setColour (col::line);
        g.drawRect (r, 1.0f);
        const float lo = rysum::LucierResult::kCurveLo, hi = rysum::LucierResult::kCurveHi;
        auto xOf = [&] (float f) { return r.getX() + r.getWidth() * std::log (f / lo) / std::log (hi / lo); };
        g.setFont (A.monoFont (9.0f));
        for (float f : { 100.0f, 1000.0f, 10000.0f })
        {
            g.setColour (col::line);
            g.drawVerticalLine ((int) xOf (f), r.getY(), r.getBottom());
            g.setColour (col::faint);
            g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + " kHz" : juce::String ((int) f) + " Hz",
                        juce::Rectangle<float> (xOf (f) + 3.0f, r.getBottom() - 14.0f, 50.0f, 12.0f), juce::Justification::centredLeft);
        }
        if (lv.curve.size() > 1)
        {
            juce::Path p;
            const int n = (int) lv.curve.size();
            for (int i = 0; i < n; ++i)
            {
                const float x = r.getX() + r.getWidth() * (float) i / (float) (n - 1);
                const float y = r.getY() + 3.0f + (r.getHeight() - 6.0f) * juce::jlimit (0.0f, 1.0f, -lv.curve[(size_t) i] / 50.0f);
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            g.setColour (on ? col::rubric.brighter (0.3f) : col::mute);
            g.strokePath (p, juce::PathStrokeType (1.2f));
            for (size_t i = 0; i < lv.peaks.size(); ++i)
            {
                const float x = xOf (lv.peaks[i]);
                g.setColour ((on ? col::ink : col::mute).withAlpha (i == 0 ? 1.0f : 0.6f));
                g.fillEllipse (x - 2.5f, r.getY() + 2.0f, 5.0f, 5.0f);
            }
        }
        // how loud the room sings right now
        const float lvl = juce::jlimit (0.0f, 1.0f, piece ? pc.level.load() : proc.engine.lucierLevel.load());
        g.setColour (col::ink.withAlpha (0.8f));
        g.fillRect (r.getRight() - 3.0f, r.getBottom() - r.getHeight() * lvl, 3.0f, r.getHeight() * lvl);
    }
    // the strongest resonances, as notes
    {
        auto r = lucPeakArea;
        g.setFont (A.monoFont (11.0f));
        g.setColour (col::dim);
        juce::String s;
        static const char* names[] { "c", "cis", "d", "es", "e", "f", "fis", "g", "gis", "a", "b", "h" };
        for (size_t i = 0; i < std::min<size_t> (4, lv.peaks.size()); ++i)
        {
            const float f = lv.peaks[i];
            const float m = 69.0f + 12.0f * std::log2 (f / 444.5f);
            const int mi = juce::roundToInt (m);
            const int ct = juce::roundToInt ((m - (float) mi) * 100.0f);
            s << (i > 0 ? U (" \xc2\xb7 ") : juce::String()) << juce::String (juce::roundToInt (f)) << " Hz " << names[((mi % 12) + 12) % 12]
              << (ct >= 0 ? "+" : "") << ct;
        }
        if (s.isEmpty()) s = on ? U ("berechne \xe2\x80\xa6") : U ("aus");
        if (lv.status.isNotEmpty()) { s = lv.status; g.setColour (col::rubric.brighter (0.4f)); }
        g.drawFittedText (s, r.removeFromTop (17), juce::Justification::centredLeft, 1, 0.8f);
        g.setFont (A.monoFont (10.0f));
        g.setColour (col::mute);
        g.drawFittedText (lv.what, r, juce::Justification::centredLeft, 1, 0.8f);
    }
    g.setFont (A.sansFont (12.0f));
    g.setColour (col::mute);
    if (piece && lucNoteArea.getHeight() > 20)
        g.drawFittedText (U ("Aufnehmen, sprechen oder spielen, nochmal klicken: die Aufnahme kommt aus dem Raum zur\xc3\xbc" "ck, wird neu aufgenommen, "
                             "wieder abgespielt \xe2\x80\xa6 jede Generation so lang wie die Aufnahme, auf gleiche Lautst\xc3\xa4rke gebracht. "
                             "1 Minute Aufnahme \xc3\x97 32 Generationen \xe2\x89\x88 33 Minuten."),
                          lucNoteArea, juce::Justification::topLeft, juce::jmax (1, lucNoteArea.getHeight() / 16));
    else if (lucNoteArea.getHeight() > 20)
        g.drawFittedText (U ("Jeder Durchgang filtert den Klang noch einmal mit dem Raum: seine st\xc3\xa4rksten Resonanzen bleiben, alles andere verschwindet. "
                             "Lautsprecher und Mikrofon sind wie bei Lucier Teil jedes Durchgangs. Die Resonanzen werden so laut gehalten wie die Orgel."),
                          lucNoteArea, juce::Justification::topLeft, juce::jmax (1, lucNoteArea.getHeight() / 16));
}

void SaalView::paint (juce::Graphics& g)
{
    auto& A = Assets::get();
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.setOpacity (0.05f);
        drawCover (g, A.wall, getLocalBounds().toFloat(), 0.5f);
    }
    const bool on = proc.readParams().room.on;
    drawSectionTitle (g, saalBox, "Saal", on ? U ("simuliert \xc2\xb7 statt der Impulsantwort") : juce::String ("aus"));
    drawSectionTitle (g, dimBox, U ("Ma\xc3\x9f" "e \xc2\xb7 Saal ") + juce::String (editRoom + 1), "modulierbar");
    {
        // the rooms strip: labels
        g.setFont (A.monoFont (10.5f));
        g.setColour (col::mute);
        auto lbl = roomsArea.withTrimmedTop (4).removeFromTop (24).removeFromLeft (60);
        g.drawText (U ("R\xc3\xa4ume"), lbl, juce::Justification::centredLeft);
        auto lbl2 = roomsArea.withTrimmedTop (50).removeFromTop (24).removeFromLeft (60);
        g.drawText (U ("im Saal"), lbl2, juce::Justification::centredLeft);
        const char* shortNames[] { "P 16", "P 8", "Ged", "Okt 4", "Okt 2", "Sesq", "Mix", "Tr", "Ein" };
        g.setFont (A.monoFont (9.5f));
        g.setColour (col::faint);
        for (int slot = 0; slot <= kNumStops; ++slot)
        {
            const auto b = assign[(size_t) slot].getBounds();
            g.drawText (shortNames[slot], b.withY (b.getY() - 16).withHeight (14), juce::Justification::centred);
        }
    }
    drawSectionTitle (g, surfBox, U ("Oberfl\xc3\xa4" "chen \xc2\xb7 Saal ") + juce::String (editRoom + 1), "Material setzt die Absorption");
    drawSectionTitle (g, levelBox, U ("H\xc3\xb6rer \xc2\xb7 Pegel"));
    g.setColour (col::mute);
    g.setFont (A.monoFont (11.0f));
    g.drawText ("Nachhall:", juce::Rectangle<int> (tailConv.getX() - 70, tailConv.getY(), 64, tailConv.getHeight()), juce::Justification::centredRight);
    paintLucier (g);
}
} // namespace rysum::ui
