#pragma once

#include "RoomTail.h"
#include <functional>
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <vector>

namespace rysum
{
/** Lucier mode, after Alvin Lucier's "I am sitting in a room" (1969): a recording is played into a
   room and recorded again, over and over, until only the room's resonances are left.

    Every pass multiplies the sound's spectrum by the room's response H once more, so generation n
   is the sound convolved n times with the room (H^n): the room's strongest resonances rise above
   everything else. Here this is done live: the response of n passes is computed in the background
   and convolved with the organ (crossfaded to the next one whenever the generation changes, also
   smoothly between whole numbers).

    - The room is either the simulated hall (its eigenmodes from the dimensions up to about twice
   the Schroeder frequency, a diffuse field with the Eyring decay above) or the impulse response
   loaded on the Raum page (later: the church in Rysum itself).
    - As in Lucier's setup, loudspeaker and microphone are part of every pass: a gentle band from
   about 60 Hz to 10 kHz.
    - Time: a real pass lasts the whole recording; to hear the resonances while playing, every pass
   is damped ("Nachklingen", and more for high generations), so the sound swells up within a few
   seconds instead of minutes. */
struct LucierBase
{
    std::vector<float> h; // one pass through the room, mono, unit energy, at the host rate
    double sampleRate = 48000.0;
    juce::String what; // for the display
};

std::shared_ptr<const LucierBase> lucierFromRoom(const RoomParams &r, float air, double sampleRate);
std::shared_ptr<const LucierBase> lucierFromFile(const juce::File &file, double sampleRate,
                                                 juce::String &error);

struct LucierResult
{
    std::shared_ptr<const TailIR> ir; // mono
    std::vector<float> curve;         // dB (max 0) at kCurvePoints log-spaced frequencies
    std::vector<float> peaks;         // the strongest resonances (Hz), strongest first
    float generation = 1.0f;
    static constexpr int kCurvePoints = 220;
    static constexpr float kCurveLo = 30.0f, kCurveHi = 12000.0f;
};

/** generation >= 1 (fractions blend two whole passes); tau = damping of every pass (s). Background
 * thread. */
LucierResult buildLucier(const LucierBase &base, float generation, float tau, uint32_t serial);

/** the piece itself ("Stueck"): one real pass = room x loudspeaker x microphone, undamped, unit rms
 * in the band */
std::vector<float> lucierPassResponse(const LucierBase &base);
/** a recording played into the room and recorded again: x convolved with one pass, at most maxLen
   samples long (FFT overlap-add; background thread). shouldStop() may cancel (returns an empty
   vector). */
std::vector<float> lucierPass(const std::vector<float> &x, const std::vector<float> &pass,
                              int maxLen, std::function<bool()> shouldStop = {});
} // namespace rysum
