#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace rysum;

// ---------------------------------------------------------------------------
// background sample loading
// ---------------------------------------------------------------------------
class RysumProcessor::Loader : public juce::Thread
{
public:
    Loader (RysumProcessor& p, juce::File f) : juce::Thread ("Rysum sample loader"), owner (p), folder (std::move (f)) {}
    ~Loader() override { stopThread (10000); }

    void run() override
    {
        juce::String err;
        auto lib = SampleLibrary::load (folder, err, [this] { return threadShouldExit(); });
        if (threadShouldExit()) return;
        std::shared_ptr<const SampleLibrary> shared (lib.release());
        juce::MessageManager::callAsync ([proc = &owner, shared, err, folder = folder, alive = owner.aliveFlag]
        {
            if (! alive->load()) return;
            proc->onLoaded (shared, err, folder);
        });
    }

private:
    RysumProcessor& owner;
    juce::File folder;
};

// ---------------------------------------------------------------------------
juce::AudioProcessorValueTreeState::ParameterLayout RysumProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    auto pct = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (juce::roundToInt (v * 100)) + " %"; });

    for (int s = 0; s < kNumStops; ++s)
        l.add (std::make_unique<AudioParameterBool> (ParameterID { stopParamId (s), 1 }, prettyLabel (kStops[(size_t) s].label), s == 0));
    for (int s = 0; s < kNumStops; ++s)
    {
        const String name = prettyLabel (kStops[(size_t) s].label);
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { volParamId (s), 1 }, name + u8 (" Lautst\xc3\xa4rke"),
                                                      NormalisableRange<float> (-48.0f, 12.0f, 0.1f), 0.0f,
                                                      AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v > 0 ? "+" : "") + String (v, 1) + " dB"; })));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { windParamId (s), 1 }, name + " Wind", NormalisableRange<float> (0, 1), 1.0f, pct));
    }

    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "master", 1 }, u8 ("Lautst\xc3\xa4rke"), NormalisableRange<float> (-60.0f, 12.0f, 0.1f), -12.0f,
                                                  AudioParameterFloatAttributes().withLabel ("dB")));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "output", 1 }, "Ausgabe",
                                                   StringArray { "Stereo", "AmbiX 1. Ordnung", "AmbiX 2. Ordnung", "AmbiX 3. Ordnung", "AmbiX 4. Ordnung", "AmbiX 5. Ordnung" }, 0));

    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "click", 1 },        "Tastenklick",        NormalisableRange<float> (0, 1), 0.65f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "clickBlend", 1 },   "Klick ins Anschlagen", NormalisableRange<float> (0, 1), 0.45f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "releaseClick", 1 }, "Loslassklick",       NormalisableRange<float> (0, 1), 0.40f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "stopPull", 1 },     "Registerzug",        NormalisableRange<float> (0, 1), 0.70f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "wind", 1 },         "Wind",               NormalisableRange<float> (0, 1), 0.15f, pct));

    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "degAmount", 1 },   "Verfall Menge",     NormalisableRange<float> (0, 1), 0.0f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "degGrain", 1 },    u8 ("K\xc3\xb6rnung"),          NormalisableRange<float> (0, 1), 0.5f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "degBits", 1 },     "Bittiefe",          NormalisableRange<float> (3.0f, 16.0f, 0.1f), 12.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " bit"; })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "degRate", 1 },     "Abtastrate",        NormalisableRange<float> (600.0f, 48000.0f, 1.0f, 0.35f), 22050.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v >= 1000 ? String (v / 1000.0f, 2) + " kHz" : String (roundToInt (v)) + " Hz"; })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "degWobble", 1 },   "Windschwankung",    NormalisableRange<float> (0, 1), 0.2f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "degDropouts", 1 }, "Aussetzer",         NormalisableRange<float> (0, 1), 0.08f, pct));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "degMode", 1 },    "Verfall auf",       StringArray { "pro Pfeife", "Summe" }, 0));

    for (int s = 0; s < kNumStops; ++s)
    {
        const String name = prettyLabel (kStops[(size_t) s].label);
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { motionParamId (s), 1 }, name + " Bewegung", motionNames(), 0));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { rateParamId (s), 1 }, name + " Geschwindigkeit",
                                                      NormalisableRange<float> (0.005f, 2.0f, 0.001f, 0.35f), 0.08f,
                                                      AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 3) + " Hz"; })));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { depthParamId (s), 1 }, name + " Tiefe",
                                                      NormalisableRange<float> (0.0f, 180.0f, 0.1f), 40.0f,
                                                      AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0")); })));
    }
    return l;
}

RysumProcessor::RysumProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::ambisonic (kMaxOrder), true)),
      apvts (*this, nullptr, "RYSUM", createLayout())
{
    for (int s = 0; s < kNumStops; ++s)
    {
        stopParams[(size_t) s]   = apvts.getRawParameterValue (stopParamId (s));
        volParams[(size_t) s]    = apvts.getRawParameterValue (volParamId (s));
        windParams[(size_t) s]   = apvts.getRawParameterValue (windParamId (s));
        motionParams[(size_t) s] = apvts.getRawParameterValue (motionParamId (s));
        rateParams[(size_t) s]   = apvts.getRawParameterValue (rateParamId (s));
        depthParams[(size_t) s]  = apvts.getRawParameterValue (depthParamId (s));
        layouts[(size_t) s] = defaultLayout (s);
    }
    master = apvts.getRawParameterValue ("master");
    output = apvts.getRawParameterValue ("output");
    click = apvts.getRawParameterValue ("click");
    clickBlend = apvts.getRawParameterValue ("clickBlend");
    releaseClick = apvts.getRawParameterValue ("releaseClick");
    stopPull = apvts.getRawParameterValue ("stopPull");
    wind = apvts.getRawParameterValue ("wind");
    degAmount = apvts.getRawParameterValue ("degAmount");
    degGrain = apvts.getRawParameterValue ("degGrain");
    degBits = apvts.getRawParameterValue ("degBits");
    degRate = apvts.getRawParameterValue ("degRate");
    degWobble = apvts.getRawParameterValue ("degWobble");
    degDropouts = apvts.getRawParameterValue ("degDropouts");
    degMode = apvts.getRawParameterValue ("degMode");

    startTimer (250);   // first tick: find + load the samples (after a possible setStateInformation)
}

RysumProcessor::~RysumProcessor()
{
    aliveFlag->store (false);
    stopTimer();
    loader.reset();
}

bool RysumProcessor::isBusesLayoutSupported (const BusesLayout& busLayout) const
{
    if (busLayout.getMainInputChannels() > 0) return false;
    const int n = busLayout.getMainOutputChannels();
    return n >= 1 && n <= kMaxOutChannels;
}

void RysumProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    keyboardState.reset();
}

EngineParams RysumProcessor::readParams() const
{
    EngineParams p;
    for (int s = 0; s < kNumStops; ++s)
    {
        p.stopOn[(size_t) s] = stopParams[(size_t) s]->load() > 0.5f;
        p.stopGain[(size_t) s] = dbToGain (volParams[(size_t) s]->load());
        p.stopWind[(size_t) s] = windParams[(size_t) s]->load();
        p.motionMode[(size_t) s] = (int) motionParams[(size_t) s]->load();
        p.motionRate[(size_t) s] = rateParams[(size_t) s]->load();
        p.motionDepth[(size_t) s] = depthParams[(size_t) s]->load();
    }
    p.masterGain = dbToGain (master->load());
    p.outputMode = (int) output->load();
    p.clickLevel = click->load();
    p.clickBlend = clickBlend->load();
    p.releaseClick = releaseClick->load();
    p.stopPull = stopPull->load();
    p.wind = wind->load();
    p.deg.amount = degAmount->load();
    p.deg.grain = degGrain->load();
    p.deg.bits = degBits->load();
    p.deg.rateHz = degRate->load();
    p.deg.wobble = degWobble->load();
    p.deg.dropouts = degDropouts->load();
    p.deg.perPipe = degMode->load() < 0.5f;
    return p;
}

void RysumProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    if (auto pending = std::atomic_exchange (&pendingLibrary, std::shared_ptr<const SampleLibrary>()))
        engine.setLibrary (std::move (pending));      // the old one is still referenced by retiredLibrary

    keyboardState.processNextMidiBuffer (midi, 0, buffer.getNumSamples(), true);
    engine.process (buffer, midi, readParams());
    midi.clear();
}

// ---------------------------------------------------------------------------
// samples
// ---------------------------------------------------------------------------
void RysumProcessor::loadSamples (const juce::File& folder)
{
    loader.reset();
    samplesFolder = folder;
    loading = true;
    setStatus ("Lade Samples aus " + folder.getFullPathName() + " ...");
    loader = std::make_unique<Loader> (*this, folder);
    loader->startThread();
}

void RysumProcessor::onLoaded (std::shared_ptr<const SampleLibrary> lib, const juce::String& err, const juce::File& folder)
{
    loading = false;
    if (lib == nullptr)
    {
        setStatus ("Keine Samples: " + err);
        ++changeCounter;
        return;
    }
    retiredLibrary = std::atomic_load (&uiLibrary);
    retireCountdown = 20;                                 // ~5 s, then freed on the message thread
    std::atomic_store (&uiLibrary, lib);
    std::atomic_store (&pendingLibrary, lib);
    samplesFolder = folder;
    juce::String s;
    s << lib->loadedPipes << " Pfeifen geladen";
    if (lib->substitutedPipes > 0) s << ", " << lib->substitutedPipes << " ersetzt";
    setStatus (s);
    ++changeCounter;
}

void RysumProcessor::startAutoLoad()
{
    if (samplesFolder != juce::File() && SampleLibrary::looksLikeSampleFolder (samplesFolder))
    {
        loadSamples (samplesFolder);
        return;
    }
    for (auto& f : SampleLibrary::candidateFolders())
        if (SampleLibrary::looksLikeSampleFolder (f)) { loadSamples (f); return; }
    setStatus (u8 ("Samples-Ordner nicht gefunden \xe2\x80\x93 bitte w\xc3\xa4hlen"));
    ++changeCounter;
}

void RysumProcessor::timerCallback()
{
    if (! autoLoadDone) { autoLoadDone = true; startAutoLoad(); }
    if (retireCountdown > 0 && --retireCountdown == 0) retiredLibrary.reset();
}

// ---------------------------------------------------------------------------
// layouts / positions
// ---------------------------------------------------------------------------
void RysumProcessor::applyLayout (int stop)
{
    std::array<Pos, kNumKeys> pos {};
    generateLayout (layouts[(size_t) stop], stop, pos);
    for (int k = 0; k < kNumKeys; ++k) setPipePosition (stop, k, pos[(size_t) k]);
}

void RysumProcessor::setPipePosition (int stop, int key, Pos p)
{
    engine.baseAz[(size_t) stop][(size_t) key] = wrapAz (p.az);
    engine.baseEl[(size_t) stop][(size_t) key] = juce::jlimit (-90.0f, 90.0f, p.el);
    ++changeCounter;
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
void RysumProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    juce::ValueTree extra ("EXTRA");
    extra.setProperty ("samplesFolder", samplesFolder.getFullPathName(), nullptr);
    for (int s = 0; s < kNumStops; ++s)
    {
        juce::ValueTree st ("STOP");
        st.setProperty ("id", kStops[(size_t) s].id, nullptr);
        const auto& L = layouts[(size_t) s];
        st.setProperty ("layout", L.type, nullptr);
        st.setProperty ("width", L.width, nullptr);
        st.setProperty ("center", L.center, nullptr);
        st.setProperty ("elevation", L.elevation, nullptr);
        st.setProperty ("spread", L.spread, nullptr);
        juce::StringArray az, el, mu;
        for (int k = 0; k < kNumKeys; ++k)
        {
            az.add (juce::String (engine.baseAz[(size_t) s][(size_t) k].load(), 2));
            el.add (juce::String (engine.baseEl[(size_t) s][(size_t) k].load(), 2));
            mu.add (engine.muted[(size_t) s][(size_t) k].load() ? "1" : "0");
        }
        st.setProperty ("az", az.joinIntoString (","), nullptr);
        st.setProperty ("el", el.joinIntoString (","), nullptr);
        st.setProperty ("mute", mu.joinIntoString (","), nullptr);
        extra.appendChild (st, nullptr);
    }
    state.removeChild (state.getChildWithName ("EXTRA"), nullptr);
    state.appendChild (extra, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, destData);
}

void RysumProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr) return;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.hasType (apvts.state.getType())) return;
    auto extra = state.getChildWithName ("EXTRA");
    apvts.replaceState (state);

    if (extra.isValid())
    {
        for (auto st : extra)
        {
            const int s = stopIndexForId (st["id"].toString());
            if (s < 0) continue;
            auto& L = layouts[(size_t) s];
            L.type = (int) st.getProperty ("layout", L.type);
            L.width = (float) st.getProperty ("width", L.width);
            L.center = (float) st.getProperty ("center", L.center);
            L.elevation = (float) st.getProperty ("elevation", L.elevation);
            L.spread = (float) st.getProperty ("spread", L.spread);
            juce::StringArray az, el, mu;
            az.addTokens (st["az"].toString(), ",", {});
            el.addTokens (st["el"].toString(), ",", {});
            mu.addTokens (st["mute"].toString(), ",", {});
            for (int k = 0; k < kNumKeys; ++k)
            {
                if (k < az.size()) engine.baseAz[(size_t) s][(size_t) k] = az[k].getFloatValue();
                if (k < el.size()) engine.baseEl[(size_t) s][(size_t) k] = el[k].getFloatValue();
                if (k < mu.size()) engine.muted[(size_t) s][(size_t) k] = mu[k] == "1";
            }
        }
        const juce::File f (extra["samplesFolder"].toString());
        if (f != juce::File() && f != samplesFolder && SampleLibrary::looksLikeSampleFolder (f))
        {
            samplesFolder = f;
            if (autoLoadDone) loadSamples (f);       // otherwise the auto-load picks it up
        }
    }
    ++changeCounter;
}

juce::AudioProcessorEditor* RysumProcessor::createEditor() { return new RysumEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RysumProcessor(); }
