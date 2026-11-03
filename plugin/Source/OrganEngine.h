#pragma once

#include "Common.h"
#include "SampleLibrary.h"
#include "Spatial.h"
#include "Degrade.h"
#include "Modulation.h"
#include "RoomSim.h"
#include "RoomTail.h"
#include "WindChaos.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <vector>

namespace rysum
{
/** the "Effekte" page: sounds and effects that are easy to locate in space */
struct FxParams
{
    // own sounds (samples/noise/eigene), triggered by the keys
    float custLevel = 0.0f, custDown = 1.0f, custUp = 0.0f, custPitch = 0.0f, custScatter = 0.0f, custWander = 0.0f;
    int   custPlace = 0;                 // 0 at the pipe, 1 random, 2 console, 3 circling
    // tracker path: ticks running from the console to the pipe
    float pathLevel = 0.0f, pathTime = 0.12f, pathSpread = 12.0f;
    int   pathTicks = 6;
    bool  pathWait = false;              // pipes speak when the path arrives
    // crackle at every sounding pipe
    float crackLevel = 0.0f, crackRate = 6.0f, crackLen = 1.2f;   // Hz per pipe, ms
    // wind leaks: hissing spots
    float leakLevel = 0.0f, leakFreq = 4000.0f, leakWander = 0.0f;
    int   leakCount = 3;
    bool  windRound = false;             // blower noise from all sides (instead of one omni signal)
    // upper partials placed away from the pipe
    float partSpread = 0.0f, partSplit = 3.0f, partTurn = 0.0f;   // deg, x fundamental, Hz
    // echoes, every repeat from another direction
    float echoLevel = 0.0f, echoTime = 0.35f, echoFeedback = 0.6f, echoTurn = 90.0f, echoTilt = 0.0f, echoDamp = 0.3f;
    bool  echoSync = false;
    int   echoDiv = 4, echoRepeats = 4;
};

/** the audio input ("Eingang"): live sound placed and moved in the same space as the pipes */
struct InputParams
{
    bool  on = false;
    float level = 1.0f;                  // linear
    bool  stereo = true;                 // two sources (left / right) or one (the sum)
    float az = 0.0f, el = 10.0f, dist = 4.0f;   // deg, deg, m (own place; following a stop: a turn on top of it)
    float width = 60.0f;                 // deg between left and right
    float spin = 0.0f;                   // deg per second, turning around the listener
    int   follow = -1;                   // -1 = own place, else: rides on the pipes of this stop (and its motion)
    bool  gate = false;                  // only sounds while keys are held
    float release = 0.3f;                // s, gate closing
    float send = 1.0f;                   // room send, linear (1 = like a pipe)
};

/** free tuning (Scala file, MTS-ESP / MIDI Tuning Standard): the frequency of every MIDI note on every channel */
struct NoteTable
{
    static constexpr int kSize = 16 * 128;           // [channel 0..15][note 0..127]
    std::array<float, kSize> hz {};                  // Hz
    std::array<uint8_t, kSize> mapped {};            // 0 = this note does not sound (unmapped in the .kbm / by the master)
};

struct EngineParams
{
    std::array<bool, kNumStops> stopOn {};
    std::array<float, kNumStops> stopGain = ones();   // linear
    std::array<float, kNumStops> stopWind = ones();   // 0 = stop closed (silent) .. 1 = fully pulled
    int   windModel = 0;                               // 0 = classic, 1 = unstable (whistles, intermittent speech at low stop wind)
    static std::array<float, kNumStops> ones() { std::array<float, kNumStops> a {}; a.fill (1.0f); return a; }
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
    std::array<float, kNumStops> ellipse {};        // 0..0.9: elliptic orbit, closer on the near side
    std::array<float, kNumStops> ellipseDir {};     // deg: direction of the near side
    std::array<float, kNumStops> ellipseTilt {};    // deg: the orbit leans up towards the near side
    std::array<int,   kNumStops> ellipseLobes = onesInt();   // 1..4 near sides
    std::array<float, kNumStops> pathSpread = ones();        // drawn path: how far the pipes are spread along it
    std::array<bool,  kNumStops> jumpHard {};       // jumps without any glide (they click)
    static std::array<int, kNumStops> onesInt() { std::array<int, kNumStops> a {}; a.fill (1); return a; }
    bool   sync = false;                             // speeds relative to 120 BPM, locked to the host position
    double bpm = 120.0, ppq = 0.0;
    bool   playing = false;

    // distance
    float doppler = 1.0f;              // 0 = off, 1 = physical, up to 4 = exaggerated
    float air = 0.5f;                  // air absorption amount
    float distAmount = 0.7f;           // 0 = distance does not change the level, 1 = 1/r

    // tuning
    KeyTuning tuning = makeKeyTuning (0); // per key: shift (cents) from the recorded pipe that is played, and which pipe
    /** not null: free tuning - every MIDI note (and channel) has its own pitch; the recorded pipe nearest to it
        plays (several notes may share one pipe, each with its own voice) */
    const NoteTable* notes = nullptr;
    bool notesContinuous = true;         // held notes follow tuning changes (MTS-ESP)
    // Lucier mode: the whole output passes through the room n times (the response is built by the processor)
    bool lucierOn = false;
    float lucierMix = 0.5f;              // 0 = only the organ .. 1 = only the room's resonances
    // MPE: every note on its own MIDI channel (2..16; channel 1 = the whole instrument), bent, pressed and slid on its own
    bool mpe = false;
    float mpeBendRange = 48.0f;          // semitones for a full bend on a note channel
    float pitchCents = 0.0f;             // whole organ
    float detune = 0.0f;                 // max random detune per pipe (cents)

    // tremulant
    bool  tremOn = false;
    float tremRate = 5.5f, tremDepth = 0.5f;

    // freeze
    bool  freeze = false;
    float freezeSmear = 0.5f, freezeRelease = 2.0f;

    // modulation (LFOs, envelopes, routings)
    ModParams mod;

    FxParams fx;

    InputParams input;

    // simulated room ("Saal"): early reflections per pipe + late reverb. Up to four rooms: every stop and the input
    // sound in one of them (room 0 = "Saal 1", everything else - clicks, effects - is in it too)
    RoomParams room;
    std::array<RoomParams, kNumRooms - 1> moreRooms {};
    std::array<int, kNumStops> stopRoom {};
    int inputRoom = 0;
    const RoomParams& roomAt (int i) const noexcept { return i <= 0 ? room : moreRooms[(size_t) juce::jmin (kNumRooms - 2, i - 1)]; }
    /** notes to single pipes: 0 = every pulled stop sounds (as an organ), 1 = one stop per note in turn,
        2 = by position among the held notes (lowest -> first stop), 3 = random */
    int noteSpread = 0;

    // room sends: circular harmonics of every pipe's direction (see Convolver)
    int   roomHarmonics = 0;          // 0 = only the plain sum
    bool  roomFollow = true;          // the room's early reflections turn with each pipe
    float roomBaseAz = 0.0f;          // direction used for all pipes when not following
};

/** called by the engine before every 64-sample slice: lets the processor modulate any parameter.
    'globalSources' holds kNumSources values (LFOs without spread, global envelopes, last key, mod wheel, last random). */
struct ModHook
{
    virtual ~ModHook() = default;
    virtual void modulate (EngineParams& p, const float* globalSources) = 0;
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

    /** sends to the room of the last block: [0] all pipes (weighted by distance), [2m-1] / [2m] the same
        weighted by cos / sin (m * pipe azimuth) */
    static constexpr int kMaxRoomSends = 2 * kMaxOrder + 1;
    const float* getRoomSend (int k = 0) const noexcept { return roomSend[(size_t) k].data(); }

    // ---------------- shared with the UI (any thread) ----------------
    std::array<std::array<std::atomic<float>, kNumKeys>, kNumStops> baseAz, baseEl, baseDist;
    std::array<std::array<std::atomic<bool>,  kNumKeys>, kNumStops> muted;
    std::atomic<double>   timeSeconds { 0.0 };
    std::atomic<uint64_t> heldMask { 0 };          // bit k = physical key k held
    std::atomic<int>      activeVoices { 0 }, frozenVoices { 0 };
    std::atomic<float>    meterL { 0.0f }, meterR { 0.0f };
    // simulated room as heard right now (with modulation), for the Saal page
    std::atomic<float> roomW { 14.0f }, roomL { 28.0f }, roomH { 11.0f }, roomLx { 0.5f }, roomLy { 0.4f },
                       roomRtLow { 0.0f }, roomRtHigh { 0.0f }, roomCritDist { 0.0f };
    std::atomic<int>   lastStruckStop { -1 }, lastStruckKey { -1 }, erActive { 0 };
    // ...and the rest of it, for building the late reverb (processor, message thread)
    std::atomic<float> roomAbsW { 0.0f }, roomAbsF { 0.0f }, roomHighD { 0.0f }, roomScat { 0.0f }, roomAir { 0.5f };
    std::atomic<int>   roomOutOrder { 0 };
    std::atomic<bool>  roomOn { false };
    std::atomic<uint32_t> tailSerialInUse { 0xffffffffu };
    /** audio thread: the late reverb for a room (built in the background) */
    void setTailIR (std::shared_ptr<const TailIR> ir, int roomIndex = 0) noexcept { rt[(size_t) juce::jlimit (0, kNumRooms - 1, roomIndex)].tail.setIR (std::move (ir)); }
    // the other rooms as heard (rooms 1..3; room 0 = the atomics above), for building their tails and the display
    struct HeardRoom { std::atomic<float> W { 14 }, L { 28 }, H { 11 }, Lx { 0.5f }, Ly { 0.4f }, absW { 0 }, absF { 0 }, highD { 0 }, scat { 0 }, rtLow { 0 }, rtHigh { 0 }; };
    std::array<HeardRoom, kNumRooms> heard;
    std::atomic<int> roomsUsed { 1 };                       // bit r = room r has sources
    std::array<std::atomic<uint32_t>, kNumRooms> tailSerialInUseR {};
    /** audio thread: the response of the Lucier passes (built in the background) */
    void setLucierIR (std::shared_ptr<const TailIR> ir) noexcept { lucier.setIR (std::move (ir)); }
    std::atomic<uint32_t> lucierSerialInUse { 0xffffffffu };
    std::atomic<float> lucierLevel { 0.0f };          // wet peak, for the display
    std::array<std::atomic<uint64_t>, kNumStops> soundingMask {};   // bit k = pipe k of that stop sounds

    void setModHook (ModHook* h) noexcept { modHook = h; }

    /** audio thread, before process(): the input of this block (left / right; right may be null = mono, both null = silence).
        The pointers must stay valid until process() returns. */
    void setInput (const float* l, const float* r, int numSamples) noexcept { inPtr[0] = l; inPtr[1] = r; inLen = numSamples; }
    // the input as placed right now, for the UI
    std::array<std::atomic<float>, 2> inShowAz {}, inShowEl {}, inShowDist {};
    std::atomic<int>   inShowCount { 0 };
    std::atomic<float> inPeak { 0.0f };          // raw input peak (before the level)

    // modulation state for the UI
    std::array<std::atomic<double>, kNumLfos> lfoPhaseOut {};
    std::array<std::atomic<float>, kNumSources> sourceOut {};
    std::array<std::array<std::atomic<float>, kNumKeys>, kNumStops> modAz, modEl, modDistMul;   // per pipe, while sounding

    /** ask the engine to play one pipe for ~1.5 s. Lock-free, UI thread. */
    void requestPreview (int stop, int key);

    /** position of a pipe right now (base + motion), for the UI */
    Pos currentPosition (int stop, int key, const EngineParams& p) const noexcept;

    /** position as heard: motion + per-pipe modulation (direction, height, distance). UI thread.
        Sounding pipes use the engine's own values (incl. per-pipe envelopes / random per note),
        silent pipes the LFOs, key position, mod wheel and global envelopes. */
    Pos displayPosition (int stop, int key, const EngineParams& p) const noexcept;

    /** integrated motion phase per stop (cycles); speed changes only change its slope */
    std::array<std::atomic<double>, kNumStops> motionPhase {};

    /** drawn trajectory of a stop (message thread; the engine and the UI read it without locking) */
    void setPath (int stop, const MotionPath& path) noexcept;
    const MotionPath& getPath (int stop) const noexcept { return paths[(size_t) stop][(size_t) pathActive[(size_t) stop].load()]; }
    MotionShape shapeFor (int stop, int key, const EngineParams& p) const noexcept;

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
        float sendAz0 = 0.0f, sendAz1 = 0.0f;   // direction used for the room sends
        bool fresh = true;
        // degradation
        DegradeState deg;
        float held1 = 0.0f, env = 0.0f;
        Wobble wobble, breath;
        float windSm = -1.0f;            // smoothed stop wind (-1 = take target directly)
        float lp = 0.0f, lpA = 1.0f;     // tone filter for under-winded pipes
        float lpAir = 0.0f, airA = 1.0f; // air absorption by distance
        WindChaos chaos;                 // wind model 2: resonances, intermittent jet, hiss
        float chAir = 0.0f;              // its own air filter
        float prevDist = -1.0f, dop = 1.0f, dopTarget = 1.0f;
        float detuneRnd = 0.0f;          // -1..1, fixed per pipe
        // modulation
        std::array<EnvState, kNumEnvs> menv {};
        float keyPos = 0.0f, randMod = 0.0f, velMod = 0.0f;
        int noteId = -1;                 // free tuning: channel * 128 + MIDI note (-1 = key mode)
        float bendSm = 0.0f;             // MPE: this note's own pitch bend in cents (smoothed)
        // hard jumps: gains switched sample-accurately at every jump (no ramp)
        bool hard = false;
        float pmAz = 0.0f, pmEl = 0.0f, pmDist = 1.0f, gainLin = 1.0f;
        float noteCents = 0.0f;          // free tuning: shift from the pipe's own pitch
        float modCents0 = 0.0f, modCents1 = 0.0f;
        // crackle
        int crkCount = 0;
        float crkAmp = 0.0f, crkEnv = 1.0f;
        // upper partials elsewhere
        float partLp = 0.0f, partSide = 1.0f;
        std::array<float, kMaxAmbiChannels> gh0 {}, gh1 {};
        // simulated room
        int erSlot = -1;
        float erBase = 0.0f;
        Pos erPos;
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
        float lp = 0.0f, lpA = 1.0f;          // tone (soft clicks are duller)
        std::array<float, kMaxAmbiChannels> g {};
        // placed / moving sounds (own sounds, tracker path)
        Pos where;                            // direction / distance
        float azVel = 0.0f;                   // deg per second, 0 = still
        std::array<float, kMaxRoomSends> sendW {};   // room-send weights (direction harmonics)
        int erSlot = -1;                      // simulated room: early reflections
    };

    // ---- simulated room ----
    static constexpr int kErSlots = 48;                  // sounds with their own early reflections at once
    static constexpr int kErRingBits = 16, kErRing = 1 << kErRingBits;
    static constexpr int kMaxTaps = 62;                  // image sources up to 3rd order
    static constexpr int kErCh = 16;                     // reflections are encoded up to 3rd order
    struct ErTap { float d0 = 0, d1 = 0, g0 = 0, g1 = 0, r0 = 1, r1 = 1, lp = 0; int ch = 4; std::array<float, kErCh> enc {}; };
    struct ErSlot
    {
        bool active = false, fresh = true;
        int voice = -1, noiseVoice = -1;      // owner (or both -1: the owner is gone, the reflections ring out)
        int input = -1;                       // ... or one of the input sources
        int drain = 0;
        int numTaps = 0;
        std::array<ErTap, kMaxTaps> taps {};
        // what the taps were computed from: unchanged (a pipe that does not move) = nothing to recompute
        static constexpr int kKey = 18;
        std::array<float, kKey> key {};
        bool keyValid = false;
    };

    struct Scheduled
    {
        int delay = 0;                        // samples
        bool voice = false;                   // true: start a pipe (stop, key), false: a sound
        int stop = 0, key = 0, noteId = -1;
        const Sample* sample = nullptr;
        float gain = 0.0f, tone = 1.0f, azVel = 0.0f, send = 0.0f;
        double rate = 1.0;
        Pos pos;
    };

    struct Leak
    {
        Pos pos;
        float freqMul = 1.0f, dir = 1.0f;
        float s1 = 0.0f, s2 = 0.0f, amp = 0.0f, ampTarget = 0.5f;
        int ampCount = 0;
        XorShift rng { 1 };
        std::array<float, kMaxAmbiChannels> g0 {}, g1 {};
    };

    void handleMidi (const juce::MidiMessage& m, const EngineParams& p);
    void keyDown (int key, float velocity, const EngineParams& p, int noteId = -1);
    void keyUp (int key, const EngineParams& p, int noteId = -1);
    void startVoice (int stop, int key, int previewSamples = -1, int noteId = -1);
    // free tuning
    void noteOnFree (const juce::MidiMessage& m, const EngineParams& p);
    void noteOffFree (const juce::MidiMessage& m, const EngineParams& p);
    void releaseAllKeys (const EngineParams& p);
    float centsFor (int noteId, int key) const noexcept;
    struct FreeNote { bool held = false, sustained = false; int key = -1; };
    std::array<FreeNote, NoteTable::kSize> freeNotes {};
    std::array<int, kNumKeys> keyNoteCount {};          // free notes held on each pipe
    std::array<float, kNumKeys> slotHz {};              // the pitch every key's own pipe sounds at
    const NoteTable* notesNow = nullptr;
    bool notesContinuous = true, freeMode = false;
    // MPE / expression: per MIDI channel (pitch bend -1..1, pressure, slide) and per note (polyphonic aftertouch)
    bool mpeNow = false;
    float mpeBendCents = 4800.0f;
    NoteTable mpeTable;                                  // MPE without a free tuning: the organ's own tuning as a note table
    std::array<float, 16> chBend {}, chPress {}, chSlide {};
    std::array<float, NoteTable::kSize> notePress {};
    std::array<float, kNumKeys> keyPress {};
    float lastPress = 0.0f, lastSlide = 0.0f, lastChanPress = 0.0f;
    float lastPressChannel() const noexcept { return lastChanPress; }
    int tableIndex (int noteId) const noexcept { return mpeNow ? (noteId & 127) : noteId; }   // MPE: one tuning for all channels
    float voicePressure (const Voice& v) const noexcept;
    float voiceSlide (const Voice& v) const noexcept;
    float voiceBendTarget (const Voice& v) const noexcept
    {
        return mpeNow && v.noteId >= 128 ? chBend[(size_t) (v.noteId >> 7)] * mpeBendCents : 0.0f;   // (channel 1 is the master channel)
    }
    void releaseVoice (Voice& v);
    void freezeVoice (Voice& v);
    void releaseStopKey (int stop, int key, int noteId = -1);
    void playNoise (const std::vector<NoiseSample>& pool, float gain, const float* gains, int& lastIndex, float send, float tone = 1.0f);
    void spawnSound (const Sample& s, float gain, float tone, double rate, Pos pos, float azVel, float send);
    void schedule (const Scheduled& e);
    void customSounds (int key, float velocity, const EngineParams& p);
    void trackerPath (int key, float velocity, const EngineParams& p);
    void placedGains (NoiseVoice& nv) noexcept;
    void renderLeaks (juce::AudioBuffer<float>& out, int n, const EngineParams& p);
    void renderRoom (juce::AudioBuffer<float>& out, int n, const EngineParams& p);
    struct RoomTail;
    void renderFdn (RoomTail& R, float* const* out, int K, const float* in, int n, float rtL, float rtH, float gainTarget, const RoomParams& r);
    void resetFdn (RoomTail& R) noexcept;
    int  allocSlot (int voice, int noiseVoice) noexcept;
    void releaseSlot (int& slot) noexcept;
    void updateSlotTaps (ErSlot& s, Pos srcPos, float base, const EngineParams& p, const RoomParams& room) noexcept;
    void renderEchoes (juce::AudioBuffer<float>& out, int n, const EngineParams& p);
    void renderInput (juce::AudioBuffer<float>& out, int n, const EngineParams& p) noexcept;
    void followInput (int start, int len, const EngineParams& p) noexcept;
    const std::vector<NoiseSample>& customPool() const noexcept;
    static float clickCurve (float v) noexcept { return 0.04f + 0.96f * v * std::sqrt (v); }          // velocity -> level
    float clickTone (float v) const noexcept                                                          // velocity -> one-pole coefficient
    {
        const float fc = 900.0f * std::pow (20000.0f / 900.0f, juce::jlimit (0.0f, 1.0f, v * 1.25f));
        return fc >= 19999.0f ? 1.0f : 1.0f - std::exp (-juce::MathConstants<float>::twoPi * fc / (float) sr);
    }
    std::array<float, kNumKeys> keyVelocity {};
    std::array<int, kNumKeys> keySource {};             // which recorded pipe a key plays (step tunings), from the last params
    void gainsFor (Pos pos, float* out) const noexcept;
    void renderRange (juce::AudioBuffer<float>& out, int start, int end, int blockLen, const EngineParams& p);
    void updateVoiceSpatial (Voice& v, const EngineParams& p, double blockSeconds) noexcept;
    float readSample (const float* d, int len, double pos) const noexcept;
    void advanceModulation (const EngineParams& p, double dt) noexcept;
    float voiceSource (const Voice& v, int src, const EngineParams& p) const noexcept;
    float grainSample (Voice& v, double rate, float smear) noexcept;

    std::shared_ptr<const SampleLibrary> library;
    std::vector<Voice> voices;
    std::vector<NoiseVoice> noise;
    std::array<bool, kNumKeys> keyHeld {}, keySustained {};
    bool sustainPedal = false;
    std::array<bool, kNumStops> lastStopOn {};
    bool firstBlock = true;
    /** positions/gains are updated every kSubBlock samples so fast motion (up to 10 Hz) stays smooth */
    static constexpr int kSubBlock = 64;
    void processChunk (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi, const EngineParams& p);
    juce::MidiBuffer chunkMidi;
    ModHook* modHook = nullptr;
    std::array<double, kNumLfos> lfoPhase {};
    std::array<EnvState, kNumEnvs> globalEnv {};
    std::array<float, kNumSources> globalSrc {};
    bool globalTrig = false;
    // audio input
    const float* inPtr[2] {};
    int inLen = 0;
    static constexpr int kInRingBits = 17, kInRing = 1 << kInRingBits;   // delay by distance (Doppler), up to ~1.4 s
    std::vector<float> inRing;                                           // [2][kInRing]
    uint32_t inClock = 0;
    struct InSrc
    {
        std::array<float, kMaxAmbiChannels> g0 {}, g1 {};
        float send0 = 0.0f, send1 = 0.0f, sendAz0 = 0.0f, sendAz1 = 0.0f;
        float d0 = 0.0f, d1 = 0.0f, lp = 0.0f, airA = 1.0f;
        Pos pos;
        int erSlot = -1;
        bool fresh = true;
    };
    std::array<InSrc, 2> inSrc {};
    float inGate = 0.0f, inLevelSm = 0.0f, inEnv = 0.0f, inFollowVal = 0.0f;
    double inSpin = 0.0;
    bool inRunning = false;
    int inDrain = -1;
    // Lucier mode
    TailConvolver lucier;
    std::vector<float> lucIn, lucWet;
    float lucDry = 1.0f, lucWetGain = 0.0f;
    float lucInEnv = 0.0f, lucOutEnv = 0.0f, lucAgc = 1.0f;     // the resonances follow the organ's loudness
    int lucRing = 0;
    bool lucRunning = false;
    void renderLucier (juce::AudioBuffer<float>& out, int n, const EngineParams& p) noexcept;
    float lastKeyPos = 0.0f, lastRandom = 0.0f, modWheel = 0.0f, lastVelocity = 0.0f, pitchWheel = 0.0f;
    ModParams lastMod;
    bool haveLastMod = false;
    int roomOffset = 0;
    double sr = 48000.0;
    int maxBlock = 512;
    int usedChannels = 2;
    int order = 0;                      // 0 = stereo
    uint32_t ageCounter = 0;
    XorShift rng { 4711 };
    int lastDown = -1, lastUp = -1, lastPull = -1;
    std::vector<float> scratch, scratchRamp;
    std::array<std::vector<float>, kMaxRoomSends> roomSend;
    int roomSends = 1;                                  // used this block: 2 * roomHarmonics + 1
    std::array<float, kMaxRoomSends> noiseRoomW {};     // room weights of the click / pull sounds

    // effects
    std::array<Scheduled, 512> scheduled {};
    int numScheduled = 0;
    std::array<Leak, 8> leaks {};
    std::vector<float> echoBuf, fxScratch;
    int echoWrite = 0;
    double echoDelay = -1.0;                          // smoothed delay (samples)
    std::array<float, 8> echoLp {};
    double partRot = 0.0;                             // rotation of the upper partials (deg)
    float distAmountNow = 0.7f, chunkSeconds = 0.0f;
    bool roomFollowNow = true;
    float roomBaseAzNow = 0.0f;
    const std::vector<NoiseSample> noSounds;
    int lastCustom = -1;

    // simulated rooms: the late reverb of every room (convolution with a ray-traced response, or a feedback delay network)
    static constexpr int kFdnLines = 16, kFdnBits = 16, kFdnLen = 1 << kFdnBits;
    struct RoomTail
    {
        TailConvolver tail;                          // late reverb (ray-traced, synthesised, convolved)
        float tailGain = 0.0f;
        bool tailWasOn = false;
        // the other late reverb: a 16-line feedback delay network (decay from the ray tracer)
        std::vector<float> fdnBuf, fdnPre;           // [line][kFdnLen], input pre-delay
        uint32_t fdnPos = 0;
        std::array<float, kFdnLines> fdnDelay {}, fdnLp {}, fdnGL {}, fdnGH {};
        std::array<std::array<float, kMaxAmbiChannels>, kFdnLines> fdnEnc {};
        float fdnPreDelay = -1.0f, fdnGain = 0.0f, fdnLevelLast = 0.0f;
        std::array<float, kFdnLines> fdnModC {}, fdnModS {};
        int fdnEncOrder = -1;
        // switching between the two: the one switched off rings out on its own (input silent)
        int convRingOut = 0, fdnRingOut = 0, lastTailModel = 0;
        std::vector<float> send;                     // rooms 1..3: the sources in this room (room 0 = all others)
        bool running = false;
    };
    std::array<RoomTail, kNumRooms> rt;
    std::array<std::array<float, kFdnLines>, kFdnLines> fdnMatrix {};      // random orthogonal mixing matrix
    std::array<float, kFdnLines> fdnModStepC {}, fdnModStepS {};           // slow delay modulation (phasor steps)
    std::vector<float> zeroIn, room0In;
    std::array<int, kNumStops> stopRoomNow {};
    int inputRoomNow = 0;
    // notes to single pipes
    uint32_t spreadCounter = 0;
    int lastSpreadStop = -1;
    int spreadStopFor (int key, const EngineParams& p) noexcept;
    std::vector<ErSlot> erSlots;
    std::vector<float> erRing;                       // [slot][kErRing]
    uint32_t erClock = 0;
    // drawn paths, double-buffered (the writer fills the inactive one, then flips)
    std::array<std::array<MotionPath, 2>, kNumStops> paths {};
    std::array<std::atomic<int>, kNumStops> pathActive {};
    std::array<double, kNumStops> phasePrev {}, phaseNow {};       // motion phase at the start / end of this block
    void hardJumpGains (const Voice& v, const EngineParams& p, double phase, float* g, float* gh) const noexcept;
    bool roomOnNow = false;
    float consoleAzNow = 0.0f, consoleElNow = -10.0f;

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
