#pragma once

#include "Common.h"
#include "Modulation.h"
#include "RoomSim.h"
#include <array>
#include <atomic>
#include <deque>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <map>
#include <memory>
#include <vector>

class RysumProcessor;

namespace rysum
{
/** "Konzert": one standalone plays, others listen - each on their own computer, each rendering
    the organ themselves, from wherever they walk in the 3D Saal.

    The player sends what makes the piece: the patch state (when a stop list, a layout, a path or
    the Verlauf changes), every parameter change, the MIDI it plays (after Halten and the
    controller assignments) and, 20 times a second, the clocks that move things (motion phases,
    LFOs, the Verlauf clock). Everything carries the player's audio time; a listener plays it a
    fixed delay later (a jitter buffer), in the order it was sent. Settings that belong to the
    listener stay theirs: output format, headphones, loudspeakers, volume, the samples folder and
    where they stand.

    No sound travels: every listener needs the plugin and the samples. Off in every new instance,
    never saved. The listener's own setup comes back when they leave.

    Threads: the audio thread and the network thread talk through lock-free FIFOs; parameters and
    the patch state are read / applied on the message thread (a state change is a barrier in the
    listener's stream: what comes after it waits until it is applied). */
class ConcertLink : private juce::Thread, private juce::Timer
{
public:
    enum class Role
    {
        off,
        performer,
        listener
    };
    explicit ConcertLink(RysumProcessor &);
    ~ConcertLink() override;

    // ---- message thread
    /** play: listeners connect to this port (loopbackOnly: 127.0.0.1, for the tests) */
    bool startPerformer(int port = kDefaultPort, bool loopbackOnly = false);
    /** play through a relay server, "server[:port]/room": listeners join with the same text. The
        player connects out to the relay (no router, no open port here); for every listener the relay
        joins a connection of the player's to the listener's, so all else - the password too - is
        between the two of them */
    bool startPerformerRelay(const juce::String &relayAddress);
    /** listen to a player: "host", "host:port", or a relay's "server[:port]/room" */
    bool startListener(const juce::String &address, int port = kDefaultPort);
    /** the concert's password (set before starting; empty = none). It never travels: the player sends
        every listener a random challenge, the listener answers with SHA-256(challenge, password) */
    void setPassword(const juce::String &pw);
    juce::String getPassword() const;
    /** the player: through a relay now, and what listeners type ("server:port/room") */
    bool viaRelay() const noexcept
    {
        return relayMode.load();
    }
    juce::String relayAddress() const;
    /** "host[:port][/room]" -> its parts (false = not an address) */
    static bool parseAddress(const juce::String &text, juce::String &host, int &port, juce::String &room);
    /** SHA-256 of some bytes, as hex (the password's answer; tests) */
    static juce::String sha256Hex(const void *data, size_t n);

    /** the player: talk to the listeners - the audio input goes to them as a voice of its own (also
        with the input "Eingang" off, not into the room, not heard here) */
    void setTalking(bool on) noexcept
    {
        talking = on;
    }
    bool isTalking() const noexcept
    {
        return talking.load();
    }
    /** the listener: how loud the player's voice is (linear, 0..2) */
    void setVoiceGain(float g) noexcept
    {
        voiceGain = juce::jlimit(0.0f, 2.0f, g);
    }
    float getVoiceGain() const noexcept
    {
        return voiceGain.load();
    }
    /** the listener: the player spoke within the last second */
    bool voiceHeard() const noexcept
    {
        return juce::Time::getMillisecondCounterHiRes() - voiceHeardMs.load() < 1000.0;
    }
    /** back to playing alone (a listener gets their own setup back) */
    void stop();
    Role role() const noexcept
    {
        return roleNow.load();
    }
    /** a line for the panel: who is connected, how it goes */
    juce::String status() const;
    /** the player: listeners connected now */
    int listenerCount() const noexcept
    {
        return numListeners.load();
    }
    /** the listener: the link is up and the piece is here */
    bool joined() const noexcept
    {
        return isJoined.load();
    }
    /** the listener's delay behind the player (ms): room for an uneven network */
    void setDelayMs(double ms) noexcept
    {
        delayMs = juce::jlimit(60.0, 2000.0, ms);
    }
    double getDelayMs() const noexcept
    {
        return delayMs.load();
    }
    /** the addresses listeners can use (this computer's IPv4 addresses on the network) */
    static juce::StringArray localAddresses();
    static constexpr int kDefaultPort = 8790;

    /** the others in the Saal: the player and every listener, where they stand (offsets from the
        Saal's listening place, m, like walkPose) and where they look - not oneself */
    struct Figure
    {
        int id = 0; // 0 = the player
        juce::String name;
        float x = 0, y = 0, z = 0, yaw = 0;
        double seenMs = 0.0;
    };
    std::vector<Figure> figures() const;

    /** the player: what the listeners' hands may do in the Saal - 0 nothing, 1 knock, push (the
        shockwave) and clap, 2 also carry, throw, call the pipes home, whirl and sound pipes */
    void setGuestHands(int level) noexcept
    {
        guestHands = juce::jlimit(0, 2, level);
    }
    int getGuestHands() const noexcept
    {
        return guestHands.load();
    }
    /** the listener: what the player allows its hands now (0 .. 2) */
    int allowedHands() const noexcept
    {
        return roleNow.load() == Role::listener && isJoined.load() ? allowedLevel.load() : 0;
    }
    /** the listener: ask the player to let this stop's pipes hover (so they can be taken) */
    void requestHover(int stop);
    /** the listener's name, as the others see it over their figure */
    void setName(const juce::String &n);
    juce::String getName() const;

    // ---- audio thread (from processBlock)
    /** the player: this block's MIDI (after Halten and the controller assignments) */
    void captureMidi(const juce::MidiBuffer &midi, double blockTime, double sr) noexcept;
    /** the player, after the engine ran: the clocks, every ~50 ms */
    void captureClocks(double blockEndTime, const double *motion, const double *lfo, double beats,
                       bool running) noexcept;
    /** the listener: what is due in this block (MIDI into 'midi'; parameters, clocks applied) */
    void playInto(juce::MidiBuffer &midi, double blockTime, int numSamples, double sr) noexcept;
    /** the player: this block's audio input (only while the input is on) */
    void captureAudio(const float *l, const float *r, int n, double blockTime, double sr) noexcept;
    /** the listener: the player's input for this block, in time with the notes (false = none) */
    bool playAudioInto(float *l, float *r, int n, double blockTime, double sr) noexcept;
    /** the player: this block's audio input as the voice (only while talking) */
    void captureVoice(const float *l, const float *r, int n, double blockTime, double sr) noexcept;
    /** the listener: the player's voice for this block, mono (false = none) */
    bool playVoiceInto(float *out, int n, double blockTime, double sr) noexcept;
    /** both: the audio clock (seconds of audio processed), set at the start of every block */
    void setAudioTime(double t) noexcept
    {
        audioTime.store(t);
        audioWall.store(juce::Time::getMillisecondCounterHiRes());
    }

    // ---- tests
    /** the listener pretends every message arrived up to this much later (uneven network) */
    double testJitterMs = 0.0;
    /** the listener: local audio times at which note-ons were played (newest last, up to 64) */
    std::vector<double> testNoteTimes() const;
    /** the player: how many state changes were sent */
    int stateSendsForTest() const noexcept
    {
        return stateSeq;
    }
    /** the listener: the local audio time the player's input first went over 0.5 (-1 = not yet) */
    double testLoudAt() const noexcept
    {
        return loudAt.load();
    }
    void testResetLoud() noexcept
    {
        loudAt = -1.0;
        voicePeak = 0.0f;
    }
    /** the listener: the loudest of the player's voice so far */
    float testVoicePeak() const noexcept
    {
        return voicePeak.load();
    }
    /** the listener: knocks of the player's pipes heard so far */
    int knocksHeardForTest() const noexcept
    {
        return knocksHeard.load();
    }
    /** the listener: their own setup, kept while they listen (saving now - the host, the
        standalone on quit - keeps theirs, not the player's piece); invalid = not listening */
    juce::ValueTree ownSetupWhileListening() const
    {
        return roleNow.load() == Role::listener ? ownSetup : juce::ValueTree();
    }

private:
    void run() override;
    void timerCallback() override;
    void runPerformer();
    void runListener();
    double audioTimeNow() const noexcept;

    // one event in a FIFO (audio <-> network)
    struct Ev
    {
        double t = 0.0;    // player: own audio time; listener: local audio time it is due
        char type = 0;     // 'M' midi, 'P' parameter, 'S' clocks, 'B' barrier (state), 'K' a sound of the hands
        uint8_t n = 0;     // midi bytes
        uint8_t b[3]{};    // midi
        int32_t index = 0; // parameter index / state number
        float value = 0.0f;
        std::array<double, kNumStops + kNumLfos + 1> clocks{}; // motion, LFOs, Verlauf beats
        bool running = false;
        uint32_t session = 0; // the listener's connection it came on
    };
    static constexpr int kFifo = 8192;
    juce::AbstractFifo fifo{kFifo};
    std::vector<Ev> fifoData;
    bool push(const Ev &) noexcept;

    RysumProcessor &proc;
    std::atomic<Role> roleNow{Role::off};
    std::atomic<double> audioTime{0.0}, audioWall{0.0}, delayMs{250.0};
    std::atomic<int> numListeners{0};
    std::atomic<bool> isJoined{false};
    juce::String host;
    int port = 0;
    bool loopback = false;
    std::unique_ptr<juce::StreamingSocket> listenerSocket; // (the player's)
    mutable juce::CriticalSection statusLock;
    juce::String statusText;
    void setStatus(const juce::String &s);

    // ---- player
    struct Client
    {
        std::unique_ptr<juce::StreamingSocket> sock;
        std::array<uint8_t, 16> nonce{}; // the challenge it must answer with the password
        bool helloOk = false, joined = false;
        double since = 0.0; // events before the join state are in it already
        double connectedMs = 0.0;
        std::vector<std::pair<double, juce::MemoryBlock>> backlog; // while the join is being made
        int id = 0;
        Figure fig;          // where this listener stands (from its pose frames)
        bool hasFig = false;
    };
    std::vector<std::unique_ptr<Client>> clients; // network thread
    void admit(std::unique_ptr<juce::StreamingSocket> s, double now); // network thread: a new listener
    std::deque<double> failedAt;   // network thread: wrong passwords, the last minute
    double refuseUntil = 0.0;      // network thread: too many - nobody for a while
    juce::String password;         // (statusLock)
    // the relay (player)
    std::atomic<bool> relayMode{false}, relayUp{false};
    juce::String relayHost, relayRoom; // (set before the thread runs)
    int relayPort = kDefaultPort;
    std::unique_ptr<juce::StreamingSocket> relayCtl; // network thread
    std::string relayBuf;
    double relayRetryMs = 0.0, relayPingMs = 0.0;
    bool relayConnect(double now);   // network thread
    void relayPoll(double now);      // network thread
    void beginPerformer();           // message thread: what both ways of playing start with
    // the listener through a relay: the room (empty = straight to the player)
    juce::String listenRoom;
    // the voice: the player's input, mono, at about 16 kHz, 16-bit (player: audio -> network thread)
    std::atomic<bool> talking{false};
    struct VoiceChunk
    {
        double t = 0.0;
        int sr = 16000, n = 0;
        std::array<int16_t, 512> s{};
    };
    static constexpr int kVoiceChunks = 128;
    juce::AbstractFifo voiceOutFifo{kVoiceChunks};
    std::vector<VoiceChunk> voiceOut = std::vector<VoiceChunk>((size_t)kVoiceChunks);
    VoiceChunk vCur;          // audio thread: the chunk being filled
    float vLp1 = 0, vLp2 = 0, vAcc = 0;
    int vCnt = 0;
    // ... and at the listener: a ring of samples, each due at its moment (as the input)
    static constexpr int kVoiceRing = 1 << 17;
    juce::AbstractFifo voiceInFifo{kVoiceRing};
    std::vector<float> voiceIn = std::vector<float>((size_t)kVoiceRing);
    std::atomic<double> voiceDueEnd{0.0}, voiceHeardMs{-1.0e9};
    std::atomic<int> voiceSr{16000};
    std::atomic<uint32_t> voiceSession{0};
    uint32_t voiceSessionSeen = 0;
    bool voiceRunning = false;
    float vA = 0, vB = 0;
    double vFrac = 0.0, vErr = 0.0;
    int vStartAt = 0;
    std::atomic<float> voiceGain{1.0f}, voicePeak{0.0f};
    int nextClientId = 1;
    double lastFiguresMs = 0.0, lastPoseMs = 0.0; // network thread
    mutable juce::CriticalSection figLock;
    std::vector<Figure> figs; // what figures() gives
    int myId = -1;             // the listener: its own id (from the player)
    juce::String myName{"Gast"};
    juce::MemoryBlock figuresFrame(const std::vector<Figure> &all);
    // the listeners' hands
    std::atomic<int> guestHands{0}, allowedLevel{0};
    struct GuestAct
    {
        int client = 0, act = 0, stop = -1, key = -1;
        Vec3f v{};
        float amount = 1.0f;
    };
    std::vector<GuestAct> guestActsIn; // the player: network thread -> message thread (figLock)
    std::vector<GuestAct> actsOut;     // the listener: message thread -> network thread (figLock)
    std::array<int, 8> handOwner{};    // the player, network thread: the client holding each guest hand
    int lastGrabSent = -2;             // the listener, network thread
    double lastHandMs = 0.0;
    static constexpr int kHoverRequest = 200;
    void applyGuestActs(); // the player, message thread
    void releaseGuestHand(int clientId); // the player, network thread
    void readClientFrame(Client &c, char type, juce::MemoryInputStream &in); // the player, network thread
    // the message thread makes join packages and parameter / state changes
    juce::CriticalSection outLock;
    std::vector<std::pair<double, juce::MemoryBlock>> outFrames; // (time, frame) for everyone
    std::deque<juce::MemoryBlock> joinPackages;                 // for the next waiting client
    std::atomic<int> joinsWanted{0};
    std::vector<float> lastSent;      // message thread: the parameter values listeners have
    juce::String lastStateKey;        // message thread: the state without parameters, last sent
    double lastStateCheck = 0.0;
    int stateSeq = 0;
    std::array<std::atomic<uint8_t>, 16 * 128> heldVel{}; // audio thread writes: notes down now
    double lastClockSent = -1.0;                          // audio thread
    juce::MemoryBlock makeStateFrame(double t, int seq);
    juce::String stateKey();
    juce::MemoryBlock makeJoinPackage();
    void scanParameters();
    // the pipes that are bodies in the Saal (thrown, carried, flying): their places, 50 times a
    // second while they move; the lengths (Wachsen & Brechen) when they change
    std::array<std::array<Vec3f, kNumKeys>, kNumStops> bodySent{};
    std::array<bool, kNumStops> bodyWasOn{};
    std::array<double, kNumStops> bodySentMs{};
    std::array<std::array<float, kNumKeys>, kNumStops> lenSent{};
    double lenSentMs = 0.0;
    juce::MemoryBlock bodyFrame(double t, int stop, bool on);
    juce::MemoryBlock lengthFrame(double t);
    void scanBodies(std::vector<std::pair<double, juce::MemoryBlock>> &out, bool all);

    // ---- listener
    std::unique_ptr<juce::StreamingSocket> sock; // (the listener's)
    std::deque<std::pair<double, double>> offsets; // (wall ms, local - remote), last few seconds
    double lastTarget = 0.0;
    double lastGlideMs = 0.0;
    double smoothOff = 0.0; // the offset in use (glides towards the smallest)
    double lastHeardMs = 0.0;
    juce::CriticalSection stateLock;
    std::map<int, juce::String> pendingStates; // number -> state, waiting for its time
    std::atomic<int> barrierWanted{-1}, barrierDone{-1};
    juce::ValueTree ownSetup; // the listener's setup before the concert
    std::unique_ptr<std::atomic<float>[]> mirrored; // the player's parameter values (NaN = none yet)
    int numParams = 0;
    std::array<uint8_t, 16 * 128> playing{};  // audio thread: notes this listener plays now
    std::atomic<bool> releaseNow{false};
    std::atomic<int> knocksHeard{0};
    mutable juce::SpinLock testSpin;
    std::array<double, 64> noteRing{};
    size_t noteRingPos = 0;
    double holdUntil = 0.0; // audio thread
    std::atomic<uint32_t> session{0};
    bool overflow = false, everJoined = false; // network thread
    // the player's pipe places, per stop: network thread -> audio thread (its own ring: in Flug
    // every pipe moves all the time, the event FIFO stays for what must not be lost)
    struct BodySnap
    {
        double t = 0.0;
        uint32_t session = 0;
        bool on = false;
        std::array<Vec3f, kNumKeys> p{};
    };
    static constexpr int kBodyRing = 256;
    struct BodyRing
    {
        juce::AbstractFifo fifo{kBodyRing};
        std::vector<BodySnap> data = std::vector<BodySnap>((size_t)kBodyRing);
        BodySnap cur, next;   // audio thread: the one in use, the one after it
        bool haveCur = false, haveNext = false;
    };
    std::array<std::unique_ptr<BodyRing>, kNumStops> bodyRings;
    struct LenSnap
    {
        double t = 0.0;
        uint32_t session = 0;
        std::array<std::array<float, kNumKeys>, kNumStops> len{};
    };
    static constexpr int kLenRing = 16;
    juce::AbstractFifo lenFifo{kLenRing};
    std::vector<LenSnap> lenData = std::vector<LenSnap>((size_t)kLenRing);
    // the player's audio input: 16-bit chunks of up to 512 frames (the player: audio thread ->
    // network thread), then a ring of samples at the listener (network thread -> audio thread)
    struct AudioChunk
    {
        double t = 0.0;
        int sr = 48000, n = 0;
        std::array<int16_t, 2 * 512> s{};
    };
    static constexpr int kAudioChunks = 256;
    juce::AbstractFifo audioOutFifo{kAudioChunks};
    std::vector<AudioChunk> audioOut = std::vector<AudioChunk>((size_t)kAudioChunks);
    static constexpr int kAudioRing = 1 << 19;
    juce::AbstractFifo audioInFifo{kAudioRing};
    std::vector<float> audioInL = std::vector<float>((size_t)kAudioRing), audioInR = std::vector<float>((size_t)kAudioRing);
    std::atomic<double> audioDueEnd{0.0}; // local time the sample after the last one written is due
    std::atomic<int> audioSr{48000};
    std::atomic<uint32_t> audioSession{0};
    uint32_t audioSessionSeen = 0; // audio thread from here
    bool audioRunning = false;
    float aL = 0, aR = 0, bL = 0, bR = 0;
    double audioFrac = 0.0, audioErr = 0.0;
    int audioStartAt = 0;
    std::atomic<double> loudAt{-1.0};
    void followBodies(double blockTime) noexcept; // audio thread
    void stopFollowing() noexcept;                // audio thread
    std::vector<char> forcedOff; // parameters kept off here (notes / sound of their own on the player)
    double lastAssert = 0.0;
    bool listenSession(); // true = lost (try again)
    void applyState(const juce::String &xml);
    void receiveFrame(char type, juce::MemoryInputStream &in);
    double scheduleAt(double remoteT);
};
} // namespace rysum
