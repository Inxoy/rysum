#include "SpaceView.h"

namespace rysum::ui
{
// ---------------------------------------------------------------------------
juce::Point<float> SpaceCircle::toScreen (Pos p) const
{
    const float R = radius();
    const float r = R * juce::jlimit (0.0f, 1.18f, (90.0f - p.el) / 90.0f);
    const float a = juce::degreesToRadians (p.az);
    const auto c = centre();
    return { c.x - r * std::sin (a), c.y - r * std::cos (a) };
}

Pos SpaceCircle::fromScreen (juce::Point<float> s) const
{
    const auto c = centre();
    const float dx = s.x - c.x, dy = s.y - c.y;
    const float r = std::sqrt (dx * dx + dy * dy) / radius();
    Pos p;
    p.az = juce::radiansToDegrees (std::atan2 (-dx, -dy));
    p.el = juce::jlimit (-16.0f, 90.0f, 90.0f - 90.0f * r);
    return p;
}

void SpaceCircle::paint (juce::Graphics& g)
{
    auto& A = Assets::get();
    const auto c = centre();
    const float R = radius();

    g.setColour (col::border);
    g.drawEllipse (juce::Rectangle<float> (2 * R, 2 * R).withCentre (c), 1.0f);
    g.setColour (col::line);
    for (float el : { 30.0f, 60.0f })
    {
        const float r = R * (90.0f - el) / 90.0f;
        g.drawEllipse (juce::Rectangle<float> (2 * r, 2 * r).withCentre (c), 1.0f);
    }
    g.setColour (juce::Colour (0xff23231f));
    g.drawLine (c.x, c.y - R, c.x, c.y + R);
    g.drawLine (c.x - R, c.y, c.x + R, c.y);

    g.setFont (A.monoFont (10.0f));
    g.setColour (col::mute);
    g.drawText (juce::String (juce::CharPointer_UTF8 ("vorn \xc2\xb7 0\xc2\xb0")), juce::Rectangle<float> (120, 14).withCentre ({ c.x, c.y - R - 12 }), juce::Justification::centred);
    g.setColour (col::faint);
    g.drawText (juce::String (juce::CharPointer_UTF8 ("+90\xc2\xb0")), juce::Rectangle<float> (40, 14).withCentre ({ c.x - R - 22, c.y }), juce::Justification::centred);
    g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92" "90\xc2\xb0")), juce::Rectangle<float> (40, 14).withCentre ({ c.x + R + 22, c.y }), juce::Justification::centred);
    g.drawText (juce::String (juce::CharPointer_UTF8 ("hinten")), juce::Rectangle<float> (60, 14).withCentre ({ c.x, c.y + R + 12 }), juce::Justification::centred);
    g.setColour (juce::Colour (0xff55534f));
    g.drawText (juce::String (juce::CharPointer_UTF8 ("30\xc2\xb0")), juce::Rectangle<float> (30, 12).withPosition (c.x + 4, c.y - R * 0.667f - 13), juce::Justification::centredLeft);
    g.drawText (juce::String (juce::CharPointer_UTF8 ("60\xc2\xb0")), juce::Rectangle<float> (30, 12).withPosition (c.x + 4, c.y - R * 0.333f - 13), juce::Justification::centredLeft);

    const auto params = proc.readParams();

    // the drawn trajectory of the selected stop (bright while it is used, faint otherwise) and the stroke being drawn
    {
        const auto& dp = proc.getDrawnPath (selected);
        const bool used = params.motionMode[(size_t) selected] == (int) Motion::Drawn;
        if (dp.points.size() >= 2 && (used || drawMode))
        {
            juce::Path pth;
            for (size_t i = 0; i < dp.points.size(); ++i)
            {
                const auto q = toScreen ({ dp.points[i].az, dp.points[i].el, 6.0f });
                if (i == 0) pth.startNewSubPath (q); else pth.lineTo (q);
            }
            if (! dp.pingPong) pth.closeSubPath();
            g.setColour (col::rubric.withAlpha (used ? 0.75f : 0.35f));
            g.strokePath (pth, juce::PathStrokeType (1.3f));
            const auto s0 = toScreen ({ dp.points.front().az, dp.points.front().el, 6.0f });
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (s0));
        }
        if (stroke.size() >= 2)
        {
            juce::Path pth;
            for (size_t i = 0; i < stroke.size(); ++i)
            {
                const auto q = toScreen ({ stroke[i].az, stroke[i].el, 6.0f });
                if (i == 0) pth.startNewSubPath (q); else pth.lineTo (q);
            }
            g.setColour (col::rubric.brighter (0.4f));
            g.strokePath (pth, juce::PathStrokeType (2.0f));
        }
        if (drawMode)
        {
            g.setFont (A.monoFont (11.0f));
            g.setColour (col::rubric.brighter (0.4f));
            g.drawText (juce::String::fromUTF8 ("Bahn zeichnen: mit gedr\xc3\xbc" "ckter Maustaste eine Linie ziehen (Mitte = oben, Rand = Horizont)"),
                        juce::Rectangle<float> (c.x - R, c.y + R + 22.0f, 2.0f * R, 14.0f), juce::Justification::centred);
        }
    }

    // draw the selected stop last (on top)
    for (int pass = 0; pass < 2; ++pass)
        for (int s = 0; s < kNumStops; ++s)
        {
            const bool isSel = s == selected;
            if ((pass == 1) != isSel) continue;
            const bool on = params.stopOn[(size_t) s];
            const uint64_t sounding = proc.engine.soundingMask[(size_t) s].load();
            juce::Graphics::ScopedSaveState ss (g);
            g.setOpacity (isSel ? 1.0f : (on ? 0.75f : 0.3f));
            for (int k = 0; k < kNumKeys; ++k)
            {
                const auto pos = proc.engine.displayPosition (s, k, params);
                const auto p = toScreen (pos);
                const bool snd = (sounding >> k) & 1;
                if (snd)
                {
                    g.setColour (col::white.withAlpha (0.8f));
                    g.drawEllipse (juce::Rectangle<float> (11.0f, 11.0f).withCentre (p), 1.0f);
                }
                // nearer pipes are drawn bigger
                const float near = juce::jlimit (0.55f, 1.6f, std::sqrt (6.0f / pos.dist));
                drawStopDot (g, s, p, (isSel ? 5.0f : 3.6f) * near, isSel && snd);
            }
        }

    // listener in the centre
    g.setColour (col::ink);
    g.drawEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (c), 1.0f);
}

void SpaceCircle::mouseDown (const juce::MouseEvent& e)
{
    if (drawMode)
    {
        stroke.clear();
        strokeStart = juce::Time::getMillisecondCounterHiRes();
        const auto q = fromScreen (e.position);
        stroke.push_back ({ q.az, q.el, 0.0f });
        lastStrokePos = e.position;
        repaint();
        return;
    }
    const auto params = proc.readParams();
    dragKey = -1;
    float best = 10.0f;
    for (int k = 0; k < kNumKeys; ++k)
    {
        const float d = toScreen (proc.engine.displayPosition (selected, k, params)).getDistanceFrom (e.position);
        if (d < best) { best = d; dragKey = k; }
    }
    rotating = dragKey < 0;
    if (rotating)
    {
        const auto c = centre();
        rotateStartAngle = juce::radiansToDegrees (std::atan2 (-(e.position.x - c.x), -(e.position.y - c.y)));
        for (int k = 0; k < kNumKeys; ++k)
            rotateStart[(size_t) k] = { proc.engine.baseAz[(size_t) selected][(size_t) k].load(), proc.engine.baseEl[(size_t) selected][(size_t) k].load(),
                                        proc.engine.baseDist[(size_t) selected][(size_t) k].load() };
    }
}

void SpaceCircle::mouseDrag (const juce::MouseEvent& e)
{
    if (drawMode)
    {
        if (e.position.getDistanceFrom (lastStrokePos) >= 2.0f && stroke.size() < 4000)
        {
            const auto q = fromScreen (e.position);
            stroke.push_back ({ q.az, q.el, (float) ((juce::Time::getMillisecondCounterHiRes() - strokeStart) * 0.001) });
            lastStrokePos = e.position;
            repaint();
        }
        return;
    }
    if (dragKey >= 0)
    {
        auto np = fromScreen (e.position);
        np.dist = proc.engine.baseDist[(size_t) selected][(size_t) dragKey].load();     // dragging keeps the distance
        proc.setPipePosition (selected, dragKey, np);
    }
    else if (rotating)
    {
        // drag on empty space: turn all pipes of the selected stop around the listener
        const auto c = centre();
        const float a = juce::radiansToDegrees (std::atan2 (-(e.position.x - c.x), -(e.position.y - c.y)));
        const float delta = a - rotateStartAngle;
        for (int k = 0; k < kNumKeys; ++k)
        {
            auto p = rotateStart[(size_t) k];
            p.az += delta;
            proc.setPipePosition (selected, k, p);
        }
    }
    if (onUserMovedPipes) onUserMovedPipes();
    repaint();
}

void SpaceCircle::mouseUp (const juce::MouseEvent&)
{
    dragKey = -1; rotating = false;
    if (drawMode)
    {
        auto pts = std::move (stroke);
        stroke.clear();
        if (pts.size() >= 3 && onPathDrawn) onPathDrawn (std::move (pts));
        repaint();
    }
}

// ---------------------------------------------------------------------------
SpaceView::SpaceView (RysumProcessor& p) : proc (p), circle (p, selected)
{
    addAndMakeVisible (circle);

    for (int s = 0; s < kNumStops; ++s)
    {
        stopButtons.push_back (std::make_unique<StopRow> (s));
        stopButtons.back()->onClick = [this, s] { selectStop (s); };
        addAndMakeVisible (*stopButtons.back());
        pullButtons.push_back (std::make_unique<PullToggle>());
        addAndMakeVisible (*pullButtons.back());
        pullAttachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, RysumProcessor::stopParamId (s), *pullButtons.back()));
    }
    const auto lnames = layoutNames();
    for (int i = 0; i < 6; ++i)
    {
        auto& b = layoutButtons[(size_t) i];
        b.setButtonText (lnames[i]);
        b.onClick = [this, i] { proc.layouts[(size_t) selected].type = i; layoutChanged(); };
        addAndMakeVisible (b);
    }
    const auto mnames = motionNames();
    for (int i = 0; i < 7; ++i)
    {
        auto& b = motionButtons[(size_t) i];
        b.setButtonText (mnames[i]);
        b.onClick = [this, i]
        {
            if (auto* pr = proc.apvts.getParameter (RysumProcessor::motionParamId (selected)))
                pr->setValueNotifyingHost (pr->convertTo0to1 ((float) i));
        };
        addAndMakeVisible (b);
    }
    motionButtons[6].setTooltip (u8 ("Die Pfeifen laufen die gezeichnete Bahn entlang (Bahn zeichnen \xe2\x86\x92)"));
    motionButtons[5].setTooltip (u8 ("Spr\xc3\xbcnge an zuf\xc3\xa4llige Orte \xe2\x80\x93 mit \xe2\x80\x9eharte Spr\xc3\xbcnge\xe2\x80\x9c ohne jede Gl\xc3\xa4ttung: bei hohem Tempo klickt es"));
    drawButton.setClickingTogglesState (true);
    drawButton.setTooltip (u8 ("Eine Bahn f\xc3\xbcr das gew\xc3\xa4hlte Register in die Draufsicht zeichnen. Die Pfeifen folgen ihr wie ein Zug; "
                               "\xe2\x80\x9eGeschwindigkeit\xe2\x80\x9c = Runden pro Sekunde."));
    drawButton.onClick = [this] { circle.drawMode = drawButton.getToggleState(); circle.repaint(); };
    addAndMakeVisible (drawButton);
    circle.onPathDrawn = [this] (std::vector<RysumProcessor::DrawnPath::Pt> pts)
    {
        proc.setDrawnPath (selected, std::move (pts));
        if (auto* pr = proc.apvts.getParameter (RysumProcessor::motionParamId (selected)))
        {
            pr->beginChangeGesture(); pr->setValueNotifyingHost (pr->convertTo0to1 ((float) Motion::Drawn)); pr->endChangeGesture();
        }
        drawButton.setToggleState (false, juce::dontSendNotification);
        circle.drawMode = false;
    };
    auto pathOpt = [this] (juce::TextButton& b, std::function<void (RysumProcessor::DrawnPath&)> f, const juce::String& tip)
    {
        b.setTooltip (tip);
        b.onClick = [this, f]
        {
            auto d = proc.getDrawnPath (selected);
            f (d);
            proc.setPathOptions (selected, d.pingPong, d.drawnTiming);
            circle.repaint();
        };
        addAndMakeVisible (b);
    };
    pathOpt (pathLoop, [] (RysumProcessor::DrawnPath& d) { d.pingPong = false; }, u8 ("Am Ende zur\xc3\xbc" "ck zum Anfang (die Bahn schlie\xc3\x9ft sich)"));
    pathOpt (pathPing, [] (RysumProcessor::DrawnPath& d) { d.pingPong = true; }, u8 ("Die Bahn hin und wieder zur\xc3\xbc" "ck"));
    pathOpt (pathEven, [] (RysumProcessor::DrawnPath& d) { d.drawnTiming = false; }, u8 ("Gleichm\xc3\xa4\xc3\x9fige Geschwindigkeit entlang der Bahn"));
    pathOpt (pathDrawnT, [] (RysumProcessor::DrawnPath& d) { d.drawnTiming = true; }, u8 ("Schnell, wo schnell gezeichnet wurde, langsam, wo langsam gezeichnet wurde"));
    jumpHard.setClickingTogglesState (true);
    jumpHard.setTooltip (u8 ("Spr\xc3\xbcnge ohne die kurze Gl\xc3\xa4ttung: der Ort wechselt auf das Sample genau \xe2\x80\x93 bei hohem Tempo h\xc3\xb6rbare Klicks, ab ~50 Hz ein Schnarren"));
    addAndMakeVisible (jumpHard);
    const auto fnames = ellipseFormNames();
    for (int i = 0; i < 4; ++i)
    {
        ellForm[(size_t) i].setButtonText (juce::String (i + 1));
        ellForm[(size_t) i].setTooltip ("Ellipse: " + fnames[i] + (i == 0 ? u8 (" \xe2\x80\x93 eine nahe Seite (du im Brennpunkt)")
                                                     : i == 1 ? u8 (" \xe2\x80\x93 zwei nahe Seiten (du in der Mitte)")
                                                              : u8 (" \xe2\x80\x93 ") + juce::String (i + 1) + " nahe Seiten"));
        ellForm[(size_t) i].onClick = [this, i] { if (ellFormAttachment) ellFormAttachment->setValueAsCompleteGesture ((float) i); };
        addAndMakeVisible (ellForm[(size_t) i]);
    }
    tabMotion.onClick = [this] { showTab (false); };
    tabHall.onClick = [this] { showTab (true); };
    addAndMakeVisible (tabMotion);
    addAndMakeVisible (tabHall);

    // room / impulse response
    addAndMakeVisible (irLoad);
    addAndMakeVisible (irClear);
    addAndMakeVisible (irFuma);
    irLoad.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Impulsantwort (WAV, AmbiX / Stereo / Mono)", proc.getIrFile(), "*.wav;*.aif;*.aiff;*.flac");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) proc.loadIr (fc.getResult()); });
    };
    irClear.onClick = [this] { proc.clearIr(); };
    irClear.setTooltip ("Impulsantwort entfernen");
    irFumaAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "irFuma", irFuma);
    irFuma.setClickingTogglesState (true);
    irFuma.setTooltip (juce::String::fromUTF8 ("Nur f\xc3\xbcr 4-Kanal-IRs: B-Format in FuMa-Reihenfolge (W X Y Z) statt AmbiX"));
    irDry = std::make_unique<ParamRow> (proc.apvts, "irDry", "Direkt", true);
    irWet = std::make_unique<ParamRow> (proc.apvts, "irWet", "Hall", true);
    irEnvelop = std::make_unique<ParamRow> (proc.apvts, "irEnvelop", juce::String::fromUTF8 ("Umh\xc3\xbcllung"), true);
    irDirect = std::make_unique<ParamRow> (proc.apvts, "irDirect", "Direktschall der IR", true);
    for (auto* r : { irDry.get(), irWet.get(), irEnvelop.get(), irDirect.get() }) addAndMakeVisible (*r);
    irEnvelop->slider.setTooltip (juce::String::fromUTF8 ("Nachhall als gleichm\xc3\xa4\xc3\x9figes Feld von allen Seiten (auch f\xc3\xbcr Mono-/Stereo-IRs und bis zur 5. Ordnung)"));
    irDirect->slider.setTooltip (juce::String::fromUTF8 ("Direktschall in der Impulsantwort \xe2\x80\x93 die Pfeifen selbst sind schon der Direktschall"));
    for (auto* b : { &irN3D, &irFollow }) { b->setClickingTogglesState (true); addAndMakeVisible (*b); }
    irOrder.addItemList ({ "Ord. 1", "Ord. 2", "Ord. 3", "Ord. 4", "Ord. 5" }, 1);
    irOrderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "irOrder", irOrder);
    irOrder.setTooltip (juce::String::fromUTF8 ("H\xc3\xb6" "chste Ambisonics-Ordnung des Halls (die Pfeifen selbst bleiben in voller Ordnung). "
                                               "H\xc3\xb6" "her = genauer, aber mehr Rechenlast."));
    addAndMakeVisible (irOrder);
    irLoad.setTooltip (juce::String::fromUTF8 ("Impulsantwort laden: WAV/AIFF/FLAC, Mono, Stereo oder AmbiX 1.\xe2\x80\x93" "5. Ordnung"));
    irN3DAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "irN3D", irN3D);
    irFollowAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "irFollow", irFollow);
    irN3D.setTooltip (juce::String::fromUTF8 ("Ambisonics-IR in N3D-Normierung (statt SN3D)"));
    irFollow.setTooltip (juce::String::fromUTF8 ("Die fr\xc3\xbchen Reflexionen kommen f\xc3\xbcr jede Pfeife aus deren Richtung"));

    auto setup = [this] (juce::Slider& s, double lo, double hi, const juce::String& suffix)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        s.setRange (lo, hi, 0.5);
        s.setTextValueSuffix (suffix);
        s.onValueChange = [this] { if (! updating) layoutChanged(); };
        addAndMakeVisible (s);
    };
    const auto deg = juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"));
    setup (width, 5.0, 360.0, deg);
    setup (centre, -180.0, 180.0, deg);
    setup (elevation, -15.0, 90.0, deg);
    setup (spread, 0.0, 45.0, deg);
    setup (distance, 1.0, 40.0, " m");
    distance.setSkewFactorFromMidPoint (8.0);

    selectStop (selected);
    showTab (false);
}

void SpaceView::showTab (bool hall)
{
    hallTab = hall;
    tabMotion.setToggleState (! hall, juce::dontSendNotification);
    tabHall.setToggleState (hall, juce::dontSendNotification);
    juce::Component* motionComps[] { &drawButton, &pathLoop, &pathPing, &pathEven, &pathDrawnT, &jumpHard,
                                     &ellForm[0], &ellForm[1], &ellForm[2], &ellForm[3] };
    for (auto* c : motionComps) c->setVisible (! hall);
    for (auto& b : motionButtons) b.setVisible (! hall);
    for (auto* r : { rateRow.get(), depthRow.get(), ellipseRow.get(), spreadRow.get(), ellDirRow.get(), ellTiltRow.get() }) if (r) r->setVisible (! hall);
    juce::Component* hallComps[] { &irLoad, &irClear, &irFuma, &irN3D, &irFollow, &irOrder, irDry.get(), irWet.get(), irEnvelop.get(), irDirect.get() };
    for (auto* c : hallComps) c->setVisible (hall);
    repaint();
}

void SpaceView::selectStop (int s)
{
    selected = s;
    for (int i = 0; i < kNumStops; ++i) stopButtons[(size_t) i]->setToggleState (i == s, juce::dontSendNotification);
    rateRow = std::make_unique<ParamRow> (proc.apvts, RysumProcessor::rateParamId (s), "Geschwindigkeit");
    depthRow = std::make_unique<ParamRow> (proc.apvts, RysumProcessor::depthParamId (s), "Tiefe", true);
    ellipseRow = std::make_unique<ParamRow> (proc.apvts, RysumProcessor::ellipseParamId (s), "Ellipse", true);
    spreadRow = std::make_unique<ParamRow> (proc.apvts, RysumProcessor::pathSpreadParamId (s), "Verteilung auf der Bahn", true);
    ellDirRow = std::make_unique<ParamRow> (proc.apvts, RysumProcessor::ellDirParamId (s), "Richtung (nahe Seite)", true);
    ellTiltRow = std::make_unique<ParamRow> (proc.apvts, RysumProcessor::ellTiltParamId (s), "Neigung", true);
    ellipseRow->slider.setTooltip (u8 ("Wie viel n\xc3\xa4her die Pfeifen auf der nahen Seite der Bahn kommen"));
    spreadRow->slider.setTooltip (u8 ("Gezeichnete Bahn: 0 % = alle Pfeifen des Registers an einem Punkt, 100 % = \xc3\xbc" "ber die ganze Bahn verteilt"));
    ellDirRow->slider.setTooltip (u8 ("Wo die nahe Seite der Ellipse liegt (0\xc2\xb0 = vorn) \xe2\x80\x93 modulierbar: die Ellipse dreht sich"));
    ellTiltRow->slider.setTooltip (u8 ("Die Bahn neigt sich: zur nahen Seite hin h\xc3\xb6her, auf der anderen Seite tiefer (negativ: umgekehrt)"));
    for (auto* r : { rateRow.get(), depthRow.get(), ellipseRow.get(), spreadRow.get(), ellDirRow.get(), ellTiltRow.get() }) addAndMakeVisible (*r);
    jumpHardAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, RysumProcessor::jumpHardParamId (s), jumpHard);
    if (auto* f = proc.apvts.getParameter (RysumProcessor::ellFormParamId (s)))
    {
        ellFormAttachment = std::make_unique<juce::ParameterAttachment> (*f, [this] (float v)
        {
            for (int i = 0; i < 4; ++i) ellForm[(size_t) i].setToggleState (juce::roundToInt (v) == i, juce::dontSendNotification);
            repaint (ellPreview);
        });
        ellFormAttachment->sendInitialUpdate();
    }
    for (auto* r : { rateRow.get(), depthRow.get(), ellipseRow.get(), spreadRow.get(), ellDirRow.get(), ellTiltRow.get() }) r->setVisible (! hallTab);
    syncLayoutControls();
    resized();
    repaint();
}

void SpaceView::syncLayoutControls()
{
    updating = true;
    const auto& L = proc.layouts[(size_t) selected];
    width.setValue (L.width, juce::dontSendNotification);
    centre.setValue (L.center, juce::dontSendNotification);
    elevation.setValue (L.elevation, juce::dontSendNotification);
    spread.setValue (L.spread, juce::dontSendNotification);
    distance.setValue (L.distance, juce::dontSendNotification);
    for (int i = 0; i < 6; ++i) layoutButtons[(size_t) i].setToggleState (i == L.type, juce::dontSendNotification);
    updating = false;
}

void SpaceView::layoutChanged()
{
    auto& L = proc.layouts[(size_t) selected];
    L.width = (float) width.getValue();
    L.center = (float) centre.getValue();
    L.elevation = (float) elevation.getValue();
    L.spread = (float) spread.getValue();
    L.distance = (float) distance.getValue();
    proc.applyLayout (selected);
    syncLayoutControls();
    repaint();
}

void SpaceView::resized()
{
    auto r = getLocalBounds();
    left = r.removeFromLeft (660);
    right = r.reduced (28, 18);
    circle.setBounds (left.reduced (20, 34).withTrimmedTop (10));

    auto rr = right;
    stopsBox = rr.removeFromTop (20 + kNumStops * 18);
    auto sb = stopsBox.withTrimmedTop (20);
    for (int slot = 0; slot < kNumStops; ++slot)
    {
        const int s = kDisplayOrder[(size_t) slot];
        auto row = sb.removeFromTop (18);
        pullButtons[(size_t) s]->setBounds (row.removeFromLeft (16));
        row.removeFromLeft (4);
        stopButtons[(size_t) s]->setBounds (row.reduced (0, 1));
    }
    rr.removeFromTop (4);

    layoutBox = rr.removeFromTop (30 + 44 + 5 * 18);
    auto lb = layoutBox.withTrimmedTop (30);
    auto row1 = lb.removeFromTop (22), row2 = lb.removeFromTop (22);
    const int bw1 = (row1.getWidth() - 8) / 3;
    for (int i = 0; i < 3; ++i) layoutButtons[(size_t) i].setBounds (row1.getX() + i * (bw1 + 4), row1.getY(), bw1, 20);
    for (int i = 0; i < 3; ++i) layoutButtons[(size_t) i + 3].setBounds (row2.getX() + i * (bw1 + 4), row2.getY(), bw1, 20);
    for (auto* s : { &width, &centre, &elevation, &spread, &distance })
        s->setBounds (lb.removeFromTop (18).withTrimmedLeft (100).withTrimmedRight (52));
    rr.removeFromTop (4);

    // lower part: "Bewegung" or "Hall" (tabs in the title line)
    tabBox = rr;
    tabHall.setBounds (tabBox.getRight() - 56, tabBox.getY() + 9, 56, 20);
    tabMotion.setBounds (tabHall.getX() - 4 - 86, tabBox.getY() + 9, 86, 20);
    motionBox = hallBox = tabBox;
    auto mb = tabBox.withTrimmedTop (34);
    {
        auto m1 = mb.removeFromTop (22), m2 = mb.removeFromTop (22);
        const int bw2 = (m1.getWidth() - 12) / 4;
        for (int i = 0; i < 4; ++i) motionButtons[(size_t) i].setBounds (m1.getX() + i * (bw2 + 4), m1.getY(), bw2, 20);
        for (int i = 0; i < 3; ++i) motionButtons[(size_t) i + 4].setBounds (m2.getX() + i * (bw2 + 4), m2.getY(), bw2, 20);
        drawButton.setBounds (m2.getX() + 3 * (bw2 + 4), m2.getY(), bw2, 20);
        mb.removeFromTop (2);
        if (rateRow) rateRow->setBounds (mb.removeFromTop (34));
        auto half = mb.removeFromTop (26);
        const int hw = (half.getWidth() - 16) / 2;
        if (depthRow)  depthRow->setBounds (half.getX(), half.getY(), hw, 26);
        if (spreadRow) spreadRow->setBounds (half.getX() + hw + 16, half.getY(), hw, 26);
        mb.removeFromTop (3);
        auto prow = mb.removeFromTop (20);
        const int pw = (prow.getWidth() - 16) / 5;
        juce::TextButton* pb[] { &pathLoop, &pathPing, &pathEven, &pathDrawnT, &jumpHard };
        for (int i = 0; i < 5; ++i) pb[i]->setBounds (prow.getX() + i * (pw + 4), prow.getY(), pw, 20);
        mb.removeFromTop (8);
        // ellipse: two rows of sliders, the form buttons, a small picture of the orbit (from above)
        auto eb = mb.removeFromTop (2 * 28 + 2);
        ellPreview = eb.removeFromRight (64).withSizeKeepingCentre (60, 56);
        eb.removeFromRight (10);
        auto e1 = eb.removeFromTop (26);
        eb.removeFromTop (4);
        auto e2 = eb.removeFromTop (26);
        const int ew = (e1.getWidth() - 12) / 2;
        if (ellipseRow) ellipseRow->setBounds (e1.getX(), e1.getY(), ew, 26);
        if (ellDirRow)  ellDirRow->setBounds (e1.getX() + ew + 12, e1.getY(), ew, 26);
        if (ellTiltRow) ellTiltRow->setBounds (e2.getX(), e2.getY(), ew, 26);
        const int fx = e2.getX() + ew + 12, fw = (ew - 9) / 4;
        for (int i = 0; i < 4; ++i) ellForm[(size_t) i].setBounds (fx + i * (fw + 3), e2.getY() + 6, fw, 20);
    }
    auto hb = tabBox.withTrimmedTop (34);
    auto brow = hb.removeFromTop (22);
    irClear.setBounds (brow.removeFromRight (22));
    brow.removeFromRight (4);
    irN3D.setBounds (brow.removeFromRight (40));
    brow.removeFromRight (4);
    irFuma.setBounds (brow.removeFromRight (46));
    brow.removeFromRight (4);
    irFollow.setBounds (brow.removeFromRight (92));
    brow.removeFromRight (4);
    irOrder.setBounds (brow.removeFromRight (78));
    brow.removeFromRight (4);
    irLoad.setBounds (brow);
    hb.removeFromTop (8);
    const int half = (hb.getWidth() - 16) / 2;
    irDry->setBounds (hb.getX(), hb.getY(), half, 26);
    irWet->setBounds (hb.getX() + half + 16, hb.getY(), half, 26);
    hb.removeFromTop (32);
    irEnvelop->setBounds (hb.getX(), hb.getY(), half, 26);
    irDirect->setBounds (hb.getX() + half + 16, hb.getY(), half, 26);
}

void SpaceView::refresh()
{
    const int mode = (int) proc.apvts.getRawParameterValue (RysumProcessor::motionParamId (selected))->load();
    for (int i = 0; i < 7; ++i) motionButtons[(size_t) i].setToggleState (i == mode, juce::dontSendNotification);
    {
        const auto& dp = proc.getDrawnPath (selected);
        pathLoop.setToggleState (! dp.pingPong, juce::dontSendNotification);
        pathPing.setToggleState (dp.pingPong, juce::dontSendNotification);
        pathEven.setToggleState (! dp.drawnTiming, juce::dontSendNotification);
        pathDrawnT.setToggleState (dp.drawnTiming, juce::dontSendNotification);
        const float pathAlpha = mode == (int) Motion::Drawn ? 1.0f : 0.45f;
        for (auto* b : { &pathLoop, &pathPing, &pathEven, &pathDrawnT }) b->setAlpha (pathAlpha);
        if (spreadRow) spreadRow->setAlpha (pathAlpha);
        jumpHard.setAlpha (mode == (int) Motion::Jumps ? 1.0f : 0.45f);
        if (depthRow) depthRow->setAlpha (mode == (int) Motion::Drawn || mode == (int) Motion::Rotation || mode == (int) Motion::Vertical ? 0.45f : 1.0f);
    }
    irDry->repaint();
    irWet->repaint();
    irEnvelop->repaint();
    irDirect->repaint();
    const auto mnames = motionNames();
    for (int s = 0; s < kNumStops; ++s)
    {
        const auto txt = mnames[(int) proc.apvts.getRawParameterValue (RysumProcessor::motionParamId (s))->load()];
        const bool pulled = proc.apvts.getRawParameterValue (RysumProcessor::stopParamId (s))->load() > 0.5f;
        if (stopButtons[(size_t) s]->motion != txt || stopButtons[(size_t) s]->pulled != pulled)
        { stopButtons[(size_t) s]->motion = txt; stopButtons[(size_t) s]->pulled = pulled; stopButtons[(size_t) s]->repaint(); }
    }
    if (rateRow) rateRow->repaint();
    if (depthRow) depthRow->repaint();
    for (auto* r : { ellipseRow.get(), spreadRow.get(), ellDirRow.get(), ellTiltRow.get() }) if (r) r->repaint();
    circle.repaint();
    repaint (right);
}

void SpaceView::paint (juce::Graphics& g)
{
    auto& A = Assets::get();
    g.setColour (col::line);
    g.fillRect (left.getRight() - 1, left.getY(), 1, left.getHeight());

    g.setFont (A.monoFont (11.0f));
    g.setColour (col::mute);
    auto top = left.reduced (28, 0).withTrimmedTop (16).withHeight (16);
    g.drawText (juce::String (juce::CharPointer_UTF8 ("Draufsicht \xc2\xb7 Mitte = Zenit \xc2\xb7 Rand = Horizont \xc2\xb7 gro\xc3\x9f = nah")), top, juce::Justification::centredLeft);
    g.drawText (juce::String (juce::CharPointer_UTF8 ("Punkt ziehen = Pfeife \xc2\xb7 Fl\xc3\xa4" "che ziehen = Register drehen")), top, juce::Justification::centredRight);

    // stop list header + dot legend + motion
    g.setColour (col::mute);
    g.setFont (A.sansFont (11.0f).withExtraKerningFactor (0.18f));
    g.drawText ("REGISTER", stopsBox.withHeight (18), juce::Justification::centredLeft);
    g.setFont (A.monoFont (11.0f));
    g.drawText ("Bewegung", stopsBox.withHeight (18), juce::Justification::centredRight);
    drawSectionTitle (g, layoutBox, "Anordnung " + juce::String (juce::CharPointer_UTF8 ("\xc2\xb7 ")) + prettyLabel (kStops[(size_t) selected].label));
    const char* names[] { "Breite", "Mitte", "H\xc3\xb6he", "Streuung", "Abstand" };
    juce::Slider* sl[] { &width, &centre, &elevation, &spread, &distance };
    for (int i = 0; i < 5; ++i)
    {
        auto b = sl[i]->getBounds();
        g.setFont (A.sansFont (13.0f));
        g.setColour (col::ink);
        g.drawText (juce::String (juce::CharPointer_UTF8 (names[i])), b.withX (right.getX()).withWidth (96), juce::Justification::centredLeft);
        g.setFont (A.monoFont (11.0f));
        g.setColour (col::dim);
        g.drawText (i == 4 ? juce::String (sl[i]->getValue(), 1) + " m"
                           : juce::String (juce::roundToInt (sl[i]->getValue())) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0")),
                    b.withX (b.getRight()).withWidth (52), juce::Justification::centredRight);
    }

    if (! hallTab)
    {
        drawSectionTitle (g, tabBox, "Bewegung");
        g.setFont (A.monoFont (10.0f));
        g.setColour (col::faint);
        g.drawText (u8 ("Tempo negativ = Gegenrichtung"), juce::Rectangle<int> (tabBox.getX() + 120, tabBox.getY() + 9, tabMotion.getX() - 8 - tabBox.getX() - 120, 20),
                    juce::Justification::centredRight);
        // the orbit from above (front up): distance as radius, with the current ellipse settings
        const auto prm = proc.readParams();
        const float e = prm.ellipse[(size_t) selected], dir = prm.ellipseDir[(size_t) selected];
        const int lobes = prm.ellipseLobes[(size_t) selected];
        auto pr = ellPreview.toFloat();
        const auto c = pr.getCentre();
        const float R = juce::jmin (pr.getWidth(), pr.getHeight()) * 0.5f - 2.0f;
        g.setColour (col::line);
        g.drawEllipse (juce::Rectangle<float> (2 * R, 2 * R).withCentre (c), 0.8f);
        juce::Path orbit;
        for (int i = 0; i <= 96; ++i)
        {
            const float az = 360.0f * (float) i / 96.0f;
            const float d = 1.0f - e * std::cos ((float) lobes * juce::degreesToRadians (az - dir));
            const float rr2 = R * juce::jlimit (0.05f, 1.9f, d) / (1.0f + e);
            const float a = juce::degreesToRadians (az);
            const juce::Point<float> q { c.x - rr2 * std::sin (a), c.y - rr2 * std::cos (a) };
            if (i == 0) orbit.startNewSubPath (q); else orbit.lineTo (q);
        }
        g.setColour (col::ink.withAlpha (0.85f));
        g.strokePath (orbit, juce::PathStrokeType (1.2f));
        g.setColour (col::ink);
        g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (c));
        g.setFont (A.monoFont (9.0f));
        g.setColour (col::faint);
        g.drawText ("vorn", pr.withHeight (9.0f).translated (0.0f, -9.0f), juce::Justification::centred);
    }
    else
        drawSectionTitle (g, tabBox, u8 ("Hall"), {});
    if (hallTab)
    {
        g.setFont (A.monoFont (10.0f));
        g.setColour (col::mute);
        g.drawText (proc.readParams().room.on ? juce::String::fromUTF8 ("pausiert \xe2\x80\x93 der Saal ist an") : proc.getIrStatus(),
                    juce::Rectangle<int> (tabBox.getX() + 70, tabBox.getY() + 9, tabMotion.getX() - 8 - tabBox.getX() - 70, 20), juce::Justification::centredRight);
    }
}
} // namespace rysum::ui
