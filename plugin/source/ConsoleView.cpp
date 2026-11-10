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
    const int n = 46;
    const float w = (float)getWidth() / (float)n;
    return 36 + juce::jlimit(0, n - 1, (int)((float)p.x / w));
}

void KeyStrip::play(int midi)
{
    if (midi == playing)
        return;
    if (playing >= 0)
        proc.keyboardState.noteOff(1, playing, 0.0f);
    playing = -1;
    if (keyIndexForMidi(midi) >= 0)
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
    const int n = 46;
    const float gap = 2.0f;
    const float w = ((float)getWidth() - gap * (n - 1)) / (float)n;
    const float keyH = (float)getHeight() - 16.0f;
    const uint64_t held = proc.engine.heldMask.load();
    for (int i = 0; i < n; ++i)
    {
        const int midi = 36 + i;
        const int k = keyIndexForMidi(midi);
        const bool black = juce::Array<int>{1, 3, 6, 8, 10}.contains(midi % 12);
        juce::Rectangle<float> r((float)i * (w + gap), 0.0f, w, black ? keyH * 0.64f : keyH);
        if (k < 0)
        {
            g.setColour(col::border);
            const float dash[]{3.0f, 3.0f};
            juce::Path p;
            p.addRectangle(r.reduced(0.5f));
            juce::PathStrokeType(1.0f).createDashedStroke(p, p, dash, 2);
            g.fillPath(p);
            continue;
        }
        const bool on = (held >> k) & 1;
        g.setColour(on ? col::white
                       : (black ? juce::Colour(0xff2b2b28) : juce::Colour(0xffbdbbb5)));
        g.fillRect(r);
    }
    g.setFont(A.monoFont(10.0f));
    g.setColour(col::mute);
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
            std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
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

    testToneButton.setClickingTogglesState(true);
    testToneButton.setTooltip("DSF-Referenzton. Stimmung und Frequenz auf der Seite Klang.");
    addAndMakeVisible(testToneButton);
    testToneAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
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
        "(1. Ton Praestant, 2. Ton Gedackt â¦), nach Lage im Akkord (tiefster Ton aufs erste "
        "Register links) "
        "oder zufÃ¤llig. Stumm geschaltete Pfeifen (Seite Raum) werden Ã¼"
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
        const int columns = kNumStops + 1;
        const int w = (t.getWidth() - gap * (columns - 1)) / columns;
        for (int s = 0; s < kNumStops; ++s)
        {
            const int x = t.getX() + displaySlot(s) * (w + gap);
            tiles[(size_t)s]->setBounds(x - 3, t.getY() - 3, w + 6, 124 + 22 + 3);
            stopVol[(size_t)s]->setBounds(x, t.getY() + 150, w, 28);
            stopWind[(size_t)s]->setBounds(x, t.getY() + 177, w, 28);
        }
        const int testX = t.getX() + kNumStops * (w + gap);
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
    for (auto &row : decayRows)
    {
        row->setBounds(rows.removeFromTop(36));
    }
    perPipe.setBounds(decayArea.getRight() - 150, decayArea.getY() + 8, 80, 20);
    onSum.setBounds(decayArea.getRight() - 70, decayArea.getY() + 8, 70, 20);

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

void ConsoleView::refresh()
{
    const bool sum = proc.apvts.getRawParameterValue("degMode")->load() > 0.5f;
    perPipe.setToggleState(!sum, juce::dontSendNotification);
    onSum.setToggleState(sum, juce::dontSendNotification);
    meterL = proc.engine.meterL.load();
    meterR = proc.engine.meterR.load();
    for (auto &r : noiseRows)
        r->repaint();
    for (auto &r : decayRows)
        r->repaint();
    for (auto &r : stopVol)
        r->repaint();
    for (auto &r : stopWind)
        r->repaint();
    testToneLevel->repaint();
    testToneColour->repaint();
    keys.repaint();
    sceneMorph->repaint();
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
    g.setFont(A.monoFont(11.0f));
    g.drawText(juce::String(on) + " von " + juce::String(kNumStops) + " gezogen", head,
               juce::Justification::centredRight);

    drawSectionTitle(g, noiseArea, "Traktur",
                     juce::String(juce::CharPointer_UTF8("Ger\xc3\xa4uschschicht")));
    g.setFont(Assets::get().sansFont(14.0f));
    g.setColour(col::ink);
    g.drawText(juce::String::fromUTF8("T\xc3\xb6ne verteilen"), spreadLabel,
               juce::Justification::centredLeft);
    drawSectionTitle(g, decayArea, "Verfall");
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
    g.drawText(juce::String(juce::CharPointer_UTF8(
                   "C \xe2\x80\x93 a\xe2\x80\xb3 \xc2\xb7 kurze Oktave \xc2\xb7 41 Tasten")),
               kh, juce::Justification::centredLeft);
    g.drawText("MIDI 36-81", kh, juce::Justification::centredRight);
}
} // namespace rysum::ui
