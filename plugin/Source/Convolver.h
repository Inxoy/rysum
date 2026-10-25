#pragma once

#include "Common.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <vector>

namespace rysum
{
/** Multichannel room response ("Hall") from a recorded impulse response.

    One mono input (the omnidirectional part of the dry organ) is convolved with every
    channel of the IR. Uniformly partitioned overlap-save convolution; the input spectrum
    is computed once and shared by all IR channels, so a 36-channel IR costs one forward
    FFT plus 36 inverse FFTs per partition block. Latency of the wet signal: kBlock samples
    (~11 ms at 48 kHz, acts like a tiny pre-delay); the dry signal is not delayed.

    Everything is allocated in create() (background thread); process() is real-time safe. */
class Convolver
{
public:
    static constexpr int kBlock = 512;
    static constexpr int kFft = 2 * kBlock;
    static constexpr int kBins = kFft / 2 + 1;
    static constexpr double kMaxSeconds = 10.0;

    enum class Format { AmbiX, FuMa };

    struct Info
    {
        juce::String fileName, description;
        int channels = 0, order = -1;      // order -1 = not Ambisonics (mono / stereo)
        double seconds = 0;
    };

    /** reads, resamples, normalises and partitions an IR file. nullptr + error on failure. */
    static std::unique_ptr<Convolver> create (const juce::File& file, double hostRate, Format format, juce::String& error);

    /** convolve 'n' samples of 'input' and ADD the result (times wetGain) to out[0..numOut).
        stereoOut = true: out has L/R, the IR is mapped to stereo (see .cpp). */
    void process (const float* input, int n, float* const* out, int numOut, bool stereoOut, float wetGain) noexcept;

    const Info& getInfo() const noexcept { return info; }
    void reset() noexcept;

private:
    Convolver() = default;
    void processBlock() noexcept;

    Info info;
    int numIr = 0;               // IR channels kept
    int numParts = 0;            // partitions per channel
    juce::dsp::FFT fft { 10 };   // 2^10 = kFft

    // IR spectra: [channel][partition][bin] split in re / im
    std::vector<float> irRe, irIm;
    // frequency-domain delay line of input spectra: [partition][bin]
    std::vector<float> fdlRe, fdlIm;
    int fdlPos = 0;

    std::vector<float> inBuf;            // 2*kBlock: previous + current input block
    std::vector<float> work;             // 2*kFft scratch for the FFT
    std::vector<float> accRe, accIm;     // kBins
    std::vector<float> outBuf;           // [channel][kBlock] wet output of the last block
    int pos = 0;                         // position inside the current block
};
} // namespace rysum
