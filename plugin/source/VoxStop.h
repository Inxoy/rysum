#pragma once

#include "Common.h"
#include "OrganEngine.h"
#include "vox/VoxEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace rysum
{
/** streaming sample-rate converter (4-point Hermite) for the Vox humana's 48 kHz world */
class StreamResampler
{
public:
    /** step = input samples per output sample */
    void prepare(double stepIn, int maxBlock, double lagSamples)
    {
        step = stepIn;
        int cap = 1;
        while (cap < 4 * maxBlock + 256)
            cap <<= 1;
        ring.assign((size_t)cap, 0.0f);
        mask = cap - 1;
        lag = lagSamples;
        reset();
    }
    void reset() noexcept
    {
        std::fill(ring.begin(), ring.end(), 0.0f);
        written = 0;
        pos = 1.0 - lag; // starts `lag` samples behind (and with one sample of history)
    }
    void push(const float *x, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
            ring[(size_t)((written + i) & mask)] = x != nullptr ? x[i] : 0.0f;
        written += n;
    }
    /** writes up to maxOut samples, as many as the input allows; returns how many */
    int pull(float *out, int maxOut) noexcept
    {
        int k = 0;
        while (k < maxOut && pos + 2.0 < (double)written)
        {
            out[k++] = at(pos);
            pos += step;
        }
        return k;
    }
    /** exactly n samples; missing input (only right at the start) gives silence */
    void pullExactly(float *out, int n) noexcept
    {
        for (int k = 0; k < n; ++k)
        {
            if (pos + 2.0 < (double)written && pos >= 1.0)
                out[k] = at(pos);
            else
                out[k] = 0.0f;
            pos += step;
        }
    }

private:
    float at(double p) const noexcept
    {
        const int64_t i = (int64_t)std::floor(p);
        const float t = (float)(p - (double)i);
        const float xm = ring[(size_t)((i - 1) & mask)], x0 = ring[(size_t)(i & mask)],
                    x1 = ring[(size_t)((i + 1) & mask)], x2 = ring[(size_t)((i + 2) & mask)];
        const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2,
                    c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }
    std::vector<float> ring;
    int64_t mask = 0, written = 0;
    double step = 1.0, pos = 0.0, lag = 0.0;
};

/** The Vox humana stop and "Eingang spielt die Orgel": runs the neural engine at 48 kHz on the
    host's block, plays the held keys in the organ's tuning, and turns what it hears in the input
    into notes for the sample organ. */
class VoxStop
{
public:
    struct Settings
    {
        bool stopOn = false;   // the stop is pulled: the Vox humana sounds
        bool inputPlays = false; // the input's notes play the organ (all pulled stops)
        bool follow = false;   // the input steers the Vox humana's timbre
        float wind = 1.0f;     // 0..1, like a stop's wind
        float x = 0.0f, y = 0.0f; // timbre (the two main axes of the latent, in std devs)
        float bright = 0.0f, noisy = 0.0f, spread = 0.0f;
        float room = 1.0f;     // learned room 0..1
        float thresh = 0.6f;   // tracker: start threshold
        int confirm = 4;       // tracker: frames a pitch must last
        int voices = 8;        // tracker: most notes at once
    };

    bool load(const void *data, size_t size, juce::String &error)
    {
        std::string err;
        if (!eng.load(data, size, err))
        {
            error = juce::String(err);
            return false;
        }
        return true;
    }
    bool isLoaded() const noexcept { return eng.isLoaded(); }

    void prepare(double hostRate, int maxBlock)
    {
        rate = hostRate;
        bypass = std::abs(hostRate - 48000.0) < 0.5;
        const int max48 = (int)std::ceil(maxBlock * 48000.0 / hostRate) + 8;
        toModel.prepare(hostRate / 48000.0, maxBlock, 0.0);
        fromModel.prepare(48000.0 / hostRate, max48, 4.0);
        in48.assign((size_t)max48 + 8, 0.0f);
        out48.assign((size_t)max48 + 8, 0.0f);
        mono.assign((size_t)maxBlock, 0.0f);
        out.assign((size_t)maxBlock, 0.0f);
        eng.reset();
        heldTracked.fill(-1);
        noteCount.fill(0);
        playedOn.fill(false);
        sustain = false;
        sustained.fill(false);
        wasActive = false;
    }

    /** audio thread. inL / inR: the input of this block (null = none). midi: the block's MIDI;
        tracked notes are added to `organMidi` (channel 16) when the input plays the organ.
        Afterwards output() holds the Vox humana (mono, host rate). */
    void process(const float *inL, const float *inR, int n, const juce::MidiBuffer &midi,
                 juce::MidiBuffer &organMidi, const Settings &s, const EngineParams &p) noexcept
    {
        const bool active = eng.isLoaded() && (s.stopOn || s.inputPlays);
        if (n > (int)out.size())
            n = (int)out.size(); // (never: prepare() sized it for the host's block)
        if (!active)
        {
            if (wasActive)
            {
                releaseTracked(organMidi, 0);
                eng.reset();
                toModel.reset();
                fromModel.reset();
                playedOn.fill(false);
                wasActive = false;
            }
            std::fill(out.begin(), out.begin() + n, 0.0f);
            return;
        }
        wasActive = true;

        // controls
        eng.synthOn = s.stopOn;
        eng.trackInput = s.inputPlays;
        eng.tracedVoices = true;
        eng.follow = s.follow;
        eng.setRoomMix(s.room);
        eng.setLatent(0, s.x);
        eng.setLatent(1, s.y);
        // a half-drawn stop: duller and breathier (and quieter, see level())
        const float lack = 1.0f - juce::jlimit(0.0f, 1.0f, s.wind);
        eng.setDescriptor(0, s.bright - 0.6f * lack);
        eng.setDescriptor(1, s.noisy + 0.5f * lack);
        eng.setDescriptor(2, s.spread);
        auto &tp = eng.trackerParams();
        tp.on = s.thresh;
        tp.confirm = juce::jlimit(1, 32, s.confirm);
        tp.voices = juce::jlimit(1, 8, s.voices);

        // keys -> played voices (in the organ's tuning)
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            const int off48 = toModelOffset(meta.samplePosition);
            if (m.isNoteOn() && s.stopOn)
            {
                const int id = (m.getChannel() - 1) * 128 + m.getNoteNumber();
                eng.noteOn(off48, id, keyFor(m.getChannel(), m.getNoteNumber(), p));
                playedOn[(size_t)id] = true;
                sustained[(size_t)id] = false;
            }
            else if (m.isNoteOff())
            {
                const int id = (m.getChannel() - 1) * 128 + m.getNoteNumber();
                if (sustain)
                    sustained[(size_t)id] = playedOn[(size_t)id];
                else if (playedOn[(size_t)id])
                {
                    eng.noteOff(off48, id);
                    playedOn[(size_t)id] = false;
                }
            }
            else if (m.isSustainPedalOn())
                sustain = true;
            else if (m.isSustainPedalOff())
            {
                sustain = false;
                for (int id = 0; id < (int)sustained.size(); ++id)
                    if (sustained[(size_t)id])
                    {
                        eng.noteOff(off48, id);
                        playedOn[(size_t)id] = false;
                        sustained[(size_t)id] = false;
                    }
            }
            else if (m.isAllNotesOff() || m.isAllSoundOff())
            {
                eng.allNotesOff(off48);
                playedOn.fill(false);
                sustained.fill(false);
            }
        }
        if (!s.stopOn)
        {
            bool any = false;
            for (bool b : playedOn)
                any = any || b;
            if (any)
            {
                eng.allNotesOff(0);
                playedOn.fill(false);
            }
        }

        // input: mono at 48 kHz
        for (int i = 0; i < n; ++i)
            mono[(size_t)i] = inL == nullptr ? 0.0f : (inR != nullptr ? 0.5f * (inL[i] + inR[i]) : inL[i]);
        int m48 = n;
        const float *src = mono.data();
        if (!bypass)
        {
            toModel.push(mono.data(), n);
            m48 = toModel.pull(in48.data(), (int)in48.size());
            src = in48.data();
        }
        eng.process(src, bypass ? out.data() : out48.data(), m48);
        if (!bypass)
        {
            fromModel.push(out48.data(), m48);
            fromModel.pullExactly(out.data(), n);
        }

        // what the input played -> notes for the sample organ
        if (s.inputPlays)
            for (int e = 0; e < eng.numTrackedEvents(); ++e)
            {
                const auto &ev = eng.trackedEvent(e);
                const int at = juce::jlimit(0, n - 1, (int)std::lround(ev.offset * (bypass ? 1.0 : rate / 48000.0)));
                if (ev.on)
                {
                    if (heldTracked[(size_t)ev.voice] >= 0)
                        noteOffTracked(organMidi, ev.voice, at);
                    const int k = organKey(ev.key);
                    heldTracked[(size_t)ev.voice] = k;
                    if (noteCount[(size_t)k]++ == 0)
                        organMidi.addEvent(juce::MidiMessage::noteOn(kChannel, k, (juce::uint8)96), at);
                }
                else
                    noteOffTracked(organMidi, ev.voice, at);
            }
        else
            releaseTracked(organMidi, 0);
    }

    const float *output() const noexcept { return out.data(); }
    /** the tracker's notes right now (for displays): MIDI key or -1 */
    int trackedNote(int v) const noexcept { return heldTracked[(size_t)v]; }

    static constexpr int kChannel = 16; // the input's notes arrive on this MIDI channel

private:
    int toModelOffset(int hostOffset) const noexcept
    {
        return bypass ? hostOffset : (int)std::lround(hostOffset * 48000.0 / rate);
    }

    /** fractional MIDI key of a note in the organ's tuning (and Stimmton) */
    static float keyFor(int channel, int note, const EngineParams &p) noexcept
    {
        double hz = 0.0;
        if (p.notes != nullptr)
        {
            const size_t i = (size_t)(juce::jlimit(0, 15, channel - 1) * 128 + juce::jlimit(0, 127, note));
            hz = p.notes->hz[i];
        }
        else
            hz = keyboardTuningHz(p.tuning, note) * std::exp2(p.pitchCents / 1200.0);
        if (!(hz > 1.0))
            hz = 440.0 * std::exp2((note - 69) / 12.0);
        return (float)(69.0 + 12.0 * std::log2(hz / 440.0));
    }

    /** the organ key closest to a tracked pitch: folded into the compass, skipping the short octave's gaps */
    static int organKey(float key) noexcept
    {
        int m = (int)std::lround(key);
        const int lo = kPhysKeys.front(), hi = kPhysKeys.back();
        while (m < lo)
            m += 12;
        while (m > hi)
            m -= 12;
        if (keyIndexForMidi(m) < 0)
        {
            if (m + 12 <= hi && keyIndexForMidi(m + 12) >= 0)
                m += 12;
            else if (m - 12 >= lo && keyIndexForMidi(m - 12) >= 0)
                m -= 12;
        }
        return juce::jlimit(0, 127, m);
    }

    void noteOffTracked(juce::MidiBuffer &organMidi, int v, int at) noexcept
    {
        const int k = heldTracked[(size_t)v];
        if (k < 0)
            return;
        heldTracked[(size_t)v] = -1;
        if (noteCount[(size_t)k] > 0 && --noteCount[(size_t)k] == 0)
            organMidi.addEvent(juce::MidiMessage::noteOff(kChannel, k), at);
    }

    void releaseTracked(juce::MidiBuffer &organMidi, int at) noexcept
    {
        for (int v = 0; v < (int)heldTracked.size(); ++v)
            noteOffTracked(organMidi, v, at);
    }

    vox::Engine eng;
    StreamResampler toModel, fromModel;
    std::vector<float> in48, out48, mono, out;
    double rate = 48000.0;
    bool bypass = true, wasActive = false, sustain = false;
    std::array<int, vox::Engine::kTracked> heldTracked{};
    std::array<int, 128> noteCount{};
    std::array<bool, 16 * 128> playedOn{}, sustained{};
};
} // namespace rysum
