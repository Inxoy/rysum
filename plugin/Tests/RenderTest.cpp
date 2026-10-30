// Offline test: loads the sample folder, renders a few MIDI scenes through the engine
// in stereo and in AmbiX 5th order, writes WAV files and checks the results.
//   RysumRenderTest <samples folder> <output folder>
#include "OrganEngine.h"
#include "Convolver.h"
#include "Binaural.h"
#include "RoomSim.h"
#include "RoomTail.h"
#include "Scala.h"
#include "Lucier.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <chrono>
#include <complex>
#include <iostream>

using namespace rysum;

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
    if (! ok) ++failures;
}

static void testEncoder()
{
    std::cout << "encoder" << std::endl;
    float g[kMaxAmbiChannels];
    encodeAmbiX (90.0f, 0.0f, 1, g);      // left
    check (std::abs (g[0] - 1) < 1e-5 && std::abs (g[1] - 1) < 1e-5 && std::abs (g[2]) < 1e-5 && std::abs (g[3]) < 1e-5, "order 1, left: W=1 Y=1 Z=0 X=0");
    encodeAmbiX (0.0f, 90.0f, 1, g);      // up
    check (std::abs (g[2] - 1) < 1e-5 && std::abs (g[1]) < 1e-5, "order 1, zenith: Z=1");
    encodeAmbiX (30.0f, 20.0f, 2, g);
    const double az = juce::degreesToRadians (30.0), el = juce::degreesToRadians (20.0);
    check (std::abs (g[4] - std::sqrt (3.0) / 2 * std::cos (el) * std::cos (el) * std::sin (2 * az)) < 1e-5, "ACN4 = sqrt(3)/2 cos^2(el) sin(2az)");
    check (std::abs (g[6] - 0.5 * (3 * std::sin (el) * std::sin (el) - 1)) < 1e-5, "ACN6 = (3 sin^2 el - 1)/2");

    // SN3D orthogonality: mean over the sphere of Y_i*Y_j = delta_ij / (2l+1)
    const int N = 200;
    double M[36][36] {};
    double wsum = 0;
    for (int a = 0; a < N; ++a)
        for (int b = 0; b < N; ++b)
        {
            const float azd = -180.0f + 360.0f * (a + 0.5f) / N;
            const double u = -1.0 + 2.0 * (b + 0.5) / N;                     // sin(el) uniform -> equal area
            const float eld = (float) juce::radiansToDegrees (std::asin (u));
            encodeAmbiX (azd, eld, 5, g);
            for (int i = 0; i < 36; ++i) for (int j = 0; j < 36; ++j) M[i][j] += (double) g[i] * g[j];
            wsum += 1;
        }
    double maxErr = 0;
    for (int i = 0; i < 36; ++i)
        for (int j = 0; j < 36; ++j)
        {
            const int l = (int) std::floor (std::sqrt ((double) i));
            const double expect = i == j ? 1.0 / (2 * l + 1) : 0.0;
            maxErr = std::max (maxErr, std::abs (M[i][j] / wsum - expect));
        }
    check (maxErr < 2e-3, "SN3D orthogonality up to order 5 (max err " + juce::String (maxErr, 5) + ")");

    float s[2];
    encodeStereo (90.0f, s);  check (s[0] > 0.999f && s[1] < 0.001f, "stereo: +90 = left");
    encodeStereo (0.0f, s);   check (std::abs (s[0] - s[1]) < 1e-5 && std::abs (s[0] * s[0] + s[1] * s[1] - 1) < 1e-5, "stereo: centre equal power");
}

static void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double sr);

// ---------------------------------------------------------------------------
// binaural: HRIR sets, MagLS filters, real-time decoder
// ---------------------------------------------------------------------------
static double bandDb (const std::vector<float>& x, double fs, double f0, double f1)
{
    const int n = 4096;
    juce::dsp::FFT fft (12);
    std::vector<float> b ((size_t) (2 * n), 0.0f);
    for (int i = 0; i < juce::jmin ((int) x.size(), n); ++i) b[(size_t) i] = x[(size_t) i];
    fft.performFrequencyOnlyForwardTransform (b.data(), true);
    double e = 0; int cnt = 0;
    for (int k = 1; k < n / 2; ++k) { const double f = k * fs / n; if (f >= f0 && f < f1) { e += (double) b[(size_t) k] * b[(size_t) k]; ++cnt; } }
    return 10.0 * std::log10 (juce::jmax (1e-30, e / juce::jmax (1, cnt)));
}

static double itdMs (const std::vector<float>& l, const std::vector<float>& r, double fs)
{
    // interaural phase delay, averaged over 200..500 Hz (no phase ambiguity for |ITD| < 1 ms)
    double sum = 0; int cnt = 0;
    for (double f = 200; f <= 500; f += 25)
    {
        std::complex<double> L, R;
        for (size_t i = 0; i < l.size(); ++i) L += (double) l[i] * std::polar (1.0, -2.0 * juce::MathConstants<double>::pi * f * (double) i / fs);
        for (size_t i = 0; i < r.size(); ++i) R += (double) r[i] * std::polar (1.0, -2.0 * juce::MathConstants<double>::pi * f * (double) i / fs);
        sum += std::arg (R / L) / (2.0 * juce::MathConstants<double>::pi * f);
        ++cnt;
    }
    return 1000.0 * sum / cnt;
}

[[maybe_unused]] static double itdXcorrMs (const std::vector<float>& l, const std::vector<float>& r, double fs)
{
    // cross-correlation of the low-passed ears (< 700 Hz: no ambiguity within +-1 ms); positive = left ear first
    auto lp = [&] (std::vector<float> x) { juce::IIRFilter f; f.setCoefficients (juce::IIRCoefficients::makeLowPass (fs, 700.0)); f.processSamples (x.data(), (int) x.size()); return x; };
    const auto a = lp (l), b = lp (r);
    const int maxLag = (int) (0.0012 * fs);
    int best = 0; double bv = -1e30;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double s = 0;
        for (int i = 0; i < (int) a.size(); ++i) { const int j = i + lag; if (j >= 0 && j < (int) b.size()) s += (double) a[(size_t) i] * b[(size_t) j]; }
        if (s > bv) { bv = s; best = lag; }
    }
    return 1000.0 * best / fs;
}

static void testBinaural (const juce::File& sofa)
{
    juce::String err;
    auto fab = HrirSet::fabian (err);
    check (fab != nullptr && fab->numDirections() > 1900, "FABIAN set built in (" + juce::String (fab != nullptr ? fab->numDirections() : 0) + " directions) " + err);
    if (fab == nullptr) return;

    std::unique_ptr<HrirSet> full;
    if (sofa.existsAsFile())
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        full = HrirSet::fromSofa (sofa, err);
        const double secs = std::chrono::duration<double> (std::chrono::high_resolution_clock::now() - t0).count();
        check (full != nullptr && full->numDirections() == 11950 && full->length == 279, "SOFA read with libmysofa (" + juce::String (secs, 2) + " s) " + err);
        if (full != nullptr)
        {
            // the same direction in both: identical data (up to the 16-bit storage)
            int a = 0; double bestA = 1e9;
            for (int q = 0; q < fab->numDirections(); ++q)
            {
                const double dd = std::abs (fab->az[(size_t) q] - 90.0f) + std::abs (fab->el[(size_t) q]);
                if (dd < bestA) { bestA = dd; a = q; }
            }
            std::cout << "comparing direction " << fab->az[(size_t) a] << " / " << fab->el[(size_t) a] << std::endl;
            int b = -1; for (int q = 0; q < full->numDirections(); ++q) if (std::abs (full->az[(size_t) q] - fab->az[(size_t) a]) < 0.01f && std::abs (full->el[(size_t) q] - fab->el[(size_t) a]) < 0.01f) b = q;
            double d = 0, e = 0;
            if (b >= 0) for (int i = 0; i < 240; ++i) { const double x = full->get (b, 0)[i], y = fab->get (a, 0)[i]; d += (x - y) * (x - y); e += x * x; }
            check (b >= 0 && d / e < 1e-6, "built-in FABIAN = SOFA file");
            // left source: left ear louder (coordinate convention of the SOFA reader)
            double el = 0, er = 0; for (int i = 0; i < full->length; ++i) { el += full->get (b, 0)[i] * full->get (b, 0)[i]; er += full->get (b, 1)[i] * full->get (b, 1)[i]; }
            check (b >= 0 && el > 4 * er, "SOFA: +90 deg is on the left");
        }
    }
    {   // broken file
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rysum_bad.sofa");
        tmp.replaceWithText ("not a sofa file");
        auto bad = HrirSet::fromSofa (tmp, err);
        check (bad == nullptr && err.isNotEmpty(), "broken SOFA rejected: " + err);
        tmp.deleteFile();
    }

    const auto& ref = full != nullptr ? *full : *fab;
    auto nearest = [&] (float az, float el)
    {
        int best = 0; double bd = 1e9;
        for (int q = 0; q < ref.numDirections(); ++q)
        {
            const double d = std::abs (wrapAz (ref.az[(size_t) q] - az)) * std::cos (juce::degreesToRadians ((double) el)) + std::abs (ref.el[(size_t) q] - el);
            if (d < bd) { bd = d; best = q; }
        }
        return best;
    };
    auto render = [] (const BinauralFilters& f, float az, float el, int ear)
    {
        float g[kMaxAmbiChannels] {};
        encodeAmbiX (az, el, f.order, g);
        std::vector<float> y ((size_t) f.length, 0.0f);
        for (int c = 0; c < f.numChannels; ++c)
            for (int i = 0; i < f.length; ++i) y[(size_t) i] += g[c] * f.get (ear, c)[i];
        return y;
    };
    const float dirsTest[][2] { { 0, 0 }, { 30, 0 }, { 90, 0 }, { 135, 0 }, { -60, 0 }, { 180, 0 }, { 45, 40 }, { -120, -30 }, { 0, 70 } };
    const double bands[] { 250, 500, 1000, 2000, 4000, 8000, 16000 };

    for (int order : { 1, 3, 5 })
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        auto f = BinauralFilters::design (*fab, order, 48000.0, false);
        const double secs = std::chrono::duration<double> (std::chrono::high_resolution_clock::now() - t0).count();
        check (f != nullptr, "MagLS design order " + juce::String (order) + " (" + juce::String (secs, 2) + " s)");
        if (f == nullptr) continue;
        // compare with the measured HRIRs: band levels (after removing the overall gain) and ITD
        std::vector<double> diffs;
        double worstItd = 0, worstIld = 0;
        for (auto& d : dirsTest)
        {
            const int q = nearest (d[0], d[1]);
            std::vector<float> hl (ref.get (q, 0), ref.get (q, 0) + ref.length), hr (ref.get (q, 1), ref.get (q, 1) + ref.length);
            const auto yl = render (*f, ref.az[(size_t) q], ref.el[(size_t) q], 0), yr = render (*f, ref.az[(size_t) q], ref.el[(size_t) q], 1);
            for (int b = 0; b + 1 < 7; ++b)
            {
                diffs.push_back (bandDb (yl, 48000, bands[b], bands[b + 1]) - bandDb (hl, 48000, bands[b], bands[b + 1]));
                diffs.push_back (bandDb (yr, 48000, bands[b], bands[b + 1]) - bandDb (hr, 48000, bands[b], bands[b + 1]));
            }
            worstItd = juce::jmax (worstItd, std::abs (itdMs (yl, yr, 48000) - itdMs (hl, hr, 48000)));
            const double ildH = bandDb (hl, 48000, 2000, 12000) - bandDb (hr, 48000, 2000, 12000);
            const double ildY = bandDb (yl, 48000, 2000, 12000) - bandDb (yr, 48000, 2000, 12000);
            worstIld = juce::jmax (worstIld, std::abs (ildH - ildY));
        }
        double mean = 0; for (auto x : diffs) mean += x; mean /= (double) diffs.size();
        double rms = 0; for (auto x : diffs) rms += (x - mean) * (x - mean); rms = std::sqrt (rms / (double) diffs.size());
        std::cout << "binaural order " << order << ": band error " << rms << " dB rms (gain " << mean << " dB), worst ITD error "
                  << worstItd * 1000.0 << " us, worst ILD error " << worstIld << " dB" << std::endl;
        if (order == 5) check (rms < 1.5 && worstItd < 0.03 && worstIld < 5.0, "order 5 matches the measured HRTFs");
        if (order == 3) check (rms < 1.5 && worstItd < 0.04, "order 3 close to the measured HRTFs");
        if (order == 1) check (rms < 3.5 && worstItd < 0.1, "order 1: ITDs right");
    }

    // real-time decoder = direct convolution, 128 samples later; head rotation
    {
        auto f = BinauralFilters::design (*fab, 3, 48000.0, true);
        check (f != nullptr, "design with diffuse-field EQ");
        BinauralDecoder dec (*f);
        const int n = 4096, K = f->numChannels;
        std::vector<std::vector<float>> in ((size_t) K, std::vector<float> ((size_t) n, 0.0f));
        float g[kMaxAmbiChannels] {};
        encodeAmbiX (60.0f, 10.0f, 3, g);
        juce::Random rnd (3);
        std::vector<float> src ((size_t) n);
        for (auto& x : src) x = rnd.nextFloat() * 2.0f - 1.0f;
        for (int c = 0; c < K; ++c) for (int i = 0; i < n; ++i) in[(size_t) c][(size_t) i] = src[(size_t) i] * g[c];
        std::vector<float> L ((size_t) n), R ((size_t) n);
        std::vector<float*> ptr; for (auto& v : in) ptr.push_back (v.data());
        for (int start = 0; start < n; start += 100)       // odd block size
        {
            const int m = juce::jmin (100, n - start);
            std::vector<float*> p2; for (auto* q : ptr) p2.push_back (q + start);
            dec.process (p2.data(), m, L.data() + start, R.data() + start, 0.0f);
        }
        double err = 0, en = 0;
        for (int i = 1000; i < n; ++i)
        {
            double y = 0;
            for (int c = 0; c < K; ++c)
                for (int j = 0; j < f->length; ++j) { const int s = i - BinauralDecoder::kBlock - j; if (s >= 0) y += (double) f->get (0, c)[j] * src[(size_t) s] * g[c]; }
            err += (y - L[(size_t) i]) * (y - L[(size_t) i]); en += y * y;
        }
        std::cout << "binaural decoder vs direct convolution: " << 10 * std::log10 (err / en) << " dB" << std::endl;
        check (err / en < 1e-8, "decoder = convolution with the filters (128 samples latency)");

        // a source in front, head turned 90 deg to the left: it is now on the right
        auto turned = [&] (float yaw)
        {
            BinauralDecoder d2 (*f);
            std::vector<std::vector<float>> x ((size_t) K, std::vector<float> ((size_t) n, 0.0f));
            float gg[kMaxAmbiChannels] {};
            encodeAmbiX (0.0f, 0.0f, 3, gg);
            for (int c = 0; c < K; ++c) for (int i = 0; i < n; ++i) x[(size_t) c][(size_t) i] = src[(size_t) i] * gg[c];
            std::vector<float*> p; for (auto& v : x) p.push_back (v.data());
            std::vector<float> l ((size_t) n), r ((size_t) n);
            for (int start = 0; start < n; start += 256)
            {
                std::vector<float*> p2; for (auto* q : p) p2.push_back (q + start);
                d2.process (p2.data(), 256, l.data() + start, r.data() + start, yaw);
            }
            double el = 0, er = 0; for (int i = 1024; i < n; ++i) { el += l[(size_t) i] * l[(size_t) i]; er += r[(size_t) i] * r[(size_t) i]; }
            return 10.0 * std::log10 (el / er);
        };
        const double ild0 = turned (0.0f), ild90 = turned (90.0f);
        std::cout << "head rotation: front L/R " << ild0 << " dB, head turned left 90 deg: " << ild90 << " dB" << std::endl;
        check (std::abs (ild0) < 2.0 && ild90 < -6.0, "head rotation turns the scene");

        // level: diffuse noise (random directions) about as loud per ear as an equal-power stereo pan
        double eOut = 0, eIn = 0;
        BinauralDecoder d3 (*f);
        std::vector<std::vector<float>> x ((size_t) K, std::vector<float> ((size_t) n, 0.0f));
        for (int s = 0; s < 64; ++s)
        {
            float gg[kMaxAmbiChannels] {};
            encodeAmbiX (360.0f * rnd.nextFloat() - 180.0f, juce::radiansToDegrees (std::asin (2.0f * rnd.nextFloat() - 1.0f)), 3, gg);
            for (int i = 0; i < n; ++i) { const float v = (rnd.nextFloat() * 2.0f - 1.0f) * 0.125f; eIn += v * v; for (int c = 0; c < K; ++c) x[(size_t) c][(size_t) i] += v * gg[c]; }
        }
        std::vector<float*> p; for (auto& v : x) p.push_back (v.data());
        d3.process (p.data(), n, L.data(), R.data(), 0.0f);
        for (int i = 1024; i < n; ++i) eOut += 0.5 * ((double) L[(size_t) i] * L[(size_t) i] + (double) R[(size_t) i] * R[(size_t) i]);
        const double lvl = 10.0 * std::log10 (eOut / (eIn * (n - 1024) / n));
        std::cout << "binaural level of a diffuse field per ear: " << lvl << " dB (stereo pan: -3 dB)" << std::endl;
        check (std::abs (lvl + 3.0) < 2.5, "binaural level like stereo");
    }

    // other host rates: same sound
    {
        auto a = BinauralFilters::design (*fab, 3, 48000.0, false), b = BinauralFilters::design (*fab, 3, 44100.0, false), c = BinauralFilters::design (*fab, 3, 96000.0, false);
        check (a && b && c, "design at 44.1 / 96 kHz");
        if (a && b && c)
        {
            const auto ya = render (*a, 30, 0, 0), yb = render (*b, 30, 0, 0), yc = render (*c, 30, 0, 0);
            double worst = 0;
            for (double fr : { 300.0, 1000.0, 3000.0, 8000.0 })
            {
                const double la = bandDb (ya, 48000, fr, fr * 1.5), lb = bandDb (yb, 44100, fr, fr * 1.5), lc = bandDb (yc, 96000, fr, fr * 1.5);
                worst = juce::jmax (worst, std::abs (lb - la), std::abs (lc - la));
            }
            std::cout << "binaural 44.1 / 96 kHz vs 48 kHz: worst band difference " << worst << " dB" << std::endl;
            check (worst < 1.0, "resampled filters sound the same");
        }
    }

    // CPU: 5th order, 36 channels
    {
        auto f = BinauralFilters::design (*fab, 5, 48000.0, true);
        BinauralDecoder dec (*f);
        const int n = 512;
        std::vector<std::vector<float>> x (36, std::vector<float> ((size_t) n, 0.1f));
        std::vector<float*> p; for (auto& v : x) p.push_back (v.data());
        std::vector<float> l ((size_t) n), r ((size_t) n);
        auto t0 = std::chrono::high_resolution_clock::now();
        const int blocks = 48000 * 10 / n;
        for (int i = 0; i < blocks; ++i) dec.process (p.data(), n, l.data(), r.data(), 20.0f * (float) (i % 7));
        const double rt = 10.0 / std::chrono::duration<double> (std::chrono::high_resolution_clock::now() - t0).count();
        std::cout << "binaural decoder 5th order: " << rt << " x real time" << std::endl;
        check (rt > 8.0, "binaural decoder is cheap");
    }
}

static void testConvolver()
{
    std::cout << "convolver" << std::endl;
    const double sr = 48000.0;
    const int L = (int) (0.7 * sr);
    XorShift r (99);
    // synthetic first-order IR: direct sound from 60 deg left at sample 100, then a decaying noise tail
    juce::AudioBuffer<float> ir (4, L);
    ir.clear();
    const float az = juce::degreesToRadians (60.0f);
    ir.setSample (0, 100, 1.0f); ir.setSample (1, 100, std::sin (az)); ir.setSample (3, 100, std::cos (az));
    for (int c = 0; c < 4; ++c)
        for (int i = 300; i < L; ++i)
            ir.setSample (c, i, r.bi() * 0.25f * std::exp (-6.0f * (float) i / (float) L) * (c == 0 ? 1.0f : 0.5f));
    auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rysum_test_ir.wav");
    writeWav (f, ir, sr);

    auto run = [&] (Convolver& cv, std::function<void (int, std::vector<std::vector<float>>&)> fill, int nIn, int numOut, int N)
    {
        juce::AudioBuffer<float> out (numOut, N); out.clear();
        cv.reset();
        int pos = 0, bs = 37;
        while (pos < N)
        {
            const int n = juce::jmin (bs, N - pos);
            std::vector<std::vector<float>> ins ((size_t) nIn, std::vector<float> ((size_t) n, 0.0f));
            for (int i = 0; i < n; ++i) { std::vector<std::vector<float>> one ((size_t) nIn, std::vector<float> (1)); fill (pos + i, one); for (int k = 0; k < nIn; ++k) ins[(size_t) k][(size_t) i] = one[(size_t) k][0]; }
            std::vector<const float*> ip; for (auto& v : ins) ip.push_back (v.data());
            std::vector<float*> o; for (int c = 0; c < numOut; ++c) o.push_back (out.getWritePointer (c, pos));
            cv.process (ip.data(), n, o.data(), numOut, 1.0f);
            pos += n; bs = bs == 37 ? 501 : (bs == 501 ? 1024 : 37);
        }
        return out;
    };

    juce::String err;
    Convolver::Settings st; st.outOrder = 1; st.direct = 1.0f; st.envelop = 0.0f; st.hostRate = sr;
    auto cv = Convolver::create (f, st, err);
    check (cv != nullptr, "IR loaded " + err);
    if (! cv) return;
    std::cout << "  " << cv->getInfo().description << ", direct sound at " << cv->getInfo().baseAzDeg << " deg" << std::endl;
    check (std::abs (cv->getInfo().baseAzDeg - 60.0f) < 1.0f, "direction of the IR's direct sound found");

    // 1) exactness: all 'pipes' at the measured direction (= not following) must give plain convolution
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
    juce::AudioBuffer<float> ref (4, L); rd->read (&ref, 0, L, 0, true, true);
    double e = 0; for (int i = 0; i < L; ++i) e += (double) ref.getSample (0, i) * ref.getSample (0, i);
    ref.applyGain ((float) (1.0 / std::sqrt (e)));
    const int fade = juce::jmin (L / 4, (int) (0.05 * sr));
    for (int c = 0; c < 4; ++c) ref.applyGainRamp (c, L - fade, fade, 1.0f, 0.0f);
    const int N = 9000;
    std::vector<float> x ((size_t) N);
    for (int i = 0; i < 6000; ++i) x[(size_t) i] = r.bi();
    const float a0 = juce::degreesToRadians (cv->getInfo().baseAzDeg);
    auto out = run (*cv, [&] (int t, std::vector<std::vector<float>>& v) { v[0][0] = x[(size_t) t]; v[1][0] = std::cos (a0) * x[(size_t) t]; v[2][0] = std::sin (a0) * x[(size_t) t]; }, 3, 4, N);
    double maxErr = 0, maxRef = 0;
    for (int c = 0; c < 4; ++c)
        for (int t = Convolver::kBlock; t < N; t += 7)
        {
            double y = 0; const int tt = t - Convolver::kBlock;
            for (int k = juce::jmax (0, tt - 6000 + 1); k <= juce::jmin (tt, L - 1); ++k) y += (double) ref.getSample (c, k) * x[(size_t) (tt - k)];
            maxErr = std::max (maxErr, std::abs (y - out.getSample (c, t)));
            maxRef = std::max (maxRef, std::abs (y));
        }
    check (maxErr < 2.0e-3 * maxRef, "not following = the measured room exactly (rel err " + juce::String (maxErr / maxRef, 7) + ")");

    // 2) following: a pipe at -30 deg gets its direct sound / early reflections from -30 deg
    auto impulseAt = [&] (Convolver& c, float deg, int nIn, int numOut)
    {
        const float a = juce::degreesToRadians (deg);
        return run (c, [&] (int t, std::vector<std::vector<float>>& v)
        {
            const float s = t == 0 ? 1.0f : 0.0f;
            v[0][0] = s;
            for (int m = 1; 2 * m < nIn; ++m) { v[(size_t) (2 * m - 1)][0] = std::cos ((float) m * a) * s; v[(size_t) (2 * m)][0] = std::sin ((float) m * a) * s; }
        }, nIn, numOut, (int) (0.7 * sr));
    };
    auto o2 = impulseAt (*cv, -30.0f, 3, 4);
    const int td = Convolver::kBlock + 100;
    const float dir = juce::radiansToDegrees (std::atan2 (o2.getSample (1, td), o2.getSample (3, td)));
    std::cout << "  pipe at -30 deg -> direct sound of the room from " << dir << " deg" << std::endl;
    check (std::abs (dir + 30.0f) < 1.0f, "early reflections turn with the pipe");

    // 3) the IR's own direct sound turned down
    Convolver::Settings st2 = st; st2.direct = 0.2f;
    auto cvD = Convolver::create (f, st2, err);
    auto o3 = impulseAt (*cvD, -30.0f, 3, 4);
    check (std::abs (o3.getSample (0, td) / o2.getSample (0, td) - 0.2f) < 0.01f, "direct sound of the IR at 20 %");

    // 4) envelopment: mono IR, 3rd order output, fully diffuse tail -> energies 1/(2l+1), decorrelated from W
    juce::AudioBuffer<float> mono (1, L);
    for (int i = 0; i < L; ++i) mono.setSample (0, i, r.bi() * std::exp (-6.0f * (float) i / (float) L));
    auto fm1 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rysum_test_ir_mono.wav");
    writeWav (fm1, mono, sr);
    Convolver::Settings st3; st3.outOrder = 3; st3.envelop = 1.0f; st3.hostRate = sr;
    auto cvM = Convolver::create (fm1, st3, err);
    check (cvM != nullptr && cvM->getInfo().numInputs == 7, "mono IR lifted to 3rd order (7 sends)");
    if (cvM)
    {
        auto o4 = impulseAt (*cvM, 0.0f, 7, 16);
        const int a = Convolver::kBlock + (int) (0.15 * sr), n = (int) (0.4 * sr);
        auto en = [&] (int c) { double s = 0; for (int i = a; i < a + n; ++i) s += (double) o4.getSample (c, i) * o4.getSample (c, i); return s; };
        const double eW = en (0);
        double corr = 0; for (int i = a; i < a + n; ++i) corr += (double) o4.getSample (0, i) * o4.getSample (1, i);
        corr /= std::sqrt (eW * en (1));
        juce::String ratios;
        bool ok = true;
        for (int l = 1; l <= 3; ++l)
        {
            const double rr = en (l * l + l) / eW * (2 * l + 1);
            ratios << " l" << l << "=" << juce::String (rr, 2);
            ok = ok && std::abs (rr - 1.0) < 0.35;
        }
        std::cout << "  diffuse tail energy x(2l+1)/W:" << ratios << ", corr(W,Y) " << juce::String (corr, 3) << std::endl;
        check (ok && std::abs (corr) < 0.2, "enveloping tail: isotropic energies, decorrelated");
    }

    // 6) cost: 4 s, 5th order IR, 5th order output, following (11 sends)
    {
        juce::AudioBuffer<float> big (36, (int) (4.0 * sr));
        for (int c = 0; c < 36; ++c) for (int i = 0; i < big.getNumSamples(); ++i) big.setSample (c, i, r.bi() * std::exp (-4.0f * (float) i / (float) big.getNumSamples()) * (c == 0 ? 1.0f : 0.4f));
        auto fb = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rysum_test_ir_big.wav");
        writeWav (fb, big, sr);
        Convolver::Settings s5; s5.outOrder = 5; s5.hostRate = sr;
        auto t0 = std::chrono::high_resolution_clock::now();
        auto cvB = Convolver::create (fb, s5, err);
        auto t1 = std::chrono::high_resolution_clock::now();
        if (cvB)
        {
            const int n = 512, blocks = (int) (5.0 * sr / n);
            std::vector<std::vector<float>> ins (11, std::vector<float> ((size_t) n));
            for (auto& v : ins) for (auto& q : v) q = r.bi() * 0.1f;
            std::vector<const float*> ip; for (auto& v : ins) ip.push_back (v.data());
            juce::AudioBuffer<float> o (36, n);
            std::vector<float*> op; for (int c = 0; c < 36; ++c) op.push_back (o.getWritePointer (c));
            auto t2 = std::chrono::high_resolution_clock::now();
            for (int b = 0; b < blocks; ++b) cvB->process (ip.data(), n, op.data(), 36, 1.0f);
            auto t3 = std::chrono::high_resolution_clock::now();
            const double rt = 5.0 / std::chrono::duration<double> (t3 - t2).count();
            std::cout << "  5th order, 4 s IR: build " << std::chrono::duration<double> (t1 - t0).count() << " s, " << rt << "x realtime" << std::endl;
            check (rt > 0.8, "5th-order room runs in real time (this slow test machine: ~1x)");
        }
        fb.deleteFile();
    }

    // 5) resampling path
    Convolver::Settings st4 = st; st4.hostRate = 44100.0;
    auto cv2 = Convolver::create (f, st4, err);
    check (cv2 != nullptr && std::abs (cv2->getInfo().seconds - 0.7) < 0.02, "IR resampled to 44.1 kHz, length " + (cv2 ? juce::String (cv2->getInfo().seconds, 3) : juce::String()));
    f.deleteFile(); fm1.deleteFile();
}

static void testMotion()
{
    std::cout << "motion" << std::endl;
    // continuity: position as a function of phase must not jump (except Jumps mode)
    for (int m : { 1, 2, 3, 4 })
    {
        float worst = 0;
        Pos b { 30.0f, 20.0f };
        Pos prev = applyMotion (b, (Motion) m, -3.0, 40.0f, 5);
        for (int i = 1; i < 20000; ++i)
        {
            const double ph = -3.0 + i * 0.0003;
            Pos q = applyMotion (b, (Motion) m, ph, 40.0f, 5);
            float da = std::abs (wrapAz (q.az - prev.az));
            if (std::abs (q.el) > 85.0f || std::abs (prev.el) > 85.0f) da = 0;      // azimuth is undefined at the poles
            worst = std::max (worst, da + std::abs (q.el - prev.el));
            prev = q;
        }
        check (worst < 3.0f, motionNames()[m] + " moves continuously (max step " + juce::String (worst, 2) + " deg)");
    }
    // vertical rotation passes over the top
    Pos top = applyMotion ({ 0.0f, 0.0f }, Motion::Vertical, 0.25, 0, 1);
    check (top.el > 89.0f, "vertical rotation: front -> zenith after a quarter turn");
    Pos back = applyMotion ({ 0.0f, 0.0f }, Motion::Vertical, 0.5, 0, 1);
    check (std::abs (std::abs (back.az) - 180.0f) < 1.0f && std::abs (back.el) < 1.0f, "vertical rotation: behind after half a turn");
    // jumps: constant within one step, different after
    Pos j1 = applyMotion ({ 0, 0 }, Motion::Jumps, 0.10, 90.0f, 3), j2 = applyMotion ({ 0, 0 }, Motion::Jumps, 0.11, 90.0f, 3);
    int changes = 0; Pos pj = j1;
    for (int i = 0; i < 1000; ++i) { Pos q = applyMotion ({ 0, 0 }, Motion::Jumps, i * 0.01, 90.0f, 3); if (std::abs (q.az - pj.az) > 1.0e-6f) ++changes; pj = q; }
    check (std::abs (j1.az - j2.az) < 1.0e-6f || changes > 0, "jumps hold still between jumps");
    check (changes >= 8 && changes <= 11, "about one jump per cycle (" + juce::String (changes) + " in 10 cycles)");
}

static void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
    auto opts = juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (b.getNumChannels()).withBitsPerSample (24);
    if (auto w = wav.createWriterFor (os, opts))
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

struct Scene
{
    juce::String name;
    int outputMode;
    int channels;
    std::function<void (EngineParams&, double t)> params;
    juce::MidiBuffer midiAll;        // positions in samples from the start
    double seconds;
};

static juce::AudioBuffer<float> render (OrganEngine& e, Scene& sc, double sr, int block, double& rtFactor, int& maxVoices)
{
    const int total = (int) (sc.seconds * sr);
    juce::AudioBuffer<float> out (sc.channels, total), tmp (sc.channels, block);
    EngineParams p;
    p.outputMode = sc.outputMode;
    auto t0 = std::chrono::high_resolution_clock::now();
    maxVoices = 0;
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = std::min (block, total - pos);
        tmp.setSize (sc.channels, n, false, false, true);
        juce::MidiBuffer mb;
        for (const auto m : sc.midiAll)
            if (m.samplePosition >= pos && m.samplePosition < pos + n)
                mb.addEvent (m.getMessage(), m.samplePosition - pos);
        sc.params (p, pos / sr);
        e.process (tmp, mb, p);
        maxVoices = std::max (maxVoices, e.activeVoices.load());
        for (int c = 0; c < sc.channels; ++c) out.copyFrom (c, pos, tmp, c, 0, n);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    rtFactor = sc.seconds / std::chrono::duration<double> (t1 - t0).count();
    return out;
}

// ---------------- Lucier mode ----------------
static void testLucier (std::shared_ptr<const SampleLibrary> lib)
    {
        RoomParams rp;     // the default hall
        auto t0 = std::chrono::high_resolution_clock::now();
        auto base = lucierFromRoom (rp, 0.5f, 48000.0);
        auto t1 = std::chrono::high_resolution_clock::now();
        auto contrast = [] (const LucierResult& r)       // how far the strongest resonance stands out (curve: max 0 dB, median below)
        {
            auto c = r.curve; std::sort (c.begin(), c.end()); return -c[c.size() / 2];
        };
        std::vector<LucierResult> res;
        double buildMs = 0;
        for (float g : { 1.0f, 4.0f, 12.0f, 30.0f, 12.5f })
        {
            auto a0 = std::chrono::high_resolution_clock::now();
            res.push_back (buildLucier (*base, g, 0.3f, 1));
            buildMs = std::max (buildMs, std::chrono::duration<double, std::milli> (std::chrono::high_resolution_clock::now() - a0).count());
        }
        std::cout << "lucier: base '" << base->what << "' in " << std::chrono::duration<double, std::milli> (t1 - t0).count() << " ms; builds up to "
                  << buildMs << " ms; contrast gen 1/4/12/30: " << contrast (res[0]) << " / " << contrast (res[1]) << " / " << contrast (res[2]) << " / "
                  << contrast (res[3]) << " dB; strongest at gen 30:";
        for (auto f : res[3].peaks) std::cout << " " << f;
        std::cout << " Hz" << std::endl;
        check (contrast (res[1]) > contrast (res[0]) + 3.0f && contrast (res[2]) > contrast (res[1]) + 3.0f && contrast (res[3]) > contrast (res[2]),
               "lucier: the room's resonances stand out more with every generation");
        check (! res[3].peaks.empty() && res[3].peaks[0] > 30.0f && res[3].peaks[0] < 12000.0f, "lucier: resonances found");

        // in the engine: a chord through 12 generations, all wet; then off: the room rings out, the organ is back unchanged
        auto run = [&] (bool on, float mix, float& wetDb, float& afterDb, bool& finite, float& peak)
        {
            OrganEngine e; e.prepare (48000.0, 512); e.setLibrary (lib);
            e.setLucierIR (res[2].ir);
            EngineParams p; p.stopOn = {}; p.stopOn[1] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0;
            p.lucierOn = on; p.lucierMix = mix;
            juce::AudioBuffer<float> b (2, 512);
            double eOn = 0, eAfter = 0; finite = true; peak = 0;
            for (int pos = 0; pos < 48000 * 12; pos += 512)
            {
                juce::MidiBuffer mb;
                if (pos == 0) for (int nn : { 48, 55, 60, 64 }) mb.addEvent (juce::MidiMessage::noteOn (1, nn, (juce::uint8) 100), 0);
                if (pos >= 48000 * 6) p.lucierOn = false;
                e.process (b, mb, p);
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) { const float v = b.getSample (c, i); finite = finite && std::isfinite (v); peak = std::max (peak, std::abs (v)); }
                const double r = b.getRMSLevel (0, 0, 512);
                if (pos > 48000 * 3 && pos < 48000 * 6) eOn += r * r;
                if (pos > 48000 * 10) eAfter += r * r;
                if (std::getenv ("RYSUM_DEBUG_LUCIER") != nullptr && pos % 48128 == 0) std::cout << "   t " << pos / 48000.0 << " rms " << r << std::endl;
            }
            wetDb = (float) (10.0 * std::log10 (eOn + 1e-20)); afterDb = (float) (10.0 * std::log10 (eAfter + 1e-20));
        };
        float dryOn, dryAfter, wetOn, wetAfter, pk1, pk2; bool f1, f2;
        run (false, 1.0f, dryOn, dryAfter, f1, pk1);
        run (true, 1.0f, wetOn, wetAfter, f2, pk2);
        std::cout << "lucier in the engine: dry " << dryOn << " dB, all wet (gen 12) " << wetOn << " dB, peak " << pk2
                  << "; after switching off " << wetAfter << " dB (dry " << dryAfter << ")" << std::endl;
        check (f2 && pk2 < 2.0f, "lucier: output finite and limited");
        check (std::abs (wetOn - dryOn) < 12.0f, "lucier: the wet sound is about as loud as the organ");
        check (std::abs (wetAfter - dryAfter) < 0.5f, "lucier: switched off, the organ sounds as before (after the room rang out)");

        // a response from a file (a recorded room): read, trimmed to the direct sound
        juce::File tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rysum_lucier_test.wav");
        {
            juce::AudioBuffer<float> ir (1, 96000); ir.clear();
            XorShift rng (7);
            for (int i = 4800; i < 96000; ++i) ir.setSample (0, i, (i == 4800 ? 1.0f : 0.3f * rng.bi()) * std::exp (-(float) (i - 4800) / 9000.0f));
            tmp.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (tmp), 44100.0, 1, 24, {}, 0));
            w->writeFromAudioSampleBuffer (ir, 0, ir.getNumSamples());
        }
        juce::String err;
        auto fb = lucierFromFile (tmp, 48000.0, err);
        int argMax = 0;
        if (fb != nullptr) for (int i = 0; i < (int) fb->h.size(); ++i) if (std::abs (fb->h[(size_t) i]) > std::abs (fb->h[(size_t) argMax])) argMax = i;
        const bool fileOk = fb != nullptr && fb->h.size() > 48000 && argMax > 0.001 * 48000 && argMax < 0.004 * 48000;
        std::cout << "lucier from a file (44.1 kHz, 100 ms silence before): " << (fb != nullptr ? fb->what : err) << ", starts at the direct sound: " << fileOk << std::endl;
        check (fileOk, "lucier: impulse response file read, resampled and trimmed");
        tmp.deleteFile();
    }

int main (int argc, char** argv)
{
    const bool onlyRoom = std::getenv ("RYSUM_ONLY_ROOM") != nullptr;       // quick run: just the room tests
    const bool onlyWind = onlyRoom || std::getenv ("RYSUM_ONLY_WIND") != nullptr;   // quick run: just the wind model tests
    const bool onlyLucier = std::getenv ("RYSUM_ONLY_LUCIER") != nullptr;
    if (! onlyWind && ! onlyLucier)
    {
        testEncoder();
        testConvolver();
        testMotion();
        testBinaural (argc > 3 ? juce::File (argv[3]) : juce::File());      // optional 3rd argument: a SOFA file to test with
    }
    if (argc < 3) { std::cout << "usage: RysumRenderTest <samples> <outdir> [hrtf.sofa]" << std::endl; return failures; }

    juce::File folder (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    juce::File outDir (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    outDir.createDirectory();

    std::cout << "loading " << folder.getFullPathName() << std::endl;
    juce::String err;
    auto t0 = std::chrono::high_resolution_clock::now();
    std::shared_ptr<const SampleLibrary> lib (SampleLibrary::load (folder, err).release());
    auto t1 = std::chrono::high_resolution_clock::now();
    check (lib != nullptr, "library loaded " + err);
    if (lib == nullptr) return 1;
    if (std::getenv ("RYSUM_LUCIER_DEMO") != nullptr)
    {
        // a demo for the ears: a slow chord progression while the generation grows from 1 to 20 (all wet towards the end)
        RoomParams rp;
        auto base = lucierFromRoom (rp, 0.5f, 48000.0);
        OrganEngine e; e.prepare (48000.0, 512); e.setLibrary (lib);
        EngineParams p; p.stopOn = {}; p.stopOn[1] = true; p.stopOn[2] = true; p.clickLevel = 0.3f; p.wind = 0.1f;
        p.lucierOn = true; p.lucierMix = 0.35f;
        const int secs = 48;
        juce::AudioBuffer<float> all (2, 48000 * secs), b (2, 512);
        const int chords[4][4] { { 48, 55, 60, 64 }, { 45, 52, 57, 60 }, { 41, 53, 57, 60 }, { 43, 50, 55, 59 } };
        int lastGen = -1;
        for (int pos = 0; pos + 512 <= all.getNumSamples(); pos += 512)
        {
            const double t = pos / 48000.0;
            const float gen = 1.0f + 19.0f * (float) juce::jlimit (0.0, 1.0, t / 40.0);
            if ((int) (gen * 2) != lastGen) { lastGen = (int) (gen * 2); e.setLucierIR (buildLucier (*base, gen, 0.3f, (uint32_t) lastGen).ir); }
            p.lucierMix = 0.35f + 0.6f * (float) juce::jlimit (0.0, 1.0, t / 40.0);
            juce::MidiBuffer mb;
            const int c = (int) (t / 6.0) % 4, cPrev = (int) ((t - 512 / 48000.0) / 6.0) % 4;
            if (pos == 0 || (c != cPrev && t < 42.0))
            {
                if (pos > 0) for (int nn : chords[cPrev]) mb.addEvent (juce::MidiMessage::noteOff (1, nn), 0);
                for (int nn : chords[c]) mb.addEvent (juce::MidiMessage::noteOn (1, nn, (juce::uint8) 90), 1);
            }
            if (t >= 42.0 && t < 42.0 + 512 / 48000.0) for (int nn : chords[c]) mb.addEvent (juce::MidiMessage::noteOff (1, nn), 0);
            e.process (b, mb, p);
            for (int ch = 0; ch < 2; ++ch) all.copyFrom (ch, pos, b, ch, 0, 512);
        }
        all.applyGain (0.8f / juce::jmax (0.01f, all.getMagnitude (0, all.getNumSamples())));
        auto f = outDir.getChildFile ("lucier_demo.wav");
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), 48000.0, 2, 24, {}, 0));
        w->writeFromAudioSampleBuffer (all, 0, all.getNumSamples());
        std::cout << "wrote " << f.getFullPathName() << std::endl;
        return 0;
    }
    if (onlyLucier) { testLucier (lib); std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string (failures)) << std::endl; return failures; }
    std::cout << "  " << lib->loadedPipes << " pipes, " << lib->substitutedPipes << " substituted, "
              << lib->clicksDown.size() << "/" << lib->clicksUp.size() << "/" << lib->clicksOther.size() << " clicks, "
              << lib->stopPulls.size() << " pulls, " << lib->wind.size() << " wind, "
              << std::chrono::duration<double> (t1 - t0).count() << " s" << std::endl;
    for (int s = 0; s < kNumStops; ++s)
        for (int k = 0; k < kNumKeys; ++k)
        {
            const auto& p = lib->pipes[(size_t) s][(size_t) k];
            if (p.substituted) std::cout << "  substituted " << kStops[(size_t) s].id << " " << helmholtzName (p.midi) << " <- " << p.flags.joinIntoString (", ") << std::endl;
            if (! p.valid) std::cout << "  INVALID " << kStops[(size_t) s].id << " " << helmholtzName (p.midi) << std::endl;
        }

    const double sr = 48000.0; const int block = 256;
    auto note = [sr] (juce::MidiBuffer& mb, int midi, double on, double off)
    {
        mb.addEvent (juce::MidiMessage::noteOn (1, midi, (juce::uint8) 100), (int) (on * sr));
        mb.addEvent (juce::MidiMessage::noteOff (1, midi), (int) (off * sr));
    };

    std::vector<Scene> scenes;
    {   // 1. Praestant alone: a slow walk through all keys (stereo)
        Scene s { "01_praestant_scale_stereo", 0, 2, nullptr, {}, 0 };
        double t = 0.2;
        for (int k = 0; k < kNumKeys; ++k) { note (s.midiAll, kPhysKeys[(size_t) k], t, t + 0.55); t += 0.6; }
        s.seconds = t + 1.5;
        s.params = [] (EngineParams& p, double) { p.stopOn = {}; p.stopOn[0] = true; };
        scenes.push_back (s);
    }
    {   // 2. held chord, stops pulled one after another, long hold (loops!), stereo
        Scene s { "02_plenum_build_stereo", 0, 2, nullptr, {}, 14.0 };
        for (int m : { 48, 55, 60, 64, 67 }) note (s.midiAll, m, 0.3, 12.0);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {};
            for (int i = 0; i < kNumStops; ++i) p.stopOn[(size_t) i] = t > 0.2 + 1.3 * i;
        };
        scenes.push_back (s);
    }
    {   // 3. same in AmbiX 5th order with motion
        Scene s { "03_plenum_ambix5_motion", 5, 36, nullptr, {}, 12.0 };
        for (int m : { 41, 53, 60, 65, 69, 72 }) note (s.midiAll, m, 0.3, 10.0);
        s.params = [] (EngineParams& p, double)
        {
            p.stopOn.fill (true);
            p.motionMode.fill ((int) Motion::Rotation);
            p.motionRate.fill (0.15f);
            p.motionDepth.fill (40.0f);
            p.motionMode[1] = (int) Motion::Drift;
        };
        scenes.push_back (s);
    }
    {   // 4. degradation per pipe, then on the sum (stereo)
        Scene s { "04_verfall_stereo", 0, 2, nullptr, {}, 12.0 };
        for (int m : { 45, 52, 57, 61, 64 }) note (s.midiAll, m, 0.2, 5.5);
        for (int m : { 45, 52, 57, 61, 64 }) note (s.midiAll, m, 6.2, 11.0);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {}; p.stopOn[0] = p.stopOn[1] = p.stopOn[2] = true;
            p.deg.amount = 0.9f; p.deg.grain = 0.6f; p.deg.bits = 7.0f; p.deg.rateHz = 6000.0f; p.deg.wobble = 0.7f; p.deg.dropouts = 0.4f;
            p.deg.perPipe = t < 6.0;
        };
        scenes.push_back (s);
    }
    {   // 5. stress: all stops, all 41 keys at once, AmbiX 5
        Scene s { "05_stress_all_keys_ambix5", 5, 36, nullptr, {}, 6.0 };
        for (int k = 0; k < kNumKeys; ++k) note (s.midiAll, kPhysKeys[(size_t) k], 0.1 + 0.01 * k, 4.0);
        s.params = [] (EngineParams& p, double) { p.stopOn.fill (true); p.motionMode.fill ((int) Motion::Drift); p.motionRate.fill (0.3f); p.motionDepth.fill (60.0f);
                                                  p.deg.amount = 0.5f; p.deg.perPipe = true; };
        scenes.push_back (s);
        Scene s2 = s; s2.name = "06_stress_all_keys_ambix5_clean";
        s2.params = [] (EngineParams& p, double) { p.stopOn.fill (true); };
        scenes.push_back (s2);
    }

    {   // 7. stop wind: Praestant slowly closed and opened again (like pulling the stop in and out)
        Scene s { "07_stopwind_sweep_stereo", 0, 2, nullptr, {}, 12.0 };
        for (int m : { 53, 60, 65, 69 }) note (s.midiAll, m, 0.2, 11.0);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {}; p.stopOn[0] = true;
            const double x = juce::jlimit (0.0, 1.0, (t - 1.0) / 10.0);           // 0..1 over 1..11 s
            p.stopWind[0] = (float) std::abs (1.0 - 2.0 * x);                        // 1 -> 0 -> 1
        };
        scenes.push_back (s);
    }
    {   // 8. global wind 0 -> 100 %
        Scene s { "08_global_wind_ramp_stereo", 0, 2, nullptr, {}, 14.0 };
        for (int m : { 48, 55, 60, 64 }) note (s.midiAll, m, 0.2, 13.0);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {}; p.stopOn[0] = p.stopOn[2] = true;
            p.wind = (float) juce::jlimit (0.0, 1.0, t / 12.0);
        };
        scenes.push_back (s);
    }

    {   // 9. speed glissando of a rotation, -2 Hz .. +2 Hz, first order (must stay click-free)
        Scene s { "09_rotation_glissando_ambix1", 1, 4, nullptr, {}, 12.0 };
        for (int m : { 57, 64, 69 }) note (s.midiAll, m, 0.2, 11.5);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0;
            p.motionMode[0] = (int) Motion::Rotation;
            p.motionRate[0] = (float) (-2.0 + 4.0 * juce::jlimit (0.0, 1.0, t / 11.0));
        };
        scenes.push_back (s);
    }

    {   // 13. fast: rotation glissando -10 Hz .. +10 Hz in 5th order, then jumps at 10 Hz
        Scene s { "13_fast_rotation_ambix5", 5, 36, nullptr, {}, 14.0 };
        for (int m : { 57, 64, 69 }) note (s.midiAll, m, 0.2, 13.5);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0;
            p.motionMode[0] = (int) (t < 10.0 ? Motion::Rotation : Motion::Jumps);
            p.motionRate[0] = t < 10.0 ? (float) (-10.0 + 20.0 * juce::jlimit (0.0, 1.0, t / 10.0)) : 10.0f;
            p.motionDepth[0] = 90.0f;
        };
        scenes.push_back (s);
    }

    {   // 10. Doppler: rotation 0.5 Hz with an elliptic orbit (closest in front)
        Scene s { "10_doppler_ellipse_stereo", 0, 2, nullptr, {}, 9.0 };
        note (s.midiAll, 69, 0.2, 8.5);
        s.params = [] (EngineParams& p, double)
        {
            p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0;
            p.motionMode[0] = (int) Motion::Rotation; p.motionRate[0] = 0.5f; p.ellipse[0] = 0.8f; p.doppler = 1.0f;
        };
        scenes.push_back (s);
    }
    {   // 11. tremulant
        Scene s { "11_tremulant_stereo", 0, 2, nullptr, {}, 6.0 };
        for (int m : { 53, 60, 65, 69 }) note (s.midiAll, m, 0.2, 5.5);
        s.params = [] (EngineParams& p, double) { p.stopOn = {}; p.stopOn[1] = true; p.tremOn = true; p.tremRate = 5.5f; p.tremDepth = 0.8f; };
        scenes.push_back (s);
    }
    {   // 12. freeze: chord released at 1.5 s keeps sounding, freeze off at 6 s -> fades in 2 s
        Scene s { "12_freeze_stereo", 0, 2, nullptr, {}, 10.0 };
        for (int m : { 57, 64, 69, 72 }) note (s.midiAll, m, 0.2, 1.5);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {}; p.stopOn[0] = p.stopOn[2] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0;
            p.freeze = t < 6.0; p.freezeSmear = 0.6f; p.freezeRelease = 2.0f;
        };
        scenes.push_back (s);
    }

    if (onlyWind) scenes.clear();
    for (auto& sc : scenes)
    {
        OrganEngine e;
        e.prepare (sr, block);
        e.setLibrary (lib);
        double rt = 0; int maxV = 0;
        auto b = render (e, sc, sr, block, rt, maxV);
        bool finite = true;
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! std::isfinite (b.getSample (c, i))) { finite = false; break; }
        const float peak = b.getMagnitude (0, b.getNumSamples());
        const float rms0 = b.getRMSLevel (0, 0, b.getNumSamples());
        std::cout << sc.name << ": peak " << juce::Decibels::gainToDecibels (peak) << " dBFS, rms ch0 "
                  << juce::Decibels::gainToDecibels (rms0) << " dB, " << rt << "x realtime, max voices " << maxV << std::endl;
        check (finite, sc.name + " finite");
        check (peak > 0.001f, sc.name + " not silent");
        // the 328-voice stress scene with per-pipe degradation is a worst case; on slow test machines it may sit just below 1x
        check (rt > (sc.name.startsWith ("05_stress") ? 0.7 : 1.0), sc.name + " faster than realtime");
        writeWav (outDir.getChildFile (sc.name + ".wav"), b, sr);
        if (sc.name.startsWith ("12_freeze"))
        {
            const float held = b.getRMSLevel (0, (int) (4.0 * sr), (int) (1.0 * sr));
            const float after = b.getRMSLevel (0, (int) (9.0 * sr), (int) (0.9 * sr));
            std::cout << "  freeze: rms 4-5 s " << juce::Decibels::gainToDecibels (held) << " dB, 9-10 s " << juce::Decibels::gainToDecibels (after) << " dB" << std::endl;
            check (held > 0.01f && after < held * 0.01f, "freeze holds after release and fades when switched off");
        }
    }
    // pitch check at a different host sample rate: Praestant a' must stay at its recorded pitch,
    // the substituted Gedackt f must land on meantone f
    if (! onlyWind)
    for (double hostSr : { 44100.0, 48000.0, 96000.0 })
    for (auto [stop, midi] : { std::pair<int, int> { 0, 69 }, std::pair<int, int> { 1, 53 }, std::pair<int, int> { kPraestant16, 69 } })
    {
        OrganEngine e;
        e.prepare (hostSr, 512);
        e.setLibrary (lib);
        EngineParams p; p.stopOn = {}; p.stopOn[(size_t) stop] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0;
        juce::AudioBuffer<float> b (2, 512), all (1, (int) (hostSr * 2));
        for (int pos = 0; pos < all.getNumSamples(); pos += 512)
        {
            juce::MidiBuffer mb;
            if (pos == 0) mb.addEvent (juce::MidiMessage::noteOn (1, midi, (juce::uint8) 100), 0);
            e.process (b, mb, p);
            all.copyFrom (0, pos, b, 0, 0, juce::jmin (512, all.getNumSamples() - pos));
        }
        // autocorrelation pitch on 0.5..1.5 s
        const float* x = all.getReadPointer (0) + (int) (0.5 * hostSr);
        const int N = (int) hostSr;
        const double f0nom = meantoneHz (midi) * (stop == kPraestant16 ? 0.5 : 1.0);     // the 16' sounds an octave lower
        int bestLag = 0; double best = -1e9;
        for (int lag = (int) (hostSr / (f0nom * 1.1)); lag <= (int) (hostSr / (f0nom / 1.1)); ++lag)
        {
            double s = 0; for (int i = 0; i < N - lag; ++i) s += (double) x[i] * x[i + lag];
            if (s > best) { best = s; bestLag = lag; }
        }
        // parabolic refinement
        auto ac = [&] (int lag) { double s = 0; for (int i = 0; i < N - lag; ++i) s += (double) x[i] * x[i + lag]; return s; };
        const double y0 = ac (bestLag - 1), y1 = ac (bestLag), y2 = ac (bestLag + 1);
        const double lagF = bestLag + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2);
        const double f = hostSr / lagF;
        const double cents = 1200.0 * std::log2 (f / f0nom);
        std::cout << "pitch " << kStops[(size_t) stop].id << " " << helmholtzName (midi) << " @" << hostSr << ": " << f << " Hz (" << cents << " ct vs meantone)" << std::endl;
        check (std::abs (cents) < 8.0, "pitch within 8 ct of meantone");
    }

    {
        auto measure = [&] (int temperament, double pitchCents, int midi = 60, double expectHz = 0.0)
        {
            OrganEngine e; e.prepare (48000.0, 512); e.setLibrary (lib);
            EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0;
            p.tuning = makeKeyTuning (temperament); p.pitchCents = (float) pitchCents;
            juce::AudioBuffer<float> b (2, 512), all (1, 96000);
            for (int pos = 0; pos < all.getNumSamples(); pos += 512)
            {
                juce::MidiBuffer mb;
                if (pos == 0) mb.addEvent (juce::MidiMessage::noteOn (1, midi, (juce::uint8) 100), 0);
                e.process (b, mb, p);
                all.copyFrom (0, pos, b, 0, 0, juce::jmin (512, all.getNumSamples() - pos));
            }
            const float* x = all.getReadPointer (0) + 24000;
            const int N = 48000;
            if (expectHz <= 0.0) expectHz = 265.0;
            auto ac = [&] (int lag) { double s2 = 0; for (int i = 0; i < N - lag; ++i) s2 += (double) x[i] * x[i + lag]; return s2; };
            int best = 0; double bv = -1e9;
            for (int lag = (int) (48000.0 / (expectHz * 1.15)); lag <= (int) (48000.0 / (expectHz / 1.15)); ++lag) { const double v = ac (lag); if (v > bv) { bv = v; best = lag; } }
            const double y0 = ac (best - 1), y1 = ac (best), y2 = ac (best + 1);
            return 48000.0 / (best + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2));
        };
        {
            // exotic tunings: a' is the reference (444.5 Hz as recorded)
            const double a1 = measure (0, 0, 69, 444.5);
            struct Case { int t, midi; double cents; const char* what; };
            const Case cases[] { { 14, 70, 1200.0 / 31.0, "31-EDO step: b-flat' one 31st above a'" },
                                 { 14, 66, -3 * 1200.0 / 31.0, "31-EDO step: 3 steps below a'" },
                                 { 7, 64, 1200.0 * std::log2 ((5.0 / 4.0) / (5.0 / 3.0)), "just intonation: e' = 5/4 over c, a' = 5/3" },
                                 { 9, 70, 1200.0 * std::log2 (28.0 / 27.0), "harmonic series: b-flat' = 7th partial" },
                                 { 13, 64, 350.0 - 900.0, "Maqam Rast: e' a quarter tone low" },
                                 { 17, 60, -9 * 1200.0 * std::log2 (3.0) / 13.0, "Bohlen-Pierce: 9 steps below a'" } };
            for (auto& c : cases)
            {
                const double want = a1 * std::pow (2.0, c.cents / 1200.0);
                const double got = measure (c.t, 0, c.midi, want);
                const double err = 1200.0 * std::log2 (got / want);
                std::cout << "tuning " << c.what << ": " << got << " Hz (want " << want << ", " << err << " ct)" << std::endl;
                check (std::abs (err) < 3.0, juce::String ("tuning: ") + c.what);
            }
            // step tunings use the nearest recorded pipe: inside the organ's range no pipe moves more than ~70 ct
            const auto q = makeKeyTuning (14);
            float worst = 0; for (int k = 0; k < kNumKeys; ++k) worst = std::max (worst, std::abs (q.cents[(size_t) k]));
            std::cout << "31-EDO step: largest shift of a pipe " << worst << " ct" << std::endl;
            check (worst < 80.0f, "31-EDO step tuning picks the nearest pipes");
        }
        const double mt = measure (0, 0), eq = measure (6, 0), up = measure (0, 100);
        const double dEq = 1200 * std::log2 (eq / mt), dUp = 1200 * std::log2 (up / mt);
        std::cout << "tuning c': meantone " << mt << " Hz, equal " << eq << " Hz (" << dEq << " ct), +100 ct " << dUp << " ct" << std::endl;
        check (std::abs (dEq - (-10.3)) < 1.0, "equal temperament moves c' by -10.3 ct");
        check (std::abs (dUp - 100.0) < 1.0, "pitch +100 ct");
    }
    {
        // tempo sync: phase locked to the host position
        OrganEngine e; e.prepare (48000.0, 512);
        EngineParams p; p.sync = true; p.playing = true; p.bpm = 90; p.ppq = 8.0; p.motionRate[2] = 0.25f;
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer mb;
        e.process (b, mb, p);
        const double expect = 0.25 * (8.0 + 512.0 / 48000.0 * 90.0 / 60.0) * 0.5;
        check (std::abs (e.motionPhase[2].load() - expect) < 1.0e-9, "tempo sync: phase follows the host position");
    }

    // ---------------- wind model 2: a starving pipe whistles, stutters and hisses ----------------
    {
        const double wsr = 48000.0;
        auto renderWind = [&] (int stop, int model, std::vector<int> midis, std::function<float (double)> windAt, double secs)
        {
            OrganEngine e; e.prepare (wsr, 256); e.setLibrary (lib);
            EngineParams p; p.stopOn = {}; p.stopOn[(size_t) stop] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0;
            p.windModel = model;
            juce::AudioBuffer<float> b (2, 256), all (2, (int) (wsr * secs));
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int pos = 0; pos < all.getNumSamples(); pos += 256)
            {
                juce::MidiBuffer mb;
                if (pos == 0) for (int m : midis) mb.addEvent (juce::MidiMessage::noteOn (1, m, (juce::uint8) 100), 0);
                p.stopWind[(size_t) stop] = windAt (pos / wsr);
                e.process (b, mb, p);
                for (int c = 0; c < 2; ++c) all.copyFrom (c, pos, b, c, 0, juce::jmin (256, all.getNumSamples() - pos));
            }
            const double took = std::chrono::duration<double> (std::chrono::high_resolution_clock::now() - t0).count();
            return std::make_pair (all, secs / took);
        };
        auto highpassed = [&] (const juce::AudioBuffer<float>& b)
        {
            std::vector<float> y ((size_t) b.getNumSamples());
            juce::IIRFilter f1, f2;
            f1.setCoefficients (juce::IIRCoefficients::makeHighPass (wsr, 2500.0)); f2.setCoefficients (juce::IIRCoefficients::makeHighPass (wsr, 2500.0));
            for (int i = 0; i < b.getNumSamples(); ++i) y[(size_t) i] = f2.processSingleSampleRaw (f1.processSingleSampleRaw (b.getSample (0, i)));
            return y;
        };
        auto rmsDb = [&] (const float* x, int n) { double s = 0; for (int i = 0; i < n; ++i) s += (double) x[i] * x[i]; return 10.0 * std::log10 (s / juce::jmax (1, n) + 1e-30); };
        auto starve = [] (double t) { return t < 2.0 ? 1.0f : 0.12f; };
        for (int stop : { 0, 1, 6 })
        {
            const std::vector<int> chord { 53, 60, 65 };
            auto [m0, rt0] = renderWind (stop, 0, chord, starve, 8.0);
            auto [m1, rt1] = renderWind (stop, 1, chord, starve, 8.0);
            const juce::String id = kStops[(size_t) stop].id;
            writeWav (outDir.getChildFile ("46_wind2_" + id + ".wav"), m1, wsr);
            // full wind: the new model changes nothing
            float maxDiff = 0.0f;
            for (int i = 0; i < (int) (1.9 * wsr); ++i) maxDiff = std::max (maxDiff, std::abs (m0.getSample (0, i) - m1.getSample (0, i)));
            check (maxDiff == 0.0f, "wind model 2 at full wind is identical to the classic model (" + id + ")");
            bool finite = true;
            for (int i = 0; i < m1.getNumSamples(); ++i) finite = finite && std::isfinite (m1.getSample (0, i));
            check (finite, "wind model 2 finite (" + id + ")");
            const auto h0 = highpassed (m0), h1 = highpassed (m1);
            const int a = (int) (3.0 * wsr), n = (int) (5.0 * wsr);
            const double full = rmsDb (m0.getReadPointer (0) + (int) (0.8 * wsr), (int) wsr);
            const double low0 = rmsDb (m0.getReadPointer (0) + a, n), low1 = rmsDb (m1.getReadPointer (0) + a, n);
            const double hf0 = rmsDb (h0.data() + a, n), hf1 = rmsDb (h1.data() + a, n);
            std::cout << "wind model 2 " << id << ": full wind " << full << " dB; at 12 % wind classic " << low0 << " dB, new " << low1
                      << " dB; above 2.5 kHz classic " << hf0 << " dB, new " << hf1 << " dB; " << rt1 << "x realtime" << std::endl;
            check (hf1 > hf0 + 6.0, "wind model 2 adds high resonances at low wind (" + id + ")");
            check (low1 < full - 6.0, "a starving pipe stays quieter than at full wind (" + id + ")");
        }
        // the edge tone of one pipe: strongest spectral peak above 1.2 kHz in a 4096-sample frame
        auto edgePeak = [&] (const float* x, double lo = 1200.0, double hi = 10000.0) -> std::pair<double, double>
        {
            juce::dsp::FFT fft (12);
            std::vector<float> b (8192, 0.0f);
            for (int i = 0; i < 4096; ++i) b[(size_t) i] = x[i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / 4096.0f));
            fft.performFrequencyOnlyForwardTransform (b.data(), true);
            int best = 0; float bv = 0.0f;
            for (int k = (int) (lo * 4096 / wsr); k < (int) (hi * 4096 / wsr); ++k) if (b[(size_t) k] > bv) { bv = b[(size_t) k]; best = k; }
            const double y0 = b[(size_t) best - 1], y1 = b[(size_t) best], y2 = b[(size_t) best + 1];
            const double kf = best + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2);
            return { kf * wsr / 4096.0, 20.0 * std::log10 (bv + 1e-12) };
        };
        {
            // steady wind: the edge tone stays where it is (pitch and level)
            auto [b, rt] = renderWind (0, 1, { 60 }, [] (double t) { return t < 1.0 ? 1.0f : 0.22f; }, 8.0);
            writeWav (outDir.getChildFile ("46_wind2_steady_c1.wav"), b, wsr);
            std::vector<double> fs, ls;
            const double f0 = edgePeak (b.getReadPointer (0) + (int) (3.0 * wsr)).first;       // follow that one (a pipe may have two)
            for (double t = 3.0; t < 7.8; t += 0.1) { auto [f, l] = edgePeak (b.getReadPointer (0) + (int) (t * wsr), f0 / 1.06, f0 * 1.06); fs.push_back (f); ls.push_back (l); }
            auto spread = [] (const std::vector<double>& v) { double m = 0; for (double x : v) m += x; m /= (double) v.size();
                                                             double d = 0; for (double x : v) d += (x - m) * (x - m); return std::make_pair (m, std::sqrt (d / (double) v.size())); };
            const auto [fm, fsd] = spread (fs); const auto [lm, lsd] = spread (ls);
            std::cout << "wind model 2, c' at a steady 22 % wind: edge tone " << fm << " Hz (spread "
                      << 1200.0 * std::log2 ((fm + fsd) / fm) << " ct), level spread " << lsd << " dB" << std::endl;
            check (1200.0 * std::log2 ((fm + fsd) / fm) < 15.0 && lsd < 2.0, "wind model 2: steady wind -> steady edge tone");

            // the same pipe struck again: the same edge tone
            OrganEngine e; e.prepare (wsr, 256); e.setLibrary (lib);
            EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0; p.windModel = 1;
            p.stopWind[0] = 0.22f;
            juce::AudioBuffer<float> blk (2, 256), all (2, (int) (wsr * 7.0));
            for (int pos = 0; pos < all.getNumSamples(); pos += 256)
            {
                juce::MidiBuffer mb;
                if (pos == 0 || pos == (int) (3.5 * wsr) / 256 * 256) mb.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                if (pos == (int) (2.5 * wsr) / 256 * 256) mb.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                e.process (blk, mb, p);
                all.copyFrom (0, pos, blk, 0, 0, juce::jmin (256, all.getNumSamples() - pos));
            }
            const double f1 = edgePeak (all.getReadPointer (0) + (int) (1.8 * wsr)).first, f2 = edgePeak (all.getReadPointer (0) + (int) (5.3 * wsr)).first;
            std::cout << "wind model 2, c' struck twice: " << f1 << " / " << f2 << " Hz" << std::endl;
            check (std::abs (1200.0 * std::log2 (f2 / f1)) < 15.0, "wind model 2: the same pipe has the same edge tone every time");

            // falling wind: the edge tone glides down and gets quieter
            auto [c, rt2] = renderWind (0, 1, { 60 }, [] (double t) { return (float) juce::jlimit (0.06, 1.0, 0.5 - 0.06 * (t - 1.0)); }, 8.0);
            writeWav (outDir.getChildFile ("46_wind2_falling_c1.wav"), c, wsr);
            const auto early = edgePeak (c.getReadPointer (0) + (int) (2.5 * wsr)), late = edgePeak (c.getReadPointer (0) + (int) (6.6 * wsr));
            std::cout << "wind model 2, c' with falling wind: " << early.first << " Hz " << early.second << " dB -> " << late.first << " Hz " << late.second << " dB" << std::endl;
            check (late.first < early.first * 0.85 && late.second < early.second - 3.0, "wind model 2: falling wind -> edge tone glides down and fades");
            // different pipes: different edge tones
            auto [d, rt3] = renderWind (0, 1, { 64 }, [] (double t) { return t < 1.0 ? 1.0f : 0.22f; }, 4.0);
            const double fe = edgePeak (d.getReadPointer (0) + (int) (3.0 * wsr)).first;
            std::cout << "wind model 2, e' at 22 % wind: " << fe << " Hz (c' " << fm << " Hz)" << std::endl;
            check (std::abs (1200.0 * std::log2 (fe / fm)) > 50.0, "wind model 2: neighbouring pipes have their own edge tones");
        }
        {
            // 18 pipes starving at once
            std::vector<int> many; for (int k = 0; k < 18; ++k) many.push_back (kPhysKeys[(size_t) (k * 2)]);
            auto [b, rt] = renderWind (0, 1, many, [] (double t) { return (float) juce::jlimit (0.0, 1.0, 1.0 - t / 4.0) * 0.9f + 0.05f; }, 6.0);
            writeWav (outDir.getChildFile ("46_wind2_sweep_18_pipes.wav"), b, wsr);
            std::cout << "wind model 2, 18 pipes sweeping down: " << rt << "x realtime, peak " << juce::Decibels::gainToDecibels (b.getMagnitude (0, b.getNumSamples())) << " dBFS" << std::endl;
            check (rt > 1.0, "wind model 2 with 18 pipes faster than realtime");
        }
        // ---------------- free tuning (Scala, MTS-ESP) and the new modulation sources ----------------
        {
            // Scala parser
            {
                ScalaTuning t; juce::String err;
                const bool ok = ScalaTuning::parseScl ("! test.scl\n!\n31-EDO\n 31\n!\n" + [] { juce::String s; for (int i = 1; i <= 31; ++i) s << juce::String (1200.0 * i / 31.0, 5) << "\n"; return s; }(), t, err);
                const auto f = t.frequencies();
                const double c70 = 1200.0 * std::log2 (f[70] / f[69]), c38 = 1200.0 * std::log2 (f[69 + 31] / f[69]);
                std::cout << "scala: 31-EDO parsed " << ok << ", a' " << f[69] << " Hz, one step " << c70 << " ct, 31 steps " << c38 << " ct" << std::endl;
                check (ok && std::abs (f[69] - kA4) < 1e-6 && std::abs (c70 - 1200.0 / 31.0) < 1e-3 && std::abs (c38 - 1200.0) < 1e-3, "scala: 31-EDO, default mapping (a' = 444.5 Hz on note 69)");
                ScalaTuning j; juce::String e2;
                const bool ok2 = ScalaTuning::parseScl ("just\n7\n9/8\n5/4\n4/3\n3/2\n5/3\n15/8\n2/1\n", j, e2)
                              && ScalaTuning::parseKbm ("! white keys only\n12\n0\n127\n60\n69\n440.0\n7\n0\nx\n1\nx\n2\n3\nx\n4\nx\n5\nx\n6\n", j, e2);
                const auto g = j.frequencies();
                std::cout << "scala: just C major on the white keys: c' " << g[60] << ", e' " << g[64] << ", g' " << g[67] << ", c'' " << g[72] << ", c#' " << g[61] << std::endl;
                check (ok2 && std::abs (g[69] - 440.0) < 1e-6 && std::abs (g[64] / g[60] - 1.25) < 1e-9 && std::abs (g[67] / g[60] - 1.5) < 1e-9
                       && std::abs (g[72] / g[60] - 2.0) < 1e-9 && g[61] == 0.0, "scala: ratios, .kbm with unmapped keys and reference frequency");
                ScalaTuning bad; juce::String e3;
                check (! ScalaTuning::parseScl ("bad\n3\n100.0\n", bad, e3) && e3.isNotEmpty(), "scala: a broken file is refused with a message");
            }
            // free tuning in the engine
            auto pitchOf = [&] (const juce::AudioBuffer<float>& b, double f0)
            {
                const float* x = b.getReadPointer (0) + (int) (0.6 * 48000);
                const int N = 24000;
                auto ac = [&] (int lag) { double s2 = 0; for (int i = 0; i < N - lag; ++i) s2 += (double) x[i] * x[i + lag]; return s2; };
                int best = 0; double bv = -1e18;
                for (int lag = (int) (48000.0 / (f0 * 1.1)); lag <= (int) (48000.0 / (f0 / 1.1)); ++lag) { const double v = ac (lag); if (v > bv) { bv = v; best = lag; } }
                const double y0 = ac (best - 1), y1 = ac (best), y2 = ac (best + 1);
                return 48000.0 / (best + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2));
            };
            NoteTable table;
            for (int ch = 0; ch < 16; ++ch)
                for (int n = 0; n < 128; ++n)
                {
                    table.hz[(size_t) (ch * 128 + n)] = (float) (kA4 * std::pow (2.0, (n - 69 + ch * 0.5) / 31.0 * 1.0));   // 31-EDO, channel 2 half a step up
                    table.mapped[(size_t) (ch * 128 + n)] = n == 72 ? 0 : 1;
                }
            auto play = [&] (std::vector<std::pair<int, int>> notes, std::function<void (EngineParams&)> setup, double secs, int* voices = nullptr)
            {
                OrganEngine e; e.prepare (48000.0, 512); e.setLibrary (lib);
                EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0; p.notes = &table;
                setup (p);
                juce::AudioBuffer<float> b (2, 512), all (1, (int) (48000 * secs));
                for (int pos = 0; pos < all.getNumSamples(); pos += 512)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0) for (auto [ch, n] : notes) mb.addEvent (juce::MidiMessage::noteOn (ch, n, (juce::uint8) 100), 0);
                    e.process (b, mb, p);
                    if (voices != nullptr && pos == 512 * 10) *voices = e.activeVoices.load();
                    all.copyFrom (0, pos, b, 0, 0, juce::jmin (512, all.getNumSamples() - pos));
                }
                return all;
            };
            auto none = [] (EngineParams&) {};
            double worst = 0;
            for (int n : { 60, 69, 70, 71, 80 })
            {
                const double want = table.hz[(size_t) n];
                const double got = pitchOf (play ({ { 1, n } }, none, 1.5), want);
                worst = std::max (worst, std::abs (1200.0 * std::log2 (got / want)));
            }
            int v2 = 0, vUnmapped = 0, vFar = 0, vCh = 0;
            play ({ { 1, 69 }, { 1, 70 } }, none, 0.5, &v2);            // 38.7 ct apart: the same pipe, two voices
            play ({ { 1, 72 } }, none, 0.5, &vUnmapped);
            play ({ { 1, 127 } }, none, 0.5, &vFar);                     // 31-EDO note 127: 22 ct below a'' + 1 oct -> shifted from a'' (allowed: up to 2 octaves)
            play ({ { 1, 60 }, { 2, 60 } }, none, 0.5, &vCh);
            const double gotCh2 = pitchOf (play ({ { 2, 69 } }, none, 1.5), table.hz[128 + 69]);
            const double errCh2 = 1200.0 * std::log2 (gotCh2 / table.hz[128 + 69]);
            std::cout << "free tuning (31-EDO table): worst pitch error " << worst << " ct; a' + one step together: " << v2 << " voices; unmapped note: "
                      << vUnmapped << "; far above: " << vFar << "; same note on two channels: " << vCh << " voices, channel 2 error " << errCh2 << " ct" << std::endl;
            check (worst < 5.0, "free tuning: every MIDI note sounds at its own pitch (within the recordings' own accuracy)");
            check (v2 == 2, "free tuning: two notes on one pipe sound as two voices");
            check (vUnmapped == 0, "free tuning: unmapped notes stay silent");
            check (vCh == 2 && std::abs (errCh2) < 3.0, "free tuning: MIDI channels have their own pitches (multi-channel tables)");
            // velocity and pitch wheel as modulation sources
            auto levelAt = [&] (int vel)
            {
                OrganEngine e; e.prepare (48000.0, 512); e.setLibrary (lib);
                EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0;
                p.mod.routes[0] = { srcVelocity, tgtLevel, 0.3f };
                juce::AudioBuffer<float> b (2, 512); double en = 0;
                for (int pos = 0; pos < 48000; pos += 512)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0) mb.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) vel), 0);
                    e.process (b, mb, p);
                    if (pos > 24000) en += b.getRMSLevel (0, 0, 512);
                }
                return juce::Decibels::gainToDecibels ((float) en);
            };
            const float soft = levelAt (20), hard = levelAt (127);
            OrganEngine e; e.prepare (48000.0, 512); e.setLibrary (lib);
            EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0;
            p.mod.routes[0] = { srcPitchWheel, tgtPitch, 1.0f / 6.0f };        // full wheel = +2 semitones
            juce::AudioBuffer<float> b (2, 512), all (1, 72000);
            for (int pos = 0; pos < all.getNumSamples(); pos += 512)
            {
                juce::MidiBuffer mb;
                if (pos == 0) mb.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 100), 0);
                if (pos == 0) mb.addEvent (juce::MidiMessage::pitchWheel (1, 16383), 1);
                e.process (b, mb, p);
                all.copyFrom (0, pos, b, 0, 0, juce::jmin (512, all.getNumSamples() - pos));
            }
            const double bent = 1200.0 * std::log2 (pitchOf (all, kA4 * std::pow (2.0, 2.0 / 12.0)) / kA4);
            std::cout << "modulation: velocity 20 -> " << soft << " dB, 127 -> " << hard << " dB; pitch wheel up: " << bent << " ct" << std::endl;
            check (hard - soft > 8.0f, "modulation source: key velocity");
            check (std::abs (bent - 200.0) < 4.0, "modulation source: pitch wheel");

            // MPE: two notes on their own channels; channel 2 bends its note +2 semitones (range 48), channel 3 stays;
            // pressure on channel 3 makes only that note louder; MPE off: the same bend does nothing to the pitch
            auto mpePlay = [&] (bool mpe, int onlyCh, std::function<void (juce::MidiBuffer&, int)> extra, EngineParams* out = nullptr)
            {
                OrganEngine e; e.prepare (48000.0, 512); e.setLibrary (lib);
                EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0;
                p.mpe = mpe; p.mpeBendRange = 48.0f;
                p.mod.routes[0] = { srcPressure, tgtLevel, 0.4f };
                if (out != nullptr) *out = p;
                juce::AudioBuffer<float> b (2, 512), all (1, 72000);
                for (int pos = 0; pos < all.getNumSamples(); pos += 512)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                    {
                        if (onlyCh == 0 || onlyCh == 2) mb.addEvent (juce::MidiMessage::noteOn (2, 69, (juce::uint8) 100), 0);
                        if (onlyCh == 0 || onlyCh == 3) mb.addEvent (juce::MidiMessage::noteOn (3, 57, (juce::uint8) 100), 0);
                    }
                    extra (mb, pos);
                    e.process (b, mb, p);
                    all.copyFrom (0, pos, b, 0, 0, juce::jmin (512, all.getNumSamples() - pos));
                }
                return all;
            };
            const int bend2 = 8192 + juce::roundToInt (8191.0 * 2.0 / 48.0);
            auto bendCh2 = [&] (juce::MidiBuffer& mb, int pos) { if (pos == 4096) mb.addEvent (juce::MidiMessage::pitchWheel (2, bend2), 0); };
            const double mpeBent = 1200.0 * std::log2 (pitchOf (mpePlay (true, 2, bendCh2), kA4 * std::pow (2.0, 2.0 / 12.0)) / kA4);
            const double other = 1200.0 * std::log2 (pitchOf (mpePlay (true, 3, bendCh2), kA4 / 2.0) / (kA4 / 2.0));
            const double offBent = 1200.0 * std::log2 (pitchOf (mpePlay (false, 2, bendCh2), kA4) / kA4);
            auto rmsLate = [] (const juce::AudioBuffer<float>& b) { return juce::Decibels::gainToDecibels (b.getRMSLevel (0, 36000, 30000)); };
            auto press3 = [&] (juce::MidiBuffer& mb, int pos) { if (pos == 4096) mb.addEvent (juce::MidiMessage::channelPressureChange (3, 127), 0); };
            const float quiet2 = rmsLate (mpePlay (true, 2, [] (juce::MidiBuffer&, int) {})), pressed2 = rmsLate (mpePlay (true, 2, press3));
            const float quiet3 = rmsLate (mpePlay (true, 3, [] (juce::MidiBuffer&, int) {})), pressed3 = rmsLate (mpePlay (true, 3, press3));
            auto polyAt = [&] (juce::MidiBuffer& mb, int pos) { if (pos == 4096) mb.addEvent (juce::MidiMessage::aftertouchChange (3, 57, 127), 0); };
            const float poly3 = rmsLate (mpePlay (false, 3, polyAt));
            const float plain3 = rmsLate (mpePlay (false, 3, [] (juce::MidiBuffer&, int) {}));
            std::cout << "MPE: channel 2 bent +2 HT -> " << mpeBent << " ct; channel 3 note meanwhile " << other << " ct; MPE off: " << offBent
                      << " ct; pressure on ch 3: ch 2 note " << pressed2 - quiet2 << " dB, ch 3 note " << pressed3 - quiet3
                      << " dB; poly aftertouch (MPE off) " << poly3 - plain3 << " dB" << std::endl;
            check (std::abs (mpeBent - 200.0) < 5.0 && std::abs (other) < 5.0, "MPE: every note channel bends its own note");
            check (std::abs (offBent) < 5.0, "MPE off: pitch bend does not bend (unchanged)");
            check (std::abs (pressed2 - quiet2) < 0.5f && pressed3 - quiet3 > 6.0f, "MPE: pressure modulates only its own note");
            check (poly3 - plain3 > 6.0f, "polyphonic aftertouch as pressure");
        }

        testLucier (lib);

        // ---------------- motion: ellipse shape, drawn paths, hard jumps ----------------
        {
            // ellipse: near side where the direction says, 1 or 2 near sides, tilt
            auto distAt = [] (float az, float dir, int lobes, float tilt, float& el)
            {
                MotionShape sh; sh.ellipse = 0.5f; sh.ellipseDir = dir; sh.ellipseLobes = lobes; sh.ellipseTilt = tilt;
                const Pos p = applyMotion ({ az, 10.0f, 6.0f }, Motion::Static, 0.0, 0.0f, 1u, sh);
                el = p.el; return p.dist;
            };
            float el = 0;
            const float near1 = distAt (90, 90, 1, 0, el), far1 = distAt (-90, 90, 1, 0, el), side1 = distAt (0, 90, 1, 0, el);
            const float n2a = distAt (90, 90, 2, 0, el), n2b = distAt (-90, 90, 2, 0, el), s2 = distAt (0, 90, 2, 0, el);
            float elNear = 0, elFar = 0; distAt (30, 30, 1, 20, elNear); distAt (-150, 30, 1, 20, elFar);
            MotionShape old; old.ellipse = 0.5f;
            const float oldFormula = 6.0f * (1.0f - 0.5f * std::cos (juce::degreesToRadians (10.0f)) * std::cos (juce::degreesToRadians (40.0f)));
            const float now = applyMotion ({ 40.0f, 10.0f, 6.0f }, Motion::Static, 0.0, 0.0f, 1u, old).dist;
            std::cout << "ellipse: dir 90: near " << near1 << " m, far " << far1 << " m, side " << side1 << "; two near sides " << n2a << " / " << n2b
                      << ", side " << s2 << "; tilt 20: near side el " << elNear << ", far side " << elFar << std::endl;
            check (near1 < side1 && side1 < far1, "ellipse: the near side follows the direction");
            check (std::abs (n2a - n2b) < 1e-4f && n2a < s2, "ellipse form 2: near on both sides");
            check (std::abs (elNear - (10.0f + 20.0f * std::cos (juce::degreesToRadians (10.0f)))) < 0.01f && elFar < 10.0f - 15.0f, "ellipse: tilted orbit");
            check (std::abs (now - oldFormula) < 1e-4f, "ellipse: default settings unchanged (older patches sound the same)");

            // drawn path: a square loop; the pipes follow it, spread along it
            MotionPath path;
            const float corners[4][2] { { 0, 0 }, { 90, 0 }, { 180, 0 }, { -90, 0 } };
            for (int i = 0; i < 4; ++i)
            {
                const float a = juce::degreesToRadians (corners[i][0]);
                path.x[(size_t) i] = std::cos (a); path.y[(size_t) i] = std::sin (a); path.z[(size_t) i] = 0.0f;
                path.u[(size_t) i] = (float) i / 4.0f;
            }
            path.x[4] = 1.0f; path.y[4] = 0.0f; path.z[4] = 0.0f; path.u[4] = 1.0f; path.n = 5;
            MotionShape sh; sh.path = &path; sh.pathSpread = 0.0f;
            const Pos q0 = applyMotion ({ 33.0f, 20.0f, 7.0f }, Motion::Drawn, 0.25, 0.0f, 1u, sh);
            const Pos q1 = applyMotion ({ 33.0f, 20.0f, 7.0f }, Motion::Drawn, 0.625, 0.0f, 1u, sh);
            sh.pathSpread = 1.0f; sh.keyFrac = 0.5f;
            const Pos q2 = applyMotion ({ 33.0f, 20.0f, 7.0f }, Motion::Drawn, 0.0, 0.0f, 1u, sh);
            path.pingPong = true; sh.pathSpread = 0.0f;
            const Pos q3 = applyMotion ({ 33.0f, 20.0f, 7.0f }, Motion::Drawn, 0.75, 0.0f, 1u, sh);
            std::cout << "drawn path: at 1/4 az " << q0.az << ", at 5/8 az " << q1.az << " (between 180 and -90), spread pipe at 1/2: az " << q2.az
                      << ", there-and-back at 3/4: az " << q3.az << ", distance kept " << q0.dist << " m" << std::endl;
            check (std::abs (q0.az - 90.0f) < 0.5f && q1.az < -90.0f && q1.az > -180.0f && std::abs (std::abs (q2.az) - 180.0f) < 0.5f
                   && std::abs (std::abs (q3.az) - 180.0f) < 0.5f && std::abs (q0.dist - 7.0f) < 1e-4f, "drawn path: pipes follow the path (loop, spread, there and back)");

            // hard jumps: same jump rate, with and without the glide: the hard ones click (much more energy above 8 kHz)
            auto jumps = [&] (bool hard)
            {
                OrganEngine e; e.prepare (48000.0, 512); e.setLibrary (lib);
                EngineParams p; p.stopOn = {}; p.stopOn[1] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0;
                p.motionMode[1] = (int) Motion::Jumps; p.motionRate[1] = 150.0f; p.motionDepth[1] = 120.0f; p.jumpHard[1] = hard;
                juce::AudioBuffer<float> b (2, 512), all (2, 48000 * 2);
                for (int pos = 0; pos < all.getNumSamples(); pos += 512)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0) mb.addEvent (juce::MidiMessage::noteOn (1, 53, (juce::uint8) 100), 0);
                    e.process (b, mb, p);
                    for (int c = 0; c < 2; ++c) all.copyFrom (c, pos, b, c, 0, juce::jmin (512, all.getNumSamples() - pos));
                }
                writeWav (outDir.getChildFile (hard ? "47_jumps_hard_150hz.wav" : "47_jumps_soft_150hz.wav"), all, 48000.0);
                juce::IIRFilter f1, f2;
                f1.setCoefficients (juce::IIRCoefficients::makeHighPass (48000.0, 8000.0)); f2.setCoefficients (juce::IIRCoefficients::makeHighPass (48000.0, 8000.0));
                double eh = 0, et = 0; bool finite = true;
                for (int i = 24000; i < all.getNumSamples(); ++i)
                {
                    const float x = all.getSample (0, i);
                    finite = finite && std::isfinite (x);
                    const float h = f2.processSingleSampleRaw (f1.processSingleSampleRaw (x));
                    eh += (double) h * h; et += (double) x * x;
                }
                return finite ? 10.0 * std::log10 (eh / juce::jmax (1e-30, et)) : 999.0;
            };
            const double soft = jumps (false), hard = jumps (true);
            std::cout << "jumps at 150 Hz: energy above 8 kHz relative to all: glided " << soft << " dB, hard " << hard << " dB" << std::endl;
            check (hard < 900.0 && hard > soft + 10.0, "hard jumps: they click (and stay finite)");
            // very fast rotation stays clean (finite)
            {
                OrganEngine e; e.prepare (48000.0, 512); e.setLibrary (lib);
                EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.motionMode[0] = (int) Motion::Rotation; p.motionRate[0] = 500.0f; p.ellipse[0] = 0.8f;
                juce::AudioBuffer<float> b (2, 512); bool finite = true;
                for (int pos = 0; pos < 48000; pos += 512)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0) mb.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 100), 0);
                    e.process (b, mb, p);
                    for (int i = 0; i < 512; ++i) finite = finite && std::isfinite (b.getSample (0, i));
                }
                check (finite, "rotation at 500 Hz with an ellipse stays finite");
            }
        }

        if (onlyWind && ! onlyRoom) { std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string (failures)) << std::endl; return failures; }
    }

    // ---------------- modulation ----------------
    {
        const double msr = 48000.0;
        auto run = [&] (std::function<void (EngineParams&)> setup, std::vector<int> midis, double secs, int mode, int ch, double offAt,
                        const juce::String& name)
        {
            OrganEngine e; e.prepare (msr, 512); e.setLibrary (lib);
            EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0;
            p.outputMode = mode;
            setup (p);
            juce::AudioBuffer<float> b (ch, 512), all (ch, (int) (msr * secs));
            for (int pos = 0; pos < all.getNumSamples(); pos += 512)
            {
                juce::MidiBuffer mb;
                if (pos == 0) for (int m : midis) mb.addEvent (juce::MidiMessage::noteOn (1, m, (juce::uint8) 100), 0);
                if (offAt > 0 && pos == ((int) (offAt * msr) / 512) * 512) for (int m : midis) mb.addEvent (juce::MidiMessage::noteOff (1, m), 0);
                e.process (b, mb, p);
                for (int c = 0; c < ch; ++c) all.copyFrom (c, pos, b, c, 0, juce::jmin (512, all.getNumSamples() - pos));
            }
            writeWav (outDir.getChildFile (name + ".wav"), all, msr);
            return all;
        };
        auto rms = [&] (const juce::AudioBuffer<float>& b, double t0, double t1)
        { return juce::Decibels::gainToDecibels (b.getRMSLevel (0, (int) (t0 * msr), (int) ((t1 - t0) * msr))); };
        auto pitchAt = [&] (const juce::AudioBuffer<float>& b, double t0, double len, double f0)
        {
            const float* x = b.getReadPointer (0) + (int) (t0 * msr);
            const int N = (int) (len * msr);
            auto ac = [&] (int lag) { double s = 0; for (int i = 0; i < N - lag; ++i) s += (double) x[i] * x[i + lag]; return s; };
            int bestLag = 0; double best = -1e18;
            for (int lag = (int) (msr / (f0 * 1.15)); lag <= (int) (msr / (f0 / 1.15)); ++lag) { const double v = ac (lag); if (v > best) { best = v; bestLag = lag; } }
            const double y0 = ac (bestLag - 1), y1 = ac (bestLag), y2 = ac (bestLag + 1);
            return msr / (bestLag + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2));
        };

        {   // LFO square 1 Hz -> pipe level +-10.8 dB
            auto b = run ([] (EngineParams& p) { p.mod.lfo[0].shape = shSquare; p.mod.lfo[0].rate = 1.0f; p.mod.routes[0] = { srcLfo1, tgtLevel, 0.3f }; },
                          { 69 }, 3.0, 0, 2, -1, "14_mod_lfo_level");
            const float hi = rms (b, 1.1, 1.4), lo = rms (b, 1.6, 1.9);
            std::cout << "mod level: high " << hi << " dB, low " << lo << " dB" << std::endl;
            check (hi - lo > 18.0f && hi - lo < 25.0f, "LFO -> pipe level (expect 21.6 dB swing)");
        }
        {   // LFO square 1 Hz -> pitch +-24 ct
            auto b = run ([] (EngineParams& p) { p.mod.lfo[1].shape = shSquare; p.mod.lfo[1].rate = 1.0f; p.mod.routes[3] = { srcLfo2, tgtPitch, 0.02f }; },
                          { 69 }, 3.0, 0, 2, -1, "15_mod_lfo_pitch");
            const double fHi = pitchAt (b, 1.1, 0.3, meantoneHz (69)), fLo = pitchAt (b, 1.6, 0.3, meantoneHz (69));
            const double d = 1200.0 * std::log2 (fHi / fLo);
            std::cout << "mod pitch: " << fHi << " / " << fLo << " Hz = " << d << " ct" << std::endl;
            check (std::abs (d - 48.0) < 3.0, "LFO -> pipe pitch (expect 48 ct)");
        }
        {   // per-pipe envelope -> distance: far at the envelope peak, back to base when it has decayed
            auto b = run ([] (EngineParams& p)
                          {
                              p.mod.env[0] = { true, 0.0f, 0.4f, 0.8f, 0.0f, 1.0f };
                              p.mod.routes[5] = { srcEnv1, tgtDist, 0.5f };
                          }, { 57, 64, 69 }, 4.0, 1, 4, -1, "16_mod_env_distance");
            const float farL = rms (b, 0.4, 0.5), nearL = rms (b, 3.0, 3.5);
            std::cout << "mod env->distance: at peak " << farL << " dB, later " << nearL << " dB" << std::endl;
            check (nearL - farL > 4.0f, "per-pipe envelope -> distance");
        }
        {   // global envelope with release: level follows the key
            auto b = run ([] (EngineParams& p)
                          {
                              p.mod.env[1] = { false, 0.0f, 0.01f, 0.1f, 1.0f, 0.5f };
                              p.mod.routes[7] = { srcEnv2, tgtLevel, 0.0f };
                          }, { 69 }, 2.0, 0, 2, 1.0, "17_mod_global_env");
            check (rms (b, 0.5, 0.9) > -60.0f, "global envelope scene renders");
        }
        {   // the wave: sine LFO 0.5 Hz on direction with full keyboard spread (listen in AmbiX)
            auto b = run ([] (EngineParams& p)
                          {
                              p.mod.lfo[2].rate = 0.5f; p.mod.lfo[2].spread = 1.0f;
                              p.mod.routes[2] = { srcLfo3, tgtAz, 0.5f };
                              p.mod.lfo[3].shape = shSmooth; p.mod.lfo[3].rate = 3.0f; p.mod.lfo[3].spread = 1.0f; p.mod.lfo[3].spreadMode = 1;
                              p.mod.routes[9] = { srcLfo4, tgtBright, -0.4f };
                          }, { 45, 48, 52, 57, 60, 64, 69, 72, 76 }, 8.0, 3, 16, -1, "18_mod_wave_ambix3");
            check (rms (b, 1.0, 7.0) > -60.0f, "spread wave scene renders");
        }
        {   // spread maths: offset along the keyboard
            check (std::abs (lfoValue (shSine, 0.25, 0) - 1.0f) < 1e-6f && std::abs (lfoValue (shTriangle, 0.75, 0) + 1.0f) < 1e-6f, "LFO shapes");
            EnvState es; EnvParams ep { true, 0.0f, 0.1f, 0.1f, 0.5f, 0.1f };
            es.trigger(); for (int i = 0; i < 100; ++i) es.advance (ep, 0.01f);
            const float sus = es.level; es.releaseGate(); for (int i = 0; i < 200; ++i) es.advance (ep, 0.01f);
            check (std::abs (sus - 0.5f) < 0.01f && es.level == 0.0f, "envelope sustain + release");
        }
    }

    // ---------------- effects (page "Effekte") ----------------
    {
        const double fsr = 48000.0;
        constexpr int B = 512;
        auto run = [&] (std::function<void (EngineParams&)> setup, std::vector<int> midis, double secs, double offAt, const juce::String& name,
                        OrganEngine* keep = nullptr)
        {
            OrganEngine local; OrganEngine& e = keep != nullptr ? *keep : local;
            e.prepare (fsr, B); e.setLibrary (lib);
            EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0;
            p.outputMode = 1;
            setup (p);
            juce::AudioBuffer<float> b (4, B), all (4, (int) (fsr * secs));
            for (int pos = 0; pos < all.getNumSamples(); pos += B)
            {
                juce::MidiBuffer mb;
                if (pos == 0) for (int m : midis) mb.addEvent (juce::MidiMessage::noteOn (1, m, (juce::uint8) 100), 0);
                if (offAt > 0 && pos == ((int) (offAt * fsr) / B) * B) for (int m : midis) mb.addEvent (juce::MidiMessage::noteOff (1, m), 0);
                e.process (b, mb, p);
                for (int c = 0; c < 4; ++c) all.copyFrom (c, pos, b, c, 0, juce::jmin (B, all.getNumSamples() - pos));
            }
            writeWav (outDir.getChildFile (name + ".wav"), all, fsr);
            return all;
        };
        // direction of the energy (intensity vector) in a window, optionally above a frequency
        struct Dir { float az, el, dirness, firstOrderRatio, db; };
        auto direction = [&] (const juce::AudioBuffer<float>& in, double t0, double t1, float hpHz = 0.0f)
        {
            const int a = (int) (t0 * fsr), n = (int) ((t1 - t0) * fsr);
            std::array<std::vector<float>, 4> x;
            for (int c = 0; c < 4; ++c)
            {
                x[(size_t) c].assign (in.getReadPointer (c) + a, in.getReadPointer (c) + a + n);
                if (hpHz > 0.0f)
                {
                    // 4th order: two passes of a 2nd order Butterworth high-pass
                    for (int pass = 0; pass < 2; ++pass)
                    {
                        auto co = juce::IIRCoefficients::makeHighPass (fsr, hpHz);
                        juce::IIRFilter f; f.setCoefficients (co);
                        f.processSamples (x[(size_t) c].data(), n);
                    }
                }
            }
            double ww = 0, wy = 0, wz = 0, wx = 0, e1 = 0;
            for (int i = 0; i < n; ++i)
            {
                const double w = x[0][(size_t) i];
                ww += w * w; wy += w * x[1][(size_t) i]; wz += w * x[2][(size_t) i]; wx += w * x[3][(size_t) i];
                e1 += (double) x[1][(size_t) i] * x[1][(size_t) i] + (double) x[2][(size_t) i] * x[2][(size_t) i] + (double) x[3][(size_t) i] * x[3][(size_t) i];
            }
            Dir d;
            d.az = (float) juce::radiansToDegrees (std::atan2 (wy, wx));
            d.el = (float) juce::radiansToDegrees (std::atan2 (wz, std::sqrt (wx * wx + wy * wy)));
            d.dirness = (float) (std::sqrt (wx * wx + wy * wy + wz * wz) / juce::jmax (1.0e-20, ww));
            d.firstOrderRatio = (float) (e1 / juce::jmax (1.0e-20, ww));
            d.db = juce::Decibels::gainToDecibels ((float) std::sqrt (ww / juce::jmax (1, n)), -200.0f);
            return d;
        };
        auto angleDiff = [] (float a, float b) { return std::abs (wrapAz (a - b)); };
        OrganEngine probe; probe.prepare (fsr, B); probe.setLibrary (lib);
        const EngineParams dflt;
        int keyA = -1;
        for (int k = 0; k < kNumKeys; ++k) if (kPhysKeys[(size_t) k] == 69) keyA = k;
        const Pos posA = probe.currentPosition (0, keyA, dflt);
        std::cout << "pipe a' (Praestant) at az " << posA.az << ", el " << posA.el << std::endl;

        {   // wandering echoes: 1st repeat from +90 deg, 2nd from 180 deg
            auto b = run ([] (EngineParams& p)
                          {
                              p.fx.echoLevel = 1.0f; p.fx.echoTime = 1.5f; p.fx.echoRepeats = 2; p.fx.echoFeedback = 1.0f;
                              p.fx.echoTurn = 90.0f; p.fx.echoDamp = 0.0f;
                          }, { 69 }, 3.8, 0.35, "30_fx_echo_ambix1");
            const auto d0 = direction (b, 0.05, 0.3), d1 = direction (b, 1.55, 1.8), d2 = direction (b, 3.05, 3.3);
            std::cout << "echo: dry az " << d0.az << ", 1st " << d1.az << " (" << d1.db << " dB), 2nd " << d2.az << " (" << d2.db << " dB)" << std::endl;
            check (angleDiff (d1.az, 90.0f) < 25.0f && angleDiff (d2.az, 180.0f) < 25.0f, "echoes: every repeat from the next direction");
            check (std::abs (d1.db - d0.db) < 6.0f, "echo level about like the dry sound at 100 %");
        }
        {   // upper partials 90 deg away; the fundamental stays at the pipe
            auto b = run ([] (EngineParams& p) { p.fx.partSpread = 90.0f; p.fx.partSplit = 2.5f; }, { 69 }, 1.5, -1, "31_fx_partials_ambix1");
            const auto lo = direction (b, 0.5, 1.4), hi = direction (b, 0.5, 1.4, (float) meantoneHz (69) * 5.0f);
            std::cout << "partials: whole az " << lo.az << ", above 5x f0 az " << hi.az << " (pipe " << posA.az << ")" << std::endl;
            check (angleDiff (hi.az, posA.az) > 40.0f, "upper partials placed away from the pipe");
            auto off = run ([] (EngineParams&) {}, { 69 }, 1.5, -1, "31_fx_partials_off");
            const float a = off.getRMSLevel (0, (int) (0.5 * fsr), (int) (0.9 * fsr)), c = b.getRMSLevel (0, (int) (0.5 * fsr), (int) (0.9 * fsr));
            check (std::abs (juce::Decibels::gainToDecibels (c / a)) < 1.0f, "partial split keeps the level (W)");
        }
        {   // crackle: (on - off) is only the crackle, and it sits at the pipe
            auto on = run ([] (EngineParams& p) { p.fx.crackLevel = 1.0f; p.fx.crackRate = 30.0f; }, { 69 }, 2.0, 1.2, "32_fx_crackle_ambix1");
            auto off = run ([] (EngineParams&) {}, { 69 }, 2.0, 1.2, "32_fx_crackle_off");
            juce::AudioBuffer<float> diff (on);
            for (int c = 0; c < 4; ++c) diff.addFrom (c, 0, off, c, 0, off.getNumSamples(), -1.0f);
            const auto d = direction (diff, 0.1, 1.1);
            const auto late = direction (diff, 1.9, 2.0);
            std::cout << "crackle: " << d.db << " dB at az " << d.az << ", el " << d.el << "; after release " << late.db << " dB" << std::endl;
            check (d.db > -50.0f && angleDiff (d.az, posA.az) < 8.0f, "crackle at the pipe");
            check (late.db < d.db - 30.0f, "crackle fades after the key is released");
        }
        {   // wind leaks: one leak = one direction; the diffuse blower = all around
            auto b = run ([] (EngineParams& p) { p.stopOn = {}; p.fx.leakLevel = 1.0f; p.fx.leakCount = 1; }, {}, 2.0, -1, "33_fx_leak_ambix1");
            const auto d = direction (b, 0.3, 2.0);
            std::cout << "leak: " << d.db << " dB, az " << d.az << ", directedness " << d.dirness << std::endl;
            check (d.db > -50.0f && d.db < -12.0f && d.dirness > 0.9f, "one wind leak: a point source");
            auto w = run ([] (EngineParams& p) { p.stopOn = {}; p.wind = 0.5f; p.fx.windRound = true; }, {}, 3.0, -1, "34_fx_wind_round_ambix1");
            const auto dw = direction (w, 1.0, 3.0);
            std::cout << "blower all around: directedness " << dw.dirness << ", 1st order / W " << dw.firstOrderRatio << std::endl;
            check (dw.dirness < 0.25f && dw.firstOrderRatio > 0.6f && dw.firstOrderRatio < 1.4f, "diffuse blower");
            auto w0 = run ([] (EngineParams& p) { p.stopOn = {}; p.wind = 0.5f; }, {}, 3.0, -1, "34_fx_wind_omni");
            const float a = w0.getRMSLevel (0, (int) fsr, (int) (2 * fsr)), c = w.getRMSLevel (0, (int) fsr, (int) (2 * fsr));
            std::cout << "blower level: omni " << juce::Decibels::gainToDecibels (a) << " dB, all around " << juce::Decibels::gainToDecibels (c) << " dB" << std::endl;
            check (std::abs (juce::Decibels::gainToDecibels (c / a)) < 2.0f, "diffuse blower keeps the level");
        }
        {   // tracker path: with "pipes wait" the pipe speaks after the path time
            auto lowRms = [&] (const juce::AudioBuffer<float>& b, double t0, double t1)
            {
                std::vector<float> x (b.getReadPointer (0) + (int) (t0 * fsr), b.getReadPointer (0) + (int) (t1 * fsr));
                juce::IIRFilter f; f.setCoefficients (juce::IIRCoefficients::makeLowPass (fsr, 1500.0));
                f.processSamples (x.data(), (int) x.size());
                double s = 0; for (auto v : x) s += (double) v * v;
                return juce::Decibels::gainToDecibels ((float) std::sqrt (s / (double) x.size()), -200.0f);
            };
            auto wait = run ([] (EngineParams& p) { p.fx.pathLevel = 1.0f; p.fx.pathTime = 0.3f; p.fx.pathTicks = 8; p.fx.pathWait = true; },
                             { 69 }, 1.2, -1, "35_fx_path_wait_ambix1");
            auto noWait = run ([] (EngineParams& p) { p.fx.pathLevel = 1.0f; p.fx.pathTime = 0.3f; p.fx.pathTicks = 8; }, { 69 }, 1.2, -1, "35_fx_path_ambix1");
            const float wEarly = lowRms (wait, 0.08, 0.25), nEarly = lowRms (noWait, 0.08, 0.25), wLate = lowRms (wait, 0.6, 1.0);
            std::cout << "tracker path: pipe band early " << wEarly << " dB (waiting) vs " << nEarly << " dB, later " << wLate << " dB" << std::endl;
            check (nEarly - wEarly > 15.0f && wLate > nEarly - 3.0f, "pipes wait for the tracker path");
            auto ticks = run ([] (EngineParams& p) { p.stopGain[0] = 0.0f; p.fx.pathLevel = 1.0f; p.fx.pathTime = 0.4f; p.fx.pathTicks = 8; p.fx.pathSpread = 0.0f; },
                              { 69 }, 0.6, -1, "35_fx_path_ticks_only");
            const auto first = direction (ticks, 0.0, 0.07), last = direction (ticks, 0.33, 0.45);
            std::cout << "tracker ticks: first at az " << first.az << " el " << first.el << ", last at az " << last.az << " el " << last.el << std::endl;
            check (angleDiff (first.az, 0.0f) < 20.0f && angleDiff (last.az, posA.az) < 12.0f, "tracker ticks run from the console to the pipe");
        }
        {   // own sounds: at the pipe (fallback = the recorded clicks when the folder is empty)
            auto b = run ([] (EngineParams& p) { p.stopGain[0] = 0.0f; p.fx.custLevel = 1.0f; p.fx.custPlace = 0; }, { 69 }, 0.8, -1, "36_fx_own_at_pipe");
            const auto d = direction (b, 0.0, 0.5);
            std::cout << "own sound at pipe: " << d.db << " dB, az " << d.az << std::endl;
            check (d.db > -60.0f && angleDiff (d.az, posA.az) < 10.0f, "own sound at the pipe");
            auto c = run ([] (EngineParams& p) { p.stopGain[0] = 0.0f; p.fx.custLevel = 1.0f; p.fx.custPlace = 3; p.fx.custWander = 180.0f; p.fx.custScatter = 1.0f; },
                          { 69 }, 2.0, 0.5, "36_fx_own_circling");
            check (c.getRMSLevel (0, 0, c.getNumSamples()) > 1.0e-5f, "own sounds circling render");

            // loading: transient at the start, peak -6 dBFS
            auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rysum_custom_test");
            tmp.deleteRecursively();
            auto dir = SampleLibrary::customFolder (tmp);
            dir.createDirectory();
            juce::AudioBuffer<float> snd (2, 24000);
            snd.clear();
            for (int i = 12000; i < 24000; ++i) { const float v = 0.2f * std::exp (-(i - 12000) / 800.0f) * (float) std::sin (i * 0.9); snd.setSample (0, i, v); snd.setSample (1, i, v); }
            writeWav (dir.getChildFile ("schraube.wav"), snd, 48000.0);
            auto list = SampleLibrary::loadCustom (tmp);
            float pk = 0; for (auto v : list.empty() ? std::vector<float>() : list[0].sample->data) pk = std::max (pk, std::abs (v));
            check (list.size() == 1 && list[0].sample->size() < 12100 && std::abs (pk - 0.5f) < 0.01f, "own sounds folder: trimmed + normalised");
            tmp.deleteRecursively();
        }
        {   // timing: everything on, 3rd order
            OrganEngine e; e.prepare (fsr, B); e.setLibrary (lib);
            EngineParams p; p.stopOn.fill (true); p.outputMode = 3;
            p.fx.custLevel = 0.5f; p.fx.custScatter = 1.0f; p.fx.pathLevel = 0.5f; p.fx.crackLevel = 0.5f; p.fx.crackRate = 40.0f;
            p.fx.leakLevel = 0.5f; p.fx.leakCount = 8; p.fx.leakWander = 30.0f; p.fx.windRound = true; p.wind = 0.3f;
            p.fx.partSpread = 60.0f; p.fx.partTurn = 0.3f; p.fx.echoLevel = 0.5f; p.fx.echoRepeats = 8;
            juce::AudioBuffer<float> b (16, B);
            const int blocks = (int) (6.0 * fsr / B);
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < blocks; ++i)
            {
                juce::MidiBuffer mb;
                if (i == 0) for (int m : { 48, 55, 60, 64, 67, 72 }) mb.addEvent (juce::MidiMessage::noteOn (1, m, (juce::uint8) 90), 0);
                e.process (b, mb, p);
            }
            const double rt = 6.0 / std::chrono::duration<double> (std::chrono::high_resolution_clock::now() - t0).count();
            std::cout << "all effects, 8 stops x 6 keys, 3rd order: " << rt << " x real time" << std::endl;
            check (rt > 2.0, "effects are cheap enough");
        }
    }

    // ---------------- simulated room ("Saal") ----------------
    {
        // ray tracer vs Eyring
        {
            RoomParams r; r.on = true; r.width = r.length = r.height = 10.0f; r.absWalls = r.absFloor = 0.2f; r.scatter = 0.5f; r.highDamp = 0.0f;
            auto t0 = std::chrono::high_resolution_clock::now();
            const auto rt = traceRoom (r, 0.5f);
            const double ms = 1000.0 * std::chrono::duration<double> (std::chrono::high_resolution_clock::now() - t0).count();
            const float ey = eyringRt (r, false, 0.5f);
            std::cout << "room: cube 10 m, alpha 0.2: ray traced " << rt.rtLow << " s, Eyring " << ey << " s (" << ms << " ms, "
                      << rt.reflections << " reflections)" << std::endl;
            check (std::abs (rt.rtLow / ey - 1.0f) < 0.12f, "ray tracer agrees with Eyring in a diffuse cube");
            RoomParams c = r; c.width = 3.0f; c.length = 40.0f; c.height = 3.0f; c.absWalls = 0.02f; c.absFloor = 0.6f; c.scatter = 0.02f;
            const auto rc = traceRoom (c, 0.5f);
            std::cout << "room: corridor, hard walls, dead floor/ceiling: ray traced " << rc.rtLow << " s, Eyring " << eyringRt (c, false, 0.5f) << " s" << std::endl;
            check (rc.rtLow > 1.3f * eyringRt (c, false, 0.5f), "ray tracer: non-diffuse corridor rings longer than Eyring says");
        }

        const double fsr = 48000.0;
        constexpr int B = 512;
        int keyA = -1; for (int k = 0; k < kNumKeys; ++k) if (kPhysKeys[(size_t) k] == 69) keyA = k;
        auto run = [&] (std::function<void (EngineParams&)> setup, double secs, double offAt, const juce::String& name, int ch = 4, OrganEngine* keep = nullptr)
        {
            OrganEngine local; OrganEngine& e = keep != nullptr ? *keep : local;
            e.prepare (fsr, B); e.setLibrary (lib);
            EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0;
            p.outputMode = ch == 4 ? 1 : (ch == 2 ? 0 : 3);
            setup (p);
            if (p.room.on) e.setTailIR (TailIR::build (p.room, p.air, p.outputMode, fsr, 1));      // (the plugin builds it in the background)
            juce::AudioBuffer<float> b (ch, B), all (ch, (int) (fsr * secs));
            for (int pos = 0; pos < all.getNumSamples(); pos += B)
            {
                juce::MidiBuffer mb;
                if (pos == 0) mb.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 110), 0);
                if (offAt > 0 && pos == ((int) (offAt * fsr) / B) * B) mb.addEvent (juce::MidiMessage::noteOff (1, 69), 0);
                e.process (b, mb, p);
                for (int c = 0; c < ch; ++c) all.copyFrom (c, pos, b, c, 0, juce::jmin (B, all.getNumSamples() - pos));
            }
            if (name.isNotEmpty()) writeWav (outDir.getChildFile (name + ".wav"), all, fsr);
            return all;
        };

        {   // long sessions: the reflections must sound the same after the plugin has run for a long time
            // (the read position of the delay lines once lost its fraction after ~6 minutes -> aliasing, high ringing)
            auto setup = [] (EngineParams& p) { p.room.on = true; p.room.tailGain = 0.0f; p.room.order = 2; };
            OrganEngine fresh, aged;
            {
                aged.prepare (fsr, 4096); aged.setLibrary (lib);
                EngineParams q; q.stopOn = {}; q.clickLevel = 0; q.releaseClick = 0; q.wind = 0; q.stopPull = 0; q.outputMode = 1;
                juce::AudioBuffer<float> b (4, 4096); juce::MidiBuffer none;
                const int blocks = (int) (40.0 * 60.0 * fsr / 4096.0);          // 40 minutes of silence
                for (int i = 0; i < blocks; ++i) aged.process (b, none, q);
            }
            auto a = run (setup, 2.0, -1, "", 4, &fresh);
            auto b = run (setup, 2.0, -1, "41b_room_after_40_minutes", 4, &aged);
            double e = 0, d = 0;
            for (int i = (int) (0.5 * fsr); i < a.getNumSamples(); ++i)
            {
                const double x = a.getSample (0, i), y = b.getSample (0, i);
                e += x * x; d += (x - y) * (x - y);
            }
            const double db = 10.0 * std::log10 (d / juce::jmax (1e-30, e) + 1e-30);
            std::cout << "room: reflections after 40 minutes of running differ from a fresh start by " << db << " dB" << std::endl;
            check (db < -80.0, "room: early reflections unchanged after a long session");
        }

        {   // one strong reflection from the left wall: delay and direction
            auto setup = [&] (bool on) { return [on] (EngineParams& p)
            {
                p.stopGain[0] = 0.0f;                                   // the pipe silent: only its key click at the pipe
                p.fx.custLevel = 1.0f; p.fx.custPlace = 0;
                p.room.on = on; p.room.width = 20; p.room.length = 20; p.room.height = 20; p.room.listenerX = 0.1f; p.room.listenerY = 0.5f;
                p.room.absWalls = 0.0f; p.room.absFloor = 0.99f; p.room.scatter = 0.0f; p.room.order = 1; p.room.tailGain = 0.0f;
            }; };
            OrganEngine probe; probe.prepare (fsr, B);
            // put the pipe straight ahead, 5 m
            OrganEngine eOn, eOff;
            for (auto* e : { &eOn, &eOff }) { e->baseAz[0][(size_t) keyA] = 0.0f; e->baseEl[0][(size_t) keyA] = 0.0f; e->baseDist[0][(size_t) keyA] = 5.0f; }
            auto on = run (setup (true), 0.5, -1, "40_room_left_wall", 4, &eOn);
            auto off = run (setup (false), 0.5, -1, "", 4, &eOff);
            juce::AudioBuffer<float> diff (on);
            for (int c = 0; c < 4; ++c) diff.addFrom (c, 0, off, c, 0, off.getNumSamples(), -1.0f);
            // lag of the reflection: cross-correlation of the reflections with the dry sound (W)
            const float* a = off.getReadPointer (0); const float* d = diff.getReadPointer (0);
            int best = 0; double bv = -1e30;
            for (int lag = 0; lag < 960; ++lag)
            {
                double sum = 0; for (int i = 0; i < 20000; ++i) sum += (double) a[i] * d[i + lag];
                if (sum > bv) { bv = sum; best = lag; }
            }
            const double expectMs = (std::sqrt (4.0 * 4.0 + 5.0 * 5.0) - 5.0) / 343.0 * 1000.0;
            // direction: only the first 20 ms after the reflection arrives (the front wall comes after 29 ms)
            int onset = 0; { float pk = off.getMagnitude (0, 0, off.getNumSamples()); while (onset < off.getNumSamples() && std::abs (a[onset]) < 0.05f * pk) ++onset; }
            double wy = 0, wx = 0;
            for (int i = onset + best - 24; i < onset + best + 960; ++i) { wy += (double) diff.getSample (0, i) * diff.getSample (1, i); wx += (double) diff.getSample (0, i) * diff.getSample (3, i); }
            const double az = juce::radiansToDegrees (std::atan2 (wy, wx));
            const double expectAz = juce::radiansToDegrees (std::atan2 (4.0, 5.0));
            std::cout << "room: left-wall reflection after " << best / 48.0 << " ms (expect " << expectMs << "), from az " << az << " (expect " << expectAz
                      << "), level " << juce::Decibels::gainToDecibels ((float) std::sqrt (diff.getRMSLevel (0, 0, diff.getNumSamples()))) << std::endl;
            check (std::abs (best / 48.0 - expectMs) < 0.15 && std::abs (az - expectAz) < 8.0, "early reflection: right delay and direction");
        }
        {   // late reverb decays with the computed decay time - both models (convolution, FDN), at the same level
            double energy[2] {};
            for (int model = 0; model < 2; ++model)
            {
                OrganEngine e;
                auto b = run ([model] (EngineParams& p)
                              {
                                  p.room.on = true; p.room.width = 12; p.room.length = 24; p.room.height = 10;
                                  p.room.absWalls = 0.08f; p.room.absFloor = 0.15f; p.room.highDamp = 0.3f; p.room.erGain = 0.0f;
                                  p.room.tailModel = model;
                              }, 5.0, 0.6, model == 0 ? "41_room_tail" : "41_room_tail_fdn", 4, &e);
                const float rt = e.roomRtLow.load();
                // decay of the W energy after the note (10 ms frames), fit -5 .. -25 dB
                const int start = (int) (0.75 * fsr), fr = 480;
                std::vector<double> en;
                for (int pos = start; pos + fr < b.getNumSamples(); pos += fr) { double s2 = 0; for (int i = 0; i < fr; ++i) s2 += (double) b.getSample (0, pos + i) * b.getSample (0, pos + i); en.push_back (s2); }
                std::vector<double> sch (en.size()); double acc = 0; for (int i = (int) en.size() - 1; i >= 0; --i) { acc += en[(size_t) i]; sch[(size_t) i] = acc; }
                energy[model] = acc;
                double sx = 0, sy = 0, sxx = 0, sxy = 0; int nn = 0;
                for (size_t i = 0; i < sch.size(); ++i)
                {
                    const double db = 10 * std::log10 (sch[i] / acc);
                    if (db > -5) continue; if (db < -25) break;
                    const double x = i * 0.01; sx += x; sy += db; sxx += x * x; sxy += x * db; ++nn;
                }
                const double slope = (nn * sxy - sx * sy) / (nn * sxx - sx * sx);
                const double measured = -60.0 / slope;
                std::cout << "room: tail (" << (model == 0 ? "convolution" : "FDN") << ") decay measured " << measured << " s, ray-traced " << rt
                          << " s, critical distance " << e.roomCritDist.load() << " m" << std::endl;
                check (nn > 3 && std::abs (measured / rt - 1.0) < 0.25, juce::String ("late reverb decays with the room's decay time (") + (model == 0 ? "convolution)" : "FDN)"));
            }
            const double diffDb = 10.0 * std::log10 (energy[1] / energy[0]);
            std::cout << "room: FDN tail level against the convolution tail: " << diffDb << " dB" << std::endl;
            check (std::abs (diffDb) < 2.0, "room: both late reverb models equally loud");
        }
        {   // FDN: unchanged after a long session as well; switching models while playing rings out without a gap
            auto setup = [] (EngineParams& p) { p.room.on = true; p.room.erGain = 0.0f; p.room.tailModel = 1; };
            OrganEngine fresh, aged;
            {
                aged.prepare (fsr, 4096); aged.setLibrary (lib);
                EngineParams q; q.stopOn = {}; q.clickLevel = 0; q.releaseClick = 0; q.wind = 0; q.stopPull = 0; q.outputMode = 1;
                q.room.on = true; q.room.tailModel = 1;          // the FDN runs (silently) all the time
                juce::AudioBuffer<float> b (4, 4096); juce::MidiBuffer none;
                const int blocks = (int) (12.0 * 60.0 * fsr / 4096.0);
                for (int i = 0; i < blocks; ++i) aged.process (b, none, q);
            }
            auto a = run (setup, 2.0, -1, "", 4, &fresh);
            auto b = run (setup, 2.0, -1, "", 4, &aged);
            double e2 = 0, d2 = 0;
            for (int i = (int) (0.5 * fsr); i < a.getNumSamples(); ++i) { const double x = a.getSample (0, i), y = b.getSample (0, i); e2 += x * x; d2 += (x - y) * (x - y); }
            const double db = 10.0 * std::log10 (d2 / juce::jmax (1e-30, e2) + 1e-30);
            std::cout << "room: FDN after 12 minutes of running differs from a fresh start by " << db << " dB" << std::endl;
            check (db < -60.0, "room: FDN unchanged after a long session");

            auto sw = run ([] (EngineParams& p) { p.room.on = true; p.room.erGain = 0.0f; }, 4.0, 0.5, "41_room_tail_switch", 4);
            (void) sw;
            // switch while the tail rings (note off at 0.5 s, switch at 1.0 s), both ways: no gap, and it keeps decaying
            // (the old one rings out underneath with a silent input - it must not build up)
            for (int from = 0; from < 2; ++from)
            {
                OrganEngine e; e.prepare (fsr, B); e.setLibrary (lib);
                EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0; p.outputMode = 1;
                p.room.on = true; p.room.erGain = 0.0f;
                e.setTailIR (TailIR::build (p.room, p.air, p.outputMode, fsr, 1));
                juce::AudioBuffer<float> blk (4, B), all (4, (int) (fsr * 6.0));
                for (int pos = 0; pos < all.getNumSamples(); pos += B)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0) mb.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 110), 0);
                    if (pos == ((int) (0.5 * fsr) / B) * B) mb.addEvent (juce::MidiMessage::noteOff (1, 69), 0);
                    p.room.tailModel = pos >= (int) (1.0 * fsr) ? 1 - from : from;
                    e.process (blk, mb, p);
                    for (int c = 0; c < 4; ++c) all.copyFrom (c, pos, blk, c, 0, juce::jmin (B, all.getNumSamples() - pos));
                }
                writeWav (outDir.getChildFile (from == 0 ? "41_room_tail_switch_to_fdn.wav" : "41_room_tail_switch_to_conv.wav"), all, fsr);
                auto rmsAt = [&] (double t) { return all.getRMSLevel (0, (int) (t * fsr), (int) (0.1 * fsr)); };
                const double jump = juce::Decibels::gainToDecibels (rmsAt (1.0) / juce::jmax (1e-9f, rmsAt (0.9)));
                const double later = juce::Decibels::gainToDecibels (rmsAt (3.0) / juce::jmax (1e-9f, rmsAt (1.0)));
                const double end = juce::Decibels::gainToDecibels (rmsAt (5.8) / juce::jmax (1e-9f, rmsAt (1.0)));
                std::cout << "room: switching " << (from == 0 ? "convolution -> FDN" : "FDN -> convolution") << " while the tail rings: level change "
                          << jump << " dB, 2 s later " << later << " dB, at 5.8 s " << end << " dB" << std::endl;
                check (std::isfinite (jump) && jump > -3.0 && jump < 6.0, "room: switching the late reverb model while playing has no gap");
                check (later < -10.0 && end < later, "room: after switching the late reverb keeps decaying (no build-up)");
            }
        }
        {   // listening scenes: default room, note on / off, early reflections only / tail only / both (stereo)
            for (int mode = 0; mode < 4; ++mode)
                run ([mode] (EngineParams& p)
                     {
                         p.outputMode = 0; p.clickLevel = 0.2f; p.releaseClick = 0.2f;
                         p.room.on = mode < 3;
                         p.room.width = 14; p.room.length = 28; p.room.height = 11; p.room.absWalls = 0.06f; p.room.absFloor = 0.2f;
                         p.room.highDamp = 0.15f; p.room.scatter = 0.3f; p.room.order = 2;
                         if (mode == 0) p.room.tailGain = 0.0f;
                         if (mode == 1) p.room.erGain = 0.0f;
                     }, 4.0, 1.0, mode == 0 ? "42_room_er_only" : (mode == 1 ? "42_room_tail_only" : (mode == 2 ? "42_room_both" : "42_room_dry")), 2);
        }
        {   // impulse responses of the default room (a key click at the pipe), tail only / reflections only
            for (int mode = 0; mode < 3; ++mode)
                run ([mode] (EngineParams& p)
                     {
                         p.outputMode = 0; p.stopGain[0] = 0.0f; p.fx.custLevel = 1.0f; p.fx.custPlace = 0;
                         p.room.on = mode < 2;
                         p.room.width = 14; p.room.length = 28; p.room.height = 11; p.room.absWalls = 0.06f; p.room.absFloor = 0.2f;
                         p.room.highDamp = 0.15f; p.room.scatter = 0.3f; p.room.order = 2;
                         if (mode == 0) p.room.erGain = 0.0f; else p.room.tailGain = 0.0f;
                     }, 4.0, -1, mode == 0 ? "43_room_click_tail" : (mode == 1 ? "43_room_click_er" : "43_room_click_dry"), 2);
        }
        {   // the tail's response to a pure impulse (an "own sound" that is one sample): dense, no ringing modes
            auto lib2 = std::make_shared<SampleLibrary> (*lib);
            auto imp = std::make_shared<Sample>();
            imp->sampleRate = 48000.0;
            imp->data.assign (64, 0.0f);
            imp->data[2] = 1.0f;
            NoiseSample ns; ns.sample = imp; ns.norm = 1.0f;
            lib2->custom = { ns };
            std::shared_ptr<const SampleLibrary> lib2c (lib2);
            OrganEngine e; e.prepare (fsr, B); e.setLibrary (lib2c);
            EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.stopGain[0] = 0.0f; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0;
            p.outputMode = 0; p.fx.custLevel = 1.0f; p.fx.custPlace = 0;
            p.room.on = true; p.room.width = 14; p.room.length = 28; p.room.height = 11; p.room.absWalls = 0.06f; p.room.absFloor = 0.2f;
            p.room.highDamp = 0.15f; p.room.scatter = 0.3f; p.room.order = 2; p.room.erGain = 0.0f;
            e.setTailIR (TailIR::build (p.room, p.air, 0, fsr, 1));
            juce::AudioBuffer<float> b (2, B), all (2, (int) (fsr * 2.0));
            for (int pos = 0; pos < all.getNumSamples(); pos += B)
            {
                juce::MidiBuffer mb;
                if (pos == 0) mb.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 127), 0);
                e.process (b, mb, p);
                for (int c = 0; c < 2; ++c) all.copyFrom (c, pos, b, c, 0, juce::jmin (B, all.getNumSamples() - pos));
            }
            writeWav (outDir.getChildFile ("44_room_impulse_tail.wav"), all, fsr);
            // ringing = narrow spectral peaks far above their neighbourhood. Dense decaying noise: the top 0.1 % of
            // the bins lie ~10-12 dB above the local median; the earlier Hadamard network without modulation: ~22 dB
            const int a0 = (int) (0.3 * fsr), len = 65536;
            juce::dsp::FFT fft (16);
            std::vector<float> w ((size_t) (2 * len), 0.0f);
            for (int i = 0; i < len && a0 + i < all.getNumSamples(); ++i)
                w[(size_t) i] = 0.5f * (all.getSample (0, a0 + i) + all.getSample (1, a0 + i)) * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) len));
            fft.performFrequencyOnlyForwardTransform (w.data(), true);
            std::vector<double> db ((size_t) (len / 2));
            for (int k = 0; k < len / 2; ++k) db[(size_t) k] = 20.0 * std::log10 (w[(size_t) k] + 1e-12);
            std::vector<double> ex;
            const int k0 = (int) (500.0 * len / fsr), k1 = (int) (8000.0 * len / fsr);
            for (int k = k0; k < k1; ++k)
            {
                std::vector<double> nb (db.begin() + (k - 200), db.begin() + (k + 201));
                std::nth_element (nb.begin(), nb.begin() + 200, nb.end());
                ex.push_back (db[(size_t) k] - nb[200]);
            }
            std::sort (ex.begin(), ex.end());
            const double p999 = ex[(size_t) (0.999 * (double) (ex.size() - 1))];
            std::cout << "room: tail impulse response, peaks over local median (99.9 %): " << p999 << " dB" << std::endl;
            check (p999 < 14.0, "late reverb is dense (no ringing modes)");
        }
        {   // the room changes while a note sounds: the new response is crossfaded from the same history - no gap
            auto render = [&] (bool swap)
            {
                OrganEngine e; e.prepare (fsr, B); e.setLibrary (lib);
                EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0; p.outputMode = 0;
                p.room.on = true; p.room.erGain = 0.0f;
                e.setTailIR (TailIR::build (p.room, p.air, 0, fsr, 1));
                auto changed = p.room; changed.absWalls *= 1.02f;               // a small change: the level must stay
                auto nextIr = TailIR::build (changed, p.air, 0, fsr, 2);
                juce::AudioBuffer<float> b (2, B), all (2, (int) (fsr * 2.5));
                for (int pos = 0; pos < all.getNumSamples(); pos += B)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0) mb.addEvent (juce::MidiMessage::noteOn (1, 57, (juce::uint8) 100), 0);
                    if (swap && pos == B * 94) e.setTailIR (nextIr);         // ~1 s
                    e.process (b, mb, p);
                    for (int c = 0; c < 2; ++c) all.copyFrom (c, pos, b, c, 0, juce::jmin (B, all.getNumSamples() - pos));
                }
                return std::make_pair (all, e.tailSerialInUse.load());
            };
            auto [a1, s1] = render (false);
            auto [a2, s2] = render (true);
            writeWav (outDir.getChildFile ("45_room_tail_swap.wav"), a2, fsr);
            double worst = 0;
            for (double t = 0.8; t < 2.3; t += 0.02)
            {
                const double l1 = juce::Decibels::gainToDecibels (a1.getRMSLevel (0, (int) (t * fsr), (int) (0.02 * fsr)));
                const double l2 = juce::Decibels::gainToDecibels (a2.getRMSLevel (0, (int) (t * fsr), (int) (0.02 * fsr)));
                worst = juce::jmax (worst, std::abs (l1 - l2));
            }
            std::cout << "room: response swapped while playing: largest level difference to no swap " << worst << " dB (in use: " << s2 << ")" << std::endl;
            check (worst < 1.0 && s2 == 2 && s1 == 1, "room change: the reverb turns into the new room without a gap");
        }
        {   // CPU: a chord on 3 stops (18 pipes) and on all 8 stops (48 pipes), reflections up to 2nd / 3rd order, 3rd-order output
            for (int stops : { 3, 8 })
                for (int ord : { 2, 3 })
                {
                    OrganEngine e; e.prepare (fsr, B); e.setLibrary (lib);
                    EngineParams p; p.stopOn = {}; for (int st = 0; st < stops; ++st) p.stopOn[(size_t) st] = true;
                    p.outputMode = 3; p.room.on = true; p.room.order = ord;
                    e.setTailIR (TailIR::build (p.room, p.air, 3, fsr, 1));
                    juce::AudioBuffer<float> b (16, B);
                    const int blocks = (int) (6.0 * fsr / B);
                    auto t0 = std::chrono::high_resolution_clock::now();
                    for (int i = 0; i < blocks; ++i)
                    {
                        juce::MidiBuffer mb;
                        if (i == 0) for (int m : { 48, 55, 60, 64, 67, 72 }) mb.addEvent (juce::MidiMessage::noteOn (1, m, (juce::uint8) 90), 0);
                        e.process (b, mb, p);
                    }
                    const double rt = 6.0 / std::chrono::duration<double> (std::chrono::high_resolution_clock::now() - t0).count();
                    std::cout << "room on, reflections up to order " << ord << ", " << stops * 6 << " pipes (" << e.erActive.load()
                              << " with own reflections), 3rd-order out: " << rt << " x real time" << std::endl;
                    if (stops == 3 && ord == 2) check (rt > 2.0, "room simulation runs in real time (18 pipes)");
                }
        }
    }

    std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string (failures)) << std::endl;
    return failures;
}
