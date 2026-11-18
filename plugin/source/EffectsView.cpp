#include "EffectsView.h"

namespace rysum::ui
{
static juce::String U(const char *s)
{
    return juce::String::fromUTF8(s);
}

ParamRow &EffectsView::row(const char *id, const juce::String &name)
{
    rows.push_back(std::make_unique<ParamRow>(proc.apvts, id, name, true));
    addAndMakeVisible(*rows.back());
    return *rows.back();
}

juce::TextButton &EffectsView::toggle(const char *id, const juce::String &text)
{
    toggles.push_back(std::make_unique<juce::TextButton>(text));
    auto &b = *toggles.back();
    b.setClickingTogglesState(true);
    addAndMakeVisible(b);
    toggleAtts.push_back(rysum::ui::attachButton(proc.apvts, id, b));
    return b;
}

juce::ComboBox &EffectsView::combo(const char *id)
{
    combos.push_back(std::make_unique<juce::ComboBox>());
    auto &c = *combos.back();
    if (auto *choice = dynamic_cast<juce::AudioParameterChoice *>(proc.apvts.getParameter(id)))
        c.addItemList(choice->choices, 1);
    addAndMakeVisible(c);
    comboAtts.push_back(std::make_unique<ComboAtt>(proc.apvts, id, c));
    return c;
}

float EffectsView::value(const char *id) const
{
    if (auto *v = proc.apvts.getRawParameterValue(id))
        return v->load();
    return 0.0f;
}

EffectsView::EffectsView(RysumProcessor &p) : proc(p)
{
    // ---- own sounds
    openFolder.setTooltip(rysum::tr("\xc3\x96"
                            "ffnet den Ordner im Explorer (der Standardordner samples/noise/eigene "
                            "wird bei Bedarf angelegt) \xe2\x80\x93 "
                            "Dateien hineinlegen, dann \xe2\x80\x9eNeu einlesen\xe2\x80\x9c. "
                            "Kurze, helle Ger\xc3\xa4usche orten sich am besten."));
    rescan.setTooltip(rysum::tr("Liest den Ordner neu ein, ohne die Pfeifen neu zu laden"));
    openFolder.onClick = [this]
    {
        auto dir = proc.getCustomFolder();
        if (dir == juce::File())
            return;
        if (!dir.isDirectory())
            dir.createDirectory();
        dir.startAsProcess();
    };
    rescan.onClick = [this] { proc.reloadCustomSounds(); };
    chooseFolder.setTooltip(rysum::tr("Einen beliebigen Ordner mit Ger\xc3\xa4uschen w\xc3\xa4hlen "
                              "(WAV/AIFF/FLAC/OGG bis 20 s, auch in Unterordnern, bis 64 Dateien). "
                              "Wird im Projekt und in Patches gespeichert."));
    chooseFolder.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser>(rysum::tr("Ordner mit eigenen Ger\xc3\xa4uschen"),
                                                      proc.getCustomFolder());
        chooser->launchAsync(juce::FileBrowserComponent::openMode |
                                 juce::FileBrowserComponent::canSelectDirectories,
                             [this](const juce::FileChooser &fc)
                             {
                                 const auto r = fc.getResult();
                                 if (r.isDirectory())
                                     proc.setCustomFolder(r);
                                 else if (r.existsAsFile())
                                     proc.setCustomFolder(r.getParentDirectory());
                                 repaint();
                             });
    };
    resetFolder.setTooltip(rysum::tr("Zur\xc3\xbc"
                             "ck zum Standardordner samples/noise/eigene"));
    resetFolder.onClick = [this]
    {
        proc.setCustomFolder({});
        repaint();
    };
    addAndMakeVisible(chooseFolder);
    addChildComponent(resetFolder);
    addAndMakeVisible(openFolder);
    addAndMakeVisible(rescan);
    custPlace = &combo("fxCustPlace");
    custPlace->setTooltip(rysum::tr("Wo die Ger\xc3\xa4usche erklingen: an einer Pfeife eines gezogenen "
                            "Registers, irgendwo im Raum, am Spieltisch oder auf einer Kreisbahn"));
    custRows = {&row("fxCustLevel", rysum::tr("Lautst\xc3\xa4rke")),
                &row("fxCustDown", rysum::tr("Beim Anschlag (Wahrscheinlichkeit)")),
                &row("fxCustUp", rysum::tr("Beim Loslassen")),
                &row("fxCustPitch", rysum::tr("Tonh\xc3\xb6he folgt der Taste")),
                &row("fxCustScatter", rysum::tr("Streuung (Echos, Zeit, Ort)")),
                &row("fxCustWander", rysum::tr("Wandern"))};

    // ---- tracker path
    pathRows = {&row("fxPathLevel", rysum::tr("Lautst\xc3\xa4rke")),
                &row("fxPathTime", rysum::tr("Laufzeit")),
                &row("fxPathTicks", rysum::tr("Schritte")),
                &row("fxPathSpread", rysum::tr("Streuung des Weges")),
                nullptr,
                nullptr};
    pathWait = &toggle("fxPathWait", rysum::tr("Pfeifen warten"));
    pathWait->setTooltip(
        rysum::tr("Die Pfeife spricht erst, wenn die Klicks bei ihr ankommen (Laufzeit)."));

    // ---- crackle
    crackRows = {&row("fxCrackLevel", rysum::tr("Lautst\xc3\xa4rke")),
                 &row("fxCrackRate", rysum::tr("Dichte (je Pfeife)")),
                 &row("fxCrackLen", rysum::tr("L\xc3\xa4nge eines Knacksers")),
                 nullptr,
                 nullptr,
                 nullptr};

    // ---- wind leaks
    leakRows = {&row("fxLeakLevel", rysum::tr("Lautst\xc3\xa4rke")),
                &row("fxLeakCount", rysum::tr("Anzahl")),
                &row("fxLeakFreq", rysum::tr("Tonh\xc3\xb6he")),
                &row("fxLeakWander", rysum::tr("Wandern")),
                nullptr,
                nullptr};
    windRound = &toggle("fxWindRound", rysum::tr("Gebl\xc3\xa4se ringsum"));
    windRound->setTooltip(
        rysum::tr("Das Gebl\xc3\xa4serauschen (Wind) kommt von allen Seiten statt als ein Signal."));

    // ---- upper partials
    partRows = {&row("fxPartSpread", rysum::tr("Versatz der Obert\xc3\xb6ne")),
                &row("fxPartSplit", rysum::tr("Trennung (\xc3\x97 Grundton)")),
                &row("fxPartTurn", rysum::tr("Kreisen")),
                nullptr,
                nullptr,
                nullptr};

    // ---- echoes
    echoSync = &toggle("fxEchoSync", rysum::tr("Im Tempo"));
    echoDiv = &combo("fxEchoDiv");
    echoTime = &row("fxEchoTime", rysum::tr("Zeit"));
    echoRows = {&row("fxEchoLevel", rysum::tr("Lautst\xc3\xa4rke")),
                &row("fxEchoRepeats", rysum::tr("Wiederholungen")),
                &row("fxEchoFeedback", rysum::tr("Abnahme je Wiederholung")),
                &row("fxEchoTurn", rysum::tr("Drehung je Wiederholung")),
                &row("fxEchoTilt", rysum::tr("Neigung je Wiederholung")),
                &row("fxEchoDamp", rysum::tr("D\xc3\xa4mpfung"))};
    for (int i = 0; i < getNumChildComponents(); ++i)
        page0.push_back(getChildComponent(i));

    // ---- second tab: the tuning in space, photographic operations
    const int before = getNumChildComponents();
    beatFrom = &combo("fxBeatIdeal");
    beatFrom->setTooltip(rysum::tr("Woraus die Schwebungen berechnet werden: aus der Stimmung, wie sie "
                           "gemeint ist (reine Terzen stehen still), "
                           "oder aus den Pfeifen, wie sie aufgenommen sind (\xc2\xb1"
                           "5 ct: auch reine Terzen schweben ein wenig)."));
    ghostRows = {&row("fxGhostLevel", rysum::tr("Lautst\xc3\xa4rke")),
                 &row("fxGhostPlace", rysum::tr("Ort (zwischen \xe2\x86\x92 unten)")),
                 &row("fxGhostCons", rysum::tr("nur Konsonanzen")),
                 nullptr,
                 nullptr,
                 nullptr};
    dexMirror = &combo("fxDexMirror");
    dexMirror->setTooltip(
        rysum::tr("Das alte Bild gespiegelt: links / rechts, vorn / hinten, oben / unten"));
    dexRows = {&row("fxDexMix", rysum::tr("Menge")),
               &row("fxDexTime", rysum::tr("Abstand (Sekunden davor)")),
               &row("fxDexTurn", rysum::tr("Drehung")),
               &row("fxDexFade", rysum::tr("Verblassen")),
               &row("fxDexAgain", rysum::tr("mehrfach belichten")),
               nullptr};
    expoRows = {&row("fxExpoMix", rysum::tr("Menge")),
                &row("fxExpoTime", rysum::tr("Belichtungszeit")),
                &row("fxExpoBright", rysum::tr("Helligkeit der Spuren")),
                nullptr,
                nullptr,
                nullptr};
    for (int i = before; i < getNumChildComponents(); ++i)
        page1.push_back(getChildComponent(i));

    // ---- third tab: the living organ
    const int before2 = getNumChildComponents();
    blower = &combo("lvBlower");
    blower->setTooltip(rysum::tr("Woher der Wind kommt: ein Gebl\xc3\xa4se liefert stetig nach (gro\xc3\x9f"
                         "e Akkorde dr\xc3\xbc"
                         "cken den Druck, solange sie klingen), "
                         "oder ein Kalkant tritt den Balg \xe2\x80\x93 dazwischen sinkt er, und "
                         "wer zu viel spielt, dem geht der Wind aus."));
    treadDiv = &combo("lvTreadDiv");
    treadRate = &row("lvTreadRate", rysum::tr("Tritte"));
    bellowsRows = {&row("lvBellows", rysum::tr("Windverbrauch")),
                   &row("lvBellowsSize", rysum::tr("Gr\xc3\xb6\xc3\x9f"
                                           "e des Balgs")),
                   treadRate,
                   &row("lvTreadNoise", rysum::tr("Trittger\xc3\xa4usch")),
                   nullptr,
                   nullptr};
    bellowsRows[0]->setTooltip(rysum::tr("0 % = unendlich viel Wind (wie bisher). Dar\xc3\xbc"
                                 "ber teilen sich alle klingenden Pfeifen einen Vorrat: "
                                 "tiefe Pfeifen und viele Register verbrauchen am meisten. Fehlt "
                                 "Wind, werden alle Pfeifen tiefer, leiser, dumpfer und unruhig."));
    palletSrc = &combo("lvPalletSrc");
    palletRows = {&row("lvPallet", rysum::tr("Wie weit das Ventil geschlossen bleibt")),
                  nullptr,
                  nullptr,
                  nullptr,
                  nullptr,
                  nullptr};
    walkShape = &combo("lvWalkShape");
    walkRows = {&row("lvListenX", rysum::tr("links / rechts")),
                &row("lvListenY", rysum::tr("zur\xc3\xbc"
                                    "ck / vor")),
                &row("lvWalkRate", "Tempo"),
                &row("lvWalkSize", rysum::tr("Weite des Wegs")),
                nullptr,
                nullptr};
    sympRows = {&row("lvSympLevel", rysum::tr("Lautst\xc3\xa4rke")),
                &row("lvSympDecay", rysum::tr("Nachklang")),
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    wearRows = {&row("lvWear", rysum::tr("Wie stark man ihn h\xc3\xb6rt")),
                &row("lvWearTime", rysum::tr("Lebensdauer einer Pfeife")),
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    wearReset.setTooltip(rysum::tr("Setzt den Verschlei\xc3\x9f aller Pfeifen zur\xc3\xbc"
                           "ck. Er wird mit dem Projekt gespeichert (nicht in Patches)."));
    wearReset.onClick = [this] { proc.resetWear(); };
    addAndMakeVisible(wearReset);
    pureRows = {&row("lvPure", rysum::tr("Menge")),
                &row("lvPureTime", rysum::tr("Zeit bis rein")),
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    netOn = &toggle("lvNetOn", rysum::tr("Wanderer an"));
    netOn->setTooltip(rysum::tr("Solange Tasten gehalten werden, wandert ein Spieler durch das Tonnetz der "
                        "Stimmung und spielt jede Taste, an der er vorbeikommt. "
                        "Er beginnt bei der zuletzt angeschlagenen Taste. Nicht mit freier "
                        "Stimmung (Scala / MTS) und MPE."));
    netPath = &combo("lvNetPath");
    netDiv = &combo("lvNetDiv");
    netRate = &row("lvNetRate", "Tempo");
    netRows = {netRate, &row("lvNetGate", rysum::tr("Tonl\xc3\xa4nge")), nullptr, nullptr, nullptr,
               nullptr};
    impRows = {&row("lvImpLevel", rysum::tr("Lautst\xc3\xa4rke")),
               &row("lvImpRing", rysum::tr("Nachklang des Rohrs")),
               nullptr,
               nullptr,
               nullptr,
               nullptr};
    impRows[0]->setTooltip(rysum::tr("Wie laut Pfeifen klopfen, die gegen W\xc3\xa4nde, Boden und Decke "
                             "des Saals fliegen (aus = still). "
                             "Nur mit den Bewegungen Flug, Fall, Schwarm und Str\xc3\xb6mung "
                             "(Raum-Seite). Je schneller der Aufprall, "
                             "desto lauter und heller; eine schluckende Wand d\xc3\xa4mpft ihn."));
    impSrc = &combo("lvImpSrc");
    impSrc->setTooltip(rysum::tr("Nur klingende Pfeifen klopfen \xe2\x80\x93 oder alle Pfeifen gezogener "
                         "Register, auch die stummen."));
    playerMode = &combo("lvPlayerMode");
    playerMode->setTooltip(
        rysum::tr("Der H\xc3\xb6rer - du im 3D-Saal, oder der H\xc3\xb6rer auf seinem Weg - bewegt die Pfeifen: wie eine "
          "Masse, zu der sie fallen (zieht an), vor der sie fliehen (st\xc3\xb6\xc3\x9ft ab), um die sie "
          "kreisen (Umlaufbahn) oder die sie mitrei\xc3\x9ft (Sog). Wirkt auf Register, deren Bewegung "
          "(Raum-Seite) im Saal stattfindet: Schweben, Flug, Fall, Schwarm, Str\xc3\xb6mung."));
    playerRows = {&row("lvPlayerForce", rysum::tr("Kraft")),
                  &row("lvPlayerRange", rysum::tr("Reichweite")),
                  nullptr,
                  nullptr,
                  nullptr,
                  nullptr};
    for (int i = before2; i < getNumChildComponents(); ++i)
        page2.push_back(getChildComponent(i));

    // ---- fourth tab: the "Summe"
    {
        const int before3 = getNumChildComponents();
        eqOn = &toggle("mstEqOn", rysum::tr("EQ an"));
        eqOn->setTooltip(rysum::tr("Drei B\xc3\xa4nder auf allem, was hinausgeht: Tiefen (Kuhschwanz), Mitten "
                           "(Glocke mit G\xc3\xbcte), H\xc3\xb6hen (Kuhschwanz)."));
        eqRows = {&row("mstLowHz", rysum::tr("Tiefen bei")),       &row("mstLowDb", rysum::tr("Tiefen")),
                  &row("mstMidHz", rysum::tr("Mitten bei")),       &row("mstMidDb", rysum::tr("Mitten")),
                  &row("mstMidQ", rysum::tr("G\xc3\xbcte")),     &row("mstHighHz", rysum::tr("H\xc3\xb6hen bei")),
                  &row("mstHighDb", rysum::tr("H\xc3\xb6hen"))};
        compOn = &toggle("mstCompOn", rysum::tr("Kompressor an"));
        compOn->setTooltip(rysum::tr("Ein Kompressor auf allem, was hinausgeht: der lauteste Kanal entscheidet, "
                             "alle Kan\xc3\xa4le werden gleich leiser (das Raumbild bleibt stehen)."));
        compRows = {&row("mstThresh", rysum::tr("Schwelle")), &row("mstRatio", rysum::tr("Verh\xc3\xa4ltnis")),
                    &row("mstAttack", rysum::tr("Ansprache")), &row("mstRelease", rysum::tr("Ausklang")),
                    &row("mstMakeup", rysum::tr("Aufholen"))};
        for (int i = before3; i < getNumChildComponents(); ++i)
            page3.push_back(getChildComponent(i));
    }

    for (auto *b : {&tabSounds, &tabLight, &tabLive, &tabMaster})
    {
        b->setClickingTogglesState(false);
        addAndMakeVisible(*b);
    }
    tabSounds.onClick = [this] { showTab(0); };
    tabLight.onClick = [this] { showTab(1); };
    tabLive.onClick = [this] { showTab(2); };
    tabMaster.onClick = [this] { showTab(3); };
    tabMaster.setTooltip(rysum::tr("Die Summe: EQ und Kompressor auf allem, was hinausgeht (Mischen, Mastern)"));
    tabLive.setTooltip(rysum::tr("Die Orgel als ein K\xc3\xb6rper: gemeinsamer Wind, Ventile, ein "
                         "H\xc3\xb6rer, der geht, Pfeifen, die altern und mitklingen, "
                         "eine Stimmung, die sich rein zieht, ein Wanderer im Tonnetz"));
    showTab(0);
    refresh();
}

void EffectsView::showTab(int t)
{
    tab = t;
    for (auto *c : page0)
        c->setVisible(t == 0);
    for (auto *c : page1)
        c->setVisible(t == 1);
    for (auto *c : page2)
        c->setVisible(t == 2);
    for (auto *c : page3)
        c->setVisible(t == 3);
    if (t == 0)
        resetFolder.setVisible(proc.hasOwnCustomFolder());
    tabSounds.setToggleState(t == 0, juce::dontSendNotification);
    tabLight.setToggleState(t == 1, juce::dontSendNotification);
    tabLive.setToggleState(t == 2, juce::dontSendNotification);
    tabMaster.setToggleState(t == 3, juce::dontSendNotification);
    repaint();
}

void EffectsView::resized()
{
    tabMaster.setBounds(getWidth() - 28 - 70, 4, 70, 18);
    tabLive.setBounds(tabMaster.getX() - 4 - 130, 4, 130, 18);
    tabLight.setBounds(tabLive.getX() - 4 - 170, 4, 170, 18);
    {
        // the "Summe": the EQ on the left, the compressor on the right
        auto r = getLocalBounds().reduced(28, 0).withTrimmedTop(22).withTrimmedBottom(18);
        const int gap = 40;
        eqBox = r.removeFromLeft((r.getWidth() - gap) / 2);
        r.removeFromLeft(gap);
        compBox = r;
        auto b = eqBox.withTrimmedTop(38);
        eqOn->setBounds(b.removeFromTop(24).removeFromLeft(110));
        b.removeFromTop(8);
        eqCurve = b.removeFromTop(juce::jmin(220, b.getHeight() / 2));
        b.removeFromTop(10);
        for (auto *x : eqRows)
            x->setBounds(b.removeFromTop(30).withTrimmedBottom(2));
        eqNote = b;
        auto c = compBox.withTrimmedTop(38);
        compOn->setBounds(c.removeFromTop(24).removeFromLeft(130));
        c.removeFromTop(8);
        compMeter = c.removeFromTop(juce::jmin(220, c.getHeight() / 2));
        c.removeFromTop(10);
        for (auto *x : compRows)
            x->setBounds(c.removeFromTop(30).withTrimmedBottom(2));
        compNote = c;
    }
    tabSounds.setBounds(tabLight.getX() - 4 - 80, 4, 80, 18);
    auto r = getLocalBounds().reduced(28, 0).withTrimmedTop(22).withTrimmedBottom(18);
    const int gap = 28;
    const int cw = (r.getWidth() - 2 * gap) / 3;
    const int rh = (r.getHeight() - gap) / 2;
    auto cell = [&](int c, int rr)
    { return juce::Rectangle<int>(r.getX() + c * (cw + gap), r.getY() + rr * (rh + gap), cw, rh); };
    custBox = cell(0, 0);
    pathBox = cell(1, 0);
    crackBox = cell(2, 0);
    leakBox = cell(0, 1);
    partBox = cell(1, 1);
    echoBox = cell(2, 1);
    constexpr int kRow = 30;

    auto body = [](juce::Rectangle<int> b) { return b.withTrimmedTop(38); };
    auto stack = [&](juce::Rectangle<int> &b, std::array<ParamRow *, 6> &rs)
    {
        for (auto *x : rs)
            if (x != nullptr)
                x->setBounds(b.removeFromTop(kRow).withTrimmedBottom(2));
    };
    {
        auto b = body(custBox);
        auto t = b.removeFromTop(24);
        chooseFolder.setBounds(t.removeFromLeft(132));
        t.removeFromLeft(6);
        openFolder.setBounds(t.removeFromLeft(70));
        t.removeFromLeft(6);
        rescan.setBounds(t.removeFromLeft(104));
        t.removeFromLeft(6);
        resetFolder.setBounds(t.removeFromLeft(24));
        b.removeFromTop(6);
        custPlace->setBounds(b.removeFromTop(24));
        b.removeFromTop(6);
        stack(b, custRows);
        custNote = b;
    }
    {
        auto b = body(pathBox);
        stack(b, pathRows);
        b.removeFromTop(4);
        pathWait->setBounds(b.removeFromTop(24).withWidth(150));
        b.removeFromTop(8);
        pathNote = b;
    }
    {
        auto b = body(crackBox);
        stack(b, crackRows);
        b.removeFromTop(8);
        crackNote = b;
    }
    {
        auto b = body(leakBox);
        stack(b, leakRows);
        b.removeFromTop(4);
        windRound->setBounds(b.removeFromTop(24).withWidth(150));
        b.removeFromTop(8);
        leakNote = b;
    }
    {
        auto b = body(partBox);
        stack(b, partRows);
        b.removeFromTop(6);
        auto rest = b;
        partCompass = rest.removeFromRight(juce::jmin(rest.getHeight(), 130));
        partNote = rest.withTrimmedRight(8);
    }
    {
        auto b = body(echoBox);
        echoCompass = b.removeFromRight(104).withSizeKeepingCentre(104, 104).withY(b.getY() + 30);
        b.removeFromRight(10);
        auto t = b.removeFromTop(24);
        echoSync->setBounds(t.removeFromLeft(84));
        t.removeFromLeft(6);
        echoDiv->setBounds(t);
        b.removeFromTop(4);
        echoTime->setBounds(b.removeFromTop(kRow).withTrimmedBottom(2));
        stack(b, echoRows);
    }
    // ---- second tab: Schwebung | Differenztoene | (note) / Doppelbelichtung | Belichtung
    beatBox = cell(0, 0);
    ghostBox = cell(1, 0);
    dexBox = cell(0, 1).withRight(cell(1, 1).getRight());
    expoBox = cell(2, 1);
    {
        auto b = body(beatBox);
        beatFrom->setBounds(b.removeFromTop(24));
        b.removeFromTop(8);
        beatNote = b;
    }
    {
        auto b = body(ghostBox);
        stack(b, ghostRows);
        b.removeFromTop(8);
        ghostNote = b;
    }
    {
        auto b = body(dexBox);
        auto left = b.removeFromLeft(cw);
        dexMirror->setBounds(left.removeFromTop(24));
        left.removeFromTop(6);
        stack(left, dexRows);
        b.removeFromLeft(gap);
        dexNote = b;
    }
    {
        auto b = body(expoBox);
        stack(b, expoRows);
        b.removeFromTop(8);
        expoNote = b;
    }
    // ---- third tab, four columns: Balg | Ventil | Hoerer | Mitklingen / Verschleiss (two columns)
    // | Reinheit | Wanderer
    const int cw4 = (r.getWidth() - 3 * gap) / 4;
    auto cell4 = [&](int c, int rr, int span = 1)
    {
        return juce::Rectangle<int>(r.getX() + c * (cw4 + gap), r.getY() + rr * (rh + gap),
                                    span * cw4 + (span - 1) * gap, rh);
    };
    bellowsBox = cell4(0, 0);
    palletBox = cell4(1, 0);
    walkBox = cell4(2, 0);
    sympBox = cell4(3, 0);
    wearBox = cell4(0, 1);
    impBox = cell4(1, 1);
    pureBox = cell4(2, 1);
    netBox = cell4(3, 1);
    {
        auto b = body(bellowsBox);
        blower->setBounds(b.removeFromTop(24));
        b.removeFromTop(6);
        for (int i = 0; i < 2; ++i)
            bellowsRows[(size_t)i]->setBounds(b.removeFromTop(kRow).withTrimmedBottom(2));
        auto t = b.removeFromTop(kRow).withTrimmedBottom(2);
        treadRate->setBounds(t);
        treadDiv->setBounds(t.withTrimmedTop(3).withTrimmedBottom(1));
        bellowsRows[3]->setBounds(b.removeFromTop(kRow).withTrimmedBottom(2));
        b.removeFromTop(10);
        bellowsMeter = b.removeFromTop(30);
        b.removeFromTop(8);
        bellowsNote = b;
    }
    {
        auto b = body(palletBox);
        palletSrc->setBounds(b.removeFromTop(24));
        b.removeFromTop(6);
        stack(b, palletRows);
        b.removeFromTop(8);
        palletNote = b;
    }
    {
        auto b = body(walkBox);
        walkShape->setBounds(b.removeFromTop(24));
        b.removeFromTop(6);
        stack(b, walkRows);
        b.removeFromTop(6);
        walkMap = b;
    }
    {
        auto b = body(sympBox);
        stack(b, sympRows);
        b.removeFromTop(8);
        sympNote = b;
    }
    {
        auto b = body(wearBox);
        stack(b, wearRows);
        b.removeFromTop(4);
        wearReset.setBounds(b.removeFromTop(24).withWidth(150));
        b.removeFromTop(8);
        wearNote = {}; // (the map below says it: one row per stop)
        wearMap = b;
    }
    {
        auto b = body(impBox);
        stack(b, impRows);
        b.removeFromTop(2);
        impSrc->setBounds(b.removeFromTop(24));
        // the player and the pipes, in the same box (both are about the pipes as bodies in the Saal)
        b.removeFromTop(10);
        playerLabel = b.removeFromTop(16);
        playerMode->setBounds(b.removeFromTop(24));
        b.removeFromTop(4);
        stack(b, playerRows);
        b.removeFromTop(6);
        impNote = b;
    }
    {
        auto b = body(pureBox);
        stack(b, pureRows);
        b.removeFromTop(8);
        pureNote = b;
    }
    {
        auto b = body(netBox);
        auto t = b.removeFromTop(24);
        netOn->setBounds(t.removeFromLeft(104));
        t.removeFromLeft(6);
        netDiv->setBounds(t);
        b.removeFromTop(6);
        netPath->setBounds(b.removeFromTop(24));
        b.removeFromTop(6);
        stack(b, netRows);
        b.removeFromTop(8);
        netNote = b;
    }
}

void EffectsView::refresh()
{
    const bool sync = value("fxEchoSync") > 0.5f;
    echoDiv->setEnabled(sync);
    echoTime->setEnabled(!sync);
    echoTime->setAlpha(sync ? 0.35f : 1.0f);
    echoDiv->setAlpha(sync ? 1.0f : 0.45f);
    const int n = proc.getNumCustomSounds();
    if (n != shownCustom)
    {
        shownCustom = n;
        repaint(custBox);
    }
    if (tab == 0 && resetFolder.isVisible() != proc.hasOwnCustomFolder())
    {
        resetFolder.setVisible(proc.hasOwnCustomFolder());
        repaint(custBox);
    }
    repaint(echoCompass.expanded(14));
    repaint(partCompass.expanded(14));
    if (tab == 3)
    {
        // the compressor's meter, and the EQ curve when a lane moves it
        const float red = proc.masterBus.reductionDb.load();
        if (std::abs(red - shownReduction) > 0.05f)
        {
            shownReduction = red;
            repaint(compMeter.expanded(4));
        }
        repaint(eqCurve.expanded(4));
    }
    if (tab == 2)
    {
        // the Kalkant treads freely (slider) or in tempo (note value); the walker steps freely or
        // in tempo
        const int bl = juce::roundToInt(value("lvBlower"));
        treadRate->setVisible(bl != 2);
        treadRate->setAlpha(bl == 1 ? 1.0f : 0.35f);
        treadDiv->setVisible(bl == 2);
        bellowsRows[3]->setAlpha(bl == 0 ? 0.35f : 1.0f);
        netRate->setAlpha(value("lvNetDiv") < 0.5f ? 1.0f : 0.35f);
        for (auto a : {bellowsMeter, walkMap, wearMap})
            repaint(a.expanded(4));
        for (auto a : {sympBox, pureBox, netBox})
            repaint(a.withHeight(36));
        // knocks per second (smoothed), for the title
        const double now = juce::Time::getMillisecondCounterHiRes();
        const uint32_t cnt = proc.engine.impactCount.load();
        if (impTime > 0.0 && now > impTime)
        {
            const float inst = (float)(cnt - impCountShown) / (float)((now - impTime) * 0.001);
            const float was = impRate;
            impRate += 0.15f * (inst - impRate);
            if (std::abs(impRate - was) > 0.05f || (impRate < 0.05f) != (was < 0.05f))
                repaint(impBox.withHeight(36));
        }
        impCountShown = cnt;
        impTime = now;
    }
}

/** the third tab: section heads, the bellows, the listener's walk, the wear of every pipe */
void EffectsView::paintLive(juce::Graphics &g)
{
    auto &A = Assets::get();
    auto &e = proc.engine;
    auto noteIn = [&](juce::Rectangle<int> r, const juce::String &text)
    {
        g.setFont(A.sansFont(12.5f));
        g.setColour(col::mute);
        if (r.getHeight() > 14)
            g.drawFittedText(text, r, juce::Justification::topLeft,
                             juce::jmax(1, r.getHeight() / 15));
    };
    const int ringing = e.sympNow.load();
    const float pureCt = e.pureNow.load();
    const int netKey = e.netKeyNow.load();
    drawSectionTitle(g, bellowsBox, rysum::tr("Balg"), rysum::tr("ein Wind f\xc3\xbcr alle"));
    drawSectionTitle(g, palletBox, rysum::tr("Ventil"),
                     rysum::tr("wie die Taste \xc3\xb6"
                       "ffnet"));
    drawSectionTitle(g, walkBox, rysum::tr("H\xc3\xb6rer"), rysum::tr("geht durch die Orgel"));
    drawSectionTitle(g, sympBox, rysum::tr("Mitklingen"),
                     ringing > 0 ? juce::String(ringing) + (ringing == 1 ? rysum::tr(" Pfeife klingt mit")
                                                                         : rysum::tr(" Pfeifen klingen mit"))
                                 : rysum::tr("stumme Pfeifen antworten"));
    drawSectionTitle(g, wearBox, rysum::tr("Verschlei\xc3\x9f"),
                     rysum::tr("das St\xc3\xbc"
                       "ck nutzt die Orgel ab"));
    drawSectionTitle(g, impBox, rysum::tr("Aufprall"),
                     impRate > 0.05f ? juce::String(impRate, impRate < 10.0f ? 1 : 0) +
                                           rysum::tr(" Schl\xc3\xa4ge pro Sekunde")
                                     : rysum::tr("Pfeifen gegen die W\xc3\xa4nde"));
    g.setColour(col::mute);
    g.setFont(Assets::get().monoFont(11.0f));
    g.drawText(rysum::tr("Spieler bewegt die Pfeifen"), playerLabel, juce::Justification::centredLeft);
    drawSectionTitle(g, pureBox, rysum::tr("Reinheit"),
                     pureCt > 0.05f ? rysum::tr("zieht bis ") + juce::String(pureCt, 1) + " ct"
                                    : rysum::tr("Akkorde ziehen sich rein"));
    drawSectionTitle(
        g, netBox, "Tonnetz-Wanderer",
        netKey >= 0
            ? rysum::tr("bei ") + helmholtzName(kPhysKeys[(size_t)juce::jlimit(0, kNumKeys - 1, netKey)])
            : rysum::tr("im Gitter"));

    // ---- the bellows: how full it is, and the pressure the pipes get
    {
        const float fill = juce::jlimit(0.0f, 1.0f, e.bellowsFillNow.load()),
                    press = juce::jlimit(0.0f, 1.0f, e.bellowsNow.load());
        auto m = bellowsMeter.toFloat();
        auto bar = [&](juce::Rectangle<float> b, float v, const juce::String &name, bool red)
        {
            g.setColour(col::track);
            g.fillRect(b.withTrimmedLeft(64));
            g.setColour(red ? col::rubric.brighter(0.2f) : col::ink);
            g.fillRect(b.withTrimmedLeft(64).withWidth((b.getWidth() - 64) * v));
            g.setColour(col::dim);
            g.setFont(A.monoFont(10.0f));
            g.drawText(name, b.withWidth(62), juce::Justification::centredLeft);
        };
        bar(m.removeFromTop(11), fill, rysum::tr("Vorrat"), false);
        m.removeFromTop(6);
        bar(m.removeFromTop(11), press, rysum::tr("Winddruck"), press < 0.6f);
    }
    noteIn(bellowsNote, rysum::tr("Als Modulationsquelle \xe2\x80\x9e"
                        "Balg (Winddruck)\xe2\x80\x9c steuert der Druck alles andere."));
    noteIn(
        palletNote,
        rysum::tr("Das Ventil unter der Pfeife \xc3\xb6"
        "ffnet nicht mehr schlagartig. \xe2\x80\x9e"
        "Anschlag\xe2\x80\x9c: eine langsam gedr\xc3\xbc"
        "ckte Taste "
        "l\xc3\xa4sst den Wind z\xc3\xb6gernd ein \xe2\x80\x93 die Pfeife spricht tief, hauchig "
        "und mit Schneidenton an und findet "
        "erst dann ihren Ton; eine schnelle spricht sofort. \xe2\x80\x9e"
        "Druck\xe2\x80\x9c und \xe2\x80\x9eGleiten\xe2\x80\x9c: der Finger "
        "h\xc3\xa4lt das Ventil selbst \xe2\x80\x93 ohne Druck bleibt die Pfeife bei 100 % stumm."));
    noteIn(sympNote, rysum::tr("Stumme Pfeifen \xe2\x80\x93 auch die nicht gezogener Register \xe2\x80\x93 "
                     "klingen leise mit, wenn ein Teilton einer "
                     "klingenden Pfeife ihre Tonh\xc3\xb6he trifft, jede an ihrem eigenen Ort. "
                     "Reine Intervalle wecken mehr Pfeifen "
                     "als unreine; in einer anderen Stimmung antworten andere."));
    noteIn(wearNote,
           rysum::tr("Jede Pfeife merkt sich, wie lange sie geklungen hat. Viel gespielte Pfeifen zerfallen "
           "zuerst: "
           "sie verstimmen sich, k\xc3\xb6rnen, schwanken und setzen aus (K\xc3\xb6rnung, Bittiefe "
           "\xe2\x80\xa6 wie unter Verfall "
           "auf dem Spieltisch eingestellt). Rechts: jede Pfeife, je Register eine Zeile."));
    noteIn(impNote, rysum::tr("In den Bewegungen Flug, Fall, Schwarm und Str\xc3\xb6mung (Raum-Seite, je "
                    "Register) fliegen die Pfeifen frei durch "
                    "den Saal. Wo eine gegen Wand, Boden oder Decke st\xc3\xb6\xc3\x9ft, klopft es "
                    "dort: kurz und hell bei schnellem Aufprall, "
                    "dumpf bei langsamem oder an einer schluckenden Wand. Das Rohr klingt nach "
                    "\xe2\x80\x93 lange Pfeifen tief, kurze hoch."));
    noteIn(pureNote,
           rysum::tr("Gehaltene T\xc3\xb6ne ziehen einander zu reinen Intervallen: die Schwebungen beruhigen "
           "sich, der Akkord "
           "rastet ein. Neue T\xc3\xb6ne beginnen in der eingestellten Stimmung und gleiten dann. "
           "Auch der Wolf wird zahm \xe2\x80\x93 "
           "mit der Bewegung \xe2\x80\x9eSchwebung\xe2\x80\x9c kommt der Raum dabei zur Ruhe."));
    noteIn(netNote,
           rysum::tr("Quinten nach rechts, gro\xc3\x9f"
           "e Terzen nach oben. Mit der Anordnung \xe2\x80\x9eTonnetz\xe2\x80\x9c (Raum-Seite) "
           "geht der Klang denselben Weg durch den Raum. Tonl\xc3\xa4nge \xc3\xbc"
           "ber 1 Schritt: die T\xc3\xb6ne \xc3\xbc"
           "berlappen."));

    // ---- the listener's walk from above: the organ's usual listening place in the middle, front
    // up
    if (walkMap.getHeight() > 40)
    {
        const auto params = proc.readParams();
        const auto c = walkMap.toFloat().getCentre();
        const float R = 0.5f * (float)juce::jmin(walkMap.getWidth(), walkMap.getHeight()) - 4.0f;
        const float range =
            juce::jmax(4.0f, std::abs(params.live.listenX) + params.live.walkSize + 1.0f,
                       std::abs(params.live.listenY) + params.live.walkSize + 1.0f);
        const float sc = R / range;
        g.setColour(col::border);
        g.drawEllipse(c.x - R, c.y - R, 2 * R, 2 * R, 1.0f);
        g.setColour(col::line);
        g.drawLine(c.x - R, c.y, c.x + R, c.y, 0.6f);
        g.drawLine(c.x, c.y - R, c.x, c.y + R, 0.6f);
        // the pulled stops' pipes (every third one), where they stand
        for (int s = 0; s < kNumStops; ++s)
        {
            if (!params.stopOn[(size_t)s])
                continue;
            for (int k = 0; k < kNumKeys; k += 3)
            {
                const auto q = e.currentPosition(s, k, params);
                const float a = juce::degreesToRadians(q.az),
                            d = q.dist * std::cos(juce::degreesToRadians(q.el));
                const juce::Point<float> pt(c.x - std::sin(a) * d * sc, c.y - std::cos(a) * d * sc);
                if (pt.getDistanceFrom(c) < R)
                    drawStopDot(g, s, pt, 2.5f, false);
            }
        }
        const juce::Point<float> me(c.x + e.listenerXNow.load() * sc,
                                    c.y - e.listenerYNow.load() * sc);
        g.setColour(col::rubric.brighter(0.3f));
        g.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(me));
        g.setColour(col::white);
        g.drawEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(me), 1.0f);
        g.setFont(A.monoFont(9.0f));
        g.setColour(col::faint);
        g.drawText(juce::String(juce::roundToInt(range)) + " m",
                   juce::Rectangle<float>(c.x + R - 44, c.y + 2, 42, 11),
                   juce::Justification::centredRight);
    }

    // ---- the wear of every pipe: a row per stop (as on the console), low keys left
    if (wearMap.getHeight() > 40)
    {
        const auto order = keysByPitch();
        const float lw = 78.0f, cwid = ((float)wearMap.getWidth() - lw) / (float)kNumKeys;
        const float chgt = juce::jmin(22.0f, (float)wearMap.getHeight() / (float)kNumStops);
        for (int slot = 0; slot < kNumStops; ++slot)
        {
            const int s = kDisplayOrder[(size_t)slot];
            const float y = (float)wearMap.getY() + (float)slot * chgt;
            g.setFont(A.romanFont(11.0f));
            g.setColour(col::dim);
            g.drawText(prettyLabel(kStops[(size_t)s].label),
                       juce::Rectangle<float>((float)wearMap.getX(), y, lw - 4, chgt - 2),
                       juce::Justification::centredLeft);
            const uint64_t sounding = e.soundingMask[(size_t)s].load();
            for (int i = 0; i < kNumKeys; ++i)
            {
                const int k = order[(size_t)i];
                const float w = juce::jlimit(0.0f, 1.0f, e.wear[(size_t)s][(size_t)k].load());
                const juce::Rectangle<float> cellR((float)wearMap.getX() + lw + (float)i * cwid, y,
                                                   cwid - 1.0f, chgt - 3.0f);
                g.setColour(col::track.interpolatedWith(col::rubric.brighter(0.25f), std::sqrt(w)));
                g.fillRect(cellR);
                if ((sounding >> k) & 1)
                {
                    g.setColour(col::white.withAlpha(0.8f));
                    g.drawRect(cellR, 1.0f);
                }
            }
        }
    }
}

/** top view: listener in the middle, front up. kind 0 = upper partials, 1 = echoes */
void EffectsView::drawCompass(juce::Graphics &g, juce::Rectangle<int> area, int kind) const
{
    auto &A = Assets::get();
    const auto c = area.toFloat().getCentre();
    const float R =
        static_cast<float>(juce::jmin(area.getWidth(), area.getHeight())) * 0.5f - 12.0f;
    if (R < 10.0f)
        return;
    g.setColour(col::border);
    g.drawEllipse(c.x - R, c.y - R, 2 * R, 2 * R, 1.0f);
    g.setColour(col::line);
    g.drawLine(c.x - R, c.y, c.x + R, c.y, 0.6f);
    g.drawLine(c.x, c.y - R, c.x, c.y + R, 0.6f);
    g.setColour(col::mute);
    g.fillEllipse(c.x - 2.5f, c.y - 2.5f, 5.0f, 5.0f);
    g.setFont(A.monoFont(9.0f));
    g.setColour(col::faint);
    g.drawText(rysum::tr("vorn"), juce::Rectangle<float>(c.x - 20, c.y - R - 13, 40, 11),
               juce::Justification::centred);

    // azimuth positive = to the left (AmbiX)
    auto at = [&](float az, float el, float frac)
    {
        const float a = juce::degreesToRadians(az);
        const float rr =
            R * frac * std::cos(juce::degreesToRadians(juce::jlimit(-89.0f, 89.0f, el)));
        return juce::Point<float>(c.x - std::sin(a) * rr, c.y - std::cos(a) * rr);
    };
    if (kind == 0)
    {
        const float spread = value("fxPartSpread");
        const float turn = value("fxPartTurn");
        const auto pipe = at(0.0f, 0.0f, 0.82f);
        g.setColour(col::ink);
        g.fillEllipse(pipe.x - 5, pipe.y - 5, 10, 10);
        if (spread > 0.5f)
        {
            const float rot =
                std::abs(turn) > 0.0005f
                    ? (float)std::fmod(proc.engine.timeSeconds.load() * turn * 360.0, 360.0)
                    : 0.0f;
            for (float side : {-1.0f, 1.0f})
            {
                const auto q = at(spread * side + rot, 0.0f, 0.82f);
                g.setColour(col::rubric.brighter(0.3f));
                g.drawEllipse(q.x - 4, q.y - 4, 8, 8, 1.4f);
                g.setColour(col::rubric.withAlpha(0.5f));
                g.drawLine(pipe.x, pipe.y, q.x, q.y, 0.7f);
            }
        }
        g.setFont(A.monoFont(9.0f));
        g.setColour(col::dim);
        g.drawText(rysum::tr("\xe2\x97\x8f Grundton   \xe2\x97\x8b Obert\xc3\xb6ne"),
                   area.withTop(area.getBottom() - 11).expanded(20, 0),
                   juce::Justification::centred);
        return;
    }
    // echoes: numbered dots, size by level
    const int reps = juce::roundToInt(value("fxEchoRepeats"));
    const float fb = value("fxEchoFeedback"), lvl = value("fxEchoLevel");
    const float turn = value("fxEchoTurn"), tilt = value("fxEchoTilt");
    for (int r = 1; r <= reps; ++r)
    {
        const float gain = std::pow(fb, (float)(r - 1));
        if (gain < 0.01f)
            break;
        const float el = juce::jlimit(-80.0f, 80.0f, tilt * (float)r);
        const auto q = at(turn * (float)r, el, 0.8f);
        const float s = 5.0f + 7.0f * gain;
        g.setColour((lvl > 0.0001f ? col::rubric.brighter(0.3f) : col::mute)
                        .withAlpha(0.35f + 0.65f * gain));
        g.fillEllipse(q.x - s * 0.5f, q.y - s * 0.5f, s, s);
        g.setColour(col::ink);
        g.setFont(A.monoFont(9.0f));
        g.drawText(juce::String(r), juce::Rectangle<float>(q.x - 10, q.y - s * 0.5f - 12, 20, 11),
                   juce::Justification::centred);
    }
}

void EffectsView::paint(juce::Graphics &g)
{
    auto &A = Assets::get();
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.setOpacity(0.07f);
        drawCover(g, A.wall, getLocalBounds().toFloat(), 0.4f);
    }
    auto noteIn = [&](juce::Rectangle<int> r, const juce::String &text)
    {
        g.setFont(A.sansFont(12.5f));
        g.setColour(col::mute);
        if (r.getHeight() > 14)
            g.drawFittedText(text, r, juce::Justification::topLeft,
                             juce::jmax(1, r.getHeight() / 15));
    };
    if (tab == 2)
    {
        paintLive(g);
        return;
    }
    if (tab == 3)
    {
        paintMaster(g);
        return;
    }
    if (tab == 1)
    {
        auto tonnetzBox = juce::Rectangle<int>(ghostBox.getRight() + 28, ghostBox.getY(),
                                               expoBox.getWidth(), ghostBox.getHeight());
        drawSectionTitle(g, beatBox, rysum::tr("Schwebungsraum"), rysum::tr("die Stimmung bewegt den Raum"));
        drawSectionTitle(g, ghostBox, rysum::tr("Differenzt\xc3\xb6ne"), rysum::tr("Geister zwischen den Pfeifen"));
        drawSectionTitle(g, tonnetzBox, rysum::tr("Tonnetz"), rysum::tr("die Stimmung als Landkarte"));
        drawSectionTitle(g, dexBox, rysum::tr("Doppelbelichtung"),
                         rysum::tr("das Bild von vorhin dar\xc3\xbc"
                           "ber"));
        drawSectionTitle(g, expoBox, rysum::tr("Belichtung"), rysum::tr("Spuren auf der Kugel"));
        noteIn(beatNote, rysum::tr("Je zwei klingende Pfeifen schweben gegeneinander (die n\xc3\xa4"
                         "chsten Teilt\xc3\xb6ne, die fast zusammenfallen). "
                         "Mit der Bewegung \xe2\x80\x9eSchwebung\xe2\x80\x9c (Raum-Seite) dreht "
                         "sich jede Pfeife so schnell, wie sie schwebt: "
                         "reine Terzen stehen still, mittelt\xc3\xb6nige Quinten wandern langsam "
                         "(um 2 Hz), kleine Terzen schneller, "
                         "die Wolfsquinte G\xe2\x99\xaf\xe2\x80\x93"
                         "Es kreist heftig (um 13 Hz), die Wolfsterzen noch heftiger. "
                         "\xe2\x80\x9eGeschwindigkeit\xe2\x80\x9c = Umdrehungen je Schwebung.\n\n"
                         "Als Modulationsquelle \xe2\x80\x9eSchwebung\xe2\x80\x9c (je Pfeife) "
                         "steuert sie alles andere."));
        noteIn(ghostNote,
               rysum::tr("Aus je zwei gehaltenen Pfeifen entsteht der Differenzton (h\xc3\xb6here minus "
               "tiefere Frequenz) \xe2\x80\x93 "
               "wie die Resultante, mit der Orgelbauer einen 32\xe2\x80\xb2 andeuten. Er "
               "erscheint, solange das Intervall "
               "gehalten wird, zwischen den beiden Pfeifen oder tief unter dem H\xc3\xb6rer. "
               "Quinte: die Oktave unter dem "
               "tieferen Ton; reine Terz: zwei Oktaven unter dem Grundton des Dreiklangs."));
        noteIn(
            tonnetzBox.withTrimmedTop(38),
            rysum::tr("Auf der Raum-Seite: Anordnung \xe2\x80\x9eTonnetz\xe2\x80\x9c. Jede Taste bekommt "
            "ihren Platz im Gitter "
            "der Stimmung: Quinten nach rechts, gro\xc3\x9f"
            "e Terzen nach oben. Reine Intervalle liegen eng "
            "beieinander, unreine weit \xe2\x80\x93 in \xc2\xbc-Komma bilden Dreikl\xc3\xa4nge "
            "enge Gruppen, "
            "G\xe2\x99\xaf und Es liegen an entgegengesetzten R\xc3\xa4ndern (dort sitzt der "
            "Wolf). "
            "Eine andere Stimmung zeichnet die Karte neu (pythagoreisch: eine Kette von Quinten, "
            "gleichstufig: weit auseinander liegende Terzen)."));
        noteIn(dexNote, rysum::tr("Das Klangfeld von vor ein paar Sekunden wird \xc3\xbc"
                        "ber das jetzige gelegt \xe2\x80\x93 gedreht, gespiegelt, "
                        "verblasst, wie eine zweite Belichtung auf demselben Film. "
                        "\xe2\x80\x9eMehrfach\xe2\x80\x9c belichtet das "
                        "alte Bild noch einmal: weitere, immer blassere Bilder weiter zur\xc3\xbc"
                        "ck.\n\n"
                        "Wirkt auf das ganze Feld nach Hall und Saal (bis 3. Ordnung; in Stereo "
                        "wird gespiegelt durch Tauschen der Seiten)."));
        noteIn(expoNote, rysum::tr("Jede Richtung der Kugel h\xc3\xa4lt fest, was durch sie kommt, und gibt "
                         "es langsam wieder ab: "
                         "bewegte Pfeifen ziehen Lichtspuren, schnelle malen B\xc3\xb6gen."));
        return;
    }
    const int n = proc.getNumCustomSounds();
    const juce::String where = proc.hasOwnCustomFolder()
                                   ? proc.getCustomFolder().getFileName() + U(" \xc2\xb7 ")
                                   : juce::String();
    drawSectionTitle(g, custBox, rysum::tr("Eigene Ger\xc3\xa4usche"),
                     where + (n > 0 ? juce::String(n) + (n == 1 ? rysum::tr(" Datei") : rysum::tr(" Dateien"))
                                    : rysum::tr("Ordner leer \xe2\x86\x92 Klicks")));
    drawSectionTitle(g, pathBox, rysum::tr("Trakturweg"), rysum::tr("Spieltisch \xe2\x86\x92 Pfeife"));
    drawSectionTitle(g, crackBox, rysum::tr("Knistern"), rysum::tr("an jeder Pfeife"));
    drawSectionTitle(g, leakBox, rysum::tr("Windlecks"), rysum::tr("zischende Stellen"));
    drawSectionTitle(g, partBox, rysum::tr("Obert\xc3\xb6ne verteilen"), rysum::tr("Grundton bleibt"));
    drawSectionTitle(g, echoBox, rysum::tr("Wandernde Echos"), rysum::tr("jede Wiederholung woanders"));

    g.setFont(A.sansFont(12.5f));
    g.setColour(col::mute);
    auto note = [&](juce::Rectangle<int> r, const juce::String &text)
    {
        if (r.getHeight() > 14)
            g.drawFittedText(text, r, juce::Justification::topLeft,
                             juce::jmax(1, r.getHeight() / 15));
    };
    note(pathNote, rysum::tr("Kleine Klicks laufen vom Spieltisch zu den Pfeifen der gezogenen Register, wie "
                   "die Abstrakten "
                   "der Traktur. Mit \xe2\x80\x9ePfeifen warten\xe2\x80\x9c spricht die Pfeife "
                   "erst, wenn der Weg ankommt."));
    note(crackNote, rysum::tr("Kurze Knackser an jeder klingenden Pfeife: sie folgen jeder Bewegung, das Ohr "
                    "findet die Pfeifen leichter. "
                    "Hohe Dichte und kurze L\xc3\xa4nge klingen wie Glut, wenige lange Knackser "
                    "wie altes Holz.\n\n"
                    "Nach dem Loslassen verklingt das Knistern in einer halben Sekunde."));
    note(leakNote,
         rysum::tr("Undichte Stellen im Windsystem zischen ungleichm\xc3\xa4\xc3\x9fig von festen Orten. "
         "Wie laut das Gebl\xc3\xa4se selbst rauscht, stellt \xe2\x80\x9eWind\xe2\x80\x9c auf dem "
         "Spieltisch ein."));
    note(partNote, rysum::tr("Oberhalb der Trennung wandert der Klang jeder Pfeife zur Seite "
                   "(zuf\xc3\xa4llig links oder rechts); "
                   "\xe2\x80\x9eKreisen\xe2\x80\x9c dreht diesen Teil um den H\xc3\xb6rer."));
    note(custNote, "");

    drawCompass(g, partCompass, 0);
    drawCompass(g, echoCompass, 1);
}

/** the fourth tab: the EQ's curve, the compressor's line and how much it turns down now */
void EffectsView::paintMaster(juce::Graphics &g)
{
    auto &A = Assets::get();
    MasterSettings s;
    s.eqOn = value("mstEqOn") > 0.5f;
    s.lowHz = value("mstLowHz");
    s.lowDb = value("mstLowDb");
    s.midHz = value("mstMidHz");
    s.midDb = value("mstMidDb");
    s.midQ = value("mstMidQ");
    s.highHz = value("mstHighHz");
    s.highDb = value("mstHighDb");
    s.compOn = value("mstCompOn") > 0.5f;
    s.threshDb = value("mstThresh");
    s.ratio = value("mstRatio");
    s.makeupDb = value("mstMakeup");
    const double sr = proc.getSampleRate() > 0.0 ? proc.getSampleRate() : 48000.0;
    drawSectionTitle(g, eqBox, "EQ", s.eqOn ? rysum::tr("drei B\xc3\xa4nder auf der Summe") : rysum::tr("aus"));
    drawSectionTitle(g, compBox, rysum::tr("Kompressor"),
                     s.compOn ? rysum::tr("zieht ") + juce::String(proc.masterBus.reductionDb.load(), 1) + rysum::tr(" dB ab")
                              : rysum::tr("aus"));
    // ---- the EQ: 20 Hz .. 20 kHz on a log axis, +-15 dB
    {
        const auto r = eqCurve.toFloat();
        g.setColour(col::panel.withAlpha(0.6f));
        g.fillRect(r);
        auto xOf = [&](double hz) { return r.getX() + (float)(std::log(hz / 20.0) / std::log(1000.0)) * r.getWidth(); };
        auto yOf = [&](float db) { return r.getCentreY() - db / 15.0f * r.getHeight() * 0.5f; };
        g.setFont(A.monoFont(9.0f));
        for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0})
        {
            g.setColour(col::line);
            g.fillRect(xOf(hz), r.getY(), 1.0f, r.getHeight());
            g.setColour(col::faint);
            g.drawText(hz >= 1000.0 ? juce::String((int)(hz / 1000.0)) + "k" : juce::String((int)hz),
                       juce::Rectangle<float>(xOf(hz) + 2.0f, r.getBottom() - 12.0f, 30.0f, 11.0f),
                       juce::Justification::centredLeft);
        }
        for (float db : {-12.0f, -6.0f, 0.0f, 6.0f, 12.0f})
        {
            g.setColour(db == 0.0f ? col::dim.withAlpha(0.6f) : col::line);
            g.fillRect(r.getX(), yOf(db), r.getWidth(), 1.0f);
            g.setColour(col::faint);
            g.drawText((db > 0 ? "+" : "") + juce::String((int)db), juce::Rectangle<float>(r.getX() + 2.0f, yOf(db) - 11.0f, 30.0f, 10.0f),
                       juce::Justification::centredLeft);
        }
        juce::Path p;
        for (int i = 0; i <= 200; ++i)
        {
            const double hz = 20.0 * std::pow(1000.0, i / 200.0);
            const float db = juce::jlimit(-15.0f, 15.0f, MasterBus::responseDb(s, sr, (float)hz));
            if (i == 0)
                p.startNewSubPath(xOf(hz), yOf(db));
            else
                p.lineTo(xOf(hz), yOf(db));
        }
        g.setColour(s.eqOn ? col::rubric.brighter(0.3f) : col::faint);
        g.strokePath(p, juce::PathStrokeType(1.8f));
        if (s.eqOn)
            for (auto [hz, db] : {std::pair<float, float>{s.lowHz, s.lowDb}, {s.midHz, s.midDb}, {s.highHz, s.highDb}})
                drawLozenge(g, {xOf(hz), yOf(juce::jlimit(-15.0f, 15.0f, db))}, 4.0f, 5.0f);
    }
    // ---- the compressor: what goes in (dB) against what comes out, and the turning down now
    {
        auto area = compMeter.toFloat();
        const float side = juce::jmin(area.getHeight(), area.getWidth() - 40.0f);
        // the square of the curve, the meter right beside it
        auto r = area.withWidth(side).withHeight(side);
        const auto bar = juce::Rectangle<float>(r.getRight() + 12.0f, r.getY(), 22.0f, side);
        g.setColour(col::panel.withAlpha(0.6f));
        g.fillRect(r);
        auto xOf = [&](float db) { return r.getX() + (db + 48.0f) / 48.0f * r.getWidth(); };
        auto yOf = [&](float db) { return r.getBottom() - (db + 48.0f) / 48.0f * r.getHeight(); };
        g.setFont(A.monoFont(9.0f));
        for (float db : {-36.0f, -24.0f, -12.0f})
        {
            g.setColour(col::line);
            g.fillRect(xOf(db), r.getY(), 1.0f, r.getHeight());
            g.fillRect(r.getX(), yOf(db), r.getWidth(), 1.0f);
            g.setColour(col::faint);
            g.drawText(juce::String((int)db), juce::Rectangle<float>(xOf(db) + 2.0f, r.getBottom() - 12.0f, 30.0f, 11.0f),
                       juce::Justification::centredLeft);
        }
        g.setColour(col::line);
        g.drawLine(r.getX(), r.getBottom(), r.getRight(), r.getY(), 1.0f);
        juce::Path p;
        const float W = 6.0f, slope = 1.0f / juce::jmax(1.0f, s.ratio) - 1.0f;
        for (int i = 0; i <= 96; ++i)
        {
            const float in = -48.0f + 0.5f * (float)i, over = in - s.threshDb;
            float gr = 0.0f;
            if (2.0f * over > W)
                gr = slope * over;
            else if (2.0f * over > -W)
                gr = slope * (over + W * 0.5f) * (over + W * 0.5f) / (2.0f * W);
            const float out = juce::jlimit(-48.0f, 0.0f, in + gr + s.makeupDb);
            if (i == 0)
                p.startNewSubPath(xOf(in), yOf(out));
            else
                p.lineTo(xOf(in), yOf(out));
        }
        g.setColour(s.compOn ? col::rubric.brighter(0.3f) : col::faint);
        g.strokePath(p, juce::PathStrokeType(1.8f));
        g.setColour(col::dim.withAlpha(0.5f));
        g.fillRect(xOf(s.threshDb), r.getY(), 1.0f, r.getHeight());
        // the meter: how much it turns down now (0 .. 24 dB, from the top)
        g.setColour(col::panel.withAlpha(0.6f));
        g.fillRect(bar);
        const float red = juce::jlimit(0.0f, 24.0f, proc.masterBus.reductionDb.load());
        g.setColour(col::rubric.brighter(0.2f));
        g.fillRect(bar.withHeight(bar.getHeight() * red / 24.0f));
        g.setColour(col::faint);
        for (float db : {6.0f, 12.0f, 18.0f})
            g.fillRect(bar.getX(), bar.getY() + bar.getHeight() * db / 24.0f, bar.getWidth(), 1.0f);
    }
    g.setFont(A.sansFont(12.5f));
    g.setColour(col::mute);
    if (eqNote.getHeight() > 14)
        g.drawFittedText(rysum::tr("Wirkt auf alles, was hinausgeht \xe2\x80\x93 Stereo, Kopfh\xc3\xb6rer, Ambisonics oder "
                           "Lautsprecher \xe2\x80\x93 als letzter Schritt. Jeder Regler kann eine Spur im Verlauf haben."),
                         eqNote, juce::Justification::topLeft, juce::jmax(1, eqNote.getHeight() / 15));
    if (compNote.getHeight() > 14)
        g.drawFittedText(rysum::tr("Der lauteste Kanal entscheidet, alle werden gleich leiser: das Raumbild bleibt stehen. "
                           "Schwelle, Verh\xc3\xa4ltnis (mit weichem Knie), Ansprache, Ausklang, Aufholen."),
                         compNote, juce::Justification::topLeft, juce::jmax(1, compNote.getHeight() / 15));
}
} // namespace rysum::ui
