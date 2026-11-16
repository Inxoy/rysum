#pragma once

#include "Common.h"
#include "Degrade.h"
#include "MeshAcoustics.h"
#include "Modulation.h"
#include "RoomSim.h"
#include "RoomTail.h"
#include "SampleLibrary.h"
#include "SpaceFx.h"
#include "Spatial.h"
#include "SpatialDegrade.h"
#include "WindChaos.h"
#include "AudioWorkers.h"
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <vector>

namespace rysum
{
/** the "Effekte" page: sounds and effects that are easy to locate in space */
/** the sound of the pipe itself, per stop (Raum page, tab "Pfeife"); all neutral by default */
struct PipeSoundParams
{
    // Ansprache: the measured attack of every pipe (onset .. attack end from the preprocessing)
    float attGain =
        0.0f; // -1 = the attack swells in softly .. 0 = as recorded .. +1 = +12 dB chiff
    float attSharp = 0.0f; // 0..1: the highs of the attack lifted (a sharper chiff)
    float attIso = 0.0f;   // 0..1: the sustain falls away after the attack (1 = only the chiff)
    float attRep = 0.0f;   // 0..1: the chiff repeats as a short burst (up to 6 times)
    // Abriss: a puff of air when the pallet closes
    float puff = 0.0f;
    // upper partials above hiFreq as their own layer: gain and transient shaping
    float hiFreq = 4000.0f, hiGain = 0.0f /* dB */, hiShape = 0.0f /* -1 soft .. +1 spiky */;
    bool attOn() const noexcept
    {
        return attGain != 0.0f || attSharp > 0.0f || attIso > 0.0f;
    }
    bool hiOn() const noexcept
    {
        return hiGain != 0.0f || hiShape != 0.0f;
    }
};

/** "Verfall · Glitch": transients of a failing playback (per pipe); all off by default */
struct GlitchParams
{
    float stutter = 0.0f; // a short slice of the pipe repeats 2..6 times
    float splice = 0.0f;  // loop points become hard tape splices (with a tick)
    float zero = 0.0f;    // no care for zero crossings: hard note starts and releases
    float clip = 0.0f;    // short bursts of hard digital clipping
    bool on() const noexcept
    {
        return stutter > 0.0f || splice > 0.0f || zero > 0.0f || clip > 0.0f;
    }
};

struct FxParams
{
    // own sounds (samples/noise/eigene), triggered by the keys
    float custLevel = 0.0f, custDown = 1.0f, custUp = 0.0f, custPitch = 0.0f, custScatter = 0.0f,
          custWander = 0.0f;
    int custPlace = 0; // 0 at the pipe, 1 random, 2 console, 3 circling
    // tracker path: ticks running from the console to the pipe
    float pathLevel = 0.0f, pathTime = 0.12f, pathSpread = 12.0f;
    int pathTicks = 6;
    bool pathWait = false; // pipes speak when the path arrives
    // crackle at every sounding pipe
    float crackLevel = 0.0f, crackRate = 6.0f, crackLen = 1.2f; // Hz per pipe, ms
    // wind leaks: hissing spots
    float leakLevel = 0.0f, leakFreq = 4000.0f, leakWander = 0.0f;
    int leakCount = 3;
    bool windRound = false; // blower noise from all sides (instead of one omni signal)
    // upper partials placed away from the pipe
    float partSpread = 0.0f, partSplit = 3.0f, partTurn = 0.0f; // deg, x fundamental, Hz
    // echoes, every repeat from another direction
    float echoLevel = 0.0f, echoTime = 0.35f, echoFeedback = 0.6f, echoTurn = 90.0f,
          echoTilt = 0.0f, echoDamp = 0.3f;
    bool echoSync = false;
    int echoDiv = 4, echoRepeats = 4;
};

/** the audio input ("Eingang"): live sound placed and moved in the same space as the pipes */
struct InputParams
{
    bool on = false;
    float level = 1.0f; // linear
    bool stereo = true; // two sources (left / right) or one (the sum)
    float az = 0.0f, el = 10.0f,
          dist = 4.0f;    // deg, deg, m (own place; following a stop: a turn on top of it)
    float width = 60.0f;  // deg between left and right
    float spin = 0.0f;    // deg per second, turning around the listener
    int follow = -1;      // -1 = own place, else: rides on the pipes of this stop (and its motion)
    bool gate = false;    // only sounds while keys are held
    float release = 0.3f; // s, gate closing
    float send = 1.0f;    // room send, linear (1 = like a pipe)
};

/** free tuning (Scala file, MTS-ESP / MIDI Tuning Standard): the frequency of every MIDI note on
 * every channel */
struct NoteTable
{
    static constexpr int kSize = 16 * 128; // [channel 0..15][note 0..127]
    std::array<float, kSize> hz{};         // Hz
    std::array<uint8_t, kSize>
        mapped{}; // 0 = this note does not sound (unmapped in the .kbm / by the master)
};

/** "Lebendige Orgel": the organ as one physical thing - a wind supply shared by all pipes, pallets
   that open as the key is played, a listener who walks, pipes that wear out, silent pipes that ring
   along, a tuning that pulls itself pure, and a walker in the Tonnetz. Everything off by default.
 */
struct LiveParams
{
    // Balg: one finite wind supply for all stops
    float bellows = 0.0f;     // 0 = endless wind (as before) .. 1 = big chords drain it quickly
    float bellowsSize = 4.0f; // s: how long the bellows lasts / how slowly the blower answers
    int blower = 0; // 0 = blower (steady feed), 1 = Kalkant (treads, free), 2 = Kalkant in tempo
    float treadRate = 20.0f; // treads per minute (Kalkant, free)
    int treadDiv = 3;        // Kalkant in tempo: a tread every 8 / 4 / 2 / 1 bars, 1/2, 1/4
    float treadNoise = 0.3f; // the thump of the tread at the console
    // Ventil: how far the pallet opens
    float pallet = 0.0f; // 0 = always fully open
    int palletSrc = 0;   // 0 = velocity (a slow key opens slowly), 1 = pressure, 2 = slide (CC 74)
    // the listener walks (metres from the usual place; x = to the right, y = forward)
    float listenX = 0.0f, listenY = 0.0f;
    int walkShape =
        0; // 0 = stands, 1 = circle, 2 = left / right, 3 = forward / back, 4 = figure eight
    float walkRate = 0.05f, walkSize = 3.0f; // rounds per second, metres
    // Verschleiss: pipes wear out by being played (the engine keeps the wear per pipe)
    float wear = 0.0f;        // how strongly the wear is heard
    float wearMinutes = 5.0f; // minutes of sounding until a pipe is fully worn
    // sympathetic pipes: silent pipes ring when a partial of a sounding one meets their pitch
    float sympLevel = 0.0f, sympDecay = 0.6f; // linear, s
    // Reinheit: held chords pull themselves towards pure intervals
    float pure = 0.0f, pureTime = 1.5f; // 0..1, s
    // Tonnetz-Wanderer: while keys are held, a walker steps through the lattice of fifths and
    // thirds and plays what it passes
    bool netOn = false;
    int netPath = 0; // 0 fifths, 1 chain of thirds, 2 random, 3 random that comes home, 4 spiral
    int netDiv = 0;  // 0 = free (netRate), else a note value
    float netRate = 3.0f, netGate = 0.9f; // steps per second, note length in steps
    // Aufprall: pipes flying through the Saal (the room motion modes) knock against walls, floor
    // and ceiling
    float impLevel = 0.0f; // linear (0 = silent)
    bool impAll = false;   // false: only sounding pipes knock, true: every pipe of a pulled stop
    float impRing = 0.6f;  // s: how long the struck pipe rings
    // Spieler: the listener (walking in the 3D view, or on its automatic way) moves the pipes that
    // are bodies in the Saal (the room motion modes; "Schweben" is made for it)
    int playerMode = 0; // 0 = nothing, 1 = pulls them, 2 = pushes them away, 3 = they circle,
                        // 4 = drags them along in its wake
    float playerForce = 0.5f; // 0..1
    float playerRange = 4.0f; // m: within this the force is strong
    // Selbstspiel: a pipe that knocks against a wall sounds its own note - the room plays itself
    float selfPlay = 0.0f; // 0 = off .. 1 = long notes
    // Wachsen & Brechen: pipes left alone grow longer and sink in pitch; a hard knock breaks one
    // in two (an octave up)
    float grow = 0.0f; // 0 = off (every pipe finds back to its length) .. 1 = fast, brittle
    // Fluestern: a pipe right at the head is heard in the ear itself (headphones / stereo)
    float whisper = 0.0f; // 0..1
    // Unmoeglicher Raum: the late reverb is that of a room of another size than the one seen and
    // walked; near a wall it grows further
    float roomHear = 1.0f; // times the size
    float wallPull = 0.0f; // 0..1
    // Zeit als Entfernung: the far end of the room plays the past (done by the plugin, after the
    // room): seconds heard back at the front wall
    float timeDepth = 0.0f;
};

struct EngineParams
{
    /** threads the audio may use for the Saal's reflections: 0 = as many as suit the processor
     *  (AudioWorkers::wantedThreads), 1 = only the audio thread, 2 ... = so many */
    int audioThreads = 0;
    std::array<bool, kNumStops> stopOn{};
    std::array<float, kNumStops> stopGain = ones(); // linear
    std::array<float, kNumStops> stopWind = ones(); // 0 = stop closed (silent) .. 1 = fully pulled
    int windModel = 0; // 0 = classic, 1 = unstable (whistles, intermittent speech at low stop wind)
    static std::array<float, kNumStops> ones()
    {
        std::array<float, kNumStops> a{};
        a.fill(1.0f);
        return a;
    }
    float masterGain = 1.0f;
    int outputMode = 0;       // 0 = stereo, 1..5 = AmbiX order
    float clickLevel = 0.65f; // key-down click
    float clickBlend =
        0.45f; // 0 = clicks at the console, 1 = clicks at the pipes (into the attack)
    float releaseClick = 0.4f; // key-up click
    float stopPull = 0.7f;     // stop pull / push sound
    float wind = 0.15f; // global wind: blower noise, above ~35 % increasingly unstable (gusts)
    float consoleAz = 0.0f, consoleEl = -10.0f;
    DegradeParams deg;
    SpaceDegParams space;                       // "Raumverfall": degradation of the spatial image
    std::array<CloudParams, kNumStops> cloud{}; // "Körnerwolke" per stop

    // movement
    std::array<int, kNumStops> motionMode{};
    std::array<float, kNumStops> motionRate{};
    std::array<float, kNumStops> motionDepth{};
    std::array<float, kNumStops> ellipse{};     // 0..0.9: elliptic orbit, closer on the near side
    std::array<float, kNumStops> ellipseDir{};  // deg: direction of the near side
    std::array<float, kNumStops> ellipseTilt{}; // deg: the orbit leans up towards the near side
    std::array<int, kNumStops> ellipseLobes = onesInt(); // 1..4 near sides
    std::array<float, kNumStops> pathSpread =
        ones();                             // drawn path: how far the pipes are spread along it
    std::array<bool, kNumStops> jumpHard{}; // jumps without any glide (they click)
    static std::array<int, kNumStops> onesInt()
    {
        std::array<int, kNumStops> a{};
        a.fill(1);
        return a;
    }
    bool sync = false; // speeds relative to 120 BPM, locked to the host position
    double bpm = 120.0, ppq = 0.0;
    bool playing = false;
    // the Verlauf (automation) clock: beats at the start of the block / slice, beats per second
    double autoBeats = 0.0, autoBeatRate = 0.0;
    // ... and what it plays besides the parameters: per-pipe lanes, timed paths (nullptr = none)
    const struct AutoEngineView *autoView = nullptr;

    // distance
    float doppler = 1.0f; // 0 = off, 1 = physical, up to 4 = exaggerated
    float air = 0.5f;     // air absorption amount
    float airInvert =
        0.0f; // 0 = natural, 0.5 = the highs of distant pipes restored, 1 = exaggerated
    // resonated clicks: key clicks ring a bank of resonators tuned to the harmonics of the pressed
    // key
    float resClick = 0.0f, resDecay = 0.4f /* s */, resBright = 0.5f;
    std::array<PipeSoundParams, kNumStops> pipeSnd{};
    GlitchParams glitch;
    // tuning made audible in space: beats ("Schwebung", motion mode and modulation source) and
    // difference tones
    bool beatIdeal =
        true; // beats from the tuning as it should be (true) or from the pipes as recorded
    float ghostLevel = 0.0f; // difference tones of sounding pairs (linear, 0 = off)
    float ghostPlace = 0.3f; // 0 = between the two pipes .. 1 = deep below the listener
    float ghostCons =
        0.5f; // 0 = every pair .. 1 = only pairs with simple ratios (fifths, octaves, thirds)
    // "Eis": dispersive walls - every early reflection becomes a falling chirp
    float ice = 0.0f, iceLen = 0.5f;
    // photographic operations on the whole field (applied by the processor after the room, see
    // applyPhotoFx)
    DoubleExposureParams dex;
    LongExposureParams expo;
    float distAmount = 0.7f; // 0 = distance does not change the level, 1 = 1/r

    // tuning
    bool correctSamplePitch = false;
    bool extendedRange = false;
    KeyTuning tuning = makeKeyTuning(
        0); // per key: shift (cents) from the recorded pipe that is played, and which pipe
    /** not null: free tuning - every MIDI note (and channel) has its own pitch; the recorded pipe
       nearest to it plays (several notes may share one pipe, each with its own voice) */
    const NoteTable *notes = nullptr;
    bool notesContinuous = true; // held notes follow tuning changes (MTS-ESP)
    // Lucier mode: the whole output passes through the room n times (the response is built by the
    // processor)
    bool lucierOn = false;
    float lucierMix = 0.5f; // 0 = only the organ .. 1 = only the room's resonances
    // MPE: every note on its own MIDI channel (2..16; channel 1 = the whole instrument), bent,
    // pressed and slid on its own
    bool mpe = false;
    float mpeBendRange = 48.0f; // semitones for a full bend on a note channel
    double pitchCents = 0.0;    // whole organ
    float detune = 0.0f;        // max random detune per pipe (cents)

    // tremulant
    bool tremOn = false;
    float tremRate = 5.5f, tremDepth = 0.5f;

    // freeze
    bool freeze = false;
    float freezeSmear = 0.5f, freezeRelease = 2.0f;

    // modulation (LFOs, envelopes, routings)
    ModParams mod;

    FxParams fx;
    LiveParams live;

    InputParams input;

    // simulated room ("Saal"): early reflections per pipe + late reverb. Up to four rooms: every
    // stop and the input sound in one of them (room 0 = "Saal 1", everything else - clicks, effects
    // - is in it too)
    RoomParams room;
    std::array<RoomParams, kNumRooms - 1> moreRooms{};
    std::array<int, kNumStops> stopRoom{};
    int inputRoom = 0;
    const RoomParams &roomAt(int i) const noexcept
    {
        return i <= 0 ? room : moreRooms[(size_t)juce::jmin(kNumRooms - 2, i - 1)];
    }
    /** notes to single pipes: 0 = every pulled stop sounds (as an organ), 1 = one stop per note in
       turn, 2 = by position among the held notes (lowest -> first stop), 3 = random */
    int noteSpread = 0;

    // room sends: circular harmonics of every pipe's direction (see Convolver)
    int roomHarmonics = 0;   // 0 = only the plain sum
    bool roomFollow = true;  // the room's early reflections turn with each pipe
    float roomBaseAz = 0.0f; // direction used for all pipes when not following
};

/** called by the engine before every 64-sample slice: lets the processor modulate any parameter.
    'globalSources' holds kNumSources values (LFOs without spread, global envelopes, last key, mod
   wheel, last random). */
struct ModHook
{
    virtual ~ModHook() = default;
    virtual void modulate(EngineParams &p, const float *globalSources) = 0;
};

class OrganEngine
{
public:
    OrganEngine();

    void prepare(double sampleRate, int maxBlockSize);

    /** call from the audio thread only (the processor swaps the library in at block start) */
    void setLibrary(std::shared_ptr<const SampleLibrary> lib);

    std::array<double, kNumKeys> recordedPipeFrequencies(int stop) const
    {
        std::array<double, kNumKeys> frequencies{};
        if (library != nullptr && stop >= 0 && stop < kNumStops)
        {
            const auto &pipes = library->pipes[static_cast<std::size_t>(stop)];
            for (std::size_t key = 0; key < frequencies.size(); ++key)
                if (pipes[key].valid)
                    frequencies[key] = pipes[key].rawReferenceHz * pipes[key].nativePitchRatio;
        }
        return frequencies;
    }
    const SampleLibrary *getLibrary() const noexcept
    {
        return library.get();
    }

    /** renders one block. 'out' is cleared first. MIDI events are handled sample-accurately. */
    void process(juce::AudioBuffer<float> &out, const juce::MidiBuffer &midi,
                 const EngineParams &p);

    /** sends to the room of the last block: [0] all pipes (weighted by distance), [2m-1] / [2m] the
       same weighted by cos / sin (m * pipe azimuth) */
    static constexpr int kMaxRoomSends = 2 * kMaxOrder + 1;
    const float *getRoomSend(int k = 0) const noexcept
    {
        return roomSend[(size_t)k].data();
    }

    // ---------------- shared with the UI (any thread) ----------------
    std::array<std::array<std::atomic<float>, kNumKeys>, kNumStops> baseAz, baseEl, baseDist;
    std::array<std::array<std::atomic<bool>, kNumKeys>, kNumStops> muted;
    std::atomic<double> timeSeconds{0.0};
    std::atomic<uint64_t> heldMask{0}; // bit k = physical key k held
    std::atomic<int> activeVoices{0}, frozenVoices{0};
    std::atomic<float> meterL{0.0f}, meterR{0.0f};
    /** where the time goes (the benchmark switches it on): milliseconds per stage since it was
     *  cleared - 0 before the pipes, 1 the pipes, 2 live / impacts / leaks / input / echoes,
     *  3 early reflections (all of it), 4 late reverb, 5 the reflections' geometry (part of 3),
     *  6 the pipes as bodies in the Saal (part of 0), 7 the pipes' places and gains (part of 0) */
    bool profOn = false;
    /** how many threads the audio can use at most on this machine */
    int audioThreadsAvailable() const noexcept
    {
        return audioWorkers->threads();
    }
    std::array<double, 16> prof{}; // (8 .. 11: the parts of 7 - where, around the corner, pitch, gains)
    // simulated room as heard right now (with modulation), for the Saal page
    std::atomic<float> roomW{14.0f}, roomL{28.0f}, roomH{11.0f}, roomLx{0.5f}, roomLy{0.4f},
        roomRtLow{0.0f}, roomRtHigh{0.0f}, roomCritDist{0.0f};
    /** how long the late reverb of the rooms in use rings after the last note (s; for the host) */
    std::atomic<float> roomTailSeconds{0.0f};
    std::atomic<int> lastStruckStop{-1}, lastStruckKey{-1}, erActive{0};
    // ...and the rest of it, for building the late reverb (processor, message thread)
    std::atomic<float> roomAbsW{0.0f}, roomAbsF{0.0f}, roomHighD{0.0f}, roomScat{0.0f},
        roomAir{0.5f};
    std::atomic<int> roomOutOrder{0};
    std::atomic<bool> roomOn{false};
    std::atomic<uint32_t> tailSerialInUse{0xffffffffu};
    /** audio thread: the late reverb for a room (built in the background) */
    void setTailIR(std::shared_ptr<const TailIR> ir, int roomIndex = 0) noexcept
    {
        rt[(size_t)juce::jlimit(0, kNumRooms - 1, roomIndex)].tail.setIR(std::move(ir));
    }
    // the other rooms as heard (rooms 1..3; room 0 = the atomics above), for building their tails
    // and the display
    struct HeardRoom
    {
        std::atomic<float> W{14}, L{28}, H{11}, Lx{0.5f}, Ly{0.4f}, absW{0}, absF{0}, highD{0},
            scat{0}, rtLow{0}, rtHigh{0}, shapeAmt{0.5f};
        std::atomic<int> shape{0};
        // "Unmoeglicher Raum": the late reverb is built for the room this many times as large
        std::atomic<float> tailScale{1.0f};
    };
    /** the drawn floor plan of a room (message thread writes, the audio thread reads; double
     * buffered) */
    void setDrawnPlan(int room, const FloorPlan &f) noexcept
    {
        const int r = juce::jlimit(0, kNumRooms - 1, room);
        const int next = 1 - drawnPlanActive[(size_t)r].load();
        drawnPlans[(size_t)r][(size_t)next] = f;
        drawnPlanActive[(size_t)r].store(next);
    }
    const FloorPlan &getDrawnPlan(int room) const noexcept
    {
        const int r = juce::jlimit(0, kNumRooms - 1, room);
        return drawnPlans[(size_t)r][(size_t)drawnPlanActive[(size_t)r].load()];
    }
    std::array<HeardRoom, kNumRooms> heard;
    std::atomic<int> roomsUsed{1}; // bit r = room r has sources
    std::array<std::atomic<uint32_t>, kNumRooms> tailSerialInUseR{};
    /** audio thread: the response of the Lucier passes (built in the background) */
    void setLucierIR(std::shared_ptr<const TailIR> ir) noexcept
    {
        lucier.setIR(std::move(ir));
    }
    std::atomic<uint32_t> lucierSerialInUse{0xffffffffu};
    std::atomic<float> lucierLevel{0.0f}; // wet peak, for the display
    std::array<std::atomic<uint64_t>, kNumStops>
        soundingMask{}; // bit k = pipe k of that stop sounds
    /** the wind a sounding pipe gets right now (0..1: its stop's draw, the bellows, its pallet;
        only valid while its bit in soundingMask is set) - the displays light a pipe by it */
    std::array<std::array<std::atomic<float>, kNumKeys>, kNumStops> windNow{};
    float windShown(int stop, int key) const noexcept
    {
        return juce::jlimit(0.0f, 1.0f, windNow[(size_t)stop][(size_t)key].load(std::memory_order_relaxed));
    }

    // ---- "Lebendige Orgel", for the display and the project state
    std::atomic<float> bellowsNow{1.0f},
        bellowsFillNow{1.0f};       // wind pressure, how full the bellows is (0..1)
    std::atomic<int> treadCount{0}; // counts the Kalkant's treads
    std::atomic<float> listenerXNow{0.0f},
        listenerYNow{0.0f}; // where the listener is right now (m)
    std::array<std::array<std::atomic<float>, kNumKeys>, kNumStops>
        wear;                                   // 0 = new .. 1 = worn out (saved with the project)
    std::atomic<bool> wearReset{false};         // UI: all pipes new again
    /** Wachsen & Brechen: the length of every pipe, 1 = as built (0.25 .. 2; the pitch follows:
        half the length sounds an octave higher) */
    std::array<std::array<std::atomic<float>, kNumKeys>, kNumStops> pipeLen;
    std::atomic<bool> pipeLenReset{false}; // UI: every pipe whole again
    std::atomic<uint32_t> breakCount{0};   // pipes broken so far
    /** Fluestern: where the head looks (degrees, to the left), set before process(); and the two
        ear signals of this block, to be added after the binaural decoding (nullptr = nothing) */
    std::atomic<float> headYaw{0.0f};
    /** Fluestern: the ear signals are used (binaural or stereo output); false = nothing is done */
    std::atomic<bool> nearHeard{true};
    const float *nearBus(int ear) const noexcept
    {
        return nearUsed ? (ear == 0 ? nearL.data() : nearR.data()) : nullptr;
    }
    float *nearBusWritable(int ear) noexcept
    {
        return ear == 0 ? nearL.data() : nearR.data();
    }
    std::atomic<int> sympNow{0}, netKeyNow{-1}; // pipes ringing along; the key the walker sounds
    std::atomic<float> pureNow{0.0f};           // largest correction right now (cents)
    // ---- "Begehen" (the 3D view): while it is open the camera is the listener. Offset from the
    // listener's place
    //      in the room (m, like listenX / listenY) and where the head points (degrees, + = left,
    //      like binYaw)
    std::atomic<bool> viewWalkOn{false};
    std::atomic<float> viewX{0.0f}, viewY{0.0f}, viewYaw{0.0f};
    std::atomic<float> viewZ{0.0f}; // m above standing on the Saal's floor (a jump, a step)
    /** a loaded room model (Saal 1): pipes behind its walls are heard around them. Message thread;
        the caller keeps the model alive for as long as the engine may use it. */
    void setMesh(std::shared_ptr<const MeshAcoustics> m) noexcept
    {
        std::atomic_store(&meshShared, std::move(m));
    }
    std::atomic<bool> meshActive{false}; // the model hides pipes right now (for the display)
    /** the model as the flying pipes see it (Saal 1): they bounce off it whether it is heard or
        not. Message thread; the caller keeps it alive as for setMesh. */
    void setBodyMesh(std::shared_ptr<const MeshAcoustics> m) noexcept
    {
        std::atomic_store(&bodyMeshShared, std::move(m));
    }

    // ---- the player's actions (the keys of the 3D view). Message thread -> audio thread.
    enum class Act
    {
        Shock,  // a burst at the player's place: every pipe that is a body in the Saal is pushed away
        Home,   // every pipe flies back to its place in the organ
        Throw,  // the pipe (stop, key) let go with the speed v (m/s) added
        Strike, // the pipe (stop, key), at the place v, knocked: it rings without its key
        Clap,   // a hand clap where the player stands: the room answers
        Wind,   // every pipe that is a body in the Saal gets the speed v (m/s) added (the audience)
        Arrange, // the pipes marked in goMask fly to goX/goY/goZ and stay there (formations, layouts, undo)
        Whirl,   // a whirlwind at the place v for a few seconds: the pipes near it are spun around it (amount: strength)
        BowOn,   // the pipe (stop, key) sounds like a held key, for as long as the hand holds it
        BowOff   // ... and is let go
    };
    void playerAct(Act a, int stop = -1, int key = -1, Vec3f v = {}, float amount = 1.0f) noexcept;
    std::atomic<bool> actFreeze{false};      // held: everything that moves stands still
    std::atomic<int> grabStop{-1}, grabKey{-1}; // the pipe carried by the player (-1: none)
    std::atomic<float> grabX{0.0f}, grabY{0.0f}, grabZ{0.0f}; // where the hand holds it (m, in its room)
    std::atomic<int> hiddenNow{0}; // sounding pipes heard around a corner (out of sight)
    // arranging: where each pipe is to fly (m, in its room), and which pipes take part; the message thread
    // writes them, then sends Act::Arrange
    std::array<std::array<std::atomic<float>, kNumKeys>, kNumStops> goX, goY, goZ;
    std::array<std::atomic<uint64_t>, kNumStops> goMask{};
    // the magnet (the right mouse button held): +1 draws the pipes to the point, -1 drives them away, 0 off;
    // only the pipes in magnetMask (all of them while it is all ones)
    std::atomic<int> magnetMode{0};
    std::atomic<float> magnetX{0.0f}, magnetY{0.0f}, magnetZ{0.0f};
    std::array<std::atomic<uint64_t>, kNumStops> magnetMask{};
    // pinned pipes stay where they are, whatever pushes them (bit = key); held: everything stands still
    // (P); the pipes' time runs at this pace (Z held: slow motion)
    std::array<std::atomic<uint64_t>, kNumStops> pinMask{};
    std::atomic<bool> actPause{false};
    std::atomic<float> bodyPace{1.0f};

    // ---- room motion (Flug, Fall, Schwarm, Strömung): every pipe a body in the Saal, for the
    // display
    /** where the pipe is in its room (m) - true while its stop moves in the room */
    bool bodyAt(int stop, int key, Vec3f &out) const noexcept
    {
        if (stop < 0 || stop >= kNumStops || key < 0 || key >= kNumKeys ||
            !bodyOn[(size_t)stop].load())
            return false;
        out = {bodyX[(size_t)stop][(size_t)key].load(), bodyY[(size_t)stop][(size_t)key].load(),
               bodyZ[(size_t)stop][(size_t)key].load()};
        return true;
    }
    std::array<std::array<std::atomic<float>, kNumKeys>, kNumStops> bodyX, bodyY, bodyZ;
    std::array<std::atomic<bool>, kNumStops> bodyOn{};
    std::array<std::atomic<float>, kNumRooms> homeX{}, homeY{},
        homeZ{}; // the usual listening place per room
    /** the last impacts (a ring of 16; impactCount counts them all) */
    struct ImpactShow
    {
        std::atomic<float> x{0}, y{0}, z{0}, strength{0};
        std::atomic<int> stop{0};
    };
    std::array<ImpactShow, 16> impactShow;
    std::atomic<uint32_t> impactCount{0};
    /** tests: an impact as if a pipe had hit there (audio thread / before process) */
    void testImpact(float x, float y, float z, float speed, float absorb, int stop, int key,
                    bool floor = false) noexcept
    {
        ImpactEvent e;
        e.x = x;
        e.y = y;
        e.z = z;
        e.speed = speed;
        e.absorb = absorb;
        e.stop = stop;
        e.key = key;
        e.floor = floor;
        queueImpact(e);
    }
    bool occlusionEnabled = true; // (tests: the same walk with and without the walls hiding pipes)

    void setModHook(ModHook *h) noexcept
    {
        modHook = h;
    }
    /** "Raumverfall" on the Ambisonics channels. The processor calls it itself after the impulse
       response (then set spaceDegInProcess = false); otherwise process() does it at the end. */
    bool spaceDegInProcess = true;
    void applySpaceDegradation(float *const *ch, int order, int n, const EngineParams &p) noexcept
    {
        eroder.process(ch, order, n, p.space, activeVoices.load() > 0);
    }
    std::atomic<float> spaceErosionNow{0.0f}; // for the display
    std::atomic<int> cloudGrainsNow{0};

    /** audio thread, before process(): the input of this block (left / right; right may be null =
       mono, both null = silence). The pointers must stay valid until process() returns. */
    void setInput(const float *l, const float *r, int numSamples) noexcept
    {
        inPtr[0] = l;
        inPtr[1] = r;
        inLen = numSamples;
    }
    // the input as placed right now, for the UI
    std::array<std::atomic<float>, 2> inShowAz{}, inShowEl{}, inShowDist{};
    std::atomic<int> inShowCount{0};
    std::atomic<float> inPeak{0.0f}; // raw input peak (before the level)

    // modulation state for the UI
    std::array<std::atomic<double>, kNumLfos> lfoPhaseOut{};
    std::array<std::atomic<float>, kNumSources> sourceOut{};
    std::array<std::array<std::atomic<float>, kNumKeys>, kNumStops> modAz, modEl,
        modDistMul; // per pipe, while sounding

    /** ask the engine to play one pipe for ~1.5 s. Lock-free, UI thread. */
    void requestPreview(int stop, int key);

    /** position of a pipe right now (base + motion), for the UI */
    Pos currentPosition(int stop, int key, const EngineParams &p) const noexcept;

    /** position as heard: motion + per-pipe modulation (direction, height, distance). UI thread.
        Sounding pipes use the engine's own values (incl. per-pipe envelopes / random per note),
        silent pipes the LFOs, key position, mod wheel and global envelopes. */
    Pos displayPosition(int stop, int key, const EngineParams &p) const noexcept;

    /** integrated motion phase per stop (cycles); speed changes only change its slope */
    std::array<std::atomic<double>, kNumStops> motionPhase{};

    /** drawn trajectory of a stop (message thread; the engine and the UI read it without locking)
     */
    void setPath(int stop, const MotionPath &path) noexcept;
    const MotionPath &getPath(int stop) const noexcept
    {
        return paths[(size_t)stop][(size_t)pathActive[(size_t)stop].load()];
    }
    MotionShape shapeFor(int stop, int key, const EngineParams &p) const noexcept;

    static constexpr int kMaxVoices = 384;
    static constexpr int kMaxNoiseVoices = 48;
    static constexpr int kGrains = 4;

private:
    struct Grain
    {
        double pos = 0;
        int len = 0, age = 0;
        float rateMul = 1.0f;
        bool on = false;
    };

    struct Voice
    {
        bool active = false;
        int stop = 0, key = 0, sourceKey = 0;
        const Pipe *pipe = nullptr;
        const float *data = nullptr;
        int length = 0;
        double pos = 0.0, baseRate = 1.0;
        bool looping = false, held = true;
        int loopXf = 0;
        bool relXfade = false;
        double relPos = 0.0;
        int relCount = 0, relLen = 1;
        bool killing = false;
        float killGain = 1.0f, killStep = 0.0f;
        int previewRemain = -1; // samples until an automatic note-off (preview)
        uint32_t age = 0;
        std::array<float, kMaxAmbiChannels> g0{}, g1{};
        float send0 = 0.0f, send1 = 0.0f;
        float sendAz0 = 0.0f, sendAz1 = 0.0f; // direction used for the room sends
        bool fresh = true;
        // degradation
        DegradeState deg;
        float held1 = 0.0f, env = 0.0f;
        Wobble wobble, breath;
        float windSm = -1.0f;            // smoothed stop wind (-1 = take target directly)
        float lp = 0.0f, lpA = 1.0f;     // tone filter for under-winded pipes
        float lpAir = 0.0f, airA = 1.0f; // air absorption by distance
        float hidLp = 0.0f, hidA = 1.0f,
              hidAc = 1.0f; // out of sight: the direct sound comes around a corner, duller (1 =
                            // open; target, now)
        bool hasBody = false;
        // Fluestern: this pipe at the two ears (gains, delays in samples, the far ear's shadow),
        // start and end of the block; a short history for the delays
        struct NearQ
        {
            float gL = 0.0f, gR = 0.0f, dL = 0.0f, dR = 0.0f, aL = 1.0f, aR = 1.0f, bass = 0.0f;
        };
        struct Near
        {
            NearQ q0, q1;
            std::array<float, 128> hist{};
            int w = 0;
            float sL = 0.0f, sR = 0.0f, lp = 0.0f;
        } near;
        float lenCents = 0.0f; // Wachsen & Brechen: the pitch of the pipe's length now
        // a loaded model: what was found the last time (searched again when listener or pipe moved)
        struct MeshOcc
        {
            Vec3f L{}, S{};
            Pos heard{};
            float loss = 0.0f, fc = 20000.0f;
            bool valid = false, blocked = false;
        } mocc;
        Vec3f bodyPos{};    // room motion: where the pipe is in the Saal (this block)
        WindChaos chaos;    // wind model 2: resonances, intermittent jet, hiss
        float chAir = 0.0f; // its own air filter
        float prevDist = -1.0f, dop = 1.0f, dopTarget = 1.0f;
        float detuneRnd = 0.0f; // -1..1, fixed per pipe
        // modulation
        std::array<EnvState, kNumEnvs> menv{};
        float keyPos = 0.0f, randMod = 0.0f, velMod = 0.0f;
        int noteId = -1;     // free tuning: channel * 128 + MIDI note (-1 = key mode)
        float bendSm = 0.0f; // MPE: this note's own pitch bend in cents (smoothed)
        // hard jumps: gains switched sample-accurately at every jump (no ramp)
        bool hard = false;
        float pmAz = 0.0f, pmEl = 0.0f, pmDist = 1.0f, gainLin = 1.0f;
        float pmWind = 0.0f, pmDeg = 0.0f, pmWob = 0.0f,
              pmCrk = 0.0f; // per-pipe modulation of stop wind, Verfall, Windschwankung, crackle
        // per-pipe modulation of the pipe sound, cloud, Verfall details and glitches (pmx: all
        // per-pipe targets of this block)
        std::array<float, kNumPipeTargets> pmx{};
        bool pmNew = false; // anything routed to the targets from tgtAttack on
        float hiGSm = -1.0f,
              airInvSm = -1.0f; // (smoothed while modulated: no steps from block to block)
        // beats: the pipe's frequency now, how fast it beats with the others (Hz, smoothed) and as
        // a modulation source
        float hzNow = 0.0f, beatT = 0.0f, beatHz = 0.0f, beatNorm = 0.0f;
        // grain cloud: the pipe's sound history (a ring of the pool), when the next grain is due,
        // where the pipe is
        int cloudRing = -1;
        double cloudAcc = 0.0;
        Pos spPos;
        float spK = 0.0f, cloudDry = 1.0f;
        float noteCents = 0.0f; // free tuning: shift from the pipe's own pitch
        // living organ: the pallet (how far open, the wind it lets through), the wear of this pipe,
        // the pull towards pure intervals (cents: target and now)
        float palOpen = 1.0f, palWind = 1.0f, wearNow = 0.0f, wearSign = 1.0f, pureT = 0.0f,
              pureC = 0.0f;
        float modCents0 = 0.0f, modCents1 = 0.0f;
        // Ansprache: the attack (gain, sharpness, isolation) and the repeated chiff (a second
        // reader)
        float attLp = 0.0f;
        int chLeft = 0, chWait = 0, chLen = 0, chAge = 0, chCount = 0;
        double chPos = 0.0;
        // Abriss: the release puff (band-passed noise)
        bool puffPending = false;
        float puffEnv = 0.0f, puffDec = 0.0f, puffAmp = 0.0f, pb1 = 0.0f, pb2 = 0.0f, py1 = 0.0f,
              py2 = 0.0f, pLp = 0.0f, pLpA = 1.0f;
        // upper partials as their own layer
        float hiS1 = 0.0f, hiS2 = 0.0f, hiEf = 0.0f,
              hiEs = 0.0f; // (2nd-order high-pass state, envelopes)
        // glitch: stutter (a slice repeated), clipping bursts
        double stStart = 0.0;
        int stLen = 0, stAcc = 0, stCount = 0, glTimer = 0, clipLeft = 0, clipTimer = 0,
            splLeft = 0;
        float clipDrive = 1.0f;
        // crackle
        int crkCount = 0;
        float crkAmp = 0.0f, crkEnv = 1.0f;
        // upper partials elsewhere
        float partLp = 0.0f, partSide = 1.0f;
        std::array<float, kMaxAmbiChannels> gh0{}, gh1{};
        // simulated room
        int erSlot = -1;
        float erBase = 0.0f;
        Pos erPos;
        // freeze
        bool frozen = false;
        float freezeMix = 0.0f;
        std::array<Grain, kGrains> grains{};
        int grainTimer = 0;
        XorShift grainRng{1};
    };

    struct NoiseVoice
    {
        bool active = false;
        const float *data = nullptr;
        int length = 0;
        double pos = 0.0, rate = 1.0;
        float gain = 0.0f, send = 0.0f;
        float lp = 0.0f, lpA = 1.0f; // tone (soft clicks are duller)
        std::array<float, kMaxAmbiChannels> g{};
        // placed / moving sounds (own sounds, tracker path)
        Pos where;                                // direction / distance
        float azVel = 0.0f;                       // deg per second, 0 = still
        std::array<float, kMaxRoomSends> sendW{}; // room-send weights (direction harmonics)
        int erSlot = -1;                          // simulated room: early reflections
        // resonated clicks: up to 6 two-pole resonators on the harmonics of the key
        int resN = 0, resTail = 0;
        float resMix = 0.0f;
        std::array<float, 6> rb1{}, rb2{}, rg{}, ry1{}, ry2{};
    };

    // ---- simulated room ----
    static constexpr int kErSlots = 48; // sounds with their own early reflections at once
    static constexpr int kErRingBits = 16, kErRing = 1 << kErRingBits;
    static constexpr int kMaxTaps = 62; // image sources up to 3rd order
    static constexpr int kErCh = 16;    // reflections are encoded up to 3rd order
    struct ErTap
    {
        float d0 = 0, d1 = 0, g0 = 0, g1 = 0, r0 = 1, r1 = 1, lp = 0;
        int ch = 4;
        std::array<float, kErCh> enc{};
        // polygon rooms: which reflection path this tap plays (-1 = free), fading in / out as it
        // becomes (in)visible
        int code = -1, k = 0, mz = 0, hw = 0, hf = 0;
        std::array<uint8_t, 3> w{};
        float vis = 0.0f, visT = 0.0f, gFull = 0.0f;
        bool hold = false, born = false;
    };
    struct ErSlot
    {
        bool active = false, fresh = true;
        int voice = -1,
            noiseVoice = -1; // owner (or both -1: the owner is gone, the reflections ring out)
        int input = -1;      // ... or one of the input sources
        int drain = 0;
        int numTaps = 0;
        std::array<ErTap, kMaxTaps> taps{};
        // what the taps were computed from: unchanged (a pipe that does not move) = nothing to
        // recompute
        static constexpr int kKey = 23;
        std::array<float, kKey> key{};
        bool keyValid = false;
        // polygon rooms: where the visible paths were last found, and from which plan
        Vec3f visSrc{};
        bool visValid = false, fading = false;
        int visShape = -1, visN = -1;
    };
    // polygon rooms: scratch for the path search (audio thread), and how many searches a block may
    // do
    static constexpr int kPlanPaths = 2048, kPlanCand = 1024;
    // The reflections are rendered in groups of slots, every group into its own bus with its own
    // scratch memory, and the buses added in a fixed order: the groups can be done on several
    // threads (AudioWorkers), and the result is the same whoever did which.
    static constexpr int kErGroups = 12, kErGroupSlots = kErSlots / kErGroups;
    struct ErWork
    {
        std::vector<float> y;         // 8 x slice: four delayed signals, four filtered ones
        std::vector<float> bus;       // 20 x slice: the group's reflections (16 + 4 for "Eis")
        std::vector<PlanPath> paths;  // polygon rooms: the path search
        std::vector<PlanImage> cands;
        int slotIndex = 0, active = 0;
        bool used = false;
        double geoMs = 0.0; // (measuring)
    };
    std::array<ErWork, kErGroups> erWork;
    struct ErJob
    {
        const EngineParams *p = nullptr;
        int n = 0, K = 0;
        float inv = 1.0f, shelfA = 0.0f;
        bool ice = false;
    } erJob;
    void renderErGroup(int group) noexcept;
    juce::SharedResourcePointer<AudioWorkers> audioWorkers;
    static constexpr int kPlanEvery = 6; // 48 slots / 6 = at most 8 path searches per block
    int erBlockN = 64;
    uint32_t erBlockCount = 0;
    // the rooms of this block with their plans filled in
    std::array<RoomParams, kNumRooms> roomsNow{};

    struct Scheduled
    {
        int delay = 0;      // samples
        bool voice = false; // true: start a pipe (stop, key), false: a sound
        int stop = 0, key = 0, noteId = -1;
        const Sample *sample = nullptr;
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
        XorShift rng{1};
        std::array<float, kMaxAmbiChannels> g0{}, g1{};
    };

    void handleMidi(const juce::MidiMessage &m, const EngineParams &p);
    void keyDown(int key, float velocity, const EngineParams &p, int noteId = -1);
    void keyUp(int key, const EngineParams &p, int noteId = -1);
    void startVoice(int stop, int key, int previewSamples = -1, int noteId = -1);
    // free tuning
    void noteOnFree(const juce::MidiMessage &m, const EngineParams &p);
    void noteOffFree(const juce::MidiMessage &m, const EngineParams &p);
    void releaseAllKeys(const EngineParams &p);
    /** free tuning: the pipe that sounds note id (channel * 128 + MIDI note), -1 = none */
    int pipeForFreeNote(int id, const EngineParams &p) const noexcept;
    float centsFor(int noteId, int key) const noexcept;
    struct FreeNote
    {
        bool held = false, sustained = false;
        int key = -1;
    };
    std::array<FreeNote, NoteTable::kSize> freeNotes{};
    std::array<int, kNumKeys> keyNoteCount{}; // free notes held on each pipe
    std::array<float, kNumKeys> slotHz{};     // the pitch every key's own pipe sounds at
    const NoteTable *notesNow = nullptr;
    bool notesContinuous = true, freeMode = false;
    // MPE / expression: per MIDI channel (pitch bend -1..1, pressure, slide) and per note
    // (polyphonic aftertouch)
    bool mpeNow = false;
    float mpeBendCents = 4800.0f;
    NoteTable keyboardTable; // built-in tuning, shared across MIDI channels
    std::array<float, 16> chBend{}, chPress{}, chSlide{};
    std::array<float, NoteTable::kSize> notePress{};
    std::array<float, kNumKeys> keyPress{};
    float lastPress = 0.0f, lastSlide = 0.0f, lastChanPress = 0.0f;
    float lastPressChannel() const noexcept
    {
        return lastChanPress;
    }
    int tableIndex(int noteId) const noexcept
    {
        return mpeNow || notesNow == &keyboardTable ? (noteId & 127) : noteId;
    } // MPE: one tuning for all channels
    float voicePressure(const Voice &v) const noexcept;
    float voiceSlide(const Voice &v) const noexcept;
    float voiceBendTarget(const Voice &v) const noexcept
    {
        return mpeNow && v.noteId >= 128 ? chBend[(size_t)(v.noteId >> 7)] * mpeBendCents
                                         : 0.0f; // (channel 1 is the master channel)
    }
    void releaseVoice(Voice &v);
    void freezeVoice(Voice &v);
    void releaseStopKey(int stop, int key, int noteId = -1);
    NoiseVoice *playNoise(const std::vector<NoiseSample> &pool, float gain, const float *gains,
                          int &lastIndex, float send, float tone = 1.0f);
    void spawnSound(const Sample &s, float gain, float tone, double rate, Pos pos, float azVel,
                    float send);
    void schedule(const Scheduled &e);
    void customSounds(int key, float velocity, const EngineParams &p);
    void trackerPath(int key, float velocity, const EngineParams &p);
    void placedGains(NoiseVoice &nv) noexcept;
    void renderLeaks(juce::AudioBuffer<float> &out, int n, const EngineParams &p);
    void renderRoom(juce::AudioBuffer<float> &out, int n, const EngineParams &p);
    struct RoomTail;
    void renderFdn(RoomTail &R, float *const *out, int K, const float *in, int n, float rtL,
                   float rtH, float gainTarget, const RoomParams &r);
    void resetFdn(RoomTail &R) noexcept;
    int allocSlot(int voice, int noiseVoice) noexcept;
    void releaseSlot(int &slot) noexcept;
    /** placeFrom: the room as seen from the listener's usual place, where srcPos is measured from
     * (null = room itself) */
    void updateSlotTaps(ErSlot &s, ErWork &w, Pos srcPos, float base, const EngineParams &p,
                        const RoomParams &room, const RoomParams *placeFrom = nullptr,
                        const Vec3f *fixedSrc = nullptr) noexcept;
    void updateSlotTapsPlan(ErSlot &s, ErWork &w, Pos srcPos, float base, const EngineParams &p,
                            const RoomParams &room, const RoomParams *placeFrom,
                            const Vec3f *fixedSrc) noexcept;
    void renderEchoes(juce::AudioBuffer<float> &out, int n, const EngineParams &p);
    void renderInput(juce::AudioBuffer<float> &out, int n, const EngineParams &p) noexcept;
    void followInput(int start, int len, const EngineParams &p) noexcept;
    const std::vector<NoiseSample> &customPool() const noexcept;
    static float clickCurve(float v) noexcept
    {
        return 0.04f + 0.96f * v * std::sqrt(v);
    } // velocity -> level
    float clickTone(float v) const noexcept // velocity -> one-pole coefficient
    {
        const float fc = 900.0f * std::pow(20000.0f / 900.0f, juce::jlimit(0.0f, 1.0f, v * 1.25f));
        return fc >= 19999.0f
                   ? 1.0f
                   : 1.0f - std::exp(-juce::MathConstants<float>::twoPi * fc / (float)sr);
    }
    std::array<float, kNumKeys> keyVelocity{};
    std::array<int, kNumKeys>
        keySource{}; // which recorded pipe a key plays (step tunings), from the last params
    void gainsFor(Pos pos, float *out) const noexcept;
    void renderRange(juce::AudioBuffer<float> &out, int start, int end, int blockLen,
                     const EngineParams &p);
    void updateVoiceSpatial(Voice &v, const EngineParams &p, double blockSeconds) noexcept;
    /** beats between the sounding pipes (motion "Schwebung", source "Schwebung") and the difference
     * tones that go with them */
    void updateBeats(const EngineParams &p, double blockSeconds) noexcept;
    void renderGhosts(float *const *outs, int start, int n, float inv) noexcept;
    // a stop's settings with this pipe's own modulation on top (exact copies when nothing is routed
    // there)
    static PipeSoundParams pipeSndFor(const PipeSoundParams &b, const Voice &v) noexcept;
    static GlitchParams glitchFor(const GlitchParams &b, const Voice &v) noexcept;
    static CloudParams cloudFor(const CloudParams &b, const Voice &v) noexcept;
    float readSample(const float *d, int len, double pos) const noexcept;
    void advanceModulation(const EngineParams &p, double dt) noexcept;
    float voiceSource(const Voice &v, int src, const EngineParams &p) const noexcept;
    float grainSample(Voice &v, double rate, float smear) noexcept;

    std::shared_ptr<const SampleLibrary> library;
    std::vector<Voice> voices;
    std::vector<NoiseVoice> noise;
    std::array<bool, kNumKeys> keyHeld{}, keySustained{};
    bool sustainPedal = false;
    std::array<bool, kNumStops> lastStopOn{};
    bool firstBlock = true;
    /** positions/gains are updated every kSubBlock samples so fast motion (up to 10 Hz) stays
     * smooth */
    static constexpr int kSubBlock = 64;
    void processChunk(juce::AudioBuffer<float> &out, const juce::MidiBuffer &midi,
                      const EngineParams &p);
    juce::MidiBuffer chunkMidi;
    ModHook *modHook = nullptr;
    std::array<double, kNumLfos> lfoPhase{};
    std::array<EnvState, kNumEnvs> globalEnv{};
    std::array<float, kNumSources> globalSrc{};
    bool globalTrig = false;
    // audio input
    const float *inPtr[2]{};
    int inLen = 0;
    static constexpr int kInRingBits = 17,
                         kInRing = 1 << kInRingBits; // delay by distance (Doppler), up to ~1.4 s
    std::vector<float> inRing;                       // [2][kInRing]
    uint32_t inClock = 0;
    struct InSrc
    {
        std::array<float, kMaxAmbiChannels> g0{}, g1{};
        float send0 = 0.0f, send1 = 0.0f, sendAz0 = 0.0f, sendAz1 = 0.0f;
        float d0 = 0.0f, d1 = 0.0f, lp = 0.0f, airA = 1.0f;
        Pos pos;
        int erSlot = -1;
        bool fresh = true;
    };
    std::array<InSrc, 2> inSrc{};
    float inGate = 0.0f, inLevelSm = 0.0f, inEnv = 0.0f, inFollowVal = 0.0f;
    double inSpin = 0.0;
    bool inRunning = false;
    int inDrain = -1;
    // Lucier mode
    TailConvolver lucier;
    std::vector<float> lucIn, lucWet;
    float lucDry = 1.0f, lucWetGain = 0.0f;
    float lucInEnv = 0.0f, lucOutEnv = 0.0f,
          lucAgc = 1.0f; // the resonances follow the organ's loudness
    int lucRing = 0;
    bool lucRunning = false;
    void renderLucier(juce::AudioBuffer<float> &out, int n, const EngineParams &p) noexcept;
    float lastKeyPos = 0.0f, lastRandom = 0.0f, modWheel = 0.0f, lastVelocity = 0.0f,
          pitchWheel = 0.0f;
    ModParams lastMod;
    bool haveLastMod = false;
    int roomOffset = 0;
    double sr = 48000.0;
    int maxBlock = 512;
    int usedChannels = 2;
    int order = 0; // 0 = stereo
    uint32_t ageCounter = 0;
    XorShift rng{4711};
    int lastDown = -1, lastUp = -1, lastPull = -1;
    int lastDownKey = -1; // the key struck last (the Tonnetz walker starts there)
    int lastDownNote = -1; // free tuning: the note (channel * 128 + MIDI note) struck last
    std::vector<float> scratch, scratchRamp,
        hidBuf; // hidBuf: the direct sound of a pipe heard around a corner (and its ramp)
    std::array<std::vector<float>, kMaxRoomSends> roomSend;
    int roomSends = 1;                             // used this block: 2 * roomHarmonics + 1
    std::array<float, kMaxRoomSends> noiseRoomW{}; // room weights of the click / pull sounds

    // effects
    std::array<Scheduled, 512> scheduled{};
    int numScheduled = 0;
    std::array<Leak, 8> leaks{};
    std::vector<float> echoBuf, fxScratch;
    int echoWrite = 0;
    double echoDelay = -1.0; // smoothed delay (samples)
    std::array<float, 8> echoLp{};
    double partRot = 0.0; // rotation of the upper partials (deg)
    float distAmountNow = 0.7f, chunkSeconds = 0.0f;
    bool roomFollowNow = true;
    float roomBaseAzNow = 0.0f;
    const std::vector<NoiseSample> noSounds;
    int lastCustom = -1;

    // simulated rooms: the late reverb of every room (convolution with a ray-traced response, or a
    // feedback delay network)
    static constexpr int kFdnLines = 16, kFdnBits = 16, kFdnLen = 1 << kFdnBits;
    struct RoomTail
    {
        TailConvolver tail; // late reverb (ray-traced, synthesised, convolved)
        float tailGain = 0.0f;
        bool tailWasOn = false;
        // the other late reverb: a 16-line feedback delay network (decay from the ray tracer)
        std::vector<float> fdnBuf, fdnPre; // [line][kFdnLen], input pre-delay
        uint32_t fdnPos = 0;
        std::array<float, kFdnLines> fdnDelay{}, fdnLp{}, fdnGL{}, fdnGH{};
        std::array<std::array<float, kMaxAmbiChannels>, kFdnLines> fdnEnc{};
        float fdnPreDelay = -1.0f, fdnGain = 0.0f, fdnLevelLast = 0.0f;
        std::array<float, kFdnLines> fdnModC{}, fdnModS{};
        int fdnEncOrder = -1;
        // switching between the two: the one switched off rings out on its own (input silent)
        int convRingOut = 0, fdnRingOut = 0, lastTailModel = 0;
        std::vector<float> send; // rooms 1..3: the sources in this room (room 0 = all others)
        bool running = false;
        bool wasUsed = false; // a source played into it in the last block
        int idleLeft = 0;     // unused: samples until what it holds has rung out
    };
    std::array<RoomTail, kNumRooms> rt;
    std::array<std::array<float, kFdnLines>, kFdnLines>
        fdnMatrix{}; // random orthogonal mixing matrix
    std::array<float, kFdnLines> fdnModStepC{},
        fdnModStepS{}; // slow delay modulation (phasor steps)
    std::vector<float> zeroIn, room0In;
    // notes to single pipes
    uint32_t spreadCounter = 0;
    int lastSpreadStop = -1;
    int spreadStopFor(int key, const EngineParams &p) noexcept;
    std::vector<ErSlot> erSlots;
    std::array<std::array<FloorPlan, 2>, kNumRooms> drawnPlans{};
    std::array<std::atomic<int>, kNumRooms> drawnPlanActive{};
    std::vector<float> erRing; // [slot][kErRing]
    uint32_t erClock = 0;
    // drawn paths, double-buffered (the writer fills the inactive one, then flips)
    std::array<std::array<MotionPath, 2>, kNumStops> paths{};
    std::array<std::atomic<int>, kNumStops> pathActive{};
    std::array<double, kNumStops> phasePrev{},
        phaseNow{}; // motion phase at the start / end of this block
    void hardJumpGains(const Voice &v, const EngineParams &p, double phase, float *g,
                       float *gh) const noexcept;
    bool roomOnNow = false;
    float consoleAzNow = 0.0f, consoleElNow = -10.0f;

    // wind + tremulant (shared by all pipes: one wind chest)
    double windPos = 0.0;
    float windGain = 0.0f;
    double gustPhase = 0.0, tremPhase = 0.0;
    std::vector<float> gust, trem; // one value per 16 samples

    // ---- living organ (OrganLive.cpp)
    void updateLive(const EngineParams &p, double blockSeconds) noexcept;
    void renderLive(float *const *outs, int n, const EngineParams &p) noexcept;

    // ---- room motion: the bodies (OrganBodies.cpp)
    struct Body
    {
        float x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0, rest = 0;
        float hx = 0, hy = 0, hz = 0; // where it rests ("Schweben" comes back to it)
        float ox = 0, oy = 0, oz = 0; // its place in the organ (where it started: "call home")
        float pushed = 0;             // s: the player has just moved it - it knocks whatever "Aufprall" says
        // shaped rooms: how far the nearest wall is at least (measured when it was last looked for,
        // less the way gone since): while that is more than the margin, no wall needs to be asked
        float clear = 0;
        // a loaded model: how far it may go before the model's triangles must be asked (the same
        // idea), whether it stands on one of the model's floors, whether it started inside a solid
        // (then it passes through until it is out)
        float meshClear = 0;
        bool meshGround = false, meshInside = false;
        bool loose = false;           // thrown ("Schweben"): it stays where it comes to rest
        float tx = 0, ty = 0, tz = 0, going = 0; // arranged: flying to (tx, ty, tz) for at most 'going' s more
        XorShift rng{1};
    };
    std::array<std::array<Body, kNumKeys>, kNumStops> bodies{};
    std::array<int, kNumStops> bodyMode{}; // the mode the bodies move in (-1 = none yet)
    std::array<PlanGeo, kNumStops> bodyGeoWas{}; // the walls the bodies' 'clear' was measured in
    std::array<int, kNumStops> bodyRoomWas{};
    int64_t bodySamples = 0; // samples since the room modes started: the steps are counted from it
                             // (the same paths at every block size)
    uint32_t bodyStepCount = 0;
    void stepBodies(const EngineParams &p, double blockSeconds) noexcept;
    // Selbstspiel: knocks that become notes
    struct SelfNote
    {
        int stop = 0, key = 0;
        float speed = 0.0f;
    };
    static constexpr int kMaxSelf = 24;
    std::array<SelfNote, kMaxSelf> selfQueue{};
    int numSelf = 0;
    std::array<std::array<float, kNumKeys>, kNumStops> selfWait{}; // s until a pipe may play again
    bool selfWaiting = false;
    void startSelfVoices(const EngineParams &p, double blockSeconds) noexcept;
    // Wachsen & Brechen
    bool lenOff = false; // some pipe is not of its own length
    void stepGrowth(const EngineParams &p, double blockSeconds) noexcept;
    // Fluestern
    std::vector<float> nearL, nearR;
    bool nearUsed = false;
    // Unmoeglicher Raum
    std::array<float, kNumRooms> tailScaleSm = {1.0f, 1.0f, 1.0f, 1.0f}, wallGainNow = {1.0f, 1.0f, 1.0f, 1.0f};
    // impacts: queued by the bodies, played by a few voices (a knock, the struck pipe ringing)
    struct ImpactEvent
    {
        float x = 0, y = 0, z = 0, speed = 0, absorb = 0, high = 0;
        int stop = 0, key = 0, room = 0;
        bool floor = false;
        float level = -1.0f; // < 0: as loud as "Aufprall" says
        bool struck = false; // knocked by the player's hand: no wall thumps
    };
    // the player's actions
    struct ActCmd
    {
        Act act = Act::Clap;
        int stop = -1, key = -1;
        Vec3f v;
        float amount = 1.0f;
    };
    juce::AbstractFifo actFifo{32};
    std::array<ActCmd, 32> actQueue{};
    float homing = 0.0f; // s left of the flight home
    float whirlLeft = 0.0f, whirlAmt = 1.0f; // s left of a whirlwind, and how strong
    Vec3f whirlAt;
    /** the sounds the player makes itself: the clap, the burst of the shockwave */
    struct Burst
    {
        bool on = false;
        int type = 0, pos = 0, len = 0; // 0 = clap, 1 = shockwave
        float gain = 0, low = 0, band = 0, lp = 0;
        double phase = 0;
    };
    std::array<Burst, 4> bursts{};
    void handleActs(const EngineParams &p) noexcept;
    void renderBursts(float *const *outs, int n) noexcept;
    static constexpr int kMaxImpactEvents = 24, kNumImpactVoices = 32, kImpactModes = 5;
    std::array<ImpactEvent, kMaxImpactEvents> impactQueue{};
    int numImpacts = 0;
    struct ImpactVoice
    {
        bool on = false;
        int exN = 0, exPos = 0; // the contact: a raised-cosine pulse of exN samples
        float exGain = 0, noiseGain = 0, noiseLp = 0, noiseA = 0;
        float thud = 0, thudLp = 0, thudA = 0, thudDec = 0;
        std::array<float, kImpactModes> b1{}, b2{}, y1{}, y2{}, gm{};
        float level = 0, levelDec = 0; // a rough envelope (for stealing and for ending)
        int age = 0, life = 0;
        std::array<float, kMaxAmbiChannels> g{};
        float send = 0;
    };
    std::array<ImpactVoice, kNumImpactVoices> impactVoices{};
    XorShift impactRng{4711u};
    void queueImpact(const ImpactEvent &e) noexcept;
    void startImpact(const ImpactEvent &e, const EngineParams &p) noexcept;
    void renderImpacts(float *const *outs, int n, const EngineParams &p) noexcept;
    void updateSympathy(const EngineParams &p) noexcept;
    void updatePure(const EngineParams &p, double blockSeconds) noexcept;
    void stepWalker(const EngineParams &p, int numSamples);
    Pos heardFrom(Pos pos) const noexcept;
    float pipeHzIdeal(int stop, int key, const EngineParams &p) const noexcept;
    float bellowsP = 1.0f, bellowsV = 1.0f;
    bool bellowsRising = false;
    double treadPhase = 0.0, treadTone = 0.0, treadLast = -1.0e18;
    float treadEnv = 0.0f, treadLp = 0.0f, treadLp2 = 0.0f;
    float walkX = 0.0f, walkY = 0.0f;
    float walkZ = 0.0f; // the listener's height above standing (3D view)
    // the room model of Saal 1 (line of sight), and how many searches a block may still do
    std::shared_ptr<const MeshAcoustics> meshShared, meshNow;
    std::shared_ptr<const MeshAcoustics> bodyMeshShared, bodyMeshNow;
    uint32_t bodyMeshSerial = 0; // the model the bodies' meshClear / meshInside were found in
    bool meshOn = false;
    mutable int meshBudget = 0;
    // the player (the walked listener) as the flying pipes feel it: place and speed per room
    std::array<Vec3f, kNumRooms> playerPos{}, playerVel{};
    std::array<bool, kNumRooms> playerOk{};
    double walkPhase = 0.0;
    bool liveWalk = false;
    std::array<RoomParams, kNumRooms> roomsEr{}; // the rooms as the walking listener stands in them
    // ... and what every pipe needs of them, worked out once when they are set (not again for every
    // pipe in every slice): where the listener is, and the walls of a shaped room
    std::array<Vec3f, kNumRooms> erListener{};
    // (the same for every pipe of a slice: worked out when the slice's length changes)
    double spatialDt = -1.0;
    float spatialLenK = 0.0f, spatialCrk = 1.0f;
    std::array<PlanGeo, kNumRooms> erGeo{};
    std::array<Vec3f, kNumRooms>
        walkSlid{}; // where the listener stands in a shaped room (slid along its walls)
    std::array<bool, kNumRooms> walkSlidOk{};
    // line of sight: in a plan that is not convex a walking listener can lose sight of a pipe
    // (heard around the corner)
    std::array<PlanGeo, kNumRooms> occGeo{};
    std::array<PlanCorners, kNumRooms> occCorners{};
    std::array<bool, kNumRooms> occOn{}, planOn{};
    std::array<RoomParams, kNumRooms> occUsual{}; // the room with the listener at the usual place
                                                  // (where the pipes are placed from)
    std::array<Vec3f, kNumRooms> occL{};          // the walked listener in it
    int occCount = 0;
    /** the pipe at 'home' (as placed from the listener's usual place) heard from where the listener
       stands: false if nothing is in the way; else where it is heard from and how much is lost */
    /** body: the pipe's own place in the room (room motion), else it is placed from "home" */
    int aroundCorner(int room, Pos home, Pos &heard, float &lossDb, float &cutoffHz,
                     const Vec3f *body = nullptr, Voice::MeshOcc *cache = nullptr)
        const noexcept; // 0 = not walking in a shaped room, 1 = in sight, 2 = around a corner
    // sympathetic pipes
    static constexpr int kSymp = 16;
    struct Symp
    {
        bool on = false, seen = false;
        int stop = 0, key = 0;
        float b1 = 0.0f, b2 = 0.0f, gain = 0.0f, gainT = 0.0f, y1 = 0.0f, y2 = 0.0f, hz = 0.0f,
              send = 0.0f;
        std::array<float, kMaxAmbiChannels> g0{}, g1{};
    };
    std::array<Symp, kSymp> symps{};
    std::vector<float> sympBus; // what the silent pipes hear: all sounding pipes (mono)
    int sympTick = 0;
    bool sympBusOn = false;
    // adaptive tuning
    uint64_t pureHash = 0;
    // Tonnetz walker
    struct NetNote
    {
        int key = -1, left = 0;
        int note = -1; // free tuning: the note it plays on that pipe (-1: the key itself)
    };
    std::array<NetNote, 8> netNotes{};
    // (in free tuning the walk goes through MIDI notes: netSeed / netPrev are then note ids)
    double netPhase = 0.0, netLastStep = -1.0e18;
    int netA = 0, netB = 0, netStep = 0, netSeed = -1, netPrev = -1, netLastDir = -1;
    uint64_t netHeld = 0; // the keys the player holds (not the walker's own)
    bool netRunning = false;
    XorShift netRng{0x7e77};

    // sum-mode degradation
    DegradeState sumDeg{99};
    // ---- the sound of the pipe (Ansprache, Abriss, upper partials) and glitches: this block's
    // settings,
    //      for the places that have no params at hand (voice start, release)
    std::array<PipeSoundParams, kNumStops> pipeSndNow{};
    GlitchParams glitchNow;
    void setupResonators(NoiseVoice &nv, int key, float velocity, const EngineParams &p) noexcept;
    // ---- spatial degradation
    SpaceEroder eroder;
    int gridN = 0; // direction grid (0 = off)
    std::array<float, 64> gridX{}, gridY{}, gridZ{}, gridAz{}, gridEl{};
    void updateGrid(float amount) noexcept;
    // ---- grain cloud: a pool of sound histories (rings) and of grains
    static constexpr int kCloudRings = 64, kCloudGrains = 256;
    struct CloudGrain
    {
        bool on = false, dying = false;
        int ring = -1, len = 0, age = 0;
        uint32_t rp = 0, owner = 0;
        float fade = 1.0f;
        std::array<float, kMaxAmbiChannels> g{};
    };
    std::vector<float> cloudData;
    int ringSize = 0;
    uint32_t ringMask = 0;
    std::array<uint32_t, kCloudRings> ringPos{},
        ringOwner{}; // owner = the voice's age stamp (0 = free)
    std::array<int, kCloudRings> ringRefs{};
    std::array<CloudGrain, kCloudGrains> grains{};
    XorShift cloudRng{0xc10d};
    int cloudGrainsActive = 0;
    void cloudWrite(Voice &v, const float *buf, const float *bufH, int n,
                    const EngineParams &p) noexcept;
    void renderCloud(float *const *outs, int start, int n) noexcept;

public:
    /** Doppelbelichtung and Belichtung on the finished field (the processor calls it after the
     * room, before Raumverfall) */
    void applyPhotoFx(float *const *ch, int outputMode, int n, const EngineParams &p) noexcept;
    /** "Schwebung" motion: how far every pipe has turned so far (degrees) */
    std::array<std::array<std::atomic<float>, kNumKeys>, kNumStops> beatTurn;
    std::atomic<int> ghostsNow{0};       // difference tones sounding (for tests / display)
    std::atomic<float> beatMaxNow{0.0f}; // fastest beat right now (Hz)
private:
    struct Ghost
    {
        bool on = false;
        uint32_t ageA = 0, ageB = 0; // the pair (voice age stamps)
        float hz = 0.0f, hzT = 0.0f, amp = 0.0f, ampT = 0.0f;
        double ph = 0.0;
        bool seen = false;
        std::array<float, kMaxAmbiChannels> g0{}, g1{};
    };
    static constexpr int kGhosts = 12;
    std::array<Ghost, kGhosts> ghosts{};
    DoubleExposure dexFx;
    LongExposure expoFx;
    DispersionBank iceHi, iceLo;
    bool iceWasOn = false;
    std::vector<float> iceBus; // early reflections collected for the dispersion (16 + 4 channels)
    std::array<float, kMaxOutChannels> sumHeld{}, sumEnv{};
    float masterSmoothed = 1.0f;

    // preview FIFO (UI -> audio)
    juce::AbstractFifo previewFifo{32};
    std::array<std::pair<int, int>, 32> previewQueue{};
};
} // namespace rysum
