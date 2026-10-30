#include "SaalView.h"
#include "RoomSim.h"

namespace rysum::ui
{
static juce::String U (const char* s) { return juce::String::fromUTF8 (s); }

// ---------------------------------------------------------------------------
// floor plan
// ---------------------------------------------------------------------------
SaalPlan::Frame SaalPlan::frame() const
{
    const auto base = proc.readParams().room;           // the knobs (the plan does not jump with modulation)
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
    RoomParams now = params.room;                        // as heard (modulated), from the engine
    now.width = proc.engine.roomW.load(); now.length = proc.engine.roomL.load(); now.height = proc.engine.roomH.load(); now.size = 1.0f;
    now.listenerX = proc.engine.roomLx.load(); now.listenerY = proc.engine.roomLy.load();
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
        if (! params.stopOn[(size_t) s]) continue;
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
    if (on && ls_ >= 0 && lk >= 0)
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
    if (on)
        g.drawFittedText ("V " + juce::String (juce::roundToInt (V)) + U (" m\xc2\xb3 \xc2\xb7 Nachhall ") + juce::String (proc.engine.roomRtLow.load(), 2)
                    + U (" s tief / ") + juce::String (proc.engine.roomRtHigh.load(), 2) + U (" s hoch \xc2\xb7 Hallradius ")
                    + juce::String (proc.engine.roomCritDist.load(), 1) + U (" m \xc2\xb7 ") + juce::String (proc.engine.erActive.load()) + U (" Kl\xc3\xa4nge mit eigenen Reflexionen"),
                    info, juce::Justification::bottomLeft, 2);
    else
        g.drawFittedText (U ("Saal aus \xe2\x80\x93 der Hall kommt von der Impulsantwort (Seite Raum)"), info, juce::Justification::bottomLeft, 2);
    g.setFont (A.monoFont (9.5f));
    g.setColour (col::faint);
    if (on) g.drawFittedText (proc.getRoomStatus(), getLocalBounds().removeFromBottom (15).reduced (4, 0), juce::Justification::centredLeft, 1, 0.7f);
    g.setFont (A.monoFont (10.5f));
    g.drawFittedText (U ("Draufsicht \xc2\xb7 H\xc3\xb6rer ziehen \xc2\xb7 rot: Reflexionen der zuletzt gespielten Pfeife"),
                      getLocalBounds().removeFromTop (30).reduced (4, 0), juce::Justification::topLeft, 2);
}

void SaalPlan::setListener (juce::Point<float> screen)
{
    const auto f = frame();
    const float W = proc.engine.roomW.load(), L = proc.engine.roomL.load();
    const float x = (screen.x - f.origin.x) / f.scale, y = (f.origin.y - screen.y) / f.scale;
    for (auto [id, v] : { std::pair<const char*, float> { "simLx", x / W }, { "simLy", y / L } })
        if (auto* prm = proc.apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (juce::jlimit (0.0f, 1.0f, v)));
}

void SaalPlan::mouseDown (const juce::MouseEvent& e)
{
    RoomParams now = proc.readParams().room;
    now.width = proc.engine.roomW.load(); now.length = proc.engine.roomL.load(); now.height = proc.engine.roomH.load(); now.size = 1.0f;
    now.listenerX = proc.engine.roomLx.load(); now.listenerY = proc.engine.roomLy.load();
    const auto l = listenerPos (now);
    const auto ls = toScreen (frame(), l.x, l.y);
    dragging = ls.getDistanceFrom (e.position) < 16.0f;
    if (dragging)
        for (auto* id : { "simLx", "simLy" }) if (auto* prm = proc.apvts.getParameter (id)) prm->beginChangeGesture();
}

void SaalPlan::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging) { setListener (e.position); repaint(); }
}

void SaalPlan::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
        for (auto* id : { "simLx", "simLy" }) if (auto* prm = proc.apvts.getParameter (id)) prm->endChangeGesture();
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
            setParams ({ { "simW", pr.w }, { "simL", pr.l }, { "simH", pr.h }, { "simSize", 1.0f }, { "simAbs", pr.aw }, { "simAbsFloor", pr.af },
                         { "simHigh", pr.hi }, { "simScatter", pr.sc } });
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
        b.onClick = [this, mm] { setParams ({ { "simAbs", mm.aw }, { "simAbsFloor", mm.af }, { "simHigh", mm.hi } }); };
        addAndMakeVisible (b);
    }
    materials[5]->setTooltip (U ("Ein unm\xc3\xb6glicher Raum: nichts wird geschluckt (nur die Luft d\xc3\xa4mpft)"));

    dims = { &row ("simW", "Breite"), &row ("simL", U ("L\xc3\xa4nge")), &row ("simH", U ("H\xc3\xb6he")), &row ("simSize", U ("Gr\xc3\xb6\xc3\x9f" "e (alle Ma\xc3\x9f" "e)")) };
    surf = { &row ("simAbs", U ("Absorption W\xc3\xa4nde")), &row ("simAbsFloor", "Absorption Boden/Decke"),
             &row ("simHigh", U ("H\xc3\xb6hend\xc3\xa4mpfung")), &row ("simScatter", "Streuung") };
    levels = { &row ("simLx", U ("H\xc3\xb6rer quer (links \xe2\x86\x92 rechts)")), &row ("simLy", U ("H\xc3\xb6rer l\xc3\xa4ngs (hinten \xe2\x86\x92 vorn)")),
               &row ("simEr", U ("Fr\xc3\xbche Reflexionen")), &row ("simTail", "Nachhall") };

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
}

void SaalView::resized()
{
    auto r = getLocalBounds().reduced (28, 0).withTrimmedTop (18).withTrimmedBottom (14);
    auto right = r.removeFromRight (404);
    r.removeFromRight (24);
    lucBox = r.removeFromRight (300);
    r.removeFromRight (20);
    planBox = r;
    plan.setBounds (r);
    {
        auto b = lucBox.withTrimmedTop (32);
        auto t = b.removeFromTop (26);
        lucOn.setBounds (t.removeFromLeft (100));
        t.removeFromLeft (6);
        lucSource.setBounds (t);
        b.removeFromTop (6);
        lucGenArea = b.removeFromTop (34);
        lucCurveArea = b.removeFromTop (118);
        lucPeakArea = b.removeFromTop (34);
        b.removeFromTop (4);
        for (auto* x : lucRows) x->setBounds (b.removeFromTop (30).withTrimmedBottom (2));
        b.removeFromTop (4);
        lucRestart.setBounds (b.removeFromTop (24).withWidth (180));
        b.removeFromTop (8);
        lucNoteArea = b;
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
    { auto b = dimBox.withTrimmedTop (kTitle); stack (b, dims); }
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
        stack (b, surf);
    }
    right.removeFromTop (8);
    levelBox = take (kTitle + 4 * kRow);
    { auto b = levelBox.withTrimmedTop (kTitle); stack (b, levels); }
    tailFdn.setBounds (levelBox.getRight() - 52, levelBox.getY() + 9, 52, 20);
    tailConv.setBounds (tailFdn.getX() - 4 - 66, levelBox.getY() + 9, 66, 20);
}

void SaalView::refresh()
{
    plan.repaint();
    for (auto& r : rows) r->repaint();
    const auto st = proc.getRoomStatus();
    if (st != shownStatus) { shownStatus = st; repaint (saalBox); }
    const auto& lv = proc.getLucierView();
    const float lvl = proc.engine.lucierLevel.load();
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
    // generation, large
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
        const float lvl = juce::jlimit (0.0f, 1.0f, proc.engine.lucierLevel.load());
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
        for (size_t i = 0; i < juce::jmin<size_t> (4, lv.peaks.size()); ++i)
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
    if (lucNoteArea.getHeight() > 20)
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
    drawSectionTitle (g, dimBox, U ("Ma\xc3\x9f" "e"), "modulierbar");
    drawSectionTitle (g, surfBox, U ("Oberfl\xc3\xa4" "chen"), "Material setzt die Absorption");
    drawSectionTitle (g, levelBox, U ("H\xc3\xb6rer \xc2\xb7 Pegel"));
    g.setColour (col::mute);
    g.setFont (A.monoFont (11.0f));
    g.drawText ("Nachhall:", juce::Rectangle<int> (tailConv.getX() - 70, tailConv.getY(), 64, tailConv.getHeight()), juce::Justification::centredRight);
    paintLucier (g);
}
} // namespace rysum::ui
