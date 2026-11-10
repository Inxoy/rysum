#include "PluginEditor.h"

using namespace rysum;
using namespace rysum::ui;

static constexpr int kW = 1280, kH = 720, kHeader = 56;     // 16:9 - fills a full-HD screen at 1.5x
static constexpr int kOutX = kW - 206, kOutW = 100; // output format combo in the header
static constexpr int kFullX = kW - 56;             // full-screen button (28 x 28)

RysumContent::RysumContent (RysumProcessor& p) : proc (p), console (p), space (p), klang (p), mod (p), fx (p), saal (p)
{
    addAndMakeVisible (console);
    addChildComponent (organ);
    addChildComponent (speakers);
    addChildComponent (walk);
    klang.onOpenSpeakers = [this] { showView (7); };
    saal.onOpenWalk = [this] { releaseComputerKeys(); showView (8); };
    walk.onBack = [this] { showView (2); };
    outInfoButton.setTooltip (juce::String::fromUTF8 ("Lautsprecher einrichten (AllRAD)"));
    outInfoButton.setAlpha (0.0f);
    outInfoButton.onClick = [this] { showView (current == 7 ? viewBeforeOrgan : 7); };
    addAndMakeVisible (outInfoButton);
    logoButton.setTooltip (juce::String::fromUTF8 ("\xc3\x9c" "ber die Orgel zu Rysum (nochmal klicken: zur\xc3\xbc" "ck)"));
    logoButton.onClick = [this] { showView (current == 6 ? viewBeforeOrgan : 6); };
    addAndMakeVisible (logoButton);
    diceButton.setTooltip (juce::String::fromUTF8 ("W\xc3\xbcrfeln: zuf\xc3\xa4llige Einstellungen \xe2\x80\x93 sanft oder wild, nur in den gew\xc3\xa4hlten Bereichen. "
                                                   "Ein Schritt zur\xc3\xbc" "ck ist m\xc3\xb6glich."));
    diceButton.onClick = [this] { showDiceMenu(); };
    addAndMakeVisible (diceButton);
    addChildComponent (space);
    addChildComponent (klang);
    addChildComponent (mod);
    addChildComponent (fx);
    addChildComponent (saal);
    const char* names[] { "Spieltisch", "Raum", "Saal", "Klang", "Effekte", "Modulation" };
    for (int i = 0; i < 6; ++i)
    {
        tabs[(size_t) i].setButtonText (juce::String (names[i]).toUpperCase());
        tabs[(size_t) i].onClick = [this, i] { showView (i); };
        addAndMakeVisible (tabs[(size_t) i]);
    }
    showView (0);

    patchName.setTooltip ("Patches: Klang, Register, Raum, Modulation und Hall (ohne Ausgabeformat)");
    patchName.onClick = [this] { showPatchMenu(); };
    patchPrev.onClick = [this] { stepPatch (-1); };
    patchNext.onClick = [this] { stepPatch (1); };
    patchSave.onClick = [this] { askSavePatch(); };
    patchPrev.setTooltip ("Vorheriger Patch");
    patchNext.setTooltip (u8 ("N\xc3\xa4" "chster Patch"));
    for (auto* b : { &patchPrev, &patchName, &patchNext, &patchSave }) addAndMakeVisible (b);

    // playing helpers: computer keyboard, "Halten"
    keyNoteDown.fill (-1);
    setWantsKeyboardFocus (true);
    keysButton.setTooltip (juce::String::fromUTF8 ("Mit der Computertastatur spielen: A W S E D F T G Z H U J K O L P = C bis D\xe2\x80\xb2\xe2\x80\xb2, "
                                                   "Y / X = Oktave tiefer / h\xc3\xb6her, C / V = leiser / lauter anschlagen. "
                                                   "Im Host vorher ins Plugin-Fenster klicken."));
    keysButton.onClick = [this]
    {
        proc.computerKeys = ! proc.computerKeys;
        if (proc.computerKeys) grabKeyboardFocus(); else releaseComputerKeys();
        updatePlayButtons();
    };
    latchButton.setTooltip (juce::String::fromUTF8 ("Halten: eine Taste zweimal schnell anschlagen \xe2\x80\x93 der Ton bleibt liegen, als ob du sie h\xc3\xa4ltst "
                                                    "(H\xc3\xa4nde frei f\xc3\xbcr Register und Modulation). Nochmal anschlagen = loslassen. "
                                                    "Ausschalten l\xc3\xa4sst alle gehaltenen T\xc3\xb6ne los."));
    latchButton.onClick = [this] { proc.latchOn = ! proc.latchOn.load(); updatePlayButtons(); };
    addAndMakeVisible (keysButton);
    addAndMakeVisible (latchButton);
    updatePlayButtons();

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("output")))
        outputChoice.addItemList (choice->choices, 1);
    outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "output", outputChoice);
    outputChoice.setTooltip (juce::String::fromUTF8 ("Ausgabeformat: Stereo, AmbiX (ACN/SN3D), Binaural f\xc3\xbcr Kopfh\xc3\xb6rer (Einstellungen: Seite Klang) "
                                                     "oder Lautsprecher (AllRAD, Klick auf die Kanalanzeige daneben)"));
    outputChoice.onChange = [this]
    {
        // switching to loudspeakers: show the layout once
        if (outputChoice.getSelectedId() - 1 == RysumProcessor::kSpeakerOutput && current != 7 && ! speakerPageShown) { speakerPageShown = true; showView (7); }
    };
    addAndMakeVisible (outputChoice);
    fullButton.onClick = [this] { if (onToggleFullscreen) onToggleFullscreen(); };
    setFullscreenShown (false);
    addAndMakeVisible (fullButton);

    // grain over the whole interface, rendered once
    grainLayer = juce::Image (juce::Image::ARGB, kW, kH, true);
    {
        juce::Graphics g (grainLayer);
        drawGrain (g, { 0, 0, kW, kH }, 1.0f);
    }
    // darkened edges, like an old print
    vignette = juce::Image (juce::Image::ARGB, kW, kH, true);
    {
        juce::Graphics g (vignette);
        juce::ColourGradient grad (juce::Colours::transparentBlack, kW * 0.5f, kH * 0.48f,
                                   juce::Colours::black.withAlpha (0.22f), 0.0f, 0.0f, true);
        grad.addColour (0.7, juce::Colours::transparentBlack);
        g.setGradientFill (grad);
        g.fillRect (0, 0, kW, kH);
    }
    setSize (kW, kH);
}

void RysumContent::showView (int v)
{
    if ((v == 6 || v == 7) && current != 6 && current != 7 && current != 8) viewBeforeOrgan = current;
    if (v == 8 && current != 8) viewBeforeOrgan = 2;
    current = v;
    organ.setVisible (v == 6);
    speakers.setVisible (v == 7);
    walk.setVisible (v == 8);
    console.setVisible (v == 0);
    space.setVisible (v == 1);
    saal.setVisible (v == 2);
    klang.setVisible (v == 3);
    fx.setVisible (v == 4);
    mod.setVisible (v == 5);
    for (int i = 0; i < 6; ++i) tabs[(size_t) i].setToggleState (i == v, juce::dontSendNotification);
    if (v == 8) { if (walk.isShowing()) walk.grabKeyboardFocus(); }
    else if (proc.computerKeys) grabKeyboardFocus();
}

void RysumContent::resized()
{
    auto body = getLocalBounds().withTrimmedTop (kHeader);
    console.setBounds (body);
    space.setBounds (body);
    klang.setBounds (body);
    mod.setBounds (body);
    fx.setBounds (body);
    saal.setBounds (body);
    organ.setBounds (body);
    speakers.setBounds (body);
    walk.setBounds (body);
    outInfoButton.setBounds (kOutX + kOutW + 2, 8, kFullX - 5 - (kOutX + kOutW + 2), kHeader - 16);
    logoButton.setBounds (24, 6, 116, 44);
    patchPrev.setBounds (152, 14, 22, 28);
    patchName.setBounds (177, 14, 112, 28);
    patchNext.setBounds (292, 14, 22, 28);
    patchSave.setBounds (318, 14, 72, 28);
    diceButton.setBounds (394, 14, 60, 28);
    keysButton.setBounds (462, 14, 74, 28);
    latchButton.setBounds (540, 14, 60, 28);
    const int tw[] { 94, 58, 54, 58, 76, 102 };
    int x = 608;
    for (int i = 0; i < 6; ++i) { tabs[(size_t) i].setBounds (x, 13, tw[i], 30); x += tw[i] + 3; }
    outputChoice.setBounds (kOutX, 14, kOutW, 28);
    fullButton.setBounds (kFullX, 14, 28, 28);
}

void RysumContent::updatePlayButtons()
{
    keysButton.setToggleState (proc.computerKeys, juce::dontSendNotification);
    keysButton.setButtonText (proc.computerKeys ? "Tastatur C" + juce::String (proc.keysOctave) : juce::String ("Tastatur"));
    const int n = proc.latchedCount.load();
    latchButton.setToggleState (proc.latchOn.load(), juce::dontSendNotification);
    latchButton.setButtonText (proc.latchOn.load() && n > 0 ? "Halten " + juce::String (n) : juce::String ("Halten"));
}

// computer keyboard: two rows like a piano (German layout: Z is the black key between G and H)
static const char kPianoKeys[] = "awsedftgzhujkolp";

void RysumContent::releaseComputerKeys()
{
    for (auto& n : keyNoteDown)
        if (n >= 0) { proc.keyboardState.noteOff (1, n, 0.0f); n = -1; }
}

bool RysumContent::keyPressed (const juce::KeyPress& k)
{
    // F11: full screen on / off; Esc leaves it
    if (k.getKeyCode() == juce::KeyPress::F11Key || (k == juce::KeyPress::escapeKey && fullButton.active))
    {
        if (onToggleFullscreen) onToggleFullscreen();
        return true;
    }
    if (! proc.computerKeys || k.getModifiers().isCommandDown() || k.getModifiers().isCtrlDown() || k.getModifiers().isAltDown()) return false;
    if (dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr) return false;   // typing a value
    const auto c = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());
    if (c == 'y' || c == 'x')
    {
        releaseComputerKeys();
        proc.keysOctave = juce::jlimit (2, 5, proc.keysOctave + (c == 'x' ? 1 : -1));
        updatePlayButtons();
        return true;
    }
    if (c == 'c' || c == 'v') { keysVelocity = juce::jlimit (10, 127, keysVelocity + (c == 'v' ? 20 : -20)); return true; }
    for (const char* p = kPianoKeys; *p != 0; ++p) if (c == (juce::juce_wchar) *p) return true;    // played in keyStateChanged
    return false;
}

bool RysumContent::keyStateChanged (bool)
{
    if (! proc.computerKeys) return false;
    if (dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr) return false;
    bool used = false;
    for (int i = 0; kPianoKeys[i] != 0; ++i)
    {
        const bool down = juce::KeyPress::isKeyCurrentlyDown (kPianoKeys[i]);
        auto& note = keyNoteDown[(size_t) i];
        if (down && note < 0)
        {
            note = 12 * (proc.keysOctave + 1) + i;
            proc.keyboardState.noteOn (1, note, (float) keysVelocity / 127.0f);
            used = true;
        }
        else if (! down && note >= 0)
        {
            proc.keyboardState.noteOff (1, note, 0.0f);
            note = -1;
            used = true;
        }
    }
    return used;
}

void RysumContent::tick()
{
    updatePlayButtons();
    // the window lost the keyboard: let the computer keys go (they would hang otherwise)
    if (proc.computerKeys && ! hasKeyboardFocus (true))
    {
        auto* peer = getPeer();
        if (peer != nullptr && peer->isFocused() && juce::Component::getCurrentlyFocusedComponent() == nullptr) grabKeyboardFocus();
        else releaseComputerKeys();
    }
    if (console.isVisible()) console.refresh();
    if (space.isVisible()) space.refresh();
    if (klang.isVisible()) klang.refresh();
    if (mod.isVisible()) mod.refresh();
    if (fx.isVisible()) fx.refresh();
    if (saal.isVisible()) saal.refresh();
    if (speakers.isVisible()) speakers.refresh();
    if (proc.patchVersion != shownPatchVersion)
    {
        shownPatchVersion = proc.patchVersion;
        const auto n = proc.getPatchName();
        patchName.setButtonText (n.isEmpty() ? u8 ("\xe2\x80\x93 kein Patch \xe2\x80\x93") : n);
    }
    repaint (0, 0, kW, kHeader);
}

void RysumContent::showPatchMenu()
{
    const auto files = RysumProcessor::getPatchFiles();
    juce::PopupMenu m;
    m.addSectionHeader ("Patches");
    for (int i = 0; i < files.size(); ++i)
        m.addItem (1 + i, files[i].getFileNameWithoutExtension(), true, files[i].getFileNameWithoutExtension() == proc.getPatchName());
    if (files.isEmpty()) m.addItem (-1, "(noch keine)", false);
    m.addSeparator();
    m.addItem (10000, "Speichern unter ...");
    m.addItem (10001, u8 ("Patch-Ordner \xc3\xb6" "ffnen"));
    juce::Component::SafePointer<RysumContent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&patchName), [safe, files] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r == 10000) { safe->askSavePatch(); return; }
        if (r == 10001)
        {
            auto folder = RysumProcessor::getPatchFolder();
            folder.createDirectory();
            folder.startAsProcess();
            return;
        }
        if (r >= 1 && r <= files.size()) safe->proc.loadPatch (files[r - 1]);
    });
}

void RysumContent::stepPatch (int delta)
{
    const auto files = RysumProcessor::getPatchFiles();
    if (files.isEmpty()) return;
    int cur = -1;
    for (int i = 0; i < files.size(); ++i) if (files[i].getFileNameWithoutExtension() == proc.getPatchName()) cur = i;
    const int n = files.size();
    const int next = cur < 0 ? (delta > 0 ? 0 : n - 1) : ((cur + delta) % n + n) % n;
    proc.loadPatch (files[next]);
}

void RysumContent::askSavePatch()
{
    auto* w = new juce::AlertWindow ("Patch speichern",
                                     u8 ("Name des Patches. Gespeichert wird alles au\xc3\x9f" "er dem Ausgabeformat und dem Samples-Ordner."),
                                     juce::MessageBoxIconType::NoIcon, this);
    w->setLookAndFeel (&getLookAndFeel());
    w->addTextEditor ("name", proc.getPatchName().isEmpty() ? juce::String ("Mein Patch") : proc.getPatchName());
    w->addButton ("Speichern", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Abbrechen", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<RysumContent> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int result)
    {
        if (safe == nullptr || result != 1) return;
        const auto name = RysumProcessor::cleanPatchName (w->getTextEditorContents ("name"));
        if (name.isEmpty()) return;
        auto doSave = [safe, name]
        {
            if (safe == nullptr) return;
            if (! safe->proc.savePatch (name))
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Patch speichern",
                                                        "Konnte nicht gespeichert werden:\n" + RysumProcessor::getPatchFolder().getFullPathName());
        };
        const bool exists = RysumProcessor::getPatchFolder().getChildFile (name + ".rysum").existsAsFile();
        if (exists && name != safe->proc.getPatchName())
        {
            auto opts = juce::MessageBoxOptions().withTitle ("Patch speichern")
                            .withMessage ("\"" + name + u8 ("\" gibt es schon. Ersetzen?"))
                            .withButton ("Ersetzen").withButton ("Abbrechen").withAssociatedComponent (safe.getComponent());
            juce::AlertWindow::showAsync (opts, [doSave] (int r) { if (r == 1) doSave(); });
        }
        else doSave();
    }), true);
}

void RysumContent::paint (juce::Graphics& g)
{
    auto& A = Assets::get();
    g.fillAll (col::bg);
    auto head = juce::Rectangle<int> (0, 0, kW, kHeader).reduced (28, 0);
    g.setColour (col::border);
    g.fillRect (0, kHeader - 4, kW, 1);
    g.setColour (col::line);
    g.fillRect (0, kHeader - 1, kW, 1);

    {
        // "Rysum" in blackletter, the R rubricated
        const auto f = A.frakturFont (34.0f);
        const int rw = juce::roundToInt (juce::GlyphArrangement::getStringWidth (f, "R"));
        g.setFont (f);
        g.setColour (col::rubric.brighter (0.2f));
        g.drawText ("R", head.withWidth (rw + 4).translated (0, 2), juce::Justification::centredLeft);
        g.setColour (col::ink);
        g.drawText ("ysum", head.withTrimmedLeft (rw).withWidth (110).translated (0, 2), juce::Justification::centredLeft);
        if (current == 6 || logoButton.isOver())
        {
            g.setColour (current == 6 ? col::rubric : col::border);
            g.fillRect (head.getX(), kHeader - 12, 96, 2);
        }
    }
    g.setFont (A.monoFont (11.0f));

    const int out = (int) proc.apvts.getRawParameterValue ("output")->load();
    const bool tr = out == RysumProcessor::kTransauralOutput;
    const bool bin = out == RysumProcessor::kBinauralOutput || tr, spk = out == RysumProcessor::kSpeakerOutput;
    auto dec = proc.getSpeakerDecoder();
    const int spkCh = dec != nullptr ? dec->numOutChannels : 0;
    const int needed = out == 0 || bin ? 2 : spk ? spkCh : (out + 1) * (out + 1);
    const int have = proc.getTotalNumOutputChannels();
    juce::String o = bin ? (proc.binauralReady() ? juce::String (tr ? "2 LS" : "2 ch HRTF") : juce::String::fromUTF8 ("HRTF \xe2\x80\xa6"))
                   : spk ? (dec != nullptr && dec->error.isEmpty() ? juce::String (spkCh) + " ch LS" : juce::String ("LS: Fehler")) : juce::String (needed) + " ch";
    if (have < needed) o = u8 ("nur ") + juce::String (have) + " ch!";
    g.setColour (have < needed ? col::white : col::faint);
    g.setFont (A.monoFont (10.0f));
    const juce::Rectangle<int> info (kOutX + kOutW + 2, 0, kFullX - 5 - (kOutX + kOutW + 2), kHeader);
    g.drawText (o, info.withTrimmedBottom (kHeader / 2).withTrimmedTop (9), juce::Justification::centredRight);
    g.setColour (col::faint);
    g.drawText (juce::String (proc.engine.activeVoices.load()) + " St.", info.withTrimmedTop (kHeader / 2).withTrimmedBottom (9), juce::Justification::centredRight);
}

void RysumContent::showDiceMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader (juce::String::fromUTF8 ("W\xc3\xbcrfeln"));
    m.addItem (1, juce::String::fromUTF8 ("Sanft \xe2\x80\x93 kleine Schritte vom jetzigen Klang"));
    m.addItem (2, juce::String::fromUTF8 ("Wild \xe2\x80\x93 alles neu (aber spielbar)"));
    m.addItem (3, juce::String::fromUTF8 ("Zur\xc3\xbc" "ck zum Klang davor"), proc.canUndoDice());
    m.addSeparator();
    m.addSectionHeader (juce::String::fromUTF8 ("Was gew\xc3\xbcrfelt wird"));
    const auto names = RysumProcessor::diceGroupNames();
    for (int g = 0; g < RysumProcessor::kNumDiceGroups; ++g) m.addItem (10 + g, names[g], true, proc.isDiceGroupOn (g));
    m.addSeparator();
    m.addItem (4, juce::String::fromUTF8 ("Nie: Lautst\xc3\xa4rke, Ausgabe, Stimmung, MIDI, Freeze, Anordnung der Pfeifen"), false);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&diceButton),
                     [safe = juce::Component::SafePointer<RysumContent> (this)] (int r)
                     {
                         if (safe == nullptr || r == 0) return;
                         auto& pr = safe->proc;
                         if (r == 1 || r == 2) pr.dice (r == 2);
                         else if (r == 3) pr.undoDice();
                         else if (r >= 10)
                         {
                             pr.setDiceGroupOn (r - 10, ! pr.isDiceGroupOn (r - 10));
                             safe->showDiceMenu();             // keep choosing
                         }
                         safe->repaint();
                     });
}

void RysumContent::showModMenu (juce::Component& target, const juce::String& paramId)
{
    const int idx = proc.getParamIndex (paramId);
    auto* prm = proc.getParamAt (idx);
    if (prm == nullptr) return;
    juce::PopupMenu m;
    m.addSectionHeader (prm->getName (64));
    const auto names = modSourceNames();
    if (! proc.isModTarget (idx))
        m.addItem (-1, u8 ("Schalter/Auswahl \xe2\x80\x93 nicht modulierbar"), false);
    else
    {
        bool free = false;
        for (int r = 0; r < kNumRoutes; ++r) free = free || (proc.getRouteSrc (r) < 0 && proc.getRouteTgt (r) < 0);
        juce::PopupMenu with;
        for (int s = 0; s < kNumSources; ++s) with.addItem (1 + s, names[s]);
        m.addSubMenu (free ? juce::String ("Modulieren mit") : u8 ("Modulieren mit (alle ") + juce::String (kNumRoutes) + u8 (" Pl\xc3\xa4tze belegt)"), with, free);
        for (int r = 0; r < kNumRoutes; ++r)
            if (proc.getRouteTgt (r) == kGlobalTargetBase + idx && proc.getRouteSrc (r) >= 0)
            {
                auto* a = proc.apvts.getParameter (proc.getModAmountId (r));
                m.addItem (100 + r, "Entfernen: " + names[proc.getRouteSrc (r)] + " (" + (a != nullptr ? a->getCurrentValueAsText() : juce::String()) + ")");
            }
    }
    // MIDI learn
    m.addSeparator();
    m.addItem (400, u8 ("MIDI lernen (Regler am Keyboard bewegen)"));
    for (int slot : proc.midiMapsFor (idx))
    {
        const auto mm = proc.getMidiMap (slot);
        m.addItem (500 + slot, u8 ("MIDI-Zuordnung entfernen: ") + (mm.cc >= 0 ? "CC " + juce::String (mm.cc) : u8 ("noch nicht gelernt")));
    }
    m.addSeparator();
    m.addItem (300, "Modulationsseite " + juce::String (u8 ("\xc3\xb6")) + "ffnen");
    juce::Component::SafePointer<RysumContent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target), [safe, idx] (int res)
    {
        if (safe == nullptr || res <= 0) return;
        auto& self = *safe;
        if (res >= 1 && res <= kNumSources) self.proc.addRoute (res - 1, kGlobalTargetBase + idx, 0.25f);
        else if (res >= 100 && res < 100 + kNumRoutes) self.proc.clearRoute (res - 100);
        else if (res == 300) { self.mod.showMidiTab (false); self.showView (5); }
        else if (res == 400)
        {
            const int slot = self.proc.learnMidiFor (idx);
            if (slot < 0)
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "MIDI lernen",
                                                        u8 ("Alle ") + juce::String (RysumProcessor::kNumMidiMaps) + u8 (" MIDI-Zuordnungen sind belegt."));
            else self.mod.showMidiTab (true, slot);        // the page switches only when the user opens it; the row waits there
        }
        else if (res >= 500 && res < 500 + RysumProcessor::kNumMidiMaps) self.proc.clearMidiMap (res - 500);
    });
}

void RysumContent::paintOverChildren (juce::Graphics& g)
{
    g.drawImageAt (vignette, 0, 0);
    g.setOpacity (0.22f);
    g.drawImageAt (grainLayer, 0, 0);
}

// ---------------------------------------------------------------------------
// window size: the last size the user chose (Documents/Rysum/Fenster.xml), else as large as fits the screen
static juce::File windowSizeFile()
{
    return RysumProcessor::getPatchFolder().getParentDirectory().getChildFile ("Fenster.xml");
}

static float loadWindowScale()
{
    if (auto xml = juce::parseXML (windowSizeFile()))
    {
        const double s = xml->getDoubleAttribute ("scale", 0.0);
        if (s >= 0.5 && s <= 2.0) return (float) s;
    }
    // first start: about 9/10 of the main screen's usable area (on a full-HD screen ~1.35x, text ~1.35x larger)
    float s = 1.0f;
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        s = juce::jmin (0.92f * (float) d->userArea.getWidth() / (float) kW, 0.88f * (float) d->userArea.getHeight() / (float) kH);
    return juce::jlimit (1.0f, 1.6f, std::floor (s * 20.0f) / 20.0f);
}

static void saveWindowScale (float s)
{
    juce::XmlElement xml ("RysumFenster");
    xml.setAttribute ("scale", (double) s);
    windowSizeFile().getParentDirectory().createDirectory();
    xml.writeTo (windowSizeFile());
}

RysumEditor::RysumEditor (RysumProcessor& p) : AudioProcessorEditor (p), content (p)
{
    setLookAndFeel (&lnf.get());
    content.setLookAndFeel (&lnf.get());
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf.get());     // right-click menus and dialogs have no parent: they use the default
    addAndMakeVisible (content);
    setResizable (true, true);
    setResizeLimits (kW / 2, kH / 2, kW * 2, kH * 2);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio ((double) kW / (double) kH);
    const float s0 = loadWindowScale();
    setSize (juce::roundToInt (kW * s0), juce::roundToInt (kH * s0));
    sizeRestored = true;
    // deferred: the click / key that asked for it is still being handled by a component that moves
    // (the safe pointer is made here, outside the nested lambda: MSVC reads 'this' there differently)
    content.onToggleFullscreen = [safe = juce::Component::SafePointer<RysumEditor> (this)]
    {
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->toggleFullscreen(); });
    };
    startTimer (0, 33);
}

RysumEditor::~RysumEditor()
{
    if (fullscreen != nullptr)
    {
        if (juce::Desktop::getInstance().getKioskModeComponent() == fullscreen.get())
            juce::Desktop::getInstance().setKioskModeComponent (nullptr, false);
        fullscreen->removeChildComponent (&content);
        fullscreen.reset();
    }
    content.releaseComputerKeys();
    content.setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void RysumEditor::resized()
{
    if (fullscreen != nullptr) return;            // the content is on the full-screen window
    const float s = (float) getWidth() / (float) kW;
    content.setTransform (juce::AffineTransform::scale (s));
    content.setBounds (0, 0, kW, kH);
    if (sizeRestored) { scaleToSave = s; startTimer (1, 1500); }   // remembered once the resizing has stopped
}

void RysumEditor::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);
    if (fullscreen == nullptr) return;
    // while the interface is on the full screen, the plugin window only says where it went
    auto& A = Assets::get();
    auto r = getLocalBounds().toFloat();
    g.setColour (col::ink);
    g.setFont (A.frakturFont (juce::jmax (18.0f, r.getHeight() * 0.08f)));
    g.drawText ("Rysum", r.withTrimmedBottom (r.getHeight() * 0.2f), juce::Justification::centred);
    g.setColour (col::dim);
    g.setFont (A.sansFont (juce::jmax (11.0f, r.getHeight() * 0.025f)));
    g.drawText (juce::String::fromUTF8 ("l\xc3\xa4uft im Vollbild \xe2\x80\x93 hier klicken oder Esc dr\xc3\xbc" "cken, um zur\xc3\xbc" "ckzukehren"),
                r.withTrimmedTop (r.getHeight() * 0.12f), juce::Justification::centred);
}

void RysumEditor::toggleFullscreen()
{
    if (fullscreen == nullptr)
    {
        // the screen the plugin window is on
        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds());
        if (display == nullptr) return;
        content.releaseComputerKeys();
        fullscreen = std::make_unique<FullscreenWindow> (*this);
        fullscreen->setLookAndFeel (&lnf.get());
        fullscreen->setName ("Rysum");
        fullscreen->addAndMakeVisible (content);
        fullscreen->addToDesktop (juce::ComponentPeer::windowAppearsOnTaskbar);
        fullscreen->setBounds (display->totalArea);
        fullscreen->setVisible (true);
        juce::Desktop::getInstance().setKioskModeComponent (fullscreen.get(), false);
        fullscreen->toFront (true);
        content.grabKeyboardFocus();
        content.setFullscreenShown (true);
    }
    else
    {
        content.releaseComputerKeys();
        if (juce::Desktop::getInstance().getKioskModeComponent() == fullscreen.get())
            juce::Desktop::getInstance().setKioskModeComponent (nullptr, false);
        auto old = std::move (fullscreen);           // fullscreen == nullptr from here on (resized() places the content again)
        addAndMakeVisible (content);
        resized();
        old.reset();
        content.setFullscreenShown (false);
        if (auto* top = getTopLevelComponent()) top->toFront (true);
        content.grabKeyboardFocus();
    }
    repaint();
}

void RysumEditor::FullscreenWindow::resized()
{
    // the 1280 x 720 design as large as the screen allows, centred (black bars where the shape differs)
    auto& c = editor.content;
    const float s = juce::jmin ((float) getWidth() / (float) kW, (float) getHeight() / (float) kH);
    const float ox = 0.5f * ((float) getWidth() - s * (float) kW), oy = 0.5f * ((float) getHeight() - s * (float) kH);
    c.setTransform (juce::AffineTransform::scale (s).translated (ox, oy));
    c.setBounds (0, 0, kW, kH);
}

bool RysumEditor::FullscreenWindow::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey || k.getKeyCode() == juce::KeyPress::F11Key)
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<RysumEditor> (&editor)] { if (safe != nullptr) safe->toggleFullscreen(); });
        return true;
    }
    return false;
}

void RysumEditor::timerCallback (int id)
{
    if (id == 0) { content.tick(); return; }
    stopTimer (1);
    if (std::abs (scaleToSave - savedScale) > 0.004f) { saveWindowScale (scaleToSave); savedScale = scaleToSave; }
}
