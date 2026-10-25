#pragma once

#include "OrganEngine.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

class RysumProcessor : public juce::AudioProcessor,
                       private juce::Timer
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
    rysum::EngineParams readParams() const;

    // samples
    void loadSamples (const juce::File& folder);
    juce::File getSamplesFolder() const { return samplesFolder; }
    std::shared_ptr<const rysum::SampleLibrary> getLibrary() const { return std::atomic_load (&uiLibrary); }
    juce::String getLoadStatus() const { const juce::ScopedLock sl (statusLock); return loadStatus; }
    bool isLoading() const { return loading.load(); }

    // layouts (message thread)
    std::array<rysum::LayoutSettings, rysum::kNumStops> layouts;
    void applyLayout (int stop);
    void setPipePosition (int stop, int key, rysum::Pos p);

    static juce::String stopParamId (int s)   { return "stop" + juce::String (s); }
    static juce::String motionParamId (int s) { return "motion" + juce::String (s); }
    static juce::String rateParamId (int s)   { return "rate" + juce::String (s); }
    static juce::String depthParamId (int s)  { return "depth" + juce::String (s); }

    /** bumps whenever the library, positions or mutes change (UI repaint hint) */
    std::atomic<int> changeCounter { 0 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void startAutoLoad();
    void setStatus (const juce::String& s) { const juce::ScopedLock sl (statusLock); loadStatus = s; }
    void onLoaded (std::shared_ptr<const rysum::SampleLibrary> lib, const juce::String& err, const juce::File& folder);
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

    std::array<std::atomic<float>*, rysum::kNumStops> stopParams {}, motionParams {}, rateParams {}, depthParams {};
    std::atomic<float> *master = nullptr, *output = nullptr, *click = nullptr, *clickBlend = nullptr, *releaseClick = nullptr,
                       *stopPull = nullptr, *wind = nullptr, *degAmount = nullptr, *degGrain = nullptr, *degBits = nullptr,
                       *degRate = nullptr, *degWobble = nullptr, *degDropouts = nullptr, *degMode = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RysumProcessor)
};
