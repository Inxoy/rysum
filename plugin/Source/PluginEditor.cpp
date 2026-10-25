#include "PluginEditor.h"

using namespace rysum;
using namespace rysum::ui;

static constexpr int kW = 1120, kH = 720, kHeader = 56;

RysumContent::RysumContent (RysumProcessor& p) : proc (p), console (p), space (p), pipes (p)
{
    addAndMakeVisible (console);
    addChildComponent (space);
    addChildComponent (pipes);
    const char* names[] { "Spieltisch", "Raum", "Pfeifen" };
    for (int i = 0; i < 3; ++i)
    {
        tabs[(size_t) i].setButtonText (juce::String (names[i]).toUpperCase());
        tabs[(size_t) i].onClick = [this, i] { showView (i); };
        addAndMakeVisible (tabs[(size_t) i]);
    }
    showView (0);

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
    pipes.setVisible (v == 2);
    for (int i = 0; i < 3; ++i) tabs[(size_t) i].setToggleState (i == v, juce::dontSendNotification);
    if (v == 2) pipes.refresh();
}

void RysumContent::resized()
{
    auto body = getLocalBounds().withTrimmedTop (kHeader);
    console.setBounds (body);
    space.setBounds (body);
    pipes.setBounds (body);
    const int tw[] { 112, 72, 88 };
    int x = 468;
    for (int i = 0; i < 3; ++i) { tabs[(size_t) i].setBounds (x, 13, tw[i], 30); x += tw[i] + 4; }
}

void RysumContent::tick()
{
    if (console.isVisible()) console.refresh();
    if (space.isVisible()) space.refresh();
    if (pipes.isVisible()) pipes.refresh();
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
    g.drawText (u8 ("um 1457 \xc2\xb7 \xc2\xbc-Komma mittelt\xc3\xb6nig \xc2\xb7 a\xe2\x80\xb2 \xe2\x89\x88 444,5 Hz"),
                head.withTrimmedLeft (140).withWidth (320), juce::Justification::centredLeft);

    const int out = (int) proc.apvts.getRawParameterValue ("output")->load();
    juce::String o = out == 0 ? juce::String ("Stereo") : "AmbiX " + juce::String (out) + u8 (". Ord. \xc2\xb7 ") + juce::String ((out + 1) * (out + 1)) + " ch";
    g.setColour (col::dim);
    g.drawText (o, head.withTrimmedLeft (760).withTrimmedRight (110), juce::Justification::centredRight);
    g.setColour (col::faint);
    g.drawText (juce::String (proc.engine.activeVoices.load()) + " St.", head.withTrimmedLeft (980), juce::Justification::centredRight);
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
