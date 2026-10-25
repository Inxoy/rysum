#pragma once

#include "Common.h"
#include "SampleLibrary.h"
#include "Spatial.h"
#include "Degrade.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <vector>

namespace rysum
{
struct EngineParams
{
    std::array<bool, kNumStops> stopOn {};
    float masterGain = 1.0f;
    int   outputMode = 0;              // 0 = stereo, 1..5 = AmbiX order
    float clickLevel = 0.65f;          // key-down click
    float clickBlend = 0.45f;          // 0 = clicks at the console, 1 = clicks at the pipes (into the attack)
    float releaseClick = 0.4f;         // key-up click
    float stopPull = 0.7f;             // stop pull / push sound
    float wind = 0.15f;                // blower / wind noise
    float consoleAz = 0.0f, consoleEl = -10.0f;
    DegradeParams deg;
    std::array<int,   kNumStops> motionMode {};
    std::array<float, kNumStops> motionRate {};
    std::array<float, kNumStops> motionDepth {};
};

class OrganEngine
{
public:
    OrganEngine();

    void prepare (double sampleRate, int maxBlockSize);

    /** call from the audio thread only (the processor swaps the library in at block start) */
    void setLibrary (std::shared_ptr<const SampleLibrary> lib);
    const SampleLibrary* getLibrary() const noexcept { return library.get(); }

    /** renders one block. 'out' is cleared first. MIDI events are handled sample-accurately. */
    void process (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi, const EngineParams& p);

    // ---------------- shared with the UI (any thread) ----------------
    std::array<std::array<std::atomic<float>, kNumKeys>, kNumStops> baseAz, baseEl;
    std::array<std::array<std::atomic<bool>,  kNumKeys>, kNumStops> muted;
    std::atomic<double>   timeSeconds { 0.0 };
    std::atomic<uint64_t> heldMask { 0 };          // bit k = physical key k held
    std::atomic<int>      activeVoices { 0 };
    std::atomic<float>    meterL { 0.0f }, meterR { 0.0f };
    std::array<std::atomic<uint64_t>, kNumStops> soundingMask {};   // bit k = pipe k of that stop sounds

    /** ask the engine to play one pipe for ~1.5 s (Pfeifen view). Lock-free, UI thread. */
    void requestPreview (int stop, int key);

    /** position of a pipe right now (base + motion), for the UI */
    Pos currentPosition (int stop, int key, const EngineParams& p, double t) const noexcept;

    static constexpr int kMaxVoices = 384;
    static constexpr int kMaxNoiseVoices = 48;

private:
    struct Voice
    {
        bool active = false;
        int stop = 0, key = 0;
        const Pipe* pipe = nullptr;
        const float* data = nullptr;
        int length = 0;
        double pos = 0.0, baseRate = 1.0;
        bool looping = false, held = true;
        int loopXf = 0;
        bool relXfade = false;
        double relPos = 0.0;
        int relCount = 0, relLen = 1;
        bool killing = false;
        float killGain = 1.0f;
        int previewRemain = -1;          // samples until an automatic note-off (preview)
        uint32_t age = 0;
        std::array<float, kMaxAmbiChannels> g0 {}, g1 {};
        bool fresh = true;
        // degradation
        DegradeState deg;
        float held1 = 0.0f, env = 0.0f;
        Wobble wobble;
    };

    struct NoiseVoice
    {
        bool active = false;
        const float* data = nullptr;
        int length = 0;
        double pos = 0.0, rate = 1.0;
        float gain = 0.0f;
        std::array<float, kMaxAmbiChannels> g {};
    };

    void handleMidi (const juce::MidiMessage& m, const EngineParams& p);
    void keyDown (int key, float velocity, const EngineParams& p);
    void keyUp (int key, const EngineParams& p);
    void startVoice (int stop, int key, int previewSamples = -1);
    void releaseVoice (Voice& v);
    void releaseStopKey (int stop, int key);
    void playNoise (const std::vector<NoiseSample>& pool, float gain, const float* gains, int& lastIndex);
    void gainsFor (Pos pos, float* out) const noexcept;
    void renderRange (juce::AudioBuffer<float>& out, int start, int end, int blockLen, const EngineParams& p);
    float readSample (const float* d, int len, double pos) const noexcept;

    std::shared_ptr<const SampleLibrary> library;
    std::vector<Voice> voices;
    std::vector<NoiseVoice> noise;
    std::array<bool, kNumKeys> keyHeld {}, keySustained {};
    bool sustainPedal = false;
    std::array<bool, kNumStops> lastStopOn {};
    bool firstBlock = true;
    double sr = 48000.0;
    int maxBlock = 512;
    int usedChannels = 2;
    int order = 0;                      // 0 = stereo
    uint32_t ageCounter = 0;
    XorShift rng { 4711 };
    int lastDown = -1, lastUp = -1, lastPull = -1;
    std::vector<float> scratch, scratchRamp;

    // wind
    double windPos = 0.0;
    float windGain = 0.0f;

    // sum-mode degradation
    DegradeState sumDeg { 99 };
    std::array<float, kMaxOutChannels> sumHeld {}, sumEnv {};
    float masterSmoothed = 1.0f;

    // preview FIFO (UI -> audio)
    juce::AbstractFifo previewFifo { 32 };
    std::array<std::pair<int, int>, 32> previewQueue {};

    EngineParams lastParams;
};
} // namespace rysum
