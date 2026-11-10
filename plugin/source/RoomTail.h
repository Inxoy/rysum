#pragma once

#include "PartConv.h"
#include "RoomSim.h"
#include <atomic>
#include <juce_dsp/juce_dsp.h>
#include <memory>
#include <vector>

namespace rysum
{
/** The late reverb of the simulated room, done like room-acoustics software does it (e.g. RAVEN):
    the decay of the room per frequency band comes from ray tracing, and a dense, decorrelated
    noise impulse response with exactly that decay is synthesised, one per output channel
    (energy per Ambisonics order as in a diffuse field), and convolved. Unlike a feedback delay
    network this has no modes at all, so nothing can ring.

    The convolution is non-uniformly partitioned (256 / 2048 / 8192 samples, PFFFT; the two large
   stages spread their work evenly over time instead of computing a whole block at once) and keeps
   the input history independent of the impulse response. A new response (the room changed or is
   being modulated) is therefore swapped in by crossfading two outputs computed from the SAME
   history: the reverb that is already sounding simply turns into the new room's, without a gap. */
struct TailIR
{
    static constexpr int kStages = 3;
    static constexpr int kBlock[kStages]{256, 2048, 8192};
    static constexpr int kStart[kStages]{
        256, 4096, 32768}; // the first 256 samples (~5 ms) are left to the early reflections
    static constexpr int kLag[kStages]{
        1, 2, 2}; // see PartitionedConvolution (lag 2: work spread over time)
    static constexpr double kMaxSeconds = 6.0;

    int channels = 0; // output channels (2 = stereo, else Ambisonics (o+1)^2)
    int order = 0;    // 0 = stereo
    double sampleRate = 48000.0;
    int length = 0; // samples
    float rtLow = 1.0f, rtHigh = 1.0f, volume = 0.0f;
    int rays = 0;
    uint32_t serial = 0;
    // spectra per stage: [channel][partition][2 * block] (PFFFT order, scaled). Partition q of
    // stage s is the response's segment [(first + q + lag) B, (first + q + lag + 1) B)
    std::array<AlignedFloats, kStages> spec;
    std::array<int, kStages> first{}, parts{};

    static int maxLength(double sr) noexcept;
    static int stageEnd(int stage, double sr) noexcept
    {
        return stage + 1 < kStages ? kStart[stage + 1] : maxLength(sr);
    }

    /** ray-traces the room (as heard right now), synthesises and partitions the response.
     * Background thread. */
    static std::shared_ptr<const TailIR> build(const RoomParams &r, float air, int outputOrder,
                                               double sr, uint32_t serial);
    /** any responses (one per output channel, at most maxLength samples), partitioned for the
       convolver. Background thread. (The convolver leaves out the first kStart[0] samples.) */
    static std::shared_ptr<TailIR> fromResponses(const std::vector<std::vector<float>> &h,
                                                 double sr, uint32_t serial);
    void partition(const std::vector<std::vector<float>> &h);
};

class TailConvolver
{
public:
    void prepare(double sampleRate);
    void reset() noexcept;
    /** audio thread: a new response; crossfades to it as soon as every stage has computed it once
     */
    void setIR(std::shared_ptr<const TailIR> ir) noexcept;
    /** adds the reverb of 'in' times the gain (ramped from g0 to g1) to the outputs */
    void process(const float *in, float *const *out, int numOut, int n, float g0,
                 float g1) noexcept;
    const TailIR *current() const noexcept
    {
        return cur.get();
    }
    /** audio thread, while the convolver is not used: switch to the newest response right away */
    void adoptLatest() noexcept;
    uint32_t oldestSerialInUse() const noexcept;

private:
    static constexpr int kFade = 4096;
    static constexpr int kMaxCh = 16;
    static constexpr uint32_t kNone = 0xffffffffu;
    /** outputs [set * kMaxCh, set * kMaxCh + kMaxCh) of every stage: two sets, the sounding
     * response and the next one */
    void assign(int set, const TailIR *ir) noexcept;
    void trackApplied() noexcept;

    double sr = 48000.0;
    std::array<PartitionedConvolution, TailIR::kStages> stages;
    std::shared_ptr<const TailIR> cur, next, queued;
    int curSet = 0;
    int fade = -1; // >= 0: crossfading cur -> next, samples done
    // which responses the stages still read (their spectra must stay alive): applied / waiting for
    // the next block
    std::array<std::array<uint32_t, 2>, TailIR::kStages> appliedSerial{}, pendingSerial{};
    std::array<std::array<bool, 2>, TailIR::kStages> isPending{};
};
} // namespace rysum
