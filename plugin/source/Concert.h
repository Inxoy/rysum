#pragma once

#include "Common.h"
#include "Modulation.h"
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
    /** listen to a player at host:port */
    bool startListener(const juce::String &host, int port = kDefaultPort);
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

    // ---- audio thread (from processBlock)
    /** the player: this block's MIDI (after Halten and the controller assignments) */
    void captureMidi(const juce::MidiBuffer &midi, double blockTime, double sr) noexcept;
    /** the player, after the engine ran: the clocks, every ~50 ms */
    void captureClocks(double blockEndTime, const double *motion, const double *lfo, double beats,
                       bool running) noexcept;
    /** the listener: what is due in this block (MIDI into 'midi'; parameters, clocks applied) */
    void playInto(juce::MidiBuffer &midi, double blockTime, int numSamples, double sr) noexcept;
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
        char type = 0;     // 'M' midi, 'P' parameter, 'S' clocks, 'B' barrier (state), 'R' release all
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
        bool helloOk = false, joined = false;
        double since = 0.0; // events before the join state are in it already
        double connectedMs = 0.0;
        std::vector<std::pair<double, juce::MemoryBlock>> backlog; // while the join is being made
    };
    std::vector<std::unique_ptr<Client>> clients; // network thread
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

    // ---- listener
    std::unique_ptr<juce::StreamingSocket> sock; // (the listener's)
    std::deque<std::pair<double, double>> offsets; // (wall ms, local - remote), last few seconds
    double lastTarget = 0.0;
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
    mutable juce::SpinLock testSpin;
    std::array<double, 64> noteRing{};
    size_t noteRingPos = 0;
    double holdUntil = 0.0; // audio thread
    std::atomic<uint32_t> session{0};
    bool overflow = false, everJoined = false; // network thread
    std::vector<char> forcedOff; // parameters kept off here (notes / sound of their own on the player)
    double lastAssert = 0.0;
    bool listenSession(); // true = lost (try again)
    void applyState(const juce::String &xml);
    void receiveFrame(char type, juce::MemoryInputStream &in);
    double scheduleAt(double remoteT);
};
} // namespace rysum
