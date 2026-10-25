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
                const auto pos = proc.engine.currentPosition (s, k, params);
                const auto p = toScreen (pos);
                const bool snd = (sounding >> k) & 1;
                if (snd)
                {
                    g.setColour (col::white.withAlpha (0.8f));
                    g.drawEllipse (juce::Rectangle<float> (15.0f, 15.0f).withCentre (p), 1.0f);
                }
                // nearer pipes are drawn bigger
                const float near = juce::jlimit (0.55f, 1.8f, std::sqrt (6.0f / pos.dist));
                drawStopDot (g, s, p, (isSel ? 7.0f : 5.0f) * near, isSel && snd);
            }
        }

    // listener in the centre
    g.setColour (col::ink);
    g.drawEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (c), 1.0f);
}

void SpaceCircle::mouseDown (const juce::MouseEvent& e)
{
    const auto params = proc.readParams();
    dragKey = -1;
    float best = 10.0f;
    for (int k = 0; k < kNumKeys; ++k)
    {
        const float d = toScreen (proc.engine.currentPosition (selected, k, params)).getDistanceFrom (e.position);
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

void SpaceCircle::mouseUp (const juce::MouseEvent&) { dragKey = -1; rotating = false; }

// ---------------------------------------------------------------------------
SpaceView::SpaceView (RysumProcessor& p) : proc (p), circle (p, selected)
{
    addAndMakeVisible (circle);

    for (int s = 0; s < kNumStops; ++s)
    {
        stopButtons.push_back (std::make_unique<StopRow> (s));
        stopButtons.back()->onClick = [this, s] { selectStop (s); };
        addAndMakeVisible (*stopButtons.back());
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
    for (int i = 0; i < 6; ++i)
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
    addAndMakeVisible (*irDry);
    addAndMakeVisible (*irWet);

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
}

void SpaceView::selectStop (int s)
{
    selected = s;
    for (int i = 0; i < kNumStops; ++i) stopButtons[(size_t) i]->setToggleState (i == s, juce::dontSendNotification);
    rateRow = std::make_unique<ParamRow> (proc.apvts, RysumProcessor::rateParamId (s), "Geschwindigkeit");
    depthRow = std::make_unique<ParamRow> (proc.apvts, RysumProcessor::depthParamId (s), "Tiefe", true);
    ellipseRow = std::make_unique<ParamRow> (proc.apvts, RysumProcessor::ellipseParamId (s), u8 ("Ellipse (n\xc3\xa4her vorn)"), true);
    addAndMakeVisible (*rateRow);
    addAndMakeVisible (*depthRow);
    addAndMakeVisible (*ellipseRow);
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
    stopsBox = rr.removeFromTop (20 + 7 * 20);
    auto sb = stopsBox.withTrimmedTop (20);
    for (auto& b : stopButtons) b->setBounds (sb.removeFromTop (20).reduced (0, 1));
    rr.removeFromTop (8);

    layoutBox = rr.removeFromTop (32 + 48 + 5 * 20);
    auto lb = layoutBox.withTrimmedTop (32);
    auto row1 = lb.removeFromTop (22), row2 = lb.removeFromTop (22);
    lb.removeFromTop (4);
    const int bw1 = (row1.getWidth() - 8) / 3;
    for (int i = 0; i < 3; ++i) layoutButtons[(size_t) i].setBounds (row1.getX() + i * (bw1 + 4), row1.getY(), bw1, 20);
    for (int i = 0; i < 3; ++i) layoutButtons[(size_t) i + 3].setBounds (row2.getX() + i * (bw1 + 4), row2.getY(), bw1, 20);
    for (auto* s : { &width, &centre, &elevation, &spread, &distance })
        s->setBounds (lb.removeFromTop (20).withTrimmedLeft (100).withTrimmedRight (52));
    rr.removeFromTop (8);

    motionBox = rr.removeFromTop (32 + 44 + 4 + 34 + 28);
    auto mb = motionBox.withTrimmedTop (32);
    auto m1 = mb.removeFromTop (22), m2 = mb.removeFromTop (22);
    const int bw2 = (m1.getWidth() - 8) / 3;
    for (int i = 0; i < 3; ++i) motionButtons[(size_t) i].setBounds (m1.getX() + i * (bw2 + 4), m1.getY(), bw2, 20);
    for (int i = 0; i < 3; ++i) motionButtons[(size_t) i + 3].setBounds (m2.getX() + i * (bw2 + 4), m2.getY(), bw2, 20);
    mb.removeFromTop (4);
    if (rateRow)  rateRow->setBounds (mb.removeFromTop (34));
    {
        auto half = mb.removeFromTop (26);
        const int hw = (half.getWidth() - 16) / 2;
        if (depthRow)   depthRow->setBounds (half.getX(), half.getY(), hw, 26);
        if (ellipseRow) ellipseRow->setBounds (half.getX() + hw + 16, half.getY(), hw, 26);
    }
    rr.removeFromTop (8);

    hallBox = rr;
    auto hb = hallBox.withTrimmedTop (32);
    auto brow = hb.removeFromTop (22);
    irClear.setBounds (brow.removeFromRight (26));
    brow.removeFromRight (4);
    irFuma.setBounds (brow.removeFromRight (60));
    brow.removeFromRight (4);
    irLoad.setBounds (brow);
    hb.removeFromTop (6);
    const int half = (hb.getWidth() - 16) / 2;
    irDry->setBounds (hb.getX(), hb.getY(), half, 26);
    irWet->setBounds (hb.getX() + half + 16, hb.getY(), half, 26);
}

void SpaceView::refresh()
{
    const int mode = (int) proc.apvts.getRawParameterValue (RysumProcessor::motionParamId (selected))->load();
    for (int i = 0; i < 6; ++i) motionButtons[(size_t) i].setToggleState (i == mode, juce::dontSendNotification);
    irDry->repaint();
    irWet->repaint();
    const auto mnames = motionNames();
    for (int s = 0; s < kNumStops; ++s)
    {
        const auto txt = mnames[(int) proc.apvts.getRawParameterValue (RysumProcessor::motionParamId (s))->load()];
        if (stopButtons[(size_t) s]->motion != txt) { stopButtons[(size_t) s]->motion = txt; stopButtons[(size_t) s]->repaint(); }
    }
    if (rateRow) rateRow->repaint();
    if (depthRow) depthRow->repaint();
    if (ellipseRow) ellipseRow->repaint();
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

    drawSectionTitle (g, motionBox, "Bewegung", u8 ("Tempo negativ = Gegenrichtung"));
    drawSectionTitle (g, hallBox, u8 ("Hall"), proc.getIrStatus());
}
} // namespace rysum::ui
