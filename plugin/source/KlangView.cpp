#include "KlangView.h"

namespace rysum::ui
{
ParamRow &KlangView::row(const char *id, const juce::String &name)
{
    rows.push_back(std::make_unique<ParamRow>(proc.apvts, id, name));
    addAndMakeVisible(*rows.back());
    return *rows.back();
}

juce::TextButton &KlangView::toggle(const char *id, const juce::String &text)
{
    toggles.push_back(std::make_unique<juce::TextButton>(text));
    auto &b = *toggles.back();
    b.setClickingTogglesState(true);
    addAndMakeVisible(b);
    toggleAtts.push_back(std::make_unique<ButtonAtt>(proc.apvts, id, b));
    return b;
}

KlangView::KlangView(RysumProcessor &p) : proc(p)
{
    auto &tuningParam =
        *static_cast<juce::AudioParameterChoice *>(proc.apvts.getParameter("temperament"));
    auto addTuning = [&](int index) { temperament.addItem(tuningParam.choices[index], index + 1); };
    for (const int index : {0, 5, 7, 8, 9, kQuarterCommaMeantone, kThirdCommaMeantone,
                            kTwoSeventhsCommaMeantone, kFifthCommaMeantone, 1, 2, 3, 4, 6})
        addTuning(index);
    for (int index = 10; index < kQuarterCommaMeantone; ++index)
    {
        if (index == 17) // tempered Bohlen-Pierce
            addTuning(kJustBohlenPierce);
        addTuning(index);
    }
    temperamentAtt = std::make_unique<juce::ParameterAttachment>(
        tuningParam, [this](float value)
        { temperament.setSelectedId(juce::roundToInt(value) + 1, juce::dontSendNotification); });
    temperament.onChange = [this]
    {
        if (const int id = temperament.getSelectedId(); id > 0)
            temperamentAtt->setValueAsCompleteGesture(static_cast<float>(id - 1));
    };
    temperamentAtt->sendInitialUpdate();
    temperament.setTooltip(u8("Originalstimmung spielt die Aufnahmen in ihrer Tonh\xc3\xb6he. "
                              "Bei den Stufen-Stimmungen spielt jede Taste die n\xc3\xa4"
                              "chstgelegene aufgenommene Pfeife."));
    addAndMakeVisible(temperament);
    scalaLoad.setTooltip(u8("Eine Skala im Scala-Format (.scl) laden \xe2\x80\x93 danach optional "
                            "eine Tastenzuordnung (.kbm). "
                            "Jede MIDI-Note bekommt ihre eigene Tonh\xc3\xb6he; es spielt die "
                            "aufgenommene Pfeife, die ihr am n\xc3\xa4"
                            "chsten liegt. "
                            "Die Dateien werden im Projekt und im Patch mitgespeichert."));
    scalaLoad.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser>(
            u8("Scala-Skala (.scl) oder Tastenzuordnung (.kbm)"),
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*.scl;*.kbm");
        chooser->launchAsync(juce::FileBrowserComponent::openMode |
                                 juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser &fc)
                             {
                                 if (fc.getResult().existsAsFile())
                                     proc.loadTuningFile(fc.getResult());
                                 repaint(tuneBox);
                             });
    };
    kbmClear.setTooltip(
        u8("Tastenzuordnung entfernen: Stufe 0 auf C4 (MIDI 60), 261,625565 Hz bei A4 = 440 Hz"));
    kbmClear.onClick = [this]
    {
        proc.clearKbm();
        repaint(tuneBox);
    };
    addAndMakeVisible(scalaLoad);
    addAndMakeVisible(kbmClear);
    mtsFollow = &toggle("mtsContinuous", u8("T\xc3\xb6ne folgen"));
    mtsFollow->setTooltip(
        u8("MTS-ESP: gehaltene T\xc3\xb6ne folgen einer ge\xc3\xa4nderten Stimmung sofort. Aus: "
           "jeder Ton beh\xc3\xa4lt die Tonh\xc3\xb6he seines Anschlags."));

    loadPresetTuning.setClickingTogglesState(true);
    loadPresetTuning.onClick = [this]
    { proc.loadPresetTuning.store(loadPresetTuning.getToggleState()); };
    addAndMakeVisible(loadPresetTuning);
    extendedRange.setClickingTogglesState(true);
    extendedRange.setTooltip("MIDI 24 bis 108 in umgestimmten Modi.");
    extendedRange.onClick = [this] { proc.extendedRange.store(extendedRange.getToggleState()); };
    addAndMakeVisible(extendedRange);

    pitch = &row("pitch", "Stimmton");
    pitch->slider.setDoubleClickReturnValue(true, kStimmton440Detent);
    scalaReference.setFont(Assets::get().sansFont(14.0f));
    scalaReference.setBorderSize(juce::BorderSize<int>(0));
    scalaReference.setColour(juce::Label::textColourId, col::ink);
    scalaReference.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(scalaReference);
    scalaReferenceValue.setFont(Assets::get().monoFont(12.0f));
    scalaReferenceValue.setBorderSize(juce::BorderSize<int>(0));
    scalaReferenceValue.setColour(juce::Label::textColourId, col::dim);
    scalaReferenceValue.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(scalaReferenceValue);
    detune = &row("detune", u8("Verstimmung (zuf\xc3\xa4llig je Pfeife)"));
    detune->setDisabledValueText("Original", "Verstimmung");

    tremOn = &toggle("tremOn", "Tremulant");
    tremRate = &row("tremRate", "Tempo");
    tremDepth = &row("tremDepth", "Tiefe");

    freezeOn = &toggle("freeze", "Freeze");
    freezeOn->setTooltip(u8(
        "An: losgelassene T\xc3\xb6ne frieren als Kornwolke ein. Aus: sie klingen aus. (CC 66)"));
    smear = &row("freezeSmear", "Verwischen");
    release = &row("freezeRelease", "Ausklang beim Ausschalten");

    dsfOn = &toggle("dsfOn", "Testton an");
    dsfOn->setTooltip("Referenzton: folgt MIDI und globaler Stimmung, oder kontinuierlich mit "
                      "eigener Hz-Frequenz.");
    dsfMidi = &toggle("dsfMidi", "MIDI");
    dsfMidi->setTooltip("An: letzte gehaltene MIDI-Note, globale Stimmung und eigene Feinstimmung. "
                        "Aus: kontinuierliche Frequenz in Hz. Originalstimmung verwendet das "
                        "ausgewaehlte Register oder die nominale Referenz.");
    const auto &registerParam =
        *static_cast<juce::AudioParameterChoice *>(proc.apvts.getParameter("dsfRegister"));
    dsfRegister.addItemList(registerParam.choices, 1);
    dsfRegister.setTooltip(
        "Referenz in Originalstimmung: gemessenes Register oder 1/4-Komma MT bei A = 444,5 Hz.");
    addAndMakeVisible(dsfRegister);
    dsfRegisterAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        proc.apvts, "dsfRegister", dsfRegister);
    dsfOffset = &row("dsfOffset", "Feinstimmung");
    dsfFrequency = &row("dsfFrequency", "Frequenz");
    dsfOffset->slider.fineDragIncrement = 0.01;
    dsfOffset->slider.setDoubleClickReturnValue(true, 0.0);
    dsfFrequency->slider.fineDragIncrement = 0.1;
    dsfOffset->setTooltip("0.01 Cent je Pixel oder Mausradschritt. Alt-Ziehen: grob; Doppelklick: "
                          "0 Cent; Wert anklicken: Eingabe.");
    dsfFrequency->setTooltip("0.1 Hz je Pixel oder Mausradschritt. Alt-Ziehen: grob; Wert "
                             "anklicken: genaue Hz-Eingabe.");
    dsfSpectrum = &row("dsfSpectrum", "Spektrum");
    dsfSpectrum->setTooltip("0 = Sinus; 100 % = volles integriertes DSF-Spektrum mit bis zu 1024 "
                            "Teiltoenen. Gleichmaessigere Helligkeitsregelung.");
    dsfMixtur = &row("dsfMixtur", "Mixtur (x Grundton)");
    dsfMixtur->setTooltip("MIDI: 1 bis 9 mal Grundfrequenz nach Oktavlage. Mit Klangfarbe 0 einen "
                          "einzelnen harmonischen Teilton pruefen.");
    dsfFootage = &row("dsfFootage", "Oktavlage");
    dsfFootage->setTooltip("MIDI: 32 / 16 / 8 / 4 / 2 Fuss. 8 Fuss ist die globale Tonhoehe; "
                           "manuelle Hz bleiben absolut.");
    dsfPartials = &row("dsfPartials", "Number of Harmonic Partials");
    dsfLevel = &row("dsfLevel", "Pegel");

    // two loudspeakers (transaural): the binaural signal with the crosstalk cancelled
    trOn.setClickingTogglesState(false);
    trOn.setTooltip(u8("Schaltet die Ausgabe auf Transaural: der Kopfh\xc3\xb6rer-Klang \xc3\xbc"
                       "ber zwei Lautsprecher, "
                       "das \xc3\x9c"
                       "bersprechen zum jeweils anderen Ohr wird ausgel\xc3\xb6scht. Ordnung, HRTF "
                       "und Kopfdrehung wie bei Binaural."));
    trOn.onClick = [this]
    {
        if (auto *o = proc.apvts.getParameter("output"))
            o->setValueNotifyingHost(o->convertTo0to1(
                proc.isTransaural() ? 0.0f : (float)RysumProcessor::kTransauralOutput));
    };
    addAndMakeVisible(trOn);
    trAngle = &row("trAngle", "Lautsprecher bei");
    trAngle->setTooltip(u8("Wo die beiden Lautsprecher stehen, von vorn gemessen (symmetrisch). "
                           "90\xc2\xb0 = links und rechts, "
                           "einander gegen\xc3\xbc"
                           "ber, du in der Mitte zwischen ihnen."));
    trAmount = &row("trAmount", u8("\xc3\x9c"
                                   "bersprechen aus"));
    trAmount->setTooltip(
        u8("0 % = einfach der Kopfh\xc3\xb6rer-Klang auf die Lautsprecher, 100 % = das \xc3\x9c"
           "bersprechen ganz ausgel\xc3\xb6scht "
           "(das Bild wird r\xc3\xa4umlicher, aber empfindlicher f\xc3\xbcr die Sitzposition)"));
    trBoost = &row("trBoost", u8("h\xc3\xb6"
                                 "chste Anhebung"));
    trBoost->setTooltip(u8("Die Ausl\xc3\xb6schung hebt die Tiefen im Seitensignal an (der Kopf "
                           "ist klein gegen\xc3\xbc"
                           "ber langen Wellen). "
                           "Mehr = genauer, aber mehr Bass-Hub und Schwanken bei Kopfbewegung."));
    trEq = &toggle("trSideEq", "Seitenklang ausgleichen");
    trEq->setTooltip(u8("Nimmt auch die Klangf\xc3\xa4rbung der Lautsprecher-Richtung heraus (die "
                        "Ohrmuschel h\xc3\xb6rt sie von der Seite): "
                        "die Ohren bekommen den Kopfh\xc3\xb6rer-Klang, nicht "
                        "Kopfh\xc3\xb6rer-Klang mal Seite. Gegl\xc3\xa4ttet, h\xc3\xb6"
                        "chstens \xc2\xb1"
                        "10 dB."));

    doppler = &row("doppler", "Doppler");
    distAmount = &row("distAmount", u8("Entfernung \xe2\x86\x92 Lautst\xc3\xa4rke"));
    air = &row("air", u8("Luft (H\xc3\xb6hen in der Ferne)"));
    airInvert = &row("airInvert", u8("Luft umkehren"));
    airInvert->setTooltip(
        u8("Gibt fernen Pfeifen die H\xc3\xb6hen zur\xc3\xbc"
           "ck, die die Luft ihnen nimmt: 50 % = wie nah, 100 % = unnat\xc3\xbcrlich "
           "pr\xc3\xa4sent \xe2\x80\x93 je ferner, desto heller."));

    // headphones
    if (auto *choice =
            dynamic_cast<juce::AudioParameterChoice *>(proc.apvts.getParameter("binOrder")))
        binOrder.addItemList(choice->choices, 1);
    binOrderAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        proc.apvts, "binOrder", binOrder);
    binOrder.setTooltip(u8("Ambisonics-Ordnung vor dem Kopfh\xc3\xb6rer-Decoder: h\xc3\xb6her = "
                           "sch\xc3\xa4rfer geortet, braucht mehr Rechenzeit"));
    addAndMakeVisible(binOrder);
    binOn.setClickingTogglesState(false);
    binOn.setTooltip(u8("Schaltet die Ausgabe auf Binaural (Kopfh\xc3\xb6rer) bzw. zur\xc3\xbc"
                        "ck auf Stereo"));
    binOn.onClick = [this]
    {
        if (auto *o = proc.apvts.getParameter("output"))
            o->setValueNotifyingHost(o->convertTo0to1(
                proc.isHeadphones() ? 0.0f : (float)RysumProcessor::kBinauralOutput));
    };
    addAndMakeVisible(binOn);
    sofaLoad.setTooltip(
        u8("Eigene HRTF im SOFA-Format (SimpleFreeFieldHRIR), z. B. eine individuell gemessene"));
    sofaLoad.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser>("HRTF im SOFA-Format", proc.getSofaFile(),
                                                      "*.sofa");
        chooser->launchAsync(juce::FileBrowserComponent::openMode |
                                 juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser &fc)
                             {
                                 if (fc.getResult().existsAsFile())
                                     proc.setSofaFile(fc.getResult());
                             });
    };
    sofaReset.setTooltip(
        u8("Eingebaute HRTF: FABIAN-Kunstkopf der TU Berlin (Brinkmann et al. 2017, CC BY 4.0)"));
    sofaReset.onClick = [this] { proc.setSofaFile({}); };
    addAndMakeVisible(sofaLoad);
    speakersButton.setTooltip(u8("Ausgabe auf ein Lautsprecher-Layout (AllRAD wie im IEM "
                                 "AllRADecoder): Layout, Ordnung, Test"));
    speakersButton.onClick = [this]
    {
        if (onOpenSpeakers)
            onOpenSpeakers();
    };
    addAndMakeVisible(speakersButton);
    addAndMakeVisible(sofaReset);
    yaw = &row("binYaw", "Kopfdrehung");
    yaw->slider.setTooltip(
        u8("Dreht die ganze Szene um den H\xc3\xb6rer, als ob du den Kopf drehst (modulierbar)"));
    dfEq = &toggle("binDfEq", "Diffusfeld-Entzerrung");
    dfEq->setTooltip(
        u8("Gleicht die mittlere Klangf\xc3\xa4rbung der HRTF aus (f\xc3\xbcr "
           "diffusfeld-abgestimmte Kopfh\xc3\xb6rer). Aus: Kunstkopf-Klang wie gemessen."));

    // wind model of the stops (what a half-drawn stop does)
    if (auto *wm = proc.apvts.getParameter("windModel"))
    {
        windAtt = std::make_unique<juce::ParameterAttachment>(
            *wm,
            [this](float v)
            {
                windClassic.setToggleState(v < 0.5f, juce::dontSendNotification);
                windUnstable.setToggleState(v >= 0.5f, juce::dontSendNotification);
                repaint(windBox);
            });
        windAtt->sendInitialUpdate();
    }
    windClassic.onClick = [this]
    {
        if (windAtt)
            windAtt->setValueAsCompleteGesture(0.0f);
    };
    windUnstable.onClick = [this]
    {
        if (windAtt)
            windAtt->setValueAsCompleteGesture(1.0f);
    };
    windClassic.setTooltip(u8("Ein halb gezogenes Register (Wind-Regler je Register) wird leiser, "
                              "tiefer, dumpfer und unruhig."));
    windUnstable.setTooltip(u8("Wie klassisch, und bei wenig Wind dazu ein Schneidenton: jede "
                               "Pfeife pfeift auf ihre eigene Art, ruhig bei ruhigem Wind, "
                               "tiefer und leiser, wenn der Wind sinkt. Die M\xc3\xbcndung zischt, "
                               "ganz wenig Wind l\xc3\xa4sst die Ansprache stocken. "
                               "Bei vollem Wind klingt alles wie gewohnt."));
    addAndMakeVisible(windClassic);
    addAndMakeVisible(windUnstable);

    midiOn = &toggle("midiCC", "MIDI-CC an");
    midiOn->setTooltip(u8("Schaltet die feste CC-Belegung (Liste unten). Eigene Zuordnungen (MIDI "
                          "lernen) gelten immer."));
    midiMapButton.setTooltip(u8("Drehregler, Fader und Taster deines Keyboards auf beliebige "
                                "Regler legen (Seite Modulation)"));
    midiMapButton.onClick = [this]
    {
        if (auto *a = findParentComponentOfClass<ModUiAccess>())
            a->openMidiMap();
    };
    addAndMakeVisible(midiMapButton);
    mpeOn = &toggle("mpe", "MPE");
    mpeOn->setTooltip(u8("MPE (z. B. Lumatone, Linnstrument, Seaboard, Osmose): jede Note auf "
                         "ihrem eigenen MIDI-Kanal (2\xe2\x80\x93"
                         "16) "
                         "mit eigener Tonh\xc3\xb6henbeugung; Druck und Gleiten (CC 74) sind "
                         "Modulationsquellen je Ton. "
                         "Kanal 1 gilt f\xc3\xbcr alle (Tonh\xc3\xb6henrad als Modulationsquelle). "
                         "Bleibt beim Patch-Wechsel erhalten."));
    mpeBend = &row("mpeBend", u8("MPE-Beugung (ganzer Ausschlag)"));
    syncOn = &toggle("motionSync", "Bewegung im Tempo");
    syncOn->setTooltip(u8("Geschwindigkeiten gelten bei 120 BPM, laufen mit dem Projekttempo und "
                          "rasten auf die Taktposition ein."));
    refresh();
}

void KlangView::resized()
{
    auto r = getLocalBounds().reduced(28, 0).withTrimmedTop(22).withTrimmedBottom(22);
    const int gap = 28;
    const int cw = (r.getWidth() - 3 * gap) / 4;
    const int rh = (r.getHeight() - gap) / 2;
    auto cell = [&](int c, int rr)
    { return juce::Rectangle<int>(r.getX() + c * (cw + gap), r.getY() + rr * (rh + gap), cw, rh); };
    tuneBox = cell(0, 0);
    dsfBox = cell(1, 0);
    midiBox = cell(2, 0);
    freezeBox = cell(3, 0);
    windBox = cell(0, 1);
    tremBox = cell(1, 1);
    distBox = cell(2, 1);
    binBox = cell(3, 1);

    auto body = [](juce::Rectangle<int> b) { return b.withTrimmedTop(40); };
    {
        auto b = body(tuneBox);
        temperament.setBounds(b.removeFromTop(28));
        b.removeFromTop(12);
        const auto pitchBounds = b.removeFromTop(38);
        pitch->setBounds(pitchBounds);
        auto referenceBounds = pitchBounds.withHeight(18);
        scalaReference.setBounds(referenceBounds.removeFromLeft(130));
        scalaReferenceValue.setBounds(referenceBounds);
        mtsFollow->setBounds(pitchBounds.withHeight(24));
        detune->setBounds(b.removeFromTop(38));
        b.removeFromTop(6);
        auto t = b.removeFromTop(24);
        scalaLoad.setBounds(t.removeFromLeft(118));
        t.removeFromLeft(6);
        kbmClear.setBounds(t.removeFromLeft(106));
        b.removeFromTop(6);
        loadPresetTuning.setBounds(b.removeFromTop(24));
        b.removeFromTop(6);
        extendedRange.setBounds(b.removeFromTop(24));
        b.removeFromTop(6);
        tuningStatusArea = b;
    }
    {
        auto b = body(tremBox);
        tremOn->setBounds(b.removeFromTop(28).withWidth(140));
        b.removeFromTop(12);
        tremRate->setBounds(b.removeFromTop(38));
        tremDepth->setBounds(b.removeFromTop(38));

        auto w = body(windBox);
        auto t = w.removeFromTop(26);
        windClassic.setBounds(t.removeFromLeft(110));
        t.removeFromLeft(6);
        windUnstable.setBounds(t.removeFromLeft(110));
        w.removeFromTop(8);
        windText = w;
    }
    {
        auto b = body(freezeBox);
        freezeOn->setBounds(b.removeFromTop(28).withWidth(140));
        b.removeFromTop(8);
        auto line = b.removeFromTop(38);
        const int hw = (line.getWidth() - 14) / 2;
        smear->setBounds(line.removeFromLeft(hw));
        line.removeFromLeft(14);
        release->setBounds(line);
        // two loudspeakers (transaural) below
        trBox = freezeBox.withTrimmedTop(130);
        auto t = body(trBox);
        auto top = t.removeFromTop(26);
        trOn.setBounds(top.removeFromLeft(140));
        top.removeFromLeft(8);
        trEq->setBounds(top.removeFromLeft(200));
        t.removeFromTop(6);
        auto l1 = t.removeFromTop(38);
        trAngle->setBounds(l1.removeFromLeft(hw));
        l1.removeFromLeft(14);
        trAmount->setBounds(l1);
        auto l2 = t.removeFromTop(38);
        trBoost->setBounds(l2.removeFromLeft(hw));
        l2.removeFromLeft(14);
        trStatusArea = l2.reduced(0, 2);
        auto tone = body(dsfBox);
        auto controls = tone.removeFromTop(28);
        dsfOn->setBounds(controls.removeFromLeft(90));
        controls.removeFromLeft(4);
        dsfMidi->setBounds(controls.removeFromLeft(55));
        controls.removeFromLeft(6);
        dsfRegister.setBounds(controls);

        tone.removeFromTop(12);
        const auto frequencyBounds = tone.removeFromTop(36);
        dsfFrequency->setBounds(frequencyBounds);
        dsfOffset->setBounds(frequencyBounds);
        dsfSpectrum->setBounds(tone.removeFromTop(36));
        dsfLevel->setBounds(tone.removeFromTop(36));
        dsfPartials->setBounds(tone.removeFromTop(36));
        dsfFootage->setBounds(tone.removeFromTop(36));
        dsfMixtur->setBounds(tone.removeFromTop(36));
    }
    {
        auto b = body(distBox);
        doppler->setBounds(b.removeFromTop(38));
        distAmount->setBounds(b.removeFromTop(38));
        air->setBounds(b.removeFromTop(38));
        airInvert->setBounds(b.removeFromTop(38));
        b.removeFromTop(16);
        speakersButton.setBounds(b.removeFromTop(28).withWidth(230));
    }
    {
        auto b = body(binBox);
        auto t = b.removeFromTop(28);
        binOn.setBounds(t.removeFromLeft(124));
        t.removeFromLeft(8);
        binOrder.setBounds(t);
        b.removeFromTop(8);
        t = b.removeFromTop(26);
        sofaLoad.setBounds(t.removeFromLeft(124));
        t.removeFromLeft(8);
        sofaReset.setBounds(t.removeFromLeft(80));
        b.removeFromTop(6);
        yaw->setBounds(b.removeFromTop(38));
        b.removeFromTop(2);
        dfEq->setBounds(b.removeFromTop(26).withWidth(190));
        b.removeFromTop(6);
        binStatusArea = b;
    }
    {
        auto b = body(midiBox);
        auto t = b.removeFromTop(28);
        midiOn->setBounds(t.removeFromLeft(130));
        b.removeFromTop(6);
        syncOn->setBounds(b.removeFromTop(26));
        b.removeFromTop(8);
        t = b.removeFromTop(26);
        midiMapButton.setBounds(t.removeFromLeft(200));
        t.removeFromLeft(8);
        mpeOn->setBounds(t.removeFromLeft(80));
        b.removeFromTop(6);
        mpeBend->setBounds(b.removeFromTop(38));
        midiTextTop = b.getY() + 4 - midiBox.getY();
    }
}

void KlangView::refresh()
{
    const int tuning = juce::roundToInt(proc.apvts.getRawParameterValue("temperament")->load());
    const bool original = tuning == 0;
    extendedRange.setEnabled(!original);
    extendedRange.setAlpha(original ? 0.5f : 1.0f);
    extendedRange.setToggleState(proc.usesExtendedRange(), juce::dontSendNotification);
    loadPresetTuning.setToggleState(proc.loadPresetTuning.load(), juce::dontSendNotification);
    dsfRegister.setVisible(original);
    scalaLoad.setVisible(tuning == RysumProcessor::kScalaTuning);
    kbmClear.setVisible(tuning == RysumProcessor::kScalaTuning);
    mtsFollow->setVisible(tuning == RysumProcessor::kMtsTuning);
    const bool mts = tuning == RysumProcessor::kMtsTuning;
    const bool scala = tuning == RysumProcessor::kScalaTuning;
    pitch->setVisible(!mts && !scala);
    scalaReference.setVisible(scala);
    scalaReferenceValue.setVisible(scala);
    if (scala)
    {
        const auto reference = proc.getScalaReferenceText();
        const auto note = reference.fromFirstOccurrenceOf("Stimmton ", false, false)
                              .upToFirstOccurrenceOf(" (MIDI", false, false);
        const auto midi = reference.fromFirstOccurrenceOf("(MIDI", true, false)
                              .upToFirstOccurrenceOf("\n", false, false);
        scalaReference.setText("Stimmton " + midi, juce::dontSendNotification);
        scalaReferenceValue.setText(note + " = " + reference.fromFirstOccurrenceOf("\n", false, false),
                                    juce::dontSendNotification);
    }
    pitch->setDisabledValueText("Originalstimmung");
    pitch->setEnabled(!original && !mts);
    pitch->setAlpha(original || mts ? 0.5f : 1.0f);
    detune->setEnabled(!original);
    detune->setAlpha(original ? 0.5f : 1.0f);
    const bool followsMidi = proc.apvts.getRawParameterValue("dsfMidi")->load() > 0.5f;
    dsfFrequency->setVisible(!followsMidi);
    dsfOffset->setVisible(followsMidi);
    repaint(dsfBox);
    {
        const auto st = proc.getTuningStatus() + (proc.hasKbm() ? "k" : "");
        if (st != shownTuningStatus)
        {
            shownTuningStatus = st;
            repaint(tuneBox);
        }
        kbmClear.setEnabled(proc.hasKbm());
    }
    repaint(freezeBox);
    const bool on = proc.isHeadphones();
    if (binOn.getToggleState() != on)
    {
        binOn.setToggleState(on, juce::dontSendNotification);
        repaint(binBox);
    }
    const bool trNow = proc.isTransaural();
    if (trOn.getToggleState() != trNow)
    {
        trOn.setToggleState(trNow, juce::dontSendNotification);
        repaint(trBox);
    }
    const auto ts = proc.getTransauralStatus();
    if (ts != shownTrStatus)
    {
        shownTrStatus = ts;
        repaint(trBox);
    }
    const auto st = proc.getBinauralStatus() + "|" + proc.getSofaFile().getFullPathName();
    if (st != shownBinStatus)
    {
        shownBinStatus = st;
        repaint(binBox);
    }
}

void KlangView::paint(juce::Graphics &g)
{
    auto &A = Assets::get();
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.setOpacity(0.07f);
        drawCover(g, A.wall, getLocalBounds().toFloat(), 0.6f);
    }
    drawSectionTitle(g, tuneBox, "Stimmung");
    {
        g.setFont(A.monoFont(10.5f));
        g.setColour(proc.getTuningStatus().startsWith("Fehler") ? col::rubric.brighter(0.4f)
                                                                : col::dim);
        g.drawFittedText(proc.getTuningStatus(), tuningStatusArea, juce::Justification::topLeft, 4);
    }
    drawSectionTitle(g, tremBox, "Tremulant", "CC 1 = Tiefe");
    const int fz = proc.engine.frozenVoices.load();
    drawSectionTitle(g, freezeBox, "Freeze",
                     fz > 0 ? juce::String(fz) + " eingefroren" : juce::String("CC 66"));
    drawSectionTitle(g, dsfBox, "Testton",
                     proc.getReferenceToneFrequency() > 0.0
                         ? juce::String(proc.getReferenceToneFrequency(), 6) + " Hz"
                         : "Referenz");
    drawSectionTitle(g, distBox, u8("Entfernung \xc2\xb7 Doppler"));
    drawSectionTitle(g, midiBox, "MIDI");
    drawSectionTitle(g, binBox, u8("Kopfh\xc3\xb6rer (binaural)"),
                     proc.isHeadphones()   ? juce::String("aktiv")
                     : proc.isTransaural() ? juce::String(u8("f\xc3\xbcr Transaural"))
                                           : juce::String("aus"));
    drawSectionTitle(g, trBox, "Zwei Lautsprecher",
                     proc.isTransaural() ? juce::String(u8("transaural \xc2\xb7 aktiv"))
                                         : juce::String("transaural"));
    {
        g.setFont(A.monoFont(10.0f));
        g.setColour(proc.isTransaural() ? col::dim : col::mute);
        const auto st = proc.isTransaural()
                            ? proc.getTransauralStatus()
                            : juce::String(u8("Lautsprecher links und rechts einander gegen\xc3\xbc"
                                              "ber, du dazwischen"));
        g.drawFittedText(st.replace(juce::String::fromUTF8(" \xc2\xb7 "), "\n"), trStatusArea,
                         juce::Justification::topLeft, 4);
    }

    g.setFont(A.monoFont(11.0f));
    g.setColour(col::dim);
    {
        drawSectionTitle(g, windBox, "Registerwind", "Windmodell");
        const bool unstable = windUnstable.getToggleState();
        g.setFont(A.sansFont(11.5f));
        g.setColour(col::mute);
        g.drawFittedText(
            unstable ? u8("Bei wenig Wind pfeift jede Pfeife ihren Schneidenton: ruhig bei ruhigem "
                          "Wind, "
                          "tiefer und leiser, wenn der Wind sinkt; die M\xc3\xbcndung zischt. "
                          "Voller Wind: unver\xc3\xa4ndert.")
                     : u8("Ein halb gezogenes Register wird leiser, tiefer, dumpfer und unruhig."),
            windText, juce::Justification::topLeft, 4);
    }
    g.setFont(A.monoFont(11.0f));
    g.setColour(col::dim);
    auto midiText = midiBox.withTrimmedTop(midiTextTop);
    const char *lines[]{
        "CC 11       Wind (global) / Schweller",
        "CC 20-26    Register an/aus (Praestant ... Trompete)",
        "CC 27-33    Register-Wind",
        "CC 66       Freeze (Sostenuto-Pedal)",
        "CC 1        Tremulant-Tiefe (Modrad)",
        "CC 64       Haltepedal",
    };
    for (auto *l : lines)
        g.drawText(u8(l), midiText.removeFromTop(18), juce::Justification::centredLeft);

    {
        auto r = binStatusArea;
        g.setFont(A.monoFont(11.0f));
        g.setColour(proc.isBinaural() ? col::dim : col::mute);
        const auto hrtf = proc.getSofaFile() == juce::File()
                              ? juce::String("HRTF: FABIAN (TU Berlin)")
                              : "HRTF: " + proc.getSofaFile().getFileName();
        g.drawFittedText(hrtf + "\n" + proc.getBinauralStatus(), r.removeFromTop(34),
                         juce::Justification::topLeft, 3);
        g.setFont(A.sansFont(11.5f));
        g.setColour(col::faint);
        g.drawFittedText(u8("Wie der IEM BinauralDecoder: MagLS-Decoder aus der HRTF berechnet. "
                            "FABIAN: Brinkmann et al., TU Berlin, CC BY 4.0."),
                         r, juce::Justification::bottomLeft, 3);
    }
}
} // namespace rysum::ui
