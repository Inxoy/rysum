#pragma once

#include <atomic>
#include <cmath>
#include <vector>

namespace rysum
{
/** "Zeit als Entfernung": the far end of the room plays the past. Everything that is heard runs
    through a long memory; how far back it is read depends on where the listener stands. Walking
    forwards goes into the past - quickly enough, and time is heard running backwards - walking
    back returns to the present, catching up at up to four times the speed.
    Built on the message thread for a number of channels, used by the audio thread. */
class TimeDistance
{
public:
    static constexpr float kMaxSeconds = 20.0f;

    TimeDistance(int numChannels, double sampleRate)
        : channels(numChannels < 1 ? 1 : numChannels), rate(sampleRate)
    {
        // (at most ~48 MB: many Ambisonics channels shorten how far back it goes)
        const double budget = 12.0e6 / (double)channels;
        const double wanted = (double)kMaxSeconds * rate + 8.0;
        length = (int)(wanted < budget ? wanted : budget);
        if (length < 64)
            length = 64;
        memory.assign((size_t)channels * (size_t)length, 0.0f);
    }
    int numChannels() const noexcept
    {
        return channels;
    }
    double sampleRate() const noexcept
    {
        return rate;
    }
    float maxSeconds() const noexcept
    {
        return (float)((double)(length - 8) / rate);
    }
    /** how far back it reads right now (s) */
    float secondsNow() const noexcept
    {
        return (float)(delay / rate);
    }

    /** armed: the memory runs (also while the listener stands in the present); seconds: how far
        back to read. Not armed and back in the present: nothing happens, the signal is untouched. */
    void process(float *const *io, int numCh, int n, bool armed, float seconds) noexcept
    {
        if (!armed && delay <= 0.0)
        {
            valid = 0; // (what was remembered is forgotten)
            return;
        }
        if (numCh > channels)
            numCh = channels;
        const double maxDelay = (double)(length - 8);
        double target = armed ? (double)seconds * rate : 0.0;
        target = target < 0.0 ? 0.0 : (target > maxDelay ? maxDelay : target);
        const double k = 1.0 - std::exp(-1.0 / (0.08 * rate));
        const double accel = 1.0 - std::exp(-1.0 / (0.01 * rate));
        for (int i = 0; i < n; ++i)
        {
            // towards where the listener stands - never faster than twice backwards / four times
            // forwards through time (a jump to the usual place glides there too)
            // (the speed itself changes smoothly, within a hundredth of a second: a sudden change
            // of speed would be a kink in the sound)
            double want = k * (target - delay);
            want = want > 3.0 ? 3.0 : (want < -3.0 ? -3.0 : want);
            speed += accel * (want - speed);
            delay += speed;
            if (delay < 0.0)
                delay = 0.0;
            if (std::abs(delay - target) < 1.0e-3 && std::abs(speed) < 1.0e-4)
            {
                delay = target;
                speed = 0.0;
            }
            for (int c = 0; c < numCh; ++c)
                memory[(size_t)c * (size_t)length + (size_t)write] = io[c][i];
            if (delay > 0.0)
            {
                double rp = (double)write - delay;
                if (rp < 0.0)
                    rp += (double)length;
                const int i0 = (int)rp;
                const float fr = (float)(rp - (double)i0);
                const int i1 = i0 + 1 >= length ? 0 : i0 + 1;
                const bool have = delay + 2.0 < (double)valid; // (before that: nothing was played)
                for (int c = 0; c < numCh; ++c)
                {
                    const float *m = memory.data() + (size_t)c * (size_t)length;
                    io[c][i] = have ? m[i0] * (1.0f - fr) + m[i1] * fr : 0.0f;
                }
            }
            if (++write >= length)
                write = 0;
            if (valid < length)
                ++valid;
        }
    }

private:
    int channels = 1, length = 64, write = 0, valid = 0;
    double rate = 48000.0, delay = 0.0, speed = 0.0;
    std::vector<float> memory;
};
} // namespace rysum
