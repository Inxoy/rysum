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
    std::array<float, kNumStops> stopGain { 1, 1, 1, 1, 1, 1, 1 };   // linear
    std::array<float, kNumStops> stopWind { 1, 1, 1, 1, 1, 1, 1 };   // 0 = stop closed (silent) .. 1 = fully pulled
    float masterGain = 1.0f;
    int   outputMode = 0;              // 0 = stereo, 1..5 = AmbiX order
    float clickLevel = 0.65f;          // key-down click
    float clickBlend = 0.45f;          // 0 = clicks at the console, 1 = clicks at the pipes (into the attack)
    float releaseClick = 0.4f;         // key-up click
    float stopPull = 0.7f;             // stop pull / push sound
    float wind = 0.15f;                // global wind: blower noise, above ~35 % increasingly unstable (gusts)
    float consoleAz = 0.0f, consoleEl = -10.0f;
    DegradeParams deg;

    // movement
    std::array<int,   kNumStops> motionMode {};
    std::array<float, kNumStops> motionRate {};
    std::array<float, kNumStops> motionDepth {};
    std::array<float, kNumStops> ellipse {};        // 0..0.9: elliptic orbit, closer in front
    bool   sync = false;                             // speeds relative to 120 BPM, locked to the host position
    double bpm = 120.0, ppq = 0.0;
    bool   playing = false;

    // distance
    float doppler = 1.0f;              // 0 = off, 1 = physical, up to 4 = exaggerated
    float air = 0.5f;                  // air absorption amount
    float distAmount = 0.7f;           // 0 = distance does not change the level, 1 = 1/r

    // tuning
    std::array<float, 12> tuneCents {};  // per pitch class, from the recorded meantone
    float pitchCents = 0.0f;             // whole organ
    float detune = 0.0f;                 // max random detune per pipe (cents)

    // tremulant
    bool  tremOn = false;
    float tremRate = 5.5f, tremDepth = 0.5f;

    // freeze
    bool  freeze = false;
    float freezeSmear = 0.5f, freezeRelease = 2.0f;
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

    /** mono send to the room (impulse response) of the last block: pipes weighted by distance */
    const float* getRoomSend() const noexcept { return roomSend.data(); }

    // ---------------- shared with the UI (any thread) ----------------
    std::array<std::array<std::atomic<float>, kNumKeys>, kNumStops> baseAz, baseEl, baseDist;
    std::array<std::array<std::atomic<bool>,  kNumKeys>, kNumStops> muted;
    std::atomic<double>   timeSeconds { 0.0 };
    std::atomic<uint64_t> heldMask { 0 };          // bit k = physical key k held
    std::atomic<int>      activeVoices { 0 }, frozenVoices { 0 };
    std::atomic<float>    meterL { 0.0f }, meterR { 0.0f };
    std::array<std::atomic<uint64_t>, kNumStops> soundingMask {};   // bit k = pipe k of that stop sounds

    /** ask the engine to play one pipe for ~1.5 s. Lock-free, UI thread. */
    void requestPreview (int stop, int key);

    /** position of a pipe right now (base + motion), for the UI */
    Pos currentPosition (int stop, int key, const EngineParams& p) const noexcept;

    /** integrated motion phase per stop (cycles); speed changes only change its slope */
    std::array<std::atomic<double>, kNumStops> motionPhase {};

    static constexpr int kMaxVoices = 384;
    static constexpr int kMaxNoiseVoices = 48;
    static constexpr int kGrains = 4;

private:
    struct Grain { double pos = 0; int len = 0, age = 0; float rateMul = 1.0f; bool on = false; };

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
        float killGain = 1.0f, killStep = 0.0f;
        int previewRemain = -1;          // samples until an automatic note-off (preview)
        uint32_t age = 0;
        std::array<float, kMaxAmbiChannels> g0 {}, g1 {};
        float send0 = 0.0f, send1 = 0.0f;
        bool fresh = true;
        // degradation
        DegradeState deg;
        float held1 = 0.0f, env = 0.0f;
        Wobble wobble, breath;
        float windSm = -1.0f;            // smoothed stop wind (-1 = take target directly)
        float lp = 0.0f, lpA = 1.0f;     // tone filter for under-winded pipes
        float lpAir = 0.0f, airA = 1.0f; // air absorption by distance
        float prevDist = -1.0f, dop = 1.0f, dopTarget = 1.0f;
        float detuneRnd = 0.0f;          // -1..1, fixed per pipe
        // freeze
        bool frozen = false;
        float freezeMix = 0.0f;
        std::array<Grain, kGrains> grains {};
        int grainTimer = 0;
        XorShift grainRng { 1 };
    };

    struct NoiseVoice
    {
        bool active = false;
        const float* data = nullptr;
        int length = 0;
        double pos = 0.0, rate = 1.0;
        float gain = 0.0f, send = 0.0f;
        std::array<float, kMaxAmbiChannels> g {};
    };

    void handleMidi (const juce::MidiMessage& m, const EngineParams& p);
    void keyDown (int key, float velocity, const EngineParams& p);
    void keyUp (int key, const EngineParams& p);
    void startVoice (int stop, int key, int previewSamples = -1);
    void releaseVoice (Voice& v);
    void freezeVoice (Voice& v);
    void releaseStopKey (int stop, int key);
    void playNoise (const std::vector<NoiseSample>& pool, float gain, const float* gains, int& lastIndex, float send);
    void gainsFor (Pos pos, float* out) const noexcept;
    void renderRange (juce::AudioBuffer<float>& out, int start, int end, int blockLen, const EngineParams& p);
    void updateVoiceSpatial (Voice& v, const EngineParams& p, double blockSeconds) noexcept;
    float readSample (const float* d, int len, double pos) const noexcept;
    float grainSample (Voice& v, double rate, float smear) noexcept;

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
    std::vector<float> scratch, scratchRamp, roomSend;

    // wind + tremulant (shared by all pipes: one wind chest)
    double windPos = 0.0;
    float windGain = 0.0f;
    double gustPhase = 0.0, tremPhase = 0.0;
    std::vector<float> gust, trem;    // one value per 16 samples

    // sum-mode degradation
    DegradeState sumDeg { 99 };
    std::array<float, kMaxOutChannels> sumHeld {}, sumEnv {};
    float masterSmoothed = 1.0f;

    // preview FIFO (UI -> audio)
    juce::AbstractFifo previewFifo { 32 };
    std::array<std::pair<int, int>, 32> previewQueue {};
};
} // namespace rysum
