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
    auto info = getLocalBounds().removeFromBottom (30).removeFromTop (15).reduced (4, 0);
    if (on)
        g.drawText ("V " + juce::String (juce::roundToInt (V)) + U (" m\xc2\xb3 \xc2\xb7 Nachhall ") + juce::String (proc.engine.roomRtLow.load(), 2)
                    + U (" s tief / ") + juce::String (proc.engine.roomRtHigh.load(), 2) + U (" s hoch \xc2\xb7 Hallradius ")
                    + juce::String (proc.engine.roomCritDist.load(), 1) + U (" m \xc2\xb7 ") + juce::String (proc.engine.erActive.load()) + U (" Kl\xc3\xa4nge mit eigenen Reflexionen"),
                    info, juce::Justification::centredLeft);
    else
        g.drawText (U ("Saal aus \xe2\x80\x93 der Hall kommt von der Impulsantwort (Seite Raum)"), info, juce::Justification::centredLeft);
    g.setFont (A.monoFont (9.5f));
    g.setColour (col::faint);
    if (on) g.drawText (proc.getRoomStatus(), getLocalBounds().removeFromBottom (15).reduced (4, 0), juce::Justification::centredLeft);
    g.setFont (A.monoFont (10.5f));
    g.drawText (U ("Draufsicht \xc2\xb7 H\xc3\xb6rer ziehen \xc2\xb7 rot: Reflexionen der zuletzt gespielten Pfeife"),
                getLocalBounds().removeFromTop (14).reduced (4, 0), juce::Justification::centredLeft);
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
}

void SaalView::resized()
{
    auto r = getLocalBounds().reduced (28, 0).withTrimmedTop (18).withTrimmedBottom (14);
    auto right = r.removeFromRight (404);
    r.removeFromRight (24);
    planBox = r;
    plan.setBounds (r);

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
}

void SaalView::refresh()
{
    plan.repaint();
    for (auto& r : rows) r->repaint();
    const auto st = proc.getRoomStatus();
    if (st != shownStatus) { shownStatus = st; repaint (saalBox); }
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
    drawSectionTitle (g, levelBox, U ("H\xc3\xb6rer \xc2\xb7 Pegel"), U ("H\xc3\xb6rer auch im Plan ziehen"));
}
} // namespace rysum::ui
