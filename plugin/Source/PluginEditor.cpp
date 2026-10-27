#include "PluginEditor.h"

using namespace rysum;
using namespace rysum::ui;

static constexpr int kW = 1120, kH = 720, kHeader = 56;
static constexpr int kOutX = 942, kOutW = 108;      // output format combo in the header

RysumContent::RysumContent (RysumProcessor& p) : proc (p), console (p), space (p), klang (p), mod (p), fx (p), saal (p)
{
    addAndMakeVisible (console);
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
    outputChoice.setTooltip (juce::String::fromUTF8 ("Ausgabeformat: Stereo, AmbiX (ACN/SN3D) oder Binaural f\xc3\xbcr Kopfh\xc3\xb6rer (Einstellungen: Seite Klang)"));
    addAndMakeVisible (outputChoice);

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
    current = v;
    console.setVisible (v == 0);
    space.setVisible (v == 1);
    saal.setVisible (v == 2);
    klang.setVisible (v == 3);
    fx.setVisible (v == 4);
    mod.setVisible (v == 5);
    for (int i = 0; i < 6; ++i) tabs[(size_t) i].setToggleState (i == v, juce::dontSendNotification);
    if (proc.computerKeys) grabKeyboardFocus();
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
    patchPrev.setBounds (150, 14, 20, 28);
    patchName.setBounds (173, 14, 112, 28);
    patchNext.setBounds (288, 14, 20, 28);
    patchSave.setBounds (312, 14, 64, 28);
    keysButton.setBounds (384, 14, 76, 28);
    latchButton.setBounds (464, 14, 58, 28);
    const int tw[] { 84, 48, 48, 52, 66, 86 };
    int x = 530;
    for (int i = 0; i < 6; ++i) { tabs[(size_t) i].setBounds (x, 13, tw[i], 30); x += tw[i] + 4; }
    outputChoice.setBounds (kOutX, 14, kOutW, 28);
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
    }
    g.setFont (A.monoFont (11.0f));

    const int out = (int) proc.apvts.getRawParameterValue ("output")->load();
    const bool bin = out == RysumProcessor::kBinauralOutput;
    const int needed = out == 0 || bin ? 2 : (out + 1) * (out + 1);
    const int have = proc.getTotalNumOutputChannels();
    juce::String o = bin ? (proc.binauralReady() ? juce::String ("2 ch HRTF") : juce::String::fromUTF8 ("HRTF \xe2\x80\xa6")) : juce::String (needed) + " ch";
    if (have < needed) o = u8 ("nur ") + juce::String (have) + " ch!";
    g.setColour (have < needed ? col::white : col::faint);
    const juce::Rectangle<int> info (kOutX + kOutW + 6, 0, kW - 28 - (kOutX + kOutW + 6), kHeader);
    g.drawText (o, info.withTrimmedBottom (kHeader / 2).withTrimmedTop (9), juce::Justification::centredRight);
    g.setColour (col::faint);
    g.drawText (juce::String (proc.engine.activeVoices.load()) + " St.", info.withTrimmedTop (kHeader / 2).withTrimmedBottom (9), juce::Justification::centredRight);
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
RysumEditor::RysumEditor (RysumProcessor& p) : AudioProcessorEditor (p), proc (p), content (p)
{
    setLookAndFeel (&lnf.get());
    content.setLookAndFeel (&lnf.get());
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf.get());     // right-click menus and dialogs have no parent: they use the default
    addAndMakeVisible (content);
    setResizable (true, true);
    setResizeLimits (kW / 2, kH / 2, kW * 2, kH * 2);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio ((double) kW / (double) kH);
    setSize (kW, kH);
    startTimerHz (30);
}

RysumEditor::~RysumEditor()
{
    content.releaseComputerKeys();
    content.setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
    if (lnf.getReferenceCount() <= 1 && &juce::LookAndFeel::getDefaultLookAndFeel() == &lnf.get())
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

void RysumEditor::resized()
{
    const float s = (float) getWidth() / (float) kW;
    content.setTransform (juce::AffineTransform::scale (s));
    content.setBounds (0, 0, kW, kH);
}

void RysumEditor::timerCallback() { content.tick(); }
