#include "KlangView.h"

namespace rysum::ui
{
ParamRow& KlangView::row (const char* id, const juce::String& name)
{
    rows.push_back (std::make_unique<ParamRow> (proc.apvts, id, name));
    addAndMakeVisible (*rows.back());
    return *rows.back();
}

juce::TextButton& KlangView::toggle (const char* id, const juce::String& text)
{
    toggles.push_back (std::make_unique<juce::TextButton> (text));
    auto& b = *toggles.back();
    b.setClickingTogglesState (true);
    addAndMakeVisible (b);
    toggleAtts.push_back (std::make_unique<ButtonAtt> (proc.apvts, id, b));
    return b;
}

KlangView::KlangView (RysumProcessor& p) : proc (p)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("temperament")))
        temperament.addItemList (choice->choices, 1);
    temperamentAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "temperament", temperament);
    temperament.setTooltip (u8 ("Die Aufnahmen sind \xc2\xbc-Komma mittelt\xc3\xb6nig; jede andere Stimmung verschiebt die Pfeifen, a\xe2\x80\xb2 bleibt. "
                               "Bei den Stufen-Stimmungen (jede Taste ein Schritt) spielt jede Taste die Pfeife, die ihrer Tonh\xc3\xb6he am n\xc3\xa4" "chsten liegt."));
    addAndMakeVisible (temperament);
    scalaLoad.setTooltip (u8 ("Eine Skala im Scala-Format (.scl) laden \xe2\x80\x93 danach optional eine Tastenzuordnung (.kbm). "
                              "Jede MIDI-Note bekommt ihre eigene Tonh\xc3\xb6he; es spielt die aufgenommene Pfeife, die ihr am n\xc3\xa4" "chsten liegt. "
                              "Die Dateien werden im Projekt und im Patch mitgespeichert."));
    scalaLoad.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (u8 ("Scala-Skala (.scl) oder Tastenzuordnung (.kbm)"),
                                                       juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.scl;*.kbm");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) proc.loadTuningFile (fc.getResult()); repaint (tuneBox); });
    };
    kbmClear.setTooltip (u8 ("Tastenzuordnung entfernen: Standard (Stufe 0 auf Taste 60, Taste 69 = 444,5 Hz)"));
    kbmClear.onClick = [this] { proc.clearKbm(); repaint (tuneBox); };
    addAndMakeVisible (scalaLoad);
    addAndMakeVisible (kbmClear);
    mtsFollow = &toggle ("mtsContinuous", u8 ("T\xc3\xb6ne folgen"));
    mtsFollow->setTooltip (u8 ("MTS-ESP: gehaltene T\xc3\xb6ne folgen einer ge\xc3\xa4nderten Stimmung sofort. Aus: jeder Ton beh\xc3\xa4lt die Tonh\xc3\xb6he seines Anschlags."));

    pitch  = &row ("pitch", "Stimmton");
    detune = &row ("detune", u8 ("Verstimmung (zuf\xc3\xa4llig je Pfeife)"));

    tremOn    = &toggle ("tremOn", "Tremulant");
    tremRate  = &row ("tremRate", "Tempo");
    tremDepth = &row ("tremDepth", "Tiefe");

    freezeOn = &toggle ("freeze", "Freeze");
    freezeOn->setTooltip (u8 ("An: losgelassene T\xc3\xb6ne frieren als Kornwolke ein. Aus: sie klingen aus. (CC 66)"));
    smear    = &row ("freezeSmear", "Verwischen");
    release  = &row ("freezeRelease", "Ausklang beim Ausschalten");

    // two loudspeakers (transaural): the binaural signal with the crosstalk cancelled
    trOn.setClickingTogglesState (false);
    trOn.setTooltip (u8 ("Schaltet die Ausgabe auf Transaural: der Kopfh\xc3\xb6rer-Klang \xc3\xbc" "ber zwei Lautsprecher, "
                         "das \xc3\x9c" "bersprechen zum jeweils anderen Ohr wird ausgel\xc3\xb6scht. Ordnung, HRTF und Kopfdrehung wie bei Binaural."));
    trOn.onClick = [this]
    {
        if (auto* o = proc.apvts.getParameter ("output"))
            o->setValueNotifyingHost (o->convertTo0to1 (proc.isTransaural() ? 0.0f : (float) RysumProcessor::kTransauralOutput));
    };
    addAndMakeVisible (trOn);
    trAngle = &row ("trAngle", "Lautsprecher bei");
    trAngle->setTooltip (u8 ("Wo die beiden Lautsprecher stehen, von vorn gemessen (symmetrisch). 90\xc2\xb0 = links und rechts, "
                             "einander gegen\xc3\xbc" "ber, du in der Mitte zwischen ihnen."));
    trAmount = &row ("trAmount", u8 ("\xc3\x9c" "bersprechen aus"));
    trAmount->setTooltip (u8 ("0 % = einfach der Kopfh\xc3\xb6rer-Klang auf die Lautsprecher, 100 % = das \xc3\x9c" "bersprechen ganz ausgel\xc3\xb6scht "
                              "(das Bild wird r\xc3\xa4umlicher, aber empfindlicher f\xc3\xbcr die Sitzposition)"));
    trBoost = &row ("trBoost", u8 ("h\xc3\xb6" "chste Anhebung"));
    trBoost->setTooltip (u8 ("Die Ausl\xc3\xb6schung hebt die Tiefen im Seitensignal an (der Kopf ist klein gegen\xc3\xbc" "ber langen Wellen). "
                             "Mehr = genauer, aber mehr Bass-Hub und Schwanken bei Kopfbewegung."));
    trEq = &toggle ("trSideEq", "Seitenklang ausgleichen");
    trEq->setTooltip (u8 ("Nimmt auch die Klangf\xc3\xa4rbung der Lautsprecher-Richtung heraus (die Ohrmuschel h\xc3\xb6rt sie von der Seite): "
                          "die Ohren bekommen den Kopfh\xc3\xb6rer-Klang, nicht Kopfh\xc3\xb6rer-Klang mal Seite. Gegl\xc3\xa4ttet, h\xc3\xb6" "chstens \xc2\xb1" "10 dB."));

    doppler    = &row ("doppler", "Doppler");
    distAmount = &row ("distAmount", u8 ("Entfernung \xe2\x86\x92 Lautst\xc3\xa4rke"));
    air        = &row ("air", u8 ("Luft (H\xc3\xb6hen in der Ferne)"));
    airInvert  = &row ("airInvert", u8 ("Luft umkehren"));
    airInvert->setTooltip (u8 ("Gibt fernen Pfeifen die H\xc3\xb6hen zur\xc3\xbc" "ck, die die Luft ihnen nimmt: 50 % = wie nah, 100 % = unnat\xc3\xbcrlich pr\xc3\xa4sent \xe2\x80\x93 je ferner, desto heller."));

    // headphones
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("binOrder")))
        binOrder.addItemList (choice->choices, 1);
    binOrderAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "binOrder", binOrder);
    binOrder.setTooltip (u8 ("Ambisonics-Ordnung vor dem Kopfh\xc3\xb6rer-Decoder: h\xc3\xb6her = sch\xc3\xa4rfer geortet, braucht mehr Rechenzeit"));
    addAndMakeVisible (binOrder);
    binOn.setClickingTogglesState (false);
    binOn.setTooltip (u8 ("Schaltet die Ausgabe auf Binaural (Kopfh\xc3\xb6rer) bzw. zur\xc3\xbc" "ck auf Stereo"));
    binOn.onClick = [this]
    {
        if (auto* o = proc.apvts.getParameter ("output"))
            o->setValueNotifyingHost (o->convertTo0to1 (proc.isHeadphones() ? 0.0f : (float) RysumProcessor::kBinauralOutput));
    };
    addAndMakeVisible (binOn);
    sofaLoad.setTooltip (u8 ("Eigene HRTF im SOFA-Format (SimpleFreeFieldHRIR), z. B. eine individuell gemessene"));
    sofaLoad.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("HRTF im SOFA-Format", proc.getSofaFile(), "*.sofa");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) proc.setSofaFile (fc.getResult()); });
    };
    sofaReset.setTooltip (u8 ("Eingebaute HRTF: FABIAN-Kunstkopf der TU Berlin (Brinkmann et al. 2017, CC BY 4.0)"));
    sofaReset.onClick = [this] { proc.setSofaFile ({}); };
    addAndMakeVisible (sofaLoad);
    speakersButton.setTooltip (u8 ("Ausgabe auf ein Lautsprecher-Layout (AllRAD wie im IEM AllRADecoder): Layout, Ordnung, Test"));
    speakersButton.onClick = [this] { if (onOpenSpeakers) onOpenSpeakers(); };
    addAndMakeVisible (speakersButton);
    addAndMakeVisible (sofaReset);
    yaw = &row ("binYaw", "Kopfdrehung");
    yaw->slider.setTooltip (u8 ("Dreht die ganze Szene um den H\xc3\xb6rer, als ob du den Kopf drehst (modulierbar)"));
    dfEq = &toggle ("binDfEq", "Diffusfeld-Entzerrung");
    dfEq->setTooltip (u8 ("Gleicht die mittlere Klangf\xc3\xa4rbung der HRTF aus (f\xc3\xbcr diffusfeld-abgestimmte Kopfh\xc3\xb6rer). Aus: Kunstkopf-Klang wie gemessen."));

    // wind model of the stops (what a half-drawn stop does)
    if (auto* wm = proc.apvts.getParameter ("windModel"))
    {
        windAtt = std::make_unique<juce::ParameterAttachment> (*wm, [this] (float v)
        {
            windClassic.setToggleState (v < 0.5f, juce::dontSendNotification);
            windUnstable.setToggleState (v >= 0.5f, juce::dontSendNotification);
            repaint (windBox);
        });
        windAtt->sendInitialUpdate();
    }
    windClassic.onClick = [this] { if (windAtt) windAtt->setValueAsCompleteGesture (0.0f); };
    windUnstable.onClick = [this] { if (windAtt) windAtt->setValueAsCompleteGesture (1.0f); };
    windClassic.setTooltip (u8 ("Ein halb gezogenes Register (Wind-Regler je Register) wird leiser, tiefer, dumpfer und unruhig."));
    windUnstable.setTooltip (u8 ("Wie klassisch, und bei wenig Wind dazu ein Schneidenton: jede Pfeife pfeift auf ihre eigene Art, ruhig bei ruhigem Wind, "
                                 "tiefer und leiser, wenn der Wind sinkt. Die M\xc3\xbcndung zischt, ganz wenig Wind l\xc3\xa4sst die Ansprache stocken. "
                                 "Bei vollem Wind klingt alles wie gewohnt."));
    addAndMakeVisible (windClassic);
    addAndMakeVisible (windUnstable);

    midiOn = &toggle ("midiCC", "MIDI-CC an");
    midiOn->setTooltip (u8 ("Schaltet die feste CC-Belegung (Liste unten). Eigene Zuordnungen (MIDI lernen) gelten immer."));
    midiMapButton.setTooltip (u8 ("Drehregler, Fader und Taster deines Keyboards auf beliebige Regler legen (Seite Modulation)"));
    midiMapButton.onClick = [this] { if (auto* a = findParentComponentOfClass<ModUiAccess>()) a->openMidiMap(); };
    addAndMakeVisible (midiMapButton);
    mpeOn = &toggle ("mpe", "MPE");
    mpeOn->setTooltip (u8 ("MPE (z. B. Lumatone, Linnstrument, Seaboard, Osmose): jede Note auf ihrem eigenen MIDI-Kanal (2\xe2\x80\x93" "16) "
                           "mit eigener Tonh\xc3\xb6henbeugung; Druck und Gleiten (CC 74) sind Modulationsquellen je Ton. "
                           "Kanal 1 gilt f\xc3\xbcr alle (Tonh\xc3\xb6henrad als Modulationsquelle). Bleibt beim Patch-Wechsel erhalten."));
    mpeBend = &row ("mpeBend", u8 ("MPE-Beugung (ganzer Ausschlag)"));
    syncOn = &toggle ("motionSync", "Bewegung im Tempo");
    syncOn->setTooltip (u8 ("Geschwindigkeiten gelten bei 120 BPM, laufen mit dem Projekttempo und rasten auf die Taktposition ein."));
}

void KlangView::resized()
{
    auto r = getLocalBounds().reduced (28, 0).withTrimmedTop (22).withTrimmedBottom (22);
    const int gap = 28;
    const int cw = (r.getWidth() - 2 * gap) / 3;
    const int rh = (r.getHeight() - gap) / 2;
    auto cell = [&] (int c, int rr) { return juce::Rectangle<int> (r.getX() + c * (cw + gap), r.getY() + rr * (rh + gap), cw, rh); };
    tuneBox = cell (0, 0); tremBox = cell (1, 0); freezeBox = cell (2, 0);
    distBox = cell (0, 1); midiBox = cell (1, 1); binBox = cell (2, 1);

    auto body = [] (juce::Rectangle<int> b) { return b.withTrimmedTop (40); };
    {
        auto b = body (tuneBox);
        temperament.setBounds (b.removeFromTop (28));
        b.removeFromTop (12);
        pitch->setBounds (b.removeFromTop (38));
        detune->setBounds (b.removeFromTop (38));
        b.removeFromTop (6);
        auto t = b.removeFromTop (24);
        scalaLoad.setBounds (t.removeFromLeft (118));
        t.removeFromLeft (6);
        kbmClear.setBounds (t.removeFromLeft (106));
        t.removeFromLeft (6);
        mtsFollow->setBounds (t);
        b.removeFromTop (6);
        tuningStatusArea = b;
    }
    {
        auto b = body (tremBox);
        tremOn->setBounds (b.removeFromTop (28).withWidth (140));
        b.removeFromTop (12);
        tremRate->setBounds (b.removeFromTop (38));
        tremDepth->setBounds (b.removeFromTop (38));
        windBox = tremBox.withTrimmedTop (178);
        auto w = body (windBox);
        auto t = w.removeFromTop (26);
        windClassic.setBounds (t.removeFromLeft (110));
        t.removeFromLeft (6);
        windUnstable.setBounds (t.removeFromLeft (110));
        w.removeFromTop (8);
        windText = w;
    }
    {
        auto b = body (freezeBox);
        freezeOn->setBounds (b.removeFromTop (28).withWidth (140));
        b.removeFromTop (8);
        auto line = b.removeFromTop (38);
        const int hw = (line.getWidth() - 14) / 2;
        smear->setBounds (line.removeFromLeft (hw));
        line.removeFromLeft (14);
        release->setBounds (line);
        // two loudspeakers (transaural) below
        trBox = freezeBox.withTrimmedTop (130);
        auto t = body (trBox);
        auto top = t.removeFromTop (26);
        trOn.setBounds (top.removeFromLeft (140));
        top.removeFromLeft (8);
        trEq->setBounds (top.removeFromLeft (200));
        t.removeFromTop (6);
        auto l1 = t.removeFromTop (38);
        trAngle->setBounds (l1.removeFromLeft (hw));
        l1.removeFromLeft (14);
        trAmount->setBounds (l1);
        auto l2 = t.removeFromTop (38);
        trBoost->setBounds (l2.removeFromLeft (hw));
        l2.removeFromLeft (14);
        trStatusArea = l2.reduced (0, 2);
    }
    {
        auto b = body (distBox);
        doppler->setBounds (b.removeFromTop (38));
        distAmount->setBounds (b.removeFromTop (38));
        air->setBounds (b.removeFromTop (38));
        airInvert->setBounds (b.removeFromTop (38));
        b.removeFromTop (16);
        speakersButton.setBounds (b.removeFromTop (28).withWidth (230));
    }
    {
        auto b = body (binBox);
        auto t = b.removeFromTop (28);
        binOn.setBounds (t.removeFromLeft (124));
        t.removeFromLeft (8);
        binOrder.setBounds (t);
        b.removeFromTop (8);
        t = b.removeFromTop (26);
        sofaLoad.setBounds (t.removeFromLeft (124));
        t.removeFromLeft (8);
        sofaReset.setBounds (t.removeFromLeft (80));
        b.removeFromTop (6);
        yaw->setBounds (b.removeFromTop (38));
        b.removeFromTop (2);
        dfEq->setBounds (b.removeFromTop (26).withWidth (190));
        b.removeFromTop (6);
        binStatusArea = b;
    }
    {
        auto b = body (midiBox);
        auto t = b.removeFromTop (28);
        midiOn->setBounds (t.removeFromLeft (130));
        t.removeFromLeft (8);
        syncOn->setBounds (t.removeFromLeft (170));
        b.removeFromTop (8);
        t = b.removeFromTop (26);
        midiMapButton.setBounds (t.removeFromLeft (200));
        t.removeFromLeft (8);
        mpeOn->setBounds (t.removeFromLeft (80));
        b.removeFromTop (6);
        mpeBend->setBounds (b.removeFromTop (38));
        midiTextTop = b.getY() + 4 - midiBox.getY();
    }
}

void KlangView::refresh()
{
    {
        const auto st = proc.getTuningStatus() + (proc.hasKbm() ? "k" : "");
        if (st != shownTuningStatus) { shownTuningStatus = st; repaint (tuneBox); }
        kbmClear.setEnabled (proc.hasKbm());
        const int t = juce::roundToInt (proc.apvts.getRawParameterValue ("temperament")->load());
        mtsFollow->setAlpha (t == RysumProcessor::kMtsTuning ? 1.0f : 0.4f);
    }
    for (auto& r : rows) r->repaint();
    repaint (freezeBox);
    const bool on = proc.isHeadphones();
    if (binOn.getToggleState() != on) { binOn.setToggleState (on, juce::dontSendNotification); repaint (binBox); }
    const bool trNow = proc.isTransaural();
    if (trOn.getToggleState() != trNow) { trOn.setToggleState (trNow, juce::dontSendNotification); repaint (trBox); }
    const auto ts = proc.getTransauralStatus();
    if (ts != shownTrStatus) { shownTrStatus = ts; repaint (trBox); }
    const auto st = proc.getBinauralStatus() + "|" + proc.getSofaFile().getFullPathName();
    if (st != shownBinStatus) { shownBinStatus = st; repaint (binBox); }
}

void KlangView::paint (juce::Graphics& g)
{
    auto& A = Assets::get();
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.setOpacity (0.07f);
        drawCover (g, A.wall, getLocalBounds().toFloat(), 0.6f);
    }
    drawSectionTitle (g, tuneBox, "Stimmung", u8 ("Original: \xc2\xbc-Komma"));
    {
        g.setFont (A.monoFont (10.5f));
        g.setColour (proc.getTuningStatus().startsWith ("Fehler") ? col::rubric.brighter (0.4f) : col::dim);
        g.drawFittedText (proc.getTuningStatus(), tuningStatusArea, juce::Justification::topLeft, 4);
    }
    drawSectionTitle (g, tremBox, "Tremulant", "CC 1 = Tiefe");
    const int fz = proc.engine.frozenVoices.load();
    drawSectionTitle (g, freezeBox, "Freeze", fz > 0 ? juce::String (fz) + " eingefroren" : juce::String ("CC 66"));
    drawSectionTitle (g, distBox, u8 ("Entfernung \xc2\xb7 Doppler"));
    drawSectionTitle (g, midiBox, "MIDI");
    drawSectionTitle (g, binBox, u8 ("Kopfh\xc3\xb6rer (binaural)"), proc.isHeadphones() ? juce::String ("aktiv")
                                                                     : proc.isTransaural() ? juce::String (u8 ("f\xc3\xbcr Transaural")) : juce::String ("aus"));
    drawSectionTitle (g, trBox, "Zwei Lautsprecher", proc.isTransaural() ? juce::String (u8 ("transaural \xc2\xb7 aktiv")) : juce::String ("transaural"));
    {
        g.setFont (A.monoFont (10.0f));
        g.setColour (proc.isTransaural() ? col::dim : col::mute);
        const auto st = proc.isTransaural() ? proc.getTransauralStatus()
                                            : juce::String (u8 ("Lautsprecher links und rechts einander gegen\xc3\xbc" "ber, du dazwischen"));
        g.drawFittedText (st.replace (juce::String::fromUTF8 (" \xc2\xb7 "), "\n"), trStatusArea, juce::Justification::topLeft, 4);
    }

    g.setFont (A.monoFont (11.0f));
    g.setColour (col::dim);
    {
        drawSectionTitle (g, windBox, "Registerwind", "Windmodell");
        const bool unstable = windUnstable.getToggleState();
        g.setFont (A.sansFont (11.5f));
        g.setColour (col::mute);
        g.drawFittedText (unstable ? u8 ("Bei wenig Wind pfeift jede Pfeife ihren Schneidenton: ruhig bei ruhigem Wind, "
                                         "tiefer und leiser, wenn der Wind sinkt; die M\xc3\xbcndung zischt. Voller Wind: unver\xc3\xa4ndert.")
                                   : u8 ("Ein halb gezogenes Register wird leiser, tiefer, dumpfer und unruhig."),
                          windText, juce::Justification::topLeft, 4);
    }
    g.setFont (A.monoFont (11.0f));
    g.setColour (col::dim);
    auto midiText = midiBox.withTrimmedTop (midiTextTop);
    const char* lines[] {
        "CC 11       Wind (global) / Schweller",
        "CC 20-26    Register an/aus (Praestant ... Trompete)",
        "CC 27-33    Register-Wind",
        "CC 66       Freeze (Sostenuto-Pedal)",
        "CC 1        Tremulant-Tiefe (Modrad)",
        "CC 64       Haltepedal",
    };
    for (auto* l : lines) g.drawText (u8 (l), midiText.removeFromTop (18), juce::Justification::centredLeft);

    {
        auto r = binStatusArea;
        g.setFont (A.monoFont (11.0f));
        g.setColour (proc.isBinaural() ? col::dim : col::mute);
        const auto hrtf = proc.getSofaFile() == juce::File() ? juce::String ("HRTF: FABIAN (TU Berlin)") : "HRTF: " + proc.getSofaFile().getFileName();
        g.drawFittedText (hrtf + "\n" + proc.getBinauralStatus(), r.removeFromTop (34), juce::Justification::topLeft, 3);
        g.setFont (A.sansFont (11.5f));
        g.setColour (col::faint);
        g.drawFittedText (u8 ("Wie der IEM BinauralDecoder: MagLS-Decoder aus der HRTF berechnet. "
                              "FABIAN: Brinkmann et al., TU Berlin, CC BY 4.0."), r, juce::Justification::bottomLeft, 3);
    }
}
} // namespace rysum::ui
