#pragma once

#include "Lucier.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace rysum
{
/** Lucier's piece as it is performed: record (a voice, the organ, anything), then the recording is played into the
    room and recorded again, generation after generation, each one as long as the recording - a piece that plays by
    itself. Generation 0 is the recording itself. Every generation is computed in the background while the one
    before it plays (one real pass through the room: undamped, with loudspeaker and microphone), then brought back
    to the level of the recording, as Lucier re-recorded every generation at full level.

    Threads: process() on the audio thread; everything else on the message thread. */
class LucierPiece
{
public:
    enum State { stIdle = 0, stRecording, stPlaying, stFinished };

    // ---- audio thread
    void prepare (double sampleRate);
    /** 'rec' = the mono signal to record (may be null), 'out' channels get the playback (mono, 'outGain' per channel),
        the dry signal is faded while the piece plays (equal power with 'mix') */
    void process (const float* rec, float* const* out, const float* outGain, int numOut, int n, float mix) noexcept;

    // ---- message thread
    bool startRecording (double maxSeconds = 300.0);
    void stopRecording (bool thenPlay = true);
    void play();                                     // the piece from generation 0 (needs a recording)
    void stop();
    void setRecording (std::vector<float> data, double sampleRate);   // a loaded file
    const std::vector<float>& getRecording() const { return recording; }
    double recordingRate() const { return recRate; }
    int recordingVersion = 0;                        // +1 with every new recording
    /** call regularly (timer): starts computing the next generation when there is room for it */
    void tick (std::shared_ptr<const LucierBase> base, int lastGeneration, float gapSeconds, juce::ThreadPool& pool,
               std::shared_ptr<std::atomic<bool>> alive);

    // status (any thread)
    std::atomic<int> state { stIdle };
    std::atomic<int> playingGen { 0 }, computingGen { -1 }, lastGen { 12 };
    std::atomic<int64_t> playPos { 0 }, playLen { 0 }, recPos { 0 };
    std::atomic<float> level { 0.0f };
    double sampleRate() const { return sr; }
    juce::String recordingFile;                      // where the last recording was saved / loaded from

    /** the whole piece into a WAV file (background; progress 0..1, -1 = idle). */
    void exportPiece (const juce::File& f, std::shared_ptr<const LucierBase> base, int lastGeneration, float gapSeconds, juce::ThreadPool& pool);
    std::atomic<float> exportProgress { -1.0f };

private:
    double sr = 48000.0, recRate = 48000.0;
    // recording (the buffer is allocated before the state switches to 'recording')
    std::vector<float> recBuf;
    int64_t recCap = 0;
    std::atomic<bool> recFull { false };
    std::vector<float> recording;                    // the finished recording (message thread)
    double recRms = 0.0;

    // playback: two slots, the audio thread plays one while the other is filled
    struct Slot { std::shared_ptr<std::vector<float>> data; int gen = 0; };
    std::array<Slot, 2> slots;
    std::array<std::atomic<bool>, 2> slotReady {};   // filled and not yet played
    std::atomic<int> playSlot { 0 };
    int gapLeft = 0;                                 // audio thread: silence between generations
    std::atomic<int> gapSamples { 0 };
    const float* curData = nullptr;                  // audio thread
    int64_t curLen = 0, curPos = 0;
    bool curValid = false;
    float wetGain = 0.0f, dryGain = 1.0f;
    std::shared_ptr<std::vector<float>> lastComputed;   // message thread: the newest generation
    int lastComputedGen = -1;
    std::shared_ptr<std::atomic<bool>> jobCancel;
    std::atomic<bool> exportCancel { false };
    int64_t maxLen() const;
    std::atomic<int> playEpoch { 0 };
    int seenEpoch = 0, nextSlot = 0;                 // audio thread
    // buffers the audio thread may still be reading: freed a second later
    std::vector<std::shared_ptr<std::vector<float>>> retired, retiredRec;
    double retiredAt = 0.0;
    void retire();
    std::shared_ptr<const LucierBase> passBase;
    std::shared_ptr<std::vector<float>> pass;
public:
    /** before the owner goes: background work stops */
    void cancelAll() { if (jobCancel) jobCancel->store (true); exportCancel = true; }
};

/** brings a generation back to the level of the recording (rms), never above -1 dBFS */
void lucierLevelMatch (std::vector<float>& x, double targetRms);
} // namespace rysum
