#pragma once

#include "OrganEngine.h"
#include "Convolver.h"
#include <map>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

class RysumProcessor : public juce::AudioProcessor,
                       private juce::Timer,
                       private juce::AsyncUpdater,
                       private rysum::ModHook
{
public:
    RysumProcessor();
    ~RysumProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Rysum"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Rysum"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ------------------------------------------------------------------
    juce::AudioProcessorValueTreeState apvts;
    rysum::OrganEngine engine;
    juce::MidiKeyboardState keyboardState;

    /** current parameters as the engine sees them (also used by the UI for positions) */
    rysum::EngineParams readParams (const float* values = nullptr) const;

    // ---- modulation (routes: message thread writes, audio thread reads)
    int getParamIndex (const juce::String& id) const;
    juce::RangedAudioParameter* getParamAt (int i) const { return i >= 0 && i < (int) prmList.size() ? prmList[(size_t) i] : nullptr; }
    int getNumParams() const { return (int) prmList.size(); }
    bool isModTarget (int i) const { return i >= 0 && i < (int) prmList.size() && isFloatParam[(size_t) i]; }
    /** normalised value after modulation, NaN if the parameter is not modulated right now */
    float getModulatedNorm (int index) const;
    int getRouteSrc (int r) const { return routeSrc[(size_t) r].load(); }
    int getRouteTgt (int r) const { return routeTgt[(size_t) r].load(); }
    void setRoute (int r, int src, int target);
    int addRoute (int src, int target, float amount);     // first free slot, -1 if full
    void clearRoute (int r);
    juce::String targetId (int target) const;
    int targetFromId (const juce::String& id) const;
    juce::String getModAmountId (int r) const { return "mod" + juce::String (r + 1) + "Amt"; }
    std::atomic<int> routeVersion { 0 };

    // samples
    void loadSamples (const juce::File& folder);
    juce::File getSamplesFolder() const { return samplesFolder; }
    std::shared_ptr<const rysum::SampleLibrary> getLibrary() const { return std::atomic_load (&uiLibrary); }
    juce::String getLoadStatus() const { const juce::ScopedLock sl (statusLock); return loadStatus; }
    bool isLoading() const { return loading.load(); }

    // room / impulse response (message thread)
    void loadIr (const juce::File& file);
    void clearIr();
    juce::File getIrFile() const { return irFile; }
    juce::String getIrStatus() const { const juce::ScopedLock sl (statusLock); return irStatus; }

    // layouts (message thread)
    std::array<rysum::LayoutSettings, rysum::kNumStops> layouts;
    void applyLayout (int stop);
    void setPipePosition (int stop, int key, rysum::Pos p);

    static juce::String stopParamId (int s)   { return "stop" + juce::String (s); }
    static juce::String motionParamId (int s) { return "motion" + juce::String (s); }
    static juce::String volParamId (int s)    { return "vol" + juce::String (s); }
    static juce::String windParamId (int s)   { return "swind" + juce::String (s); }
    static juce::String rateParamId (int s)   { return "rate" + juce::String (s); }
    static juce::String depthParamId (int s)  { return "depth" + juce::String (s); }
    static juce::String ellipseParamId (int s) { return "ellipse" + juce::String (s); }

    /** bumps whenever the library, positions or mutes change (UI repaint hint) */
    std::atomic<int> changeCounter { 0 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void handleAsyncUpdate() override;
    void applyCcOverrides (rysum::EngineParams&) const;
    void modulate (rysum::EngineParams& p, const float* globalSources) override;

    struct Indices
    {
        std::array<int, rysum::kNumStops> stop {}, vol {}, swind {}, motion {}, rate {}, depth {}, ellipse {};
        int doppler = 0, air = 0, distAmount = 0, temperament = 0, pitch = 0, detune = 0, tremOn = 0, tremRate = 0, tremDepth = 0,
            freeze = 0, freezeSmear = 0, freezeRelease = 0, motionSync = 0, master = 0, output = 0, click = 0, clickBlend = 0,
            releaseClick = 0, stopPull = 0, wind = 0, degAmount = 0, degGrain = 0, degBits = 0, degRate = 0, degWobble = 0,
            degDropouts = 0, degMode = 0, irDry = 0, irWet = 0;
        std::array<int, rysum::kNumLfos> lfoShape {}, lfoRate {}, lfoSync {}, lfoDiv {}, lfoSpread {}, lfoSpreadMode {};
        std::array<int, rysum::kNumEnvs> envMode {}, envDelay {}, envAttack {}, envDecay {}, envSustain {}, envRelease {};
        std::array<int, rysum::kNumRoutes> modAmt {};
    } ix;
    std::vector<juce::RangedAudioParameter*> prmList;
    std::vector<std::atomic<float>*> rawList;
    std::vector<char> isFloatParam;
    std::map<juce::String, int> paramIndex;
    std::array<std::atomic<int>, rysum::kNumRoutes> routeSrc {}, routeTgt {};
    std::vector<float> modVals, modOff;              // audio thread
    std::vector<char> modHitFlag;
    std::vector<int> hits, prevHits;
    std::unique_ptr<std::atomic<float>[]> modNorm;
    bool haveModVals = false;
    std::vector<juce::RangedAudioParameter*> ccTargets;
    std::array<std::atomic<float>, 17> ccPending {};
    std::map<std::string, std::atomic<float>*> extraParams;
    void startAutoLoad();
    void setStatus (const juce::String& s) { const juce::ScopedLock sl (statusLock); loadStatus = s; }
    void onLoaded (std::shared_ptr<const rysum::SampleLibrary> lib, const juce::String& err, const juce::File& folder);
    void setIrStatus (const juce::String& s) { const juce::ScopedLock sl (statusLock); irStatus = s; }

    juce::File irFile;
    juce::String irStatus { juce::String::fromUTF8 ("keine \xe2\x80\x93 nur trocken") };
    juce::ThreadPool irPool { 1 };
    std::shared_ptr<rysum::Convolver> irActive;          // audio thread
    std::shared_ptr<rysum::Convolver> pendingIr;         // handed over (atomic_store / exchange)
    std::shared_ptr<rysum::Convolver> irUi, retiredIr;   // message thread keeps them alive
    std::atomic<bool> irClearRequest { false };
    std::atomic<double> hostRate { 0.0 };
    std::vector<float> irInput;
    float drySmoothed = 1.0f, wetSmoothed = 0.0f;
    bool lastFuma = false;
    std::atomic<float> *irDry = nullptr, *irWet = nullptr, *irFumaParam = nullptr;
    std::shared_ptr<std::atomic<bool>> aliveFlag = std::make_shared<std::atomic<bool>> (true);

    class Loader;
    std::unique_ptr<Loader> loader;
    juce::File samplesFolder;
    std::shared_ptr<const rysum::SampleLibrary> pendingLibrary;     // handed to the audio thread
    std::shared_ptr<const rysum::SampleLibrary> uiLibrary;          // used by the editor
    std::shared_ptr<const rysum::SampleLibrary> retiredLibrary;     // kept alive briefly (freed off the audio thread)
    int retireCountdown = 0;
    juce::CriticalSection statusLock;
    juce::String loadStatus { "Samples werden gesucht ..." };
    std::atomic<bool> loading { false };
    bool autoLoadDone = false;

    std::array<std::atomic<float>*, rysum::kNumStops> stopParams {}, motionParams {}, rateParams {}, depthParams {}, volParams {}, windParams {}, ellipseParams {};
    std::atomic<float> *master = nullptr, *output = nullptr, *click = nullptr, *clickBlend = nullptr, *releaseClick = nullptr,
                       *stopPull = nullptr, *wind = nullptr, *degAmount = nullptr, *degGrain = nullptr, *degBits = nullptr,
                       *degRate = nullptr, *degWobble = nullptr, *degDropouts = nullptr, *degMode = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RysumProcessor)
};
