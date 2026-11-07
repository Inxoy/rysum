#pragma once

#include "Common.h"
#include "PartConv.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <vector>

namespace rysum
{
/** Multichannel room response ("Hall") from a recorded impulse response.

    Why not simply "mono send * IR": with one send for all pipes, the measured direct sound and early
    reflections would always come from where the measurement loudspeaker stood, and a mono IR would
    only feed W (one phantom spot). Here:

    - The IR is turned so that its direct sound points to azimuth 0 and its EARLY part (first ~80 ms
      after the direct sound) is turned again for every pipe towards that pipe's direction. Because a
      rotation about the vertical axis only mixes each (m, -m) pair with cos/sin(m*phi), this needs no
      per-pipe convolution: the engine sends the organ as circular harmonics
          C0 = sum s_p,  C_m = sum cos(m*phi_p) s_p,  S_m = sum sin(m*phi_p) s_p      (m = 1..order)
      and every output channel (l, m) is  h(l,m) * C_m -/+ h(l,-m) * S_m.
    - The LATE part (diffuse tail) is fed by C0 only and can be blended with a synthetic isotropic
      field ("Umhuellung"): random-phase copies of the omni tail's time-frequency envelope, one per
      channel, with the energy a diffuse field has in SN3D (1 / (2l+1) of W). This also lifts mono /
      stereo / low-order IRs to the full output order.
    - The direct sound of the IR can be turned down ("Direktschall"): the dry pipes are the direct sound.

    Non-uniformly partitioned overlap-save convolution (PartitionedConvolution, PFFFT): the early part and the
    first ~75 ms of the tail in blocks of 512, the tail after that in blocks of 2048 and 8192 whose work is spread
    evenly over time (no CPU spikes). All memory allocated in create() (background thread), process() is
    real-time safe. Wet latency: one block (512 samples), as before. */
class Convolver
{
public:
    static constexpr int kBlock = 512;
    static constexpr int kFft = 2 * kBlock;
    static constexpr int kBins = kFft / 2 + 1;
    static constexpr double kMaxSeconds = 10.0;

    enum class Format { AmbiX, FuMa, N3D };

    struct Settings
    {
        Format format = Format::AmbiX;
        int outOrder = 1;            // Ambisonics order of the output (stereo output: 1, decoded at the end)
        bool stereoOut = false;
        float direct = 0.2f;         // gain of the IR's own direct sound
        float envelop = 0.6f;        // 0 = tail as measured .. 1 = fully diffuse (isotropic) tail
        double hostRate = 48000.0;
    };

    struct Info
    {
        juce::String fileName, description;
        int channels = 0, order = -1;      // order -1 = not Ambisonics (mono / stereo)
        double seconds = 0;
        float baseAzDeg = 0.0f;            // direction of the IR's direct sound (as measured)
        int numInputs = 1;                 // send signals expected by process()
        int harmonics = 0;                 // = (numInputs - 1) / 2
        bool legacyStereo = false;         // stereo IR on stereo output: plain L/R convolution
    };

    static std::unique_ptr<Convolver> create (const juce::File& file, const Settings& s, juce::String& error);

    /** inputs[0] = all pipes, inputs[2m-1] = cos(m phi) weighted, inputs[2m] = sin(m phi) weighted
        (Info::numInputs signals). Convolves 'n' samples and ADDS the result times wetGain to out. */
    void process (const float* const* inputs, int n, float* const* out, int numOut, float wetGain) noexcept;

    const Info& getInfo() const noexcept { return info; }
    const Settings& getSettings() const noexcept { return settings; }
    void reset() noexcept;

private:
    Convolver() = default;

    struct Link { int input; int irChannel; float sign; };

    Info info;
    Settings settings;
    int numIn = 1, numOut = 1;
    int partsTotal = 0;          // partitions of the whole IR (blocks of 512)
    int partsEarly = 0;          // early partitions (0 = no early part: everything is 'late')
    int lateFirst = 0;           // first partition of the late part (overlaps the last early one: crossfade)
    std::vector<std::array<Link, 2>> links;     // per output channel
    std::vector<int> numLinks;

    // stage A (block 512, the early part per pipe direction + the start of the tail), B (2048) and C (8192):
    // the rest of the tail, delayed by one block like stage A's output
    static constexpr int kBlockB = 2048, kBlockC = 8192;
    static constexpr int kTailA = 7;            // the tail's partitions of 512 before stage B takes over (7 * 512 = 3584)
    PartitionedConvolution convA, convB, convC;
    bool hasB = false, hasC = false;
    AlignedFloats earlySpec, lateSpecA, lateSpecB, lateSpecC;    // [channel][partition][2 * block]
};
} // namespace rysum
