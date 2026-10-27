#include "EffectsView.h"

namespace rysum::ui
{
static juce::String U (const char* s) { return juce::String::fromUTF8 (s); }

ParamRow& EffectsView::row (const char* id, const juce::String& name)
{
    rows.push_back (std::make_unique<ParamRow> (proc.apvts, id, name, true));
    addAndMakeVisible (*rows.back());
    return *rows.back();
}

juce::TextButton& EffectsView::toggle (const char* id, const juce::String& text)
{
    toggles.push_back (std::make_unique<juce::TextButton> (text));
    auto& b = *toggles.back();
    b.setClickingTogglesState (true);
    addAndMakeVisible (b);
    toggleAtts.push_back (std::make_unique<ButtonAtt> (proc.apvts, id, b));
    return b;
}

juce::ComboBox& EffectsView::combo (const char* id)
{
    combos.push_back (std::make_unique<juce::ComboBox>());
    auto& c = *combos.back();
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)))
        c.addItemList (choice->choices, 1);
    addAndMakeVisible (c);
    comboAtts.push_back (std::make_unique<ComboAtt> (proc.apvts, id, c));
    return c;
}

float EffectsView::value (const char* id) const
{
    if (auto* v = proc.apvts.getRawParameterValue (id)) return v->load();
    return 0.0f;
}

EffectsView::EffectsView (RysumProcessor& p) : proc (p)
{
    // ---- own sounds
    openFolder.setTooltip (U ("Legt den Ordner samples/noise/eigene an (falls n\xc3\xb6tig) und \xc3\xb6" "ffnet ihn. "
                              "WAV/AIFF/FLAC bis 20 s: Schrauben, Ketten, Holz, Metall \xe2\x80\x93 kurze, helle Ger\xc3\xa4usche orten sich am besten."));
    rescan.setTooltip (U ("Liest den Ordner neu ein, ohne die Pfeifen neu zu laden"));
    openFolder.onClick = [this]
    {
        auto dir = proc.getCustomFolder();
        if (dir == juce::File()) return;
        if (! dir.isDirectory()) dir.createDirectory();
        dir.startAsProcess();
    };
    rescan.onClick = [this] { proc.reloadCustomSounds(); };
    addAndMakeVisible (openFolder);
    addAndMakeVisible (rescan);
    custPlace = &combo ("fxCustPlace");
    custPlace->setTooltip (U ("Wo die Ger\xc3\xa4usche erklingen: an einer Pfeife eines gezogenen Registers, irgendwo im Raum, am Spieltisch oder auf einer Kreisbahn"));
    custRows = { &row ("fxCustLevel", U ("Lautst\xc3\xa4rke")), &row ("fxCustDown", "Beim Anschlag (Wahrscheinlichkeit)"),
                 &row ("fxCustUp", "Beim Loslassen"), &row ("fxCustPitch", U ("Tonh\xc3\xb6he folgt der Taste")),
                 &row ("fxCustScatter", U ("Streuung (Echos, Zeit, Ort)")), &row ("fxCustWander", "Wandern") };

    // ---- tracker path
    pathRows = { &row ("fxPathLevel", U ("Lautst\xc3\xa4rke")), &row ("fxPathTime", "Laufzeit"),
                 &row ("fxPathTicks", "Schritte"), &row ("fxPathSpread", "Streuung des Weges"), nullptr, nullptr };
    pathWait = &toggle ("fxPathWait", "Pfeifen warten");
    pathWait->setTooltip (U ("Die Pfeife spricht erst, wenn die Klicks bei ihr ankommen (Laufzeit)."));

    // ---- crackle
    crackRows = { &row ("fxCrackLevel", U ("Lautst\xc3\xa4rke")), &row ("fxCrackRate", "Dichte (je Pfeife)"),
                  &row ("fxCrackLen", U ("L\xc3\xa4nge eines Knacksers")), nullptr, nullptr, nullptr };

    // ---- wind leaks
    leakRows = { &row ("fxLeakLevel", U ("Lautst\xc3\xa4rke")), &row ("fxLeakCount", "Anzahl"),
                 &row ("fxLeakFreq", U ("Tonh\xc3\xb6he")), &row ("fxLeakWander", "Wandern"), nullptr, nullptr };
    windRound = &toggle ("fxWindRound", U ("Gebl\xc3\xa4se ringsum"));
    windRound->setTooltip (U ("Das Gebl\xc3\xa4serauschen (Wind) kommt von allen Seiten statt als ein Signal."));

    // ---- upper partials
    partRows = { &row ("fxPartSpread", U ("Versatz der Obert\xc3\xb6ne")),
                 &row ("fxPartSplit", U ("Trennung (\xc3\x97 Grundton)")), &row ("fxPartTurn", "Kreisen"), nullptr, nullptr, nullptr };

    // ---- echoes
    echoSync = &toggle ("fxEchoSync", "Im Tempo");
    echoDiv = &combo ("fxEchoDiv");
    echoTime = &row ("fxEchoTime", "Zeit");
    echoRows = { &row ("fxEchoLevel", U ("Lautst\xc3\xa4rke")), &row ("fxEchoRepeats", "Wiederholungen"),
                 &row ("fxEchoFeedback", "Abnahme je Wiederholung"), &row ("fxEchoTurn", "Drehung je Wiederholung"),
                 &row ("fxEchoTilt", U ("Neigung je Wiederholung")), &row ("fxEchoDamp", U ("D\xc3\xa4mpfung")) };
    refresh();
}

void EffectsView::resized()
{
    auto r = getLocalBounds().reduced (28, 0).withTrimmedTop (22).withTrimmedBottom (18);
    const int gap = 28;
    const int cw = (r.getWidth() - 2 * gap) / 3;
    const int rh = (r.getHeight() - gap) / 2;
    auto cell = [&] (int c, int rr) { return juce::Rectangle<int> (r.getX() + c * (cw + gap), r.getY() + rr * (rh + gap), cw, rh); };
    custBox = cell (0, 0); pathBox = cell (1, 0); crackBox = cell (2, 0);
    leakBox = cell (0, 1); partBox = cell (1, 1); echoBox = cell (2, 1);
    constexpr int kRow = 30;

    auto body = [] (juce::Rectangle<int> b) { return b.withTrimmedTop (38); };
    auto stack = [&] (juce::Rectangle<int>& b, std::array<ParamRow*, 6>& rs)
    {
        for (auto* x : rs) if (x != nullptr) x->setBounds (b.removeFromTop (kRow).withTrimmedBottom (2));
    };
    {
        auto b = body (custBox);
        auto t = b.removeFromTop (24);
        openFolder.setBounds (t.removeFromLeft (118));
        t.removeFromLeft (6);
        rescan.setBounds (t.removeFromLeft (104));
        b.removeFromTop (6);
        custPlace->setBounds (b.removeFromTop (24));
        b.removeFromTop (6);
        stack (b, custRows);
        custNote = b;
    }
    {
        auto b = body (pathBox);
        stack (b, pathRows);
        b.removeFromTop (4);
        pathWait->setBounds (b.removeFromTop (24).withWidth (150));
        b.removeFromTop (8);
        pathNote = b;
    }
    {
        auto b = body (crackBox);
        stack (b, crackRows);
        b.removeFromTop (8);
        crackNote = b;
    }
    {
        auto b = body (leakBox);
        stack (b, leakRows);
        b.removeFromTop (4);
        windRound->setBounds (b.removeFromTop (24).withWidth (150));
        b.removeFromTop (8);
        leakNote = b;
    }
    {
        auto b = body (partBox);
        stack (b, partRows);
        b.removeFromTop (6);
        auto rest = b;
        partCompass = rest.removeFromRight (juce::jmin (rest.getHeight(), 130));
        partNote = rest.withTrimmedRight (8);
    }
    {
        auto b = body (echoBox);
        echoCompass = b.removeFromRight (104).withSizeKeepingCentre (104, 104).withY (b.getY() + 30);
        b.removeFromRight (10);
        auto t = b.removeFromTop (24);
        echoSync->setBounds (t.removeFromLeft (84));
        t.removeFromLeft (6);
        echoDiv->setBounds (t);
        b.removeFromTop (4);
        echoTime->setBounds (b.removeFromTop (kRow).withTrimmedBottom (2));
        stack (b, echoRows);
    }
}

void EffectsView::refresh()
{
    const bool sync = value ("fxEchoSync") > 0.5f;
    echoDiv->setEnabled (sync);
    echoTime->setEnabled (! sync);
    echoTime->setAlpha (sync ? 0.35f : 1.0f);
    echoDiv->setAlpha (sync ? 1.0f : 0.45f);
    const int n = proc.getNumCustomSounds();
    if (n != shownCustom) { shownCustom = n; repaint (custBox); }
    for (auto& r : rows) r->repaint();
    repaint (echoCompass.expanded (14));
    repaint (partCompass.expanded (14));
}

/** top view: listener in the middle, front up. kind 0 = upper partials, 1 = echoes */
void EffectsView::drawCompass (juce::Graphics& g, juce::Rectangle<int> area, int kind) const
{
    auto& A = Assets::get();
    const auto c = area.toFloat().getCentre();
    const float R = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f - 12.0f;
    if (R < 10.0f) return;
    g.setColour (col::border);
    g.drawEllipse (c.x - R, c.y - R, 2 * R, 2 * R, 1.0f);
    g.setColour (col::line);
    g.drawLine (c.x - R, c.y, c.x + R, c.y, 0.6f);
    g.drawLine (c.x, c.y - R, c.x, c.y + R, 0.6f);
    g.setColour (col::mute);
    g.fillEllipse (c.x - 2.5f, c.y - 2.5f, 5.0f, 5.0f);
    g.setFont (A.monoFont (9.0f));
    g.setColour (col::faint);
    g.drawText ("vorn", juce::Rectangle<float> (c.x - 20, c.y - R - 13, 40, 11), juce::Justification::centred);

    // azimuth positive = to the left (AmbiX)
    auto at = [&] (float az, float el, float frac)
    {
        const float a = juce::degreesToRadians (az);
        const float rr = R * frac * std::cos (juce::degreesToRadians (juce::jlimit (-89.0f, 89.0f, el)));
        return juce::Point<float> (c.x - std::sin (a) * rr, c.y - std::cos (a) * rr);
    };
    if (kind == 0)
    {
        const float spread = value ("fxPartSpread");
        const float turn = value ("fxPartTurn");
        const auto pipe = at (0.0f, 0.0f, 0.82f);
        g.setColour (col::ink);
        g.fillEllipse (pipe.x - 5, pipe.y - 5, 10, 10);
        if (spread > 0.5f)
        {
            const float rot = std::abs (turn) > 0.0005f ? (float) std::fmod (proc.engine.timeSeconds.load() * turn * 360.0, 360.0) : 0.0f;
            for (float side : { -1.0f, 1.0f })
            {
                const auto q = at (spread * side + rot, 0.0f, 0.82f);
                g.setColour (col::rubric.brighter (0.3f));
                g.drawEllipse (q.x - 4, q.y - 4, 8, 8, 1.4f);
                g.setColour (col::rubric.withAlpha (0.5f));
                g.drawLine (pipe.x, pipe.y, q.x, q.y, 0.7f);
            }
        }
        g.setFont (A.monoFont (9.0f));
        g.setColour (col::dim);
        g.drawText (U ("\xe2\x97\x8f Grundton   \xe2\x97\x8b Obert\xc3\xb6ne"), area.withTop (area.getBottom() - 11).expanded (20, 0), juce::Justification::centred);
        return;
    }
    // echoes: numbered dots, size by level
    const int reps = juce::roundToInt (value ("fxEchoRepeats"));
    const float fb = value ("fxEchoFeedback"), lvl = value ("fxEchoLevel");
    const float turn = value ("fxEchoTurn"), tilt = value ("fxEchoTilt");
    for (int r = 1; r <= reps; ++r)
    {
        const float gain = std::pow (fb, (float) (r - 1));
        if (gain < 0.01f) break;
        const float el = juce::jlimit (-80.0f, 80.0f, tilt * (float) r);
        const auto q = at (turn * (float) r, el, 0.8f);
        const float s = 5.0f + 7.0f * gain;
        g.setColour ((lvl > 0.0001f ? col::rubric.brighter (0.3f) : col::mute).withAlpha (0.35f + 0.65f * gain));
        g.fillEllipse (q.x - s * 0.5f, q.y - s * 0.5f, s, s);
        g.setColour (col::ink);
        g.setFont (A.monoFont (9.0f));
        g.drawText (juce::String (r), juce::Rectangle<float> (q.x - 10, q.y - s * 0.5f - 12, 20, 11), juce::Justification::centred);
    }
}

void EffectsView::paint (juce::Graphics& g)
{
    auto& A = Assets::get();
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.setOpacity (0.07f);
        drawCover (g, A.wall, getLocalBounds().toFloat(), 0.4f);
    }
    const int n = proc.getNumCustomSounds();
    drawSectionTitle (g, custBox, U ("Eigene Ger\xc3\xa4usche"),
                      n > 0 ? juce::String (n) + (n == 1 ? " Datei" : " Dateien") : U ("Ordner leer \xe2\x86\x92 Klicks"));
    drawSectionTitle (g, pathBox, "Trakturweg", U ("Spieltisch \xe2\x86\x92 Pfeife"));
    drawSectionTitle (g, crackBox, "Knistern", "an jeder Pfeife");
    drawSectionTitle (g, leakBox, "Windlecks", "zischende Stellen");
    drawSectionTitle (g, partBox, U ("Obert\xc3\xb6ne verteilen"), "Grundton bleibt");
    drawSectionTitle (g, echoBox, "Wandernde Echos", "jede Wiederholung woanders");

    g.setFont (A.sansFont (12.5f));
    g.setColour (col::mute);
    auto note = [&] (juce::Rectangle<int> r, const char* text)
    {
        if (r.getHeight() > 14) g.drawFittedText (U (text), r, juce::Justification::topLeft, juce::jmax (1, r.getHeight() / 15));
    };
    note (pathNote, "Kleine Klicks laufen vom Spieltisch zu den Pfeifen der gezogenen Register, wie die Abstrakten "
                    "der Traktur. Mit \xe2\x80\x9ePfeifen warten\xe2\x80\x9c spricht die Pfeife erst, wenn der Weg ankommt.");
    note (crackNote, "Kurze Knackser an jeder klingenden Pfeife: sie folgen jeder Bewegung, das Ohr findet die Pfeifen leichter. "
                     "Hohe Dichte und kurze L\xc3\xa4nge klingen wie Glut, wenige lange Knackser wie altes Holz.\n\n"
                     "Nach dem Loslassen verklingt das Knistern in einer halben Sekunde.");
    note (leakNote, "Undichte Stellen im Windsystem zischen ungleichm\xc3\xa4\xc3\x9fig von festen Orten. "
                    "Wie laut das Gebl\xc3\xa4se selbst rauscht, stellt \xe2\x80\x9eWind\xe2\x80\x9c auf dem Spieltisch ein.");
    note (partNote, "Oberhalb der Trennung wandert der Klang jeder Pfeife zur Seite (zuf\xc3\xa4llig links oder rechts); "
                    "\xe2\x80\x9eKreisen\xe2\x80\x9c dreht diesen Teil um den H\xc3\xb6rer.");
    note (custNote, "");

    drawCompass (g, partCompass, 0);
    drawCompass (g, echoCompass, 1);
}
} // namespace rysum::ui
