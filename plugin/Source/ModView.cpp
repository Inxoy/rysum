#include "ModView.h"

namespace rysum::ui
{
namespace
{
    juce::String groupFor (const juce::String& id)
    {
        if (id.startsWith ("vol") || id.startsWith ("swind") || id.startsWith ("rate") || id.startsWith ("depth") || id.startsWith ("ellipse"))
            return "Register";
        if (id.startsWith ("deg")) return "Verfall";
        if (id == "click" || id == "clickBlend" || id == "releaseClick" || id == "stopPull" || id == "wind" || id == "master")
            return juce::String::fromUTF8 ("Traktur \xc2\xb7 Ausgang");
        if (id.startsWith ("ir")) return "Hall";
        if (id.startsWith ("lfo") || id.startsWith ("env") || id.startsWith ("mod")) return "Modulation";
        return "Klang";
    }

    int targetFromComboId (int id) { return id <= 0 ? -1 : (id < 1000 ? id - 1 : kGlobalTargetBase + (id - 1000)); }
    int comboIdFromTarget (int t) { return t < 0 ? 0 : (t < kNumPipeTargets ? t + 1 : 1000 + (t - kGlobalTargetBase)); }
}

void ModView::fillTargetMenu (juce::PopupMenu& menu, RysumProcessor& proc, int idOffset)
{
    const auto pipeNames = pipeTargetNames();
    menu.addSectionHeader (juce::String::fromUTF8 ("Je Pfeife (Versatz, H\xc3\xbcllkurve pro Pfeife)"));
    for (int t = 0; t < kNumPipeTargets; ++t) menu.addItem (idOffset + t + 1, pipeNames[t]);
    menu.addSeparator();
    const juce::StringArray groups { "Register", juce::String::fromUTF8 ("Traktur \xc2\xb7 Ausgang"), "Verfall", "Klang", "Hall", "Modulation" };
    for (auto& grp : groups)
    {
        juce::PopupMenu sub;
        for (int i = 0; i < proc.getNumParams(); ++i)
            if (proc.isModTarget (i) && groupFor (proc.getParamAt (i)->getParameterID()) == grp)
                sub.addItem (idOffset + 1000 + i, proc.getParamAt (i)->getName (64));
        if (sub.getNumItems() > 0) menu.addSubMenu (grp, sub);
    }
}

ModView::ModView (RysumProcessor& p) : proc (p)
{
    auto& st = proc.apvts;
    for (int i = 0; i < kNumLfos; ++i)
    {
        auto& L = lfos[(size_t) i];
        const juce::String id = "lfo" + juce::String (i + 1);
        L.shape.addItemList (lfoShapeNames(), 1);
        L.div.addItemList (lfoDivNames(), 1);
        L.spreadMode.addItemList ({ "Versatz entlang der Tastatur", juce::String::fromUTF8 ("Versatz zuf\xc3\xa4llig je Pfeife") }, 1);
        L.shapeAtt = std::make_unique<ComboAtt> (st, id + "Shape", L.shape);
        L.divAtt = std::make_unique<ComboAtt> (st, id + "Div", L.div);
        L.spreadModeAtt = std::make_unique<ComboAtt> (st, id + "SpreadMode", L.spreadMode);
        L.sync.setClickingTogglesState (true);
        L.sync.setTooltip ("Im Projekttempo, an der Taktposition eingerastet");
        L.syncAtt = std::make_unique<ButtonAtt> (st, id + "Sync", L.sync);
        L.rate = std::make_unique<ParamRow> (st, id + "Rate", "Tempo");
        L.spread = std::make_unique<ParamRow> (st, id + "Spread", "Versatz je Pfeife");
        L.spread->slider.setTooltip (juce::String::fromUTF8 ("0 % = alle Pfeifen gleich, 100 % = ein ganzer Durchlauf \xc3\xbc" "ber die Pfeifen verteilt (nur f\xc3\xbcr Pfeifen-Ziele)"));
        for (juce::Component* c : { (juce::Component*) &L.shape, (juce::Component*) &L.div, (juce::Component*) &L.spreadMode,
                                    (juce::Component*) &L.sync, (juce::Component*) L.rate.get(), (juce::Component*) L.spread.get() })
            addAndMakeVisible (c);
    }
    for (int j = 0; j < kNumEnvs; ++j)
    {
        auto& E = envs[(size_t) j];
        const juce::String id = "env" + juce::String (j + 1);
        E.mode.addItemList ({ juce::String::fromUTF8 ("Pro Pfeife (jeder Ton eigene H\xc3\xbcllkurve)"), "Global (jede Taste startet neu)" }, 1);
        E.modeAtt = std::make_unique<ComboAtt> (st, id + "Mode", E.mode);
        E.delay   = std::make_unique<ParamRow> (st, id + "Delay", juce::String::fromUTF8 ("Verz\xc3\xb6gerung"), true);
        E.attack  = std::make_unique<ParamRow> (st, id + "Attack", "Anstieg", true);
        E.decay   = std::make_unique<ParamRow> (st, id + "Decay", "Abfall", true);
        E.sustain = std::make_unique<ParamRow> (st, id + "Sustain", "Halten", true);
        E.release = std::make_unique<ParamRow> (st, id + "Release", "Ausklang", true);
        for (juce::Component* c : { (juce::Component*) &E.mode, (juce::Component*) E.delay.get(), (juce::Component*) E.attack.get(),
                                    (juce::Component*) E.decay.get(), (juce::Component*) E.sustain.get(), (juce::Component*) E.release.get() })
            addAndMakeVisible (c);
    }
    const auto srcNames = modSourceNames();
    for (int r = 0; r < kNumRoutes; ++r)
    {
        auto& R = routes[(size_t) r];
        R.src.addItemList (srcNames, 1);
        R.src.setTextWhenNothingSelected ("Quelle");
        fillTargetMenu (*R.tgt.getRootMenu(), proc, 0);
        R.tgt.setTextWhenNothingSelected ("Ziel");
        R.src.onChange = [this, r] { routeEdited (r); };
        R.tgt.onChange = [this, r] { routeEdited (r); };
        R.amount.setSliderStyle (juce::Slider::LinearHorizontal);
        R.amount.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        R.amount.getProperties().set ("bipolar", true);
        R.amount.setDoubleClickReturnValue (true, 0.0);
        R.amountAtt = std::make_unique<SliderAtt> (st, proc.getModAmountId (r), R.amount);
        R.amount.onValueChange = [this, r] { list.repaint (routes[(size_t) r].row); };
        R.amount.setScrollWheelEnabled (false);          // the wheel scrolls the list
        R.clear.setTooltip ("Zuordnung entfernen");
        R.clear.onClick = [this, r] { proc.clearRoute (r); syncRoutes(); };
        for (juce::Component* c : { (juce::Component*) &R.src, (juce::Component*) &R.tgt, (juce::Component*) &R.amount, (juce::Component*) &R.clear })
            list.addAndMakeVisible (c);
    }
    matrixView.setViewedComponent (&list, false);
    matrixView.setScrollBarsShown (true, false);
    matrixView.setScrollBarThickness (6);
    matrixView.setSingleStepSizes (kRowH, kRowH);
    addAndMakeVisible (matrixView);
    syncRoutes();
}

ModView::~ModView() = default;

float ModView::paramValue (const juce::String& id) const
{
    if (auto* v = proc.apvts.getRawParameterValue (id)) return v->load();
    return 0.0f;
}

void ModView::syncRoutes()
{
    updating = true;
    for (int r = 0; r < kNumRoutes; ++r)
    {
        auto& R = routes[(size_t) r];
        const int s = proc.getRouteSrc (r), t = proc.getRouteTgt (r);
        R.src.setSelectedId (s >= 0 ? s + 1 : 0, juce::dontSendNotification);
        R.tgt.setSelectedId (comboIdFromTarget (t), juce::dontSendNotification);
    }
    updating = false;
    lastRouteVersion = proc.routeVersion.load();
    list.repaint();
}

void ModView::routeEdited (int r)
{
    if (updating) return;
    auto& R = routes[(size_t) r];
    const int s = R.src.getSelectedId() - 1;
    const int t = targetFromComboId (R.tgt.getSelectedId());
    proc.setRoute (r, s, t);
    lastRouteVersion = proc.routeVersion.load();
    // a fresh routing with amount 0 would do nothing: start at +25 %
    if (s >= 0 && t >= 0)
        if (auto* a = proc.apvts.getParameter (proc.getModAmountId (r)))
            if (std::abs (a->convertFrom0to1 (a->getValue())) < 0.0005f)
            {
                a->beginChangeGesture(); a->setValueNotifyingHost (a->convertTo0to1 (0.25f)); a->endChangeGesture();
            }
}

void ModView::resized()
{
    auto r = getLocalBounds().reduced (28, 0).withTrimmedTop (18).withTrimmedBottom (14);
    const int gap = 24;
    const int cw = (r.getWidth() - 3 * gap) / 4;
    const int topH = 290;
    for (int i = 0; i < kNumLfos; ++i)
    {
        auto& L = lfos[(size_t) i];
        L.box = { r.getX() + i * (cw + gap), r.getY(), cw, topH };
        auto b = L.box.withTrimmedTop (38);
        L.scope = b.removeFromTop (54);
        b.removeFromTop (8);
        L.shape.setBounds (b.removeFromTop (24));
        b.removeFromTop (6);
        auto sr = b.removeFromTop (24);
        L.sync.setBounds (sr.removeFromLeft (64));
        sr.removeFromLeft (6);
        L.div.setBounds (sr);
        b.removeFromTop (4);
        L.rate->setBounds (b.removeFromTop (38));
        L.spread->setBounds (b.removeFromTop (38));
        L.spreadMode.setBounds (b.removeFromTop (24));
    }
    const int y2 = r.getY() + topH + 18;
    const int botH = r.getBottom() - y2;
    for (int j = 0; j < kNumEnvs; ++j)
    {
        auto& E = envs[(size_t) j];
        E.box = { r.getX() + j * (cw + gap), y2, cw, botH };
        auto b = E.box.withTrimmedTop (38);
        E.scope = b.removeFromTop (54);
        b.removeFromTop (8);
        E.mode.setBounds (b.removeFromTop (24));
        b.removeFromTop (6);
        for (auto* row : { E.delay.get(), E.attack.get(), E.decay.get(), E.sustain.get(), E.release.get() })
            row->setBounds (b.removeFromTop (31));
    }
    matrixBox = { r.getX() + 2 * (cw + gap), y2, 2 * cw + gap, botH };
    auto m = matrixBox.withTrimmedTop (38 + 16);
    matrixView.setBounds (m);
    list.setSize (m.getWidth() - 10, kRowH * kNumRoutes);
    for (int i = 0; i < kNumRoutes; ++i)
    {
        auto& R = routes[(size_t) i];
        auto row = juce::Rectangle<int> (0, i * kRowH, list.getWidth(), kRowH);
        R.row = row;
        auto rr = row.reduced (0, 1);
        rr.removeFromLeft (22);                        // slot number
        R.src.setBounds (rr.removeFromLeft (104));
        rr.removeFromLeft (18);                        // live source value
        R.tgt.setBounds (rr.removeFromLeft (196));
        rr.removeFromLeft (10);
        R.clear.setBounds (rr.removeFromRight (22));
        rr.removeFromRight (50);
        R.amount.setBounds (rr);
    }
}

void ModView::refresh()
{
    if (proc.routeVersion.load() != lastRouteVersion) syncRoutes();
    for (int i = 0; i < kNumLfos; ++i)
    {
        auto& L = lfos[(size_t) i];
        const bool sync = L.sync.getToggleState();
        L.div.setEnabled (sync);
        L.rate->setAlpha (sync ? 0.35f : 1.0f);
        repaint (L.scope);
    }
    for (auto& E : envs) repaint (E.scope.withRight (E.box.getRight()));
    list.repaint();
}

void ModView::paint (juce::Graphics& g)
{
    auto& A = Assets::get();
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.setOpacity (0.07f);
        drawCover (g, A.wall, getLocalBounds().toFloat(), 0.6f);
    }

    // ---- LFOs
    for (int i = 0; i < kNumLfos; ++i)
    {
        auto& L = lfos[(size_t) i];
        const juce::String id = "lfo" + juce::String (i + 1);
        const bool sync = paramValue (id + "Sync") > 0.5f;
        juce::String right = sync ? lfoDivNames()[(int) paramValue (id + "Div")]
                                  : juce::String (paramValue (id + "Rate"), 2) + " Hz";
        drawSectionTitle (g, L.box, "LFO " + juce::String (i + 1), right);

        const int shape = (int) paramValue (id + "Shape");
        const float spread = paramValue (id + "Spread");
        const int spreadMode = (int) paramValue (id + "SpreadMode");
        auto sc = L.scope.toFloat();
        g.setColour (col::line);
        g.fillRect (sc.getX(), sc.getCentreY(), sc.getWidth(), 1.0f);
        const bool random = shape == shSteps || shape == shSmooth;
        const double cycles = random ? 4.0 : 1.0;
        auto yOf = [&] (float v) { return sc.getCentreY() - v * (sc.getHeight() * 0.5f - 3.0f); };
        juce::Path path;
        for (int x = 0; x <= (int) sc.getWidth(); ++x)
        {
            const double ph = cycles * x / sc.getWidth();
            const float y = yOf (lfoValue (shape, ph, 0));
            if (x == 0) path.startNewSubPath (sc.getX(), y); else path.lineTo (sc.getX() + (float) x, y);
        }
        g.setColour (col::mute);
        g.strokePath (path, juce::PathStrokeType (1.2f));

        const double phase = proc.engine.lfoPhaseOut[(size_t) i].load();
        auto xOf = [&] (double ph) { const double f = std::fmod (ph, cycles); return sc.getX() + (float) ((f < 0 ? f + cycles : f) / cycles) * sc.getWidth(); };
        if (spread > 0.001f)
        {
            // one dot per key: where the wave is for that pipe
            g.setColour (col::dim.withAlpha (0.7f));
            for (int k = 0; k < kNumKeys; ++k)
            {
                const float off = spread * (spreadMode == 0 ? (float) k / (float) kNumKeys : hash01 ((uint32_t) (k * 7 + 1)));
                const double ph = phase + off;
                g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre ({ xOf (ph), yOf (lfoValue (shape, ph, 0)) }));
            }
        }
        const float x = xOf (phase);
        g.setColour (col::faint);
        g.fillRect (x - 0.5f, sc.getY(), 1.0f, sc.getHeight());
        g.setColour (col::white);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, yOf (lfoValue (shape, phase, 0)) }));
    }

    // ---- envelopes
    for (int j = 0; j < kNumEnvs; ++j)
    {
        auto& E = envs[(size_t) j];
        const juce::String id = "env" + juce::String (j + 1);
        const bool perPipe = paramValue (id + "Mode") < 0.5f;
        drawSectionTitle (g, E.box, juce::String::fromUTF8 ("H\xc3\xbcllkurve ") + juce::String (j + 1), perPipe ? "pro Pfeife" : "global");
        const float d = paramValue (id + "Delay"), a = paramValue (id + "Attack"), dc = paramValue (id + "Decay"),
                    su = paramValue (id + "Sustain"), rl = paramValue (id + "Release");
        auto sc = E.scope.toFloat().withTrimmedRight (14.0f);
        const float hold = juce::jmax (0.4f, 0.25f * (d + a + dc + rl));
        const float total = d + a + dc + hold + rl;
        auto X = [&] (float t) { return sc.getX() + t / total * sc.getWidth(); };
        auto Y = [&] (float v) { return sc.getBottom() - 2.0f - v * (sc.getHeight() - 5.0f); };
        juce::Path pth;
        pth.startNewSubPath (X (0), Y (0));
        pth.lineTo (X (d), Y (0));
        pth.lineTo (X (d + a), Y (1));
        for (int k = 1; k <= 24; ++k) { const float t = dc * k / 24.0f; pth.lineTo (X (d + a + t), Y (su + (1.0f - su) * std::exp (-4.0f * t / juce::jmax (0.001f, dc)))); }
        pth.lineTo (X (d + a + dc + hold), Y (su));
        for (int k = 1; k <= 24; ++k) { const float t = rl * k / 24.0f; pth.lineTo (X (d + a + dc + hold + t), Y (su * std::exp (-4.0f * t / juce::jmax (0.001f, rl)))); }
        g.setColour (col::line);
        g.fillRect (sc.getX(), sc.getBottom() - 2.0f, sc.getWidth(), 1.0f);
        g.setColour (col::mute);
        g.strokePath (pth, juce::PathStrokeType (1.2f));
        // live level of the global envelope (also what global targets follow)
        const float lv = juce::jlimit (0.0f, 1.0f, proc.engine.sourceOut[(size_t) (srcEnv1 + j)].load());
        auto meter = juce::Rectangle<float> (sc.getRight() + 8.0f, sc.getY(), 4.0f, sc.getHeight());
        g.setColour (col::track);
        g.fillRect (meter);
        g.setColour (col::white);
        g.fillRect (meter.withTop (Y (lv)));
    }

    // ---- matrix
    drawSectionTitle (g, matrixBox, "Zuordnungen", "Rechtsklick auf einen Regler: schnell zuweisen");
    {
        auto h = matrixBox.withTrimmedTop (38).withHeight (14);
        g.setFont (A.monoFont (10.0f));
        g.setColour (col::faint);
        g.drawText ("Quelle", h.withTrimmedLeft (22).withWidth (104), juce::Justification::centredLeft);
        g.drawText ("Ziel", h.withTrimmedLeft (144).withWidth (196), juce::Justification::centredLeft);
        g.drawText ("Menge", h.withTrimmedLeft (350), juce::Justification::centredLeft);
    }
}

void MatrixList::paint (juce::Graphics& g)
{
    auto& A = Assets::get();
    auto& proc = owner.proc;
    for (int r = 0; r < kNumRoutes; ++r)
    {
        auto& R = owner.routes[(size_t) r];
        const int s = proc.getRouteSrc (r);
        // live source value as a small bar between source and target
        g.setColour (r % 2 == 0 ? col::panel.withAlpha (0.0f) : col::panel.withAlpha (0.5f));
        g.fillRect (R.row);
        g.setColour (s >= 0 ? col::mute : col::faint.withAlpha (0.6f));
        g.setFont (A.monoFont (10.0f));
        g.drawText (juce::String (r + 1), R.row.withWidth (18), juce::Justification::centredRight);
        auto mark = juce::Rectangle<float> ((float) R.row.getX() + 130.0f, (float) R.row.getCentreY() - 6.0f, 10.0f, 12.0f);
        if (s >= 0)
        {
            const float v = juce::jlimit (-1.0f, 1.0f, proc.engine.sourceOut[(size_t) s].load());
            g.setColour (col::track);
            g.fillRect (mark.withWidth (2.0f).withX (mark.getCentreX() - 1.0f));
            g.setColour (col::ink);
            const float cy = mark.getCentreY(), hh = v * mark.getHeight() * 0.5f;
            g.fillRect (mark.getCentreX() - 2.0f, juce::jmin (cy, cy - hh), 4.0f, juce::jmax (1.0f, std::abs (hh)));
        }
        else
        {
            g.setColour (col::faint);
            g.setFont (A.monoFont (11.0f));
            g.drawText (juce::String::fromUTF8 ("\xe2\x86\x92"), mark.toNearestInt().expanded (2, 0), juce::Justification::centred);
        }
        if (auto* a = proc.apvts.getParameter (proc.getModAmountId (r)))
        {
            g.setColour (proc.getRouteTgt (r) >= 0 && s >= 0 ? col::dim : col::faint);
            g.setFont (A.monoFont (11.0f));
            g.drawText (a->getCurrentValueAsText(), R.clear.getBounds().translated (-50, 0).withWidth (46), juce::Justification::centredRight);
        }
    }
}
} // namespace rysum::ui
