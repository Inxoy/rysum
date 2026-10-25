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

    auto dbAttr = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v <= -59.9f ? String ("aus") : (v > 0 ? "+" : "") + String (v, 1) + " dB"; });
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "irDry", 1 }, "Direkt", NormalisableRange<float> (-60.0f, 6.0f, 0.1f, 2.0f), 0.0f, dbAttr));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "irWet", 1 }, "Hall",   NormalisableRange<float> (-60.0f, 12.0f, 0.1f, 2.0f), -6.0f, dbAttr));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "irFuma", 1 }, "IR FuMa", false));

    // distance
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "doppler", 1 }, "Doppler", NormalisableRange<float> (0.0f, 4.0f, 0.01f), 1.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v < 0.005f ? String ("aus") : String (v, 2) + juce::String::fromUTF8 (" \xc3\x97"); })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "air", 1 }, "Luft", NormalisableRange<float> (0, 1), 0.5f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "distAmount", 1 }, "Entfernung", NormalisableRange<float> (0, 1), 0.7f, pct));

    // tuning
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "temperament", 1 }, "Stimmung", temperamentNames(), 0));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "pitch", 1 }, "Stimmton", NormalisableRange<float> (-200.0f, 200.0f, 0.1f), 0.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                                                  { return juce::String::fromUTF8 ("a\xe2\x80\xb2 = ") + String (kA4 * std::pow (2.0, v / 1200.0), 1) + " Hz"; })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "detune", 1 }, "Verstimmung", NormalisableRange<float> (0.0f, 60.0f, 0.1f, 0.6f), 0.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return juce::String::fromUTF8 ("\xc2\xb1") + String (v, 1) + " ct"; })));

    // tremulant
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "tremOn", 1 }, "Tremulant", false));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "tremRate", 1 }, "Tremulant Tempo", NormalisableRange<float> (1.5f, 12.0f, 0.01f, 0.7f), 5.5f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 2) + " Hz"; })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "tremDepth", 1 }, "Tremulant Tiefe", NormalisableRange<float> (0, 1), 0.5f, pct));

    // freeze
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "freeze", 1 }, "Freeze", false));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "freezeSmear", 1 }, "Verwischen", NormalisableRange<float> (0, 1), 0.5f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "freezeRelease", 1 }, "Freeze Ausklang", NormalisableRange<float> (0.1f, 12.0f, 0.01f, 0.5f), 2.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " s"; })));

    // MIDI / tempo
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "motionSync", 1 }, "Bewegung im Tempo", false));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "midiCC", 1 }, "MIDI-CC", true));

    for (int s = 0; s < kNumStops; ++s)
    {
        const String name = prettyLabel (kStops[(size_t) s].label);
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { motionParamId (s), 1 }, name + " Bewegung", motionNames(), 0));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { rateParamId (s), 1 }, name + " Geschwindigkeit",
                                                      NormalisableRange<float> (-10.0f, 10.0f, 0.001f, 0.3f, true), 0.08f,
                                                      AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v > 0 ? "+" : "") + String (v, std::abs (v) < 1.0f ? 3 : 2) + " Hz"; })));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { ellipseParamId (s), 1 }, name + " Ellipse",
                                                      NormalisableRange<float> (0.0f, 0.9f, 0.001f), 0.0f, pct));
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
        ellipseParams[(size_t) s] = apvts.getRawParameterValue (ellipseParamId (s));
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
    irDry = apvts.getRawParameterValue ("irDry");
    irWet = apvts.getRawParameterValue ("irWet");
    irFumaParam = apvts.getRawParameterValue ("irFuma");
    for (auto* id : { "doppler", "air", "distAmount", "temperament", "pitch", "detune", "tremOn", "tremRate", "tremDepth",
                      "freeze", "freezeSmear", "freezeRelease", "motionSync", "midiCC" })
        extraParams[id] = apvts.getRawParameterValue (id);

    // MIDI CC targets (see handleCc)
    ccTargets.push_back (apvts.getParameter ("wind"));
    for (int s = 0; s < kNumStops; ++s) ccTargets.push_back (apvts.getParameter (stopParamId (s)));
    for (int s = 0; s < kNumStops; ++s) ccTargets.push_back (apvts.getParameter (windParamId (s)));
    ccTargets.push_back (apvts.getParameter ("freeze"));
    ccTargets.push_back (apvts.getParameter ("tremDepth"));
    for (auto& c : ccPending) c = std::numeric_limits<float>::quiet_NaN();

    startTimer (250);   // first tick: find + load the samples (after a possible setStateInformation)
}

RysumProcessor::~RysumProcessor()
{
    aliveFlag->store (false);
    stopTimer();
    loader.reset();
    irPool.removeAllJobs (true, 10000);
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
    irInput.assign ((size_t) juce::jmax (512, samplesPerBlock), 0.0f);
    const bool rateChanged = std::abs (sampleRate - hostRate.load()) > 0.5;
    hostRate = sampleRate;
    if (irActive != nullptr) irActive->reset();
    if (rateChanged && irFile.existsAsFile())
        juce::MessageManager::callAsync ([this, alive = aliveFlag] { if (alive->load()) loadIr (irFile); });   // resample IR
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
        p.ellipse[(size_t) s] = ellipseParams[(size_t) s]->load();
    }
    auto x = [this] (const char* id) { return extraParams.at (id)->load(); };
    p.doppler = x ("doppler");
    p.air = x ("air");
    p.distAmount = x ("distAmount");
    p.tuneCents = temperamentShift ((int) x ("temperament"));
    p.pitchCents = x ("pitch");
    p.detune = x ("detune");
    p.tremOn = x ("tremOn") > 0.5f;
    p.tremRate = x ("tremRate");
    p.tremDepth = x ("tremDepth");
    p.freeze = x ("freeze") > 0.5f;
    p.freezeSmear = x ("freezeSmear");
    p.freezeRelease = x ("freezeRelease");
    p.sync = x ("motionSync") > 0.5f;
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

    // MIDI controllers -> parameters (applied right away, the parameter follows on the message thread)
    if (extraParams.at ("midiCC")->load() > 0.5f)
    {
        bool any = false;
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            if (! m.isController()) continue;
            const int cc = m.getControllerNumber(), v = m.getControllerValue();
            int target = -1; float value = 0.0f;
            if (cc == 11)                  { target = 0; value = v / 127.0f; }                       // expression -> global wind
            else if (cc >= 20 && cc <= 26) { target = 1 + (cc - 20); value = v >= 64 ? 1.0f : 0.0f; } // stops on / off
            else if (cc >= 27 && cc <= 33) { target = 8 + (cc - 27); value = v / 127.0f; }           // stop wind
            else if (cc == 66)             { target = 15; value = v >= 64 ? 1.0f : 0.0f; }           // sostenuto -> freeze
            else if (cc == 1)              { target = 16; value = v / 127.0f; }                      // mod wheel -> tremulant depth
            if (target >= 0) { ccPending[(size_t) target] = value; any = true; }
        }
        if (any) triggerAsyncUpdate();
    }

    auto params = readParams();
    applyCcOverrides (params);

    // host tempo / position for tempo-synced movement
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm()) params.bpm = *bpm;
            if (auto ppq = pos->getPpqPosition()) params.ppq = *ppq;
            params.playing = pos->getIsPlaying() && pos->getPpqPosition().hasValue();
        }

    engine.process (buffer, midi, params);
    midi.clear();

    // ---- room: dry level + convolution with the loaded impulse response
    if (irClearRequest.exchange (false)) irActive.reset();          // still referenced by retiredIr
    if (auto pending = std::atomic_exchange (&pendingIr, std::shared_ptr<rysum::Convolver>()))
        irActive = std::move (pending);

    const int n = buffer.getNumSamples();
    const bool stereo = params.outputMode == 0;
    const int used = juce::jmin (buffer.getNumChannels(), stereo ? 2 : (params.outputMode + 1) * (params.outputMode + 1));
    const float dryTarget = irDry->load() <= -59.9f ? 0.0f : dbToGain (irDry->load());
    const float wetTarget = irWet->load() <= -59.9f ? 0.0f : dbToGain (irWet->load());

    if (irActive != nullptr && used > 0)
    {
        // mono feed for the room: all pipes, far pipes relatively louder (engine room send); wet level ramped here
        const float* sendAll = engine.getRoomSend();
        for (int start = 0; start < n; start += (int) irInput.size())
        {
            const int m = juce::jmin ((int) irInput.size(), n - start);
            const float* a = sendAll + start;
            for (int i = 0; i < m; ++i)
            {
                const float g = wetSmoothed + (wetTarget - wetSmoothed) * (float) (start + i) / (float) n;
                irInput[(size_t) i] = a[i] * g;
            }
            // apply the dry level to this chunk before the wet signal is added
            const float d0 = drySmoothed + (dryTarget - drySmoothed) * (float) start / (float) n;
            const float d1 = drySmoothed + (dryTarget - drySmoothed) * (float) (start + m) / (float) n;
            for (int c = 0; c < used; ++c) buffer.applyGainRamp (c, start, m, d0, d1);
            float* outs[rysum::kMaxOutChannels] {};
            for (int c = 0; c < used; ++c) outs[c] = buffer.getWritePointer (c, start);
            irActive->process (irInput.data(), m, outs, used, stereo, 1.0f);
        }
    }
    else if (std::abs (dryTarget - 1.0f) > 1.0e-6f || std::abs (drySmoothed - 1.0f) > 1.0e-6f)
    {
        for (int c = 0; c < used; ++c) buffer.applyGainRamp (c, 0, n, drySmoothed, dryTarget);
    }
    drySmoothed = dryTarget;
    wetSmoothed = wetTarget;
}

// ---------------------------------------------------------------------------
// impulse response
// ---------------------------------------------------------------------------
void RysumProcessor::applyCcOverrides (EngineParams& p) const
{
    auto get = [this] (int i) { return ccPending[(size_t) i].load(); };
    float v;
    if (! std::isnan (v = get (0))) p.wind = v;
    for (int s = 0; s < kNumStops; ++s)
    {
        if (! std::isnan (v = get (1 + s))) p.stopOn[(size_t) s] = v > 0.5f;
        if (! std::isnan (v = get (8 + s))) p.stopWind[(size_t) s] = v;
    }
    if (! std::isnan (v = get (15))) p.freeze = v > 0.5f;
    if (! std::isnan (v = get (16))) p.tremDepth = v;
}

void RysumProcessor::handleAsyncUpdate()
{
    for (size_t i = 0; i < ccTargets.size(); ++i)
    {
        const float v = ccPending[i].exchange (std::numeric_limits<float>::quiet_NaN());
        if (std::isnan (v) || ccTargets[i] == nullptr) continue;
        ccTargets[i]->beginChangeGesture();
        ccTargets[i]->setValueNotifyingHost (ccTargets[i]->convertTo0to1 (v));
        ccTargets[i]->endChangeGesture();
    }
}

void RysumProcessor::loadIr (const juce::File& file)
{
    irFile = file;
    const bool fuma = irFumaParam->load() > 0.5f;
    lastFuma = fuma;
    const double rate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
    setIrStatus (u8 ("lade ") + file.getFileName() + u8 (" \xe2\x80\xa6"));
    irPool.addJob ([this, file, fuma, rate, alive = aliveFlag]
    {
        juce::String err;
        std::shared_ptr<rysum::Convolver> cv (rysum::Convolver::create (file, rate, fuma ? rysum::Convolver::Format::FuMa
                                                                                        : rysum::Convolver::Format::AmbiX, err).release());
        juce::MessageManager::callAsync ([this, cv, err, file, alive]
        {
            if (! alive->load() || file != irFile) return;
            if (cv == nullptr) { setIrStatus (err); return; }
            retiredIr = irUi;
            retireCountdown = 20;
            irUi = cv;
            std::atomic_store (&pendingIr, cv);
            const auto& in = cv->getInfo();
            setIrStatus (in.fileName + u8 (" \xc2\xb7 ") + in.description);
        });
    });
}

void RysumProcessor::clearIr()
{
    irFile = juce::File();
    retiredIr = irUi;
    retireCountdown = 20;
    irUi.reset();
    std::atomic_store (&pendingIr, std::shared_ptr<rysum::Convolver>());
    irClearRequest = true;
    setIrStatus (u8 ("keine \xe2\x80\x93 nur trocken"));
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
    if (! autoLoadDone)
    {
        autoLoadDone = true;
        startAutoLoad();
        if (irFile.existsAsFile()) loadIr (irFile);
    }
    if (retireCountdown > 0 && --retireCountdown == 0) { retiredLibrary.reset(); retiredIr.reset(); }
    const bool fuma = irFumaParam->load() > 0.5f;
    if (fuma != lastFuma && irFile.existsAsFile()) loadIr (irFile);        // FuMa switched: re-read the IR
    lastFuma = fuma;
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
    engine.baseDist[(size_t) stop][(size_t) key] = juce::jlimit (kMinDist, kMaxDist, p.dist);
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
    extra.setProperty ("irFile", irFile.getFullPathName(), nullptr);
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
        juce::StringArray az, el, mu, di;
        for (int k = 0; k < kNumKeys; ++k)
        {
            di.add (juce::String (engine.baseDist[(size_t) s][(size_t) k].load(), 2));
            az.add (juce::String (engine.baseAz[(size_t) s][(size_t) k].load(), 2));
            el.add (juce::String (engine.baseEl[(size_t) s][(size_t) k].load(), 2));
            mu.add (engine.muted[(size_t) s][(size_t) k].load() ? "1" : "0");
        }
        st.setProperty ("az", az.joinIntoString (","), nullptr);
        st.setProperty ("el", el.joinIntoString (","), nullptr);
        st.setProperty ("mute", mu.joinIntoString (","), nullptr);
        st.setProperty ("dist", di.joinIntoString (","), nullptr);
        st.setProperty ("distance", L.distance, nullptr);
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
            L.distance = (float) st.getProperty ("distance", L.distance);
            juce::StringArray di;
            di.addTokens (st["dist"].toString(), ",", {});
            for (int k = 0; k < juce::jmin (kNumKeys, di.size()); ++k)
                engine.baseDist[(size_t) s][(size_t) k] = juce::jlimit (kMinDist, kMaxDist, di[k].getFloatValue());
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
        const juce::File irf (extra["irFile"].toString());
        if (irf != juce::File() && irf != irFile && irf.existsAsFile())
        {
            irFile = irf;
            if (autoLoadDone) loadIr (irf);           // otherwise the first timer tick loads it
        }
        else if (irf == juce::File() && irFile != juce::File() && autoLoadDone) clearIr();
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
