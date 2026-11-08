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
    openFolder.setTooltip (U ("\xc3\x96" "ffnet den Ordner im Explorer (der Standardordner samples/noise/eigene wird bei Bedarf angelegt) \xe2\x80\x93 "
                              "Dateien hineinlegen, dann \xe2\x80\x9eNeu einlesen\xe2\x80\x9c. Kurze, helle Ger\xc3\xa4usche orten sich am besten."));
    rescan.setTooltip (U ("Liest den Ordner neu ein, ohne die Pfeifen neu zu laden"));
    openFolder.onClick = [this]
    {
        auto dir = proc.getCustomFolder();
        if (dir == juce::File()) return;
        if (! dir.isDirectory()) dir.createDirectory();
        dir.startAsProcess();
    };
    rescan.onClick = [this] { proc.reloadCustomSounds(); };
    chooseFolder.setTooltip (U ("Einen beliebigen Ordner mit Ger\xc3\xa4uschen w\xc3\xa4hlen (WAV/AIFF/FLAC/OGG bis 20 s, auch in Unterordnern, bis 64 Dateien). "
                                "Wird im Projekt und in Patches gespeichert."));
    chooseFolder.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (U ("Ordner mit eigenen Ger\xc3\xa4uschen"), proc.getCustomFolder());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
                              {
                                  const auto r = fc.getResult();
                                  if (r.isDirectory()) proc.setCustomFolder (r);
                                  else if (r.existsAsFile()) proc.setCustomFolder (r.getParentDirectory());
                                  repaint();
                              });
    };
    resetFolder.setTooltip (U ("Zur\xc3\xbc" "ck zum Standardordner samples/noise/eigene"));
    resetFolder.onClick = [this] { proc.setCustomFolder ({}); repaint(); };
    addAndMakeVisible (chooseFolder);
    addChildComponent (resetFolder);
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
    for (int i = 0; i < getNumChildComponents(); ++i) page0.push_back (getChildComponent (i));

    // ---- second tab: the tuning in space, photographic operations
    const int before = getNumChildComponents();
    beatFrom = &combo ("fxBeatIdeal");
    beatFrom->setTooltip (U ("Woraus die Schwebungen berechnet werden: aus der Stimmung, wie sie gemeint ist (reine Terzen stehen still), "
                             "oder aus den Pfeifen, wie sie aufgenommen sind (\xc2\xb1" "5 ct: auch reine Terzen schweben ein wenig)."));
    ghostRows = { &row ("fxGhostLevel", U ("Lautst\xc3\xa4rke")), &row ("fxGhostPlace", U ("Ort (zwischen \xe2\x86\x92 unten)")),
                  &row ("fxGhostCons", "nur Konsonanzen"), nullptr, nullptr, nullptr };
    dexMirror = &combo ("fxDexMirror");
    dexMirror->setTooltip (U ("Das alte Bild gespiegelt: links / rechts, vorn / hinten, oben / unten"));
    dexRows = { &row ("fxDexMix", "Menge"), &row ("fxDexTime", "Abstand (Sekunden davor)"), &row ("fxDexTurn", "Drehung"),
                &row ("fxDexFade", "Verblassen"), &row ("fxDexAgain", "mehrfach belichten"), nullptr };
    expoRows = { &row ("fxExpoMix", "Menge"), &row ("fxExpoTime", "Belichtungszeit"), &row ("fxExpoBright", "Helligkeit der Spuren"),
                 nullptr, nullptr, nullptr };
    for (int i = before; i < getNumChildComponents(); ++i) page1.push_back (getChildComponent (i));
    for (auto* b : { &tabSounds, &tabLight }) { b->setClickingTogglesState (false); addAndMakeVisible (*b); }
    tabSounds.onClick = [this] { showTab (0); };
    tabLight.onClick = [this] { showTab (1); };
    showTab (0);
    refresh();
}

void EffectsView::showTab (int t)
{
    tab = t;
    for (auto* c : page0) c->setVisible (t == 0);
    for (auto* c : page1) c->setVisible (t == 1);
    if (t == 0) resetFolder.setVisible (proc.hasOwnCustomFolder());
    tabSounds.setToggleState (t == 0, juce::dontSendNotification);
    tabLight.setToggleState (t == 1, juce::dontSendNotification);
    repaint();
}

void EffectsView::resized()
{
    tabSounds.setBounds (getWidth() - 28 - 170 - 4 - 80, 4, 80, 18);
    tabLight.setBounds (getWidth() - 28 - 170, 4, 170, 18);
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
        chooseFolder.setBounds (t.removeFromLeft (132));
        t.removeFromLeft (6);
        openFolder.setBounds (t.removeFromLeft (70));
        t.removeFromLeft (6);
        rescan.setBounds (t.removeFromLeft (104));
        t.removeFromLeft (6);
        resetFolder.setBounds (t.removeFromLeft (24));
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
    // ---- second tab: Schwebung | Differenztoene | (note) / Doppelbelichtung | Belichtung
    beatBox = cell (0, 0); ghostBox = cell (1, 0); dexBox = cell (0, 1).withRight (cell (1, 1).getRight()); expoBox = cell (2, 1);
    {
        auto b = body (beatBox);
        beatFrom->setBounds (b.removeFromTop (24));
        b.removeFromTop (8);
        beatNote = b;
    }
    {
        auto b = body (ghostBox);
        stack (b, ghostRows);
        b.removeFromTop (8);
        ghostNote = b;
    }
    {
        auto b = body (dexBox);
        auto left = b.removeFromLeft (cw);
        dexMirror->setBounds (left.removeFromTop (24));
        left.removeFromTop (6);
        stack (left, dexRows);
        b.removeFromLeft (gap);
        dexNote = b;
    }
    {
        auto b = body (expoBox);
        stack (b, expoRows);
        b.removeFromTop (8);
        expoNote = b;
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
    if (tab == 0 && resetFolder.isVisible() != proc.hasOwnCustomFolder()) { resetFolder.setVisible (proc.hasOwnCustomFolder()); repaint (custBox); }
    for (auto& r : rows) r->repaint();
    repaint (echoCompass.expanded (14));
    repaint (partCompass.expanded (14));
}

/** top view: listener in the middle, front up. kind 0 = upper partials, 1 = echoes */
void EffectsView::drawCompass (juce::Graphics& g, juce::Rectangle<int> area, int kind) const
{
    auto& A = Assets::get();
    const auto c = area.toFloat().getCentre();
    const float R = static_cast<float> (juce::jmin (area.getWidth(), area.getHeight())) * 0.5f - 12.0f;
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
    auto noteIn = [&] (juce::Rectangle<int> r, const char* text)
    {
        g.setFont (A.sansFont (12.5f));
        g.setColour (col::mute);
        if (r.getHeight() > 14) g.drawFittedText (U (text), r, juce::Justification::topLeft, juce::jmax (1, r.getHeight() / 15));
    };
    if (tab == 1)
    {
        auto tonnetzBox = juce::Rectangle<int> (ghostBox.getRight() + 28, ghostBox.getY(), expoBox.getWidth(), ghostBox.getHeight());
        drawSectionTitle (g, beatBox, "Schwebungsraum", "die Stimmung bewegt den Raum");
        drawSectionTitle (g, ghostBox, U ("Differenzt\xc3\xb6ne"), "Geister zwischen den Pfeifen");
        drawSectionTitle (g, tonnetzBox, "Tonnetz", "die Stimmung als Landkarte");
        drawSectionTitle (g, dexBox, "Doppelbelichtung", U ("das Bild von vorhin dar\xc3\xbc" "ber"));
        drawSectionTitle (g, expoBox, "Belichtung", "Spuren auf der Kugel");
        noteIn (beatNote, "Je zwei klingende Pfeifen schweben gegeneinander (die n\xc3\xa4" "chsten Teilt\xc3\xb6ne, die fast zusammenfallen). "
                          "Mit der Bewegung \xe2\x80\x9eSchwebung\xe2\x80\x9c (Raum-Seite) dreht sich jede Pfeife so schnell, wie sie schwebt: "
                          "reine Terzen stehen still, mittelt\xc3\xb6nige Quinten wandern langsam (um 2 Hz), kleine Terzen schneller, "
                          "die Wolfsquinte G\xe2\x99\xaf\xe2\x80\x93" "Es kreist heftig (um 13 Hz), die Wolfsterzen noch heftiger. "
                          "\xe2\x80\x9eGeschwindigkeit\xe2\x80\x9c = Umdrehungen je Schwebung.\n\n"
                          "Als Modulationsquelle \xe2\x80\x9eSchwebung\xe2\x80\x9c (je Pfeife) steuert sie alles andere.");
        noteIn (ghostNote, "Aus je zwei gehaltenen Pfeifen entsteht der Differenzton (h\xc3\xb6here minus tiefere Frequenz) \xe2\x80\x93 "
                           "wie die Resultante, mit der Orgelbauer einen 32\xe2\x80\xb2 andeuten. Er erscheint, solange das Intervall "
                           "gehalten wird, zwischen den beiden Pfeifen oder tief unter dem H\xc3\xb6rer. Quinte: die Oktave unter dem "
                           "tieferen Ton; reine Terz: zwei Oktaven unter dem Grundton des Dreiklangs.");
        noteIn (tonnetzBox.withTrimmedTop (38), "Auf der Raum-Seite: Anordnung \xe2\x80\x9eTonnetz\xe2\x80\x9c. Jede Taste bekommt ihren Platz im Gitter "
                                                 "der Stimmung: Quinten nach rechts, gro\xc3\x9f" "e Terzen nach oben. Reine Intervalle liegen eng "
                                                 "beieinander, unreine weit \xe2\x80\x93 in \xc2\xbc-Komma bilden Dreikl\xc3\xa4nge enge Gruppen, "
                                                 "G\xe2\x99\xaf und Es liegen an entgegengesetzten R\xc3\xa4ndern (dort sitzt der Wolf). "
                                                 "Eine andere Stimmung zeichnet die Karte neu (pythagoreisch: eine Kette von Quinten, "
                                                 "gleichstufig: weit auseinander liegende Terzen).");
        noteIn (dexNote, "Das Klangfeld von vor ein paar Sekunden wird \xc3\xbc" "ber das jetzige gelegt \xe2\x80\x93 gedreht, gespiegelt, "
                         "verblasst, wie eine zweite Belichtung auf demselben Film. \xe2\x80\x9eMehrfach\xe2\x80\x9c belichtet das "
                         "alte Bild noch einmal: weitere, immer blassere Bilder weiter zur\xc3\xbc" "ck.\n\n"
                         "Wirkt auf das ganze Feld nach Hall und Saal (bis 3. Ordnung; in Stereo wird gespiegelt durch Tauschen der Seiten).");
        noteIn (expoNote, "Jede Richtung der Kugel h\xc3\xa4lt fest, was durch sie kommt, und gibt es langsam wieder ab: "
                          "bewegte Pfeifen ziehen Lichtspuren, schnelle malen B\xc3\xb6gen.");
        return;
    }
    const int n = proc.getNumCustomSounds();
    const juce::String where = proc.hasOwnCustomFolder() ? proc.getCustomFolder().getFileName() + U (" \xc2\xb7 ") : juce::String();
    drawSectionTitle (g, custBox, U ("Eigene Ger\xc3\xa4usche"),
                      where + (n > 0 ? juce::String (n) + (n == 1 ? " Datei" : " Dateien") : U ("Ordner leer \xe2\x86\x92 Klicks")));
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
