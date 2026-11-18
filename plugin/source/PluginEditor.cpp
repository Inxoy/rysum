#include "PluginEditor.h"

using namespace rysum;
using namespace rysum::ui;

static constexpr int kW = 1280, kH = 720, kHeader = 56; // 16:9 - fills a full-HD screen at 1.5x
static constexpr int kOutX = kW - 206, kOutW = 100;     // output format combo in the header
static constexpr int kFullX = kW - 56;                  // full-screen button (28 x 28)

RysumContent::RysumContent(RysumProcessor &p)
    : proc(p), console(p), space(p), klang(p), mod(p), fx(p), saal(p)
{
    addAndMakeVisible(console);
    addChildComponent(organ);
    addChildComponent(speakers);
    addChildComponent(walk);
    klang.onOpenSpeakers = [this] { showView(7); };
    // the Raum page's timetables are lanes of the Verlauf page (and its undo)
    space.onTimetable = [this](int stop, bool exists)
    {
        if (exists)
            verlauf.noteExternalEdit();
        else
            verlauf.addLaneFor(rysum::autoPathLaneId(stop));
    };
    space.setTimetableEditHook([this] { verlauf.noteExternalEdit(); });
    saal.onOpenWalk = [this]
    {
        releaseComputerKeys();
        showView(8);
    };
    walk.onBack = [this] { showView(2); };
    // "Immersiv": the picture on the whole screen
    walk.prepareImmersive = [this]
    {
        if (fullButton.active || !onToggleFullscreen)
            return false;
        immersiveFull = true;
        onToggleFullscreen();
        return true;
    };
    walk.onImmersive = [this](bool on)
    {
        immersive = on;
        resized();
        if (on)
            walk.toFront(true);
        else if (immersiveFull)
        {
            // back to the window it came from
            immersiveFull = false;
            juce::Component::SafePointer<RysumContent> safe(this);
            juce::MessageManager::callAsync(
                [safe]
                {
                    if (safe == nullptr)
                        return;
                    if (safe->fullButton.active && safe->onToggleFullscreen)
                        safe->onToggleFullscreen();
                    if (safe->walk.isShowing())
                        safe->walk.grabKeyboardFocus();
                });
        }
    };
    outInfoButton.setTooltip(rysum::tr("Lautsprecher einrichten (AllRAD)"));
    outInfoButton.setAlpha(0.0f);
    outInfoButton.onClick = [this] { showView(current == 7 ? viewBeforeOrgan : 7); };
    addAndMakeVisible(outInfoButton);
    logoButton.setTooltip(
        rysum::tr("\xc3\x9c"
                               "ber die Orgel zu Rysum (nochmal klicken: zur\xc3\xbc"
                               "ck)"));
    logoButton.onClick = [this] { showView(current == 6 ? viewBeforeOrgan : 6); };
    addAndMakeVisible(logoButton);
    diceButton.setTooltip(
        rysum::tr("W\xc3\xbcrfeln: zuf\xc3\xa4llige Einstellungen \xe2\x80\x93 sanft "
                               "oder wild, nur in den gew\xc3\xa4hlten Bereichen. "
                               "Ein Schritt zur\xc3\xbc"
                               "ck ist m\xc3\xb6glich."));
    diceButton.onClick = [this] { showDiceMenu(); };
    addAndMakeVisible(diceButton);
    addChildComponent(space);
    addChildComponent(klang);
    addChildComponent(mod);
    addChildComponent(fx);
    addChildComponent(saal);
    addChildComponent(verlauf);
    const char *names[]{"Spieltisch", "Raum", "Saal", "Klang", "Effekte", "Modulation", "Verlauf"};
    for (int i = 0; i < kNumTabs; ++i)
    {
        tabs[(size_t)i].setButtonText(rysum::tr(names[i]).toUpperCase());
        tabs[(size_t)i].onClick = [this, i] { showView(tabView(i)); };
        addAndMakeVisible(tabs[(size_t)i]);
    }
    showView(0);

    patchName.setTooltip(
        rysum::tr("Patches: Klang, Register, Raum, Modulation und Hall (ohne Ausgabeformat)"));
    patchName.onClick = [this] { showPatchMenu(); };
    patchPrev.onClick = [this] { stepPatch(-1); };
    patchNext.onClick = [this] { stepPatch(1); };
    patchSave.onClick = [this] { askSavePatch(); };
    patchPrev.setTooltip(rysum::tr("Vorheriger Patch"));
    patchNext.setTooltip(rysum::tr("N\xc3\xa4"
                            "chster Patch"));
    for (auto *b : {&patchPrev, &patchName, &patchNext, &patchSave})
        addAndMakeVisible(b);

    // playing helpers: computer keyboard, "Halten"
    keyNoteDown.fill(-1);
    setWantsKeyboardFocus(true);
    keysButton.setTooltip(rysum::tr("Mit der Computertastatur spielen: A W S E D F T G Z H U J K O L P = C bis "
        "D\xe2\x80\xb2\xe2\x80\xb2, "
        "Y / X = Oktave tiefer / h\xc3\xb6her, C / V = leiser / lauter anschlagen. "
        "Im Host vorher ins Plugin-Fenster klicken."));
    keysButton.onClick = [this]
    {
        proc.computerKeys = !proc.computerKeys;
        if (proc.computerKeys)
            grabKeyboardFocus();
        else
            releaseComputerKeys();
        updatePlayButtons();
    };
    latchButton.setTooltip(rysum::tr("Halten: eine Taste zweimal schnell anschlagen \xe2\x80\x93 der Ton bleibt liegen, als ob "
        "du sie h\xc3\xa4ltst "
        "(H\xc3\xa4nde frei f\xc3\xbcr Register und Modulation). Nochmal anschlagen = loslassen. "
        "Ausschalten l\xc3\xa4sst alle gehaltenen T\xc3\xb6ne los."));
    latchButton.onClick = [this]
    {
        proc.latchOn = !proc.latchOn.load();
        updatePlayButtons();
    };
    addAndMakeVisible(keysButton);
    addAndMakeVisible(latchButton);
    updatePlayButtons();

    if (auto *choice =
            dynamic_cast<juce::AudioParameterChoice *>(proc.apvts.getParameter("output")))
        outputChoice.addItemList(choice->choices, 1);
    outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        proc.apvts, "output", outputChoice);
    outputChoice.setTooltip(
        rysum::tr("Ausgabeformat: Stereo, AmbiX (ACN/SN3D), Binaural f\xc3\xbcr "
                               "Kopfh\xc3\xb6rer (Einstellungen: Seite Klang) "
                               "oder Lautsprecher (AllRAD, Klick auf die Kanalanzeige daneben)"));
    outputChoice.onChange = [this]
    {
        // switching to loudspeakers: show the layout once
        if (outputChoice.getSelectedId() - 1 == RysumProcessor::kSpeakerOutput && current != 7 &&
            !speakerPageShown)
        {
            speakerPageShown = true;
            showView(7);
        }
    };
    addAndMakeVisible(outputChoice);
    fullButton.onClick = [this]
    {
        if (onToggleFullscreen)
            onToggleFullscreen();
    };
    setFullscreenShown(false);
    addAndMakeVisible(fullButton);
    langButton.setTooltip("Sprache: Deutsch / English (Klick wechselt)");
    langButton.onClick = [this]
    {
        rysum::setLanguage(rysum::german() ? rysum::Language::English : rysum::Language::German);
        if (onLanguageChanged)
            onLanguageChanged();
    };
    addAndMakeVisible(langButton);

    // grain over the whole interface, rendered once
    grainLayer = juce::Image(juce::Image::ARGB, kW, kH, true);
    {
        juce::Graphics g(grainLayer);
        drawGrain(g, {0, 0, kW, kH}, 1.0f);
    }
    // darkened edges, like an old print
    vignette = juce::Image(juce::Image::ARGB, kW, kH, true);
    {
        juce::Graphics g(vignette);
        juce::ColourGradient grad(juce::Colours::transparentBlack, kW * 0.5f, kH * 0.48f,
                                  juce::Colours::black.withAlpha(0.22f), 0.0f, 0.0f, true);
        grad.addColour(0.7, juce::Colours::transparentBlack);
        g.setGradientFill(grad);
        g.fillRect(0, 0, kW, kH);
    }
    // ... both in one image: every repainted area draws one overlay instead of two (in full screen
    // that counts)
    {
        juce::Graphics g(vignette);
        g.setOpacity(0.22f);
        g.drawImageAt(grainLayer, 0, 0);
    }
    grainLayer = {};
    // MIDI buttons -> buttons on screen: everything directly in here has a fixed name, so the
    // saved paths of the buttons do not depend on the order things are stacked in
    {
        const std::pair<juce::Component *, const char *> named[]{
            {&console, "spieltisch"},  {&space, "raum"},         {&saal, "saal"},
            {&klang, "klang"},         {&fx, "effekte"},         {&mod, "modulation"}, {&verlauf, "verlauf"},
            {&organ, "orgel"},         {&speakers, "lautsprecher"}, {&walk, "begehen"},
            {&outInfoButton, "ausgabe-info"}, {&logoButton, "logo"}, {&diceButton, "zufall"},
            {&keysButton, "tastatur"}, {&latchButton, "halten"}, {&fullButton, "vollbild"},
            {&langButton, "sprache"},
            {&outputChoice, "ausgabe"}, {&patchPrev, "patch-zurueck"}, {&patchNext, "patch-vor"},
            {&patchName, "patch"},     {&patchSave, "patch-speichern"}, {&learnOverlay, "midi-tasten"}};
        for (auto &n : named)
            n.first->setComponentID(n.second);
        for (int i = 0; i < kNumTabs; ++i)
            tabs[(size_t)i].setComponentID("seite-" + juce::String(i));
    }
    learnOverlay.onClose = [this] { stopButtonLearn(); };
    addChildComponent(learnOverlay);
    {
        // presses that came in while no window was open are not made up for
        int slot = 0;
        bool down = false;
        while (proc.nextButtonEvent(slot, down))
        {
        }
    }
    setSize(kW, kH);
}

// ---------------------------------------------------------------------------
// MIDI buttons -> buttons on screen
// ---------------------------------------------------------------------------
namespace rysum::ui
{
static juce::String stepName(juce::Component &c)
{
    auto clean = [](const juce::String &s) { return s.replaceCharacter('/', '_'); };
    if (c.getComponentID().isNotEmpty())
        return "@" + clean(c.getComponentID());
    const auto name = c.getName();
    int nth = 0;
    if (auto *p = c.getParentComponent())
        for (auto *s : p->getChildren())
        {
            if (s == &c)
                break;
            if (s->getComponentID().isEmpty() && s->getName() == name)
                ++nth;
        }
    if (name.isEmpty())
        return "#" + juce::String(nth);
    return clean(name) + (nth > 0 ? "~" + juce::String(nth) : juce::String());
}

juce::String buttonPath(juce::Component &root, juce::Button &b)
{
    juce::StringArray steps;
    for (juce::Component *c = &b; c != &root; c = c->getParentComponent())
    {
        if (c == nullptr)
            return {}; // not inside the interface
        steps.insert(0, stepName(*c));
    }
    return steps.joinIntoString("/");
}

juce::Button *findButton(juce::Component &root, const juce::String &path)
{
    if (path.isEmpty())
        return nullptr;
    juce::Component *c = &root;
    for (const auto &step : juce::StringArray::fromTokens(path, "/", ""))
    {
        juce::Component *next = nullptr;
        for (auto *ch : c->getChildren())
            if (stepName(*ch) == step)
            {
                next = ch;
                break;
            }
        if (next == nullptr)
            return nullptr;
        c = next;
    }
    return dynamic_cast<juce::Button *>(c);
}

ButtonLearnOverlay::ButtonLearnOverlay(RysumProcessor &p, juce::Component &r) : proc(p), root(r)
{
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void ButtonLearnOverlay::collect(juce::Component &parent, juce::Rectangle<int> clip,
                                 std::vector<Item> &out) const
{
    for (auto *ch : parent.getChildren())
    {
        if (ch == this || !ch->isVisible())
            continue;
        const auto area = getLocalArea(ch, ch->getLocalBounds()).getIntersection(clip);
        if (area.isEmpty())
            continue;
        if (auto *b = dynamic_cast<juce::Button *>(ch))
        {
            // (not the arrows of scroll bars and number boxes)
            if (dynamic_cast<juce::ScrollBar *>(&parent) == nullptr &&
                dynamic_cast<juce::Slider *>(&parent) == nullptr)
            {
                Item it;
                it.button = b;
                it.area = area;
                describe(*b, it.label, it.waiting);
                out.push_back(it);
            }
            continue;
        }
        collect(*ch, area, out);
    }
}

int ButtonLearnOverlay::paramOf(juce::Button &b) const
{
    const auto id = b.getProperties()["midiParam"].toString();
    return id.isEmpty() ? -1 : proc.getParamIndex(id);
}

void ButtonLearnOverlay::describe(juce::Button &b, juce::String &label, bool &waiting) const
{
    label.clear();
    waiting = false;
    const int prm = paramOf(b);
    if (prm >= 0)
    {
        for (int slot : proc.midiMapsFor(prm))
        {
            const auto m = proc.getMidiMap(slot);
            if (slot == proc.getLearningSlot())
                waiting = true;
            else if (m.cc >= 0 && label.isEmpty())
                label = RysumProcessor::triggerName(m.cc, m.channel);
        }
        return;
    }
    for (int slot : proc.buttonMapsFor(buttonPath(root, b)))
    {
        const auto m = proc.getButtonMap(slot);
        if (slot == proc.getLearningButton())
            waiting = true;
        else if (m.code >= 0 && label.isEmpty())
            label = RysumProcessor::triggerName(m.code, m.channel);
    }
}

juce::String ButtonLearnOverlay::mappingOf(juce::Button &b) const
{
    juce::String label;
    bool waiting = false;
    describe(b, label, waiting);
    return label;
}

void ButtonLearnOverlay::stopWaiting()
{
    proc.stopButtonLearn();
    if (waitingParamSlot >= 0 && proc.getLearningSlot() == waitingParamSlot)
        proc.clearMidiMap(waitingParamSlot); // nothing was pressed: no empty assignment stays
    waitingParamSlot = -1;
}

void ButtonLearnOverlay::forget(juce::Button &b)
{
    stopWaiting();
    const int prm = paramOf(b);
    if (prm >= 0)
    {
        for (int slot : proc.midiMapsFor(prm))
            proc.clearMidiMap(slot);
        return;
    }
    for (int slot : proc.buttonMapsFor(buttonPath(root, b)))
        proc.clearButtonMap(slot);
}

bool ButtonLearnOverlay::learn(juce::Button &b)
{
    forget(b); // (one MIDI button per button: a new one replaces the old)
    const int prm = paramOf(b);
    if (prm >= 0)
    {
        // a switch with a parameter: the MIDI button flips the parameter
        const int slot = proc.learnMidiFor(prm);
        if (slot < 0)
            return false;
        auto m = proc.getMidiMap(slot);
        m.toggle = true;
        proc.setMidiMap(slot, m);
        waitingParamSlot = slot;
        return true;
    }
    return proc.learnButton(buttonPath(root, b)) >= 0;
}

int ButtonLearnOverlay::itemAt(juce::Point<int> p) const
{
    for (int i = (int)items.size(); --i >= 0;)
        if (items[(size_t)i].area.contains(p))
            return i;
    return -1;
}

void ButtonLearnOverlay::refresh()
{
    std::vector<Item> now;
    collect(root, getLocalBounds(), now);
    if (now == items)
        return;
    items = std::move(now);
    hover = juce::jmin(hover, (int)items.size() - 1);
    repaint();
}

void ButtonLearnOverlay::visibilityChanged()
{
    if (isVisible())
    {
        note.clear();
        refresh();
        if (isShowing())
            grabKeyboardFocus();
    }
    else
        stopWaiting();
}

void ButtonLearnOverlay::mouseMove(const juce::MouseEvent &e)
{
    const int h = itemAt(e.getPosition());
    if (h != hover)
    {
        hover = h;
        repaint();
    }
}

void ButtonLearnOverlay::mouseExit(const juce::MouseEvent &)
{
    if (hover >= 0)
    {
        hover = -1;
        repaint();
    }
}

void ButtonLearnOverlay::mouseDown(const juce::MouseEvent &e)
{
    grabKeyboardFocus();
    note.clear();
    const int i = itemAt(e.getPosition());
    if (i < 0)
    {
        stopWaiting();
        refresh();
        return;
    }
    auto &b = *items[(size_t)i].button;
    if (e.mods.isPopupMenu())
        forget(b);
    else if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        // the button itself (to get to another page, for instance)
        stopWaiting();
        if (b.isEnabled())
            b.triggerClick();
    }
    else if (!learn(b))
        note = rysum::tr("Alle Pl\xc3\xa4tze sind belegt.");
    refresh();
    repaint();
}

bool ButtonLearnOverlay::keyPressed(const juce::KeyPress &k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        if (onClose)
            onClose();
        return true;
    }
    return false;
}

void ButtonLearnOverlay::paint(juce::Graphics &g)
{
    auto &A = Assets::get();
    g.fillAll(col::bg.withAlpha(0.5f));
    const auto font = A.monoFont(10.0f);
    g.setFont(font);
    for (int i = 0; i < (int)items.size(); ++i)
    {
        const auto &it = items[(size_t)i];
        const auto r = it.area.toFloat().reduced(0.5f);
        const bool mapped = it.label.isNotEmpty();
        if (it.waiting)
        {
            g.setColour(col::rubric.withAlpha(0.35f));
            g.fillRect(r);
        }
        g.setColour(it.waiting || mapped ? col::rubric.brighter(0.25f)
                                         : col::ink.withAlpha(i == hover ? 0.85f : 0.3f));
        g.drawRect(r, it.waiting ? 2.0f : (mapped || i == hover ? 1.5f : 1.0f));
        const auto text =
            it.waiting ? rysum::tr("MIDI-Taste dr\xc3\xbc"
                                                "cken \xe2\x80\xa6")
                       : it.label;
        if (text.isEmpty())
            continue;
        const float w = juce::GlyphArrangement::getStringWidth(font, text) + 8.0f;
        auto tag = juce::Rectangle<float>(w, 13.0f)
                       .withPosition(juce::jmin(r.getX(), (float)getWidth() - w), r.getY());
        g.setColour(col::rubric);
        g.fillRect(tag);
        g.setColour(col::white);
        g.drawText(text, tag, juce::Justification::centred);
    }
    // what to do, at the bottom
    auto band = getLocalBounds().removeFromBottom(26);
    g.setColour(col::bg.withAlpha(0.94f));
    g.fillRect(band);
    g.setColour(col::rubric);
    g.fillRect(band.removeFromTop(1));
    g.setFont(A.monoFont(11.0f));
    g.setColour(col::ink);
    g.drawText(note.isNotEmpty()
                   ? note
                   : rysum::tr("MIDI-Tasten zuordnen:  Knopf anklicken, dann die MIDI-Taste dr\xc3\xbc"
                         "cken (Pad, Taster oder Klaviertaste)   \xc2\xb7   Rechtsklick: "
                         "Zuordnung l\xc3\xb6schen   \xc2\xb7   Strg+Klick: Knopf bet\xc3\xa4tigen "
                         "(z. B. Seite wechseln)   \xc2\xb7   Esc: fertig"),
               band.reduced(14, 0), juce::Justification::centredLeft);
}
} // namespace rysum::ui

void RysumContent::showView(int v)
{
    if ((v == 6 || v == 7) && current != 6 && current != 7 && current != 8)
        viewBeforeOrgan = current;
    if (v == 8 && current != 8)
        viewBeforeOrgan = 2;
    current = v;
    organ.setVisible(v == 6);
    speakers.setVisible(v == 7);
    walk.setVisible(v == 8);
    console.setVisible(v == 0);
    space.setVisible(v == 1);
    saal.setVisible(v == 2);
    klang.setVisible(v == 3);
    fx.setVisible(v == 4);
    mod.setVisible(v == 5);
    verlauf.setVisible(v == 9);
    for (int i = 0; i < kNumTabs; ++i)
        tabs[(size_t)i].setToggleState(tabView(i) == v, juce::dontSendNotification);
    if (v == 8)
    {
        if (walk.isShowing())
            walk.grabKeyboardFocus();
    }
    else if (v == 9 && !proc.computerKeys)
        verlauf.grabKeyboardFocus();
    else if (proc.computerKeys)
        grabKeyboardFocus();
}

void RysumContent::resized()
{
    auto body = getLocalBounds().withTrimmedTop(kHeader);
    console.setBounds(body);
    space.setBounds(body);
    klang.setBounds(body);
    mod.setBounds(body);
    verlauf.setBounds(body);
    fx.setBounds(body);
    saal.setBounds(body);
    organ.setBounds(body);
    speakers.setBounds(body);
    walk.setBounds(immersive ? getLocalBounds() : body);
    learnOverlay.setBounds(getLocalBounds());
    outInfoButton.setBounds(kOutX + kOutW + 2, 8, kFullX - 5 - (kOutX + kOutW + 2), kHeader - 16);
    logoButton.setBounds(24, 6, 116, 44);
    // (tightened for the seventh page, Verlauf)
    patchPrev.setBounds(152, 14, 22, 28);
    patchName.setBounds(176, 14, 100, 28);
    patchNext.setBounds(278, 14, 22, 28);
    patchSave.setBounds(303, 14, 70, 28);
    diceButton.setBounds(378, 14, 54, 28);
    keysButton.setBounds(437, 14, 76, 28);
    latchButton.setBounds(517, 14, 60, 28);
    const int tw[]{88, 52, 48, 54, 68, 92, 70};
    // Spieltisch, Klang, Effekte, Modulation, Verlauf, Saal, Raum
    const int tabOrder[]{0, 3, 4, 5, 6, 2, 1};
    int x = 582;
    for (const int i : tabOrder)
    {
        tabs[(size_t)i].setBounds(x, 13, tw[i], 30);
        x += tw[i] + 2;
    }
    outputChoice.setBounds(kOutX, 14, kOutW, 28);
    fullButton.setBounds(kFullX, 14, 28, 28);
    langButton.setBounds(kFullX + 31, 14, 22, 28);
}

void RysumContent::updatePlayButtons()
{
    keysButton.setToggleState(proc.computerKeys, juce::dontSendNotification);
    keysButton.setButtonText(proc.computerKeys ? rysum::tr("Tastatur C") + juce::String(proc.keysOctave)
                                               : rysum::tr("Tastatur"));
    const int n = proc.latchedCount.load();
    latchButton.setToggleState(proc.latchOn.load(), juce::dontSendNotification);
    latchButton.setButtonText(proc.latchOn.load() && n > 0 ? rysum::tr("Halten ") + juce::String(n)
                                                           : rysum::tr("Halten"));
}

// computer keyboard: two rows like a piano (German layout: Z is the black key between G and H)
static const char kPianoKeys[] = "awsedftgzhujkolp";

void RysumContent::releaseComputerKeys()
{
    for (auto &n : keyNoteDown)
        if (n >= 0)
        {
            proc.keyboardState.noteOff(1, n, 0.0f);
            n = -1;
        }
}

static juce::File paintStatsFile();

bool RysumContent::keyPressed(const juce::KeyPress &k)
{
    // performance readout / renderer switch (for finding what makes the picture slow)
    if (k.getModifiers().isCtrlDown() && k.getModifiers().isShiftDown() &&
        (k.getKeyCode() == 'P' || k.getKeyCode() == 'R' || k.getKeyCode() == 'K'))
    {
        if (k.getKeyCode() == 'R')
            cycleRenderer();
        else if (k.getKeyCode() == 'K')
        {
            // the cores for the audio: automatisch, 1, 2, 3, 4 (shown in the readout)
            if (auto *prm = proc.apvts.getParameter("audioThreads"))
            {
                const int steps = prm->getNumSteps();
                const int now = juce::roundToInt(prm->getValue() * (float)(steps - 1));
                prm->beginChangeGesture();
                prm->setValueNotifyingHost((float)((now + 1) % steps) / (float)(steps - 1));
                prm->endChangeGesture();
            }
            if (!stats.on)
                startStats();
            repaint();
        }
        else
        {
            if (stats.on)
            {
                stats.on = false;
                proc.engine.profOn = false;
                walk.overText = {};
            }
            else
                startStats();
            repaint();
        }
        return true;
    }
    if (k.getModifiers().isCtrlDown() && !k.getModifiers().isShiftDown() && k.getKeyCode() == 'M')
    {
        if (learnOverlay.isVisible())
            stopButtonLearn();
        else
            startButtonLearn();
        return true;
    }
    // F11: full screen on / off; Esc leaves it
    if (k.getKeyCode() == juce::KeyPress::F11Key ||
        (k == juce::KeyPress::escapeKey && fullButton.active))
    {
        if (onToggleFullscreen)
            onToggleFullscreen();
        return true;
    }
    if (!proc.computerKeys || k.getModifiers().isCommandDown() || k.getModifiers().isCtrlDown() ||
        k.getModifiers().isAltDown())
        return false;
    if (dynamic_cast<juce::TextEditor *>(juce::Component::getCurrentlyFocusedComponent()) !=
        nullptr)
        return false; // typing a value
    const auto c = juce::CharacterFunctions::toLowerCase(k.getTextCharacter());
    if (c == 'y' || c == 'x')
    {
        releaseComputerKeys();
        proc.keysOctave =
            juce::jlimit(kExtendedLowestMidiNote / 12 - 1, kExtendedHighestMidiNote / 12 - 1,
                         proc.keysOctave + (c == 'x' ? 1 : -1));
        updatePlayButtons();
        return true;
    }
    if (c == 'c' || c == 'v')
    {
        keysVelocity = juce::jlimit(10, 127, keysVelocity + (c == 'v' ? 20 : -20));
        return true;
    }
    for (const char *p = kPianoKeys; *p != 0; ++p)
        if (c == (juce::juce_wchar)*p)
            return true; // played in keyStateChanged
    return false;
}

bool RysumContent::keyStateChanged(bool)
{
    if (!proc.computerKeys)
        return false;
    if (dynamic_cast<juce::TextEditor *>(juce::Component::getCurrentlyFocusedComponent()) !=
        nullptr)
        return false;
    bool used = false;
    for (int i = 0; kPianoKeys[i] != 0; ++i)
    {
        const bool down = juce::KeyPress::isKeyCurrentlyDown(kPianoKeys[i]);
        auto &note = keyNoteDown[(size_t)i];
        if (down && note < 0)
        {
            note = 12 * (proc.keysOctave + 1) + i;
            proc.keyboardState.noteOn(1, note, (float)keysVelocity / 127.0f);
            used = true;
        }
        else if (!down && note >= 0)
        {
            proc.keyboardState.noteOff(1, note, 0.0f);
            note = -1;
            used = true;
        }
    }
    return used;
}

void RysumContent::tick()
{
    if (stats.on)
        updateStats();
    tickBody();
}

void RysumContent::runImmersiveTest()
{
    auto log = std::make_shared<juce::String>();
    juce::Component::SafePointer<RysumContent> safe(this);
    showView(8);
    const bool wasFull = fullButton.active;
    walk.toggleImmersive();
    auto mouse = juce::Desktop::getInstance().getMainMouseSource();
    // a second later: still on? then the pointer is pushed 120 pixels to the right
    juce::Timer::callAfterDelay(1200, [safe, log, mouse]() mutable
    {
        if (safe == nullptr)
            return;
        const float yaw0 = safe->walk.lookYawForTest();
        *log << "immersive on after 1.2 s: " << (int)safe->walk.isImmersive()
             << "   full screen: " << (int)safe->fullButton.active
             << "   keyboard in the view: " << (int)safe->walk.hasKeyboardFocus(false)
             << "   view fills the window: " << (int)(safe->walk.getBounds() == safe->getLocalBounds())
             << "\n";
        mouse.setScreenPosition(mouse.getScreenPosition() + juce::Point<float>(120.0f, 0.0f));
        juce::Timer::callAfterDelay(1200, [safe, log, mouse, yaw0]() mutable
        {
            if (safe == nullptr)
                return;
            const auto centre = safe->walk.getScreenBounds().getCentre().toFloat();
            *log << "pointer pushed 120 px: the view turned by "
                 << juce::String(safe->walk.lookYawForTest() - yaw0, 2)
                 << " degrees (expected -18), pointer now "
                 << juce::String(mouse.getScreenPosition().getDistanceFrom(centre), 1)
                 << " px from the middle of the view, picture: " << safe->walk.pictureInfo() << "\n";
            safe->walk.setImmersive(false);
            juce::Timer::callAfterDelay(1500, [safe, log]
            {
                if (safe == nullptr)
                    return;
                *log << "after leaving: immersive " << (int)safe->walk.isImmersive()
                     << "   full screen: " << (int)safe->fullButton.active
                     << "   controls back: " << (int)(safe->walk.getNumChildComponents() > 0 &&
                                                       safe->walk.getChildComponent(0)->isVisible())
                     << "   picture: " << safe->walk.pictureInfo() << "\n";
                juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile("Rysum")
                    .getChildFile("immersive-test.txt")
                    .replaceWithText(*log);
                if (auto *app = juce::JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        });
    });
    juce::ignoreUnused(wasFull);
}

void RysumContent::startButtonLearn()
{
    if (walk.isImmersive())
        walk.setImmersive(false);
    releaseComputerKeys();
    learnOverlay.setVisible(true);
    learnOverlay.toFront(true);
    learnOverlay.refresh();
}

void RysumContent::stopButtonLearn()
{
    learnOverlay.setVisible(false);
    if (walk.isShowing())
        walk.grabKeyboardFocus();
    else
        grabKeyboardFocus();
}

void RysumContent::tickBody()
{
    updatePlayButtons();
    // (the card's picture of the 3D view is a window of its own: while something of this window lies over
    //  the view - the overlay for assigning MIDI buttons - the processor draws)
    walk.setGpuBlocked(learnOverlay.isVisible());
    // MIDI buttons mapped to buttons on screen
    {
        int slot = 0;
        bool down = false;
        while (proc.nextButtonEvent(slot, down))
        {
            // (while assigning nothing is operated - but what is held is still let go)
            const bool assigning = learnOverlay.isVisible();
            auto *b = rysum::ui::findButton(*this, proc.getButtonMap(slot).target);
            if (b == nullptr)
                continue;
            if (auto *h = dynamic_cast<rysum::ui::HoldButton *>(b))
                h->midiHeld = down && !assigning; // (held for as long as the MIDI button is)
            else if (down && !assigning && b->isEnabled())
                b->triggerClick();
        }
        if (learnOverlay.isVisible())
            learnOverlay.refresh();
    }
    // the window lost the keyboard: let the computer keys go (they would hang otherwise)
    if (proc.computerKeys && !hasKeyboardFocus(true))
    {
        auto *peer = getPeer();
        if (peer != nullptr && peer->isFocused() &&
            juce::Component::getCurrentlyFocusedComponent() == nullptr)
            grabKeyboardFocus();
        else
            releaseComputerKeys();
    }
    if (console.isVisible())
        console.refresh();
    if (space.isVisible())
        space.refresh();
    if (klang.isVisible())
        klang.refresh();
    if (mod.isVisible())
        mod.refresh();
    if (fx.isVisible())
        fx.refresh();
    if (saal.isVisible())
        saal.refresh();
    if (speakers.isVisible())
        speakers.refresh();
    if (proc.patchVersion != shownPatchVersion)
    {
        shownPatchVersion = proc.patchVersion;
        const auto n = proc.getPatchName();
        patchName.setButtonText(n.isEmpty() ? rysum::tr("\xe2\x80\x93 kein Patch \xe2\x80\x93") : n);
    }
    repaint(0, 0, kW, kHeader);
}

void RysumContent::showPatchMenu()
{
    const auto files = RysumProcessor::getPatchFiles();
    juce::PopupMenu m;
    m.addSectionHeader("Patches");
    for (int i = 0; i < files.size(); ++i)
        m.addItem(1 + i, files[i].getFileNameWithoutExtension(), true,
                  files[i].getFileNameWithoutExtension() == proc.getPatchName());
    if (files.isEmpty())
        m.addItem(-1, rysum::tr("(noch keine)"), false);
    m.addSeparator();
    m.addItem(10000, rysum::tr("Speichern unter ..."));
    m.addItem(10001, rysum::tr("Patch-Ordner \xc3\xb6"
                        "ffnen"));
    m.addSeparator();
    m.addItem(10002, rysum::tr("Alles zur\xc3\xbc"
                        "cksetzen \xe2\x80\xa6"));
    m.addSeparator();
    m.addItem(10003, proc.concert.role() == rysum::ConcertLink::Role::off
                         ? rysum::tr("Konzert \xe2\x80\xa6")
                         : rysum::tr("Konzert \xe2\x80\x93 ") + proc.concert.status());
    juce::Component::SafePointer<RysumContent> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&patchName),
                    [safe, files](int r)
                    {
                        if (safe == nullptr || r == 0)
                            return;
                        if (r == 10003)
                        {
                            safe->showConcertPanel();
                            return;
                        }
                        if (r == 10000)
                        {
                            safe->askSavePatch();
                            return;
                        }
                        if (r == 10001)
                        {
                            auto folder = RysumProcessor::getPatchFolder();
                            folder.createDirectory();
                            folder.startAsProcess();
                            return;
                        }
                        if (r == 10002)
                        {
                            safe->askResetEverything();
                            return;
                        }
                        if (r >= 1 && r <= files.size())
                            safe->proc.loadPatch(files[r - 1]);
                    });
}

namespace rysum::ui
{
/** "Konzert": play for listeners on other computers, or listen to a player. Each listener renders
    the organ themselves (their own samples), from where they walk in the 3D Saal. */
class ConcertPanel : public juce::Component, private juce::Timer
{
public:
    explicit ConcertPanel(RysumProcessor &p) : proc(p)
    {
        auto &link = proc.concert;
        playButton.setTooltip(rysum::tr("Spielen: andere verbinden sich mit diesem Computer und h\xc3\xb6ren zu"));
        playButton.onClick = [this]
        {
            if (!proc.concert.startPerformer())
                refresh();
            refresh();
        };
        listenButton.setTooltip(rysum::tr("Bei einem Spieler zuh\xc3\xb6ren (seine Adresse aus seinem Konzert-Fenster)"));
        listenButton.onClick = [this]
        {
            lastHost() = hostField.getText().trim();
            if (lastHost().isNotEmpty())
                proc.concert.startListener(lastHost());
            refresh();
        };
        stopButton.onClick = [this]
        {
            proc.concert.stop();
            refresh();
        };
        hostField.setText(lastHost(), false);
        hostField.setTextToShowWhenEmpty(rysum::tr("Adresse des Spielers, z. B. 192.168.1.20"), col::faint);
        hostField.setFont(Assets::get().monoFont(12.0f));
        hostField.onReturnKey = [this] { listenButton.triggerClick(); };
        delayRow.format = [](double v) { return juce::String(juce::roundToInt(v)) + " ms"; };
        delayRow.slider.setValue(link.getDelayMs(), juce::dontSendNotification);
        delayRow.onChange = [this](double v) { proc.concert.setDelayMs(v); };
        delayRow.slider.setTooltip(rysum::tr("Wie weit der Zuh\xc3\xb6rer hinter dem Spieler liegt: Platz f\xc3\xbcr ein "
                                      "ungleichm\xc3\xa4\xc3\x9figes Netz. Mehr = sicherer, weniger = n\xc3\xa4her dran."));
        for (juce::Component *c : {(juce::Component *)&playButton, (juce::Component *)&listenButton,
                                   (juce::Component *)&stopButton, (juce::Component *)&hostField,
                                   (juce::Component *)&delayRow})
            addAndMakeVisible(c);
        handsBox.addItem(rysum::tr("H\xc3\xa4nde der Zuh\xc3\xb6rer: keine"), 1);
        handsBox.addItem(rysum::tr("H\xc3\xa4nde: anklopfen, Druckwelle, klatschen"), 2);
        handsBox.addItem(rysum::tr("H\xc3\xa4nde: alles \xe2\x80\x93 auch tragen, werfen, zur\xc3\xbc" "ckrufen"), 3);
        handsBox.setSelectedId(link.getGuestHands() + 1, juce::dontSendNotification);
        handsBox.onChange = [this] { proc.concert.setGuestHands(handsBox.getSelectedId() - 1); };
        handsBox.setTooltip(rysum::tr("Was die Zuh\xc3\xb6rer im 3D-Saal mit den Pfeifen tun d\xc3\xbc" "rfen. Was sie tun, geschieht bei dir "
                               "und alle sehen und h\xc3\xb6ren es."));
        addAndMakeVisible(handsBox);
        setSize(460, 304);
        refresh();
        startTimerHz(4);
    }

    void paint(juce::Graphics &g) override
    {
        auto &A = Assets::get();
        g.fillAll(col::bg);
        auto r = getLocalBounds().reduced(16, 10);
        const auto role = proc.concert.role();
        drawSectionTitle(g, r.removeFromTop(34), rysum::tr("Konzert"), proc.concert.status());
        g.setFont(A.monoFont(10.5f));
        g.setColour(col::dim);
        g.drawFittedText(rysum::tr("Einer spielt, die anderen h\xc3\xb6ren zu \xe2\x80\x93 jeder auf seinem Computer, mit "
                            "dem Plugin und den Samples, und h\xc3\xb6rt die Orgel von dort, wo er im 3D-Saal "
                            "steht. Es reist kein Klang, nur das Spiel."),
                         r.removeFromTop(44), juce::Justification::topLeft, 3);
        if (role == rysum::ConcertLink::Role::performer)
        {
            auto addr = rysum::ConcertLink::localAddresses().joinIntoString(rysum::tr("  oder  "));
            g.setFont(A.monoFont(12.0f));
            g.setColour(col::ink);
            g.drawFittedText(rysum::tr("Zuh\xc3\xb6rer verbinden sich mit:  ") + addr + "   (Port " +
                                 juce::String(rysum::ConcertLink::kDefaultPort) + ")",
                             addrBox, juce::Justification::centredLeft, 2);
        }
        g.setFont(A.monoFont(9.5f));
        g.setColour(col::faint);
        g.drawFittedText(rysum::tr("Im Netz zu Hause. Nicht mitgespielt werden: Tonnetz-Wanderer, "
                            "Selbstspiel, Publikum und eigene Raummodelle. Zuh\xc3\xb6rer brauchen nur die App \xe2\x80\x9eRysum Konzert\xe2\x80\x9c."),
                         getLocalBounds().reduced(16, 8).removeFromBottom(26), juce::Justification::bottomLeft, 2);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(16, 10).withTrimmedTop(34 + 46);
        auto row1 = r.removeFromTop(26);
        playButton.setBounds(row1.removeFromLeft(120));
        row1.removeFromLeft(10);
        addrBox = row1;
        r.removeFromTop(10);
        auto row2 = r.removeFromTop(26);
        listenButton.setBounds(row2.removeFromLeft(120));
        row2.removeFromLeft(10);
        hostField.setBounds(row2);
        r.removeFromTop(8);
        auto row3 = r.removeFromTop(32);
        stopButton.setBounds(row3.removeFromRight(100).withTrimmedTop(8));
        row3.removeFromRight(16);
        delayRow.setBounds(row3);
        r.removeFromTop(8);
        handsBox.setBounds(r.removeFromTop(26).withWidth(330));
    }

private:
    static juce::String &lastHost()
    {
        static juce::String h;
        return h;
    }
    void refresh()
    {
        const auto role = proc.concert.role();
        playButton.setToggleState(role == rysum::ConcertLink::Role::performer, juce::dontSendNotification);
        listenButton.setToggleState(role == rysum::ConcertLink::Role::listener, juce::dontSendNotification);
        playButton.setEnabled(role == rysum::ConcertLink::Role::off);
        listenButton.setEnabled(role == rysum::ConcertLink::Role::off);
        hostField.setEnabled(role == rysum::ConcertLink::Role::off);
        stopButton.setEnabled(role != rysum::ConcertLink::Role::off);
        delayRow.setEnabled(role != rysum::ConcertLink::Role::performer);
        repaint();
    }
    void timerCallback() override
    {
        refresh();
    }
    RysumProcessor &proc;
    juce::TextButton playButton{"Spielen"}, listenButton{u8("Zuh\xc3\xb6ren bei")}, stopButton{"Beenden"};
    juce::TextEditor hostField;
    juce::ComboBox handsBox;
    FreeRow delayRow{u8("Verz\xc3\xb6gerung (Zuh\xc3\xb6rer)"), 60.0, 1000.0, 250.0};
    juce::Rectangle<int> addrBox;
};
} // namespace rysum::ui

std::unique_ptr<juce::Component> rysum::ui::makeConcertPanel(RysumProcessor &p)
{
    return std::make_unique<ConcertPanel>(p);
}

void RysumContent::showConcertPanel()
{
    auto panel = rysum::ui::makeConcertPanel(proc);
    juce::CallOutBox::launchAsynchronously(std::move(panel), patchName.getScreenBounds(), nullptr);
}

void RysumContent::askResetEverything()
{
    auto opts = juce::MessageBoxOptions()
                    .withTitle(rysum::tr("Alles zur\xc3\xbc"
                                  "cksetzen"))
                    .withMessage(rysum::tr("Alles wie beim ersten Start: alle Regler, der Raum, das 3D-Modell "
                                    "und wo du im Saal stehst, Laternen, Aufstellungen. Ungespeicherte "
                                    "\xc3\x84nderungen gehen verloren (Patches bleiben, der Samples-Ordner "
                                    "auch)."))
                    .withButton(rysum::tr("Zur\xc3\xbc"
                                   "cksetzen"))
                    .withButton(rysum::tr("Abbrechen"))
                    .withAssociatedComponent(this);
    juce::Component::SafePointer<RysumContent> safe(this);
    juce::AlertWindow::showAsync(opts,
                                 [safe](int r)
                                 {
                                     if (safe == nullptr || r != 1)
                                         return;
                                     safe->walk.resetView();
                                     safe->proc.resetEverything();
                                 });
}

void RysumContent::stepPatch(int delta)
{
    const auto files = RysumProcessor::getPatchFiles();
    if (files.isEmpty())
        return;
    int cur = -1;
    for (int i = 0; i < files.size(); ++i)
        if (files[i].getFileNameWithoutExtension() == proc.getPatchName())
            cur = i;
    const int n = files.size();
    const int next = cur < 0 ? (delta > 0 ? 0 : n - 1) : ((cur + delta) % n + n) % n;
    proc.loadPatch(files[next]);
}

void RysumContent::askSavePatch()
{
    auto *w = new juce::AlertWindow(rysum::tr("Patch speichern"),
                                    rysum::tr("Name des Patches. Gespeichert wird alles au\xc3\x9f"
                                       "er dem Ausgabeformat und dem Samples-Ordner."),
                                    juce::MessageBoxIconType::NoIcon, this);
    w->setLookAndFeel(&getLookAndFeel());
    w->addTextEditor("name", proc.getPatchName().isEmpty() ? rysum::tr("Mein Patch")
                                                           : proc.getPatchName());
    w->addButton(rysum::tr("Speichern"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton(rysum::tr("Abbrechen"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<RysumContent> safe(this);
    w->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safe, w](int result)
            {
                if (safe == nullptr || result != 1)
                    return;
                const auto name = RysumProcessor::cleanPatchName(w->getTextEditorContents("name"));
                if (name.isEmpty())
                    return;
                auto doSave = [safe, name]
                {
                    if (safe == nullptr)
                        return;
                    if (!safe->proc.savePatch(name))
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::MessageBoxIconType::WarningIcon, rysum::tr("Patch speichern"),
                            rysum::tr("Konnte nicht gespeichert werden:\n") +
                                RysumProcessor::getPatchFolder().getFullPathName());
                };
                const bool exists =
                    RysumProcessor::getPatchFolder().getChildFile(name + ".rysum").existsAsFile();
                if (exists && name != safe->proc.getPatchName())
                {
                    auto opts = juce::MessageBoxOptions()
                                    .withTitle(rysum::tr("Patch speichern"))
                                    .withMessage("\"" + name + rysum::tr("\" gibt es schon. Ersetzen?"))
                                    .withButton(rysum::tr("Ersetzen"))
                                    .withButton(rysum::tr("Abbrechen"))
                                    .withAssociatedComponent(safe.getComponent());
                    juce::AlertWindow::showAsync(opts,
                                                 [doSave](int r)
                                                 {
                                                     if (r == 1)
                                                         doSave();
                                                 });
                }
                else
                    doSave();
            }),
        true);
}

void RysumContent::paint(juce::Graphics &g)
{
    auto &A = Assets::get();
    if (stats.on)
        stats.paintStart = juce::Time::getMillisecondCounterHiRes();
    g.fillAll(col::bg);
    auto head = juce::Rectangle<int>(0, 0, kW, kHeader).reduced(28, 0);
    g.setColour(col::border);
    g.fillRect(0, kHeader - 4, kW, 1);
    g.setColour(col::line);
    g.fillRect(0, kHeader - 1, kW, 1);

    {
        // "Rysum" in blackletter, the R rubricated
        const auto f = A.frakturFont(34.0f);
        const int rw = juce::roundToInt(juce::GlyphArrangement::getStringWidth(f, "R"));
        g.setFont(f);
        g.setColour(col::rubric.brighter(0.2f));
        g.drawText("R", head.withWidth(rw + 4).translated(0, 2), juce::Justification::centredLeft);
        g.setColour(col::ink);
        g.drawText("ysum", head.withTrimmedLeft(rw).withWidth(110).translated(0, 2),
                   juce::Justification::centredLeft);
        if (current == 6 || logoButton.isOver())
        {
            g.setColour(current == 6 ? col::rubric : col::border);
            g.fillRect(head.getX(), kHeader - 12, 96, 2);
        }
    }
    g.setFont(A.monoFont(11.0f));

    const int out = (int)proc.apvts.getRawParameterValue("output")->load();
    const bool tr = out == RysumProcessor::kTransauralOutput;
    const bool bin = out == RysumProcessor::kBinauralOutput || tr,
               spk = out == RysumProcessor::kSpeakerOutput;
    auto dec = proc.getSpeakerDecoder();
    const int spkCh = dec != nullptr ? dec->numOutChannels : 0;
    const int needed = out == 0 || bin ? 2 : spk ? spkCh : (out + 1) * (out + 1);
    const int have = proc.getTotalNumOutputChannels();
    juce::String o = bin ? (proc.binauralReady() ? juce::String(tr ? rysum::tr("2 LS") : "2 ch HRTF")
                                                 : juce::String::fromUTF8("HRTF \xe2\x80\xa6"))
                     : spk
                         ? (dec != nullptr && dec->error.isEmpty() ? juce::String(spkCh) + rysum::tr(" ch LS")
                                                                   : rysum::tr("LS: Fehler"))
                         : juce::String(needed) + " ch";
    if (have < needed)
        o = rysum::tr("nur ") + juce::String(have) + " ch!";
    g.setColour(have < needed ? col::white : col::faint);
    g.setFont(A.monoFont(10.0f));
    const juce::Rectangle<int> info(kOutX + kOutW + 2, 0, kFullX - 5 - (kOutX + kOutW + 2),
                                    kHeader);
    g.drawText(o, info.withTrimmedBottom(kHeader / 2).withTrimmedTop(9),
               juce::Justification::centredRight);
    g.setColour(col::faint);
    g.drawText(juce::String(proc.engine.activeVoices.load()) + rysum::tr(" St."),
               info.withTrimmedTop(kHeader / 2).withTrimmedBottom(9),
               juce::Justification::centredRight);
}

void RysumContent::showDiceMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader(rysum::tr("W\xc3\xbcrfeln"));
    m.addItem(1, rysum::tr("Sanft \xe2\x80\x93 kleine Schritte vom jetzigen Klang"));
    m.addItem(2, rysum::tr("Wild \xe2\x80\x93 alles neu (aber spielbar)"));
    m.addItem(3,
              rysum::tr("Zur\xc3\xbc"
                                     "ck zum Klang davor"),
              proc.canUndoDice());
    m.addSeparator();
    m.addSectionHeader(rysum::tr("Was gew\xc3\xbcrfelt wird"));
    const auto names = RysumProcessor::diceGroupNames();
    for (int g = 0; g < RysumProcessor::kNumDiceGroups; ++g)
        m.addItem(10 + g, names[g], true, proc.isDiceGroupOn(g));
    m.addSeparator();
    m.addItem(4,
              rysum::tr("Nie: Lautst\xc3\xa4rke, Ausgabe, Stimmung, MIDI, Freeze, Anordnung der Pfeifen"),
              false);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&diceButton),
                    [safe = juce::Component::SafePointer<RysumContent>(this)](int r)
                    {
                        if (safe == nullptr || r == 0)
                            return;
                        auto &pr = safe->proc;
                        if (r == 1 || r == 2)
                            pr.dice(r == 2);
                        else if (r == 3)
                            pr.undoDice();
                        else if (r >= 10)
                        {
                            pr.setDiceGroupOn(r - 10, !pr.isDiceGroupOn(r - 10));
                            safe->showDiceMenu(); // keep choosing
                        }
                        safe->repaint();
                    });
}

void RysumContent::showModMenu(juce::Component &target, const juce::String &paramId)
{
    const int idx = proc.getParamIndex(paramId);
    auto *prm = proc.getParamAt(idx);
    if (prm == nullptr)
        return;
    juce::PopupMenu m;
    m.addSectionHeader(rysum::trText(prm->getName(64)));
    const auto names = modSourceNames();
    if (!proc.isModTarget(idx))
        m.addItem(-1, rysum::tr("Schalter/Auswahl \xe2\x80\x93 nicht modulierbar"), false);
    else
    {
        bool free = false;
        for (int r = 0; r < kNumRoutes; ++r)
            free = free || (proc.getRouteSrc(r) < 0 && proc.getRouteTgt(r) < 0);
        juce::PopupMenu with;
        for (int s = 0; s < kNumSources; ++s)
            with.addItem(1 + s, names[s]);
        m.addSubMenu(free ? rysum::tr("Modulieren mit")
                          : rysum::tr("Modulieren mit (alle ") + juce::String(kNumRoutes) +
                                rysum::tr(" Pl\xc3\xa4tze belegt)"),
                     with, free);
        for (int r = 0; r < kNumRoutes; ++r)
            if (proc.getRouteTgt(r) == kGlobalTargetBase + idx && proc.getRouteSrc(r) >= 0)
            {
                auto *a = proc.apvts.getParameter(proc.getModAmountId(r));
                m.addItem(100 + r,
                          rysum::tr("Entfernen: ") + names[proc.getRouteSrc(r)] + " (" +
                              (a != nullptr ? a->getCurrentValueAsText() : juce::String()) + ")");
            }
    }
    // MIDI learn
    m.addSeparator();
    m.addItem(400, rysum::tr("MIDI lernen (Regler am Keyboard bewegen)"));
    for (int slot : proc.midiMapsFor(idx))
    {
        const auto mm = proc.getMidiMap(slot);
        m.addItem(500 + slot,
                  rysum::tr("MIDI-Zuordnung entfernen: ") +
                      (mm.cc >= 0 ? "CC " + juce::String(mm.cc) : rysum::tr("noch nicht gelernt")));
    }
    // Verlauf: a lane for this control
    m.addSeparator();
    if (proc.isAutoTarget(idx))
    {
        bool has = false;
        for (auto &l : proc.getAuto().lanes)
            has = has || l.paramId == paramId;
        m.addItem(600, has ? rysum::tr("Im Verlauf zeigen") : rysum::tr("Im Verlauf automatisieren"));
    }
    m.addItem(300, rysum::tr("Modulationsseite \xc3\xb6"
                             "ffnen"));
    juce::Component::SafePointer<RysumContent> safe(this);
    m.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&target),
        [safe, idx](int res)
        {
            if (safe == nullptr || res <= 0)
                return;
            auto &self = *safe;
            if (res >= 1 && res <= kNumSources)
                self.proc.addRoute(res - 1, kGlobalTargetBase + idx, 0.25f);
            else if (res >= 100 && res < 100 + kNumRoutes)
                self.proc.clearRoute(res - 100);
            else if (res == 300)
            {
                self.mod.showMidiTab(false);
                self.showView(5);
            }
            else if (res == 600)
            {
                if (auto *prm = self.proc.getParamAt(idx))
                    self.verlauf.addLaneFor(prm->getParameterID());
                self.showView(9);
            }
            else if (res == 400)
            {
                const int slot = self.proc.learnMidiFor(idx);
                if (slot < 0)
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::NoIcon, rysum::tr("MIDI lernen"),
                        rysum::tr("Alle ") + juce::String(RysumProcessor::kNumMidiMaps) +
                            rysum::tr(" MIDI-Zuordnungen sind belegt."));
                else
                    self.mod.showMidiTab(
                        true,
                        slot); // the page switches only when the user opens it; the row waits there
            }
            else if (res >= 500 && res < 500 + RysumProcessor::kNumMidiMaps)
                self.proc.clearMidiMap(res - 500);
        });
}

void RysumContent::paintOverChildren(juce::Graphics &g)
{
    ++paintsDone;
    if (!(dbg::skip & dbg::skipOverlay))
        g.drawImageAt(vignette, 0,
                      0); // darkened edges and the grain, blended once in the constructor
    if (stats.on)
    {
        // the time from paint() to here covers every child painted in this repaint
        const double ms = juce::Time::getMillisecondCounterHiRes() - stats.paintStart;
        ++stats.paints;
        stats.paintMs += ms;
        stats.paintMax = juce::jmax(stats.paintMax, ms);
        const auto cb = g.getClipBounds();
        stats.area += (double)cb.getWidth() * (double)cb.getHeight() / ((double)kW * (double)kH);
        auto box = juce::Rectangle<int>(kW - 430, kHeader + 4, 424, 54);
        g.setColour(juce::Colours::black.withAlpha(0.8f));
        g.fillRect(box);
        g.setColour(juce::Colours::white);
        g.setFont(Assets::get().monoFont(11.0f));
        g.drawFittedText(stats.shown, box.reduced(6, 3), juce::Justification::topLeft, 3);
    }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// performance readout (Ctrl+Shift+P) and the renderer switch (Ctrl+Shift+R)
static juce::File paintStatsFile()
{
    return RysumProcessor::getPatchFolder().getParentDirectory().getChildFile("paint-stats.txt");
}

void RysumContent::updateStats()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (stats.lastTick > 0.0)
        stats.tickMax = juce::jmax(stats.tickMax, now - stats.lastTick);
    stats.lastTick = now;
    ++stats.ticks;
    if (now - stats.secondStart < 1000.0)
        return;
    const double secs = (now - stats.secondStart) * 0.001;
    juce::String engine = "?";
    float phys = 1.0f;
    if (auto *peer = getPeer())
    {
        const auto names = peer->getAvailableRenderingEngines();
        const int cur = peer->getCurrentRenderingEngine();
        if (cur >= 0 && cur < names.size())
            engine = names[cur];
        phys = (float)peer->getPlatformScaleFactor();
    }
    const auto t = getTransform();
    const float scale = std::sqrt(std::abs(t.getDeterminant())) * phys;
    const char *views[]{"Spieltisch", "Raum",  "Saal",         "Klang",  "Effekte",
                        "Modulation", "Orgel", "Lautsprecher", "Begehen"};
    stats.shown = juce::String(stats.paints / secs, 1) + " Bilder/s   Malen " +
                  juce::String(stats.paints > 0 ? stats.paintMs / stats.paints : 0.0, 1) +
                  " ms (max " + juce::String(stats.paintMax, 1) + ")   neu gemalt " +
                  juce::String(100.0 * stats.area / secs, 0) + " % des Fensters pro s\n" + "Takt " +
                  juce::String(stats.ticks / secs, 1) + "/s (max Pause " +
                  juce::String(stats.tickMax, 0) + " ms)   " + engine + "   Pixel x" +
                  juce::String(scale, 2) + "   " + (fullButton.active ? rysum::tr("Vollbild") : "Fenster") +
                  "\n" + "Seite " + views[juce::jlimit(0, 8, current)] +
                  "   Strg+Umschalt+R: Renderer wechseln";
    {
        // the audio: how much of its time a block takes (the worst one since the last line), on
        // how many cores
        const int choice = juce::roundToInt(proc.apvts.getRawParameterValue("audioThreads")->load());
        const int cores = juce::jmin(proc.engine.audioThreadsAvailable(),
                                     choice > 0 ? choice : rysum::AudioWorkers::wantedThreads());
        stats.shown << "\nTon " << juce::String(juce::roundToInt(100.0f * proc.cpuLoad.load())) << " % (max "
                    << juce::String(juce::roundToInt(100.0f * proc.cpuPeak.exchange(0.0f))) << " %)   Kerne "
                    << (choice > 0 ? juce::String(cores) : "automatisch (" + juce::String(cores) + ")")
                    << "   Strg+Umschalt+K: Kerne wechseln";
        // where the engine's time goes, in % of real time (measured only while this readout is on)
        const auto pf = proc.engine.prof;
        proc.engine.prof.fill(0.0);
        proc.engine.profOn = true;
        auto pc = [secs](double ms) { return juce::String(ms * 0.1 / secs, 1); };
        stats.shown << "\nVor den Pfeifen " << pc(pf[0]) << " % (Bewegung " << pc(pf[6]) << ", Orte " << pc(pf[7]) << ")   Pfeifen " << pc(pf[1])
                    << " %   Weiteres " << pc(pf[2]) << " %   Reflexionen " << pc(pf[3])
                    << " % (ihre Geometrie, alle Kerne zusammen: " << pc(pf[5]) << ")   Nachhall " << pc(pf[4]) << " %   Stimmen "
                    << proc.engine.activeVoices.load() << ", gespiegelt " << proc.engine.erActive.load()
                    << "   Block " << proc.getBlockSize();
    }
    if (current == 8) // the 3D view: what its picture costs
        stats.shown << "\n3D: " << juce::String(walk.frameMs(), 1) << " ms (Geometrie "
                    << juce::String(walk.geometryMs(), 1) << "), " << walk.renderStats() << ", "
                    << walk.renderSize() << "   Blick " << juce::String(proc.walkPose[2].load(), 1)
                    << "   Bild: " << walk.pictureInfo();
    walk.overText = stats.shown; // (shown by the view itself while the graphics card draws)
    paintStatsFile().appendText(juce::Time::getCurrentTime().toString(false, true, true, true) +
                                "  " + stats.shown.replace("\n", "  |  ") + "\n");
    stats.secondStart = now;
    stats.paints = 0;
    stats.ticks = 0;
    stats.paintMs = stats.paintMax = stats.area = stats.tickMax = 0.0;
    repaint(kW - 430, kHeader + 4, 424, 54);
}

void RysumContent::startStats()
{
    stats.on = true;
    stats.secondStart = juce::Time::getMillisecondCounterHiRes();
    stats.lastTick = 0.0;
    stats.paints = stats.ticks = 0;
    stats.paintMs = stats.paintMax = stats.area = stats.tickMax = 0.0;
    stats.shown = rysum::tr("Messe ...");
    paintStatsFile().appendText("---- " + juce::Time::getCurrentTime().toString(true, true) + "\n");
}

void RysumContent::cycleRenderer()
{
    if (auto *peer = getPeer())
    {
        const auto names = peer->getAvailableRenderingEngines();
        if (names.size() > 1)
        {
            peer->setCurrentRenderingEngine((peer->getCurrentRenderingEngine() + 1) % names.size());
            paintStatsFile().appendText(
                "---- renderer: " + names[peer->getCurrentRenderingEngine()] + "\n");
        }
    }
    getTopLevelComponent()->repaint();
}

// window size: the last size the user chose (Documents/Rysum/Fenster.xml), else as large as fits
// the screen
static juce::File windowSizeFile()
{
    return RysumProcessor::getPatchFolder().getParentDirectory().getChildFile("Fenster.xml");
}

static float loadWindowScale()
{
    if (auto xml = juce::parseXML(windowSizeFile()))
    {
        const double s = xml->getDoubleAttribute("scale", 0.0);
        if (s >= 0.5 && s <= 2.0)
            return (float)s;
    }
    // first start: about 9/10 of the main screen's usable area (on a full-HD screen ~1.35x, text
    // ~1.35x larger)
    float s = 1.0f;
    if (auto *d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        s = juce::jmin(0.92f * (float)d->userArea.getWidth() / (float)kW,
                       0.88f * (float)d->userArea.getHeight() / (float)kH);
    return juce::jlimit(1.0f, 1.6f, std::floor(s * 20.0f) / 20.0f);
}

static void saveWindowScale(float s)
{
    juce::XmlElement xml("RysumFenster");
    xml.setAttribute("scale", (double)s);
    windowSizeFile().getParentDirectory().createDirectory();
    xml.writeTo(windowSizeFile());
}

RysumEditor::RysumEditor(RysumProcessor &p)
    : AudioProcessorEditor(p), content(std::make_unique<RysumContent>(p))
{
    setLookAndFeel(&lnf.get());
    juce::LookAndFeel::setDefaultLookAndFeel(
        &lnf.get()); // right-click menus and dialogs have no parent: they use the default
    connectContent();
    addAndMakeVisible(*content);
    setResizable(true, true);
    setResizeLimits(kW / 2, kH / 2, kW * 2, kH * 2);
    if (auto *c = getConstrainer())
        c->setFixedAspectRatio((double)kW / (double)kH);
    const float s0 = loadWindowScale();
    setSize(juce::roundToInt(kW * s0), juce::roundToInt(kH * s0));
    sizeRestored = true;
    startTimer(0, 16); // 60 times a second: on fast screens 30 looked choppy (the paints are cheap:
                       // 0.5 - 3 ms)
    // (measuring: RYSUM_PAINT_STATS=1 switches the readout on at start, RYSUM_PAINT_STATS=fs also
    // goes full screen,
    //  RYSUM_PAINT_VIEW=n opens page n)
    const auto ps = juce::SystemStats::getEnvironmentVariable("RYSUM_PAINT_STATS", {});
    if (juce::SystemStats::getEnvironmentVariable("RYSUM_IMMERSIVE_TEST", {}).isNotEmpty())
        juce::Timer::callAfterDelay(1500,
                                    [safe = juce::Component::SafePointer<RysumEditor>(this)]
                                    {
                                        if (safe != nullptr)
                                            safe->content->runImmersiveTest();
                                    });
    if (ps.isNotEmpty())
    {
        content->startStats();
        const auto pv = juce::SystemStats::getEnvironmentVariable("RYSUM_PAINT_VIEW", {});
        if (pv.isNotEmpty())
            content->showView(pv.getIntValue());
        if (juce::SystemStats::getEnvironmentVariable("RYSUM_PAINT_CHORD", {}) == "heavy")
        {
            // (measuring) a full scene: every stop flying in the Saal, knocking, playing itself
            auto &pr = static_cast<RysumProcessor &>(processor);
            auto setP = [&pr](const juce::String &id, float plain)
            {
                if (auto *p = pr.apvts.getParameter(id))
                    p->setValueNotifyingHost(p->convertTo0to1(plain));
            };
            for (int s = 0; s < rysum::kNumStops; ++s)
            {
                setP(RysumProcessor::stopParamId(s), 1.0f);
                setP(RysumProcessor::motionParamId(s), (float)(8 + s % 4));
            }
            setP("simOn", 1.0f);
            setP("lvImpLevel", 0.7f);
            setP("lvSelfPlay", 0.7f);
            setP("lvWhisper", 1.0f);
            setP("master", -60.0f); // (silent: only the load matters)
            {
                // (and what else is set in this scene: everything that is not at its default)
                juce::String diff = "nicht Standard: ";
                for (auto *prm : pr.getParameters())
                    if (auto *rp = dynamic_cast<juce::RangedAudioParameter *>(prm))
                        if (std::abs(rp->getValue() - rp->getDefaultValue()) > 1.0e-4f)
                            diff << rp->getParameterID() << "=" << rp->getCurrentValueAsText() << "  ";
                paintStatsFile().appendText(diff + "\n");
            }
            for (int n : {41, 53, 57, 65, 69, 72, 76})
                pr.keyboardState.noteOn(1, n, 0.8f);
        }
        else if (juce::SystemStats::getEnvironmentVariable("RYSUM_PAINT_CHORD", {}).isNotEmpty())
            for (int n : {48, 55, 60, 64, 67})
                static_cast<RysumProcessor &>(processor).keyboardState.noteOn(
                    1, n, 0.8f); // a held chord, as when playing
        if (ps == "fs")
            juce::Timer::callAfterDelay(1500,
                                        [safe = juce::Component::SafePointer<RysumEditor>(this)]
                                        {
                                            if (safe != nullptr && safe->fullscreen == nullptr)
                                                safe->toggleFullscreen();
                                        });
    }
}

void RysumEditor::connectContent()
{
    content->setLookAndFeel(&lnf.get());
    // deferred: the click / key that asked for it is still being handled by a component that moves
    // (the safe pointer is made here, outside the nested lambda: MSVC reads 'this' there
    // differently)
    content->onToggleFullscreen = [safe = juce::Component::SafePointer<RysumEditor>(this)]
    {
        juce::MessageManager::callAsync(
            [safe]
            {
                if (safe != nullptr)
                    safe->toggleFullscreen();
            });
    };
    // the language switch builds the interface anew (texts are set as its parts are made); deferred:
    // the button that asked for it belongs to the content that goes
    content->onLanguageChanged = [safe = juce::Component::SafePointer<RysumEditor>(this)]
    {
        juce::MessageManager::callAsync(
            [safe]
            {
                if (safe != nullptr)
                    safe->rebuildContent();
            });
    };
}

void RysumEditor::rebuildContent()
{
    auto &pr = static_cast<RysumProcessor &>(processor);
    const int view = content->currentView();
    auto *parent = content->getParentComponent();
    const auto transform = content->getTransform();
    content->releaseComputerKeys();
    if (parent != nullptr)
        parent->removeChildComponent(content.get());
    content->setLookAndFeel(nullptr);
    content.reset(); // the old one first: the processor knows only one interface at a time
    content = std::make_unique<RysumContent>(pr);
    connectContent();
    if (parent != nullptr)
        parent->addAndMakeVisible(*content, 0);
    content->setTransform(transform);
    content->setBounds(0, 0, kW, kH);
    content->setFullscreenShown(fullscreen != nullptr);
    content->showView(view);
    content->grabKeyboardFocus();
    repaint();
}

RysumEditor::~RysumEditor()
{
    if (fullscreen != nullptr)
    {
        if (juce::Desktop::getInstance().getKioskModeComponent() == fullscreen.get())
            juce::Desktop::getInstance().setKioskModeComponent(nullptr, false);
        fullscreen->removeChildComponent(content.get());
        fullscreen.reset();
    }
    content->releaseComputerKeys();
    content->setLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
}

void RysumEditor::resized()
{
    if (fullscreen != nullptr)
        return; // the content is on the full-screen window
    const float s = (float)getWidth() / (float)kW;
    content->setTransform(juce::AffineTransform::scale(s));
    content->setBounds(0, 0, kW, kH);
    if (sizeRestored)
    {
        scaleToSave = s;
        startTimer(1, 1500);
    } // remembered once the resizing has stopped
}

void RysumEditor::paint(juce::Graphics &g)
{
    g.fillAll(col::bg);
    if (fullscreen == nullptr)
        return;
    // while the interface is on the full screen, the plugin window only says where it went
    auto &A = Assets::get();
    auto r = getLocalBounds().toFloat();
    g.setColour(col::ink);
    g.setFont(A.frakturFont(juce::jmax(18.0f, r.getHeight() * 0.08f)));
    g.drawText("Rysum", r.withTrimmedBottom(r.getHeight() * 0.2f), juce::Justification::centred);
    g.setColour(col::dim);
    g.setFont(A.sansFont(juce::jmax(11.0f, r.getHeight() * 0.025f)));
    g.drawText(rysum::tr("l\xc3\xa4uft im Vollbild \xe2\x80\x93 hier klicken oder Esc dr\xc3\xbc"
                   "cken, um zur\xc3\xbc"
                   "ckzukehren"),
               r.withTrimmedTop(r.getHeight() * 0.12f), juce::Justification::centred);
}

void RysumEditor::toggleFullscreen()
{
    if (fullscreen == nullptr)
    {
        // the screen the plugin window is on
        const auto *display =
            juce::Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds());
        if (display == nullptr)
            return;
        content->releaseComputerKeys();
        fullscreen = std::make_unique<FullscreenWindow>(*this);
        fullscreen->setLookAndFeel(&lnf.get());
        fullscreen->setName("Rysum");
        fullscreen->addAndMakeVisible(*content);
        fullscreen->addToDesktop(juce::ComponentPeer::windowAppearsOnTaskbar);
        fullscreen->setBounds(display->totalArea);
        fullscreen->setVisible(true);
        juce::Desktop::getInstance().setKioskModeComponent(fullscreen.get(), false);
        fullscreen->toFront(true);
        content->grabKeyboardFocus();
        content->setFullscreenShown(true);
    }
    else
    {
        content->releaseComputerKeys();
        if (juce::Desktop::getInstance().getKioskModeComponent() == fullscreen.get())
            juce::Desktop::getInstance().setKioskModeComponent(nullptr, false);
        auto old = std::move(
            fullscreen); // fullscreen == nullptr from here on (resized() places the content again)
        addAndMakeVisible(*content);
        resized();
        old.reset();
        content->setFullscreenShown(false);
        if (auto *top = getTopLevelComponent())
            top->toFront(true);
        content->grabKeyboardFocus();
    }
    repaint();
}

void RysumEditor::FullscreenWindow::resized()
{
    // the 1280 x 720 design as large as the screen allows, centred (black bars where the shape
    // differs)
    auto &c = *editor.content;
    const float s = juce::jmin((float)getWidth() / (float)kW, (float)getHeight() / (float)kH);
    const float ox = 0.5f * ((float)getWidth() - s * (float)kW),
                oy = 0.5f * ((float)getHeight() - s * (float)kH);
    c.setTransform(juce::AffineTransform::scale(s).translated(ox, oy));
    c.setBounds(0, 0, kW, kH);
}

bool RysumEditor::FullscreenWindow::keyPressed(const juce::KeyPress &k)
{
    if (k == juce::KeyPress::escapeKey || k.getKeyCode() == juce::KeyPress::F11Key)
    {
        juce::MessageManager::callAsync(
            [safe = juce::Component::SafePointer<RysumEditor>(&editor)]
            {
                if (safe != nullptr)
                    safe->toggleFullscreen();
            });
        return true;
    }
    return false;
}

void RysumEditor::timerCallback(int id)
{
    if (id == 0)
    {
        content->tick();
        return;
    }
    stopTimer(1);
    if (std::abs(scaleToSave - savedScale) > 0.004f)
    {
        saveWindowScale(scaleToSave);
        savedScale = scaleToSave;
    }
}
