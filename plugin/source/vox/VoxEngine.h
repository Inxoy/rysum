// Vox humana engine: NeuralRysum v2 at 48 kHz, frame by frame.
//
//   input --> PitchEstimator --> VoiceTracker --> tracked voices --\
//   MIDI notes -------------------------------> played voices ----> OrganSynth --> output
//   input --> timbre encoder (Follow) / knobs (Manual) --> conditioning --/
//
// The tracker's notes are also reported as note events (trackedEvents()), so the sample organ
// can play along. Everything runs at the model rate (48 kHz); the plugin resamples around it.
// Latency: one synth frame (128 samples) for any block size. Real-time safe after load().
#pragma once

#include "VoxModel.h"
#include "VoxPitch.h"
#include "VoxSynth.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace vox
{
class Engine
{
public:
    static constexpr int kTracked = 8, kPlayed = 8;
    static constexpr int kMaxEvents = 512, kMaxQueued = 256;

    struct NoteEvent
    {
        int offset = 0;     // sample within the input of the last process() call
        int voice = 0;      // tracker voice
        float key = 0.0f;   // fractional MIDI note (folded)
        bool on = false;
    };

    // ---- setup (not real-time safe) ---------------------------------------------------------
    bool load(const void *bytes, size_t size, std::string &err)
    {
        ModelFile m;
        if (!m.loadFromMemory(bytes, size, err))
            return false;
        try
        {
            build(m);
        }
        catch (const std::exception &e)
        {
            err = e.what();
            loaded = false;
            return false;
        }
        loaded = true;
        reset();
        return true;
    }

    void reset() noexcept
    {
        if (!loaded)
            return;
        est.reset();
        tracker.reset();
        synth.reset();
        for (auto &c : encConv)
            c.reset();
        for (auto &c : encRes)
            c.reset();
        for (auto &c : slowConv)
            c.reset();
        std::fill(slowCount.begin(), slowCount.end(), 0);
        slowGru.reset();
        std::fill(followed.begin(), followed.end(), 0.0f);
        std::fill(inBuf.begin(), inBuf.end(), 0.0f);
        std::fill(outBuf.begin(), outBuf.end(), 0.0f);
        fill = 0;
        frame = 0;
        for (auto &t : trk)
            t = Tracked{};
        for (auto &p : played)
            p = Played{};
        nQueued = 0;
        nEvents = 0;
        condPrimed = false;
        std::fill(cond.begin(), cond.end(), 0.0f);
    }

    // ---- info -------------------------------------------------------------------------------
    bool isLoaded() const noexcept { return loaded; }
    int sampleRate() const noexcept { return synth.sampleRate(); }
    int latencySamples() const noexcept { return synth.hopSize(); }
    int latentDim() const noexcept { return zDim; }

    // ---- controls (audio thread, between process() calls) ------------------------------------
    bool trackInput = true;   // run the pitch estimator on the input
    bool synthOn = true;      // render the Vox humana (else only tracking / events)
    bool tracedVoices = true; // the synth also plays the tracked notes (not only MIDI)
    bool follow = false;      // Follow: the input steers the timbre (else Manual)
    float foldLo = 36.0f, foldHi = 84.0f;
    float smoothingMs = 30.0f;

    VoiceTracker::Params &trackerParams() noexcept { return tracker.params; }
    void setNoise(bool on) noexcept { synth.noiseOn = on; }
    void setRoomMix(float m) noexcept { synth.roomMix = m; }
    /** timbre knobs: latent axis i (0, 1 = the two main directions) in standard deviations */
    void setLatent(int i, float v) noexcept
    {
        if (i >= 0 && i < zDim)
            knobs[(size_t)i] = v;
    }
    /** descriptor offsets: 0 brightness, 1 noisiness, 2 spread (about -1 .. 1) */
    void setDescriptor(int i, float v) noexcept
    {
        if (i >= 0 && i < nDesc)
            descKnobs[(size_t)i] = v;
    }
    /** test hook: use exactly this conditioning (nullptr = back to knobs) */
    void setRawConditioning(const float *c) noexcept
    {
        useRaw = c != nullptr;
        if (useRaw)
            std::copy(c, c + condDim, raw.begin());
    }

    // ---- MIDI (played voices): times are samples into the next process() call ---------------
    void noteOn(int offset, int id, float key) noexcept { queue(offset, id, key, true); }
    void noteOff(int offset, int id) noexcept { queue(offset, id, 0.0f, false); }
    void allNotesOff(int offset) noexcept
    {
        for (const auto &p : played)
            if (p.held)
                queue(offset, p.id, 0.0f, false);
    }

    // ---- audio --------------------------------------------------------------------------------
    /** in: input (nullptr = silence), out: the Vox humana; n samples at the model rate */
    void process(const float *in, float *out, int n) noexcept
    {
        nEvents = 0;
        if (!loaded)
        {
            std::fill(out, out + n, 0.0f);
            nQueued = 0;
            return;
        }
        const int H = synth.hopSize();
        for (int i = 0; i < n; ++i)
        {
            out[i] = outBuf[(size_t)fill];
            inBuf[(size_t)fill] = in != nullptr ? in[i] : 0.0f;
            if (++fill == H)
            {
                blockEnd = i + 1; // the frame's input ends here (for event offsets)
                runFrame();
                fill = 0;
            }
        }
        // queued notes: make their times relative to the next call
        int keep = 0;
        for (int q = 0; q < nQueued; ++q)
        {
            Queued e = queued[(size_t)q];
            e.time -= n;
            queued[(size_t)keep++] = e;
        }
        nQueued = keep;
    }

    int numTrackedEvents() const noexcept { return nEvents; }
    const NoteEvent &trackedEvent(int i) const noexcept { return events[(size_t)i]; }

    /** current tracked voice (for displays): folded key, gate */
    float trackedKey(int v) const noexcept { return trk[(size_t)v].lastMidi; }
    bool trackedGate(int v) const noexcept { return trk[(size_t)v].lastGate; }
    const std::vector<float> &conditioning() const noexcept { return cond; }

private:
    struct Tracked
    {
        // what the last two estimator frames said (tracked_voices in v2.py)
        bool startedAny = false, lastGate = false;
        float lastMidi = 0.0f;
        // synth voice
        bool have = false;
        float key = 0.0f;
        double onT = 0.0, offT = -1.0;
        // note events for the organ
        bool noteOn = false;
        float noteKey = 0.0f;
    };
    struct Played
    {
        int id = -1;
        bool held = false, used = false;
        float key = 0.0f;
        int64_t on = 0, off = 0;     // samples, absolute
        double freeAt = -1e18;        // seconds
    };
    struct Queued
    {
        int64_t time = 0; // relative to the start of the current process() input
        int id = 0;
        float key = 0.0f;
        bool on = false;
    };

    void queue(int offset, int id, float key, bool on) noexcept
    {
        if (nQueued >= kMaxQueued)
            return;
        queued[(size_t)nQueued++] = {(int64_t)offset, id, key, on};
    }

    void build(const ModelFile &m)
    {
        est.load(m);
        synth.load(m);
        const auto &consts = m.t("consts").data;
        foldLo = consts[1];
        foldHi = consts[2];
        tracker.init(est.numBins(), (double)m.num("midi_lo"), m.num("bins_per_semitone"), kTracked);
        tracker.params.on = consts[3];
        tracker.params.off = consts[4];
        tracker.params.match = consts[5];
        tracker.params.onset = consts[6];
        tracker.params.hold = m.num("track_hold");
        tracker.params.confirm = m.num("track_confirm");
        estHop = est.hopSize();
        if (synth.hopSize() % estHop != 0)
            throw std::runtime_error("synth hop must be a multiple of the estimator hop");
        zDim = m.num("z_dim");
        nDesc = m.num("n_desc");
        condDim = zDim + nDesc;
        zManual = m.t("timbre.z_manual").data;
        dManual = m.t("timbre.d_manual").data;
        zBasis = m.t("timbre.z_basis").data;
        knobs.assign((size_t)zDim, 0.0f);
        descKnobs.assign((size_t)nDesc, 0.0f);
        cond.assign((size_t)condDim, 0.0f);
        target.assign((size_t)condDim, 0.0f);
        raw.assign((size_t)condDim, 0.0f);
        followed.assign((size_t)condDim, 0.0f);
        salP.assign((size_t)est.numBins(), 0.0f);
        onsP.assign((size_t)est.numBins(), 0.0f);

        // timbre encoder: v1's fast trunk + slow encoder (Follow)
        const auto encStrides = m.list("enc_strides");
        const auto slowStrides = m.list("slow_strides");
        encStride = encStrides;
        encHop = 1;
        for (int s : encStrides)
            encHop *= s;
        if (encHop != estHop)
            throw std::runtime_error("timbre trunk hop must equal the estimator hop");
        const int nEnc = (int)encStrides.size();
        encConv.assign((size_t)nEnc, {});
        encRes.assign((size_t)(2 * nEnc), {});
        int T = encHop;
        size_t scratch = (size_t)encHop;
        for (int i = 0; i < nEnc; ++i)
        {
            const std::string p = "synth.trunk.convs." + std::to_string(i) + ".conv";
            encConv[(size_t)i].init(m.t(p + ".weight"), m.t(p + ".bias"), encStrides[(size_t)i], 1, T);
            T /= encStrides[(size_t)i];
            const std::string r = "synth.trunk.res." + std::to_string(i);
            encRes[(size_t)(2 * i)].init(m.t(r + ".conv1.conv.weight"), m.t(r + ".conv1.conv.bias"), 1, 1, T);
            encRes[(size_t)(2 * i + 1)].init(m.t(r + ".conv2.conv.weight"), m.t(r + ".conv2.conv.bias"), 1, 1, T);
            scratch = std::max(scratch, (size_t)T * (size_t)encConv[(size_t)i].cout);
        }
        trunkCh = encConv.back().cout;
        ah.assign((size_t)trunkCh, 0.0f);
        const int nSlow = (int)slowStrides.size();
        slowConv.assign((size_t)nSlow, {});
        slowStage.assign((size_t)nSlow, {});
        slowTmp.assign((size_t)nSlow, {});
        slowCount.assign((size_t)nSlow, 0);
        slowStride = slowStrides;
        for (int i = 0; i < nSlow; ++i)
        {
            const std::string p = "synth.slow.convs." + std::to_string(i) + ".conv";
            slowConv[(size_t)i].init(m.t(p + ".weight"), m.t(p + ".bias"), slowStrides[(size_t)i], 1,
                                     slowStrides[(size_t)i]);
            slowStage[(size_t)i].assign((size_t)slowStrides[(size_t)i] * (size_t)slowConv[(size_t)i].cin, 0.0f);
            slowTmp[(size_t)i].assign((size_t)slowConv[(size_t)i].cout, 0.0f);
        }
        slowGru.init(m, "synth.slow.gru");
        slowHead.init(m.t("synth.slow.head.weight"), m.t("synth.slow.head.bias"));
        slowOut.assign((size_t)slowHead.out, 0.0f);
        for (auto *v : {&sA, &sB, &sC})
            v->assign(scratch, 0.0f);

        const int H = synth.hopSize();
        inBuf.assign((size_t)H, 0.0f);
        outBuf.assign((size_t)H, 0.0f);
        voices.assign((size_t)(kTracked + kPlayed), {});
    }

    // ---- one synth frame ------------------------------------------------------------------------
    void runFrame() noexcept
    {
        const int H = synth.hopSize();
        const int sr = synth.sampleRate();
        const int64_t T0 = frame * H; // first sample of this frame
        const double t = (double)frame * ((double)H / (double)sr);

        // played voices: apply queued notes that are due by the frame start
        const int64_t callStart = T0 + H - blockEnd; // absolute sample of offset 0 of this call
        applyQueued(callStart, T0);

        // tracked voices -> synth voices (from the estimator frames that ended before T0)
        int nv = 0;
        for (int v = 0; v < kTracked; ++v)
        {
            Tracked &s = trk[(size_t)v];
            if (frame > 0)
            {
                if (s.startedAny)
                {
                    s.onT = t;
                    s.offT = -1.0;
                    s.have = true;
                }
                if (s.lastGate)
                {
                    s.key = s.lastMidi;
                    s.offT = s.have ? -1.0 : s.offT;
                }
                else if (s.have && s.offT < 0.0)
                    s.offT = t;
            }
            VoiceState &vs = voices[(size_t)nv++];
            vs = VoiceState{};
            if (s.have && tracedVoices && trackInput)
            {
                vs.key = s.key;
                vs.gate = s.offT < 0.0 ? 1.0f : 0.0f;
                vs.onAge = (float)(t - s.onT);
                vs.offAge = s.offT < 0.0 ? -1.0f : (float)(t - s.offT);
            }
        }
        for (int v = 0; v < kPlayed; ++v)
        {
            const Played &p = played[(size_t)v];
            VoiceState &vs = voices[(size_t)nv++];
            vs = VoiceState{};
            if (!p.used)
                continue;
            vs.key = p.key;
            const bool held = p.held || T0 < p.off;
            vs.gate = held ? 1.0f : 0.0f;
            vs.onAge = (float)((double)(T0 - p.on) / sr);
            vs.offAge = held ? -1.0f : (float)((double)(T0 - p.off) / sr);
        }

        updateConditioning();
        if (synthOn)
            synth.renderFrame(voices.data(), nv, cond.data(), outBuf.data());
        else
            std::fill(outBuf.begin(), outBuf.end(), 0.0f);

        // now this frame's input: estimator (+ tracker, events) and timbre encoder
        for (auto &s : trk)
            s.startedAny = false;
        for (int o = 0; o < H; o += estHop)
        {
            const float *x = inBuf.data() + o;
            if (trackInput)
            {
                est.push(x);
                const float *sl = est.salienceLogits(), *ol = est.onsetLogits();
                for (int b = 0; b < est.numBins(); ++b)
                {
                    salP[(size_t)b] = 1.0f / (1.0f + std::exp(-sl[b]));
                    onsP[(size_t)b] = 1.0f / (1.0f + std::exp(-ol[b]));
                }
                tracker.step(salP.data(), onsP.data());
                const int offset = blockEnd - H + o + estHop; // the estimator frame ends here
                for (int v = 0; v < kTracked; ++v)
                {
                    Tracked &s = trk[(size_t)v];
                    const bool st = tracker.voiceStarted(v), g = tracker.voiceGate(v);
                    s.startedAny = s.startedAny || st;
                    s.lastGate = g;
                    s.lastMidi = (float)fold(tracker.voiceMidi(v));
                    report(s, v, st, g, offset);
                }
            }
            if (follow)
                encodeTimbre(x);
        }
        ++frame;
    }

    double fold(double k) const noexcept
    {
        while (k > 0.0 && k < (double)foldLo)
            k += 12.0;
        while (k >= (double)foldHi)
            k -= 12.0;
        return k;
    }

    /** note events for the organ: a voice that starts, moves to another key, or stops */
    void report(Tracked &s, int v, bool started, bool gate, int offset) noexcept
    {
        auto push = [&](bool on, float key)
        {
            if (nEvents < kMaxEvents)
                events[(size_t)nEvents++] = {std::max(0, offset - 1), v, key, on};
        };
        const float k = s.lastMidi;
        if (s.noteOn && (!gate || started || std::fabs(k - s.noteKey) > 0.6f))
        {
            push(false, s.noteKey);
            s.noteOn = false;
        }
        if (gate && !s.noteOn)
        {
            push(true, k);
            s.noteOn = true;
            s.noteKey = k;
        }
    }

    void applyQueued(int64_t callStart, int64_t T0) noexcept
    {
        const int sr = synth.sampleRate();
        int keep = 0;
        for (int q = 0; q < nQueued; ++q)
        {
            const Queued e = queued[(size_t)q];
            const int64_t at = callStart + e.time;
            if (at > T0)
            {
                queued[(size_t)keep++] = e;
                continue;
            }
            if (e.on)
            {
                const double onS = (double)at / sr;
                int v = -1;
                for (int i = 0; i < kPlayed; ++i)
                    if (!played[(size_t)i].held && played[(size_t)i].freeAt <= onS)
                    {
                        v = i;
                        break;
                    }
                if (v < 0)
                {
                    double best = std::numeric_limits<double>::infinity();
                    for (int i = 0; i < kPlayed; ++i)
                    {
                        const double f = played[(size_t)i].held ? 1e300 : played[(size_t)i].freeAt;
                        if (f < best || v < 0)
                        {
                            best = f;
                            v = i;
                        }
                    }
                }
                Played &p = played[(size_t)v];
                p.id = e.id;
                p.held = true;
                p.used = true;
                p.key = e.key;
                p.on = at;
                p.off = std::numeric_limits<int64_t>::max();
                p.freeAt = 1e300;
            }
            else
            {
                for (auto &p : played)
                    if (p.held && p.id == e.id)
                    {
                        p.held = false;
                        p.off = at;
                        p.freeAt = (double)at / sr + synth.releaseHoldSeconds();
                    }
            }
        }
        nQueued = keep;
    }

    void updateConditioning() noexcept
    {
        if (useRaw)
        {
            std::copy(raw.begin(), raw.end(), cond.begin());
            return;
        }
        for (int j = 0; j < zDim; ++j)
            target[(size_t)j] = follow ? followed[(size_t)j] : zManual[(size_t)j];
        for (int i = 0; i < zDim; ++i)
        {
            const float k = knobs[(size_t)i];
            if (k == 0.0f)
                continue;
            for (int j = 0; j < zDim; ++j)
                target[(size_t)j] += k * zBasis[(size_t)i * zDim + j];
        }
        for (int i = 0; i < nDesc; ++i)
            target[(size_t)(zDim + i)] =
                descKnobs[(size_t)i] + (follow ? followed[(size_t)(zDim + i)] : dManual[(size_t)i]);
        const float frames = smoothingMs * 0.001f * (float)synth.sampleRate() / (float)synth.hopSize();
        const float a = condPrimed && frames > 1e-3f ? 1.0f - std::exp(-1.0f / frames) : 1.0f;
        for (int j = 0; j < condDim; ++j)
            cond[(size_t)j] += a * (target[(size_t)j] - cond[(size_t)j]);
        condPrimed = true;
    }

    /** one trunk frame (encHop samples) into the slow timbre encoder */
    void encodeTimbre(const float *in) noexcept
    {
        float *x = sA.data(), *y = sB.data(), *tmp = sC.data();
        std::copy(in, in + encHop, x);
        int T = encHop;
        for (size_t i = 0; i < encConv.size(); ++i)
        {
            if (i > 0)
                leaky(x, x, T * encConv[i].cin);
            encConv[i].process(x, T, y);
            T /= encStride[i];
            const int nn = T * encConv[i].cout;
            leaky(y, tmp, nn);
            encRes[2 * i].process(tmp, T, x);
            leaky(x, x, nn);
            encRes[2 * i + 1].process(x, T, tmp);
            for (int j = 0; j < nn; ++j)
                y[j] += tmp[j];
            std::swap(x, y);
        }
        leaky(x, ah.data(), trunkCh);
        const float *lv = ah.data();
        for (size_t level = 0; level < slowConv.size(); ++level)
        {
            Conv1d &c = slowConv[level];
            std::copy(lv, lv + c.cin, slowStage[level].begin() + (size_t)slowCount[level] * c.cin);
            if (++slowCount[level] < slowStride[level])
                return;
            slowCount[level] = 0;
            float *o = slowTmp[level].data();
            c.process(slowStage[level].data(), slowStride[level], o);
            leaky(o, o, c.cout);
            lv = o;
        }
        slowGru.step(lv);
        slowHead.apply(slowGru.h.data(), slowOut.data());
        for (int i = 0; i < zDim; ++i)
            followed[(size_t)i] = slowOut[(size_t)i];
        for (int i = 0; i < nDesc; ++i)
            followed[(size_t)(zDim + i)] = slowOut[(size_t)(2 * zDim + i)];
    }

    bool loaded = false;
    PitchEstimator est;
    VoiceTracker tracker;
    OrganSynth synth;
    int estHop = 64, encHop = 64, zDim = 8, nDesc = 3, condDim = 11, trunkCh = 0;

    std::vector<Conv1d> encConv, encRes, slowConv;
    std::vector<int> encStride, slowStride, slowCount;
    std::vector<std::vector<float>> slowStage, slowTmp;
    GRU slowGru;
    Linear slowHead;
    std::vector<float> ah, slowOut, followed, sA, sB, sC;

    std::vector<float> zManual, dManual, zBasis, knobs, descKnobs, cond, target, raw;
    bool condPrimed = false, useRaw = false;

    std::vector<float> inBuf, outBuf, salP, onsP;
    int fill = 0, blockEnd = 0;
    int64_t frame = 0;
    std::vector<VoiceState> voices;
    std::array<Tracked, kTracked> trk{};
    std::array<Played, kPlayed> played{};
    std::array<Queued, kMaxQueued> queued{};
    int nQueued = 0;
    std::array<NoteEvent, kMaxEvents> events{};
    int nEvents = 0;
};
} // namespace vox
