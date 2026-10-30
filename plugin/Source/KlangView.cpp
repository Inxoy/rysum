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

    doppler    = &row ("doppler", "Doppler");
    distAmount = &row ("distAmount", u8 ("Entfernung \xe2\x86\x92 Lautst\xc3\xa4rke"));
    air        = &row ("air", u8 ("Luft (H\xc3\xb6hen in der Ferne)"));

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
            o->setValueNotifyingHost (o->convertTo0to1 (proc.isBinaural() ? 0.0f : (float) RysumProcessor::kBinauralOutput));
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
        b.removeFromTop (12);
        smear->setBounds (b.removeFromTop (38));
        release->setBounds (b.removeFromTop (38));
    }
    {
        auto b = body (distBox);
        doppler->setBounds (b.removeFromTop (38));
        distAmount->setBounds (b.removeFromTop (38));
        air->setBounds (b.removeFromTop (38));
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
    const bool on = proc.isBinaural();
    if (binOn.getToggleState() != on) { binOn.setToggleState (on, juce::dontSendNotification); repaint (binBox); }
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
    drawSectionTitle (g, binBox, u8 ("Kopfh\xc3\xb6rer (binaural)"), proc.isBinaural() ? juce::String ("aktiv") : juce::String ("aus"));

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
