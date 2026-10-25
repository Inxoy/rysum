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
    const int tw[] { 108, 66, 70, 108 };
    int x = 418;
    for (int i = 0; i < 4; ++i) { tabs[(size_t) i].setBounds (x, 13, tw[i], 30); x += tw[i] + 4; }
    outputChoice.setBounds (kW - 28 - 60 - 170, 14, 170, 28);
}

void RysumContent::tick()
{
    if (console.isVisible()) console.refresh();
    if (space.isVisible()) space.refresh();
    if (klang.isVisible()) klang.refresh();
    if (mod.isVisible()) mod.refresh();
    repaint (0, 0, kW, kHeader);
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
    g.setColour (col::mute);
    g.setFont (A.monoFont (11.0f));
    g.drawText (u8 ("\xc2\xbc-Komma mittelt\xc3\xb6nig \xc2\xb7 a\xe2\x80\xb2 \xe2\x89\x88 444,5 Hz"),
                head.withTrimmedLeft (140).withWidth (240), juce::Justification::centredLeft);

    const int out = (int) proc.apvts.getRawParameterValue ("output")->load();
    const int needed = out == 0 ? 2 : (out + 1) * (out + 1);
    const int have = proc.getTotalNumOutputChannels();
    juce::String o = juce::String (needed) + " ch";
    if (have < needed) o = u8 ("nur ") + juce::String (have) + " ch!";
    g.setColour (have < needed ? col::white : col::faint);
    g.drawText (o, juce::Rectangle<int> (kW - 28 - 60 - 170 - 82, 0, 74, kHeader), juce::Justification::centredRight);
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
        m.addItem (-1, u8 ("Schalter/Auswahl â nicht modulierbar"), false);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
        return;
    }
    bool free = false;
    for (int r = 0; r < kNumRoutes; ++r) free = free || (proc.getRouteSrc (r) < 0 && proc.getRouteTgt (r) < 0);
    juce::PopupMenu with;
    const auto names = modSourceNames();
    for (int s = 0; s < kNumSources; ++s) with.addItem (1 + s, names[s]);
    m.addSubMenu (free ? juce::String ("Modulieren mit") : u8 ("Modulieren mit (alle 12 PlÃ¤tze belegt)"), with, free);
    for (int r = 0; r < kNumRoutes; ++r)
        if (proc.getRouteTgt (r) == kGlobalTargetBase + idx && proc.getRouteSrc (r) >= 0)
        {
            auto* a = proc.apvts.getParameter (proc.getModAmountId (r));
            m.addItem (100 + r, "Entfernen: " + names[proc.getRouteSrc (r)] + " (" + (a != nullptr ? a->getCurrentValueAsText() : juce::String()) + ")");
        }
    m.addSeparator();
    m.addItem (300, "Modulationsseite " + juce::String (u8 ("Ã¶")) + "ffnen");
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
