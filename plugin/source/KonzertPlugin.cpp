// "Rysum Konzert" as a plugin (VST3): the listener for a venue. It is the app's window inside a host
// such as Reaper, and the host gets every channel the processor makes - AmbiX up to the 5th order
// (36 channels) for the venue's own decoder, or stereo, binaural, loudspeakers as in the app.
// Nothing is played here: the host's MIDI is ignored, there are no inputs (the player's input
// comes over the network).
#include "ListenerView.h"

using namespace rysum::ui;
using namespace rysum::ui::konzert;

namespace
{
class KonzertEditor : public juce::AudioProcessorEditor
{
public:
    explicit KonzertEditor(RysumProcessor &p)
        : juce::AudioProcessorEditor(p), settings(settingsOptions()), view(p, nullptr, settings)
    {
        setLookAndFeel(&lnf.get());
        addAndMakeVisible(view);
        setResizable(true, false);
        setResizeLimits(800, 480, 4000, 3000);
        setSize(1180, 700);
    }
    ~KonzertEditor() override
    {
        settings.saveIfNeeded();
        setLookAndFeel(nullptr);
    }
    void paint(juce::Graphics &g) override
    {
        g.fillAll(col::bg);
    }
    void resized() override
    {
        view.setBounds(getLocalBounds());
    }

private:
    juce::SharedResourcePointer<Assets> assets;
    juce::SharedResourcePointer<LookAndFeel> lnf;
    juce::PropertiesFile settings;
    ListenerMain view;
};
} // namespace

namespace rysum
{
juce::AudioProcessorEditor *createKonzertEditor(RysumProcessor &p)
{
    return new KonzertEditor(p);
}

void setUpKonzertPlugin(RysumProcessor &p)
{
    // a venue decodes itself: AmbiX 5 out, unless the project says otherwise (its saved state wins)
    if (auto *o = p.apvts.getParameter("output"))
        o->setValueNotifyingHost(o->convertTo0to1(5.0f));
    // (tests: RYSUM_KONZERT_JOIN=host:port joins at once, without the window)
    const auto join = juce::SystemStats::getEnvironmentVariable("RYSUM_KONZERT_JOIN", {});
    if (join.isNotEmpty())
        p.concert.startListener(join);
}
} // namespace rysum
