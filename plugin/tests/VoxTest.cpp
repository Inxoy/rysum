// Vox humana: the C++ engine against the PyTorch reference (NeuralRysum scripts/make_v2_reference.py).
//
//   VoxTest <model.nrv2> <reference.nrv2>
//
// Checks the estimator logits, the tracker (on the reference salience), the synth in Manual,
// Follow and MIDI (noise off, so the result is exact up to float rounding), arbitrary block sizes,
// and measures the CPU time.
#include "vox/VoxEngine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace vox;

static int failures = 0;

static void check(bool ok, const char *what, double value, double limit)
{
    std::printf("%-46s %12.3g  (limit %.3g)  %s\n", what, value, limit, ok ? "ok" : "FAIL");
    if (!ok)
        ++failures;
}

static std::vector<char> readFile(const char *path)
{
    std::FILE *f = std::fopen(path, "rb");
    if (f == nullptr)
    {
        std::printf("cannot open %s\n", path);
        std::exit(2);
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> b((size_t)n);
    if (std::fread(b.data(), 1, (size_t)n, f) != (size_t)n)
        std::exit(2);
    std::fclose(f);
    return b;
}

/** run the engine over x in blocks of varying size; returns the output aligned to the input
    (the engine's one-frame latency removed) */
static std::vector<float> run(Engine &e, const std::vector<float> &x, const std::vector<float> &midi = {})
{
    const int lat = e.latencySamples();
    std::vector<float> y(x.size() + (size_t)lat, 0.0f), in(x.size() + (size_t)lat, 0.0f);
    std::copy(x.begin(), x.end(), in.begin());
    static const int sizes[]{100, 37, 512, 128, 1, 999, 64, 256};
    size_t pos = 0;
    int si = 0;
    // MIDI notes: rows of (on s, off s, key)
    struct Ev
    {
        int64_t t;
        int id;
        float key;
        bool on;
    };
    std::vector<Ev> evs;
    for (size_t r = 0; r + 2 < midi.size(); r += 3)
    {
        evs.push_back({(int64_t)std::llround(midi[r] * 48000.0), (int)(r / 3), midi[r + 2], true});
        evs.push_back({(int64_t)std::llround(midi[r + 1] * 48000.0), (int)(r / 3), 0.0f, false});
    }
    while (pos < in.size())
    {
        const int n = (int)std::min<size_t>((size_t)sizes[si++ % 8], in.size() - pos);
        for (const auto &ev : evs)
            if (ev.t >= (int64_t)pos && ev.t < (int64_t)(pos + (size_t)n))
            {
                if (ev.on)
                    e.noteOn((int)(ev.t - (int64_t)pos), ev.id, ev.key);
                else
                    e.noteOff((int)(ev.t - (int64_t)pos), ev.id);
            }
        e.process(in.data() + pos, y.data() + pos, n);
        pos += (size_t)n;
    }
    return std::vector<float>(y.begin() + lat, y.begin() + lat + (std::ptrdiff_t)x.size());
}

static double maxAbs(const std::vector<float> &a, const std::vector<float> &b, size_t n)
{
    double m = 0.0;
    for (size_t i = 0; i < n; ++i)
        m = std::max(m, (double)std::fabs(a[i] - b[i]));
    return m;
}

static double peak(const std::vector<float> &a)
{
    double m = 0.0;
    for (float v : a)
        m = std::max(m, (double)std::fabs(v));
    return m;
}

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // (a crash still shows how far it got)
    if (argc < 3)
    {
        std::printf("usage: VoxTest model.nrv2 reference.nrv2\n");
        return 2;
    }
    const auto modelBytes = readFile(argv[1]);
    const auto refBytes = readFile(argv[2]);
    std::string err;
    ModelFile ref;
    if (!ref.loadFromMemory(refBytes.data(), refBytes.size(), err))
    {
        std::printf("reference: %s\n", err.c_str());
        return 2;
    }
    ModelFile model;
    if (!model.loadFromMemory(modelBytes.data(), modelBytes.size(), err))
    {
        std::printf("model: %s\n", err.c_str());
        return 2;
    }
    const auto &x = ref.t("input").data;
    const int estFrames = ref.num("est_frames");

    // 1. estimator logits
    {
        PitchEstimator est;
        est.load(model);
        const auto &sal = ref.t("sal_logit");
        const auto &ons = ref.t("ons_logit");
        const int nb = est.numBins();
        double es = 0.0, eo = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < estFrames; ++f)
        {
            est.push(x.data() + (size_t)f * est.hopSize());
            for (int b = 0; b < nb; ++b)
            {
                es = std::max(es, (double)std::fabs(est.salienceLogits()[b] - sal.data[(size_t)b * estFrames + f]));
                eo = std::max(eo, (double)std::fabs(est.onsetLogits()[b] - ons.data[(size_t)b * estFrames + f]));
            }
        }
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check(es < 2e-3, "estimator: salience logits, max error", es, 2e-3);
        check(eo < 2e-3, "estimator: onset logits, max error", eo, 2e-3);
        std::printf("   estimator CPU: %.1f %% of real time (one thread)\n", 100.0 * sec / (x.size() / 48000.0));
    }

    // 2. tracker on the reference salience
    {
        VoiceTracker tr;
        tr.init(262, 21.0, 3, 8);
        const auto &sal = ref.t("sal_logit");
        const auto &ons = ref.t("ons_logit");
        const auto &rm = ref.t("trk_midi").data, &rg = ref.t("trk_gate").data, &rs = ref.t("trk_started").data;
        std::vector<float> s(262), o(262);
        int gateDiff = 0, startDiff = 0;
        double midiErr = 0.0;
        for (int f = 0; f < estFrames; ++f)
        {
            for (int b = 0; b < 262; ++b)
            {
                s[(size_t)b] = 1.0f / (1.0f + std::exp(-sal.data[(size_t)b * estFrames + f]));
                o[(size_t)b] = 1.0f / (1.0f + std::exp(-ons.data[(size_t)b * estFrames + f]));
            }
            tr.step(s.data(), o.data());
            for (int v = 0; v < 8; ++v)
            {
                const size_t i = (size_t)v * estFrames + f;
                gateDiff += (tr.voiceGate(v) ? 1.0f : 0.0f) != rg[i];
                startDiff += (tr.voiceStarted(v) ? 1.0f : 0.0f) != rs[i];
                if (rg[i] > 0)
                    midiErr = std::max(midiErr, std::fabs(tr.voiceMidi(v) - (double)rm[i]));
            }
        }
        check(gateDiff == 0, "tracker: gate frames that differ", gateDiff, 0);
        check(startDiff == 0, "tracker: note starts that differ", startDiff, 0);
        check(midiErr < 1e-3, "tracker: pitch error (semitones)", midiErr, 1e-3);
    }

    const double refPeak = peak(ref.t("manual_dry").data);
    // tracked paths: C++ and PyTorch logits differ by ~1e-3, so tracked pitches differ by ~1e-5 semitones,
    // which shifts the oscillator phase by a few tenths of a percent over seconds
    const double tol = 1e-2 * refPeak + 1e-5;

    // 3. whole engine, Manual (tracked voices only), dry and with the learned room
    {
        Engine e;
        if (!e.load(modelBytes.data(), modelBytes.size(), err))
        {
            std::printf("engine: %s\n", err.c_str());
            return 2;
        }
        e.setNoise(false);
        e.setRoomMix(0.0f);
        e.setBody(false); // the network's raw output, as the reference
        e.smoothingMs = 0.0f;
        const auto t0 = std::chrono::steady_clock::now();
        const auto y = run(e, x);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check(maxAbs(y, ref.t("manual_dry").data, x.size()) < tol, "engine Manual dry: max error", maxAbs(y, ref.t("manual_dry").data, x.size()), tol);
        std::printf("   reference peak %.4f, C++ peak %.4f; engine CPU %.1f %% of real time\n", refPeak, peak(y),
                    100.0 * sec / (x.size() / 48000.0));
        e.reset();
        e.setRoomMix(1.0f);
        const auto yw = run(e, x);
        const double tw = 1e-2 * peak(ref.t("manual_wet").data) + 1e-5;
        check(maxAbs(yw, ref.t("manual_wet").data, x.size()) < tw, "engine Manual + learned room: max error",
              maxAbs(yw, ref.t("manual_wet").data, x.size()), tw);
    }

    // 4. Follow
    {
        Engine e;
        e.load(modelBytes.data(), modelBytes.size(), err);
        e.setNoise(false);
        e.setRoomMix(0.0f);
        e.setBody(false);
        e.smoothingMs = 0.0f;
        e.follow = true;
        const auto y = run(e, x);
        const auto &r = ref.t("follow_dry").data;
        const double tf = 1e-2 * std::max(peak(r), refPeak * 0.1) + 1e-5;
        check(maxAbs(y, r, x.size()) < tf, "engine Follow dry: max error", maxAbs(y, r, x.size()), tf);
    }

    // 5. MIDI only
    {
        Engine e;
        e.load(modelBytes.data(), modelBytes.size(), err);
        e.setNoise(false);
        e.setRoomMix(0.0f);
        e.setBody(false);
        e.smoothingMs = 0.0f;
        e.trackInput = false;
        const auto &r = ref.t("midi_dry").data;
        std::vector<float> silence(r.size(), 0.0f);
        const auto y = run(e, silence, ref.t("midi_notes").data);
        const double tm = 1e-3 * peak(r) + 1e-5;
        check(maxAbs(y, r, r.size()) < tm, "engine MIDI dry: max error", maxAbs(y, r, r.size()), tm);
    }

    // 5b. the eight pipes and the room's tail add up to the whole
    {
        Engine e;
        e.load(modelBytes.data(), modelBytes.size(), err);
        for (int k = 0; k < 6; ++k)
            e.noteOn(0, k, 48.0f + 4.0f * k);
        const int n = 48000;
        std::vector<float> in((size_t)n), out((size_t)n);
        for (int i = 0; i < n; ++i)
            in[(size_t)i] = x[(size_t)i % x.size()];
        std::array<std::vector<float>, Engine::kPipeOutputs> ch;
        std::array<float *, Engine::kPipeOutputs> ptr{};
        double err2 = 0.0, pk = 0.0;
        int pipesSounding = 0;
        for (int c = 0; c < Engine::kPipeOutputs; ++c)
        {
            ch[(size_t)c].assign((size_t)n, 0.0f);
            ptr[(size_t)c] = ch[(size_t)c].data();
        }
        e.process(in.data(), out.data(), n, ptr.data());
        for (int i = 0; i < n; ++i)
        {
            float s = 0.0f;
            for (int c = 0; c < Engine::kPipeOutputs; ++c)
                s += ch[(size_t)c][(size_t)i];
            err2 = std::max(err2, (double)std::fabs(s - out[(size_t)i]));
            pk = std::max(pk, (double)std::fabs(out[(size_t)i]));
        }
        for (int c = 0; c < 8; ++c)
        {
            double m = 0.0;
            for (float v : ch[(size_t)c])
                m = std::max(m, (double)std::fabs(v));
            pipesSounding += m > 1e-4 ? 1 : 0;
        }
        check(err2 < 1e-5 * pk + 1e-7, "pipes + tail = the whole: max error", err2, 1e-5 * pk + 1e-7);
        check(pipesSounding >= 6, "pipes that sound (8 played + tracked voices)", pipesSounding, 6);
    }

    // 5c. without the learned room ("eigener Raum" 0) the pipe keeps its tone and level, only the
    //     reverberation goes: a chord held 1.5 s, then released
    {
        auto render = [&](float room)
        {
            Engine e;
            e.load(modelBytes.data(), modelBytes.size(), err);
            e.trackInput = false;
            e.setRoomMix(room);
            const int n = 48000 * 3, offAt = 48000 * 3 / 2;
            std::vector<float> out((size_t)n);
            for (int k = 0; k < 3; ++k)
                e.noteOn(0, k, 48.0f + 4.0f * k);
            for (int p = 0; p < n; p += 256)
            {
                if (p == offAt / 256 * 256)
                    for (int k = 0; k < 3; ++k)
                        e.noteOff(0, k);
                e.process(nullptr, out.data() + p, std::min(256, n - p));
            }
            return out;
        };
        auto rms = [](const std::vector<float> &v, int a, int b, float lpA)
        {
            double s = 0.0, z = 0.0;
            for (int i = 0; i < b; ++i)
            {
                z += lpA * (v[(size_t)i] - z);
                const double y = lpA < 1.0f ? z : v[(size_t)i];
                if (i >= a)
                    s += y * y;
            }
            return std::sqrt(s / (double)(b - a));
        };
        const auto dry = render(0.0f), wet = render(1.0f);
        const int a = 24000, b = 72000; // 0.5 .. 1.5 s: held
        const float lp = 1.0f - std::exp(-6.2831853f * 300.0f / 48000.0f);
        const double lvl = 20.0 * std::log10(rms(dry, a, b, 1.0f) / rms(wet, a, b, 1.0f));
        const double bass = 20.0 * std::log10((rms(dry, a, b, lp) / rms(dry, a, b, 1.0f)) /
                                              (rms(wet, a, b, lp) / rms(wet, a, b, 1.0f)));
        const double tailDry = rms(dry, 96000, 120000, 1.0f) / rms(dry, a, b, 1.0f);
        const double tailWet = rms(wet, 96000, 120000, 1.0f) / rms(wet, a, b, 1.0f);
        const double tailDb = 20.0 * std::log10(tailWet / std::max(tailDry, 1e-12));
        check(std::fabs(lvl) < 3.0, "room 0: level against room 1 (dB)", lvl, 3.0);
        check(std::fabs(bass) < 3.0, "room 0: share below 300 Hz against room 1 (dB)", bass, 3.0);
        check(tailDb > 15.0, "room 0: 0.5-1 s after release, quieter than with the room by (dB)", tailDb, 15.0);
    }

    // 6. CPU: 8 played voices only (keyboard, no tracking, no learned room)
    for (int noise = 0; noise < 2; ++noise)
    {
        Engine e;
        e.load(modelBytes.data(), modelBytes.size(), err);
        e.trackInput = false;
        e.setRoomMix(0.0f);
        e.setNoise(noise != 0);
        for (int k = 0; k < 8; ++k)
            e.noteOn(0, k, 48.0f + 3.0f * k);
        std::vector<float> out(48000 * 4);
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t p = 0; p < out.size(); p += 256)
            e.process(nullptr, out.data() + p, 256);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("   8 played voices, noise %s, no room: %.1f %% of real time\n", noise ? "on" : "off",
                    100.0 * sec / 4.0);
    }

    // 7. CPU with noise and room on, 8 tracked + 8 played voices held
    {
        Engine e;
        e.load(modelBytes.data(), modelBytes.size(), err);
        for (int k = 0; k < 8; ++k)
            e.noteOn(0, k, 48.0f + 3.0f * k);
        std::vector<float> in(48000 * 4, 0.0f), out(in.size());
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = x[i % x.size()];
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t p = 0; p < in.size(); p += 256)
            e.process(in.data() + p, out.data() + p, 256);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("   full load (tracking, 16 voices, noise, room): %.1f %% of real time\n", 100.0 * sec / 4.0);
        bool finite = true;
        for (float v : out)
            finite = finite && std::isfinite(v);
        check(finite && peak(out) > 1e-4, "full load: output finite and not silent", peak(out), 1e-4);
    }

    std::printf(failures == 0 ? "all vox checks passed\n" : "%d vox checks FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
