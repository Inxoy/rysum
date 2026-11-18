#include "ConsoleView.h"

namespace rysum::ui
{
// ---------------------------------------------------------------------------
void StopTile::paintButton(juce::Graphics &g, bool over, bool)
{
    auto &A = Assets::get();
    const bool on = getToggleState();
    auto r = getLocalBounds().toFloat();
    auto tile = r.removeFromTop(r.getHeight() - 22.0f).reduced(3.0f);
    auto caption = r;

    g.setColour(juce::Colour(0xff5f5e5b));
    g.fillRect(tile);

    if (A.knobs[(size_t)stop].isValid())
    {
        drawCover(g, A.knobs[(size_t)stop], tile);
    }
    else
    {
        // Praestant: no photo of the label – typeset label + bare knob photo
        auto t = tile.reduced(8.0f, 10.0f);
        auto label = t.removeFromTop(34.0f);
        const auto txt = prettyLabel(kStops[(size_t)stop].label);
        auto f = A.frakturFont(25.0f);
        const float w = juce::GlyphArrangement::getStringWidth(f, txt) + 22.0f;
        auto lb = label.withSizeKeepingCentre(juce::jmin(w, label.getWidth()), 32.0f);
        g.setColour(juce::Colour(0xffe2e0da));
        g.fillRect(lb);
        g.setColour(juce::Colour(0xff151514));
        g.setFont(f);
        g.drawText(txt, lb, juce::Justification::centred);
        auto knob = t.withSizeKeepingCentre(58.0f, 58.0f).translated(0.0f, 4.0f);
        juce::Path clip;
        clip.addEllipse(knob.reduced(2.0f));
        juce::Graphics::ScopedSaveState ss(g);
        if (!(dbg::skip & dbg::skipClipPath))
            g.reduceClipRegion(clip);
        drawCover(g, A.knobBare, knob.expanded(12.0f).translated(0.0f, -3.0f));
    }
    drawGrain(g, tile.toNearestInt(), 0.35f);

    if (!on)
    {
        g.setColour(juce::Colours::black.withAlpha(over ? 0.5f : 0.64f));
        g.fillRect(tile);
    }
    else
    {
        g.setColour(col::ink);
        g.drawRect(tile.expanded(3.0f), 1.0f);
    }

    // caption
    auto cap = caption.withTrimmedTop(6.0f).reduced(3.0f, 0.0f);
    g.setFont(A.monoFont(11.0f));
    g.setColour(col::dim);
    // the Praestant is a lever without a label – name it in the caption
    const auto capText =
        (stop == 0 || stop == kPraestant16)
            ? prettyLabel(kStops[(size_t)stop].label)
            : juce::String(juce::CharPointer_UTF8(kStops[(size_t)stop].footage))
                  .replace("'", juce::String(juce::CharPointer_UTF8("\xe2\x80\xb2")));
    g.drawText(capText, cap, juce::Justification::centredLeft);
    auto stateR = cap.removeFromRight(80.0f);
    g.setColour(on ? col::ink : col::faint);
    g.drawText(on ? "gezogen" : "ab", stateR, juce::Justification::centredRight);
    auto sq = juce::Rectangle<float>(7.0f, 7.0f)
                  .withCentre({stateR.getRight() - (on ? 58.0f : 22.0f), stateR.getCentreY()});
    g.setColour(col::ink);
    if (on)
        g.fillRect(sq);
    else
        g.drawRect(sq, 1.0f);
}

// ---------------------------------------------------------------------------
void VoxTile::paintButton(juce::Graphics &g, bool over, bool)
{
    auto &A = Assets::get();
    const bool on = getToggleState();
    auto r = getLocalBounds().toFloat();
    auto tile = r.removeFromTop(r.getHeight() - 22.0f).reduced(3.0f);
    auto caption = r;
    g.setColour(juce::Colour(0xff5f5e5b));
    g.fillRect(tile);
    {
        // a typeset label above the bare knob, like the Praestant's
        auto t = tile.reduced(8.0f, 10.0f);
        auto label = t.removeFromTop(34.0f);
        const auto txt = juce::String("Vox humana");
        auto f = A.frakturFont(22.0f);
        const float w = juce::GlyphArrangement::getStringWidth(f, txt) + 16.0f;
        auto lb = label.withSizeKeepingCentre(juce::jmin(w, label.getWidth()), 32.0f);
        g.setColour(juce::Colour(0xffe2e0da));
        g.fillRect(lb);
        g.setColour(juce::Colour(0xff151514));
        g.setFont(f);
        g.drawFittedText(txt, lb.toNearestInt(), juce::Justification::centred, 1, 0.7f);
        auto knob = t.withSizeKeepingCentre(58.0f, 58.0f).translated(0.0f, 4.0f);
        juce::Path clip;
        clip.addEllipse(knob.reduced(2.0f));
        juce::Graphics::ScopedSaveState ss(g);
        if (!(dbg::skip & dbg::skipClipPath))
            g.reduceClipRegion(clip);
        drawCover(g, A.knobBare, knob.expanded(12.0f).translated(0.0f, -3.0f));
    }
    drawGrain(g, tile.toNearestInt(), 0.35f);
    if (!on)
    {
        g.setColour(juce::Colours::black.withAlpha(over ? 0.5f : 0.64f));
        g.fillRect(tile);
    }
    else
    {
        g.setColour(col::ink);
        g.drawRect(tile.expanded(3.0f), 1.0f);
    }
    auto cap = caption.withTrimmedTop(6.0f).reduced(3.0f, 0.0f);
    g.setFont(A.monoFont(11.0f));
    g.setColour(error.isEmpty() ? col::dim : col::faint);
    g.drawText(error.isEmpty() ? juce::String::fromUTF8("8\xe2\x80\xb2") : juce::String("kein Modell"), cap,
               juce::Justification::centredLeft);
    auto stateR = cap.removeFromRight(80.0f);
    g.setColour(on ? col::ink : col::faint);
    g.drawText(on ? "gezogen" : "ab", stateR, juce::Justification::centredRight);
    auto sq = juce::Rectangle<float>(7.0f, 7.0f).withCentre({stateR.getRight() - (on ? 58.0f : 22.0f), stateR.getCentreY()});
    g.setColour(col::ink);
    if (on)
        g.fillRect(sq);
    else
        g.drawRect(sq, 1.0f);
}

// ---------------------------------------------------------------------------
VoxPanel::VoxPanel(RysumProcessor &p) : proc(p)
{
    playsAtt = attachButton(proc.apvts, "inPlays", plays);
    followAtt = attachButton(proc.apvts, "voxFollow", follow);
    plays.setTooltip(juce::String::fromUTF8(
        "Was am Eingang erklingt (Stimme, Instrument, eine Aufnahme), wird erkannt und spielt die Orgel: "
        "alle gezogenen Register, die Vox humana gleitend mit der Tonh\xc3\xb6he, die anderen auf den "
        "n\xc3\xa4" "chsten Tasten. Der Eingang selbst muss daf\xc3\xbcr nicht an sein."));
    follow.setTooltip(juce::String::fromUTF8(
        "Die Klangfarbe der Vox humana folgt dem Eingang: ein heller Eingang zieht hellere Register. "
        "Die Regler unten verschieben sie dann von dort aus."));
    for (auto *b : {&plays, &follow})
        addAndMakeVisible(*b);
    const std::pair<const char *, const char *> spec[]{
        {"voxX", "Klangfarbe X"},  {"voxY", "Klangfarbe Y"},     {"voxBright", "Helligkeit"},
        {"voxNoise", "Hauch"},     {"voxSpread", "Breite"},      {"voxDyn", "Dynamik"},
        {"inThresh", "Erkennungsschwelle"}, {"inConfirm", "Best\xc3\xa4tigung"},
        {"inVoices", "Stimmen"},   {"inVelGain", "Anschlag: Empfindlichkeit"}, {"inVelRange", "Anschlag: Bereich"}};
    const char *tips[]{
        "Die Hauptrichtung, in der sich die Register der Rysumer Orgel unterscheiden (in "
        "Standardabweichungen): durch die Registrierungen gleiten.",
        "Die zweite Richtung.",
        "Spektraler Schwerpunkt: dunkler \xe2\x80\x93 heller.",
        "Rauschanteil: reiner Ton \xe2\x80\x93 Wind und Hauch.",
        "Wie breit das Spektrum ist.",
        "Wie stark jede Stimme ihrem Anschlag folgt: gespielte Tasten ihrer Anschlagst\xc3\xa4rke, erkannte T\xc3\xb6ne "
        "ihrem Pegel im Eingang (laufend). 0 = alle gleich laut, 100 % = \xc2\xb1" "15 dB.",
        "Wie sicher ein Ton erkannt sein muss, bevor er spielt: h\xc3\xb6her = weniger, sicherere T\xc3\xb6ne.",
        "Wie lange ein Ton zu h\xc3\xb6ren sein muss, bevor er spielt: l\xc3\xa4nger = ruhiger, sp\xc3\xa4ter.",
        "Wie viele T\xc3\xb6ne der Eingang h\xc3\xb6"
        "chstens zugleich spielt (1 = einstimmig).",
        "Jeder erkannte Ton bekommt einen Anschlag aus seinem Pegel im Eingang (die St\xc3\xa4rke seiner "
        "Obert\xc3\xb6ne) und, solange er klingt, seinen Pegel als Druck (polyphoner Aftertouch). Auf der Seite "
        "Modulation wirken \xe2\x80\x9e" "Anschlag\xe2\x80\x9c und \xe2\x80\x9e" "Druck\xe2\x80\x9c damit je Pfeife, "
        "z. B. auf Pfeife: Wind. Mehr = ein leiser Eingang spielt schon kr\xc3\xa4" "ftig.",
        "Wie viele dB vom leisesten bis zum st\xc3\xa4rksten Anschlag: kleiner = schon kleine Unterschiede "
        "im Eingang machen den ganzen Bereich aus."};
    int i = 0;
    for (auto &sp : spec)
    {
        rows.push_back(std::make_unique<ParamRow>(proc.apvts, sp.first, juce::String::fromUTF8(sp.second)));
        if (*tips[i] != 0)
            rows.back()->setTooltip(juce::String::fromUTF8(tips[i]));
        ++i;
        addAndMakeVisible(*rows.back());
    }
    setSize(380, 40 + 2 * 26 + 2 * 22 + (int)rows.size() * 30 + 30);
}

void VoxPanel::paint(juce::Graphics &g)
{
    auto &A = Assets::get();
    g.fillAll(col::panel);
    g.setColour(col::ink);
    g.setFont(A.romanFont(19.0f));
    g.drawText("Vox humana", getLocalBounds().removeFromTop(34).reduced(12, 0), juce::Justification::centredLeft);
    g.setFont(A.monoFont(10.0f));
    g.setColour(col::dim);
    static const char *names[]{"Klang", "Eingang"};
    for (size_t k = 0; k < heads.size(); ++k)
        g.drawText(names[k], heads[k], juce::Justification::bottomLeft);
    g.drawText(juce::String::fromUTF8("Ort, Lautst\xc3\xa4rke, Raum: Seite Raum \xc2\xb7 Vox humana"),
               getLocalBounds().removeFromBottom(24).reduced(12, 0), juce::Justification::centredLeft);
    if (proc.voxError.isNotEmpty())
    {
        g.setColour(col::ink);
        g.drawText(proc.voxError, getLocalBounds().removeFromTop(34).reduced(12, 0), juce::Justification::centredRight);
    }
}

void VoxPanel::resized()
{
    auto r = getLocalBounds().reduced(12, 0);
    r.removeFromTop(36);
    plays.setBounds(r.removeFromTop(26));
    follow.setBounds(r.removeFromTop(26));
    heads.clear();
    const int groups[]{6, 5};
    size_t ri = 0;
    for (int gsize : groups)
    {
        heads.push_back(r.removeFromTop(22));
        for (int k = 0; k < gsize && ri < rows.size(); ++k)
            rows[ri++]->setBounds(r.removeFromTop(30));
    }
}

// ---------------------------------------------------------------------------
void SceneButton::paintButton(juce::Graphics &g, bool over, bool)
{
    auto &A = Assets::get();
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    const bool stored = proc.isSceneStored(slot);
    if (current && stored)
    {
        g.setColour(col::ink);
        g.fillRect(r);
    }
    else
    {
        g.setColour(over ? col::line : col::panel);
        g.fillRect(r);
    }
    g.setColour(stored ? (current ? col::ink : col::dim) : col::border);
    g.drawRect(r, 1.0f);
    g.setColour(current && stored ? col::bg : (stored ? col::ink : col::faint));
    g.setFont(A.romanFont(16.0f));
    g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred);
    if (!stored)
    {
        g.setFont(A.monoFont(8.5f));
        g.setColour(col::faint);
        g.drawText("leer", getLocalBounds().removeFromBottom(12), juce::Justification::centred);
    }
}

void SceneButton::clicked(const juce::ModifierKeys &m)
{
    if (m.isPopupMenu())
        return;
    if (proc.isSceneStored(slot))
        proc.goToScene(slot);
    else
    {
        proc.storeScene(slot);
        proc.goToScene(slot);
    }
    if (auto *p = getParentComponent())
        p->repaint();
}

void SceneButton::mouseDown(const juce::MouseEvent &e)
{
    if (e.mods.isPopupMenu())
    {
        showMenu();
        return;
    }
    juce::Button::mouseDown(e);
}

void SceneButton::showMenu()
{
    juce::PopupMenu m;
    const auto name = getButtonText();
    m.addSectionHeader("Szene " + name);
    m.addItem(1, juce::String::fromUTF8("Aktuellen Klang hier speichern"));
    m.addItem(2, "Hierhin wechseln", proc.isSceneStored(slot));
    m.addItem(3, "Leeren", proc.isSceneStored(slot));
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                    [safe = juce::Component::SafePointer<SceneButton>(this)](int r)
                    {
                        if (safe == nullptr)
                            return;
                        auto &pr = safe->proc;
                        if (r == 1)
                            pr.storeScene(safe->slot);
                        if (r == 2)
                            pr.goToScene(safe->slot);
                        if (r == 3)
                            pr.clearScene(safe->slot);
                        if (auto *p = safe->getParentComponent())
                            p->repaint();
                    });
}

// ---------------------------------------------------------------------------
int KeyStrip::midiAt(juce::Point<int> p) const
{
    const bool extended = proc.usesExtendedRange();
    const int first = extended ? kExtendedLowestMidiNote : kPhysKeys.front();
    const int last = extended ? kExtendedHighestMidiNote : kPhysKeys.back();
    const int n = last - first + 1;
    const float cellWidth = static_cast<float>(getWidth() + 2) / static_cast<float>(n);
    return first + juce::jlimit(0, n - 1, static_cast<int>(static_cast<float>(p.x) / cellWidth));
}

void KeyStrip::play(int midi)
{
    if (midi == playing)
        return;
    if (playing >= 0)
        proc.keyboardState.noteOff(1, playing, 0.0f);
    playing = -1;
    if (keyIndexForMidi(midi) >= 0 || proc.apvts.getRawParameterValue("temperament")->load() > 0.5f)
    {
        proc.keyboardState.noteOn(1, midi, 0.8f);
        playing = midi;
    }
}

void KeyStrip::mouseDown(const juce::MouseEvent &e)
{
    play(midiAt(e.getPosition()));
}
void KeyStrip::mouseDrag(const juce::MouseEvent &e)
{
    if (getLocalBounds().contains(e.getPosition()))
        play(midiAt(e.getPosition()));
}
void KeyStrip::mouseUp(const juce::MouseEvent &)
{
    if (playing >= 0)
        proc.keyboardState.noteOff(1, playing, 0.0f);
    playing = -1;
}

void KeyStrip::paint(juce::Graphics &g)
{
    auto &A = Assets::get();
    const bool extended = proc.usesExtendedRange();
    const int first = extended ? kExtendedLowestMidiNote : kPhysKeys.front();
    const int last = extended ? kExtendedHighestMidiNote : kPhysKeys.back();
    const int n = last - first + 1;
    const float gap = 2.0f;
    const float w =
        (static_cast<float>(getWidth()) - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
    const float keyH = (float)getHeight() - 16.0f;
    const bool chromatic = proc.apvts.getRawParameterValue("temperament")->load() > 0.5f;
    uint64_t held = proc.engine.heldMask.load();
    if (const int nk = proc.engine.netKeyNow.load(); nk >= 0)
        held |= (uint64_t)1 << nk; // the Tonnetz walker's key
    for (int i = 0; i < n; ++i)
    {
        const int midi = first + i;
        const int k = keyIndexForMidi(midi);
        const bool black = juce::Array<int>{1, 3, 6, 8, 10}.contains(midi % 12);
        juce::Rectangle<float> r((float)i * (w + gap), 0.0f, w, black ? keyH * 0.64f : keyH);
        if (k < 0 && !chromatic)
        {
            g.setColour(col::border);
            const float dash[]{3.0f, 3.0f};
            juce::Path p;
            p.addRectangle(r.reduced(0.5f));
            juce::PathStrokeType(1.0f).createDashedStroke(p, p, dash, 2);
            g.fillPath(p);
            continue;
        }
        const bool on =
            chromatic ? proc.keyboardState.isNoteOnForChannels(0xffff, midi) : (held >> k) & 1;
        g.setColour(on ? col::white
                       : (black ? juce::Colour(0xff2b2b28) : juce::Colour(0xffbdbbb5)));
        g.fillRect(r);
    }
    g.setFont(A.monoFont(10.0f));
    g.setColour(col::mute);
    if (extended)
    {
        for (int midi = first; midi <= last; midi += 12)
        {
            const float x = static_cast<float>(midi - first) * (w + gap);
            g.drawText("C" + juce::String(midi / 12 - 1),
                       juce::Rectangle<float>(std::min(x, static_cast<float>(getWidth()) - 24.0f),
                                              keyH + 2.0f, 24.0f, 14.0f),
                       juce::Justification::centredLeft);
        }
        return;
    }
    const std::pair<int, const char *> labels[]{
        {36, "C"}, {48, "c"}, {60, "c\xe2\x80\xb2"}, {72, "c\xe2\x80\xb3"}, {81, "a\xe2\x80\xb3"}};
    for (auto &l : labels)
        g.drawText(
            juce::String(juce::CharPointer_UTF8(l.second)),
            juce::Rectangle<float>((float)(l.first - 36) * (w + gap), keyH + 2.0f, 40.0f, 14.0f),
            juce::Justification::centredLeft);
}

// ---------------------------------------------------------------------------
ConsoleView::ConsoleView(RysumProcessor &p) : proc(p), keys(p)
{
    for (int s = 0; s < kNumStops; ++s)
    {
        auto t = std::make_unique<StopTile>(s);
        addAndMakeVisible(*t);
        tileAttachments.push_back(
            rysum::ui::attachButton(
                proc.apvts, RysumProcessor::stopParamId(s), *t));
        tiles.push_back(std::move(t));

        // per stop: volume + wind (a half-drawn stop: quieter, flatter, unsteady; 0 = silent)
        stopVol.push_back(std::make_unique<ParamRow>(proc.apvts, RysumProcessor::volParamId(s),
                                                     u8("Lautst."), true));
        stopWind.push_back(
            std::make_unique<ParamRow>(proc.apvts, RysumProcessor::windParamId(s), "Wind", true));
        addAndMakeVisible(*stopVol.back());
        addAndMakeVisible(*stopWind.back());
    }

    // Vox humana: the neural stop
    voxTile.error = proc.voxError;
    addAndMakeVisible(voxTile);
    voxAtt = rysum::ui::attachButton(proc.apvts, "voxOn", voxTile);
    voxVol = std::make_unique<ParamRow>(proc.apvts, "voxVol", u8("Lautst."), true);
    voxWind = std::make_unique<ParamRow>(proc.apvts, "voxWind", "Wind", true);
    addAndMakeVisible(*voxVol);
    addAndMakeVisible(*voxWind);
    voxMore.setTooltip(juce::String::fromUTF8(
        "Einstellungen der Vox humana: Klangfarbe, Ort im Raum, und \xe2\x80\x9e" "Eingang spielt die Orgel\xe2\x80\x9c."));
    addAndMakeVisible(voxMore);
    voxMore.onClick = [this]
    {
        auto panel = std::make_unique<VoxPanel>(proc);
        // inside the editor (not on the desktop): so its rows find the modulation menu (right-click)
        auto *host = dynamic_cast<juce::Component *>(findParentComponentOfClass<ModUiAccess>());
        if (host != nullptr)
            juce::CallOutBox::launchAsynchronously(std::move(panel), host->getLocalArea(&voxMore, voxMore.getLocalBounds()),
                                                   host);
        else
            juce::CallOutBox::launchAsynchronously(std::move(panel), voxMore.getScreenBounds(), nullptr);
    };

    testToneButton.setClickingTogglesState(true);
    testToneButton.setTooltip("DSF-Referenzton. Stimmung und Frequenz auf der Seite Klang.");
    addAndMakeVisible(testToneButton);
    testToneAttachment = rysum::ui::attachButton(
        proc.apvts, "dsfOn", testToneButton);
    testToneLevel =
        std::make_unique<ParamRow>(proc.apvts, "dsfLevel", u8("Lautst\xc3\xa4rke"), true);
    testToneColour = std::make_unique<ParamRow>(proc.apvts, "dsfSpectrum", "Klangfarbe", true);
    addAndMakeVisible(*testToneLevel);
    addAndMakeVisible(*testToneColour);

    const std::pair<const char *, const char *> noise[]{
        {"click", "Tastenklick"},
        {"clickBlend", "Klick \xe2\x86\x92 ins Anschlagen"},
        {"releaseClick", "Loslassklick"},
        {"stopPull", "Registerzug"},
        {"wind", "Wind (global)"}};
    for (auto &n : noise)
    {
        noiseRows.push_back(std::make_unique<ParamRow>(
            proc.apvts, n.first, juce::String(juce::CharPointer_UTF8(n.second))));
        addAndMakeVisible(*noiseRows.back());
    }
    if (auto *choice =
            dynamic_cast<juce::AudioParameterChoice *>(proc.apvts.getParameter("noteSpread")))
        spreadChoice.addItemList(choice->choices, 1);
    spreadAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        proc.apvts, "noteSpread", spreadChoice);
    spreadChoice.setTooltip(juce::String::fromUTF8(
        "Jeder Ton nur auf einer Pfeife eines gezogenen Registers statt auf allen: der Reihe nach "
        "(1. Ton Praestant, 2. Ton Gedackt \xe2\x80\xa6), nach Lage im Akkord (tiefster Ton aufs "
        "erste Register links) "
        "oder zuf\xc3\xa4llig. Stumm geschaltete Pfeifen (Seite Raum) werden \xc3\xbc"
        "bersprungen."));
    addAndMakeVisible(spreadChoice);
    const std::pair<const char *, const char *> decay[]{
        {"degAmount", "Menge"},    {"degGrain", "K\xc3\xb6rnung"},  {"degBits", "Bittiefe"},
        {"degRate", "Abtastrate"}, {"degWobble", "Windschwankung"}, {"degDropouts", "Aussetzer"}};
    for (auto &n : decay)
    {
        decayRows.push_back(std::make_unique<ParamRow>(
            proc.apvts, n.first, juce::String(juce::CharPointer_UTF8(n.second))));
        addAndMakeVisible(*decayRows.back());
    }

    {
        // "Raumverfall": the spatial counterpart of Körnung, Bittiefe, Abtastrate
        const std::pair<const char *, const char *> space[]{
            {"spcErode", "Ordnung abtragen"}, {"spcWeather", "Verwittern (bis ganz abgetragen)"},
            {"spcGrid", "Richtungsraster"},   {"spcBits", "Raum-Bittiefe"},
            {"spcRate", "Raum-Abtastrate"},   {"spcDrops", "Raum-Aussetzer"},
            {"spcAlias", "Raum-Aliasing"}};
        const char *tips[]{
            "Die Kugel verliert Aufl\xc3\xb6sung: die h\xc3\xb6heren Ambisonics-Ordnungen "
            "verschwinden von oben her, stufenlos \xe2\x80\x93 bei 100 % klingt alles von \xc3\xbc"
            "berall (nur W).",
            "Die Kugel nutzt sich ab, solange etwas klingt: nach dieser Zeit ist sie ganz "
            "abgetragen. In der Stille w\xc3\xa4"
            "chst sie viermal so schnell nach. Aus = 0.",
            "Richtungen rasten auf wenige Punkte ein (bis zu 4 Richtungen): bewegte Pfeifen "
            "springen von Punkt zu Punkt. Wirkt auch in Stereo.",
            "Bittiefe der Richtungskan\xc3\xa4le: je h\xc3\xb6her die Ordnung, desto gr\xc3\xb6"
            "ber (W bleibt sauber) \xe2\x80\x93 die feinen Richtungen zerfallen zuerst.",
            "Abtastrate der Richtungskan\xc3\xa4le: je h\xc3\xb6her die Ordnung, desto tiefer.",
            "R\xc3\xa4umliche Aussetzer: einzelne Kan\xc3\xa4le oder ganze Ordnungen fallen "
            "f\xc3\xbcr 50\xe2\x80\x93"
            "400 ms aus \xe2\x80\x93 der Klang kippt kurz zur Seite oder wird flach.",
            "Das Schallfeld wird an zu wenigen Punkten abgetastet und neu kodiert: "
            "Geisterrichtungen, die langsam wandern."};
        int i = 0;
        for (auto &n : space)
        {
            spaceRows.push_back(std::make_unique<ParamRow>(
                proc.apvts, n.first, juce::String(juce::CharPointer_UTF8(n.second))));
            spaceRows.back()->setTooltip(juce::String::fromUTF8(tips[i++]));
            addChildComponent(*spaceRows.back());
        }
        // "Glitch": transients of a failing playback, per pipe
        const std::pair<const char *, const char *> glitch[]{{"degStutter", "Stottern"},
                                                             {"degSplice", "Bandschnitt (Loop)"},
                                                             {"degZero", "Harte Kanten"},
                                                             {"degClip", "Clipping"}};
        const char *gtips[]{
            "Ein kurzes St\xc3\xbc"
            "ck der Pfeife (20\xe2\x80\x93"
            "150 ms) springt zwei- bis sechsmal zur\xc3\xbc"
            "ck \xe2\x80\x93 wie ein h\xc3\xa4ngender Puffer.",
            "Die Loop-Punkte werden zu harten Bandschnitten: kein \xc3\x9c"
            "berblenden mehr, dazu ein Ticken und ein kurzes Wegsacken bei jedem Durchlauf.",
            "Keine R\xc3\xbc"
            "cksicht auf Nulldurchg\xc3\xa4nge: T\xc3\xb6ne beginnen irgendwo in ihren ersten "
            "Momenten und enden mit einem harten Schnitt.",
            "Kurze St\xc3\xb6\xc3\x9f"
            "e harten digitalen Clippings (8\xe2\x80\x93"
            "80 ms), mal mehr, mal weniger \xc3\xbc"
            "bersteuert."};
        int gi = 0;
        for (auto &n : glitch)
        {
            glitchRows.push_back(std::make_unique<ParamRow>(
                proc.apvts, n.first, juce::String(juce::CharPointer_UTF8(n.second))));
            glitchRows.back()->setTooltip(juce::String::fromUTF8(gtips[gi++]));
            addChildComponent(*glitchRows.back());
        }
        for (auto *b : {&tabSound, &tabSpace, &tabGlitch})
        {
            b->setClickingTogglesState(false);
            addAndMakeVisible(*b);
        }
        tabSound.onClick = [this] { showDecayTab(0); };
        tabSpace.onClick = [this] { showDecayTab(1); };
        tabGlitch.onClick = [this] { showDecayTab(2); };
        tabGlitch.setTooltip(
            juce::String::fromUTF8("Glitches einer versagenden Wiedergabe: Stottern, Bandschnitte, "
                                   "harte Kanten, Clipping (pro Pfeife)"));
        // Traktur · Resonanz: the key clicks ring the harmonics of the pressed key
        const std::pair<const char *, const char *> res[]{{"clickRes", "Klick-Resonanz"},
                                                          {"clickResDecay", "Nachklang"},
                                                          {"clickResBright", "Helligkeit"}};
        const char *rtips[]{
            "Die Tastenklicks regen Resonatoren an, gestimmt auf die Teilt\xc3\xb6ne der "
            "gedr\xc3\xbc"
            "ckten Taste (in der gew\xc3\xa4hlten Stimmung): kurze, glockige T\xc3\xb6ne.",
            "Wie lange die Resonanz nachklingt (die oberen Teilt\xc3\xb6ne k\xc3\xbcrzer).",
            "Wie stark die oberen Teilt\xc3\xb6ne mitklingen."};
        int ri = 0;
        for (auto &n : res)
        {
            resRows.push_back(std::make_unique<ParamRow>(
                proc.apvts, n.first, juce::String(juce::CharPointer_UTF8(n.second))));
            resRows.back()->setTooltip(juce::String::fromUTF8(rtips[ri++]));
            addChildComponent(*resRows.back());
        }
        for (auto *b : {&tabNoise, &tabRes})
        {
            b->setClickingTogglesState(false);
            addAndMakeVisible(*b);
        }
        tabNoise.onClick = [this] { showNoiseTab(false); };
        tabRes.onClick = [this] { showNoiseTab(true); };
        tabSound.setTooltip(juce::String::fromUTF8(
            "Verfall des Klangs: K\xc3\xb6rnung, Bittiefe, Abtastrate, Windschwankung, Aussetzer"));
        tabSpace.setTooltip(juce::String::fromUTF8(
            "Verfall des Raumbilds: die Ambisonics-Kugel nutzt sich ab (Ambisonics, Binaural, "
            "Lautsprecher; in Stereo nur das Richtungsraster)"));
    }
    for (auto *b : {&perPipe, &onSum})
    {
        addAndMakeVisible(*b);
        b->setClickingTogglesState(false);
    }
    perPipe.onClick = [this]
    {
        if (auto *pr = proc.apvts.getParameter("degMode"))
            pr->setValueNotifyingHost(0.0f);
    };
    onSum.onClick = [this]
    {
        if (auto *pr = proc.apvts.getParameter("degMode"))
            pr->setValueNotifyingHost(1.0f);
    };
    showDecayTab(0);
    showNoiseTab(false);

    volume.setSliderStyle(juce::Slider::LinearVertical);
    volume.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    volume.setDoubleClickReturnValue(true, -12.0);
    addAndMakeVisible(volume);
    volumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        proc.apvts, "master", volume);

    addAndMakeVisible(folderButton);
    folderButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser>("Ordner mit rysum_samples.json",
                                                      proc.getSamplesFolder());
        chooser->launchAsync(juce::FileBrowserComponent::openMode |
                                 juce::FileBrowserComponent::canSelectDirectories,
                             [this](const juce::FileChooser &fc)
                             {
                                 auto f = fc.getResult();
                                 if (f.isDirectory())
                                     proc.loadSamples(f);
                             });
    };

    addAndMakeVisible(keys);

    // scenes A..D and the control that blends between them
    for (int s = 0; s < RysumProcessor::kNumScenes; ++s)
    {
        sceneButtons.push_back(std::make_unique<SceneButton>(proc, s));
        sceneButtons.back()->setTooltip(
            juce::String::fromUTF8("Szene ") + sceneButtons.back()->getButtonText() +
            juce::String::fromUTF8(
                ": Klicken = dorthin wechseln (eine leere Szene speichert den aktuellen Klang). "
                "Rechtsklick = speichern / leeren."));
        addAndMakeVisible(*sceneButtons.back());
    }
    sceneMorph = std::make_unique<ParamRow>(proc.apvts, "sceneMorph",
                                            juce::String::fromUTF8("\xc3\x9c"
                                                                   "berblenden"),
                                            true);
    sceneMorph->setTooltip(juce::String::fromUTF8(
        "Blendet zwischen den gespeicherten Szenen (leere werden \xc3\xbc"
        "bersprungen): alle Regler gleiten, "
        "Schalter wechseln auf halbem Weg. Automatisierbar, MIDI-lernbar, modulierbar. "
        "Nicht dabei: Lautst\xc3\xa4rke, Ausgabe, Stimmung, MIDI."));
    addAndMakeVisible(*sceneMorph);
}

void ConsoleView::resized()
{
    auto r = getLocalBounds();
    keyboardArea = r.removeFromBottom(116);
    r.reduce(28, 0);
    r.removeFromTop(22);
    stopsArea = r.removeFromTop(228);
    {
        auto t = stopsArea.withTrimmedTop(26);
        const int gap = 12;
        const int columns = kNumStops + 2; // the stops, the Vox humana, the test tone
        const int w = (t.getWidth() - gap * (columns - 1)) / columns;
        for (int s = 0; s < kNumStops; ++s)
        {
            const int x = t.getX() + displaySlot(s) * (w + gap);
            tiles[(size_t)s]->setBounds(x - 3, t.getY() - 3, w + 6, 124 + 22 + 3);
            stopVol[(size_t)s]->setBounds(x, t.getY() + 150, w, 28);
            stopWind[(size_t)s]->setBounds(x, t.getY() + 177, w, 28);
        }
        {
            const int x = t.getX() + kNumStops * (w + gap);
            voxTile.setBounds(x - 3, t.getY() - 3, w + 6, 124 + 22 + 3);
            voxVol->setBounds(x, t.getY() + 150, w, 28);
            voxWind->setBounds(x, t.getY() + 177, w, 28);
            voxMore.setBounds(x + w - 44, t.getY() - 25, 44, 18);
        }
        const int testX = t.getX() + (kNumStops + 1) * (w + gap);
        testToneButton.setBounds(testX, t.getY() + 44, w, 38);
        testToneLevel->setBounds(testX, t.getY() + 150, w, 28);
        testToneColour->setBounds(testX, t.getY() + 177, w, 28);
    }
    r.removeFromTop(12);
    auto panels = r.withTrimmedBottom(14);
    const int gap = 28;
    const int total = panels.getWidth() - 2 * gap;
    noiseArea = panels.removeFromLeft(total / 3);
    panels.removeFromLeft(gap);
    decayArea = panels.removeFromLeft(total / 3);
    panels.removeFromLeft(gap);
    outArea = panels;

    auto rows = noiseArea.withTrimmedTop(40);
    for (auto &row : noiseRows)
    {
        row->setBounds(rows.removeFromTop(36));
    }
    {
        rows.removeFromTop(6);
        auto sr = rows.removeFromTop(24);
        spreadLabel = sr.removeFromLeft(118);
        spreadChoice.setBounds(sr);
    }
    rows = decayArea.withTrimmedTop(40);
    {
        auto r2 = rows;
        for (auto &row : decayRows)
        {
            row->setBounds(rows.removeFromTop(36));
        }
        auto r3 = r2;
        const int rh = juce::jmin(36, r2.getHeight() / (int)spaceRows.size());
        for (auto &row : spaceRows)
            row->setBounds(r2.removeFromTop(rh));
        for (auto &row : glitchRows)
            row->setBounds(r3.removeFromTop(36));
    }
    {
        auto r4 = noiseArea.withTrimmedTop(40);
        for (auto &row : resRows)
            row->setBounds(r4.removeFromTop(36));
    }
    tabNoise.setBounds(noiseArea.getX() + 104, noiseArea.getY() + 8, 76, 20);
    tabRes.setBounds(tabNoise.getRight() + 4, noiseArea.getY() + 8, 74, 20);
    // three tabs and the two mode buttons share one line: narrow enough that the mode buttons do
    // not lie over "Glitch"
    tabSound.setBounds(decayArea.getX() + 112, decayArea.getY() + 8, 50, 20);
    tabSpace.setBounds(tabSound.getRight() + 3, decayArea.getY() + 8, 46, 20);
    tabGlitch.setBounds(tabSpace.getRight() + 3, decayArea.getY() + 8, 52, 20);
    onSum.setBounds(decayArea.getRight() - 49, decayArea.getY() + 8, 49, 20);
    perPipe.setBounds(juce::jmax(tabGlitch.getRight() + 6, onSum.getX() - 66), decayArea.getY() + 8,
                      66, 20);

    auto o = outArea.withTrimmedTop(40);
    {
        auto top = o.removeFromTop(120);
        meterArea = top.removeFromLeft(110);
        top.removeFromLeft(16);
        outInfoArea = top;
    }
    volume.setBounds(meterArea.getRight() - 30, meterArea.getY(), 28, meterArea.getHeight());
    folderButton.setBounds(outInfoArea.getX(), outInfoArea.getBottom() - 22, outInfoArea.getWidth(),
                           22);
    o.removeFromTop(14);
    scenesArea = o.removeFromTop(18);
    {
        auto b = o.removeFromTop(30);
        const int bw = 44;
        for (auto &sb : sceneButtons)
        {
            sb->setBounds(b.removeFromLeft(bw));
            b.removeFromLeft(6);
        }
        o.removeFromTop(4);
        sceneMorph->setBounds(o.removeFromTop(30));
    }

    auto kb = keyboardArea;
    photoArea = kb.removeFromLeft(250);
    keys.setBounds(kb.reduced(20, 0).withTrimmedTop(34).withTrimmedBottom(10).withTrimmedLeft(0));
}

void ConsoleView::showNoiseTab(bool res)
{
    resTab = res;
    tabNoise.setToggleState(!res, juce::dontSendNotification);
    tabRes.setToggleState(res, juce::dontSendNotification);
    for (auto &r : noiseRows)
        r->setVisible(!res);
    for (auto &r : resRows)
        r->setVisible(res);
    spreadChoice.setVisible(!res);
    repaint(noiseArea);
}

void ConsoleView::showDecayTab(int tab)
{
    decayTab = tab;
    const bool space = tab == 1;
    spaceTab = space;
    for (auto &r : glitchRows)
        r->setVisible(tab == 2);
    tabGlitch.setToggleState(tab == 2, juce::dontSendNotification);
    tabSound.setToggleState(tab == 0, juce::dontSendNotification);
    tabSpace.setToggleState(space, juce::dontSendNotification);
    for (auto &r : decayRows)
        r->setVisible(tab == 0);
    for (auto &r : spaceRows)
        r->setVisible(space);
    perPipe.setVisible(tab == 0);
    onSum.setVisible(tab == 0);
    repaint(decayArea);
}

juce::String ConsoleView::spaceInfo() const
{
    const int out = juce::roundToInt(proc.apvts.getRawParameterValue("output")->load());
    if (out == 0)
        return juce::String::fromUTF8("Stereo: nur das Raster");
    const float e = proc.engine.spaceErosionNow.load();
    return e > 0.005f ? juce::String::fromUTF8("abgetragen ") +
                            juce::String(juce::roundToInt(e * 100.0f)) + " %"
                      : juce::String();
}

void ConsoleView::refresh()
{
    const bool sum = proc.apvts.getRawParameterValue("degMode")->load() > 0.5f;
    perPipe.setToggleState(!sum, juce::dontSendNotification);
    onSum.setToggleState(sum, juce::dontSendNotification);
    meterL = proc.engine.meterL.load();
    meterR = proc.engine.meterR.load();
    if (spaceTab)
    {
        const auto si = spaceInfo();
        if (si != shownSpaceInfo)
        {
            shownSpaceInfo = si;
            repaint(decayArea.withHeight(36));
        }
    }
    testToneLevel->repaint();
    testToneColour->repaint();
    keys.refresh();
    {
        const float x = proc.apvts.getRawParameterValue("sceneMorph")->load();
        if (proc.getSceneVersion() != shownSceneVersion || std::abs(x - shownMorph) > 0.001f)
        {
            shownSceneVersion = proc.getSceneVersion();
            shownMorph = x;
            for (int s = 0; s < (int)sceneButtons.size(); ++s)
            {
                sceneButtons[(size_t)s]->current = std::abs(x - (float)s) < 0.02f;
                sceneButtons[(size_t)s]->repaint();
            }
        }
    }
    repaint(outArea);
}

void ConsoleView::paint(juce::Graphics &g)
{
    auto &A = Assets::get();
    if (!(dbg::skip & dbg::skipTranslucent))
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.setOpacity(0.09f);
        drawCover(g, A.wall, getLocalBounds().toFloat(), 0.3f);
    }

    // stops header
    auto head = stopsArea.withHeight(20);
    g.setColour(col::mute);
    g.setFont(A.sansFont(11.0f).withExtraKerningFactor(0.18f));
    g.drawText("REGISTER", head, juce::Justification::centredLeft);
    int on = 0;
    for (int s = 0; s < kNumStops; ++s)
        on += proc.apvts.getRawParameterValue(RysumProcessor::stopParamId(s))->load() > 0.5f;
    on += proc.apvts.getRawParameterValue("voxOn")->load() > 0.5f; // the Vox humana
    g.setFont(A.monoFont(11.0f));
    g.drawText(juce::String(on) + " von " + juce::String(kNumStops + 1) + " gezogen", head,
               juce::Justification::centredRight);

    drawSectionTitle(g, noiseArea, "Traktur");
    g.setFont(Assets::get().sansFont(14.0f));
    g.setColour(col::ink);
    if (!resTab)
        g.drawText(juce::String::fromUTF8("T\xc3\xb6ne verteilen"), spreadLabel,
                   juce::Justification::centredLeft);
    drawSectionTitle(g, decayArea, "Verfall", spaceTab ? shownSpaceInfo : juce::String());
    drawSectionTitle(g, outArea, "Ausgang");

    // meters + volume scale
    auto m = meterArea;
    auto scale = m.removeFromLeft(30);
    g.setFont(A.monoFont(10.0f));
    g.setColour(col::faint);
    const char *labels[]{"+12", "-6", "-24", "-42", "-60"};
    for (int i = 0; i < 5; ++i)
        g.drawText(labels[i],
                   scale.withHeight(12).withY(scale.getY() + (i * (scale.getHeight() - 12) / 4)),
                   juce::Justification::centredLeft);
    auto meterBar = [&](juce::Rectangle<int> b, float v)
    {
        g.setColour(juce::Colour(0xff23231f));
        g.fillRect(b);
        const float db = juce::Decibels::gainToDecibels(v, -60.0f);
        const float frac = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 72.0f);
        g.setColour(db > 0.0f ? col::white : col::ink);
        g.fillRect(b.withTrimmedTop((int)((1.0f - frac) * (float)b.getHeight())));
    };
    meterBar({m.getX() + 4, m.getY(), 10, m.getHeight()}, meterL);
    meterBar({m.getX() + 20, m.getY(), 10, m.getHeight()}, meterR);

    auto info = outInfoArea;
    g.setFont(A.sansFont(14.0f));
    g.setColour(col::ink);
    g.drawText(juce::String(juce::CharPointer_UTF8("Lautst\xc3\xa4rke")), info.removeFromTop(18),
               juce::Justification::centredLeft);
    g.setFont(A.monoFont(12.0f));
    g.setColour(col::dim);
    g.drawText(proc.apvts.getParameter("master")->getCurrentValueAsText() + " dB",
               info.withY(info.getY() - 18).withHeight(18), juce::Justification::centredRight);
    g.setFont(A.monoFont(11.0f));
    g.setColour(col::mute);
    g.drawText(juce::String(proc.engine.activeVoices.load()) + " Stimmen aktiv",
               info.removeFromTop(18), juce::Justification::centredLeft);
    g.drawFittedText(proc.getLoadStatus(), info.removeFromTop(48), juce::Justification::topLeft, 3);
    {
        g.setColour(col::mute);
        g.setFont(A.sansFont(11.0f).withExtraKerningFactor(0.18f));
        g.drawText("SZENEN", scenesArea, juce::Justification::centredLeft);
        int n = 0;
        for (int s = 0; s < RysumProcessor::kNumScenes; ++s)
            n += proc.isSceneStored(s);
        g.setFont(A.monoFont(10.5f));
        g.drawText(n == 0 ? juce::String::fromUTF8("Klick auf einen Buchstaben speichert")
                          : juce::String(n) + " gespeichert",
                   scenesArea, juce::Justification::centredRight);
    }

    // keyboard band
    g.setColour(col::line);
    g.fillRect(keyboardArea.getX(), keyboardArea.getY(), keyboardArea.getWidth(), 1);
    {
        juce::Graphics::ScopedSaveState ss(g);
        drawCover(g, A.keys, photoArea.toFloat().withTrimmedTop(1), 0.52f);
    }
    auto kh =
        keyboardArea.withTrimmedLeft(270).withTrimmedRight(20).withTrimmedTop(12).withHeight(16);
    g.setFont(A.monoFont(11.0f));
    g.setColour(col::mute);
    const bool extended = proc.usesExtendedRange();
    const bool chromatic = proc.apvts.getRawParameterValue("temperament")->load() > 0.5f;
    auto compass = u8("C \xe2\x80\x93 a\xe2\x80\xb3 \xc2\xb7 kurze Oktave \xc2\xb7 41 Tasten");
    if (extended)
        compass = u8("C1 \xe2\x80\x93 C8 \xc2\xb7 chromatisch");
    else if (chromatic)
        compass = u8("C \xe2\x80\x93 a\xe2\x80\xb3 \xc2\xb7 chromatisch");
    g.drawText(compass, kh, juce::Justification::centredLeft);
    g.drawText(extended ? "MIDI 24-108" : "MIDI 36-81", kh, juce::Justification::centredRight);
}
} // namespace rysum::ui
