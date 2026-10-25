#include "KlangView.h"

namespace rysum::ui
{
ParamRow& KlangView::row (const char* id, const juce::String& name)
{
    rows.push_back (std::make_unique<ParamRow> (proc.apvts, id, name));
    addAndMakeVisible (*rows.back());
    return *rows.back();
}

juce::TextButton& KlangView::toggle (const char* id, const juce::String& text)
{
    toggles.push_back (std::make_unique<juce::TextButton> (text));
    auto& b = *toggles.back();
    b.setClickingTogglesState (true);
    addAndMakeVisible (b);
    toggleAtts.push_back (std::make_unique<ButtonAtt> (proc.apvts, id, b));
    return b;
}

KlangView::KlangView (RysumProcessor& p) : proc (p)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("temperament")))
        temperament.addItemList (choice->choices, 1);
    temperamentAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "temperament", temperament);
    temperament.setTooltip (u8 ("Die Aufnahmen sind \xc2\xbc-Komma mittelt\xc3\xb6nig; jede andere Stimmung verschiebt die Pfeifen, a\xe2\x80\xb2 bleibt."));
    addAndMakeVisible (temperament);
    pitch  = &row ("pitch", "Stimmton");
    detune = &row ("detune", u8 ("Verstimmung (zuf\xc3\xa4llig je Pfeife)"));

    tremOn    = &toggle ("tremOn", "Tremulant");
    tremRate  = &row ("tremRate", "Tempo");
    tremDepth = &row ("tremDepth", "Tiefe");

    freezeOn = &toggle ("freeze", "Freeze");
    freezeOn->setTooltip (u8 ("An: losgelassene T\xc3\xb6ne frieren als Kornwolke ein. Aus: sie klingen aus. (CC 66)"));
    smear    = &row ("freezeSmear", "Verwischen");
    release  = &row ("freezeRelease", "Ausklang beim Ausschalten");

    doppler    = &row ("doppler", "Doppler");
    distAmount = &row ("distAmount", u8 ("Entfernung \xe2\x86\x92 Lautst\xc3\xa4rke"));
    air        = &row ("air", u8 ("Luft (H\xc3\xb6hen in der Ferne)"));

    midiOn = &toggle ("midiCC", "MIDI-CC an");
    syncOn = &toggle ("motionSync", "Bewegung im Tempo");
    syncOn->setTooltip (u8 ("Geschwindigkeiten gelten bei 120 BPM, laufen mit dem Projekttempo und rasten auf die Taktposition ein."));
}

void KlangView::resized()
{
    auto r = getLocalBounds().reduced (28, 0).withTrimmedTop (22).withTrimmedBottom (22);
    const int gap = 28;
    const int cw = (r.getWidth() - 2 * gap) / 3;
    const int rh = (r.getHeight() - gap) / 2;
    auto cell = [&] (int c, int rr) { return juce::Rectangle<int> (r.getX() + c * (cw + gap), r.getY() + rr * (rh + gap), cw, rh); };
    tuneBox = cell (0, 0); tremBox = cell (1, 0); freezeBox = cell (2, 0);
    distBox = cell (0, 1); midiBox = cell (1, 1); noteBox = cell (2, 1);

    auto body = [] (juce::Rectangle<int> b) { return b.withTrimmedTop (40); };
    {
        auto b = body (tuneBox);
        temperament.setBounds (b.removeFromTop (28));
        b.removeFromTop (12);
        pitch->setBounds (b.removeFromTop (38));
        detune->setBounds (b.removeFromTop (38));
    }
    {
        auto b = body (tremBox);
        tremOn->setBounds (b.removeFromTop (28).withWidth (140));
        b.removeFromTop (12);
        tremRate->setBounds (b.removeFromTop (38));
        tremDepth->setBounds (b.removeFromTop (38));
    }
    {
        auto b = body (freezeBox);
        freezeOn->setBounds (b.removeFromTop (28).withWidth (140));
        b.removeFromTop (12);
        smear->setBounds (b.removeFromTop (38));
        release->setBounds (b.removeFromTop (38));
    }
    {
        auto b = body (distBox);
        doppler->setBounds (b.removeFromTop (38));
        distAmount->setBounds (b.removeFromTop (38));
        air->setBounds (b.removeFromTop (38));
    }
    {
        auto b = body (midiBox);
        auto t = b.removeFromTop (28);
        midiOn->setBounds (t.removeFromLeft (130));
        t.removeFromLeft (8);
        syncOn->setBounds (t.removeFromLeft (170));
    }
}

void KlangView::refresh()
{
    for (auto& r : rows) r->repaint();
    repaint (freezeBox);
}

void KlangView::paint (juce::Graphics& g)
{
    auto& A = Assets::get();
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.setOpacity (0.07f);
        drawCover (g, A.wall, getLocalBounds().toFloat(), 0.6f);
    }
    drawSectionTitle (g, tuneBox, "Stimmung", u8 ("Original: \xc2\xbc-Komma"));
    drawSectionTitle (g, tremBox, "Tremulant", "CC 1 = Tiefe");
    const int fz = proc.engine.frozenVoices.load();
    drawSectionTitle (g, freezeBox, "Freeze", fz > 0 ? juce::String (fz) + " eingefroren" : juce::String ("CC 66"));
    drawSectionTitle (g, distBox, u8 ("Entfernung \xc2\xb7 Doppler"));
    drawSectionTitle (g, midiBox, "MIDI");
    drawSectionTitle (g, noteBox, "Hinweise");

    g.setFont (A.monoFont (11.0f));
    g.setColour (col::dim);
    auto midiText = midiBox.withTrimmedTop (84);
    const char* lines[] {
        "CC 11       Wind (global) / Schweller",
        "CC 20-26    Register an/aus (Praestant ... Trompete)",
        "CC 27-33    Register-Wind",
        "CC 66       Freeze (Sostenuto-Pedal)",
        "CC 1        Tremulant-Tiefe (Modrad)",
        "CC 64       Haltepedal",
    };
    for (auto* l : lines) g.drawText (u8 (l), midiText.removeFromTop (18), juce::Justification::centredLeft);

    auto note = noteBox.withTrimmedTop (44);
    g.setFont (A.sansFont (13.0f));
    g.setColour (col::mute);
    g.drawFittedText (u8 ("Abstand und Ellipse jedes Registers stellst du auf der Seite Raum ein "
                          "(Anordnung \xe2\x86\x92 Abstand, Bewegung \xe2\x86\x92 Ellipse).\n\n"
                          "Doppler entsteht, wenn sich der Abstand \xc3\xa4ndert: mit Ellipse > 0 bei jeder Bewegung "
                          "(z. B. Rotation), bei Drift auch ohne. Spr\xc3\xbcnge haben keinen Doppler.\n\n"
                          "Weit entfernte Pfeifen schicken mehr in den Hall (Seite Raum)."),
                      note, juce::Justification::topLeft, 12);
}
} // namespace rysum::ui
