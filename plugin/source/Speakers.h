#pragma once

#include "Spatial.h"
#include <array>
#include <atomic>
#include <juce_core/juce_core.h>
#include <memory>
#include <vector>

namespace rysum
{
/** a loudspeaker: direction as seen from the listening position, distance, output channel (1-based)
 */
struct Speaker
{
    float az = 0.0f, el = 0.0f, radius = 2.0f; // deg (0 = front, +90 = left), deg, m
    int channel = 1;
    float gainDb = 0.0f;
    bool imaginary = false; // only helps the triangulation; its signal is dropped
};

struct SpeakerLayout
{
    juce::String name;
    std::vector<Speaker> speakers;
};

/** some common layouts (name, speakers) */
std::vector<SpeakerLayout> speakerPresets();

/** IEM Plug-in Suite layout files (AllRADecoder / SimpleDecoder JSON: "LoudspeakerLayout" ->
 * "Loudspeakers") */
bool speakerLayoutFromJson(const juce::String &json, SpeakerLayout &out, juce::String &error);
juce::String speakerLayoutToJson(const SpeakerLayout &layout);

/** AllRAD (Zotter & Frank 2012), as in the IEM AllRADecoder: a dense virtual layout on the whole
   sphere is decoded with max-rE weights, every virtual loudspeaker is panned onto the real ones
   with VBAP (triangulated convex hull, imaginary loudspeakers fill gaps and are dropped). Input:
   ACN / SN3D up to 5th order. */
struct AllRadDecoder
{
    int order = 3;
    int numIn = 16;                         // (order + 1)^2
    std::vector<std::vector<float>> matrix; // [real speaker][ACN]
    std::vector<int> outChannel;            // 0-based, per real speaker
    std::vector<float> delaySec, gain;      // distance compensation + own gain, per real speaker
    int numOutChannels = 0;                 // highest channel used
    // for the display
    std::vector<Speaker> all;                  // the layout plus added imaginary loudspeakers
    std::vector<std::array<int, 3>> triangles; // indices into 'all'
    int addedImaginary = 0;
    std::vector<float> energyMap; // dB re mean, az -180..180 step 5 (73) x el -90..90 step 5 (37)
    float energyMinDb = 0, energyMaxDb = 0, angleErrMean = 0, angleErrMax = 0;
    juce::String error;
    uint32_t serial = 0;
    static constexpr int kMapAz = 73, kMapEl = 37;
};

std::shared_ptr<AllRadDecoder> buildAllRad(const SpeakerLayout &layout, int order,
                                           bool distanceCompensation, uint32_t serial);

/** audio thread: Ambisonics in -> loudspeaker channels out, with delay / gain per speaker; a new
 * decoder is crossfaded in */
class SpeakerRenderer
{
public:
    void prepare(double sampleRate, int maxBlock);
    void setDecoder(std::shared_ptr<const AllRadDecoder> d) noexcept
    {
        pending = std::move(d);
    }
    const AllRadDecoder *current() const noexcept
    {
        return cur.get();
    }
    /** the decoder that will be used for the next block (audio thread) */
    const AllRadDecoder *next() const noexcept
    {
        return pending != nullptr ? pending.get() : cur.get();
    }
    /** 'out' channels are overwritten (all of them: the ones without a speaker are cleared) */
    void process(const float *const *ambi, int numAmbi, float *const *out, int numOut,
                 int n) noexcept;
    std::atomic<uint32_t> serialInUse{0};

private:
    std::shared_ptr<const AllRadDecoder> cur, prev, pending;
    double sr = 48000.0;
    int fadeLeft = 0, fadeLen = 1;
    static constexpr int kDelayBits = 14, kDelayLen = 1 << kDelayBits; // up to ~0.34 s at 48 kHz
    std::vector<float> delayBuf;                                       // [64][kDelayLen]
    uint32_t delayPos = 0;
    std::vector<float> tmp, tmpPrev;
};
} // namespace rysum
