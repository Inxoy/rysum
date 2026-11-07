#include "PipesView.h"

namespace rysum::ui
{
PipesView::PipesView (RysumProcessor& p) : proc (p)
{
    for (int s = 0; s < kNumStops; ++s)
    {
        auto& b = stopButtons[(size_t) s];
        b.setButtonText (prettyLabel (kStops[(size_t) s].label));
        b.onClick = [this, s] { select (s, key); };
        addAndMakeVisible (b);
    }
    addAndMakeVisible (preview);
    addAndMakeVisible (muteButton);
    preview.onClick = [this] { proc.engine.requestPreview (stop, key); };
    muteButton.onClick = [this]
    {
        auto& m = proc.engine.muted[(size_t) stop][(size_t) key];
        m = ! m.load();
        ++proc.changeCounter;
        refresh();
    };
    select (0, keyIndexForMidi (69));
}

void PipesView::select (int s, int k)
{
    stop = s;
    key = juce::jlimit (0, kNumKeys - 1, k);
    for (int i = 0; i < kNumStops; ++i) stopButtons[(size_t) i].setToggleState (i == s, juce::dontSendNotification);
    rebuildWave();
    refresh();
    repaint();
}

void PipesView::rebuildWave()
{
    envMax.clear(); envMin.clear();
    shownLib = proc.getLibrary();
    if (shownLib == nullptr) return;
    const auto& p = shownLib->pipes[(size_t) stop][(size_t) key];
    if (! p.valid || p.sample == nullptr) return;
    const int cols = 800;
    const auto& d = p.sample->data;
    envMax.assign (cols, 0.0f); envMin.assign (cols, 0.0f);
    for (int c = 0; c < cols; ++c)
    {
        const size_t a = d.size() * (size_t) c / cols, b = juce::jmax (a + 1, d.size() * (size_t) (c + 1) / cols);
        float mx = 0, mn = 0;
        for (size_t i = a; i < b && i < d.size(); ++i) { mx = juce::jmax (mx, d[i]); mn = juce::jmin (mn, d[i]); }
        envMax[(size_t) c] = mx; envMin[(size_t) c] = mn;
    }
}

void PipesView::resized()
{
    auto r = getLocalBounds();
    nav = r.removeFromLeft (190);
    auto n = nav.reduced (0, 18).withTrimmedLeft (28).withTrimmedRight (18).withTrimmedTop (24);
    for (auto& b : stopButtons) b.setBounds (n.removeFromTop (30).reduced (0, 2));
    r.reduce (28, 18);
    strip = r.removeFromTop (100);
    r.removeFromTop (16);
    header = r.removeFromTop (40);
    muteButton.setBounds (header.getRight() - 90, header.getY() + 6, 90, 26);
    preview.setBounds (header.getRight() - 196, header.getY() + 6, 100, 26);
    wave = r.removeFromTop (220);
    r.removeFromTop (16);
    info = r;
}

int PipesView::keyAt (juce::Point<int> pt) const
{
    if (! strip.contains (pt)) return -1;
    const float w = (float) strip.getWidth() / 46.0f;
    const int midi = 36 + juce::jlimit (0, 45, (int) ((float) (pt.x - strip.getX()) / w));
    return keyIndexForMidi (midi);
}

void PipesView::mouseDown (const juce::MouseEvent& e)
{
    const int k = keyAt (e.getPosition());
    if (k >= 0)
    {
        select (stop, k);
        if (e.getNumberOfClicks() > 1) proc.engine.requestPreview (stop, key);
    }
}

void PipesView::refresh()
{
    if (proc.getLibrary() != shownLib) rebuildWave();
    muteButton.setToggleState (proc.engine.muted[(size_t) stop][(size_t) key].load(), juce::dontSendNotification);
    repaint();
}

void PipesView::paint (juce::Graphics& g)
{
    auto& A = Assets::get();
    g.setColour (col::line);
    g.fillRect (nav.getRight() - 1, nav.getY(), 1, nav.getHeight());
    g.setColour (col::mute);
    g.setFont (A.sansFont (11.0f).withExtraKerningFactor (0.18f));
    g.drawText ("REGISTER", nav.withTrimmedLeft (28).withTrimmedTop (18).withHeight (18), juce::Justification::centredLeft);
    g.setFont (A.monoFont (11.0f));
    g.drawFittedText (juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f eigene Aufnahme\n\xe2\x97\x8c ersetzt (Nachbar)\n\xe2\x80\x93 keine Pfeife\n\nDoppelklick = h\xc3\xb6ren")),
                      nav.withTrimmedLeft (28).withTrimmedRight (12).withTop (nav.getBottom() - 110), juce::Justification::topLeft, 6);

    auto lib = proc.getLibrary();
    if (lib == nullptr)
    {
        g.setColour (col::dim);
        g.setFont (A.sansFont (16.0f));
        g.drawText (proc.getLoadStatus(), strip.withHeight (60), juce::Justification::centredLeft);
        return;
    }

    // ---- level strip over the 46 semitones
    {
        auto head = strip.removeFromTop (16);
        g.setFont (A.monoFont (11.0f));
        g.setColour (col::mute);
        g.drawText (prettyLabel (kStops[(size_t) stop].label) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 Pegel pro Taste (nicht normalisiert)")), head, juce::Justification::centredLeft);
        g.drawText ("Taste klicken", head, juce::Justification::centredRight);
        auto bars = strip.withTrimmedTop (4).withTrimmedBottom (16);
        const float gap = 2.0f, w = ((float) bars.getWidth() - gap * 45) / 46.0f;
        for (int i = 0; i < 46; ++i)
        {
            const int midi = 36 + i;
            const int k = keyIndexForMidi (midi);
            juce::Rectangle<float> col ((float) bars.getX() + (float) i * (w + gap), (float) bars.getY(), w, (float) bars.getHeight());
            if (k < 0)
            {
                g.setColour (col::border);
                g.fillRect (col.withTop (col.getBottom() - 2.0f));
                continue;
            }
            const auto& p = lib->pipes[(size_t) stop][(size_t) k];
            const float frac = juce::jlimit (0.05f, 1.0f, (p.levelDb + 60.0f) / 50.0f);
            auto bar = col.withTop (col.getBottom() - col.getHeight() * frac);
            const bool sel = k == key;
            const bool muted = proc.engine.muted[(size_t) stop][(size_t) k].load();
            if (p.substituted || muted)
            {
                g.setColour (sel ? col::white : col::ink);
                const float dash[] { 3.0f, 2.0f };
                juce::Path pth; pth.addRectangle (bar.reduced (0.5f));
                juce::PathStrokeType (1.0f).createDashedStroke (pth, pth, dash, 2);
                g.fillPath (pth);
            }
            else
            {
                g.setColour (sel ? col::white : col::faint);
                g.fillRect (bar);
            }
        }
        g.setFont (A.monoFont (10.0f));
        const std::pair<int, const char*> labels[] { { 36, "C" }, { 48, "c" }, { 60, "c\xe2\x80\xb2" }, { 69, "a\xe2\x80\xb2" }, { 72, "c\xe2\x80\xb3" }, { 81, "a\xe2\x80\xb3" } };
        for (auto& l : labels)
        {
            g.setColour (kPhysKeys[(size_t) key] == l.first ? col::white : col::mute);
            g.drawText (juce::String (juce::CharPointer_UTF8 (l.second)),
                        juce::Rectangle<float> ((float) bars.getX() + (float) (l.first - 36) * (w + gap), (float) bars.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
        }
    }

    const auto& p = lib->pipes[(size_t) stop][(size_t) key];

    // ---- header
    g.setColour (col::ink);
    g.fillRect (header.getX(), header.getY(), header.getWidth(), 1);
    g.setFont (A.sansFont (24.0f));
    const auto name = helmholtzName (p.midi);
    g.drawText (name, header.withTrimmedTop (6), juce::Justification::topLeft);
    g.setFont (A.monoFont (12.0f));
    g.setColour (col::mute);
    g.drawText ("MIDI " + juce::String (p.midi) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + prettyLabel (kStops[(size_t) stop].label),
                header.withTrimmedTop (14).withTrimmedLeft ((int) juce::GlyphArrangement::getStringWidth (A.sansFont (24.0f), name) + 12), juce::Justification::topLeft);

    // ---- waveform (label band on top, wave below)
    auto labelBand = wave.withHeight (18);
    auto waveArea = wave.withTrimmedTop (18);
    g.setColour (col::panel);
    g.fillRect (waveArea);
    if (p.valid && p.sample != nullptr && ! envMax.empty())
    {
        const float len = (float) p.sample->size();
        auto xAt = [&] (int s) { return (float) waveArea.getX() + (float) waveArea.getWidth() * (float) s / len; };
        const float mid = (float) waveArea.getCentreY(), half = (float) waveArea.getHeight() * 0.45f;
        float peak = 0.0001f;
        for (size_t i = 0; i < envMax.size(); ++i) peak = juce::jmax (peak, envMax[i], -envMin[i]);
        const float scale = half / peak;
        juce::Path path;
        const int n = (int) envMax.size();
        for (int i = 0; i < n; ++i)
        {
            const float x = (float) waveArea.getX() + (float) waveArea.getWidth() * (float) i / (float) (n - 1);
            if (i == 0) path.startNewSubPath (x, mid - envMax[0] * scale); else path.lineTo (x, mid - envMax[(size_t) i] * scale);
        }
        for (int i = n - 1; i >= 0; --i)
            path.lineTo ((float) waveArea.getX() + (float) waveArea.getWidth() * (float) i / (float) (n - 1), mid - envMin[(size_t) i] * scale);
        path.closeSubPath();
        g.setColour (col::ink.withAlpha (0.8f));
        g.fillPath (path);

        const double sr = p.sample->sampleRate;
        g.setFont (A.monoFont (10.0f));
        auto line = [&] (int s, juce::Colour c) { g.setColour (c); g.fillRect (xAt (s), (float) waveArea.getY(), 1.0f, (float) waveArea.getHeight()); };
        auto label = [&] (int s, const juce::String& t)
        {
            g.setColour (col::dim);
            g.drawText (t, juce::Rectangle<float> (xAt (s) + 3.0f, (float) labelBand.getY(), 260.0f, (float) labelBand.getHeight()), juce::Justification::centredLeft);
        };
        if (p.hasLoop())
        {
            g.setColour (col::bg.withAlpha (0.35f));
            g.fillRect (juce::Rectangle<float> (xAt (p.loopStart), (float) waveArea.getY(), xAt (p.loopEnd) - xAt (p.loopStart), (float) waveArea.getHeight()));
            line (p.loopStart, col::white); line (p.loopEnd, col::white);
            label (p.loopStart, u8 ("Schleife ") + juce::String (p.loopStart / sr, 3) + u8 (" \xe2\x80\x93 ") + juce::String (p.loopEnd / sr, 3) + " s");
        }
        line (p.onset, col::faint);
        label (p.onset, "Einsatz");
        line (p.release, col::mute);
        label (p.release, u8 ("Loslassen \xc2\xb7 Ausklang ") + juce::String ((len - p.release) / sr, 2) + " s");
        drawGrain (g, waveArea, 0.5f);
    }
    else
    {
        g.setColour (col::mute);
        g.setFont (A.sansFont (15.0f));
        g.drawText ("keine Aufnahme", waveArea, juce::Justification::centred);
    }

    // ---- info grid
    struct KV { juce::String k, v; };
    std::vector<KV> kv;
    const auto ct = juce::String (juce::CharPointer_UTF8 (" ct"));
    if (p.substituted)
        kv.push_back ({ "Status", juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8c ersetzt durch ")) + helmholtzName (kPhysKeys[(size_t) p.substituteFrom]) + " (tonh. verschoben)" });
    else
        kv.push_back ({ "Status", juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f eigene Aufnahme")) + (p.takes > 1 ? u8 (" \xc2\xb7 ") + juce::String (p.takes) + " Takes" : juce::String()) });
    kv.push_back ({ u8 ("Soll (mittelt\xc3\xb6nig)"), juce::String (meantoneHz (p.midi), 2) + " Hz" });
    kv.push_back ({ "Gemessen", p.measuredHz > 0 && ! p.substituted ? juce::String (p.measuredHz, 2) + " Hz" : juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93")) });
    kv.push_back ({ "Abweichung", p.hasCents && ! p.substituted ? juce::String (p.cents - 1200.0 * std::round (p.cents / 1200.0), 1) + ct : juce::String (juce::CharPointer_UTF8 ("aus Tastenfolge")) });
    kv.push_back ({ "Pegel", juce::String (p.levelDb, 1) + " dBFS" });
    kv.push_back ({ juce::String (juce::CharPointer_UTF8 ("L\xc3\xa4nge")), p.sample ? juce::String (p.sample->size() / p.sample->sampleRate, 2) + " s" : juce::String() });
    kv.push_back ({ "Quelle", p.source + " @ " + juce::String (p.sourceTime, 2) + " s" });
    kv.push_back ({ "Mono", p.monoMethod.isEmpty() ? juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93")) : p.monoMethod.replace ("aligned_sum", "beide Mikros ausgerichtet").replace ("left", "linkes Mikro").replace ("right", "rechtes Mikro") });
    const int cols = 4;
    const int cw = (info.getWidth() - (cols - 1) * 24) / cols;
    for (size_t i = 0; i < kv.size(); ++i)
    {
        const int c = (int) i % cols, r = (int) i / cols;
        juce::Rectangle<int> cell (info.getX() + c * (cw + 24), info.getY() + r * 52, cw, 46);
        g.setColour (col::line);
        g.fillRect (cell.getX(), cell.getY(), cell.getWidth(), 1);
        g.setColour (col::mute);
        g.setFont (A.sansFont (11.0f));
        g.drawText (kv[i].k, cell.withTrimmedTop (6).withHeight (14), juce::Justification::centredLeft);
        g.setColour (col::ink);
        g.setFont (A.monoFont (12.0f));
        g.drawFittedText (kv[i].v, cell.withTrimmedTop (22), juce::Justification::topLeft, 2);
    }
    if (p.flags.size() > 0)
    {
        g.setColour (col::mute);
        g.setFont (A.monoFont (11.0f));
        g.drawFittedText ("Hinweise: " + p.flags.joinIntoString (", "), info.withTrimmedTop (110).withHeight (30), juce::Justification::topLeft, 2);
    }
}
} // namespace rysum::ui
