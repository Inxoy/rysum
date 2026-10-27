#pragma once

#include "Common.h"
#include <juce_dsp/juce_dsp.h>
#include <functional>
#include <memory>
#include <vector>

namespace rysum
{
/** a set of head-related impulse responses: one stereo IR per direction */
struct HrirSet
{
    juce::String name, info;                 // e.g. "FABIAN (TU Berlin)", "2000 Richtungen, 48 kHz"
    double sampleRate = 48000.0;
    int length = 0;                          // samples per IR
    std::vector<float> az, el;               // degrees, AmbiX convention (azimuth counter-clockwise = to the left)
    std::vector<float> ir;                   // [direction][ear 0 = left, 1 = right][sample]
    int numDirections() const noexcept { return (int) az.size(); }
    const float* get (int dir, int ear) const noexcept { return ir.data() + ((size_t) dir * 2 + (size_t) ear) * (size_t) length; }

    /** the FABIAN head-and-torso simulator (TU Berlin, CC BY 4.0), 2000 directions, built into the plugin */
    static std::unique_ptr<HrirSet> fabian (juce::String& error);
    /** any SOFA file of the convention SimpleFreeFieldHRIR (read with libmysofa) */
    static std::unique_ptr<HrirSet> fromSofa (const juce::File& file, juce::String& error);
};

/** Ambisonics (AmbiX: ACN / SN3D) -> binaural decoding filters, one FIR per ear and Ambisonics channel.

    Designed like the IEM BinauralDecoder: magnitude least squares (MagLS, Schoerkhuber, Zaunschirm and
    Hoeldrich 2018). Below a cutoff that grows with the order (~620 Hz x order) the HRTFs are fitted
    with spherical harmonics in the usual least-squares sense (correct interaural time differences);
    above it only the magnitudes are fitted and the phase is left free (taken from the fit of the
    previous frequency) - at low orders the phase of the high frequencies cannot be represented
    anyway, and trying to only costs level and timbre. Then the diffuse-field energy of the fit is
    matched to the one of the HRTFs (no dull top end), optionally the whole set is diffuse-field
    equalised (minimum phase), and the filters are resampled to the host rate. */
struct BinauralFilters
{
    int order = 1, numChannels = 4;
    double sampleRate = 48000.0;
    int length = 0;
    std::vector<float> h;                    // [ear][channel][sample]
    juce::String description;
    const float* get (int ear, int ch) const noexcept { return h.data() + ((size_t) ear * (size_t) numChannels + (size_t) ch) * (size_t) length; }

    static std::unique_ptr<BinauralFilters> design (const HrirSet& set, int order, double hostRate, bool diffuseEq,
                                                    std::function<bool()> shouldAbort = {});
};

/** real-time part: uniformly partitioned convolution (block 128) of up to 36 Ambisonics channels into
    two ears, plus a rotation about the vertical axis (head turned). Latency: 128 samples. */
class BinauralDecoder
{
public:
    static constexpr int kBlock = 128;
    static constexpr int kFft = 2 * kBlock;
    static constexpr int kBins = kFft / 2 + 1;

    explicit BinauralDecoder (const BinauralFilters& f);

    int getOrder() const noexcept { return order; }
    int getNumChannels() const noexcept { return numCh; }
    const juce::String& getDescription() const noexcept { return description; }

    /** in: numCh AmbiX channels (rotated in place by the head yaw), out: left / right (overwritten).
        yawDeg is ramped from the previous call. */
    void process (float* const* in, int n, float* left, float* right, float yawDeg) noexcept;
    void reset() noexcept;

private:
    void processBlock() noexcept;
    void rotate (float* const* in, int n, float yawDeg) noexcept;

    int order = 1, numCh = 4, parts = 1;
    juce::String description;
    juce::dsp::FFT fft { 8 };               // 2^8 = kFft
    std::vector<float> hRe, hIm;            // [ear][ch][part][bin]
    std::vector<float> fdlRe, fdlIm;        // [ch][slot][bin]
    int fdlPos = 0;
    std::vector<float> inBuf;               // [ch][2 * kBlock]
    std::vector<float> outBuf;              // [ear][kBlock]
    std::vector<float> work, accRe, accIm;
    int pos = 0;
    float lastYaw = 0.0f;
};
} // namespace rysum
