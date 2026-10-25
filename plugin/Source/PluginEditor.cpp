#include "PluginEditor.h"

using namespace rysum;
using namespace rysum::ui;

static constexpr int kW = 1120, kH = 720, kHeader = 56;

RysumContent::RysumContent (RysumProcessor& p) : proc (p), console (p), space (p), klang (p), mod (p)
{
    addAndMakeVisible (console);
    addChildComponent (space);
    addChildComponent (klang);
    addChildComponent (mod);
    const char* names[] { "Spieltisch", "Raum", "Klang", "Modulation" };
    for (int i = 0; i < 4; ++i)
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

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("output")))
        outputChoice.addItemList (choice->choices, 1);
    outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "output", outputChoice);
    outputChoice.setTooltip ("Ausgabeformat: Stereo oder AmbiX (ACN/SN3D)");
    addAndMakeVisible (outputChoice);

    // grain over the whole interface, rendered once
    grainLayer = juce::Image (juce::Image::ARGB, kW, kH, true);
    {
        juce::Graphics g (grainLayer);
        drawGrain (g, { 0, 0, kW, kH }, 1.0f);
    }
    setSize (kW, kH);
}

void RysumContent::showView (int v)
{
    current = v;
    console.setVisible (v == 0);
    space.setVisible (v == 1);
    klang.setVisible (v == 2);
    mod.setVisible (v == 3);
    for (int i = 0; i < 4; ++i) tabs[(size_t) i].setToggleState (i == v, juce::dontSendNotification);
}

void RysumContent::resized()
{
    auto body = getLocalBounds().withTrimmedTop (kHeader);
    console.setBounds (body);
    space.setBounds (body);
    klang.setBounds (body);
    mod.setBounds (body);
    patchPrev.setBounds (150, 14, 24, 28);
    patchName.setBounds (178, 14, 160, 28);
    patchNext.setBounds (342, 14, 24, 28);
    patchSave.setBounds (372, 14, 72, 28);
    const int tw[] { 100, 60, 64, 100 };
    int x = 456;
    for (int i = 0; i < 4; ++i) { tabs[(size_t) i].setBounds (x, 13, tw[i], 30); x += tw[i] + 4; }
    outputChoice.setBounds (kW - 28 - 60 - 170, 14, 170, 28);
}

void RysumContent::tick()
{
    if (console.isVisible()) console.refresh();
    if (space.isVisible()) space.refresh();
    if (klang.isVisible()) klang.refresh();
    if (mod.isVisible()) mod.refresh();
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
    g.setColour (col::line);
    g.fillRect (0, kHeader - 1, kW, 1);

    g.setColour (col::ink);
    g.setFont (A.boldFont (20.0f).withExtraKerningFactor (0.42f));
    g.drawText ("RYSUM", head.withWidth (130), juce::Justification::centredLeft);
    g.setFont (A.monoFont (11.0f));

    const int out = (int) proc.apvts.getRawParameterValue ("output")->load();
    const int needed = out == 0 ? 2 : (out + 1) * (out + 1);
    const int have = proc.getTotalNumOutputChannels();
    juce::String o = juce::String (needed) + " ch";
    if (have < needed) o = u8 ("nur ") + juce::String (have) + " ch!";
    g.setColour (have < needed ? col::white : col::faint);
    g.drawText (o, juce::Rectangle<int> (kW - 28 - 60 - 170 - 74, 0, 66, kHeader), juce::Justification::centredRight);
    g.setColour (col::faint);
    g.drawText (juce::String (proc.engine.activeVoices.load()) + " St.", head.withTrimmedLeft (980), juce::Justification::centredRight);
}

void RysumContent::showModMenu (juce::Component& target, const juce::String& paramId)
{
    const int idx = proc.getParamIndex (paramId);
    auto* prm = proc.getParamAt (idx);
    if (prm == nullptr) return;
    juce::PopupMenu m;
    m.addSectionHeader (prm->getName (64));
    if (! proc.isModTarget (idx))
    {
        m.addItem (-1, u8 ("Schalter/Auswahl \xe2\x80\x93 nicht modulierbar"), false);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
        return;
    }
    bool free = false;
    for (int r = 0; r < kNumRoutes; ++r) free = free || (proc.getRouteSrc (r) < 0 && proc.getRouteTgt (r) < 0);
    juce::PopupMenu with;
    const auto names = modSourceNames();
    for (int s = 0; s < kNumSources; ++s) with.addItem (1 + s, names[s]);
    m.addSubMenu (free ? juce::String ("Modulieren mit") : u8 ("Modulieren mit (alle 12 Pl\xc3\xa4tze belegt)"), with, free);
    for (int r = 0; r < kNumRoutes; ++r)
        if (proc.getRouteTgt (r) == kGlobalTargetBase + idx && proc.getRouteSrc (r) >= 0)
        {
            auto* a = proc.apvts.getParameter (proc.getModAmountId (r));
            m.addItem (100 + r, "Entfernen: " + names[proc.getRouteSrc (r)] + " (" + (a != nullptr ? a->getCurrentValueAsText() : juce::String()) + ")");
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
        else if (res == 300) self.showView (3);
    });
}

void RysumContent::paintOverChildren (juce::Graphics& g)
{
    g.setOpacity (0.22f);
    g.drawImageAt (grainLayer, 0, 0);
}

// ---------------------------------------------------------------------------
RysumEditor::RysumEditor (RysumProcessor& p) : AudioProcessorEditor (p), proc (p), content (p)
{
    setLookAndFeel (&lnf);
    content.setLookAndFeel (&lnf);
    addAndMakeVisible (content);
    setResizable (true, true);
    setResizeLimits (kW / 2, kH / 2, kW * 2, kH * 2);
    if (auto* c = getConstrainer()) c->setFixedAspectRatio ((double) kW / (double) kH);
    setSize (kW, kH);
    startTimerHz (30);
}

RysumEditor::~RysumEditor()
{
    content.setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void RysumEditor::resized()
{
    const float s = (float) getWidth() / (float) kW;
    content.setTransform (juce::AffineTransform::scale (s));
    content.setBounds (0, 0, kW, kH);
}

void RysumEditor::timerCallback() { content.tick(); }
