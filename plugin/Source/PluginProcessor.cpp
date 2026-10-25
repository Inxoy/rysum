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

    // ---- modulation
    auto hz = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, v < 1.0f ? 3 : 2) + " Hz"; });
    auto secs = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                { return v < 1.0f ? String (roundToInt (v * 1000.0f)) + " ms" : String (v, 2) + " s"; });
    for (int i = 0; i < kNumLfos; ++i)
    {
        const String n = "LFO " + String (i + 1);
        const String id = "lfo" + String (i + 1);
        const float rates[] { 0.25f, 0.5f, 2.0f, 0.1f };
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { id + "Shape", 1 }, n + " Form", lfoShapeNames(), i == 3 ? (int) shSmooth : (int) shSine));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Rate", 1 }, n + " Tempo", NormalisableRange<float> (0.01f, 20.0f, 0.001f, 0.3f), rates[i], hz));
        l.add (std::make_unique<AudioParameterBool> (ParameterID { id + "Sync", 1 }, n + " Sync", false));
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { id + "Div", 1 }, n + " Notenwert", lfoDivNames(), 2));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Spread", 1 }, n + " Versatz", NormalisableRange<float> (0, 1), 0.0f, pct));
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { id + "SpreadMode", 1 }, n + " Versatz-Art", StringArray { "Tastatur", "Zufall" }, 0));
    }
    for (int j = 0; j < kNumEnvs; ++j)
    {
        const String n = String::fromUTF8 ("H\xc3\xbcllkurve ") + String (j + 1);
        const String id = "env" + String (j + 1);
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { id + "Mode", 1 }, n + " Modus", StringArray { "Pro Pfeife", "Global" }, 0));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Delay", 1 },   n + String::fromUTF8 (" Verz\xc3\xb6gerung"), NormalisableRange<float> (0.0f, 10.0f, 0.001f, 0.35f), 0.0f, secs));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Attack", 1 },  n + " Anstieg",  NormalisableRange<float> (0.001f, 20.0f, 0.001f, 0.3f), j == 0 ? 0.3f : 3.0f, secs));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Decay", 1 },   n + " Abfall",   NormalisableRange<float> (0.001f, 20.0f, 0.001f, 0.3f), j == 0 ? 1.0f : 4.0f, secs));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Sustain", 1 }, n + " Halten",   NormalisableRange<float> (0, 1), j == 0 ? 0.6f : 0.0f, pct));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Release", 1 }, n + " Ausklang", NormalisableRange<float> (0.001f, 20.0f, 0.001f, 0.3f), j == 0 ? 1.5f : 3.0f, secs));
    }
    for (int r = 0; r < kNumRoutes; ++r)
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "mod" + String (r + 1) + "Amt", 1 }, "Modulation " + String (r + 1) + " Menge",
                                                      NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f,
                                                      AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                                                      { return (v > 0.0005f ? "+" : "") + String (roundToInt (v * 100.0f)) + " %"; })));
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

    // ---- parameter table (modulation works on indices into it)
    for (auto* prm : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
        {
            paramIndex[rp->getParameterID()] = (int) prmList.size();
            prmList.push_back (rp);
            rawList.push_back (apvts.getRawParameterValue (rp->getParameterID()));
            isFloatParam.push_back (dynamic_cast<juce::AudioParameterFloat*> (rp) != nullptr && rp->getParameterID() != "output" ? 1 : 0);
        }
    const int N = (int) prmList.size();
    modVals.assign ((size_t) N, 0.0f);
    modOff.assign ((size_t) N, 0.0f);
    modHitFlag.assign ((size_t) N, 0);
    modNorm.reset (new std::atomic<float>[(size_t) N]);
    for (int i = 0; i < N; ++i) modNorm[i] = std::numeric_limits<float>::quiet_NaN();
    hits.reserve (kNumRoutes); prevHits.reserve (kNumRoutes);
    auto I = [this] (const juce::String& id) { jassert (paramIndex.count (id) > 0); return paramIndex.at (id); };
    for (int s = 0; s < kNumStops; ++s)
    {
        ix.stop[(size_t) s] = I (stopParamId (s)); ix.vol[(size_t) s] = I (volParamId (s)); ix.swind[(size_t) s] = I (windParamId (s));
        ix.motion[(size_t) s] = I (motionParamId (s)); ix.rate[(size_t) s] = I (rateParamId (s));
        ix.depth[(size_t) s] = I (depthParamId (s)); ix.ellipse[(size_t) s] = I (ellipseParamId (s));
    }
    ix.doppler = I ("doppler"); ix.air = I ("air"); ix.distAmount = I ("distAmount"); ix.temperament = I ("temperament");
    ix.pitch = I ("pitch"); ix.detune = I ("detune"); ix.tremOn = I ("tremOn"); ix.tremRate = I ("tremRate"); ix.tremDepth = I ("tremDepth");
    ix.freeze = I ("freeze"); ix.freezeSmear = I ("freezeSmear"); ix.freezeRelease = I ("freezeRelease"); ix.motionSync = I ("motionSync");
    ix.master = I ("master"); ix.output = I ("output"); ix.click = I ("click"); ix.clickBlend = I ("clickBlend");
    ix.releaseClick = I ("releaseClick"); ix.stopPull = I ("stopPull"); ix.wind = I ("wind");
    ix.degAmount = I ("degAmount"); ix.degGrain = I ("degGrain"); ix.degBits = I ("degBits"); ix.degRate = I ("degRate");
    ix.degWobble = I ("degWobble"); ix.degDropouts = I ("degDropouts"); ix.degMode = I ("degMode");
    ix.irDry = I ("irDry"); ix.irWet = I ("irWet");
    for (int i = 0; i < kNumLfos; ++i)
    {
        const juce::String id = "lfo" + juce::String (i + 1);
        ix.lfoShape[(size_t) i] = I (id + "Shape"); ix.lfoRate[(size_t) i] = I (id + "Rate"); ix.lfoSync[(size_t) i] = I (id + "Sync");
        ix.lfoDiv[(size_t) i] = I (id + "Div"); ix.lfoSpread[(size_t) i] = I (id + "Spread"); ix.lfoSpreadMode[(size_t) i] = I (id + "SpreadMode");
    }
    for (int j = 0; j < kNumEnvs; ++j)
    {
        const juce::String id = "env" + juce::String (j + 1);
        ix.envMode[(size_t) j] = I (id + "Mode"); ix.envDelay[(size_t) j] = I (id + "Delay"); ix.envAttack[(size_t) j] = I (id + "Attack");
        ix.envDecay[(size_t) j] = I (id + "Decay"); ix.envSustain[(size_t) j] = I (id + "Sustain"); ix.envRelease[(size_t) j] = I (id + "Release");
    }
    for (int r = 0; r < kNumRoutes; ++r) { ix.modAmt[(size_t) r] = I ("mod" + juce::String (r + 1) + "Amt"); routeSrc[(size_t) r] = -1; routeTgt[(size_t) r] = -1; }
    engine.setModHook (this);

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

EngineParams RysumProcessor::readParams (const float* vals) const
{
    auto V = [this, vals] (int i) { return vals != nullptr ? vals[i] : rawList[(size_t) i]->load(); };
    EngineParams p;
    for (int s = 0; s < kNumStops; ++s)
    {
        p.stopOn[(size_t) s] = V (ix.stop[(size_t) s]) > 0.5f;
        p.stopGain[(size_t) s] = dbToGain (V (ix.vol[(size_t) s]));
        p.stopWind[(size_t) s] = V (ix.swind[(size_t) s]);
        p.motionMode[(size_t) s] = (int) V (ix.motion[(size_t) s]);
        p.motionRate[(size_t) s] = V (ix.rate[(size_t) s]);
        p.motionDepth[(size_t) s] = V (ix.depth[(size_t) s]);
        p.ellipse[(size_t) s] = V (ix.ellipse[(size_t) s]);
    }
    p.doppler = V (ix.doppler);
    p.air = V (ix.air);
    p.distAmount = V (ix.distAmount);
    p.tuneCents = temperamentShift ((int) V (ix.temperament));
    p.pitchCents = V (ix.pitch);
    p.detune = V (ix.detune);
    p.tremOn = V (ix.tremOn) > 0.5f;
    p.tremRate = V (ix.tremRate);
    p.tremDepth = V (ix.tremDepth);
    p.freeze = V (ix.freeze) > 0.5f;
    p.freezeSmear = V (ix.freezeSmear);
    p.freezeRelease = V (ix.freezeRelease);
    p.sync = V (ix.motionSync) > 0.5f;
    p.masterGain = dbToGain (V (ix.master));
    p.outputMode = (int) V (ix.output);
    p.clickLevel = V (ix.click);
    p.clickBlend = V (ix.clickBlend);
    p.releaseClick = V (ix.releaseClick);
    p.stopPull = V (ix.stopPull);
    p.wind = V (ix.wind);
    p.deg.amount = V (ix.degAmount);
    p.deg.grain = V (ix.degGrain);
    p.deg.bits = V (ix.degBits);
    p.deg.rateHz = V (ix.degRate);
    p.deg.wobble = V (ix.degWobble);
    p.deg.dropouts = V (ix.degDropouts);
    p.deg.perPipe = V (ix.degMode) < 0.5f;

    for (int i = 0; i < kNumLfos; ++i)
    {
        auto& L = p.mod.lfo[(size_t) i];
        L.shape = (int) V (ix.lfoShape[(size_t) i]);
        L.rate = V (ix.lfoRate[(size_t) i]);
        L.sync = V (ix.lfoSync[(size_t) i]) > 0.5f;
        L.div = (int) V (ix.lfoDiv[(size_t) i]);
        L.spread = V (ix.lfoSpread[(size_t) i]);
        L.spreadMode = (int) V (ix.lfoSpreadMode[(size_t) i]);
    }
    for (int j = 0; j < kNumEnvs; ++j)
    {
        auto& E = p.mod.env[(size_t) j];
        E.perPipe = V (ix.envMode[(size_t) j]) < 0.5f;
        E.delay = V (ix.envDelay[(size_t) j]);
        E.attack = V (ix.envAttack[(size_t) j]);
        E.decay = V (ix.envDecay[(size_t) j]);
        E.sustain = V (ix.envSustain[(size_t) j]);
        E.release = V (ix.envRelease[(size_t) j]);
    }
    for (int r = 0; r < kNumRoutes; ++r)
        p.mod.routes[(size_t) r] = { routeSrc[(size_t) r].load(), routeTgt[(size_t) r].load(), V (ix.modAmt[(size_t) r]) };
    return p;
}

// ---------------------------------------------------------------------------
// modulation of plugin parameters (audio thread, every 64 samples)
// ---------------------------------------------------------------------------
void RysumProcessor::modulate (EngineParams& p, const float* src)
{
    const int N = (int) rawList.size();
    for (int i = 0; i < N; ++i) modVals[(size_t) i] = rawList[(size_t) i]->load();
    hits.clear();
    for (int r = 0; r < kNumRoutes; ++r)
    {
        const int sIdx = routeSrc[(size_t) r].load(), t = routeTgt[(size_t) r].load() - kGlobalTargetBase;
        if (sIdx < 0 || sIdx >= kNumSources || t < 0 || t >= N || ! isFloatParam[(size_t) t]) continue;
        const float a = modVals[(size_t) ix.modAmt[(size_t) r]];
        if (a == 0.0f) continue;
        if (! modHitFlag[(size_t) t]) { modHitFlag[(size_t) t] = 1; modOff[(size_t) t] = 0.0f; hits.push_back (t); }
        modOff[(size_t) t] += a * src[sIdx];
    }
    for (int t : prevHits) modNorm[t] = std::numeric_limits<float>::quiet_NaN();
    for (int t : hits)
    {
        auto* prm = prmList[(size_t) t];
        const float n = juce::jlimit (0.0f, 1.0f, prm->convertTo0to1 (modVals[(size_t) t]) + modOff[(size_t) t]);
        modVals[(size_t) t] = prm->convertFrom0to1 (n);
        modNorm[t] = n;
        modHitFlag[(size_t) t] = 0;
    }
    std::swap (hits, prevHits);
    haveModVals = true;
    if (prevHits.empty()) return;            // nothing global is modulated: p already holds these values

    EngineParams q = readParams (modVals.data());
    applyCcOverrides (q);
    q.bpm = p.bpm; q.ppq = p.ppq; q.playing = p.playing;
    p = q;
}

int RysumProcessor::getParamIndex (const juce::String& id) const
{
    auto it = paramIndex.find (id);
    return it == paramIndex.end() ? -1 : it->second;
}

float RysumProcessor::getModulatedNorm (int index) const
{
    return index >= 0 && index < (int) prmList.size() ? modNorm[index].load() : std::numeric_limits<float>::quiet_NaN();
}

juce::String RysumProcessor::targetId (int target) const
{
    if (target < 0) return {};
    if (target < kNumPipeTargets) return pipeTargetId (target);
    const int i = target - kGlobalTargetBase;
    return i >= 0 && i < (int) prmList.size() ? prmList[(size_t) i]->getParameterID() : juce::String();
}

int RysumProcessor::targetFromId (const juce::String& id) const
{
    if (id.startsWith ("pipe:")) { const int t = id.fromFirstOccurrenceOf (":", false, false).getIntValue(); return t >= 0 && t < kNumPipeTargets ? t : -1; }
    const int i = getParamIndex (id);
    return i >= 0 && isFloatParam[(size_t) i] ? kGlobalTargetBase + i : -1;
}

void RysumProcessor::setRoute (int r, int src, int target)
{
    if (r < 0 || r >= kNumRoutes) return;
    routeSrc[(size_t) r] = src;
    routeTgt[(size_t) r] = target;
    ++routeVersion;
}

int RysumProcessor::addRoute (int src, int target, float amount)
{
    for (int r = 0; r < kNumRoutes; ++r)
        if (routeSrc[(size_t) r].load() < 0 && routeTgt[(size_t) r].load() < 0)
        {
            setRoute (r, src, target);
            if (auto* a = prmList[(size_t) ix.modAmt[(size_t) r]])
            {
                a->beginChangeGesture(); a->setValueNotifyingHost (a->convertTo0to1 (amount)); a->endChangeGesture();
            }
            return r;
        }
    return -1;
}

void RysumProcessor::clearRoute (int r)
{
    if (r < 0 || r >= kNumRoutes) return;
    setRoute (r, -1, -1);
    if (auto* a = prmList[(size_t) ix.modAmt[(size_t) r]])
    {
        a->beginChangeGesture(); a->setValueNotifyingHost (a->convertTo0to1 (0.0f)); a->endChangeGesture();
    }
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
    const float dryDb = haveModVals ? modVals[(size_t) ix.irDry] : irDry->load();
    const float wetDb = haveModVals ? modVals[(size_t) ix.irWet] : irWet->load();
    const float dryTarget = dryDb <= -59.9f ? 0.0f : dbToGain (dryDb);
    const float wetTarget = wetDb <= -59.9f ? 0.0f : dbToGain (wetDb);

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
    {
        juce::ValueTree mod ("MOD");
        for (int r = 0; r < kNumRoutes; ++r)
        {
            mod.setProperty ("src" + juce::String (r), routeSrc[(size_t) r].load(), nullptr);
            mod.setProperty ("tgt" + juce::String (r), targetId (routeTgt[(size_t) r].load()), nullptr);
        }
        extra.appendChild (mod, nullptr);
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

    {
        const auto mod = extra.getChildWithName ("MOD");
        for (int r = 0; r < kNumRoutes; ++r)
        {
            const int src = mod.isValid() ? (int) mod.getProperty ("src" + juce::String (r), -1) : -1;
            const int tgt = mod.isValid() ? targetFromId (mod.getProperty ("tgt" + juce::String (r)).toString()) : -1;
            setRoute (r, src >= 0 && src < kNumSources ? src : -1, tgt);
        }
    }
    if (extra.isValid())
    {
        for (auto st : extra)
        {
            if (! st.hasType ("STOP")) continue;
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
