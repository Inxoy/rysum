// Offline test: loads the sample folder, renders a few MIDI scenes through the engine
// in stereo and in AmbiX 5th order, writes WAV files and checks the results.
//   RysumRenderTest <samples folder> <output folder>
#include "Binaural.h"
#include "Convolver.h"
#include "Lucier.h"
#include "LucierPiece.h"
#include "OrganEngine.h"
#include "TimeDistance.h"
#include "RoomSim.h"
#include "RoomTail.h"
#include "Scala.h"
#include "Speakers.h"
#include "Transaural.h"
#include <chrono>
#include <complex>
#include <iostream>
#include <juce_audio_formats/juce_audio_formats.h>

using namespace rysum;

static int failures = 0;
static void check(bool ok, const juce::String &what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
    if (!ok)
        ++failures;
}

static void testEncoder()
{
    std::cout << "encoder" << std::endl;
    float g[kMaxAmbiChannels];
    encodeAmbiX(90.0f, 0.0f, 1, g); // left
    check(std::abs(g[0] - 1) < 1e-5 && std::abs(g[1] - 1) < 1e-5 && std::abs(g[2]) < 1e-5 &&
              std::abs(g[3]) < 1e-5,
          "order 1, left: W=1 Y=1 Z=0 X=0");
    encodeAmbiX(0.0f, 90.0f, 1, g); // up
    check(std::abs(g[2] - 1) < 1e-5 && std::abs(g[1]) < 1e-5, "order 1, zenith: Z=1");
    encodeAmbiX(30.0f, 20.0f, 2, g);
    const double az = juce::degreesToRadians(30.0), el = juce::degreesToRadians(20.0);
    check(std::abs(g[4] - std::sqrt(3.0) / 2 * std::cos(el) * std::cos(el) * std::sin(2 * az)) <
              1e-5,
          "ACN4 = sqrt(3)/2 cos^2(el) sin(2az)");
    check(std::abs(g[6] - 0.5 * (3 * std::sin(el) * std::sin(el) - 1)) < 1e-5,
          "ACN6 = (3 sin^2 el - 1)/2");

    // SN3D orthogonality: mean over the sphere of Y_i*Y_j = delta_ij / (2l+1)
    const int N = 200;
    double M[36][36]{};
    double wsum = 0;
    for (int a = 0; a < N; ++a)
        for (int b = 0; b < N; ++b)
        {
            const float azd =
                -180.0f + 360.0f * (static_cast<float>(a) + 0.5f) / static_cast<float>(N);
            const double u = -1.0 + 2.0 * (b + 0.5) / N; // sin(el) uniform -> equal area
            const float eld = (float)juce::radiansToDegrees(std::asin(u));
            encodeAmbiX(azd, eld, 5, g);
            for (int i = 0; i < 36; ++i)
                for (int j = 0; j < 36; ++j)
                    M[i][j] += (double)g[i] * g[j];
            wsum += 1;
        }
    double maxErr = 0;
    for (int i = 0; i < 36; ++i)
        for (int j = 0; j < 36; ++j)
        {
            const int l = (int)std::floor(std::sqrt((double)i));
            const double expect = i == j ? 1.0 / (2 * l + 1) : 0.0;
            maxErr = std::max(maxErr, std::abs(M[i][j] / wsum - expect));
        }
    check(maxErr < 2e-3,
          "SN3D orthogonality up to order 5 (max err " + juce::String(maxErr, 5) + ")");

    float s[2];
    encodeStereo(90.0f, s);
    check(s[0] > 0.999f && s[1] < 0.001f, "stereo: +90 = left");
    encodeStereo(0.0f, s);
    check(std::abs(s[0] - s[1]) < 1e-5 && std::abs(s[0] * s[0] + s[1] * s[1] - 1) < 1e-5,
          "stereo: centre equal power");
}

static void writeWav(const juce::File &f, const juce::AudioBuffer<float> &b, double sr);

// ---------------------------------------------------------------------------
// binaural: HRIR sets, MagLS filters, real-time decoder
// ---------------------------------------------------------------------------
static double bandDb(const std::vector<float> &x, double fs, double f0, double f1)
{
    const int n = 4096;
    juce::dsp::FFT fft(12);
    std::vector<float> b((size_t)(2 * n), 0.0f);
    for (int i = 0; i < juce::jmin((int)x.size(), n); ++i)
        b[(size_t)i] = x[(size_t)i];
    fft.performFrequencyOnlyForwardTransform(b.data(), true);
    double e = 0;
    int cnt = 0;
    for (int k = 1; k < n / 2; ++k)
    {
        const double f = k * fs / n;
        if (f >= f0 && f < f1)
        {
            e += (double)b[(size_t)k] * b[(size_t)k];
            ++cnt;
        }
    }
    return 10.0 * std::log10(juce::jmax(1e-30, e / juce::jmax(1, cnt)));
}

static double itdMs(const std::vector<float> &l, const std::vector<float> &r, double fs)
{
    // interaural phase delay, averaged over 200..500 Hz (no phase ambiguity for |ITD| < 1 ms)
    double sum = 0;
    int cnt = 0;
    for (double f = 200; f <= 500; f += 25)
    {
        std::complex<double> L, R;
        for (size_t i = 0; i < l.size(); ++i)
            L += (double)l[i] *
                 std::polar(1.0, -2.0 * juce::MathConstants<double>::pi * f * (double)i / fs);
        for (size_t i = 0; i < r.size(); ++i)
            R += (double)r[i] *
                 std::polar(1.0, -2.0 * juce::MathConstants<double>::pi * f * (double)i / fs);
        sum += std::arg(R / L) / (2.0 * juce::MathConstants<double>::pi * f);
        ++cnt;
    }
    return 1000.0 * sum / cnt;
}

[[maybe_unused]] static double itdXcorrMs(const std::vector<float> &l, const std::vector<float> &r,
                                          double fs)
{
    // cross-correlation of the low-passed ears (< 700 Hz: no ambiguity within +-1 ms); positive =
    // left ear first
    auto lp = [&](std::vector<float> x)
    {
        juce::IIRFilter f;
        f.setCoefficients(juce::IIRCoefficients::makeLowPass(fs, 700.0));
        f.processSamples(x.data(), (int)x.size());
        return x;
    };
    const auto a = lp(l), b = lp(r);
    const int maxLag = (int)(0.0012 * fs);
    int best = 0;
    double bv = -1e30;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double s = 0;
        for (int i = 0; i < (int)a.size(); ++i)
        {
            const int j = i + lag;
            if (j >= 0 && j < (int)b.size())
                s += (double)a[(size_t)i] * b[(size_t)j];
        }
        if (s > bv)
        {
            bv = s;
            best = lag;
        }
    }
    return 1000.0 * best / fs;
}

static void testBinaural(const juce::File &sofa)
{
    juce::String loadError;
    auto fab = HrirSet::fabian(loadError);
    check(fab != nullptr && fab->numDirections() > 1900,
          "FABIAN set built in (" + juce::String(fab != nullptr ? fab->numDirections() : 0) +
              " directions) " + loadError);
    if (fab == nullptr)
        return;

    std::unique_ptr<HrirSet> full;
    if (sofa.existsAsFile())
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        full = HrirSet::fromSofa(sofa, loadError);
        const double secs =
            std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
        check(full != nullptr && full->numDirections() == 11950 && full->length == 279,
              "SOFA read with libmysofa (" + juce::String(secs, 2) + " s) " + loadError);
        if (full != nullptr)
        {
            // the same direction in both: identical data (up to the 16-bit storage)
            int a = 0;
            double bestA = 1e9;
            for (int q = 0; q < fab->numDirections(); ++q)
            {
                const double dd =
                    std::abs(fab->az[(size_t)q] - 90.0f) + std::abs(fab->el[(size_t)q]);
                if (dd < bestA)
                {
                    bestA = dd;
                    a = q;
                }
            }
            std::cout << "comparing direction " << fab->az[(size_t)a] << " / " << fab->el[(size_t)a]
                      << std::endl;
            int b = -1;
            for (int q = 0; q < full->numDirections(); ++q)
                if (std::abs(full->az[(size_t)q] - fab->az[(size_t)a]) < 0.01f &&
                    std::abs(full->el[(size_t)q] - fab->el[(size_t)a]) < 0.01f)
                    b = q;
            double d = 0, e = 0;
            if (b >= 0)
                for (int i = 0; i < 240; ++i)
                {
                    const double x = full->get(b, 0)[i], y = fab->get(a, 0)[i];
                    d += (x - y) * (x - y);
                    e += x * x;
                }
            check(b >= 0 && d / e < 1e-6, "built-in FABIAN = SOFA file");
            // left source: left ear louder (coordinate convention of the SOFA reader)
            double el = 0, er = 0;
            for (int i = 0; i < full->length; ++i)
            {
                el += full->get(b, 0)[i] * full->get(b, 0)[i];
                er += full->get(b, 1)[i] * full->get(b, 1)[i];
            }
            check(b >= 0 && el > 4 * er, "SOFA: +90 deg is on the left");
        }
    }
    { // broken file
        auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("rysum_bad.sofa");
        tmp.replaceWithText("not a sofa file");
        auto bad = HrirSet::fromSofa(tmp, loadError);
        check(bad == nullptr && loadError.isNotEmpty(), "broken SOFA rejected: " + loadError);
        tmp.deleteFile();
    }

    const auto &ref = full != nullptr ? *full : *fab;
    auto nearest = [&](float az, float el)
    {
        int best = 0;
        double bd = 1e9;
        for (int q = 0; q < ref.numDirections(); ++q)
        {
            const double d = std::abs(wrapAz(ref.az[(size_t)q] - az)) *
                                 std::cos(juce::degreesToRadians((double)el)) +
                             std::abs(ref.el[(size_t)q] - el);
            if (d < bd)
            {
                bd = d;
                best = q;
            }
        }
        return best;
    };
    auto render = [](const BinauralFilters &f, float az, float el, int ear)
    {
        float g[kMaxAmbiChannels]{};
        encodeAmbiX(az, el, f.order, g);
        std::vector<float> y((size_t)f.length, 0.0f);
        for (int c = 0; c < f.numChannels; ++c)
            for (int i = 0; i < f.length; ++i)
                y[(size_t)i] += g[c] * f.get(ear, c)[i];
        return y;
    };
    const float dirsTest[][2]{{0, 0},   {30, 0},  {90, 0},     {135, 0}, {-60, 0},
                              {180, 0}, {45, 40}, {-120, -30}, {0, 70}};
    const double bands[]{250, 500, 1000, 2000, 4000, 8000, 16000};

    for (int order : {1, 3, 5})
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        auto f = BinauralFilters::design(*fab, order, 48000.0, false);
        const double secs =
            std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
        check(f != nullptr,
              "MagLS design order " + juce::String(order) + " (" + juce::String(secs, 2) + " s)");
        if (f == nullptr)
            continue;
        // compare with the measured HRIRs: band levels (after removing the overall gain) and ITD
        std::vector<double> diffs;
        double worstItd = 0, worstIld = 0;
        for (auto &d : dirsTest)
        {
            const int q = nearest(d[0], d[1]);
            std::vector<float> hl(ref.get(q, 0), ref.get(q, 0) + ref.length),
                hr(ref.get(q, 1), ref.get(q, 1) + ref.length);
            const auto yl = render(*f, ref.az[(size_t)q], ref.el[(size_t)q], 0),
                       yr = render(*f, ref.az[(size_t)q], ref.el[(size_t)q], 1);
            for (int b = 0; b + 1 < 7; ++b)
            {
                diffs.push_back(bandDb(yl, 48000, bands[b], bands[b + 1]) -
                                bandDb(hl, 48000, bands[b], bands[b + 1]));
                diffs.push_back(bandDb(yr, 48000, bands[b], bands[b + 1]) -
                                bandDb(hr, 48000, bands[b], bands[b + 1]));
            }
            worstItd = juce::jmax(worstItd, std::abs(itdMs(yl, yr, 48000) - itdMs(hl, hr, 48000)));
            const double ildH = bandDb(hl, 48000, 2000, 12000) - bandDb(hr, 48000, 2000, 12000);
            const double ildY = bandDb(yl, 48000, 2000, 12000) - bandDb(yr, 48000, 2000, 12000);
            worstIld = juce::jmax(worstIld, std::abs(ildH - ildY));
        }
        double mean = 0;
        for (auto x : diffs)
            mean += x;
        mean /= (double)diffs.size();
        double rms = 0;
        for (auto x : diffs)
            rms += (x - mean) * (x - mean);
        rms = std::sqrt(rms / (double)diffs.size());
        std::cout << "binaural order " << order << ": band error " << rms << " dB rms (gain "
                  << mean << " dB), worst ITD error " << worstItd * 1000.0
                  << " us, worst ILD error " << worstIld << " dB" << std::endl;
        if (order == 5)
            check(rms < 1.5 && worstItd < 0.03 && worstIld < 5.0,
                  "order 5 matches the measured HRTFs");
        if (order == 3)
            check(rms < 1.5 && worstItd < 0.04, "order 3 close to the measured HRTFs");
        if (order == 1)
            check(rms < 3.5 && worstItd < 0.1, "order 1: ITDs right");
    }

    // real-time decoder = direct convolution, 128 samples later; head rotation
    {
        auto f = BinauralFilters::design(*fab, 3, 48000.0, true);
        check(f != nullptr, "design with diffuse-field EQ");
        BinauralDecoder dec(*f);
        const int n = 4096, K = f->numChannels;
        std::vector<std::vector<float>> in((size_t)K, std::vector<float>((size_t)n, 0.0f));
        float g[kMaxAmbiChannels]{};
        encodeAmbiX(60.0f, 10.0f, 3, g);
        juce::Random rnd(3);
        std::vector<float> src((size_t)n);
        for (auto &x : src)
            x = rnd.nextFloat() * 2.0f - 1.0f;
        for (int c = 0; c < K; ++c)
            for (int i = 0; i < n; ++i)
                in[(size_t)c][(size_t)i] = src[(size_t)i] * g[c];
        std::vector<float> L((size_t)n), R((size_t)n);
        std::vector<float *> ptr;
        for (auto &v : in)
            ptr.push_back(v.data());
        for (int start = 0; start < n; start += 100) // odd block size
        {
            const int m = juce::jmin(100, n - start);
            std::vector<float *> p2;
            for (auto *q : ptr)
                p2.push_back(q + start);
            dec.process(p2.data(), m, L.data() + start, R.data() + start, 0.0f);
        }
        double err = 0, en = 0;
        for (int i = 1000; i < n; ++i)
        {
            double y = 0;
            for (int c = 0; c < K; ++c)
                for (int j = 0; j < f->length; ++j)
                {
                    const int s = i - BinauralDecoder::kBlock - j;
                    if (s >= 0)
                        y += (double)f->get(0, c)[j] * src[(size_t)s] * g[c];
                }
            err += (y - L[(size_t)i]) * (y - L[(size_t)i]);
            en += y * y;
        }
        std::cout << "binaural decoder vs direct convolution: " << 10 * std::log10(err / en)
                  << " dB" << std::endl;
        check(err / en < 1e-8, "decoder = convolution with the filters (128 samples latency)");

        // a source in front, head turned 90 deg to the left: it is now on the right
        auto turned = [&](float yaw)
        {
            BinauralDecoder d2(*f);
            std::vector<std::vector<float>> x((size_t)K, std::vector<float>((size_t)n, 0.0f));
            float gg[kMaxAmbiChannels]{};
            encodeAmbiX(0.0f, 0.0f, 3, gg);
            for (int c = 0; c < K; ++c)
                for (int i = 0; i < n; ++i)
                    x[(size_t)c][(size_t)i] = src[(size_t)i] * gg[c];
            std::vector<float *> p;
            for (auto &v : x)
                p.push_back(v.data());
            std::vector<float> l((size_t)n), r((size_t)n);
            for (int start = 0; start < n; start += 256)
            {
                std::vector<float *> p2;
                for (auto *q : p)
                    p2.push_back(q + start);
                d2.process(p2.data(), 256, l.data() + start, r.data() + start, yaw);
            }
            double el = 0, er = 0;
            for (int i = 1024; i < n; ++i)
            {
                el += l[(size_t)i] * l[(size_t)i];
                er += r[(size_t)i] * r[(size_t)i];
            }
            return 10.0 * std::log10(el / er);
        };
        const double ild0 = turned(0.0f), ild90 = turned(90.0f);
        std::cout << "head rotation: front L/R " << ild0
                  << " dB, head turned left 90 deg: " << ild90 << " dB" << std::endl;
        check(std::abs(ild0) < 2.0 && ild90 < -6.0, "head rotation turns the scene");

        // level: diffuse noise (random directions) about as loud per ear as an equal-power stereo
        // pan
        double eOut = 0, eIn = 0;
        BinauralDecoder d3(*f);
        std::vector<std::vector<float>> x((size_t)K, std::vector<float>((size_t)n, 0.0f));
        for (int s = 0; s < 64; ++s)
        {
            float gg[kMaxAmbiChannels]{};
            encodeAmbiX(360.0f * rnd.nextFloat() - 180.0f,
                        juce::radiansToDegrees(std::asin(2.0f * rnd.nextFloat() - 1.0f)), 3, gg);
            for (int i = 0; i < n; ++i)
            {
                const float v = (rnd.nextFloat() * 2.0f - 1.0f) * 0.125f;
                eIn += v * v;
                for (int c = 0; c < K; ++c)
                    x[(size_t)c][(size_t)i] += v * gg[c];
            }
        }
        std::vector<float *> p;
        for (auto &v : x)
            p.push_back(v.data());
        d3.process(p.data(), n, L.data(), R.data(), 0.0f);
        for (int i = 1024; i < n; ++i)
            eOut +=
                0.5 * ((double)L[(size_t)i] * L[(size_t)i] + (double)R[(size_t)i] * R[(size_t)i]);
        const double lvl = 10.0 * std::log10(eOut / (eIn * (n - 1024) / n));
        std::cout << "binaural level of a diffuse field per ear: " << lvl
                  << " dB (stereo pan: -3 dB)" << std::endl;
        check(std::abs(lvl + 3.0) < 2.5, "binaural level like stereo");
    }

    // other host rates: same sound
    {
        auto a = BinauralFilters::design(*fab, 3, 48000.0, false),
             b = BinauralFilters::design(*fab, 3, 44100.0, false),
             c = BinauralFilters::design(*fab, 3, 96000.0, false);
        check(a && b && c, "design at 44.1 / 96 kHz");
        if (a && b && c)
        {
            const auto ya = render(*a, 30, 0, 0), yb = render(*b, 30, 0, 0),
                       yc = render(*c, 30, 0, 0);
            double worst = 0;
            for (double fr : {300.0, 1000.0, 3000.0, 8000.0})
            {
                const double la = bandDb(ya, 48000, fr, fr * 1.5),
                             lb = bandDb(yb, 44100, fr, fr * 1.5),
                             lc = bandDb(yc, 96000, fr, fr * 1.5);
                worst = juce::jmax(worst, std::abs(lb - la), std::abs(lc - la));
            }
            std::cout << "binaural 44.1 / 96 kHz vs 48 kHz: worst band difference " << worst
                      << " dB" << std::endl;
            check(worst < 1.0, "resampled filters sound the same");
        }
    }

    // CPU: 5th order, 36 channels
    {
        auto f = BinauralFilters::design(*fab, 5, 48000.0, true);
        BinauralDecoder dec(*f);
        const int n = 512;
        std::vector<std::vector<float>> x(36, std::vector<float>((size_t)n, 0.1f));
        std::vector<float *> p;
        for (auto &v : x)
            p.push_back(v.data());
        std::vector<float> l((size_t)n), r((size_t)n);
        auto t0 = std::chrono::high_resolution_clock::now();
        const int blocks = 48000 * 10 / n;
        for (int i = 0; i < blocks; ++i)
            dec.process(p.data(), n, l.data(), r.data(), 20.0f * (float)(i % 7));
        const double rt =
            10.0 /
            std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
        std::cout << "binaural decoder 5th order: " << rt << " x real time" << std::endl;
        check(rt > 8.0, "binaural decoder is cheap");
    }
}

// transaural: binaural over two loudspeakers - the crosstalk to the far ear cancelled
static void testTransaural()
{
    juce::String err;
    auto fab = HrirSet::fabian(err);
    if (fab == nullptr)
    {
        check(false, "transaural: FABIAN " + err);
        return;
    }
    const double fs = 48000.0;
    // the acoustic way: speaker feeds -> ears (FABIAN at +-angle), by direct convolution
    auto conv = [](const std::vector<float> &x, const float *h, int hl)
    {
        std::vector<float> y(x.size() + (size_t)hl, 0.0f);
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i] != 0.0f)
                for (int k = 0; k < hl; ++k)
                    y[i + (size_t)k] += x[i] * h[k];
        return y;
    };
    auto nearest = [&](float az)
    {
        int best = 0;
        float bd = 1e9f;
        for (int d = 0; d < fab->numDirections(); ++d)
        {
            const float da = std::abs(std::remainder(fab->az[(size_t)d] - az, 360.0f)),
                        de = std::abs(fab->el[(size_t)d]);
            if (da + de < bd)
            {
                bd = da + de;
                best = d;
            }
        }
        return best;
    };
    auto band = [&](std::vector<float> x, double lo, double hi)
    {
        juce::IIRFilter a, b;
        a.setCoefficients(juce::IIRCoefficients::makeHighPass(fs, lo));
        b.setCoefficients(juce::IIRCoefficients::makeLowPass(fs, hi));
        double e = 0;
        for (auto v : x)
        {
            const double y = b.processSingleSampleRaw(a.processSingleSampleRaw(v));
            e += y * y;
        }
        return e;
    };
    for (float angle : {90.0f, 60.0f, 30.0f})
    {
        auto run = [&](float amount, double &farOverNearDb, double &nearDbRel)
        {
            auto f = TransauralFilters::design(*fab, angle, amount, 10.0f, false, fs);
            // a click meant only for the left ear (binaural L = impulse, R = 0)
            std::vector<float> L((size_t)4096, 0.0f), R((size_t)4096, 0.0f);
            L[0] = 1.0f;
            std::vector<float> mid((size_t)4096), side((size_t)4096);
            for (size_t i = 0; i < L.size(); ++i)
            {
                mid[i] = 0.5f * (L[i] + R[i]);
                side[i] = 0.5f * (L[i] - R[i]);
            }
            const auto m = conv(mid, f->cs.data(), f->length),
                       sd = conv(side, f->cd.data(), f->length);
            std::vector<float> spL(m.size()), spR(m.size());
            for (size_t i = 0; i < m.size(); ++i)
            {
                spL[i] = m[i] + sd[i];
                spR[i] = m[i] - sd[i];
            }
            const int dl = nearest(angle), dr = nearest(-angle);
            const int hl = fab->length;
            auto add = [](std::vector<float> a, const std::vector<float> &b)
            {
                for (size_t i = 0; i < a.size() && i < b.size(); ++i)
                    a[i] += b[i];
                return a;
            };
            const auto earL = add(conv(spL, fab->get(dl, 0), hl), conv(spR, fab->get(dr, 0), hl));
            const auto earR = add(conv(spL, fab->get(dl, 1), hl), conv(spR, fab->get(dr, 1), hl));
            const double nearE = band(earL, 250.0, 6000.0), farE = band(earR, 250.0, 6000.0);
            farOverNearDb = 10.0 * std::log10(farE / juce::jmax(1e-30, nearE));
            nearDbRel = 10.0 * std::log10(nearE);
            return f;
        };
        double plainX, plainN, ctcX, ctcN;
        auto f0 = run(0.0f, plainX, plainN);
        auto f1 = run(1.0f, ctcX, ctcN);
        std::cout << "transaural +-" << angle << " deg: far ear / near ear " << plainX
                  << " dB plain, " << ctcX << " dB cancelled (design says " << f1->crossDb
                  << " dB); ITD " << f1->itdMs << " ms; near ear level change " << (ctcN - plainN)
                  << " dB" << std::endl;
        check(std::isfinite(ctcX) && ctcX < plainX - (angle >= 60.0f ? 10.0 : 6.0),
              juce::String("transaural: crosstalk cancelled at +-") + juce::String(angle, 0) +
                  " deg");
        // amount 0 = a pure delay (the plain binaural signal over the speakers)
        if (angle == 90.0f)
        {
            double off = 0;
            for (int i = 0; i < f0->length; ++i)
                if (i != f0->delay)
                    off = juce::jmax(off, (double)std::abs(f0->cs[(size_t)i]) +
                                              std::abs(f0->cd[(size_t)i]));
            std::cout << "transaural amount 0: tap at " << f0->delay << " = "
                      << f0->cs[(size_t)f0->delay] << " / " << f0->cd[(size_t)f0->delay]
                      << ", elsewhere at most " << off << std::endl;
            check(std::abs(f0->cs[(size_t)f0->delay] - 1.0f) < 1e-3f &&
                      std::abs(f0->cd[(size_t)f0->delay] - 1.0f) < 1e-3f && off < 1e-3,
                  "transaural: amount 0 = the binaural signal, only delayed");
            // the boost limit holds (side filter, the low end)
            for (float lim : {6.0f, 15.0f})
            {
                auto fl = TransauralFilters::design(*fab, 90.0f, 1.0f, lim, false, fs);
                juce::dsp::FFT fft(13);
                std::vector<float> b((size_t)16384, 0.0f);
                std::copy(fl->cd.begin(), fl->cd.end(), b.begin());
                fft.performFrequencyOnlyForwardTransform(b.data());
                float mx = 0;
                for (int k = 0; k <= 4096; ++k)
                    mx = juce::jmax(mx, b[(size_t)k]);
                std::cout << "transaural limit " << lim << " dB: side filter peaks at "
                          << juce::Decibels::gainToDecibels(mx) << " dB" << std::endl;
                check(juce::Decibels::gainToDecibels(mx) < lim + 1.5f,
                      juce::String("transaural: boost limited to ") + juce::String(lim, 0) + " dB");
            }
            // real time: the convolutions run (filters loaded in the background), output finite and
            // the click comes through
            auto ff = TransauralFilters::design(*fab, 90.0f, 1.0f, 10.0f, true, fs);
            TransauralCtc ctc;
            ctc.prepare(fs, 512);
            ctc.load(*ff);
            std::vector<float> l(512), r(512);
            double e = 0;
            bool fin = true;
            for (int b = 0; b < 400; ++b)
            {
                std::fill(l.begin(), l.end(), 0.0f);
                std::fill(r.begin(), r.end(), 0.0f);
                if (b % 50 == 0)
                    l[0] = 1.0f;
                ctc.process(l.data(), r.data(), 512);
                for (int i = 0; i < 512; ++i)
                {
                    e += l[(size_t)i] * l[(size_t)i];
                    fin = fin && std::isfinite(l[(size_t)i]) && std::isfinite(r[(size_t)i]);
                }
                if (b % 50 == 0)
                    juce::Thread::sleep(20);
            }
            std::cout << "transaural real time: " << ff->description << ", energy out " << e
                      << std::endl;
            check(fin && e > 1e-3, "transaural: real-time convolution runs");
        }
    }
}

static void testConvolver()
{
    std::cout << "convolver" << std::endl;
    const double sr = 48000.0;
    const int L = (int)(0.7 * sr);
    XorShift r(99);
    // synthetic first-order IR: direct sound from 60 deg left at sample 100, then a decaying noise
    // tail
    juce::AudioBuffer<float> ir(4, L);
    ir.clear();
    const float az = juce::degreesToRadians(60.0f);
    ir.setSample(0, 100, 1.0f);
    ir.setSample(1, 100, std::sin(az));
    ir.setSample(3, 100, std::cos(az));
    for (int c = 0; c < 4; ++c)
        for (int i = 300; i < L; ++i)
            ir.setSample(c, i,
                         r.bi() * 0.25f * std::exp(-6.0f * (float)i / (float)L) *
                             (c == 0 ? 1.0f : 0.5f));
    auto f =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("rysum_test_ir.wav");
    writeWav(f, ir, sr);

    auto run = [&](Convolver &cv, std::function<void(int, std::vector<std::vector<float>> &)> fill,
                   int nIn, int numOut, int N)
    {
        juce::AudioBuffer<float> out(numOut, N);
        out.clear();
        cv.reset();
        int pos = 0, bs = 37;
        while (pos < N)
        {
            const int n = juce::jmin(bs, N - pos);
            std::vector<std::vector<float>> ins((size_t)nIn, std::vector<float>((size_t)n, 0.0f));
            for (int i = 0; i < n; ++i)
            {
                std::vector<std::vector<float>> one((size_t)nIn, std::vector<float>(1));
                fill(pos + i, one);
                for (int k = 0; k < nIn; ++k)
                    ins[(size_t)k][(size_t)i] = one[(size_t)k][0];
            }
            std::vector<const float *> ip;
            for (auto &v : ins)
                ip.push_back(v.data());
            std::vector<float *> o;
            for (int c = 0; c < numOut; ++c)
                o.push_back(out.getWritePointer(c, pos));
            cv.process(ip.data(), n, o.data(), numOut, 1.0f);
            pos += n;
            bs = bs == 37 ? 501 : (bs == 501 ? 1024 : 37);
        }
        return out;
    };

    juce::String err;
    Convolver::Settings st;
    st.outOrder = 1;
    st.direct = 1.0f;
    st.envelop = 0.0f;
    st.hostRate = sr;
    auto cv = Convolver::create(f, st, err);
    check(cv != nullptr, "IR loaded " + err);
    if (!cv)
        return;
    std::cout << "  " << cv->getInfo().description << ", direct sound at "
              << cv->getInfo().baseAzDeg << " deg" << std::endl;
    check(std::abs(cv->getInfo().baseAzDeg - 60.0f) < 1.0f,
          "direction of the IR's direct sound found");

    // 1) exactness: all 'pipes' at the measured direction (= not following) must give plain
    // convolution
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(f));
    juce::AudioBuffer<float> ref(4, L);
    rd->read(&ref, 0, L, 0, true, true);
    double e = 0;
    for (int i = 0; i < L; ++i)
        e += (double)ref.getSample(0, i) * ref.getSample(0, i);
    ref.applyGain((float)(1.0 / std::sqrt(e)));
    const int fade = juce::jmin(L / 4, (int)(0.05 * sr));
    for (int c = 0; c < 4; ++c)
        ref.applyGainRamp(c, L - fade, fade, 1.0f, 0.0f);
    const int N = 9000;
    std::vector<float> x((size_t)N);
    for (int i = 0; i < 6000; ++i)
        x[(size_t)i] = r.bi();
    const float a0 = juce::degreesToRadians(cv->getInfo().baseAzDeg);
    auto out = run(
        *cv,
        [&](int t, std::vector<std::vector<float>> &v)
        {
            v[0][0] = x[(size_t)t];
            v[1][0] = std::cos(a0) * x[(size_t)t];
            v[2][0] = std::sin(a0) * x[(size_t)t];
        },
        3, 4, N);
    double maxErr = 0, maxRef = 0;
    for (int c = 0; c < 4; ++c)
        for (int t = Convolver::kBlock; t < N; t += 7)
        {
            double y = 0;
            const int tt = t - Convolver::kBlock;
            for (int k = juce::jmax(0, tt - 6000 + 1); k <= juce::jmin(tt, L - 1); ++k)
                y += (double)ref.getSample(c, k) * x[(size_t)(tt - k)];
            maxErr = std::max(maxErr, std::abs(y - out.getSample(c, t)));
            maxRef = std::max(maxRef, std::abs(y));
        }
    check(maxErr < 2.0e-3 * maxRef, "not following = the measured room exactly (rel err " +
                                        juce::String(maxErr / maxRef, 7) + ")");
    {
        // the same over the whole response: the tail's large partitions (2048 from 3584 samples,
        // 8192 from 15872), whose work is spread over time, must line up exactly with the early
        // part
        const int N2 = L + 12000, X2 = 26000;
        std::vector<float> x2((size_t)N2, 0.0f);
        for (int i = 0; i < X2; ++i)
            x2[(size_t)i] = r.bi();
        auto out2 = run(
            *cv,
            [&](int t, std::vector<std::vector<float>> &v)
            {
                v[0][0] = x2[(size_t)t];
                v[1][0] = std::cos(a0) * x2[(size_t)t];
                v[2][0] = std::sin(a0) * x2[(size_t)t];
            },
            3, 4, N2);
        double err2 = 0, ref2 = 0;
        for (int c = 0; c < 4; ++c)
            for (int t = Convolver::kBlock; t < N2; t += 13)
            {
                double y = 0;
                const int tt = t - Convolver::kBlock;
                for (int k = juce::jmax(0, tt - X2 + 1); k <= juce::jmin(tt, L - 1); ++k)
                    y += (double)ref.getSample(c, k) * x2[(size_t)(tt - k)];
                err2 = std::max(err2, std::abs(y - out2.getSample(c, t)));
                ref2 = std::max(ref2, std::abs(y));
            }
        check(err2 < 2.0e-3 * ref2, "whole response exact, all partition sizes (rel err " +
                                        juce::String(err2 / ref2, 7) + ")");
    }

    // 2) following: a pipe at -30 deg gets its direct sound / early reflections from -30 deg
    auto impulseAt = [&](Convolver &c, float deg, int nIn, int numOut)
    {
        const float a = juce::degreesToRadians(deg);
        return run(
            c,
            [&](int t, std::vector<std::vector<float>> &v)
            {
                const float s = t == 0 ? 1.0f : 0.0f;
                v[0][0] = s;
                for (int m = 1; 2 * m < nIn; ++m)
                {
                    v[(size_t)(2 * m - 1)][0] = std::cos((float)m * a) * s;
                    v[(size_t)(2 * m)][0] = std::sin((float)m * a) * s;
                }
            },
            nIn, numOut, (int)(0.7 * sr));
    };
    auto o2 = impulseAt(*cv, -30.0f, 3, 4);
    const int td = Convolver::kBlock + 100;
    const float dir = juce::radiansToDegrees(std::atan2(o2.getSample(1, td), o2.getSample(3, td)));
    std::cout << "  pipe at -30 deg -> direct sound of the room from " << dir << " deg"
              << std::endl;
    check(std::abs(dir + 30.0f) < 1.0f, "early reflections turn with the pipe");

    // 3) the IR's own direct sound turned down
    Convolver::Settings st2 = st;
    st2.direct = 0.2f;
    auto cvD = Convolver::create(f, st2, err);
    auto o3 = impulseAt(*cvD, -30.0f, 3, 4);
    check(std::abs(o3.getSample(0, td) / o2.getSample(0, td) - 0.2f) < 0.01f,
          "direct sound of the IR at 20 %");

    // 4) envelopment: mono IR, 3rd order output, fully diffuse tail -> energies 1/(2l+1),
    // decorrelated from W
    juce::AudioBuffer<float> mono(1, L);
    for (int i = 0; i < L; ++i)
        mono.setSample(0, i, r.bi() * std::exp(-6.0f * (float)i / (float)L));
    auto fm1 = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("rysum_test_ir_mono.wav");
    writeWav(fm1, mono, sr);
    Convolver::Settings st3;
    st3.outOrder = 3;
    st3.envelop = 1.0f;
    st3.hostRate = sr;
    auto cvM = Convolver::create(fm1, st3, err);
    check(cvM != nullptr && cvM->getInfo().numInputs == 7, "mono IR lifted to 3rd order (7 sends)");
    if (cvM)
    {
        auto o4 = impulseAt(*cvM, 0.0f, 7, 16);
        const int a = Convolver::kBlock + (int)(0.15 * sr), n = (int)(0.4 * sr);
        auto en = [&](int c)
        {
            double s = 0;
            for (int i = a; i < a + n; ++i)
                s += (double)o4.getSample(c, i) * o4.getSample(c, i);
            return s;
        };
        const double eW = en(0);
        double corr = 0;
        for (int i = a; i < a + n; ++i)
            corr += (double)o4.getSample(0, i) * o4.getSample(1, i);
        corr /= std::sqrt(eW * en(1));
        juce::String ratios;
        bool ok = true;
        for (int l = 1; l <= 3; ++l)
        {
            const double rr = en(l * l + l) / eW * (2 * l + 1);
            ratios << " l" << l << "=" << juce::String(rr, 2);
            ok = ok && std::abs(rr - 1.0) < 0.35;
        }
        std::cout << "  diffuse tail energy x(2l+1)/W:" << ratios << ", corr(W,Y) "
                  << juce::String(corr, 3) << std::endl;
        check(ok && std::abs(corr) < 0.2, "enveloping tail: isotropic energies, decorrelated");
    }

    // 6) cost: 4 s, 5th order IR, 5th order output, following (11 sends)
    {
        juce::AudioBuffer<float> big(36, (int)(4.0 * sr));
        for (int c = 0; c < 36; ++c)
            for (int i = 0; i < big.getNumSamples(); ++i)
                big.setSample(c, i,
                              r.bi() * std::exp(-4.0f * (float)i / (float)big.getNumSamples()) *
                                  (c == 0 ? 1.0f : 0.4f));
        auto fb = juce::File::getSpecialLocation(juce::File::tempDirectory)
                      .getChildFile("rysum_test_ir_big.wav");
        writeWav(fb, big, sr);
        Convolver::Settings s5;
        s5.outOrder = 5;
        s5.hostRate = sr;
        auto t0 = std::chrono::high_resolution_clock::now();
        auto cvB = Convolver::create(fb, s5, err);
        auto t1 = std::chrono::high_resolution_clock::now();
        if (cvB)
        {
            const int n = 512, blocks = (int)(5.0 * sr / n);
            std::vector<std::vector<float>> ins(11, std::vector<float>((size_t)n));
            for (auto &v : ins)
                for (auto &q : v)
                    q = r.bi() * 0.1f;
            std::vector<const float *> ip;
            for (auto &v : ins)
                ip.push_back(v.data());
            juce::AudioBuffer<float> o(36, n);
            std::vector<float *> op;
            for (int c = 0; c < 36; ++c)
                op.push_back(o.getWritePointer(c));
            auto t2 = std::chrono::high_resolution_clock::now();
            for (int b = 0; b < blocks; ++b)
                cvB->process(ip.data(), n, op.data(), 36, 1.0f);
            auto t3 = std::chrono::high_resolution_clock::now();
            const double rt = 5.0 / std::chrono::duration<double>(t3 - t2).count();
            std::cout << "  5th order, 4 s IR: build "
                      << std::chrono::duration<double>(t1 - t0).count() << " s, " << rt
                      << "x realtime" << std::endl;
            check(rt > 0.8, "5th-order room runs in real time (this slow test machine: ~1x)");
        }
        fb.deleteFile();
    }

    // 5) resampling path
    Convolver::Settings st4 = st;
    st4.hostRate = 44100.0;
    auto cv2 = Convolver::create(f, st4, err);
    check(cv2 != nullptr && std::abs(cv2->getInfo().seconds - 0.7) < 0.02,
          "IR resampled to 44.1 kHz, length " +
              (cv2 ? juce::String(cv2->getInfo().seconds, 3) : juce::String()));
    f.deleteFile();
    fm1.deleteFile();
}

/** the simulated room's tail convolver (three partition sizes, the large ones spread over time)
   against a plain convolution, and a crossfade to a second response */
static void testTailConvolver()
{
    std::cout << "tail convolver" << std::endl;
    const double sr = 48000.0;
    const int L = 60000, C = 3;
    XorShift r(7);
    std::vector<std::vector<float>> h((size_t)C, std::vector<float>((size_t)L, 0.0f));
    for (auto &ch : h)
        for (int i = 0; i < L; ++i)
            ch[(size_t)i] = r.bi() * std::exp(-3.0f * (float)i / (float)L);
    auto ir = TailIR::fromResponses(h, sr, 1);
    TailConvolver tc;
    tc.prepare(sr);
    tc.setIR(ir);
    const int N = L + 30000, X = 50000;
    std::vector<float> x((size_t)N, 0.0f);
    std::vector<std::vector<float>> y((size_t)C, std::vector<float>((size_t)N, 0.0f));
    for (int i = 0; i < X; ++i)
        x[(size_t)i] = r.bi();
    int pos = 0, bs = 37;
    // (the response takes effect at the next block start of each stage: feed silence first)
    {
        std::vector<float> z(40000, 0.0f), o0(40000), o1(40000), o2(40000);
        float *oo[3]{o0.data(), o1.data(), o2.data()};
        tc.process(z.data(), oo, 3, 40000, 1.0f, 1.0f);
    }
    while (pos < N)
    {
        const int n = juce::jmin(bs, N - pos);
        float *o[3]{y[0].data() + pos, y[1].data() + pos, y[2].data() + pos};
        tc.process(x.data() + pos, o, C, n, 1.0f, 1.0f);
        pos += n;
        bs = bs == 37 ? 501 : (bs == 501 ? 1024 : 37);
    }
    double err = 0, ref = 0;
    for (int c = 0; c < C; ++c)
        for (int t = 0; t < N; t += 11)
        {
            double v = 0;
            for (int k = TailIR::kStart[0]; k <= juce::jmin(t, L - 1); ++k)
                if (t - k < X)
                    v += (double)h[(size_t)c][(size_t)k] * x[(size_t)(t - k)];
            err = std::max(err, std::abs(v - y[(size_t)c][(size_t)t]));
            ref = std::max(ref, std::abs(v));
        }
    check(err < 1.0e-3 * ref,
          "tail convolver = plain convolution (rel err " + juce::String(err / ref, 7) + ")");
}

/** a very long room (reverberation longer than the response can hold): it decays to silence
    smoothly, without being cut off at the end */
static void testLongTail()
{
    std::cout << "long late reverb" << std::endl;
    const double sr = 48000.0;
    RoomParams r;
    r.on = true;
    r.width = 40;
    r.length = 80;
    r.height = 25;
    r.absWalls = 0.01f;
    r.absFloor = 0.01f;
    r.highDamp = 0.0f;
    r.scatter = 0.3f;
    auto ir = TailIR::build(r, 0.0f, 0, sr, 1);
    std::cout << "  RT " << ir->rtLow << " / " << ir->rtHigh << " s, response "
              << (double)ir->length / sr << " s" << std::endl;
    check(ir->rtLow > 1.2 * TailIR::kRoomMaxSeconds, "long tail: the room rings longer than the response holds");
    TailConvolver tc;
    tc.prepare(sr);
    tc.setIR(ir);
    // (the response takes effect at the next block start of each stage: silence first)
    const int B = 512;
    std::vector<float> z((size_t)B, 0.0f), o0((size_t)B), o1((size_t)B);
    for (int i = 0; i < 80; ++i)
    {
        float *oo[2]{o0.data(), o1.data()};
        tc.process(z.data(), oo, 2, B, 1.0f, 1.0f);
    }
    const int N = ir->length + 2 * B;
    std::vector<float> y((size_t)N, 0.0f);
    for (int pos = 0; pos < N; pos += B)
    {
        std::vector<float> x((size_t)B, 0.0f);
        if (pos == 0)
            x[0] = 1.0f;
        float *oo[2]{o0.data(), o1.data()};
        std::fill(o0.begin(), o0.end(), 0.0f);
        std::fill(o1.begin(), o1.end(), 0.0f);
        tc.process(x.data(), oo, 2, B, 1.0f, 1.0f);
        for (int i = 0; i < B && pos + i < N; ++i)
            y[(size_t)(pos + i)] = o0[(size_t)i] + o1[(size_t)i];
    }
    // level in 50 ms windows (dB against the loudest)
    const int W = (int)(0.05 * sr);
    std::vector<double> db;
    double top = -1e9;
    for (int a = 0; a + W <= N; a += W)
    {
        double e = 0.0;
        for (int i = 0; i < W; ++i)
            e += (double)y[(size_t)(a + i)] * y[(size_t)(a + i)];
        db.push_back(10.0 * std::log10(e / W + 1e-30));
        top = std::max(top, db.back());
    }
    // the steepest fall from one window to the next while it is still audible (above -70 dB), and
    // the level shortly before the end
    double worst = 0.0;
    for (size_t i = 1; i < db.size(); ++i)
        if (db[i - 1] - top > -70.0)
            worst = std::max(worst, db[i - 1] - db[i]);
    const size_t endW = (size_t)juce::jmax(0, (int)((ir->length - (int)(0.15 * sr)) / W));
    const double nearEnd = endW < db.size() ? db[endW] - top : 0.0;
    std::cout << "  steepest fall per 50 ms (while above -70 dB): " << worst
              << " dB, 0.15 s before the end: " << nearEnd << " dB" << std::endl;
    check(worst < 4.0, "long tail: decays smoothly (no cut)");
    check(nearEnd < -65.0, "long tail: has died away before the response ends");
}

static void testMotion()
{
    std::cout << "motion" << std::endl;
    // continuity: position as a function of phase must not jump (except Jumps mode)
    for (int m : {1, 2, 3, 4})
    {
        float worst = 0;
        Pos b{30.0f, 20.0f};
        Pos prev = applyMotion(b, (Motion)m, -3.0, 40.0f, 5);
        for (int i = 1; i < 20000; ++i)
        {
            const double ph = -3.0 + i * 0.0003;
            Pos q = applyMotion(b, (Motion)m, ph, 40.0f, 5);
            float da = std::abs(wrapAz(q.az - prev.az));
            if (std::abs(q.el) > 85.0f || std::abs(prev.el) > 85.0f)
                da = 0; // azimuth is undefined at the poles
            worst = std::max(worst, da + std::abs(q.el - prev.el));
            prev = q;
        }
        check(worst < 3.0f, motionNames()[m] + " moves continuously (max step " +
                                juce::String(worst, 2) + " deg)");
    }
    // vertical rotation passes over the top
    Pos top = applyMotion({0.0f, 0.0f}, Motion::Vertical, 0.25, 0, 1);
    check(top.el > 89.0f, "vertical rotation: front -> zenith after a quarter turn");
    Pos back = applyMotion({0.0f, 0.0f}, Motion::Vertical, 0.5, 0, 1);
    check(std::abs(std::abs(back.az) - 180.0f) < 1.0f && std::abs(back.el) < 1.0f,
          "vertical rotation: behind after half a turn");
    // jumps: constant within one step, different after
    Pos j1 = applyMotion({0, 0}, Motion::Jumps, 0.10, 90.0f, 3),
        j2 = applyMotion({0, 0}, Motion::Jumps, 0.11, 90.0f, 3);
    int changes = 0;
    Pos pj = j1;
    for (int i = 0; i < 1000; ++i)
    {
        Pos q = applyMotion({0, 0}, Motion::Jumps, i * 0.01, 90.0f, 3);
        if (std::abs(q.az - pj.az) > 1.0e-6f)
            ++changes;
        pj = q;
    }
    check(std::abs(j1.az - j2.az) < 1.0e-6f || changes > 0, "jumps hold still between jumps");
    check(changes >= 8 && changes <= 11,
          "about one jump per cycle (" + juce::String(changes) + " in 10 cycles)");
}

static void writeWav(const juce::File &f, const juce::AudioBuffer<float> &b, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os(f.createOutputStream());
    auto opts = juce::AudioFormatWriterOptions()
                    .withSampleRate(sr)
                    .withNumChannels(b.getNumChannels())
                    .withBitsPerSample(24);
    if (auto w = wav.createWriterFor(os, opts))
    {
        if (!w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()))
            check(false, "WAV write failed: " + f.getFullPathName());
    }
    else
        check(false, "WAV writer creation failed: " + f.getFullPathName());
}

struct Scene
{
    juce::String name;
    int outputMode;
    int channels;
    std::function<void(EngineParams &, double t)> params;
    juce::MidiBuffer midiAll; // positions in samples from the start
    double seconds;
};

static juce::AudioBuffer<float> render(OrganEngine &e, Scene &sc, double sr, int block,
                                       double &rtFactor, int &maxVoices)
{
    const int total = (int)(sc.seconds * sr);
    juce::AudioBuffer<float> out(sc.channels, total), tmp(sc.channels, block);
    EngineParams p;
    p.outputMode = sc.outputMode;
    auto t0 = std::chrono::high_resolution_clock::now();
    maxVoices = 0;
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = std::min(block, total - pos);
        tmp.setSize(sc.channels, n, false, false, true);
        juce::MidiBuffer mb;
        for (const auto m : sc.midiAll)
            if (m.samplePosition >= pos && m.samplePosition < pos + n)
                mb.addEvent(m.getMessage(), m.samplePosition - pos);
        sc.params(p, pos / sr);
        e.process(tmp, mb, p);
        maxVoices = std::max(maxVoices, e.activeVoices.load());
        for (int c = 0; c < sc.channels; ++c)
            out.copyFrom(c, pos, tmp, c, 0, n);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    rtFactor = sc.seconds / std::chrono::duration<double>(t1 - t0).count();
    return out;
}

// ---------------- Lucier mode ----------------
static void testLucier(std::shared_ptr<const SampleLibrary> lib)
{
    RoomParams rp; // the default hall
    auto baseStarted = std::chrono::high_resolution_clock::now();
    auto base = lucierFromRoom(rp, 0.5f, 48000.0);
    auto baseFinished = std::chrono::high_resolution_clock::now();
    auto resonanceContrast = [](const LucierResult &r) // how far the strongest resonance stands out
                                                       // (curve: max 0 dB, median below)
    {
        auto c = r.curve;
        std::sort(c.begin(), c.end());
        return -c[c.size() / 2];
    };
    std::vector<LucierResult> res;
    double buildMs = 0;
    for (float g : {1.0f, 4.0f, 12.0f, 30.0f, 12.5f})
    {
        auto a0 = std::chrono::high_resolution_clock::now();
        res.push_back(buildLucier(*base, g, 0.3f, 1));
        buildMs = std::max(buildMs, std::chrono::duration<double, std::milli>(
                                        std::chrono::high_resolution_clock::now() - a0)
                                        .count());
    }
    std::cout << "lucier: base '" << base->what << "' in "
              << std::chrono::duration<double, std::milli>(baseFinished - baseStarted).count()
              << " ms; builds up to " << buildMs
              << " ms; contrast gen 1/4/12/30: " << resonanceContrast(res[0]) << " / "
              << resonanceContrast(res[1]) << " / " << resonanceContrast(res[2]) << " / "
              << resonanceContrast(res[3]) << " dB; strongest at gen 30:";
    for (auto f : res[3].peaks)
        std::cout << " " << f;
    std::cout << " Hz" << std::endl;
    check(resonanceContrast(res[1]) > resonanceContrast(res[0]) + 3.0f &&
              resonanceContrast(res[2]) > resonanceContrast(res[1]) + 3.0f &&
              resonanceContrast(res[3]) > resonanceContrast(res[2]),
          "lucier: the room's resonances stand out more with every generation");
    check(!res[3].peaks.empty() && res[3].peaks[0] > 30.0f && res[3].peaks[0] < 12000.0f,
          "lucier: resonances found");

    // in the engine: a chord through 12 generations, all wet; then off: the room rings out, the
    // organ is back unchanged
    auto run = [&](bool on, float mix, float &wetDb, float &afterDb, bool &finite, float &peak)
    {
        OrganEngine e;
        e.prepare(48000.0, 512);
        e.setLibrary(lib);
        e.setLucierIR(res[2].ir);
        EngineParams p;
        p.stopOn = {};
        p.stopOn[1] = true;
        p.clickLevel = 0;
        p.releaseClick = 0;
        p.wind = 0;
        p.stopPull = 0;
        p.lucierOn = on;
        p.lucierMix = mix;
        juce::AudioBuffer<float> b(2, 512);
        double eOn = 0, eAfter = 0;
        finite = true;
        peak = 0;
        for (int pos = 0; pos < 48000 * 12; pos += 512)
        {
            juce::MidiBuffer mb;
            if (pos == 0)
                for (int nn : {48, 55, 60, 64})
                    mb.addEvent(juce::MidiMessage::noteOn(1, nn, (juce::uint8)100), 0);
            if (pos >= 48000 * 6)
                p.lucierOn = false;
            e.process(b, mb, p);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i)
                {
                    const float v = b.getSample(c, i);
                    finite = finite && std::isfinite(v);
                    peak = std::max(peak, std::abs(v));
                }
            const double r = b.getRMSLevel(0, 0, 512);
            if (pos > 48000 * 3 && pos < 48000 * 6)
                eOn += r * r;
            if (pos > 48000 * 10)
                eAfter += r * r;
            if (std::getenv("RYSUM_DEBUG_LUCIER") != nullptr && pos % 48128 == 0)
                std::cout << "   t " << pos / 48000.0 << " rms " << r << std::endl;
        }
        wetDb = (float)(10.0 * std::log10(eOn + 1e-20));
        afterDb = (float)(10.0 * std::log10(eAfter + 1e-20));
    };
    float dryOn, dryAfter, wetOn, wetAfter, pk1, pk2;
    bool f1, f2;
    run(false, 1.0f, dryOn, dryAfter, f1, pk1);
    run(true, 1.0f, wetOn, wetAfter, f2, pk2);
    std::cout << "lucier in the engine: dry " << dryOn << " dB, all wet (gen 12) " << wetOn
              << " dB, peak " << pk2 << "; after switching off " << wetAfter << " dB (dry "
              << dryAfter << ")" << std::endl;
    check(f2 && pk2 < 2.0f, "lucier: output finite and limited");
    check(std::abs(wetOn - dryOn) < 12.0f, "lucier: the wet sound is about as loud as the organ");
    check(std::abs(wetAfter - dryAfter) < 0.5f,
          "lucier: switched off, the organ sounds as before (after the room rang out)");

    // a response from a file (a recorded room): read, trimmed to the direct sound
    juce::File tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("rysum_lucier_test.wav");
    {
        juce::AudioBuffer<float> ir(1, 96000);
        ir.clear();
        XorShift rng(7);
        for (int i = 4800; i < 96000; ++i)
            ir.setSample(0, i,
                         (i == 4800 ? 1.0f : 0.3f * rng.bi()) *
                             std::exp(-(float)(i - 4800) / 9000.0f));
        writeWav(tmp, ir, 44100.0);
    }
    juce::String fileError;
    auto fb = lucierFromFile(tmp, 48000.0, fileError);
    int argMax = 0;
    if (fb != nullptr)
        for (int i = 0; i < (int)fb->h.size(); ++i)
            if (std::abs(fb->h[(size_t)i]) > std::abs(fb->h[(size_t)argMax]))
                argMax = i;
    const bool fileOk =
        fb != nullptr && fb->h.size() > 48000 && argMax > 0.001 * 48000 && argMax < 0.004 * 48000;
    std::cout << "lucier from a file (44.1 kHz, 100 ms silence before): "
              << (fb != nullptr ? fb->what : fileError)
              << ", starts at the direct sound: " << fileOk << std::endl;
    check(fileOk, "lucier: impulse response file read, resampled and trimmed");
    tmp.deleteFile();

    // the piece: whole recordings through the room, generation after generation
    {
        RoomParams rp2;
        rp2.width = 6.0f;
        rp2.length = 8.0f;
        rp2.height = 3.0f;
        auto b2 = lucierFromRoom(rp2, 0.5f, 48000.0);
        const auto pass = lucierPassResponse(*b2);
        std::vector<float> imp(4800, 0.0f);
        imp[0] = 1.0f;
        const auto y1 = lucierPass(imp, pass, 1 << 22);
        double err = 0.0, ref = 0.0;
        for (size_t i = 0; i < pass.size() && i < y1.size(); ++i)
        {
            err += std::pow(y1[i] - pass[i], 2.0);
            ref += (double)pass[i] * pass[i];
        }
        std::cout << "lucier piece: impulse through one pass, error "
                  << 10.0 * std::log10(err / ref + 1e-30) << " dB, pass "
                  << static_cast<double>(pass.size()) / 48000.0 << " s" << std::endl;
        check(y1.size() == imp.size() + pass.size() - 1 && err < 1.0e-8 * ref,
              "lucier piece: one pass = convolution with the room");
        // 10 s of noise through 12 generations: the spectrum becomes a few lines (contrast grows),
        // the level stays
        XorShift rr(5);
        std::vector<float> x(480000);
        for (auto &v : x)
            v = 0.2f * rr.bi();
        double rms0 = 0;
        for (auto v : x)
            rms0 += (double)v * v;
        rms0 = std::sqrt(rms0 / (double)x.size());
        auto contrast = [](const std::vector<float> &v)
        {
            juce::dsp::FFT fft(16);
            std::vector<float> buf(1 << 17, 0.0f);
            std::vector<double> mag(1 << 15, 0.0);
            for (size_t st = 48000; st + 65536 < v.size(); st += 65536)
            {
                std::fill(buf.begin(), buf.end(), 0.0f);
                std::copy_n(v.begin() + (std::ptrdiff_t)st, 65536, buf.begin());
                fft.performFrequencyOnlyForwardTransform(buf.data());
                for (size_t k = 0; k < mag.size(); ++k)
                    mag[k] += buf[k];
            }
            std::vector<double> band(mag.begin() + 82, mag.begin() + 13650); // 60 Hz .. 10 kHz
            std::sort(band.begin(), band.end());
            return 20.0 * std::log10(band.back() / band[band.size() / 2]);
        };
        std::vector<double> c;
        c.push_back(contrast(x));
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int gnr = 1; gnr <= 12; ++gnr)
        {
            x = lucierPass(x, pass, 480000 + 144000);
            lucierLevelMatch(x, rms0);
            if (gnr == 1 || gnr == 4 || gnr == 12)
                c.push_back(contrast(x));
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::high_resolution_clock::now() - t0)
                              .count();
        double rms = 0;
        float pk = 0;
        for (auto v : x)
        {
            rms += (double)v * v;
            pk = std::max(pk, std::abs(v));
        }
        rms = std::sqrt(rms / (double)x.size());
        std::cout << "lucier piece: contrast gen 0/1/4/12: " << c[0] << " / " << c[1] << " / "
                  << c[2] << " / " << c[3] << " dB; rms " << 20 * std::log10(rms / rms0)
                  << " dB re recording, peak " << pk << "; 12 generations of 10 s in " << ms
                  << " ms" << std::endl;
        check(c[3] > c[2] && c[2] > c[1] && c[1] > c[0] + 3.0,
              "lucier piece: every generation more of the room's resonances");
        check(pk <= 0.9f && std::isfinite(rms), "lucier piece: level held, no clipping");
    }
}

// ---------------------------------------------------------------------------
// the audio input: placed like a pipe (direction, distance delay, gate, its loudness as a
// modulation source)
// ---------------------------------------------------------------------------
static void testInput(std::shared_ptr<const SampleLibrary> lib)
{
    const double sr = 48000.0;
    const int B = 256;
    auto makeEngine = [&](int outMode)
    {
        auto e = std::make_unique<OrganEngine>();
        e->prepare(sr, B);
        e->setLibrary(lib);
        (void)outMode;
        return e;
    };
    auto quiet = [](EngineParams &p)
    {
        p.stopOn = {};
        p.clickLevel = 0;
        p.releaseClick = 0;
        p.wind = 0;
        p.stopPull = 0;
    };
    XorShift rng(99);
    // render 'secs' of noise (left, right or both) through the input; returns per-channel energy of
    // the last half
    auto run = [&](EngineParams p, int chans, bool feedL, bool feedR, double secs,
                   std::vector<double> &energy,
                   std::function<void(int, juce::MidiBuffer &)> midi = {},
                   OrganEngine *keep = nullptr, float *srcOut = nullptr)
    {
        std::unique_ptr<OrganEngine> own;
        OrganEngine *e = keep;
        if (e == nullptr)
        {
            own = makeEngine(p.outputMode);
            e = own.get();
        }
        juce::AudioBuffer<float> out(chans, B), in(2, B);
        energy.assign((size_t)chans, 0.0);
        const int blocks = (int)(secs * sr / B);
        for (int k = 0; k < blocks; ++k)
        {
            for (int i = 0; i < B; ++i)
            {
                in.setSample(0, i, feedL ? 0.3f * rng.bi() : 0.0f);
                in.setSample(1, i, feedR ? 0.3f * rng.bi() : 0.0f);
            }
            juce::MidiBuffer mb;
            if (midi)
                midi(k, mb);
            e->setInput(in.getReadPointer(0), in.getReadPointer(1), B);
            e->process(out, mb, p);
            e->setInput(nullptr, nullptr, 0);
            if (k >= blocks / 2)
                for (int c = 0; c < chans; ++c)
                {
                    const double r = out.getRMSLevel(c, 0, B);
                    energy[(size_t)c] += r * r;
                }
            if (srcOut != nullptr)
                *srcOut = e->sourceOut[(size_t)srcInput].load();
        }
    };
    auto db = [](double e) { return 10.0 * std::log10(e + 1e-20); };
    std::vector<double> en;

    EngineParams p;
    quiet(p);
    run(p, 2, true, true, 0.5, en);
    check(db(en[0] + en[1]) < -150.0, "input: off by default - silent");

    p.input.on = true;
    p.input.stereo = false;
    p.input.az = 90.0f; // left
    run(p, 2, true, true, 0.5, en);
    const double l1 = db(en[0]), r1 = db(en[1]);
    p.input.az = -90.0f; // right
    run(p, 2, true, true, 0.5, en);
    const double l2 = db(en[0]), r2 = db(en[1]);
    std::cout << "input mono at +90 deg: L " << l1 << " R " << r1 << " dB; at -90 deg: L " << l2
              << " R " << r2 << " dB" << std::endl;
    check(l1 > r1 + 10.0 && r2 > l2 + 10.0, "input: placed left / right");

    // stereo input, 180 deg wide: the left input channel comes from the left
    p.input.stereo = true;
    p.input.az = 0.0f;
    p.input.width = 180.0f;
    run(p, 2, true, false, 0.5, en);
    std::cout << "input stereo, only left fed: L " << db(en[0]) << " R " << db(en[1]) << " dB"
              << std::endl;
    check(db(en[0]) > db(en[1]) + 10.0, "input: stereo - left channel on the left");

    // Ambisonics (1st order): a source at +90 deg (left) shows up in Y with the sign of W
    {
        EngineParams pa = p;
        pa.outputMode = 1;
        pa.input.stereo = false;
        pa.input.az = 90.0f;
        pa.input.el = 0.0f;
        auto e = makeEngine(1);
        juce::AudioBuffer<float> out(4, B), in(2, B);
        double wy = 0, ww = 0, wx = 0;
        for (int k = 0; k < 100; ++k)
        {
            for (int i = 0; i < B; ++i)
            {
                const float v = 0.3f * rng.bi();
                in.setSample(0, i, v);
                in.setSample(1, i, v);
            }
            e->setInput(in.getReadPointer(0), in.getReadPointer(1), B);
            e->process(out, {}, pa);
            if (k > 50)
                for (int i = 0; i < B; ++i)
                {
                    ww += out.getSample(0, i) * out.getSample(0, i);
                    wy += out.getSample(0, i) * out.getSample(1, i);
                    wx += out.getSample(0, i) * out.getSample(3, i);
                }
        }
        std::cout << "input in ambisonics at +90 deg: <W,Y>/<W,W> " << wy / ww << ", <W,X>/<W,W> "
                  << wx / ww << std::endl;
        check(wy / ww > 0.8 && std::abs(wx / ww) < 0.1,
              "input: encoded in Ambisonics from the left");
    }

    // distance takes time: 10 m with Doppler = 1 -> (10 - 0.5) / 343 s
    {
        EngineParams pd = p;
        pd.input.stereo = false;
        pd.input.az = 0.0f;
        pd.input.dist = 10.0f;
        pd.doppler = 1.0f;
        auto e = makeEngine(0);
        juce::AudioBuffer<float> out(2, B), in(2, B);
        int first = -1;
        for (int k = 0; k < 40 && first < 0; ++k)
        {
            in.clear();
            if (k == 4)
            {
                in.setSample(0, 0, 1.0f);
                in.setSample(1, 0, 1.0f);
            } // a click at block 4
            e->setInput(in.getReadPointer(0), in.getReadPointer(1), B);
            e->process(out, {}, pd);
            for (int i = 0; i < B && first < 0; ++i)
                if (std::abs(out.getSample(0, i)) > 1.0e-3f)
                    first = k * B + i - 4 * B;
        }
        const int expect = (int)((10.0 - 0.5) / 343.0 * sr);
        std::cout << "input at 10 m: arrives after " << first << " samples (expected ~" << expect
                  << ")" << std::endl;
        check(std::abs(first - expect) < 8,
              "input: distance delays the sound (Doppler when it moves)");
    }

    // gate: keys open the input
    {
        EngineParams pg = p;
        pg.input.stereo = false;
        pg.input.gate = true;
        pg.input.release = 0.05f;
        run(pg, 2, true, true, 0.5, en);
        const double closed = db(en[0] + en[1]);
        run(pg, 2, true, true, 0.5, en,
            [](int k, juce::MidiBuffer &mb)
            {
                if (k == 0)
                    mb.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
            });
        const double open = db(en[0] + en[1]);
        std::cout << "input with gate: no keys " << closed << " dB, key held " << open << " dB"
                  << std::endl;
        check(closed < -100.0 && open > -40.0, "input: keys open the gate");
    }

    // its loudness is a modulation source (also when it is not heard)
    {
        EngineParams pm = p;
        pm.input.on = false;
        float loud = 0, silent = 0;
        run(pm, 2, true, true, 0.3, en, {}, nullptr, &loud);
        run(pm, 2, false, false, 0.3, en, {}, nullptr, &silent);
        std::cout << "input as modulation source: noise " << loud << ", silence " << silent
                  << std::endl;
        check(loud > 0.6f && silent < 0.01f, "input: loudness as a modulation source");
    }

    // riding on a stop (Praestant, rotating) and in the simulated room; then Lucier: finite and not
    // silent
    {
        EngineParams pr = p;
        pr.input.follow = 1;
        pr.motionMode[1] = (int)Motion::Rotation;
        pr.motionRate[1] = 0.5f;
        pr.room.on = true;
        pr.lucierOn = false;
        run(pr, 2, true, true, 1.0, en);
        std::cout << "input on the Praestant in the simulated room: " << db(en[0] + en[1]) << " dB"
                  << std::endl;
        check(std::isfinite(en[0]) && db(en[0] + en[1]) > -40.0,
              "input: follows a stop, in the simulated room");
        RoomParams rp;
        auto base = lucierFromRoom(rp, 0.5f, sr);
        auto e = makeEngine(0);
        e->setLucierIR(buildLucier(*base, 8.0f, 0.3f, 1).ir);
        EngineParams pl = p;
        pl.lucierOn = true;
        pl.lucierMix = 1.0f;
        run(pl, 2, true, true, 3.0, en, {}, e.get());
        std::cout << "input through Lucier (gen 8, all wet): " << db(en[0] + en[1]) << " dB"
                  << std::endl;
        check(std::isfinite(en[0]) && db(en[0] + en[1]) > -40.0 && db(en[0] + en[1]) < 40.0,
              "input: through the Lucier passes");
    }
    // switched off: silent again after the delay line ran out
    {
        auto e = makeEngine(0);
        EngineParams po = p;
        po.input.dist = 20.0f;
        run(po, 2, true, true, 0.5, en, {}, e.get());
        po.input.on = false;
        run(po, 2, true, true, 2.0, en, {}, e.get());
        check(db(en[0] + en[1]) < -150.0, "input: switched off - silent");
    }
}

// ---------------------------------------------------------------------------
// loudspeaker decoding (AllRAD)
// ---------------------------------------------------------------------------
static void testSpeakers()
{
    for (const auto &L : speakerPresets())
    {
        for (int order : {1, 3, 5})
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            auto D = buildAllRad(L, order, true, 1);
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::high_resolution_clock::now() - t0)
                                  .count();
            if (order == 3 || !D->error.isEmpty())
                std::cout << "allrad " << L.name << " order " << order << ": "
                          << (D->error.isEmpty() ? juce::String("ok") : D->error) << ", "
                          << D->triangles.size() << " triangles, +" << D->addedImaginary
                          << " imaginary, energy " << D->energyMinDb << " .. " << D->energyMaxDb
                          << " dB, rE error mean " << D->angleErrMean << " max " << D->angleErrMax
                          << " deg, " << ms << " ms" << std::endl;
            check(D->error.isEmpty() && D->matrix.size() == L.speakers.size(),
                  "allrad: " + L.name + " builds");
        }
    }
    // the cube at 1st order: a regular layout - flat energy, small localisation error
    {
        auto presets = speakerPresets();
        const SpeakerLayout *cube = nullptr;
        for (auto &L : presets)
            if (L.speakers.size() == 8 && L.speakers[0].el > 30.0f)
                cube = &L;
        auto D = buildAllRad(*cube, 1, false, 1);
        check(D->energyMaxDb - D->energyMinDb < 1.5f && D->angleErrMax < 15.0f,
              "allrad: cube, 1st order: even energy, rE towards the source");
        // a source exactly at loudspeaker 1: that one is the loudest
        float y[kMaxAmbiChannels];
        encodeAmbiX(cube->speakers[0].az, cube->speakers[0].el, 1, y);
        int best = -1;
        float bestV = 0;
        for (size_t r = 0; r < D->matrix.size(); ++r)
        {
            float v = 0;
            for (int k = 0; k < 4; ++k)
                v += D->matrix[r][(size_t)k] * y[k];
            if (std::abs(v) > bestV)
            {
                bestV = std::abs(v);
                best = (int)r;
            }
        }
        check(best == 0, "allrad: a source at a loudspeaker comes mostly from it");
    }
    // 7.1.4 at 3rd order: the renderer puts a source at +30 deg (L, channel 1) out of channel 1,
    // the LFE channel stays silent
    {
        auto presets = speakerPresets();
        const SpeakerLayout *l714 = nullptr;
        for (auto &L : presets)
            if (L.speakers.size() == 11)
                l714 = &L;
        auto D = buildAllRad(*l714, 3, true, 2);
        SpeakerRenderer R;
        R.prepare(48000.0, 256);
        R.setDecoder(D);
        juce::AudioBuffer<float> ambi(16, 256), out(12, 256);
        float y[kMaxAmbiChannels];
        encodeAmbiX(30.0f, 0.0f, 3, y);
        XorShift rng(3);
        double e[12]{};
        for (int b = 0; b < 20; ++b)
        {
            for (int i = 0; i < 256; ++i)
            {
                const float v = 0.2f * rng.bi();
                for (int k = 0; k < 16; ++k)
                    ambi.setSample(k, i, v * y[k]);
            }
            R.process(ambi.getArrayOfReadPointers(), 16, out.getArrayOfWritePointers(), 12, 256);
            for (int c = 0; c < 12; ++c)
                e[c] += out.getRMSLevel(c, 0, 256);
        }
        int loud = 0;
        for (int c = 1; c < 12; ++c)
            if (e[c] > e[loud])
                loud = c;
        std::cout << "allrad 7.1.4 renderer, source at +30 deg: loudest channel " << loud + 1
                  << ", LFE " << e[3] << std::endl;
        check(loud == 0 && e[3] == 0.0, "allrad: renderer routes to the right channels");
    }
    // ring of 8 at ear level: imaginary poles are added, nothing comes from above
    {
        SpeakerLayout ringL;
        for (int i = 0; i < 8; ++i)
        {
            Speaker s;
            s.az = -180.0f + 45.0f * static_cast<float>(i);
            s.channel = i + 1;
            ringL.speakers.push_back(s);
        }
        auto D = buildAllRad(ringL, 3, false, 3);
        check(D->error.isEmpty() && D->addedImaginary == 2,
              "allrad: ring - imaginary loudspeakers above and below");
    }
    // IEM JSON round trip
    {
        auto L = speakerPresets()[3];
        L.speakers[2].radius = 3.5f;
        L.speakers[4].gainDb = -3.0f;
        L.speakers[1].imaginary = true;
        SpeakerLayout back;
        juce::String err;
        const bool ok = speakerLayoutFromJson(speakerLayoutToJson(L), back, err);
        check(ok && back.speakers.size() == L.speakers.size() &&
                  std::abs(back.speakers[2].radius - 3.5f) < 1e-4f &&
                  std::abs(back.speakers[4].gainDb + 3.0f) < 0.01f && back.speakers[1].imaginary &&
                  back.speakers[6].channel == L.speakers[6].channel,
              "allrad: IEM JSON layout round trip");
        // a real IEM file looks like this
        const char *iem =
            R"({"Name":"t","Description":"x","LoudspeakerLayout":{"Name":"t","Loudspeakers":[
            {"Azimuth":0.0,"Elevation":0.0,"Radius":1.0,"IsImaginary":false,"Channel":1,"Gain":1.0},
            {"Azimuth":120.0,"Elevation":0.0,"Radius":1.0,"IsImaginary":false,"Channel":2,"Gain":1.0},
            {"Azimuth":-120.0,"Elevation":0.0,"Radius":1.0,"IsImaginary":false,"Channel":3,"Gain":1.0},
            {"Azimuth":0.0,"Elevation":-90.0,"Radius":1.0,"IsImaginary":true,"Channel":4,"Gain":1.0}]}})";
        SpeakerLayout t;
        check(speakerLayoutFromJson(iem, t, err) && t.speakers.size() == 4 &&
                  t.speakers[3].imaginary && buildAllRad(t, 1, true, 4)->error.isEmpty(),
              "allrad: reads IEM AllRADecoder files");
    }
}

// ---------------------------------------------------------------------------
// several simulated rooms (one per stop) and notes spread to single pipes
// ---------------------------------------------------------------------------
/** per-pipe modulation: "Pfeife: Wind" driven by the key position / a per-pipe envelope / an LFO
   with "Versatz" must give every pipe its own value (two notes, rendered on their own, compared) */
/** a stop's own effects (Raum page, "Pfeife" tab): tremulant, crackle and wind leaks of that stop
    alone, and its pipes turning at their own speeds - without jumping when that is changed */
static void testStopEffects(std::shared_ptr<const SampleLibrary> lib)
{
    std::cout << "a stop's own effects" << std::endl;
    const double sr = 48000.0;
    const int B = 256;
    // one held note of stop 1 (2 s): the spread of its level in 10 ms windows (dB), the energy
    // above 6 kHz relative to all, and the level of the silence without a note
    struct Out
    {
        double wobbleDb = 0, highShare = 0, idleDb = -200;
    };
    auto run = [&](std::function<void(EngineParams &)> setup, bool note)
    {
        OrganEngine e;
        e.prepare(sr, B);
        e.setLibrary(lib);
        EngineParams p;
        p.stopOn = {};
        p.stopOn[1] = true;
        p.clickLevel = 0;
        p.releaseClick = 0;
        p.wind = 0;
        p.stopPull = 0;
        p.doppler = 0;
        setup(p);
        juce::AudioBuffer<float> b(2, B);
        std::vector<float> x;
        for (int k = 0; k < (int)(2.0 * sr / B); ++k)
        {
            juce::MidiBuffer mb;
            if (k == 1 && note)
                mb.addEvent(juce::MidiMessage::noteOn(1, 57, (juce::uint8)100), 0);
            e.process(b, mb, p);
            for (int i = 0; i < B; ++i)
                x.push_back(b.getSample(0, i));
        }
        Out o;
        // level in 10 ms windows after 0.8 s
        std::vector<double> db;
        const int W = 480;
        double all = 0, hi = 0, lp = 0;
        for (size_t i = (size_t)(0.8 * sr); i + W <= x.size(); i += W)
        {
            double e2 = 0;
            for (int j = 0; j < W; ++j)
                e2 += (double)x[i + (size_t)j] * x[i + (size_t)j];
            db.push_back(10.0 * std::log10(e2 / W + 1e-20));
        }
        for (size_t i = (size_t)(0.8 * sr); i < x.size(); ++i)
        {
            lp += 0.55 * (x[i] - lp); // (a one-pole low pass around 6 kHz: what is left is high)
            all += (double)x[i] * x[i];
            hi += (double)(x[i] - lp) * (x[i] - lp);
        }
        std::sort(db.begin(), db.end());
        o.wobbleDb = db[(size_t)(0.95 * (db.size() - 1))] - db[(size_t)(0.05 * (db.size() - 1))];
        o.highShare = hi / (all + 1e-20);
        o.idleDb = 10.0 * std::log10(all / (double)(x.size() - (size_t)(0.8 * sr)) + 1e-20);
        return o;
    };
    const auto plain = run([](EngineParams &) {}, true);
    const auto trem = run([](EngineParams &p) { p.stopTrem[1] = 1.0f; }, true);
    const auto tremOther = run([](EngineParams &p) { p.stopTrem[2] = 1.0f; }, true);
    const auto crack = run([](EngineParams &p) { p.stopCrack[1] = 1.0f; }, true);
    const auto quiet = run([](EngineParams &) {}, false);
    const auto leak = run([](EngineParams &p) { p.stopLeak[1] = 1.0f; }, false);
    const auto leakOff = run([](EngineParams &p) { p.stopLeak[1] = 1.0f; p.stopOn[1] = false; }, false);
    std::cout << "  level spread (5..95 %): plain " << plain.wobbleDb << " dB, own tremulant " << trem.wobbleDb
              << " dB, another stop's " << tremOther.wobbleDb << " dB" << std::endl;
    std::cout << "  share above ~6 kHz: plain " << plain.highShare << ", own crackle " << crack.highShare << std::endl;
    std::cout << "  no note: " << quiet.idleDb << " dB, own leaks " << leak.idleDb << " dB, stop pushed in "
              << leakOff.idleDb << " dB" << std::endl;
    check(trem.wobbleDb > plain.wobbleDb + 2.0 && std::abs(tremOther.wobbleDb - plain.wobbleDb) < 1.0,
          "a stop's own tremulant moves only that stop");
    check(crack.highShare > plain.highShare * 1.5, "a stop's own crackle");
    check(leak.idleDb > quiet.idleDb + 30.0 && leakOff.idleDb < quiet.idleDb + 3.0,
          "a stop's own wind leaks hiss while it is drawn");

    // speed spread: the pipes of a turning stop drift apart; changing it makes no pipe jump
    {
        OrganEngine e;
        e.prepare(sr, B);
        e.setLibrary(lib);
        EngineParams p;
        p.stopOn = {};
        p.stopOn[1] = true;
        p.motionMode[1] = (int)Motion::Rotation;
        p.motionRate[1] = 0.25f;
        juce::AudioBuffer<float> b(2, B);
        juce::MidiBuffer none;
        auto runFor = [&](double secs)
        {
            for (int k = 0; k < (int)(secs * sr / B); ++k)
                e.process(b, none, p);
        };
        auto gapDeg = [&]
        {
            const float a = e.currentPosition(1, 10, p).az, c = e.currentPosition(1, 30, p).az;
            return std::abs(wrapAz(a - c));
        };
        runFor(3.0);
        const float gap0 = gapDeg();
        const auto before = e.currentPosition(1, 10, p);
        p.rateSpread[1] = 1.0f;
        e.process(b, none, p);
        const auto after = e.currentPosition(1, 10, p);
        const float jump = std::abs(wrapAz(after.az - before.az)); // one block of turning: ~0.5 deg
        runFor(4.0);
        const float gap1 = gapDeg();
        std::cout << "  speed spread: two pipes " << gap0 << " deg apart, after 4 s of spread " << gap1
                  << " deg; when it was switched on a pipe moved " << jump << " deg" << std::endl;
        check(std::abs(gap1 - gap0) > 5.0f && jump < 2.0f, "a stop's pipes turn at their own speeds, without jumping");
    }
}

static void testPipeTargets(std::shared_ptr<const SampleLibrary> lib)
{
    std::cout << "per-pipe modulation" << std::endl;
    const double sr = 48000.0;
    const int B = 256;
    // level (dB) of one note after 'secs', with a route on Pfeife: Wind; the second note struck
    // 'lateBy' s after the first
    auto level = [&](int note, std::function<void(EngineParams &)> setup, int otherNote = -1,
                     double otherAt = 0.0) -> double
    {
        OrganEngine e;
        e.prepare(sr, B);
        e.setLibrary(lib);
        EngineParams p;
        p.stopOn = {};
        p.stopOn[1] = true;
        p.clickLevel = 0;
        p.releaseClick = 0;
        p.wind = 0;
        p.stopPull = 0;
        p.doppler = 0;
        setup(p);
        juce::AudioBuffer<float> b(2, B);
        double en = 0;
        int cnt = 0;
        for (int k = 0; k < (int)(2.0 * sr / B); ++k)
        {
            juce::MidiBuffer mb;
            if (k == 1)
                mb.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8)100), 0);
            if (otherNote >= 0 && k == 1 + (int)(otherAt * sr / B))
                mb.addEvent(juce::MidiMessage::noteOn(1, otherNote, (juce::uint8)100), 0);
            e.process(b, mb, p);
            if (k * B > (int)(1.5 * sr))
            {
                en += (double)b.getRMSLevel(0, 0, B) * b.getRMSLevel(0, 0, B);
                ++cnt;
            }
        }
        return 10.0 * std::log10(en / juce::jmax(1, cnt) + 1e-20);
    };
    // key position -> wind: low keys keep their wind, high keys lose most of it
    auto keyRoute = [](EngineParams &p) { p.mod.routes[0] = {srcKey, tgtWind, -0.4f, false}; };
    const double low = level(41, keyRoute), high = level(77, keyRoute),
                 lowRef = level(41, [](EngineParams &) {}),
                 highRef = level(77, [](EngineParams &) {});
    std::cout << "  Tastenlage -> Pfeife: Wind: low note " << (low - lowRef) << " dB, high note "
              << (high - highRef) << " dB (against no modulation)" << std::endl;
    check(high - highRef < -3.0 && low - lowRef > -1.0 && low - lowRef < 3.0,
          "per-pipe wind follows the key position");
    // LFO with Versatz along the keyboard (a whole cycle): the lowest pipe is at the start of the
    // square wave (+1: less wind), the highest near its end (-1: full wind)
    auto lfoRoute = [](EngineParams &p)
    {
        p.mod.lfo[0].shape = shSquare;
        p.mod.lfo[0].rate = 0.01f;
        p.mod.lfo[0].spread = 1.0f;
        p.mod.lfo[0].spreadMode = 0;
        p.mod.routes[0] = {srcLfo1, tgtWind, -0.5f, false};
    };
    const double a = level(41, lfoRoute), c = level(77, lfoRoute);
    std::cout << "  LFO Versatz entlang der Tastatur -> Pfeife: Wind: " << (a - lowRef) << " dB / "
              << (c - highRef) << " dB" << std::endl;
    check(std::abs((a - lowRef) - (c - highRef)) > 3.0,
          "LFO Versatz gives the pipes different values");
    // Pfeife: Verfall on one note only (key position): the low note stays clean
    auto degRoute = [](EngineParams &p)
    {
        p.deg.perPipe = true;
        p.deg.amount = 0.0f;
        p.deg.bits = 3.0f;
        p.mod.routes[0] = {srcKey, tgtDegrade, 1.0f, true};
    };
    const double dLow = level(41, degRoute), dHigh = level(77, degRoute);
    std::cout << "  Pfeife: Verfall (3 bit) by key position: low " << (dLow - lowRef)
              << " dB, high " << (dHigh - highRef) << " dB" << std::endl;
    check(std::abs(dLow - lowRef) < 0.5 && std::abs(dHigh - highRef) > 0.5, "per-pipe Verfall");
}

static void testRoomsAndSpread(std::shared_ptr<const SampleLibrary> lib)
{
    const double sr = 48000.0;
    const int B = 256;
    // the Praestant in a small dry room or in a cathedral (room 2): the cathedral rings much longer
    // after the note
    auto tailAfter = [&](int roomOfStop) -> double
    {
        OrganEngine e;
        e.prepare(sr, B);
        e.setLibrary(lib);
        EngineParams p;
        p.stopOn = {};
        p.stopOn[0] = true;
        p.clickLevel = 0;
        p.releaseClick = 0;
        p.wind = 0;
        p.stopPull = 0;
        p.room.on = true;
        p.room.width = 4;
        p.room.length = 5;
        p.room.height = 3;
        p.room.absWalls = 0.4f;
        p.room.absFloor = 0.4f;
        p.moreRooms[0] = p.room;
        p.moreRooms[0].width = 30;
        p.moreRooms[0].length = 90;
        p.moreRooms[0].height = 32;
        p.moreRooms[0].absWalls = 0.03f;
        p.moreRooms[0].absFloor = 0.05f;
        p.stopRoom[0] = roomOfStop;
        e.setTailIR(TailIR::build(p.room, p.air, 0, sr, 1), 0);
        e.setTailIR(TailIR::build(p.moreRooms[0], p.air, 0, sr, 2), 1);
        juce::AudioBuffer<float> b(2, B);
        double after = 0.0;
        for (int k = 0; k < (int)(6.0 * sr / B); ++k)
        {
            juce::MidiBuffer mb;
            if (k == 2)
                mb.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
            if (k == (int)(1.5 * sr / B))
                mb.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            e.process(b, mb, p);
            const double t = k * B / sr;
            if (t > 3.0 && t < 5.0)
                after += b.getRMSLevel(0, 0, B) * b.getRMSLevel(0, 0, B);
        }
        return 10.0 * std::log10(after + 1e-20);
    };
    const double small = tailAfter(0), cath = tailAfter(1);
    std::cout << "rooms: 1.5-3.5 s after the note, Praestant in the small room " << small
              << " dB, in room 2 (cathedral) " << cath << " dB" << std::endl;
    check(cath > small + 15.0, "rooms: a stop sounds in its own simulated room");

    // notes to single pipes
    auto spread = [&](int mode, std::vector<int> notes, std::array<uint64_t, kNumStops> &mask,
                      int mutedStop = -1, int mutedKey = -1)
    {
        OrganEngine e;
        e.prepare(sr, B);
        e.setLibrary(lib);
        if (mutedStop >= 0)
            e.muted[(size_t)mutedStop][(size_t)mutedKey] = true;
        EngineParams p;
        p.stopOn = {};
        for (int s2 : {0, 1, 2, 6})
            p.stopOn[(size_t)s2] = true;
        p.clickLevel = 0;
        p.noteSpread = mode;
        juce::AudioBuffer<float> b(2, B);
        for (int k = 0; k < 8; ++k)
        {
            juce::MidiBuffer mb;
            if (k == 1)
            {
                int pos = 0;
                for (int n : notes)
                    mb.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8)100), pos++);
            }
            e.process(b, mb, p);
        }
        for (int s2 = 0; s2 < kNumStops; ++s2)
            mask[(size_t)s2] = e.soundingMask[(size_t)s2].load();
    };
    auto count = [](const std::array<uint64_t, kNumStops> &m)
    {
        int c = 0;
        for (auto v : m)
            for (uint64_t b = v; b != 0; b &= b - 1)
                ++c;
        return c;
    };
    std::array<uint64_t, kNumStops> m0{}, m1{}, m2{}, m3{};
    spread(0, {48, 52, 55, 60}, m0);
    spread(1, {48, 52, 55, 60}, m1);
    spread(2, {60, 55, 52, 48}, m2); // played from the top: still the lowest note on the first
                                     // stop? (by position at note-on)
    int stopsUsed = 0;
    for (auto v : m1)
        stopsUsed += v != 0 ? 1 : 0;
    std::cout << "spread: all stops " << count(m0) << " pipes, in turn " << count(m1)
              << " pipes on " << stopsUsed << " stops" << std::endl;
    check(count(m0) == 16 && count(m1) == 4 && stopsUsed == 4,
          "spread: one pipe per note, every note on another stop");
    // by position, played from below: lowest note -> first stop on the console (Praestant 8', then
    // Gedackt, Octave 4', Trompete)
    spread(2, {48, 52, 55, 60}, m3);
    const int k48 = keyIndexForMidi(48);
    check(k48 >= 0 && ((m3[0] >> k48) & 1) == 1,
          "spread: by position, the lowest note on the first stop");
    // a muted pipe is skipped: in turn, the first note would go to the Praestant - muted there, it
    // goes to the Gedackt
    std::array<uint64_t, kNumStops> m4{};
    spread(1, {48}, m4, 0, k48);
    check(((m4[0] >> k48) & 1) == 0 && ((m4[1] >> k48) & 1) == 1,
          "spread: muted pipes are skipped");
}

// the factory patch "148 Kalkant - Der Atem geht aus" on the engine: a Kalkant treads every four
// seconds (thump at the console), four stops, a big chord held. The tread must not click, and the
// held chord must run out of wind before every tread, whenever the chord starts.
static void testKalkantPatch(std::shared_ptr<const SampleLibrary> lib, const juce::File &outDir)
{
    const double sr = 48000.0;
    const int B = 512;
    auto patch = [](EngineParams &p)
    {
        p.stopOn = {};
        for (int s : {0, 1, 2, 7})
            p.stopOn[(size_t)s] = true;
        // (as the patch has them: with 0.8 / 3 s a middle triad lasted nearly the whole cycle and
        // a high one never ran out)
        p.live.bellows = 1.0f;
        p.live.bellowsSize = 2.2f;
        p.live.blower = 1;
        p.live.treadRate = 15.0f;
        p.live.treadNoise = 0.5f;
        p.windModel = 1;
        p.wind = 0.1f;
    };
    // the step at a tread (the thump starts in the block where the tread count changes): the
    // biggest jump from one sample to the next in the first 2 ms against the biggest jump inside
    // the thump afterwards (a thump at 40-80 Hz moves slowly; a click is one big step at once), and
    // the first samples' level against the thump's peak
    struct Onset
    {
        float stepRatio, firstRatio;
    };
    auto onsetAt = [&](const juce::AudioBuffer<float> &b, int at)
    {
        float onset = 0.0f, body = 1.0e-9f, first = 0.0f, peak = 1.0e-9f;
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            const float *x = b.getReadPointer(c);
            const int end = b.getNumSamples();
            for (int i = juce::jmax(1, at); i < juce::jmin(end, at + (int)(0.002 * sr)); ++i)
                onset = juce::jmax(onset, std::abs(x[i] - x[i - 1]));
            for (int i = at; i < juce::jmin(end, at + 4); ++i)
                first = juce::jmax(first, std::abs(x[i]));
            for (int i = at + (int)(0.004 * sr); i < juce::jmin(end, at + (int)(0.12 * sr)); ++i)
            {
                body = juce::jmax(body, std::abs(x[i] - x[i - 1]));
                peak = juce::jmax(peak, std::abs(x[i]));
            }
        }
        return Onset{onset / body, first / peak};
    };
    // 1) the tread alone (no keys, no wind noise): every thump starts without a click - at the
    //    patch's rate, and fast (the next tread while the last thump still sounds), at two block
    //    sizes
    for (const float rate : {15.0f, 120.0f})
        for (const int blk : {512, 64})
        {
            OrganEngine e;
            e.prepare(sr, blk);
            e.setLibrary(lib);
            EngineParams p;
            patch(p);
            p.wind = 0.0f;
            p.live.treadRate = rate;
            const int total = (int)((rate > 60.0f ? 6.0 : 42.0) * sr);
            juce::AudioBuffer<float> keep(2, total), b(2, blk);
            keep.clear();
            std::vector<int> treads;
            int lastCount = e.treadCount.load();
            for (int pos = 0; pos + blk <= total; pos += blk)
            {
                juce::MidiBuffer mb;
                b.clear();
                e.process(b, mb, p);
                for (int c = 0; c < 2; ++c)
                    keep.copyFrom(c, pos, b, c, 0, blk);
                if (e.treadCount.load() != lastCount)
                {
                    lastCount = e.treadCount.load();
                    treads.push_back(pos);
                }
            }
            float worstStep = 0.0f, worstFirst = 0.0f;
            for (int at : treads)
                if (at + (int)(0.12 * sr) < total)
                {
                    const auto o = onsetAt(keep, at);
                    worstStep = juce::jmax(worstStep, o.stepRatio);
                    worstFirst = juce::jmax(worstFirst, o.firstRatio);
                }
            std::cout << "Kalkant patch, tread alone (" << rate << "/min, block " << blk
                      << "): " << treads.size() << " treads, worst onset step / thump step "
                      << worstStep << ", worst first samples / thump peak " << worstFirst
                      << std::endl;
            // (at 120 /min the last thump and its room still sound when the next one starts: there
            // only the step counts)
            check(treads.size() >= 9 && worstStep < 1.2f && (rate > 60.0f || worstFirst < 0.05f),
                  "Kalkant: the tread's thump starts without a click (" + juce::String(rate) +
                      "/min, block " + juce::String(blk) + ")");
            if (rate < 60.0f && blk == 512)
                writeWav(outDir.getChildFile("83_kalkant_tritt.wav"), keep, sr);
        }
    // 2) a chord held - the big one and plain triads low, middle and high - started at different
    //    points of the Kalkant's cycle: the wind runs out before every tread (the pressure falls
    //    low) and comes back with it. With the thump off: the pipes come back without a click.
    {
        struct Chord
        {
            const char *name;
            std::vector<int> notes;
        };
        const Chord chords[]{{"big chord", {41, 53, 57, 60, 65, 69, 72}},
                             {"triad C3", {48, 52, 55}},
                             {"triad C4", {60, 64, 67}},
                             {"triad C5", {72, 76, 79}}};
        int fine = 0, runs = 0;
        float worstHigh = 0.0f, worstBack = 0.0f;
        for (const auto &ch : chords)
            for (int k = 0; k < 4; ++k)
            {
                OrganEngine e;
                e.prepare(sr, B);
                e.setLibrary(lib);
                EngineParams p;
                patch(p);
                const bool quietTread = k == 1; // one run per chord without the thump
                if (quietTread)
                    p.live.treadNoise = 0.0f;
                const double pre = 1.0 * k; // silence before the chord: its place in the cycle
                const int total = (int)((pre + 13.0) * sr), start = (int)(pre * sr);
                juce::AudioBuffer<float> keep(2, total), b(2, B);
                keep.clear();
                std::vector<int> treads;
                std::vector<float> minP;
                float lowSince = 2.0f;
                std::vector<double> lasts; // how long the chord sounds after a tread until the pressure is half gone
                bool full = false;
                int lastCount = e.treadCount.load();
                for (int pos = 0; pos + B <= total; pos += B)
                {
                    juce::MidiBuffer mb;
                    if (pos <= start && start < pos + B)
                        for (int m : ch.notes)
                            mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), start - pos);
                    b.clear();
                    e.process(b, mb, p);
                    for (int c = 0; c < 2; ++c)
                        keep.copyFrom(c, pos, b, c, 0, B);
                    lowSince = juce::jmin(lowSince, e.bellowsNow.load());
                    if (!treads.empty() && e.bellowsNow.load() > 0.9f)
                        full = true;
                    if (full && e.bellowsNow.load() < 0.5f)
                    {
                        full = false;
                        lasts.push_back((pos - treads.back()) / sr);
                    }
                    if (e.treadCount.load() != lastCount)
                    {
                        lastCount = e.treadCount.load();
                        if (pos > start)
                        {
                            treads.push_back(pos);
                            // (the first tread after the chord began: the chord held only part of
                            // a cycle)
                            if (treads.size() > 1)
                                minP.push_back(lowSince);
                        }
                        lowSince = 2.0f;
                    }
                }
                // every full cycle the chord held: was the wind gone just before the tread?
                bool ok = minP.size() >= 2;
                float high = 0.0f;
                for (float m : minP)
                {
                    high = juce::jmax(high, m);
                    ok = ok && m < 0.1f;
                }
                // the pipes coming back with the wind: no sharper edge than while they sound
                // steadily (second difference: a click stands out of a pipe's tone)
                if (quietTread)
                    for (size_t t = 1; t < treads.size(); ++t)
                    {
                        const int at = treads[t];
                        int where = 0;
                        auto d2max = [&](int i0, int i1)
                        {
                            float m = 1.0e-9f;
                            for (int c = 0; c < 2; ++c)
                            {
                                const float *x = keep.getReadPointer(c);
                                for (int i = juce::jmax(2, i0); i < juce::jmin(total, i1); ++i)
                                {
                                    const float d = std::abs(x[i] - 2.0f * x[i - 1] + x[i - 2]);
                                    if (d > m)
                                    {
                                        m = d;
                                        where = i - i0;
                                    }
                                }
                            }
                            return m;
                        };
                        const float back = d2max(at, at + (int)(0.4 * sr));
                        const int backAt = where;
                        const float steady = d2max(at + (int)(0.5 * sr), at + (int)(1.0 * sr));
                        worstBack = juce::jmax(worstBack, back / steady);
                        std::cout << "  (" << ch.name << ", tread " << t << ": pipes coming back, sharpest edge "
                                  << back << " at +" << backAt * 1000.0 / sr << " ms / steady " << steady
                                  << ")" << std::endl;
                    }
                worstHigh = juce::jmax(worstHigh, high);
                fine += ok ? 1 : 0;
                ++runs;
                std::cout << "Kalkant patch, " << ch.name << " after " << pre << " s: "
                          << treads.size() << " treads, lowest pressure before each:";
                for (float m : minP)
                    std::cout << " " << m;
                std::cout << "; the wind lasts (s):";
                for (double l : lasts)
                    std::cout << " " << l;
                std::cout << std::endl;
                if (k == 0)
                    writeWav(outDir.getChildFile(juce::String("84_kalkant_") +
                                                 juce::String(ch.name).replaceCharacter(' ', '_') +
                                                 ".wav"),
                             keep, sr);
            }
        std::cout << "Kalkant patch: the wind ran out before every tread in " << fine << " of "
                  << runs << " runs (highest low " << worstHigh
                  << "); pipes coming back, sharpest edge / steady " << worstBack << std::endl;
        check(fine == runs, "Kalkant: a held chord runs out of wind before every tread");
        check(worstBack < 1.5f, "Kalkant: the pipes come back with the tread without a click");
    }
}

static void testHeldPipeTuning()
{
    constexpr double sampleRate = 48000.0;
    constexpr double measuredHz = 240.0;
    constexpr int midi = 60;
    const int key = keyIndexForMidi(midi);
    auto library = std::make_shared<SampleLibrary>();
    auto sample = std::make_shared<Sample>();
    sample->sampleRate = sampleRate;
    sample->data.resize(96000);
    for (std::size_t i = 0; i < sample->data.size(); ++i)
        sample->data[i] = static_cast<float>(0.5 * std::sin(2.0 * std::acos(-1.0) * measuredHz *
                                                            static_cast<double>(i) / sampleRate));

    auto &pipe = library->pipes[0][static_cast<std::size_t>(key)];
    pipe.valid = true;
    pipe.sample = sample;
    pipe.midi = midi;
    pipe.nominalHz = meantoneHz(midi);
    pipe.calibrationRatio = pipe.nominalHz / measuredHz;
    pipe.release = 72000;
    pipe.loopStart = 4800;
    pipe.loopEnd = 48000;
    for (const int neighbour : {36, 38, 81})
    {
        auto &adjacent = library->pipes[0][static_cast<std::size_t>(keyIndexForMidi(neighbour))];
        adjacent = pipe;
        adjacent.midi = neighbour;
        adjacent.nominalHz = meantoneHz(neighbour);
        adjacent.calibrationRatio = adjacent.nominalHz / measuredHz;
    }

    OrganEngine engine;
    engine.prepare(sampleRate, 512);
    engine.setLibrary(library);
    EngineParams params;
    params.stopOn[0] = true;
    params.correctSamplePitch = true;
    params.doppler = 0.0f;
    params.pitchCents = effectiveStimmtonCents(kStimmton440Detent);
    params.tuning = makeKeyTuning(6);
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midiEvents;
    midiEvents.addEvent(juce::MidiMessage::noteOn(1, midi, static_cast<juce::uint8>(100)), 0);

    auto renderPitch = [&]()
    {
        double first = 0.0, last = 0.0;
        int crossings = 0;
        float previous = 0.0f;
        for (int block = 0; block < 80; ++block)
        {
            engine.process(audio, midiEvents, params);
            midiEvents.clear();
            for (int i = 0; i < audio.getNumSamples(); ++i)
            {
                const float current = audio.getSample(0, i);
                if (block >= 8 && previous < 0.0f && current >= 0.0f)
                {
                    last = static_cast<double>(block * 512 + i - 1) -
                           previous / static_cast<double>(current - previous);
                    if (crossings++ == 0)
                        first = last;
                }
                previous = current;
            }
        }
        return crossings > 1 ? sampleRate * (crossings - 1) / (last - first) : 0.0;
    };

    check(std::abs(renderPitch() - 440.0 * std::exp2(-9.0 / 12.0)) < 0.02,
          "calibrated pipe follows the 12-EDO target");
    params.tuning = makeKeyTuning(14);
    check(params.tuning.source[static_cast<std::size_t>(key)] != key,
          "31-EDO changes the preferred source pipe");
    check(std::abs(renderPitch() - 440.0 * std::exp2(-9.0 / 31.0)) < 0.02,
          "held pipe follows the 31-EDO target across a source mapping change");

    engine.setLibrary(library);
    params.correctSamplePitch = false;
    params.tuning = makeKeyTuning(0);
    params.pitchCents = 0.0;
    midiEvents.addEvent(juce::MidiMessage::noteOn(2, 37, static_cast<juce::uint8>(100)), 0);
    check(renderPitch() <= 0.0, "recorded tuning leaves the short-octave C-sharp silent");

    params.correctSamplePitch = true;
    params.tuning = makeKeyTuning(6);
    params.pitchCents = effectiveStimmtonCents(kStimmton440Detent);
    midiEvents.addEvent(juce::MidiMessage::noteOn(2, 37, static_cast<juce::uint8>(100)), 0);
    check(std::abs(renderPitch() - 440.0 * std::exp2(-32.0 / 12.0)) < 0.02,
          "retuned short-octave C-sharp uses a neighbouring pipe at the target pitch");

    params.tuning = makeKeyTuning(5); // Pythagorean
    const double stimmton = effectiveStimmtonCents(kStimmton440Detent);
    const double pythagoreanHz =
        keyboardTuningHz(params.tuning, midi) * std::exp2(stimmton / 1200.0);
    NoteTable mts;
    mts.mapped[static_cast<std::size_t>(midi)] = 1;
    mts.hz[static_cast<std::size_t>(midi)] = static_cast<float>(pythagoreanHz);
    engine.setLibrary(library);
    params.notes = &mts;
    params.notesContinuous = false;
    params.pitchCents = 0.0;
    midiEvents.addEvent(juce::MidiMessage::noteOn(1, midi, static_cast<juce::uint8>(100)), 0);
    check(std::abs(renderPitch() - pythagoreanHz) < 0.02,
          "MTS renders the Pythagorean target at A440");
    params.notes = nullptr;
    params.pitchCents = stimmton;
    check(std::abs(renderPitch() - pythagoreanHz) < 0.02,
          "held MTS note keeps its pitch when switching to Pythagorean A440");
    params.notes = &mts;
    params.pitchCents = 0.0;
    check(std::abs(renderPitch() - pythagoreanHz) < 0.02,
          "held Pythagorean note keeps its pitch when switching back to MTS");
    midiEvents.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    engine.process(audio, midiEvents, params);
    midiEvents.clear();
    params.notes = nullptr;
    params.notesContinuous = true;
    params.pitchCents = stimmton;

    for (const int note : {kExtendedLowestMidiNote, kExtendedHighestMidiNote})
    {
        engine.setLibrary(library);
        params.extendedRange = false;
        midiEvents.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), 0);
        check(renderPitch() <= 0.0, "standard range leaves MIDI " + juce::String(note) + " silent");
        params.extendedRange = true;
        midiEvents.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), 0);
        const double target =
            keyboardTuningHz(params.tuning, note) * std::exp2(params.pitchCents / 1200.0);
        check(std::abs(renderPitch() - target) < 0.02,
              "extended range renders MIDI " + juce::String(note) + " at its target pitch");
        midiEvents.addEvent(juce::MidiMessage::allSoundOff(1), 0);
        engine.process(audio, midiEvents, params);
        midiEvents.clear();
    }
}

int main(int argc, char **argv)
{
    if (std::getenv("RYSUM_ONLY_PITCH") != nullptr)
    {
        testHeldPipeTuning();
        return failures;
    }
    testHeldPipeTuning();
    const bool onlyRoom =
        std::getenv("RYSUM_ONLY_ROOM") != nullptr; // quick run: just the room tests
    const bool onlyWind = onlyRoom || std::getenv("RYSUM_ONLY_WIND") !=
                                          nullptr; // quick run: just the wind model tests
    const bool onlyLucier = std::getenv("RYSUM_ONLY_LUCIER") != nullptr;
    const bool onlyInput = std::getenv("RYSUM_ONLY_INPUT") != nullptr;
    if (std::getenv("RYSUM_ONLY_TRANSAURAL") != nullptr)
    {
        testTransaural();
        std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string(failures))
                  << std::endl;
        return failures;
    }
    if (std::getenv("RYSUM_ONLY_SPEAKERS") != nullptr)
    {
        testSpeakers();
        std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string(failures))
                  << std::endl;
        return failures;
    }
    if (!onlyWind && !onlyLucier && !onlyInput)
    {
        testEncoder();
        testConvolver();
        testTailConvolver();
        testLongTail();
        testMotion();
        testBinaural(argc > 3 ? juce::File(argv[3])
                              : juce::File()); // optional 3rd argument: a SOFA file to test with
        testTransaural();
        testSpeakers();
    }
    if (argc < 3)
    {
        std::cout << "usage: RysumRenderTest <samples> <outdir> [hrtf.sofa]" << std::endl;
        return failures;
    }

    juce::File folder(juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]));
    juce::File outDir(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]));
    outDir.createDirectory();

    std::cout << "loading " << folder.getFullPathName() << std::endl;
    juce::String loadError;
    auto loadStarted = std::chrono::high_resolution_clock::now();
    std::shared_ptr<const SampleLibrary> lib(SampleLibrary::load(folder, loadError).release());
    auto loadFinished = std::chrono::high_resolution_clock::now();
    check(lib != nullptr, "library loaded " + loadError);
    if (lib == nullptr)
        return 1;
    if (std::getenv("RYSUM_LUCIER_DEMO") != nullptr)
    {
        // a demo for the ears: a slow chord progression while the generation grows from 1 to 20
        // (all wet towards the end)
        RoomParams rp;
        auto base = lucierFromRoom(rp, 0.5f, 48000.0);
        OrganEngine e;
        e.prepare(48000.0, 512);
        e.setLibrary(lib);
        EngineParams p;
        p.stopOn = {};
        p.stopOn[1] = true;
        p.stopOn[2] = true;
        p.clickLevel = 0.3f;
        p.wind = 0.1f;
        p.lucierOn = true;
        p.lucierMix = 0.35f;
        const int secs = 48;
        juce::AudioBuffer<float> all(2, 48000 * secs), b(2, 512);
        const int chords[4][4]{
            {48, 55, 60, 64}, {45, 52, 57, 60}, {41, 53, 57, 60}, {43, 50, 55, 59}};
        int lastGen = -1;
        for (int pos = 0; pos + 512 <= all.getNumSamples(); pos += 512)
        {
            const double t = pos / 48000.0;
            const float gen = 1.0f + 19.0f * (float)juce::jlimit(0.0, 1.0, t / 40.0);
            if ((int)(gen * 2) != lastGen)
            {
                lastGen = (int)(gen * 2);
                e.setLucierIR(buildLucier(*base, gen, 0.3f, (uint32_t)lastGen).ir);
            }
            p.lucierMix = 0.35f + 0.6f * (float)juce::jlimit(0.0, 1.0, t / 40.0);
            juce::MidiBuffer mb;
            const int c = (int)(t / 6.0) % 4, cPrev = (int)((t - 512 / 48000.0) / 6.0) % 4;
            if (pos == 0 || (c != cPrev && t < 42.0))
            {
                if (pos > 0)
                    for (int nn : chords[cPrev])
                        mb.addEvent(juce::MidiMessage::noteOff(1, nn), 0);
                for (int nn : chords[c])
                    mb.addEvent(juce::MidiMessage::noteOn(1, nn, (juce::uint8)90), 1);
            }
            if (t >= 42.0 && t < 42.0 + 512 / 48000.0)
                for (int nn : chords[c])
                    mb.addEvent(juce::MidiMessage::noteOff(1, nn), 0);
            e.process(b, mb, p);
            for (int ch = 0; ch < 2; ++ch)
                all.copyFrom(ch, pos, b, ch, 0, 512);
        }
        all.applyGain(0.8f / juce::jmax(0.01f, all.getMagnitude(0, all.getNumSamples())));
        auto f = outDir.getChildFile("lucier_demo.wav");
        writeWav(f, all, 48000.0);
        std::cout << "wrote " << f.getFullPathName() << std::endl;
        return failures;
    }
    if (std::getenv("RYSUM_ONLY_PIPEMOD") != nullptr)
    {
        testPipeTargets(lib);
        testStopEffects(lib);
        std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string(failures))
                  << std::endl;
        return failures;
    }
    if (std::getenv("RYSUM_ONLY_KALKANT") != nullptr)
    {
        testKalkantPatch(lib, outDir);
        std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string(failures))
                  << std::endl;
        return failures;
    }
    if (std::getenv("RYSUM_ONLY_ROOMS") != nullptr)
    {
        testRoomsAndSpread(lib);
        std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string(failures))
                  << std::endl;
        return failures;
    }
    if (onlyInput)
    {
        testInput(lib);
        std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string(failures))
                  << std::endl;
        return failures;
    }
    if (onlyLucier)
    {
        testLucier(lib);
        std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string(failures))
                  << std::endl;
        return failures;
    }
    std::cout << "  " << lib->loadedPipes << " pipes, " << lib->substitutedPipes << " substituted, "
              << lib->clicksDown.size() << "/" << lib->clicksUp.size() << "/"
              << lib->clicksOther.size() << " clicks, " << lib->stopPulls.size() << " pulls, "
              << lib->wind.size() << " wind, "
              << std::chrono::duration<double>(loadFinished - loadStarted).count() << " s"
              << std::endl;
    for (int s = 0; s < kNumStops; ++s)
        for (int k = 0; k < kNumKeys; ++k)
        {
            const auto &p = lib->pipes[(size_t)s][(size_t)k];
            if (p.substituted)
                std::cout << "  substituted " << kStops[(size_t)s].id << " "
                          << helmholtzName(p.midi) << " <- " << p.flags.joinIntoString(", ")
                          << std::endl;
            if (!p.valid)
                std::cout << "  INVALID " << kStops[(size_t)s].id << " " << helmholtzName(p.midi)
                          << std::endl;
        }

    const double sr = 48000.0;
    const int block = 256;
    auto note = [sr](juce::MidiBuffer &mb, int midi, double on, double off)
    {
        mb.addEvent(juce::MidiMessage::noteOn(1, midi, (juce::uint8)100), (int)(on * sr));
        mb.addEvent(juce::MidiMessage::noteOff(1, midi), (int)(off * sr));
    };

    std::vector<Scene> scenes;
    { // 1. Praestant alone: a slow walk through all keys (stereo)
        Scene s{"01_praestant_scale_stereo", 0, 2, nullptr, {}, 0};
        double t = 0.2;
        for (int k = 0; k < kNumKeys; ++k)
        {
            note(s.midiAll, kPhysKeys[(size_t)k], t, t + 0.55);
            t += 0.6;
        }
        s.seconds = t + 1.5;
        s.params = [](EngineParams &p, double)
        {
            p.stopOn = {};
            p.stopOn[0] = true;
        };
        scenes.push_back(s);
    }
    { // 2. held chord, stops pulled one after another, long hold (loops!), stereo
        Scene s{"02_plenum_build_stereo", 0, 2, nullptr, {}, 14.0};
        for (int m : {48, 55, 60, 64, 67})
            note(s.midiAll, m, 0.3, 12.0);
        s.params = [](EngineParams &p, double t)
        {
            p.stopOn = {};
            for (int i = 0; i < kNumStops; ++i)
                p.stopOn[(size_t)i] = t > 0.2 + 1.3 * i;
        };
        scenes.push_back(s);
    }
    { // 3. same in AmbiX 5th order with motion
        Scene s{"03_plenum_ambix5_motion", 5, 36, nullptr, {}, 12.0};
        for (int m : {41, 53, 60, 65, 69, 72})
            note(s.midiAll, m, 0.3, 10.0);
        s.params = [](EngineParams &p, double)
        {
            p.stopOn.fill(true);
            p.motionMode.fill((int)Motion::Rotation);
            p.motionRate.fill(0.15f);
            p.motionDepth.fill(40.0f);
            p.motionMode[1] = (int)Motion::Drift;
        };
        scenes.push_back(s);
    }
    { // 4. degradation per pipe, then on the sum (stereo)
        Scene s{"04_verfall_stereo", 0, 2, nullptr, {}, 12.0};
        for (int m : {45, 52, 57, 61, 64})
            note(s.midiAll, m, 0.2, 5.5);
        for (int m : {45, 52, 57, 61, 64})
            note(s.midiAll, m, 6.2, 11.0);
        s.params = [](EngineParams &p, double t)
        {
            p.stopOn = {};
            p.stopOn[0] = p.stopOn[1] = p.stopOn[2] = true;
            p.deg.amount = 0.9f;
            p.deg.grain = 0.6f;
            p.deg.bits = 7.0f;
            p.deg.rateHz = 6000.0f;
            p.deg.wobble = 0.7f;
            p.deg.dropouts = 0.4f;
            p.deg.perPipe = t < 6.0;
        };
        scenes.push_back(s);
    }
    { // 5. stress: all stops, all 41 keys at once, AmbiX 5
        Scene s{"05_stress_all_keys_ambix5", 5, 36, nullptr, {}, 6.0};
        for (int k = 0; k < kNumKeys; ++k)
            note(s.midiAll, kPhysKeys[(size_t)k], 0.1 + 0.01 * k, 4.0);
        s.params = [](EngineParams &p, double)
        {
            p.stopOn.fill(true);
            p.motionMode.fill((int)Motion::Drift);
            p.motionRate.fill(0.3f);
            p.motionDepth.fill(60.0f);
            p.deg.amount = 0.5f;
            p.deg.perPipe = true;
        };
        scenes.push_back(s);
        Scene s2 = s;
        s2.name = "06_stress_all_keys_ambix5_clean";
        s2.params = [](EngineParams &p, double) { p.stopOn.fill(true); };
        scenes.push_back(s2);
    }

    { // 7. stop wind: Praestant slowly closed and opened again (like pulling the stop in and out)
        Scene s{"07_stopwind_sweep_stereo", 0, 2, nullptr, {}, 12.0};
        for (int m : {53, 60, 65, 69})
            note(s.midiAll, m, 0.2, 11.0);
        s.params = [](EngineParams &p, double t)
        {
            p.stopOn = {};
            p.stopOn[0] = true;
            const double x = juce::jlimit(0.0, 1.0, (t - 1.0) / 10.0); // 0..1 over 1..11 s
            p.stopWind[0] = (float)std::abs(1.0 - 2.0 * x);            // 1 -> 0 -> 1
        };
        scenes.push_back(s);
    }
    { // 8. global wind 0 -> 100 %
        Scene s{"08_global_wind_ramp_stereo", 0, 2, nullptr, {}, 14.0};
        for (int m : {48, 55, 60, 64})
            note(s.midiAll, m, 0.2, 13.0);
        s.params = [](EngineParams &p, double t)
        {
            p.stopOn = {};
            p.stopOn[0] = p.stopOn[2] = true;
            p.wind = (float)juce::jlimit(0.0, 1.0, t / 12.0);
        };
        scenes.push_back(s);
    }

    { // 9. speed glissando of a rotation, -2 Hz .. +2 Hz, first order (must stay click-free)
        Scene s{"09_rotation_glissando_ambix1", 1, 4, nullptr, {}, 12.0};
        for (int m : {57, 64, 69})
            note(s.midiAll, m, 0.2, 11.5);
        s.params = [](EngineParams &p, double t)
        {
            p.stopOn = {};
            p.stopOn[0] = true;
            p.clickLevel = 0;
            p.releaseClick = 0;
            p.wind = 0;
            p.motionMode[0] = (int)Motion::Rotation;
            p.motionRate[0] = (float)(-2.0 + 4.0 * juce::jlimit(0.0, 1.0, t / 11.0));
        };
        scenes.push_back(s);
    }

    { // 13. fast: rotation glissando -10 Hz .. +10 Hz in 5th order, then jumps at 10 Hz
        Scene s{"13_fast_rotation_ambix5", 5, 36, nullptr, {}, 14.0};
        for (int m : {57, 64, 69})
            note(s.midiAll, m, 0.2, 13.5);
        s.params = [](EngineParams &p, double t)
        {
            p.stopOn = {};
            p.stopOn[0] = true;
            p.clickLevel = 0;
            p.releaseClick = 0;
            p.wind = 0;
            p.motionMode[0] = (int)(t < 10.0 ? Motion::Rotation : Motion::Jumps);
            p.motionRate[0] =
                t < 10.0 ? (float)(-10.0 + 20.0 * juce::jlimit(0.0, 1.0, t / 10.0)) : 10.0f;
            p.motionDepth[0] = 90.0f;
        };
        scenes.push_back(s);
    }

    { // 10. Doppler: rotation 0.5 Hz with an elliptic orbit (closest in front)
        Scene s{"10_doppler_ellipse_stereo", 0, 2, nullptr, {}, 9.0};
        note(s.midiAll, 69, 0.2, 8.5);
        s.params = [](EngineParams &p, double)
        {
            p.stopOn = {};
            p.stopOn[0] = true;
            p.clickLevel = 0;
            p.releaseClick = 0;
            p.wind = 0;
            p.motionMode[0] = (int)Motion::Rotation;
            p.motionRate[0] = 0.5f;
            p.ellipse[0] = 0.8f;
            p.doppler = 1.0f;
        };
        scenes.push_back(s);
    }
    { // 11. tremulant
        Scene s{"11_tremulant_stereo", 0, 2, nullptr, {}, 6.0};
        for (int m : {53, 60, 65, 69})
            note(s.midiAll, m, 0.2, 5.5);
        s.params = [](EngineParams &p, double)
        {
            p.stopOn = {};
            p.stopOn[1] = true;
            p.tremOn = true;
            p.tremRate = 5.5f;
            p.tremDepth = 0.8f;
        };
        scenes.push_back(s);
    }
    { // 12. freeze: chord released at 1.5 s keeps sounding, freeze off at 6 s -> fades in 2 s
        Scene s{"12_freeze_stereo", 0, 2, nullptr, {}, 10.0};
        for (int m : {57, 64, 69, 72})
            note(s.midiAll, m, 0.2, 1.5);
        s.params = [](EngineParams &p, double t)
        {
            p.stopOn = {};
            p.stopOn[0] = p.stopOn[2] = true;
            p.clickLevel = 0;
            p.releaseClick = 0;
            p.wind = 0;
            p.freeze = t < 6.0;
            p.freezeSmear = 0.6f;
            p.freezeRelease = 2.0f;
        };
        scenes.push_back(s);
    }

    if (onlyWind)
        scenes.clear();
    for (auto &sc : scenes)
    {
        OrganEngine e;
        e.prepare(sr, block);
        e.setLibrary(lib);
        double rt = 0;
        int maxV = 0;
        auto b = render(e, sc, sr, block, rt, maxV);
        bool finite = true;
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (!std::isfinite(b.getSample(c, i)))
                {
                    finite = false;
                    break;
                }
        const float peak = b.getMagnitude(0, b.getNumSamples());
        const float rms0 = b.getRMSLevel(0, 0, b.getNumSamples());
        std::cout << sc.name << ": peak " << juce::Decibels::gainToDecibels(peak)
                  << " dBFS, rms ch0 " << juce::Decibels::gainToDecibels(rms0) << " dB, " << rt
                  << "x realtime, max voices " << maxV << std::endl;
        check(finite, sc.name + " finite");
        check(peak > 0.001f, sc.name + " not silent");
        // the 328-voice stress scene with per-pipe degradation is a worst case; on slow test
        // machines it may sit just below 1x
        check(rt > (sc.name.startsWith("05_stress") ? 0.7 : 1.0),
              sc.name + " faster than realtime");
        writeWav(outDir.getChildFile(sc.name + ".wav"), b, sr);
        if (sc.name.startsWith("12_freeze"))
        {
            const float held = b.getRMSLevel(0, (int)(4.0 * sr), (int)(1.0 * sr));
            const float after = b.getRMSLevel(0, (int)(9.0 * sr), (int)(0.9 * sr));
            std::cout << "  freeze: rms 4-5 s " << juce::Decibels::gainToDecibels(held)
                      << " dB, 9-10 s " << juce::Decibels::gainToDecibels(after) << " dB"
                      << std::endl;
            check(held > 0.01f && after < held * 0.01f,
                  "freeze holds after release and fades when switched off");
        }
    }
    // pitch check at a different host sample rate: Praestant a' must stay at its recorded pitch,
    // the substituted Gedackt f must land on meantone f
    if (!onlyWind)
        for (double hostSr : {44100.0, 48000.0, 96000.0})
            for (auto [stop, midi] : {std::pair<int, int>{0, 69}, std::pair<int, int>{1, 53},
                                      std::pair<int, int>{kPraestant16, 69}})
            {
                OrganEngine e;
                e.prepare(hostSr, 512);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[(size_t)stop] = true;
                p.clickLevel = 0;
                p.releaseClick = 0;
                p.wind = 0;
                p.stopPull = 0;
                juce::AudioBuffer<float> b(2, 512), all(1, (int)(hostSr * 2));
                for (int pos = 0; pos < all.getNumSamples(); pos += 512)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        mb.addEvent(juce::MidiMessage::noteOn(1, midi, (juce::uint8)100), 0);
                    e.process(b, mb, p);
                    all.copyFrom(0, pos, b, 0, 0, juce::jmin(512, all.getNumSamples() - pos));
                }
                // autocorrelation pitch on 0.5..1.5 s
                const float *x = all.getReadPointer(0) + (int)(0.5 * hostSr);
                const int N = (int)hostSr;
                const double f0nom =
                    meantoneHz(midi) *
                    (stop == kPraestant16 ? 0.5 : 1.0); // the 16' sounds an octave lower
                int bestLag = 0;
                double best = -1e9;
                for (int lag = (int)(hostSr / (f0nom * 1.1)); lag <= (int)(hostSr / (f0nom / 1.1));
                     ++lag)
                {
                    double s = 0;
                    for (int i = 0; i < N - lag; ++i)
                        s += (double)x[i] * x[i + lag];
                    if (s > best)
                    {
                        best = s;
                        bestLag = lag;
                    }
                }
                // parabolic refinement
                auto ac = [&](int lag)
                {
                    double s = 0;
                    for (int i = 0; i < N - lag; ++i)
                        s += (double)x[i] * x[i + lag];
                    return s;
                };
                const double y0 = ac(bestLag - 1), y1 = ac(bestLag), y2 = ac(bestLag + 1);
                const double lagF = bestLag + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2);
                const double f = hostSr / lagF;
                const double cents = 1200.0 * std::log2(f / f0nom);
                std::cout << "pitch " << kStops[(size_t)stop].id << " " << helmholtzName(midi)
                          << " @" << hostSr << ": " << f << " Hz (" << cents << " ct vs meantone)"
                          << std::endl;
                check(std::abs(cents) < 8.0, "pitch within 8 ct of meantone");
            }

    {
        auto measure = [&](int temperament, double pitchCents, int midi = 60, double expectHz = 0.0)
        {
            OrganEngine e;
            e.prepare(48000.0, 512);
            e.setLibrary(lib);
            EngineParams p;
            p.stopOn = {};
            p.stopOn[0] = true;
            p.clickLevel = 0;
            p.releaseClick = 0;
            p.wind = 0;
            p.tuning = makeKeyTuning(temperament);
            p.pitchCents = (float)pitchCents;
            juce::AudioBuffer<float> b(2, 512), all(1, 96000);
            for (int pos = 0; pos < all.getNumSamples(); pos += 512)
            {
                juce::MidiBuffer mb;
                if (pos == 0)
                    mb.addEvent(juce::MidiMessage::noteOn(1, midi, (juce::uint8)100), 0);
                e.process(b, mb, p);
                all.copyFrom(0, pos, b, 0, 0, juce::jmin(512, all.getNumSamples() - pos));
            }
            const float *x = all.getReadPointer(0) + 24000;
            const int N = 48000;
            if (expectHz <= 0.0)
                expectHz = 265.0;
            auto ac = [&](int lag)
            {
                double s2 = 0;
                for (int i = 0; i < N - lag; ++i)
                    s2 += (double)x[i] * x[i + lag];
                return s2;
            };
            int best = 0;
            double bv = -1e9;
            for (int lag = (int)(48000.0 / (expectHz * 1.15));
                 lag <= (int)(48000.0 / (expectHz / 1.15)); ++lag)
            {
                const double v = ac(lag);
                if (v > bv)
                {
                    bv = v;
                    best = lag;
                }
            }
            const double y0 = ac(best - 1), y1 = ac(best), y2 = ac(best + 1);
            return 48000.0 / (best + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2));
        };
        {
            // exotic tunings: a' is the reference (444.5 Hz as recorded)
            const double a1 = measure(0, 0, 69, 444.5);
            struct Case
            {
                int t, midi;
                double cents;
                const char *what;
            };
            const Case cases[]{
                {14, 70, 1200.0 / 31.0, "31-EDO step: b-flat' one 31st above a'"},
                {14, 66, -3 * 1200.0 / 31.0, "31-EDO step: 3 steps below a'"},
                {7, 64, 1200.0 * std::log2((5.0 / 4.0) / (5.0 / 3.0)),
                 "just intonation: e' = 5/4 over c, a' = 5/3"},
                {9, 70, 1200.0 * std::log2(28.0 / 27.0), "harmonic series: b-flat' = 7th partial"},
                {13, 64, 350.0 - 900.0, "Maqam Rast: e' a quarter tone low"},
                {17, 60, -9 * 1200.0 * std::log2(3.0) / 13.0, "Bohlen-Pierce: 9 steps below a'"}};
            for (auto &c : cases)
            {
                const double want = a1 * std::pow(2.0, c.cents / 1200.0);
                const double got = measure(c.t, 0, c.midi, want);
                const double err = 1200.0 * std::log2(got / want);
                std::cout << "tuning " << c.what << ": " << got << " Hz (want " << want << ", "
                          << err << " ct)" << std::endl;
                check(std::abs(err) < 3.0, juce::String("tuning: ") + c.what);
            }
            // step tunings use the nearest recorded pipe: inside the organ's range no pipe moves
            // more than ~70 ct
            const auto q = makeKeyTuning(14);
            float worst = 0;
            for (int k = 0; k < kNumKeys; ++k)
                worst = std::max(worst, std::abs(q.cents[(size_t)k]));
            std::cout << "31-EDO step: largest shift of a pipe " << worst << " ct" << std::endl;
            check(worst < 80.0f, "31-EDO step tuning picks the nearest pipes");
        }
        const double mt = measure(0, 0), eq = measure(6, 0), up = measure(0, 100);
        const double dEq = 1200 * std::log2(eq / mt), dUp = 1200 * std::log2(up / mt);
        std::cout << "tuning c': meantone " << mt << " Hz, equal " << eq << " Hz (" << dEq
                  << " ct), +100 ct " << dUp << " ct" << std::endl;
        check(std::abs(dEq - (-10.3)) < 1.0, "equal temperament moves c' by -10.3 ct");
        check(std::abs(dUp - 100.0) < 1.0, "pitch +100 ct");
    }
    {
        // tempo sync: phase locked to the host position
        OrganEngine e;
        e.prepare(48000.0, 512);
        EngineParams p;
        p.sync = true;
        p.playing = true;
        p.bpm = 90;
        p.ppq = 8.0;
        p.motionRate[2] = 0.25f;
        juce::AudioBuffer<float> b(2, 512);
        juce::MidiBuffer mb;
        e.process(b, mb, p);
        const double expect = 0.25 * (8.0 + 512.0 / 48000.0 * 90.0 / 60.0) * 0.5;
        check(std::abs(e.motionPhase[2].load() - expect) < 1.0e-9,
              "tempo sync: phase follows the host position");
    }

    // ---------------- wind model 2: a starving pipe whistles, stutters and hisses ----------------
    {
        const double wsr = 48000.0;
        auto renderWind = [&](int stop, int model, std::vector<int> midis,
                              std::function<float(double)> windAt, double secs)
        {
            OrganEngine e;
            e.prepare(wsr, 256);
            e.setLibrary(lib);
            EngineParams p;
            p.stopOn = {};
            p.stopOn[(size_t)stop] = true;
            p.clickLevel = 0;
            p.releaseClick = 0;
            p.wind = 0;
            p.stopPull = 0;
            p.windModel = model;
            juce::AudioBuffer<float> b(2, 256), all(2, (int)(wsr * secs));
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int pos = 0; pos < all.getNumSamples(); pos += 256)
            {
                juce::MidiBuffer mb;
                if (pos == 0)
                    for (int m : midis)
                        mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), 0);
                p.stopWind[(size_t)stop] = windAt(pos / wsr);
                e.process(b, mb, p);
                for (int c = 0; c < 2; ++c)
                    all.copyFrom(c, pos, b, c, 0, juce::jmin(256, all.getNumSamples() - pos));
            }
            const double took =
                std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0)
                    .count();
            return std::make_pair(all, secs / took);
        };
        auto highpassed = [&](const juce::AudioBuffer<float> &b)
        {
            std::vector<float> y((size_t)b.getNumSamples());
            juce::IIRFilter f1, f2;
            f1.setCoefficients(juce::IIRCoefficients::makeHighPass(wsr, 2500.0));
            f2.setCoefficients(juce::IIRCoefficients::makeHighPass(wsr, 2500.0));
            for (int i = 0; i < b.getNumSamples(); ++i)
                y[(size_t)i] =
                    f2.processSingleSampleRaw(f1.processSingleSampleRaw(b.getSample(0, i)));
            return y;
        };
        auto rmsDb = [&](const float *x, int n)
        {
            double s = 0;
            for (int i = 0; i < n; ++i)
                s += (double)x[i] * x[i];
            return 10.0 * std::log10(s / juce::jmax(1, n) + 1e-30);
        };
        auto starve = [](double t) { return t < 2.0 ? 1.0f : 0.12f; };
        for (int stop : {0, 1, 6})
        {
            const std::vector<int> chord{53, 60, 65};
            auto [m0, rt0] = renderWind(stop, 0, chord, starve, 8.0);
            auto [m1, rt1] = renderWind(stop, 1, chord, starve, 8.0);
            const juce::String id = kStops[(size_t)stop].id;
            writeWav(outDir.getChildFile("46_wind2_" + id + ".wav"), m1, wsr);
            // full wind: the new model changes nothing
            float maxDiff = 0.0f;
            for (int i = 0; i < (int)(1.9 * wsr); ++i)
                maxDiff = std::max(maxDiff, std::abs(m0.getSample(0, i) - m1.getSample(0, i)));
            check(maxDiff == 0.0f,
                  "wind model 2 at full wind is identical to the classic model (" + id + ")");
            bool finite = true;
            for (int i = 0; i < m1.getNumSamples(); ++i)
                finite = finite && std::isfinite(m1.getSample(0, i));
            check(finite, "wind model 2 finite (" + id + ")");
            const auto h0 = highpassed(m0), h1 = highpassed(m1);
            const int a = (int)(3.0 * wsr), n = (int)(5.0 * wsr);
            const double full = rmsDb(m0.getReadPointer(0) + (int)(0.8 * wsr), (int)wsr);
            const double low0 = rmsDb(m0.getReadPointer(0) + a, n),
                         low1 = rmsDb(m1.getReadPointer(0) + a, n);
            const double hf0 = rmsDb(h0.data() + a, n), hf1 = rmsDb(h1.data() + a, n);
            std::cout << "wind model 2 " << id << ": full wind " << full
                      << " dB; at 12 % wind classic " << low0 << " dB, new " << low1
                      << " dB; above 2.5 kHz classic " << hf0 << " dB, new " << hf1 << " dB; "
                      << rt1 << "x realtime" << std::endl;
            check(hf1 > hf0 + 2.0, "wind model 2 adds breath above the pipe at low wind (" + id + ")");
            check(low1 < full - 6.0,
                  "a starving pipe stays quieter than at full wind (" + id + ")");
        }
        // the edge tone of one pipe: strongest spectral peak above 1.2 kHz in a 4096-sample frame
        auto edgePeak = [&](const float *x, double lo = 1200.0,
                            double hi = 10000.0) -> std::pair<double, double>
        {
            juce::dsp::FFT fft(12);
            std::vector<float> b(8192, 0.0f);
            for (int i = 0; i < 4096; ++i)
                b[(size_t)i] = x[i] * (0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi *
                                                              (float)i / 4096.0f));
            fft.performFrequencyOnlyForwardTransform(b.data(), true);
            int best = 0;
            float bv = 0.0f;
            for (int k = (int)(lo * 4096 / wsr); k < (int)(hi * 4096 / wsr); ++k)
                if (b[(size_t)k] > bv)
                {
                    bv = b[(size_t)k];
                    best = k;
                }
            const double y0 = b[(size_t)best - 1], y1 = b[(size_t)best], y2 = b[(size_t)best + 1];
            const double kf = best + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2);
            return {kf * wsr / 4096.0, 20.0 * std::log10(bv + 1e-12)};
        };
        {
            // (to listen to: one pipe at a steady low wind, and with the wind sinking)
            auto [b, rt] =
                renderWind(0, 1, {60}, [](double t) { return t < 1.0 ? 1.0f : 0.22f; }, 8.0);
            writeWav(outDir.getChildFile("46_wind2_steady_c1.wav"), b, wsr);
            auto [c, rt2] = renderWind(
                0, 1, {60}, [](double t)
                { return (float)juce::jlimit(0.06, 1.0, 0.5 - 0.06 * (t - 1.0)); }, 8.0);
            writeWav(outDir.getChildFile("46_wind2_falling_c1.wav"), c, wsr);
            juce::ignoreUnused(rt, rt2, edgePeak);
        }
        {
            // The mouth tone itself (the model alone, without the pipe - through the engine the
            // pipe's own partials lie where it is). What is measured of a stretch of it: where its
            // weight lies (the middle of the band around its strongest line), how
            // loud it is, and how far its strongest line stands above its neighbourhood - a clean
            // whistle would stand 30 dB and more above it, breath stays far below.
            struct Mouth
            {
                double hz = 0, db = 0, lineDb = 0;
            };
            auto mouth = [&](uint32_t noiseSeed, uint32_t pipeSeed, float f0, float w0, float w1,
                             double seconds, double from, double to)
            {
                WindChaos ch;
                ch.reset(noiseSeed, pipeSeed, f0, (float)wsr);
                const int n = (int)(seconds * wsr);
                std::vector<float> x((size_t)n + 8192, 0.0f);
                for (int i = 0; i < n; ++i)
                {
                    const float w = w0 + (w1 - w0) * (float)i / (float)n;
                    if ((i & 15) == 0)
                        ch.control(w, f0, false, false, true, (float)wsr);
                    x[(size_t)i] = ch.idle ? 0.0f : ch.sample();
                }
                juce::dsp::FFT fft(12);
                std::vector<double> pw(2048, 0.0);
                double e = 0;
                int cnt = 0;
                for (int at = (int)(from * wsr); at + 4096 <= (int)(to * wsr); at += 2048)
                {
                    std::vector<float> b(8192, 0.0f);
                    for (int i = 0; i < 4096; ++i)
                    {
                        b[(size_t)i] = x[(size_t)(at + i)] *
                                       (0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi *
                                                               (float)i / 4096.0f));
                        e += (double)x[(size_t)(at + i)] * x[(size_t)(at + i)];
                        ++cnt;
                    }
                    fft.performFrequencyOnlyForwardTransform(b.data(), true);
                    for (int k = 0; k < 2048; ++k)
                        pw[(size_t)k] += (double)b[(size_t)k] * b[(size_t)k];
                }
                Mouth m;
                double sw = 0, sl = 0;
                int best = 0;
                const int k0 = (int)(150.0 * 4096 / wsr), k1 = (int)(9000.0 * 4096 / wsr);
                for (int k = k0; k < k1; ++k)
                {
                    sw += pw[(size_t)k];
                    sl += pw[(size_t)k] * std::log2((double)k * wsr / 4096.0);
                    if (pw[(size_t)k] > pw[(size_t)best])
                        best = k;
                }
                {
                    // (the mouth tone's own band: the weight around the strongest line - the pipe's
                    // general breath lies beside it and grows as the wind sinks)
                    sw = sl = 0;
                    for (int k = juce::jmax(k0, (int)(best / 1.12)); k <= juce::jmin(k1 - 1, (int)(best * 1.12)); ++k)
                    {
                        sw += pw[(size_t)k];
                        sl += pw[(size_t)k] * std::log2((double)k * wsr / 4096.0);
                    }
                }
                m.hz = std::exp2(sl / juce::jmax(1e-30, sw));
                m.db = 10.0 * std::log10(e / juce::jmax(1, cnt) + 1e-30);
                double around = 0;
                int na = 0;
                for (int k = juce::jmax(k0, best - 60); k < juce::jmin(k1, best + 60); ++k)
                    if (std::abs(k - best) > 3)
                    {
                        around += pw[(size_t)k];
                        ++na;
                    }
                m.lineDb = 10.0 * std::log10(pw[(size_t)best] / juce::jmax(1e-30, around / juce::jmax(1, na)));
                return m;
            };
            auto cents = [](double a, double b) { return 1200.0 * std::log2(a / b); };
            const float c1 = 262.0f;
            const uint32_t pipeC = 12345u, pipeE = 12349u;
            // a steady wind: it stays where it is and as loud as it is (first half against second)
            const auto s1 = mouth(1, pipeC, c1, 0.22f, 0.22f, 9.0, 1.0, 5.0),
                       s2 = mouth(1, pipeC, c1, 0.22f, 0.22f, 9.0, 5.0, 9.0);
            std::cout << "wind model 2, the mouth tone of c' at a steady 22 % wind: around " << s1.hz
                      << " Hz, later " << s2.hz << " Hz (" << cents(s2.hz, s1.hz) << " ct), level "
                      << s1.db << " / " << s2.db << " dB; its strongest line " << s1.lineDb
                      << " dB above its surroundings" << std::endl;
            check(std::abs(cents(s2.hz, s1.hz)) < 60.0 && std::abs(s2.db - s1.db) < 1.5,
                  "wind model 2: steady wind -> the mouth tone stays where it is");
            check(s1.hz > 1.5 * c1 && s1.hz < 9.0 * c1,
                  "wind model 2: the mouth tone lies a few partials above the pipe's note");
            check(s1.lineDb > 6.0 && s1.lineDb < 24.0,
                  "wind model 2: the mouth tone is breath with a core of tone, not a clean whistle");
            // the same pipe again (another note, other noise): the same mouth tone
            const auto again = mouth(977, pipeC, c1, 0.22f, 0.22f, 9.0, 1.0, 9.0),
                       whole = mouth(1, pipeC, c1, 0.22f, 0.22f, 9.0, 1.0, 9.0);
            std::cout << "wind model 2, c' struck twice: " << whole.hz << " / " << again.hz << " Hz"
                      << std::endl;
            check(std::abs(cents(again.hz, whole.hz)) < 60.0,
                  "wind model 2: the same pipe has the same mouth tone every time");
            // the wind sinks: it comes down and gets quieter
            const auto early = mouth(1, pipeC, c1, 0.28f, 0.05f, 8.0, 0.5, 2.5),
                       late = mouth(1, pipeC, c1, 0.28f, 0.05f, 8.0, 5.0, 7.0);
            std::cout << "wind model 2, c' with falling wind: " << early.hz << " Hz " << early.db
                      << " dB -> " << late.hz << " Hz " << late.db << " dB" << std::endl;
            check(late.hz < early.hz * 0.85 && late.db < early.db - 3.0,
                  "wind model 2: falling wind -> the mouth tone comes down and fades");
            // another pipe: another mouth tone
            const auto other = mouth(1, pipeE, 330.0f, 0.22f, 0.22f, 9.0, 1.0, 9.0);
            std::cout << "wind model 2, e' at 22 % wind: " << other.hz << " Hz (c' " << whole.hz
                      << " Hz)" << std::endl;
            check(std::abs(cents(other.hz / 330.0, whole.hz / (double)c1)) > 50.0,
                  "wind model 2: neighbouring pipes have their own mouth tones");
        }
        {
            // 18 pipes starving at once
            std::vector<int> many;
            for (int k = 0; k < 18; ++k)
                many.push_back(kPhysKeys[(size_t)(k * 2)]);
            auto [b, rt] = renderWind(
                0, 1, many, [](double t)
                { return (float)juce::jlimit(0.0, 1.0, 1.0 - t / 4.0) * 0.9f + 0.05f; }, 6.0);
            writeWav(outDir.getChildFile("46_wind2_sweep_18_pipes.wav"), b, wsr);
            std::cout << "wind model 2, 18 pipes sweeping down: " << rt << "x realtime, peak "
                      << juce::Decibels::gainToDecibels(b.getMagnitude(0, b.getNumSamples()))
                      << " dBFS" << std::endl;
            check(rt > 1.0, "wind model 2 with 18 pipes faster than realtime");
        }
        // ---------------- free tuning (Scala, MTS-ESP) and the new modulation sources
        // ----------------
        {
            // Scala parser
            {
                ScalaTuning t;
                juce::String err;
                const bool ok = ScalaTuning::parseScl(
                    "! test.scl\n!\n31-EDO\n 31\n!\n" +
                        []
                        {
                            juce::String s;
                            for (int i = 1; i <= 31; ++i)
                                s << juce::String(1200.0 * i / 31.0, 5) << "\n";
                            return s;
                        }(),
                    t, err);
                const auto f = t.frequencies();
                const double c70 = 1200.0 * std::log2(f[70] / f[69]),
                             c38 = 1200.0 * std::log2(f[69 + 31] / f[69]);
                std::cout << "scala: 31edo parsed " << ok << ", C4 " << f[60] << " Hz, one step "
                          << c70 << " ct, 31 steps " << c38 << " ct" << std::endl;
                check(ok && std::abs(f[60] - kScalaDefaultReferenceHz) < 1e-6 &&
                          std::abs(c70 - 1200.0 / 31.0) < 1e-3 &&
                          std::abs(c38 - 1200.0) < 1e-3,
                      "scala: 31edo, degree zero at C4 from 12edo A4 = 440 Hz");
                ScalaTuning j;
                juce::String e2;
                const bool ok2 =
                    ScalaTuning::parseScl("just\n7\n9/8\n5/4\n4/3\n3/2\n5/3\n15/8\n2/1\n", j, e2) &&
                    ScalaTuning::parseKbm(
                        "! white keys "
                        "only\n12\n0\n127\n60\n69\n440.0\n7\n0\nx\n1\nx\n2\n3\nx\n4\nx\n5\nx\n6\n",
                        j, e2);
                const auto g = j.frequencies();
                std::cout << "scala: just C major on the white keys: c' " << g[60] << ", e' "
                          << g[64] << ", g' " << g[67] << ", c'' " << g[72] << ", c#' " << g[61]
                          << std::endl;
                check(ok2 && std::abs(g[69] - 440.0) < 1e-6 &&
                          std::abs(g[64] / g[60] - 1.25) < 1e-9 &&
                          std::abs(g[67] / g[60] - 1.5) < 1e-9 &&
                          std::abs(g[72] / g[60] - 2.0) < 1e-9 && g[61] == 0.0,
                      "scala: ratios, .kbm with unmapped keys and reference frequency");
                const bool customReference = ScalaTuning::parseKbm(
                    "0\n0\n127\n55\n57\n432.1255\n0\n", j, e2);
                check(customReference && j.middleNote == 55 && j.refNote == 57 &&
                          std::abs(j.frequencies()[57] - 432.1255) < 1.0e-9,
                      "scala: KBM reference note and frequency are independent of the middle note");
                j.clearKbm();
                check(!j.hasKbm && j.middleNote == 60 && j.refNote == 60 &&
                          std::abs(j.frequencies()[60] - kScalaDefaultReferenceHz) < 1.0e-9,
                      "scala: removing KBM restores the C4 reference");
                ScalaTuning bad;
                juce::String e3;
                check(!ScalaTuning::parseScl("bad\n3\n100.0\n", bad, e3) && e3.isNotEmpty(),
                      "scala: a broken file is refused with a message");
            }
            // free tuning in the engine
            auto pitchOf = [&](const juce::AudioBuffer<float> &b, double f0)
            {
                const float *x = b.getReadPointer(0) + (int)(0.6 * 48000);
                const int N = 24000;
                auto ac = [&](int lag)
                {
                    double s2 = 0;
                    for (int i = 0; i < N - lag; ++i)
                        s2 += (double)x[i] * x[i + lag];
                    return s2;
                };
                int best = 0;
                double bv = -1e18;
                for (int lag = (int)(48000.0 / (f0 * 1.1)); lag <= (int)(48000.0 / (f0 / 1.1));
                     ++lag)
                {
                    const double v = ac(lag);
                    if (v > bv)
                    {
                        bv = v;
                        best = lag;
                    }
                }
                const double y0 = ac(best - 1), y1 = ac(best), y2 = ac(best + 1);
                return 48000.0 / (best + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2));
            };
            NoteTable table;
            for (int ch = 0; ch < 16; ++ch)
                for (int n = 0; n < 128; ++n)
                {
                    table.hz[(size_t)(ch * 128 + n)] =
                        (float)(kA4 * std::pow(2.0, (n - 69 + ch * 0.5) / 31.0 *
                                                        1.0)); // 31-EDO, channel 2 half a step up
                    table.mapped[(size_t)(ch * 128 + n)] = n == 72 ? 0 : 1;
                }
            auto play = [&](std::vector<std::pair<int, int>> notes,
                            std::function<void(EngineParams &)> setup, double secs,
                            int *voices = nullptr)
            {
                OrganEngine e;
                e.prepare(48000.0, 512);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[0] = true;
                p.clickLevel = 0;
                p.releaseClick = 0;
                p.wind = 0;
                p.stopPull = 0;
                p.notes = &table;
                setup(p);
                juce::AudioBuffer<float> b(2, 512), all(1, (int)(48000 * secs));
                for (int pos = 0; pos < all.getNumSamples(); pos += 512)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        for (auto [ch, n] : notes)
                            mb.addEvent(juce::MidiMessage::noteOn(ch, n, (juce::uint8)100), 0);
                    e.process(b, mb, p);
                    if (voices != nullptr && pos == 512 * 10)
                        *voices = e.activeVoices.load();
                    all.copyFrom(0, pos, b, 0, 0, juce::jmin(512, all.getNumSamples() - pos));
                }
                return all;
            };
            auto none = [](EngineParams &) {};
            double worst = 0;
            for (int n : {60, 69, 70, 71, 80})
            {
                const double want = table.hz[(size_t)n];
                const double got = pitchOf(play({{1, n}}, none, 1.5), want);
                worst = std::max(worst, std::abs(1200.0 * std::log2(got / want)));
            }
            int v2 = 0, vUnmapped = 0, vFar = 0, vCh = 0;
            play({{1, 69}, {1, 70}}, none, 0.5, &v2); // 38.7 ct apart: the same pipe, two voices
            play({{1, 72}}, none, 0.5, &vUnmapped);
            play({{1, 127}}, none, 0.5, &vFar); // 31-EDO note 127: 22 ct below a'' + 1 oct ->
                                                // shifted from a'' (allowed: up to 2 octaves)
            play({{1, 60}, {2, 60}}, none, 0.5, &vCh);
            const double gotCh2 = pitchOf(play({{2, 69}}, none, 1.5), table.hz[128 + 69]);
            const double errCh2 = 1200.0 * std::log2(gotCh2 / table.hz[128 + 69]);
            std::cout << "free tuning (31-EDO table): worst pitch error " << worst
                      << " ct; a' + one step together: " << v2
                      << " voices; unmapped note: " << vUnmapped << "; far above: " << vFar
                      << "; same note on two channels: " << vCh << " voices, channel 2 error "
                      << errCh2 << " ct" << std::endl;
            check(worst < 5.0, "free tuning: every MIDI note sounds at its own pitch (within the "
                               "recordings' own accuracy)");
            check(v2 == 2, "free tuning: two notes on one pipe sound as two voices");
            check(vUnmapped == 0, "free tuning: unmapped notes stay silent");
            check(vCh == 2 && std::abs(errCh2) < 3.0,
                  "free tuning: MIDI channels have their own pitches (multi-channel tables)");
            // velocity and pitch wheel as modulation sources
            auto levelAt = [&](int vel)
            {
                OrganEngine e;
                e.prepare(48000.0, 512);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[0] = true;
                p.clickLevel = 0;
                p.releaseClick = 0;
                p.wind = 0;
                p.stopPull = 0;
                p.mod.routes[0] = {srcVelocity, tgtLevel, 0.3f};
                juce::AudioBuffer<float> b(2, 512);
                double en = 0;
                for (int pos = 0; pos < 48000; pos += 512)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)vel), 0);
                    e.process(b, mb, p);
                    if (pos > 24000)
                        en += b.getRMSLevel(0, 0, 512);
                }
                return juce::Decibels::gainToDecibels((float)en);
            };
            const float soft = levelAt(20), hard = levelAt(127);
            OrganEngine e;
            e.prepare(48000.0, 512);
            e.setLibrary(lib);
            EngineParams p;
            p.stopOn = {};
            p.stopOn[0] = true;
            p.clickLevel = 0;
            p.releaseClick = 0;
            p.wind = 0;
            p.stopPull = 0;
            p.mod.routes[0] = {srcPitchWheel, tgtPitch, 1.0f / 6.0f}; // full wheel = +2 semitones
            juce::AudioBuffer<float> b(2, 512), all(1, 72000);
            for (int pos = 0; pos < all.getNumSamples(); pos += 512)
            {
                juce::MidiBuffer mb;
                if (pos == 0)
                    mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)100), 0);
                if (pos == 0)
                    mb.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 1);
                e.process(b, mb, p);
                all.copyFrom(0, pos, b, 0, 0, juce::jmin(512, all.getNumSamples() - pos));
            }
            const double bent =
                1200.0 * std::log2(pitchOf(all, kA4 * std::pow(2.0, 2.0 / 12.0)) / kA4);
            std::cout << "modulation: velocity 20 -> " << soft << " dB, 127 -> " << hard
                      << " dB; pitch wheel up: " << bent << " ct" << std::endl;
            check(hard - soft > 8.0f, "modulation source: key velocity");
            check(std::abs(bent - 200.0) < 4.0, "modulation source: pitch wheel");

            // MPE: two notes on their own channels; channel 2 bends its note +2 semitones (range
            // 48), channel 3 stays; pressure on channel 3 makes only that note louder; MPE off: the
            // same bend does nothing to the pitch
            auto mpePlay = [&](bool mpe, int onlyCh,
                               std::function<void(juce::MidiBuffer &, int)> extra,
                               EngineParams *out = nullptr)
            {
                OrganEngine mpeEngine;
                mpeEngine.prepare(48000.0, 512);
                mpeEngine.setLibrary(lib);
                EngineParams mpeParams;
                mpeParams.stopOn = {};
                mpeParams.stopOn[0] = true;
                mpeParams.clickLevel = 0;
                mpeParams.releaseClick = 0;
                mpeParams.wind = 0;
                mpeParams.stopPull = 0;
                mpeParams.mpe = mpe;
                mpeParams.mpeBendRange = 48.0f;
                mpeParams.mod.routes[0] = {srcPressure, tgtLevel, 0.4f};
                if (out != nullptr)
                    *out = mpeParams;
                juce::AudioBuffer<float> blockBuffer(2, 512), rendered(1, 72000);
                for (int pos = 0; pos < rendered.getNumSamples(); pos += 512)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                    {
                        if (onlyCh == 0 || onlyCh == 2)
                            mb.addEvent(juce::MidiMessage::noteOn(2, 69, (juce::uint8)100), 0);
                        if (onlyCh == 0 || onlyCh == 3)
                            mb.addEvent(juce::MidiMessage::noteOn(3, 57, (juce::uint8)100), 0);
                    }
                    extra(mb, pos);
                    mpeEngine.process(blockBuffer, mb, mpeParams);
                    rendered.copyFrom(0, pos, blockBuffer, 0, 0,
                                      juce::jmin(512, rendered.getNumSamples() - pos));
                }
                return rendered;
            };
            const int bend2 = 8192 + juce::roundToInt(8191.0 * 2.0 / 48.0);
            auto bendCh2 = [&](juce::MidiBuffer &mb, int pos)
            {
                if (pos == 4096)
                    mb.addEvent(juce::MidiMessage::pitchWheel(2, bend2), 0);
            };
            const double mpeBent = 1200.0 * std::log2(pitchOf(mpePlay(true, 2, bendCh2),
                                                              kA4 * std::pow(2.0, 2.0 / 12.0)) /
                                                      kA4);
            const double other =
                1200.0 * std::log2(pitchOf(mpePlay(true, 3, bendCh2), kA4 / 2.0) / (kA4 / 2.0));
            const double offBent =
                1200.0 * std::log2(pitchOf(mpePlay(false, 2, bendCh2), kA4) / kA4);
            auto rmsLate = [](const juce::AudioBuffer<float> &buffer)
            { return juce::Decibels::gainToDecibels(buffer.getRMSLevel(0, 36000, 30000)); };
            auto press3 = [&](juce::MidiBuffer &mb, int pos)
            {
                if (pos == 4096)
                    mb.addEvent(juce::MidiMessage::channelPressureChange(3, 127), 0);
            };
            const float quiet2 = rmsLate(mpePlay(true, 2, [](juce::MidiBuffer &, int) {})),
                        pressed2 = rmsLate(mpePlay(true, 2, press3));
            const float quiet3 = rmsLate(mpePlay(true, 3, [](juce::MidiBuffer &, int) {})),
                        pressed3 = rmsLate(mpePlay(true, 3, press3));
            auto polyAt = [&](juce::MidiBuffer &mb, int pos)
            {
                if (pos == 4096)
                    mb.addEvent(juce::MidiMessage::aftertouchChange(3, 57, 127), 0);
            };
            const float poly3 = rmsLate(mpePlay(false, 3, polyAt));
            const float plain3 = rmsLate(mpePlay(false, 3, [](juce::MidiBuffer &, int) {}));
            std::cout << "MPE: channel 2 bent +2 HT -> " << mpeBent
                      << " ct; channel 3 note meanwhile " << other << " ct; MPE off: " << offBent
                      << " ct; pressure on ch 3: ch 2 note " << pressed2 - quiet2
                      << " dB, ch 3 note " << pressed3 - quiet3 << " dB; poly aftertouch (MPE off) "
                      << poly3 - plain3 << " dB" << std::endl;
            check(std::abs(mpeBent - 200.0) < 5.0 && std::abs(other) < 5.0,
                  "MPE: every note channel bends its own note");
            check(std::abs(offBent) < 5.0, "MPE off: pitch bend does not bend (unchanged)");
            check(std::abs(pressed2 - quiet2) < 0.5f && pressed3 - quiet3 > 6.0f,
                  "MPE: pressure modulates only its own note");
            check(poly3 - plain3 > 6.0f, "polyphonic aftertouch as pressure");
        }

        testLucier(lib);
        testInput(lib);
        testRoomsAndSpread(lib);
        testKalkantPatch(lib, outDir);
        testPipeTargets(lib);
        testStopEffects(lib);

        // ---------------- motion: ellipse shape, drawn paths, hard jumps ----------------
        {
            // ellipse: near side where the direction says, 1 or 2 near sides, tilt
            auto distAt = [](float az, float dir, int lobes, float tilt, float &el)
            {
                MotionShape sh;
                sh.ellipse = 0.5f;
                sh.ellipseDir = dir;
                sh.ellipseLobes = lobes;
                sh.ellipseTilt = tilt;
                const Pos p = applyMotion({az, 10.0f, 6.0f}, Motion::Static, 0.0, 0.0f, 1u, sh);
                el = p.el;
                return p.dist;
            };
            float el = 0;
            const float near1 = distAt(90, 90, 1, 0, el), far1 = distAt(-90, 90, 1, 0, el),
                        side1 = distAt(0, 90, 1, 0, el);
            const float n2a = distAt(90, 90, 2, 0, el), n2b = distAt(-90, 90, 2, 0, el),
                        s2 = distAt(0, 90, 2, 0, el);
            float elNear = 0, elFar = 0;
            distAt(30, 30, 1, 20, elNear);
            distAt(-150, 30, 1, 20, elFar);
            MotionShape old;
            old.ellipse = 0.5f;
            const float oldFormula = 6.0f * (1.0f - 0.5f * std::cos(juce::degreesToRadians(10.0f)) *
                                                        std::cos(juce::degreesToRadians(40.0f)));
            const float now =
                applyMotion({40.0f, 10.0f, 6.0f}, Motion::Static, 0.0, 0.0f, 1u, old).dist;
            std::cout << "ellipse: dir 90: near " << near1 << " m, far " << far1 << " m, side "
                      << side1 << "; two near sides " << n2a << " / " << n2b << ", side " << s2
                      << "; tilt 20: near side el " << elNear << ", far side " << elFar
                      << std::endl;
            check(near1 < side1 && side1 < far1, "ellipse: the near side follows the direction");
            check(std::abs(n2a - n2b) < 1e-4f && n2a < s2, "ellipse form 2: near on both sides");
            check(std::abs(elNear - (10.0f + 20.0f * std::cos(juce::degreesToRadians(10.0f)))) <
                          0.01f &&
                      elFar < 10.0f - 15.0f,
                  "ellipse: tilted orbit");
            check(std::abs(now - oldFormula) < 1e-4f,
                  "ellipse: default settings unchanged (older patches sound the same)");

            // drawn path: a square loop; the pipes follow it, spread along it
            MotionPath path;
            const float corners[4][2]{{0, 0}, {90, 0}, {180, 0}, {-90, 0}};
            for (int i = 0; i < 4; ++i)
            {
                const float a = juce::degreesToRadians(corners[i][0]);
                path.x[(size_t)i] = std::cos(a);
                path.y[(size_t)i] = std::sin(a);
                path.z[(size_t)i] = 0.0f;
                path.u[(size_t)i] = (float)i / 4.0f;
            }
            path.x[4] = 1.0f;
            path.y[4] = 0.0f;
            path.z[4] = 0.0f;
            path.u[4] = 1.0f;
            path.n = 5;
            MotionShape sh;
            sh.path = &path;
            sh.pathSpread = 0.0f;
            const Pos q0 = applyMotion({33.0f, 20.0f, 7.0f}, Motion::Drawn, 0.25, 0.0f, 1u, sh);
            const Pos q1 = applyMotion({33.0f, 20.0f, 7.0f}, Motion::Drawn, 0.625, 0.0f, 1u, sh);
            sh.pathSpread = 1.0f;
            sh.keyFrac = 0.5f;
            const Pos q2 = applyMotion({33.0f, 20.0f, 7.0f}, Motion::Drawn, 0.0, 0.0f, 1u, sh);
            path.pingPong = true;
            sh.pathSpread = 0.0f;
            const Pos q3 = applyMotion({33.0f, 20.0f, 7.0f}, Motion::Drawn, 0.75, 0.0f, 1u, sh);
            std::cout << "drawn path: at 1/4 az " << q0.az << ", at 5/8 az " << q1.az
                      << " (between 180 and -90), spread pipe at 1/2: az " << q2.az
                      << ", there-and-back at 3/4: az " << q3.az << ", distance kept " << q0.dist
                      << " m" << std::endl;
            check(std::abs(q0.az - 90.0f) < 0.5f && q1.az < -90.0f && q1.az > -180.0f &&
                      std::abs(std::abs(q2.az) - 180.0f) < 0.5f &&
                      std::abs(std::abs(q3.az) - 180.0f) < 0.5f && std::abs(q0.dist - 7.0f) < 1e-4f,
                  "drawn path: pipes follow the path (loop, spread, there and back)");

            // hard jumps: same jump rate, with and without the glide: the hard ones click (much
            // more energy above 8 kHz)
            auto jumps = [&](bool hard)
            {
                OrganEngine e;
                e.prepare(48000.0, 512);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[1] = true;
                p.clickLevel = 0;
                p.releaseClick = 0;
                p.wind = 0;
                p.stopPull = 0;
                p.motionMode[1] = (int)Motion::Jumps;
                p.motionRate[1] = 150.0f;
                p.motionDepth[1] = 120.0f;
                p.jumpHard[1] = hard;
                juce::AudioBuffer<float> b(2, 512), all(2, 48000 * 2);
                for (int pos = 0; pos < all.getNumSamples(); pos += 512)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 53, (juce::uint8)100), 0);
                    e.process(b, mb, p);
                    for (int c = 0; c < 2; ++c)
                        all.copyFrom(c, pos, b, c, 0, juce::jmin(512, all.getNumSamples() - pos));
                }
                writeWav(outDir.getChildFile(hard ? "47_jumps_hard_150hz.wav"
                                                  : "47_jumps_soft_150hz.wav"),
                         all, 48000.0);
                juce::IIRFilter f1, f2;
                f1.setCoefficients(juce::IIRCoefficients::makeHighPass(48000.0, 8000.0));
                f2.setCoefficients(juce::IIRCoefficients::makeHighPass(48000.0, 8000.0));
                double eh = 0, et = 0;
                bool finite = true;
                for (int i = 24000; i < all.getNumSamples(); ++i)
                {
                    const float x = all.getSample(0, i);
                    finite = finite && std::isfinite(x);
                    const float h = f2.processSingleSampleRaw(f1.processSingleSampleRaw(x));
                    eh += (double)h * h;
                    et += (double)x * x;
                }
                return finite ? 10.0 * std::log10(eh / juce::jmax(1e-30, et)) : 999.0;
            };
            const double soft = jumps(false), hard = jumps(true);
            std::cout << "jumps at 150 Hz: energy above 8 kHz relative to all: glided " << soft
                      << " dB, hard " << hard << " dB" << std::endl;
            check(hard < 900.0 && hard > soft + 10.0, "hard jumps: they click (and stay finite)");
            // very fast rotation stays clean (finite)
            {
                OrganEngine e;
                e.prepare(48000.0, 512);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[0] = true;
                p.motionMode[0] = (int)Motion::Rotation;
                p.motionRate[0] = 500.0f;
                p.ellipse[0] = 0.8f;
                juce::AudioBuffer<float> b(2, 512);
                bool finite = true;
                for (int pos = 0; pos < 48000; pos += 512)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)100), 0);
                    e.process(b, mb, p);
                    for (int i = 0; i < 512; ++i)
                        finite = finite && std::isfinite(b.getSample(0, i));
                }
                check(finite, "rotation at 500 Hz with an ellipse stays finite");
            }
        }

        if (onlyWind && !onlyRoom)
        {
            std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string(failures))
                      << std::endl;
            return failures;
        }
    }

    // ---------------- modulation ----------------
    {
        const double msr = 48000.0;
        auto run = [&](std::function<void(EngineParams &)> setup, std::vector<int> midis,
                       double secs, int mode, int ch, double offAt, const juce::String &name)
        {
            OrganEngine e;
            e.prepare(msr, 512);
            e.setLibrary(lib);
            EngineParams p;
            p.stopOn = {};
            p.stopOn[0] = true;
            p.clickLevel = 0;
            p.releaseClick = 0;
            p.wind = 0;
            p.stopPull = 0;
            p.outputMode = mode;
            setup(p);
            juce::AudioBuffer<float> b(ch, 512), all(ch, (int)(msr * secs));
            for (int pos = 0; pos < all.getNumSamples(); pos += 512)
            {
                juce::MidiBuffer mb;
                if (pos == 0)
                    for (int m : midis)
                        mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), 0);
                if (offAt > 0 && pos == ((int)(offAt * msr) / 512) * 512)
                    for (int m : midis)
                        mb.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                e.process(b, mb, p);
                for (int c = 0; c < ch; ++c)
                    all.copyFrom(c, pos, b, c, 0, juce::jmin(512, all.getNumSamples() - pos));
            }
            writeWav(outDir.getChildFile(name + ".wav"), all, msr);
            return all;
        };
        auto rms = [&](const juce::AudioBuffer<float> &b, double t0, double t1)
        {
            return juce::Decibels::gainToDecibels(
                b.getRMSLevel(0, (int)(t0 * msr), (int)((t1 - t0) * msr)));
        };
        auto pitchAt = [&](const juce::AudioBuffer<float> &b, double t0, double len, double f0)
        {
            const float *x = b.getReadPointer(0) + (int)(t0 * msr);
            const int N = (int)(len * msr);
            auto ac = [&](int lag)
            {
                double s = 0;
                for (int i = 0; i < N - lag; ++i)
                    s += (double)x[i] * x[i + lag];
                return s;
            };
            int bestLag = 0;
            double best = -1e18;
            for (int lag = (int)(msr / (f0 * 1.15)); lag <= (int)(msr / (f0 / 1.15)); ++lag)
            {
                const double v = ac(lag);
                if (v > best)
                {
                    best = v;
                    bestLag = lag;
                }
            }
            const double y0 = ac(bestLag - 1), y1 = ac(bestLag), y2 = ac(bestLag + 1);
            return msr / (bestLag + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2));
        };

        { // LFO square 1 Hz -> pipe level +-10.8 dB
            auto b = run(
                [](EngineParams &p)
                {
                    p.mod.lfo[0].shape = shSquare;
                    p.mod.lfo[0].rate = 1.0f;
                    p.mod.routes[0] = {srcLfo1, tgtLevel, 0.3f};
                },
                {69}, 3.0, 0, 2, -1, "14_mod_lfo_level");
            const float hi = rms(b, 1.1, 1.4), lo = rms(b, 1.6, 1.9);
            std::cout << "mod level: high " << hi << " dB, low " << lo << " dB" << std::endl;
            check(hi - lo > 18.0f && hi - lo < 25.0f, "LFO -> pipe level (expect 21.6 dB swing)");
        }
        { // LFO square 1 Hz -> pitch +-24 ct
            auto b = run(
                [](EngineParams &p)
                {
                    p.mod.lfo[1].shape = shSquare;
                    p.mod.lfo[1].rate = 1.0f;
                    p.mod.routes[3] = {srcLfo2, tgtPitch, 0.02f};
                },
                {69}, 3.0, 0, 2, -1, "15_mod_lfo_pitch");
            const double fHi = pitchAt(b, 1.1, 0.3, meantoneHz(69)),
                         fLo = pitchAt(b, 1.6, 0.3, meantoneHz(69));
            const double d = 1200.0 * std::log2(fHi / fLo);
            std::cout << "mod pitch: " << fHi << " / " << fLo << " Hz = " << d << " ct"
                      << std::endl;
            check(std::abs(d - 48.0) < 3.0, "LFO -> pipe pitch (expect 48 ct)");
        }
        { // per-pipe envelope -> distance: far at the envelope peak, back to base when it has
          // decayed
            auto b = run(
                [](EngineParams &p)
                {
                    p.mod.env[0] = {true, 0.0f, 0.4f, 0.8f, 0.0f, 1.0f};
                    p.mod.routes[5] = {srcEnv1, tgtDist, 0.5f};
                },
                {57, 64, 69}, 4.0, 1, 4, -1, "16_mod_env_distance");
            const float farL = rms(b, 0.4, 0.5), nearL = rms(b, 3.0, 3.5);
            std::cout << "mod env->distance: at peak " << farL << " dB, later " << nearL << " dB"
                      << std::endl;
            check(nearL - farL > 4.0f, "per-pipe envelope -> distance");
        }
        { // global envelope with release: level follows the key
            auto b = run(
                [](EngineParams &p)
                {
                    p.mod.env[1] = {false, 0.0f, 0.01f, 0.1f, 1.0f, 0.5f};
                    p.mod.routes[7] = {srcEnv2, tgtLevel, 0.0f};
                },
                {69}, 2.0, 0, 2, 1.0, "17_mod_global_env");
            check(rms(b, 0.5, 0.9) > -60.0f, "global envelope scene renders");
        }
        { // the wave: sine LFO 0.5 Hz on direction with full keyboard spread (listen in AmbiX)
            auto b = run(
                [](EngineParams &p)
                {
                    p.mod.lfo[2].rate = 0.5f;
                    p.mod.lfo[2].spread = 1.0f;
                    p.mod.routes[2] = {srcLfo3, tgtAz, 0.5f};
                    p.mod.lfo[3].shape = shSmooth;
                    p.mod.lfo[3].rate = 3.0f;
                    p.mod.lfo[3].spread = 1.0f;
                    p.mod.lfo[3].spreadMode = 1;
                    p.mod.routes[9] = {srcLfo4, tgtBright, -0.4f};
                },
                {45, 48, 52, 57, 60, 64, 69, 72, 76}, 8.0, 3, 16, -1, "18_mod_wave_ambix3");
            check(rms(b, 1.0, 7.0) > -60.0f, "spread wave scene renders");
        }
        { // spread maths: offset along the keyboard
            check(std::abs(lfoValue(shSine, 0.25, 0) - 1.0f) < 1e-6f &&
                      std::abs(lfoValue(shTriangle, 0.75, 0) + 1.0f) < 1e-6f,
                  "LFO shapes");
            EnvState es;
            EnvParams ep{true, 0.0f, 0.1f, 0.1f, 0.5f, 0.1f};
            es.trigger();
            for (int i = 0; i < 100; ++i)
                es.advance(ep, 0.01f);
            const float sus = es.level;
            es.releaseGate();
            for (int i = 0; i < 200; ++i)
                es.advance(ep, 0.01f);
            check(std::abs(sus - 0.5f) < 0.01f && es.level == 0.0f, "envelope sustain + release");
        }
    }

    // ---------------- effects (page "Effekte") ----------------
    {
        const double fsr = 48000.0;
        constexpr int B = 512;
        auto run = [&](std::function<void(EngineParams &)> setup, std::vector<int> midis,
                       double secs, double offAt, const juce::String &name,
                       OrganEngine *keep = nullptr)
        {
            OrganEngine local;
            OrganEngine &e = keep != nullptr ? *keep : local;
            e.prepare(fsr, B);
            e.setLibrary(lib);
            EngineParams p;
            p.stopOn = {};
            p.stopOn[0] = true;
            p.clickLevel = 0;
            p.releaseClick = 0;
            p.wind = 0;
            p.stopPull = 0;
            p.outputMode = 1;
            setup(p);
            juce::AudioBuffer<float> b(4, B), all(4, (int)(fsr * secs));
            for (int pos = 0; pos < all.getNumSamples(); pos += B)
            {
                juce::MidiBuffer mb;
                if (pos == 0)
                    for (int m : midis)
                        mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), 0);
                if (offAt > 0 && pos == ((int)(offAt * fsr) / B) * B)
                    for (int m : midis)
                        mb.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                e.process(b, mb, p);
                for (int c = 0; c < 4; ++c)
                    all.copyFrom(c, pos, b, c, 0, juce::jmin(B, all.getNumSamples() - pos));
            }
            writeWav(outDir.getChildFile(name + ".wav"), all, fsr);
            return all;
        };
        // direction of the energy (intensity vector) in a window, optionally above a frequency
        struct Dir
        {
            float az, el, dirness, firstOrderRatio, db;
        };
        auto direction =
            [&](const juce::AudioBuffer<float> &in, double t0, double t1, float hpHz = 0.0f)
        {
            const int a = (int)(t0 * fsr), n = (int)((t1 - t0) * fsr);
            std::array<std::vector<float>, 4> x;
            for (int c = 0; c < 4; ++c)
            {
                x[(size_t)c].assign(in.getReadPointer(c) + a, in.getReadPointer(c) + a + n);
                if (hpHz > 0.0f)
                {
                    // 4th order: two passes of a 2nd order Butterworth high-pass
                    for (int pass = 0; pass < 2; ++pass)
                    {
                        auto co = juce::IIRCoefficients::makeHighPass(fsr, hpHz);
                        juce::IIRFilter f;
                        f.setCoefficients(co);
                        f.processSamples(x[(size_t)c].data(), n);
                    }
                }
            }
            double ww = 0, wy = 0, wz = 0, wx = 0, e1 = 0;
            for (int i = 0; i < n; ++i)
            {
                const double w = x[0][(size_t)i];
                ww += w * w;
                wy += w * x[1][(size_t)i];
                wz += w * x[2][(size_t)i];
                wx += w * x[3][(size_t)i];
                e1 += (double)x[1][(size_t)i] * x[1][(size_t)i] +
                      (double)x[2][(size_t)i] * x[2][(size_t)i] +
                      (double)x[3][(size_t)i] * x[3][(size_t)i];
            }
            Dir d;
            d.az = (float)juce::radiansToDegrees(std::atan2(wy, wx));
            d.el = (float)juce::radiansToDegrees(std::atan2(wz, std::sqrt(wx * wx + wy * wy)));
            d.dirness = (float)(std::sqrt(wx * wx + wy * wy + wz * wz) / juce::jmax(1.0e-20, ww));
            d.firstOrderRatio = (float)(e1 / juce::jmax(1.0e-20, ww));
            d.db = juce::Decibels::gainToDecibels((float)std::sqrt(ww / juce::jmax(1, n)), -200.0f);
            return d;
        };
        auto angleDiff = [](float a, float b) { return std::abs(wrapAz(a - b)); };
        OrganEngine probe;
        probe.prepare(fsr, B);
        probe.setLibrary(lib);
        const EngineParams dflt;
        int keyA = -1;
        for (int k = 0; k < kNumKeys; ++k)
            if (kPhysKeys[(size_t)k] == 69)
                keyA = k;
        const Pos posA = probe.currentPosition(0, keyA, dflt);
        std::cout << "pipe a' (Praestant) at az " << posA.az << ", el " << posA.el << std::endl;

        { // wandering echoes: 1st repeat from +90 deg, 2nd from 180 deg
            auto b = run(
                [](EngineParams &p)
                {
                    p.fx.echoLevel = 1.0f;
                    p.fx.echoTime = 1.5f;
                    p.fx.echoRepeats = 2;
                    p.fx.echoFeedback = 1.0f;
                    p.fx.echoTurn = 90.0f;
                    p.fx.echoDamp = 0.0f;
                },
                {69}, 3.8, 0.35, "30_fx_echo_ambix1");
            const auto d0 = direction(b, 0.05, 0.3), d1 = direction(b, 1.55, 1.8),
                       d2 = direction(b, 3.05, 3.3);
            std::cout << "echo: dry az " << d0.az << ", 1st " << d1.az << " (" << d1.db
                      << " dB), 2nd " << d2.az << " (" << d2.db << " dB)" << std::endl;
            check(angleDiff(d1.az, 90.0f) < 25.0f && angleDiff(d2.az, 180.0f) < 25.0f,
                  "echoes: every repeat from the next direction");
            check(std::abs(d1.db - d0.db) < 6.0f, "echo level about like the dry sound at 100 %");
        }
        { // upper partials 90 deg away; the fundamental stays at the pipe
            auto b = run(
                [](EngineParams &p)
                {
                    p.fx.partSpread = 90.0f;
                    p.fx.partSplit = 2.5f;
                },
                {69}, 1.5, -1, "31_fx_partials_ambix1");
            const auto lo = direction(b, 0.5, 1.4),
                       hi = direction(b, 0.5, 1.4, (float)meantoneHz(69) * 5.0f);
            std::cout << "partials: whole az " << lo.az << ", above 5x f0 az " << hi.az << " (pipe "
                      << posA.az << ")" << std::endl;
            check(angleDiff(hi.az, posA.az) > 40.0f, "upper partials placed away from the pipe");
            auto off = run([](EngineParams &) {}, {69}, 1.5, -1, "31_fx_partials_off");
            const float a = off.getRMSLevel(0, (int)(0.5 * fsr), (int)(0.9 * fsr)),
                        c = b.getRMSLevel(0, (int)(0.5 * fsr), (int)(0.9 * fsr));
            check(std::abs(juce::Decibels::gainToDecibels(c / a)) < 1.0f,
                  "partial split keeps the level (W)");
        }
        { // crackle: (on - off) is only the crackle, and it sits at the pipe
            auto on = run(
                [](EngineParams &p)
                {
                    p.fx.crackLevel = 1.0f;
                    p.fx.crackRate = 30.0f;
                },
                {69}, 2.0, 1.2, "32_fx_crackle_ambix1");
            auto off = run([](EngineParams &) {}, {69}, 2.0, 1.2, "32_fx_crackle_off");
            juce::AudioBuffer<float> diff(on);
            for (int c = 0; c < 4; ++c)
                diff.addFrom(c, 0, off, c, 0, off.getNumSamples(), -1.0f);
            const auto d = direction(diff, 0.1, 1.1);
            const auto late = direction(diff, 1.9, 2.0);
            std::cout << "crackle: " << d.db << " dB at az " << d.az << ", el " << d.el
                      << "; after release " << late.db << " dB" << std::endl;
            check(d.db > -50.0f && angleDiff(d.az, posA.az) < 8.0f, "crackle at the pipe");
            check(late.db < d.db - 30.0f, "crackle fades after the key is released");
        }
        { // wind leaks: one leak = one direction; the diffuse blower = all around
            auto b = run(
                [](EngineParams &p)
                {
                    p.stopOn = {};
                    p.fx.leakLevel = 1.0f;
                    p.fx.leakCount = 1;
                },
                {}, 2.0, -1, "33_fx_leak_ambix1");
            const auto d = direction(b, 0.3, 2.0);
            std::cout << "leak: " << d.db << " dB, az " << d.az << ", directedness " << d.dirness
                      << std::endl;
            check(d.db > -50.0f && d.db < -12.0f && d.dirness > 0.9f,
                  "one wind leak: a point source");
            auto w = run(
                [](EngineParams &p)
                {
                    p.stopOn = {};
                    p.wind = 0.5f;
                    p.fx.windRound = true;
                },
                {}, 3.0, -1, "34_fx_wind_round_ambix1");
            const auto dw = direction(w, 1.0, 3.0);
            std::cout << "blower all around: directedness " << dw.dirness << ", 1st order / W "
                      << dw.firstOrderRatio << std::endl;
            check(dw.dirness < 0.25f && dw.firstOrderRatio > 0.6f && dw.firstOrderRatio < 1.4f,
                  "diffuse blower");
            auto w0 = run(
                [](EngineParams &p)
                {
                    p.stopOn = {};
                    p.wind = 0.5f;
                },
                {}, 3.0, -1, "34_fx_wind_omni");
            const float a = w0.getRMSLevel(0, (int)fsr, (int)(2 * fsr)),
                        c = w.getRMSLevel(0, (int)fsr, (int)(2 * fsr));
            std::cout << "blower level: omni " << juce::Decibels::gainToDecibels(a)
                      << " dB, all around " << juce::Decibels::gainToDecibels(c) << " dB"
                      << std::endl;
            check(std::abs(juce::Decibels::gainToDecibels(c / a)) < 2.0f,
                  "diffuse blower keeps the level");
        }
        { // tracker path: with "pipes wait" the pipe speaks after the path time
            auto lowRms = [&](const juce::AudioBuffer<float> &b, double t0, double t1)
            {
                std::vector<float> x(b.getReadPointer(0) + (int)(t0 * fsr),
                                     b.getReadPointer(0) + (int)(t1 * fsr));
                juce::IIRFilter f;
                f.setCoefficients(juce::IIRCoefficients::makeLowPass(fsr, 1500.0));
                f.processSamples(x.data(), (int)x.size());
                double s = 0;
                for (auto v : x)
                    s += (double)v * v;
                return juce::Decibels::gainToDecibels((float)std::sqrt(s / (double)x.size()),
                                                      -200.0f);
            };
            auto wait = run(
                [](EngineParams &p)
                {
                    p.fx.pathLevel = 1.0f;
                    p.fx.pathTime = 0.3f;
                    p.fx.pathTicks = 8;
                    p.fx.pathWait = true;
                },
                {69}, 1.2, -1, "35_fx_path_wait_ambix1");
            auto noWait = run(
                [](EngineParams &p)
                {
                    p.fx.pathLevel = 1.0f;
                    p.fx.pathTime = 0.3f;
                    p.fx.pathTicks = 8;
                },
                {69}, 1.2, -1, "35_fx_path_ambix1");
            const float wEarly = lowRms(wait, 0.08, 0.25), nEarly = lowRms(noWait, 0.08, 0.25),
                        wLate = lowRms(wait, 0.6, 1.0);
            std::cout << "tracker path: pipe band early " << wEarly << " dB (waiting) vs " << nEarly
                      << " dB, later " << wLate << " dB" << std::endl;
            check(nEarly - wEarly > 15.0f && wLate > nEarly - 3.0f,
                  "pipes wait for the tracker path");
            auto ticks = run(
                [](EngineParams &p)
                {
                    p.stopGain[0] = 0.0f;
                    p.fx.pathLevel = 1.0f;
                    p.fx.pathTime = 0.4f;
                    p.fx.pathTicks = 8;
                    p.fx.pathSpread = 0.0f;
                },
                {69}, 0.6, -1, "35_fx_path_ticks_only");
            const auto first = direction(ticks, 0.0, 0.07), last = direction(ticks, 0.33, 0.45);
            std::cout << "tracker ticks: first at az " << first.az << " el " << first.el
                      << ", last at az " << last.az << " el " << last.el << std::endl;
            check(angleDiff(first.az, 0.0f) < 20.0f && angleDiff(last.az, posA.az) < 12.0f,
                  "tracker ticks run from the console to the pipe");
        }
        { // own sounds: at the pipe (fallback = the recorded clicks when the folder is empty)
            auto b = run(
                [](EngineParams &p)
                {
                    p.stopGain[0] = 0.0f;
                    p.fx.custLevel = 1.0f;
                    p.fx.custPlace = 0;
                },
                {69}, 0.8, -1, "36_fx_own_at_pipe");
            const auto d = direction(b, 0.0, 0.5);
            std::cout << "own sound at pipe: " << d.db << " dB, az " << d.az << std::endl;
            check(d.db > -60.0f && angleDiff(d.az, posA.az) < 10.0f, "own sound at the pipe");
            auto c = run(
                [](EngineParams &p)
                {
                    p.stopGain[0] = 0.0f;
                    p.fx.custLevel = 1.0f;
                    p.fx.custPlace = 3;
                    p.fx.custWander = 180.0f;
                    p.fx.custScatter = 1.0f;
                },
                {69}, 2.0, 0.5, "36_fx_own_circling");
            check(c.getRMSLevel(0, 0, c.getNumSamples()) > 1.0e-5f, "own sounds circling render");

            // loading: transient at the start, peak -6 dBFS
            auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                           .getChildFile("rysum_custom_test");
            tmp.deleteRecursively();
            auto dir = SampleLibrary::customFolder(tmp);
            dir.createDirectory();
            juce::AudioBuffer<float> snd(2, 24000);
            snd.clear();
            for (int i = 12000; i < 24000; ++i)
            {
                const float v = 0.2f * std::exp(-static_cast<float>(i - 12000) / 800.0f) *
                                (float)std::sin(i * 0.9);
                snd.setSample(0, i, v);
                snd.setSample(1, i, v);
            }
            writeWav(dir.getChildFile("schraube.wav"), snd, 48000.0);
            auto list = SampleLibrary::loadCustom(tmp);
            float pk = 0;
            for (auto v : list.empty() ? std::vector<float>() : list[0].sample->data)
                pk = std::max(pk, std::abs(v));
            check(list.size() == 1 && list[0].sample->size() < 12100 && std::abs(pk - 0.5f) < 0.01f,
                  "own sounds folder: trimmed + normalised");
            tmp.deleteRecursively();
        }
        { // timing: everything on, 3rd order
            OrganEngine e;
            e.prepare(fsr, B);
            e.setLibrary(lib);
            EngineParams p;
            p.stopOn.fill(true);
            p.outputMode = 3;
            p.fx.custLevel = 0.5f;
            p.fx.custScatter = 1.0f;
            p.fx.pathLevel = 0.5f;
            p.fx.crackLevel = 0.5f;
            p.fx.crackRate = 40.0f;
            p.fx.leakLevel = 0.5f;
            p.fx.leakCount = 8;
            p.fx.leakWander = 30.0f;
            p.fx.windRound = true;
            p.wind = 0.3f;
            p.fx.partSpread = 60.0f;
            p.fx.partTurn = 0.3f;
            p.fx.echoLevel = 0.5f;
            p.fx.echoRepeats = 8;
            juce::AudioBuffer<float> b(16, B);
            const int blocks = (int)(6.0 * fsr / B);
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < blocks; ++i)
            {
                juce::MidiBuffer mb;
                if (i == 0)
                    for (int m : {48, 55, 60, 64, 67, 72})
                        mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)90), 0);
                e.process(b, mb, p);
            }
            const double rt =
                6.0 / std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0)
                          .count();
            std::cout << "all effects, 8 stops x 6 keys, 3rd order: " << rt << " x real time"
                      << std::endl;
            check(rt > 2.0, "effects are cheap enough");
        }
    }

    // ---------------- simulated room ("Saal") ----------------
    {
        // ray tracer vs Eyring
        {
            RoomParams r;
            r.on = true;
            r.width = r.length = r.height = 10.0f;
            r.absWalls = r.absFloor = 0.2f;
            r.scatter = 0.5f;
            r.highDamp = 0.0f;
            auto t0 = std::chrono::high_resolution_clock::now();
            const auto rt = traceRoom(r, 0.5f);
            const double ms = 1000.0 * std::chrono::duration<double>(
                                           std::chrono::high_resolution_clock::now() - t0)
                                           .count();
            const float ey = eyringRt(r, false, 0.5f);
            std::cout << "room: cube 10 m, alpha 0.2: ray traced " << rt.rtLow << " s, Eyring "
                      << ey << " s (" << ms << " ms, " << rt.reflections << " reflections)"
                      << std::endl;
            check(std::abs(rt.rtLow / ey - 1.0f) < 0.12f,
                  "ray tracer agrees with Eyring in a diffuse cube");
            RoomParams c = r;
            c.width = 3.0f;
            c.length = 40.0f;
            c.height = 3.0f;
            c.absWalls = 0.02f;
            c.absFloor = 0.6f;
            c.scatter = 0.02f;
            const auto rc = traceRoom(c, 0.5f);
            std::cout << "room: corridor, hard walls, dead floor/ceiling: ray traced " << rc.rtLow
                      << " s, Eyring " << eyringRt(c, false, 0.5f) << " s" << std::endl;
            check(rc.rtLow > 1.3f * eyringRt(c, false, 0.5f),
                  "ray tracer: non-diffuse corridor rings longer than Eyring says");
        }

        const double fsr = 48000.0;
        constexpr int B = 512;
        int keyA = -1;
        for (int k = 0; k < kNumKeys; ++k)
            if (kPhysKeys[(size_t)k] == 69)
                keyA = k;
        auto run = [&](std::function<void(EngineParams &)> setup, double secs, double offAt,
                       const juce::String &name, int ch = 4, OrganEngine *keep = nullptr)
        {
            OrganEngine local;
            OrganEngine &e = keep != nullptr ? *keep : local;
            e.prepare(fsr, B);
            e.setLibrary(lib);
            EngineParams p;
            p.stopOn = {};
            p.stopOn[0] = true;
            p.clickLevel = 0;
            p.releaseClick = 0;
            p.wind = 0;
            p.stopPull = 0;
            p.outputMode = ch == 4 ? 1 : (ch == 2 ? 0 : 3);
            setup(p);
            if (p.room.on)
                e.setTailIR(TailIR::build(p.room, p.air, p.outputMode, fsr,
                                          1)); // (the plugin builds it in the background)
            juce::AudioBuffer<float> b(ch, B), all(ch, (int)(fsr * secs));
            for (int pos = 0; pos < all.getNumSamples(); pos += B)
            {
                juce::MidiBuffer mb;
                if (pos == 0)
                    mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)110), 0);
                if (offAt > 0 && pos == ((int)(offAt * fsr) / B) * B)
                    mb.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
                e.process(b, mb, p);
                for (int c = 0; c < ch; ++c)
                    all.copyFrom(c, pos, b, c, 0, juce::jmin(B, all.getNumSamples() - pos));
            }
            if (name.isNotEmpty())
                writeWav(outDir.getChildFile(name + ".wav"), all, fsr);
            return all;
        };

        // ---------------- how the pipes radiate, doors, the room keyboard ----------------
        {
            auto en = [&](const juce::AudioBuffer<float> &a, int c, double t0, double t1)
            {
                double sum = 0.0;
                const int i0 = juce::jlimit(0, a.getNumSamples(), (int)(t0 * fsr)),
                          i1 = juce::jlimit(0, a.getNumSamples(), (int)(t1 * fsr));
                const float *x = a.getReadPointer(c);
                for (int i = i0; i < i1; ++i)
                    sum += (double)x[i] * x[i];
                return sum;
            };
            auto dB = [](double a, double b) { return 10.0 * std::log10((a + 1.0e-30) / (b + 1.0e-30)); };
            auto finite = [](const juce::AudioBuffer<float> &a)
            {
                for (int c = 0; c < a.getNumChannels(); ++c)
                    for (int i = 0; i < a.getNumSamples(); ++i)
                        if (!std::isfinite(a.getSample(c, i)))
                            return false;
                return true;
            };
            // 1) the pattern and the facing, the room off: the direct sound only
            auto dry = [&](float pat, float face)
            {
                return run(
                    [&](EngineParams &p)
                    {
                        p.room.on = false;
                        p.stopDirPat[0] = pat;
                        p.stopFace[0] = face;
                    },
                    1.0, 0.0, {});
            };
            const auto omni = dry(0.0f, 0.0f), front = dry(0.5f, 0.0f), back = dry(0.5f, 180.0f),
                       side8 = dry(1.0f, 90.0f);
            const double eO = en(omni, 0, 0.3, 0.9);
            std::cout << "radiation (direct): cardioid facing " << dB(en(front, 0, 0.3, 0.9), eO)
                      << " dB, facing away " << dB(en(back, 0, 0.3, 0.9), eO)
                      << " dB, figure-eight sideways " << dB(en(side8, 0, 0.3, 0.9), eO) << " dB"
                      << std::endl;
            check(std::abs(dB(en(front, 0, 0.3, 0.9), eO)) < 1.0 &&
                      dB(en(back, 0, 0.3, 0.9), eO) < -25.0 && dB(en(side8, 0, 0.3, 0.9), eO) < -40.0,
                  "radiation: a cardioid facing the listener is as loud as a ball, turned away or "
                  "a figure-eight sideways it is gone");
            // 2) in the Saal a pipe turned away is heard through the room
            auto inSaal = [&](float pat, float face, float spin, const juce::String &name)
            {
                return run(
                    [&](EngineParams &p)
                    {
                        p.room.on = true;
                        p.room.tailModel = 1;
                        p.stopDirPat[0] = pat;
                        p.stopFace[0] = face;
                        p.stopSpin[0] = spin;
                    },
                    3.0, 2.0, name);
            };
            const auto sO = inSaal(0.0f, 0.0f, 0.0f, {}), sB = inSaal(0.5f, 180.0f, 0.0f, "radiate_away");
            const double tail = dB(en(sB, 0, 2.2, 2.9), en(sO, 0, 2.2, 2.9)),
                         during = dB(en(sB, 0, 0.5, 1.9), en(sO, 0, 0.5, 1.9));
            std::cout << "radiation in the Saal, cardioid turned away: " << during
                      << " dB while it sounds, tail " << tail << " dB" << std::endl;
            check(finite(sB) && tail > -10.0 && tail < -1.0 && during < -2.0,
                  "radiation: turned away, the pipe is heard through the room (the room gets a "
                  "little less)");
            // 3) turning on the spot: the direct sound swings once a turn; in the Saal the
            // reflected sound sweeps round (its direction moves), the pipe stays where it is
            auto swing = [&](const juce::AudioBuffer<float> &a)
            {
                double lo = 1.0e30, hi = 0.0;
                for (double t = 0.6; t < 1.8; t += 0.05)
                {
                    const double e = en(a, 0, t, t + 0.05);
                    lo = juce::jmin(lo, e);
                    hi = juce::jmax(hi, e);
                }
                return dB(hi, lo);
            };
            auto drySpin = [&](float spin)
            {
                return run(
                    [&](EngineParams &p)
                    {
                        p.room.on = false;
                        p.stopDirPat[0] = 0.5f;
                        p.stopSpin[0] = spin;
                    },
                    2.0, 0.0, {});
            };
            const auto dS = drySpin(1.0f), dC = drySpin(0.0f);
            std::cout << "radiation: turning once a second, the direct sound swings " << swing(dS)
                      << " dB, standing " << swing(dC) << " dB" << std::endl;
            check(finite(dS) && swing(dS) > 15.0 && swing(dC) < 1.5,
                  "radiation: a turning cardioid swings once a turn (direct sound)");
            auto spread = [&](const juce::AudioBuffer<float> &a)
            {
                // the directions of the sound in windows of 0.1 s: how far apart they lie
                std::vector<double> az;
                for (double t = 0.6; t < 1.8; t += 0.1)
                {
                    double cx2 = 0.0, cy2 = 0.0;
                    for (int i = (int)(t * fsr); i < (int)((t + 0.1) * fsr); ++i)
                    {
                        cy2 += (double)a.getSample(0, i) * a.getSample(1, i);
                        cx2 += (double)a.getSample(0, i) * a.getSample(3, i);
                    }
                    az.push_back(std::atan2(cy2, cx2));
                }
                double worst = 0.0;
                for (size_t i = 0; i < az.size(); ++i)
                    for (size_t j = i + 1; j < az.size(); ++j)
                        worst = juce::jmax(worst, std::abs(std::remainder(az[i] - az[j], 2.0 * juce::MathConstants<double>::pi)));
                return juce::radiansToDegrees(worst);
            };
            // (the early reflections and the direct sound only: the late reverb's direction wanders
            // in short windows anyway)
            auto erOnly = [&](float spin, const juce::String &name)
            {
                return run(
                    [&](EngineParams &p)
                    {
                        p.room.on = true;
                        p.room.tailModel = 1;
                        p.room.tailGain = 0.0f;
                        p.stopDirPat[0] = 0.5f;
                        p.stopSpin[0] = spin;
                    },
                    3.0, 2.0, name);
            };
            const auto sS = erOnly(1.0f, "radiate_spin"), sC = erOnly(0.0f, {});
            std::cout << "radiation in the Saal: the sound's direction moves over " << spread(sS)
                      << " deg turning, " << spread(sC) << " deg standing" << std::endl;
            check(finite(sS) && spread(sS) > 2.0 * spread(sC) + 10.0,
                  "radiation: a turning cardioid sweeps its reflections round the room");

            // a figure-eight's rear lobe is negative: turned round, everything (direct sound and
            // every reflection) only changes its sign - nothing may be dropped
            {
                auto eight = [&](float face) { return run(
                    [&](EngineParams &p)
                    {
                        p.room.on = true;
                        p.room.tailModel = 1;
                        p.room.tailGain = 0.0f;
                        p.stopDirPat[0] = 1.0f;
                        p.stopFace[0] = face;
                    },
                    1.5, 0.0, {}); };
                const auto f0 = eight(0.0f), f1 = eight(180.0f);
                double sum = 0.0, ref = 0.0;
                for (int c = 0; c < 4; ++c)
                    for (int i = 0; i < f0.getNumSamples(); ++i)
                    {
                        const double a = f0.getSample(c, i), b = f1.getSample(c, i);
                        sum += (a + b) * (a + b);
                        ref += a * a;
                    }
                std::cout << "radiation: figure-eight turned round, residue " << dB(sum, ref)
                          << " dB, level " << dB(en(f1, 0, 0.3, 1.4), en(f0, 0, 0.3, 1.4)) << " dB"
                          << std::endl;
                check(ref > 0.0 && sum < 1.0e-8 * ref,
                      "radiation: a figure-eight turned round only changes its sign (the rear "
                      "lobe's reflections are kept)");
            }

            // 4) Ausdehnung: the same pressure, but no longer one direction
            auto wide = [&](float w)
            {
                return run(
                    [&](EngineParams &p)
                    {
                        p.room.on = false;
                        p.stopWidth[0] = w;
                    },
                    1.0, 0.0, w > 0.5f ? "radiate_wide" : "");
            };
            const auto w0 = wide(0.0f), w1 = wide(1.0f);
            auto coherence = [&](const juce::AudioBuffer<float> &a)
            {
                // the first-order channel with the most energy: how much of it is W itself
                int best = 1;
                for (int c = 2; c < 4; ++c)
                    if (en(a, c, 0.3, 0.9) > en(a, best, 0.3, 0.9))
                        best = c;
                double sxy = 0.0;
                const float *x = a.getReadPointer(0), *y = a.getReadPointer(best);
                for (int i = (int)(0.3 * fsr); i < (int)(0.9 * fsr); ++i)
                    sxy += (double)x[i] * y[i];
                return std::abs(sxy) / std::sqrt(en(a, 0, 0.3, 0.9) * en(a, best, 0.3, 0.9) + 1.0e-30);
            };
            const double lvl = dB(en(w1, 0, 0.3, 0.9), en(w0, 0, 0.3, 0.9));
            const double r1 = (en(w1, 1, 0.3, 0.9) + en(w1, 2, 0.3, 0.9) + en(w1, 3, 0.3, 0.9)) /
                              juce::jmax(1.0e-30, en(w1, 0, 0.3, 0.9));
            std::cout << "Ausdehnung: W " << lvl << " dB, coherence point " << coherence(w0)
                      << ", all around " << coherence(w1) << ", first order / W " << r1 << std::endl;
            check(finite(w1) && std::abs(lvl) < 0.5 && coherence(w0) > 0.95 && coherence(w1) < 0.7 &&
                      r1 > 0.6 && r1 < 1.4,
                  "Ausdehnung: all around, the pipe keeps its level but comes from everywhere "
                  "(decorrelated, an even field)");

            // 5) the room keyboard: a place pulls the pipe there (a note that starts while the key
            // is held sounds from there at once)
            auto pulled = run(
                [&](EngineParams &p)
                {
                    p.room.on = false;
                    p.roomKeys.numPlaces = 1;
                    p.roomKeys.placeAz[0] = -90.0f; // (C#4 .. : 3 o'clock, to the right)
                    p.roomKeys.placeEl[0] = 0.0f;
                    p.roomKeys.placeDepth[0] = 1.0f;
                    p.roomKeys.glide = 0.05f;
                },
                1.0, 0.0, {});
            double cx = 0.0, cy = 0.0;
            for (int i = (int)(0.3 * fsr); i < (int)(0.9 * fsr); ++i)
            {
                cy += (double)pulled.getSample(0, i) * pulled.getSample(1, i);
                cx += (double)pulled.getSample(0, i) * pulled.getSample(3, i);
            }
            const double azHeard = juce::radiansToDegrees(std::atan2(cy, cx));
            std::cout << "room keyboard: pulled to the right, heard from " << azHeard << " deg"
                      << std::endl;
            check(std::abs(azHeard + 90.0) < 5.0, "room keyboard: a held place pulls the pipe there");
            // ... and walls taken out: the room rings much shorter
            auto walls = [&](bool out)
            {
                return run(
                    [&](EngineParams &p)
                    {
                        p.room.on = true;
                        p.room.tailModel = 1;
                        p.room.absWalls = 0.03f;
                        if (out)
                        {
                            p.roomKeys.numWalls = 4;
                            for (int k = 0; k < 4; ++k)
                            {
                                p.roomKeys.wallAz[(size_t)k] = 90.0f * (float)k;
                                p.roomKeys.wallDepth[(size_t)k] = 1.0f;
                            }
                            p.roomKeys.ceilingDepth = 1.0f;
                            p.roomKeys.glide = 0.05f;
                        }
                    },
                    3.0, 1.0, out ? "roomkeys_walls" : "");
            };
            const auto wIn = walls(false), wOut = walls(true);
            const double late = dB(en(wOut, 0, 1.6, 2.6), en(wIn, 0, 1.6, 2.6));
            std::cout << "room keyboard: walls and ceiling out, the late tail " << late << " dB"
                      << std::endl;
            check(finite(wOut) && late < -10.0, "room keyboard: without walls the room rings shorter");
            {
                // the same with the convolution tail (its response is built for the closed room:
                // the level follows the shorter decay)
                auto wallsConv = [&](bool out)
                {
                    // (as the processor does: the response is built again with the surfaces the
                    // engine reports gone, here once, 0.4 s in)
                    OrganEngine e;
                    e.prepare(fsr, B);
                    e.setLibrary(lib);
                    EngineParams p;
                    p.stopOn = {};
                    p.stopOn[0] = true;
                    p.clickLevel = 0;
                    p.releaseClick = 0;
                    p.wind = 0;
                    p.stopPull = 0;
                    p.outputMode = 1;
                    p.room.on = true;
                    p.room.tailModel = 0;
                    p.room.absWalls = 0.03f;
                    if (out)
                    {
                        p.roomKeys.numWalls = 4;
                        for (int k = 0; k < 4; ++k)
                        {
                            p.roomKeys.wallAz[(size_t)k] = 90.0f * (float)k;
                            p.roomKeys.wallDepth[(size_t)k] = 1.0f;
                        }
                        p.roomKeys.ceilingDepth = 1.0f;
                        p.roomKeys.glide = 0.05f;
                    }
                    // (the engine's copies are never the last ones: the old response may still be
                    // fading out)
                    std::vector<std::shared_ptr<const TailIR>> keepIr{
                        TailIR::build(p.room, p.air, p.outputMode, fsr, 1)};
                    e.setTailIR(keepIr.back());
                    juce::AudioBuffer<float> b(4, B), all(4, (int)(fsr * 3.0));
                    for (int pos = 0; pos < all.getNumSamples(); pos += B)
                    {
                        juce::MidiBuffer mb;
                        if (pos == 0)
                            mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)110), 0);
                        if (pos == ((int)(1.0 * fsr) / B) * B)
                            mb.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
                        if (out && pos == ((int)(0.4 * fsr) / B) * B)
                        {
                            RoomParams r = p.room;
                            for (int sfc = 0; sfc < kMaxSurf; ++sfc)
                                r.gone[(size_t)sfc] = e.wallGoneNow[0][(size_t)sfc].load();
                            keepIr.push_back(TailIR::build(r, p.air, p.outputMode, fsr, 2));
                            e.setTailIR(keepIr.back());
                        }
                        e.process(b, mb, p);
                        for (int c = 0; c < 4; ++c)
                            all.copyFrom(c, pos, b, c, 0, juce::jmin(B, all.getNumSamples() - pos));
                    }
                    return all;
                };
                const double lateC = dB(en(wallsConv(true), 0, 1.6, 2.6), en(wallsConv(false), 0, 1.6, 2.6));
                std::cout << "room keyboard, convolution tail: walls and ceiling out, the late tail "
                          << lateC << " dB" << std::endl;
                check(lateC < -10.0, "room keyboard: without walls the convolution tail rings shorter (its response built anew)");
            }

            // 6) a door: a small wooden room with its door open into a large stone hall rings twice
            auto door = [&](int to, float open, const juce::String &name)
            {
                return run(
                    [&](EngineParams &p)
                    {
                        p.room.on = true;
                        p.room.tailModel = 1;
                        p.room.width = 5.0f;
                        p.room.length = 6.0f;
                        p.room.height = 3.0f;
                        p.room.absWalls = 0.3f;
                        p.room.absFloor = 0.3f;
                        auto &h = p.moreRooms[0];
                        h = p.room;
                        h.width = 30.0f;
                        h.length = 50.0f;
                        h.height = 20.0f;
                        h.absWalls = 0.02f;
                        h.absFloor = 0.03f;
                        p.stopRoom[0] = 0;
                        p.doors[0].to = to;
                        p.doors[0].open = open;
                        p.doors[0].wall = 2;
                    },
                    5.0, 1.0, name);
            };
            const auto dNone = door(-1, 1.0f, {}), dShut = door(1, 0.0f, {}),
                       dOpen = door(1, 1.0f, "door_open");
            double diff = 0.0, ref = 0.0;
            for (int c = 0; c < 4; ++c)
                for (int i = 0; i < dNone.getNumSamples(); ++i)
                {
                    const double d = (double)dShut.getSample(c, i) - dNone.getSample(c, i);
                    diff += d * d;
                    ref += (double)dNone.getSample(c, i) * dNone.getSample(c, i);
                }
            const double lateDoor = dB(en(dOpen, 0, 2.5, 4.5), en(dNone, 0, 2.5, 4.5)),
                         early = dB(en(dOpen, 0, 0.3, 0.9), en(dNone, 0, 0.3, 0.9));
            std::cout << "door: shut = no door (rel. difference " << diff / juce::jmax(1.0e-30, ref)
                      << "), open: while it sounds " << early << " dB, late (1.5 .. 3.5 s after) "
                      << lateDoor << " dB" << std::endl;
            check(diff < 1.0e-12 * ref, "door: a shut door is no door");
            check(finite(dOpen) && lateDoor > 10.0 && early < 3.0,
                  "door: open into the big hall, the small room rings out twice (the hall's long "
                  "tail through the door)");
            // the hall's tail comes from the door (the front wall): the first-order direction
            double fx = 0.0, fy = 0.0;
            for (int i = (int)(2.5 * fsr); i < (int)(4.5 * fsr); ++i)
            {
                fy += (double)dOpen.getSample(0, i) * dOpen.getSample(1, i);
                fx += (double)dOpen.getSample(0, i) * dOpen.getSample(3, i);
            }
            const double doorAz = juce::radiansToDegrees(std::atan2(fy, fx));
            std::cout << "door: the hall heard from " << doorAz << " deg (the door is in front)"
                      << std::endl;
            check(std::abs(doorAz) < 20.0, "door: the hall behind it is heard from the door");
            {
                // the default convolution tail: the hall behind the door has no stop of its own,
                // so the engine must count it as used (the processor builds the responses of the
                // rooms in use)
                OrganEngine e;
                e.prepare(fsr, B);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[0] = true;
                p.clickLevel = 0;
                p.releaseClick = 0;
                p.wind = 0;
                p.stopPull = 0;
                p.outputMode = 1;
                p.room.on = true;
                p.room.tailModel = 0;
                p.room.width = 5.0f;
                p.room.length = 6.0f;
                p.room.height = 3.0f;
                p.room.absWalls = 0.3f;
                p.room.absFloor = 0.3f;
                auto &h = p.moreRooms[0];
                h = p.room;
                h.width = 30.0f;
                h.length = 50.0f;
                h.height = 20.0f;
                h.absWalls = 0.02f;
                h.absFloor = 0.03f;
                p.doors[0].to = 1;
                p.doors[0].open = 1.0f;
                std::vector<std::shared_ptr<const TailIR>> doorIr;
                juce::AudioBuffer<float> b(4, B), all(4, (int)(fsr * 5.0));
                for (int pos = 0; pos < all.getNumSamples(); pos += B)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)110), 0);
                    if (pos == ((int)(1.0 * fsr) / B) * B)
                        mb.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
                    e.process(b, mb, p);
                    if (pos == 0)
                    {
                        // as the processor does: the responses of the rooms in use
                        const int used = e.roomsUsed.load();
                        check(((used >> 1) & 1) != 0,
                              "door: the Saal behind the door counts as in use (its response is built)");
                        for (int ri = 0; ri < 2; ++ri)
                            if ((used >> ri) & 1)
                            {
                                doorIr.push_back(TailIR::build(p.roomAt(ri), p.air, p.outputMode, fsr, (uint32_t)(10 + ri)));
                                e.setTailIR(doorIr.back(), ri);
                            }
                    }
                    for (int c = 0; c < 4; ++c)
                        all.copyFrom(c, pos, b, c, 0, juce::jmin(B, all.getNumSamples() - pos));
                }
                auto noDoor = door(-1, 1.0f, {});
                const double lateConv = dB(en(all, 0, 2.5, 4.5), en(noDoor, 0, 2.5, 4.5));
                std::cout << "door, convolution tail: late " << lateConv << " dB" << std::endl;
                check(finite(all) && lateConv > 10.0,
                      "door: also with the convolution tail the hall rings on through the door");
            }
        }
        {
            // the source "Schwebung" routed to the pipes' place: moving pipes are heard with a
            // Doppler shift, but the beats belong to the tuning - over a held chord the source rises
            // once and stays (it used to settle a second time as the moving pipes' Doppler shift
            // changed the beats it was moving them by)
            float settled[2]{};
            for (int tgt : {(int)tgtAz, (int)tgtDist})
            {
                OrganEngine e;
                e.prepare(fsr, B);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[1] = true;
                p.stopOn[2] = true;
                p.clickLevel = 0;
                p.releaseClick = 0;
                p.wind = 0;
                p.stopPull = 0;
                p.outputMode = 1;
                p.doppler = 1.0f;
                p.mod.routes[0].src = srcBeat;
                p.mod.routes[0].target = tgt;
                p.mod.routes[0].amount = 1.0f;
                juce::AudioBuffer<float> b(4, B);
                std::vector<float> src;
                for (int pos = 0; pos < (int)(6.0 * fsr); pos += B)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        for (int nt : {60, 64, 67})
                            mb.addEvent(juce::MidiMessage::noteOn(1, nt, (juce::uint8)100), 0);
                    e.process(b, mb, p);
                    src.push_back(e.sourceOut[srcBeat].load());
                }
                // after its first peak the source may only settle, never swing back
                float peak = 0.0f, back = 0.0f, late0 = src[(size_t)(3.0 * fsr / B)], late1 = src.back();
                for (float v : src)
                {
                    peak = juce::jmax(peak, v);
                    back = juce::jmax(back, peak - v);
                }
                std::cout << "Schwebung -> " << (tgt == tgtDist ? "distance" : "direction") << ": peak " << peak
                          << ", falls back by " << back << ", 3 s .. 6 s " << late0 << " .. " << late1 << std::endl;
                settled[tgt == tgtDist ? 1 : 0] = late1;
                check(back < 0.002f && std::abs(late1 - late0) < 0.001f,
                      juce::String("Schwebung routed to the pipes' ") + (tgt == tgtDist ? "distance" : "direction") +
                          ": over a held chord it rises once and stays (no Doppler feedback)");
            }
            check(std::abs(settled[1] - settled[0]) < 0.002f,
                  "Schwebung: moving the pipes does not change it (the beats of the tuning)");
            {
                // "Schwebung folgt: sofort": the chord's beat value from its first slices on, and
                // gone at once when the chord is let go
                OrganEngine e;
                e.prepare(fsr, B);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[1] = true;
                p.stopOn[2] = true;
                p.clickLevel = 0;
                p.releaseClick = 0;
                p.wind = 0;
                p.stopPull = 0;
                p.outputMode = 1;
                p.beatInstant = true;
                p.mod.routes[0].src = srcBeat;
                p.mod.routes[0].target = tgtBright;
                p.mod.routes[0].amount = 1.0f;
                juce::AudioBuffer<float> b(4, B);
                std::vector<float> src;
                for (int pos = 0; pos < (int)(3.0 * fsr); pos += B)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        for (int nt : {60, 64, 67})
                            mb.addEvent(juce::MidiMessage::noteOn(1, nt, (juce::uint8)100), 0);
                    if (pos == ((int)(2.0 * fsr) / B) * B)
                        for (int nt : {60, 64, 67})
                            mb.addEvent(juce::MidiMessage::noteOff(1, nt), 0);
                    e.process(b, mb, p);
                    src.push_back(e.sourceOut[srcBeat].load());
                }
                const float first = src[1], held = src[(size_t)(1.5 * fsr / B)];
                std::cout << "Schwebung at once: after one block " << first << ", held " << held
                          << " (gliding: " << settled[0] << ")" << std::endl;
                check(std::abs(first - held) < 0.002f && std::abs(held - settled[0]) < 0.002f,
                      "Schwebung at once: the chord's value from the first block on");
            }
            {
                // "Schwebung misst: Verhaeltnis": cents from pure, the same in every register. A
                // meantone C major chord: pure third, fifth and minor third 5.4 ct narrow ->
                // log2(1 + 5.38) / log2(51) = 0.471; a pure triad alone (c, e) is 0
                auto ratioSrc = [&](std::initializer_list<int> notes, bool ratio)
                {
                    OrganEngine e;
                    e.prepare(fsr, B);
                    e.setLibrary(lib);
                    EngineParams p;
                    p.stopOn = {};
                    p.stopOn[0] = true;
                    p.clickLevel = 0;
                    p.releaseClick = 0;
                    p.wind = 0;
                    p.stopPull = 0;
                    p.outputMode = 1;
                    p.beatInstant = true;
                    p.beatRatio = ratio;
                    p.mod.routes[0].src = srcBeat;
                    p.mod.routes[0].target = tgtBright;
                    p.mod.routes[0].amount = 1.0f;
                    juce::AudioBuffer<float> b(4, B);
                    for (int pos = 0; pos < (int)(0.5 * fsr); pos += B)
                    {
                        juce::MidiBuffer mb;
                        if (pos == 0)
                            for (int nt : notes)
                                mb.addEvent(juce::MidiMessage::noteOn(1, nt, (juce::uint8)100), 0);
                        e.process(b, mb, p);
                    }
                    return e.sourceOut[srcBeat].load();
                };
                const float r48 = ratioSrc({48, 52, 55}, true), r60 = ratioSrc({60, 64, 67}, true),
                            r72 = ratioSrc({72, 76, 79}, true), third = ratioSrc({60, 64}, true),
                            h48 = ratioSrc({48, 52, 55}, false), h72 = ratioSrc({72, 76, 79}, false);
                std::cout << "Schwebung as ratio: C major " << r48 << " / " << r60 << " / " << r72
                          << " (Hz: " << h48 << " .. " << h72 << "), pure third " << third << std::endl;
                check(std::abs(r48 - r60) < 0.01f && std::abs(r72 - r60) < 0.01f &&
                          std::abs(r60 - 0.471f) < 0.02f && third < 0.05f,
                      "Schwebung as ratio: the same in every octave (cents from pure), 0 for a pure third");
            }
        }
        if (std::getenv("RYSUM_ONLY_RADIATE") != nullptr) // (quick run, with RYSUM_ONLY_ROOM)
        {
            std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string(failures))
                      << std::endl;
            return failures;
        }

        // ---------------- polygon rooms (floor plans) ----------------
        {
            // 1) a rectangle given as a polygon must give exactly the shoebox's mirror images
            RoomParams bx;
            bx.on = true;
            bx.width = 14;
            bx.length = 26;
            bx.height = 9;
            RoomParams pr = bx;
            pr.shape = shapeFan;
            pr.shapeAmt = 0.0f;
            finishPlan(pr, defaultDrawnPlan());
            std::vector<PlanPath> paths(2048);
            std::vector<PlanImage> cand(1024);
            const Vec3f S{5.0f, 19.0f, 3.0f}, Lp = listenerPos(bx);
            bool same = true;
            for (int ord = 1; ord <= 3; ++ord)
            {
                std::array<ImageSource, 62> ims;
                const int nb = imageSources(bx, S, ord, ims.data(), 62);
                const auto g = planGeo(pr);
                const int np = planImageSources(pr, g, S, Lp, ord, paths.data(), 2048, cand.data(),
                                                1024, 1024);
                int matched = 0;
                for (int i = 0; i < nb; ++i)
                    for (int j = 0; j < np; ++j)
                    {
                        const auto &a = ims[(size_t)i].pos;
                        const auto &b = cand[(size_t)j].pos;
                        if (std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z) <
                            1.0e-3f)
                        {
                            ++matched;
                            break;
                        }
                    }
                std::cout << "plan: rectangle as polygon, order " << ord << ": " << np
                          << " images, shoebox " << nb << ", matched " << matched << std::endl;
                same = same && np == nb && matched == nb;
            }
            check(same, "plan: a rectangular polygon gives the shoebox's mirror images");
            check(!planSelfIntersects(makeFloorPlan(shapeCross, 0.5f, defaultDrawnPlan())) &&
                      !planSelfIntersects(defaultDrawnPlan()),
                  "plan: presets are simple polygons");
            {
                FloorPlan bow;
                bow.n = 4;
                bow.x = {0, 1, 0, 1};
                bow.y = {0, 1, 1, 0};
                check(planSelfIntersects(bow), "plan: a crossed polygon is detected");
            }

            // 2) L-shaped room: no reflection path passes through the inner corner; every path is
            // geometrically right
            RoomParams lr = bx;
            lr.shape = shapeL;
            lr.shapeAmt = 1.0f;
            finishPlan(lr, defaultDrawnPlan());
            {
                const auto g = planGeo(lr);
                check(!g.convex && g.n == 6, "plan: the L is non-convex with 6 walls");
                const Vec3f Ls = listenerPos(lr);
                const Vec3f Ss = roomPoint(lr, {-20.0f, 0.0f, 12.0f});
                const int n3 =
                    planImageSources(lr, g, Ss, Ls, 3, paths.data(), 2048, cand.data(), 1024, 1024);
                int bad = 0;
                for (int i = 0; i < n3; ++i)
                {
                    const auto &c = cand[(size_t)i];
                    // walk the path again: it must stay inside the room at every step
                    PlanPath pp;
                    pp.k = c.k;
                    pp.w = c.w;
                    float qx[3], qy[3];
                    if (!planPathPoints(g, pp, Ss.x, Ss.y, Ls.x, Ls.y, qx, qy))
                    {
                        ++bad;
                        continue;
                    }
                    float px = Ls.x, py = Ls.y;
                    for (int j = c.k - 1; j >= -1; --j)
                    {
                        const float tx = j >= 0 ? qx[j] : Ss.x, ty = j >= 0 ? qy[j] : Ss.y;
                        for (int m = 1; m < 10; ++m)
                        {
                            const float f = (float)m / 10.0f;
                            if (!insidePlan(g, px + f * (tx - px), py + f * (ty - py)))
                            {
                                ++bad;
                                j = -2;
                                break;
                            }
                        }
                        px = tx;
                        py = ty;
                    }
                    // and the path length equals the distance to the image
                    float len = 0, ax = Ls.x, ay = Ls.y;
                    for (int j = c.k - 1; j >= 0; --j)
                    {
                        len += std::hypot(qx[j] - ax, qy[j] - ay);
                        ax = qx[j];
                        ay = qy[j];
                    }
                    len += std::hypot(Ss.x - ax, Ss.y - ay);
                    if (std::abs(len - std::hypot(c.pos.x - Ls.x, c.pos.y - Ls.y)) > 1.0e-2f)
                        ++bad;
                }
                const auto inSrc = insidePlan(g, Ss.x, Ss.y);
                std::cout << "plan: L-shape, 3rd order: " << n3 << " visible images, " << bad
                          << " wrong" << std::endl;
                check(inSrc && bad == 0 && n3 > 10,
                      "plan: L-shape reflection paths are valid and stay inside");
            }
            // 3) listener / sources always inside, also where the shoebox position would be outside
            {
                RoomParams q = lr;
                q.listenerX = 0.95f;
                q.listenerY = 0.95f; // in the cut-out corner
                const auto g = planGeo(q);
                const auto l = listenerPos(q);
                bool ok = insidePlan(g, l.x, l.y) && planEdgeDist(g, l.x, l.y) > 0.25f;
                for (int a = -180; a < 180; a += 7)
                    for (float d : {1.0f, 8.0f, 60.0f})
                    {
                        const auto pnt = roomPoint(q, {(float)a, 10.0f, d});
                        ok = ok && insidePlan(g, pnt.x, pnt.y);
                    }
                check(ok, "plan: listener and sources are kept inside the plan");
            }
            // 4) ray tracer: a rectangle as polygon agrees with the shoebox tracer; a hexagon
            // agrees with Eyring
            {
                RoomParams c;
                c.width = 12;
                c.length = 12;
                c.height = 12;
                c.absWalls = c.absFloor = 0.2f;
                c.scatter = 0.5f;
                c.highDamp = 0.0f;
                RoomParams cp = c;
                cp.shape = shapeFan;
                cp.shapeAmt = 0.0f;
                finishPlan(cp, defaultDrawnPlan());
                RoomParams hx = c;
                hx.shape = shapeOct;
                hx.shapeAmt = 0.5f;
                finishPlan(hx, defaultDrawnPlan());
                const auto a = traceRoom(c, 0.5f), b = traceRoom(cp, 0.5f), h = traceRoom(hx, 0.5f);
                std::cout << "plan: ray tracer cube " << a.rtLow << " s, cube as polygon "
                          << b.rtLow << " s; octagon " << h.rtLow << " s (Eyring "
                          << eyringRt(hx, false, 0.5f) << " s, V " << roomVolume(hx) << " m3)"
                          << std::endl;
                check(std::abs(b.rtLow / a.rtLow - 1.0f) < 0.05f,
                      "plan: polygon ray tracer = shoebox ray tracer for a rectangle");
                check(std::abs(h.rtLow / eyringRt(hx, false, 0.5f) - 1.0f) < 0.15f,
                      "plan: octagon ray tracing agrees with Eyring");
                RoomParams lr2 = lr;
                lr2.absWalls = 0.1f;
                lr2.scatter = 0.3f;
                const auto rl = traceRoom(lr2, 0.5f);
                check(rl.valid && rl.rtLow > 0.1f && rl.rtLow < 30.0f,
                      "plan: L-shape ray tracing gives a decay");
            }
        }
        { // 5) the engine: a rectangle-as-polygon sounds like the shoebox; moving sources in an
          // L-shape stay smooth
            auto setupRect = [](int shape)
            {
                return [shape](EngineParams &p)
                {
                    p.room.on = true;
                    p.room.width = 14;
                    p.room.length = 24;
                    p.room.height = 9;
                    p.room.order = 2;
                    p.room.tailGain = 0.0f;
                    p.room.shape = shape;
                    p.room.shapeAmt = 0.0f;
                };
            };
            auto a = run(setupRect(shapeBox), 1.5, -1, "", 4);
            auto b = run(setupRect(shapeFan), 1.5, -1, "42_plan_rect_as_polygon", 4);
            double e = 0, d = 0;
            for (int i = 0; i < a.getNumSamples(); ++i)
            {
                const double x = a.getSample(0, i), y = b.getSample(0, i);
                e += x * x;
                d += (x - y) * (x - y);
            }
            const double db = 10.0 * std::log10(d / juce::jmax(1e-30, e) + 1e-30);
            std::cout << "plan: shoebox vs the same rectangle as polygon (engine): difference "
                      << db << " dB" << std::endl;
            check(db < -60.0, "plan: the polygon engine matches the shoebox for a rectangle");

            // a fast rotating stop in an L-shaped room, 3rd order: no clicks when paths appear /
            // disappear
            OrganEngine e1, e2;
            auto setupL = [](bool erOn)
            {
                return [erOn](EngineParams &p)
                {
                    p.room.on = true;
                    p.room.width = 16;
                    p.room.length = 22;
                    p.room.height = 8;
                    p.room.order = 3;
                    p.room.tailGain = 0.0f;
                    p.room.shape = shapeL;
                    p.room.shapeAmt = 1.0f;
                    p.room.absWalls = 0.02f;
                    p.room.erGain = erOn ? 1.0f : 0.0f;
                    p.motionMode[0] = (int)Motion::Rotation;
                    p.motionRate[0] = 0.15f;
                };
            };
            for (auto *engine : {&e1, &e2})
            {
                engine->baseAz[0][(size_t)keyA] = 0.0f;
                engine->baseEl[0][(size_t)keyA] = 0.0f;
                engine->baseDist[0][(size_t)keyA] = 7.0f;
            }
            auto t0 = std::chrono::high_resolution_clock::now();
            auto on = run(setupL(true), 4.0, -1, "43_plan_L_rotating", 4, &e1);
            const double rtf =
                4.0 / std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0)
                          .count();
            auto off = run(setupL(false), 4.0, -1, "", 4, &e2);
            // the reflections alone (W): the largest sample-to-sample jump relative to their level
            bool finite = true;
            double maxStep = 0, rms = 0;
            int ns = on.getNumSamples();
            for (int i = 1; i < ns; ++i)
            {
                const double x = on.getSample(0, i) - off.getSample(0, i),
                             xp = on.getSample(0, i - 1) - off.getSample(0, i - 1);
                finite = finite && std::isfinite(x);
                // second difference: a click shows up as a spike far above the smooth signal
                if (i > 1)
                {
                    const double xpp = on.getSample(0, i - 2) - off.getSample(0, i - 2);
                    maxStep = juce::jmax(maxStep, std::abs(x - 2 * xp + xpp));
                }
                rms += x * x;
            }
            rms = std::sqrt(rms / ns);
            std::cout << "plan: L-shape rotating source, 3rd order: reflections rms " << rms
                      << ", max 2nd difference " << maxStep << " ("
                      << maxStep / juce::jmax(1e-12, rms) << " x rms), " << rtf << " x real time"
                      << std::endl;
            check(finite && rms > 1.0e-4, "plan: L-shape reflections are there and finite");
            check(maxStep < 2.0 * rms, "plan: no clicks when reflection paths come and go");
        }

        // ---------------- the sound of the pipe: Ansprache, Abriss, upper partials, air, resonated
        // clicks, glitches ----------------
        {
            // one Praestant 8' note (key a', MIDI 69), stereo, clicks off unless asked; released at
            // 'relAt' seconds
            auto note = [&](std::function<void(EngineParams &)> setup, double secs, double relAt,
                            const juce::String &file, bool clicks = false, float dist = 0.0f,
                            std::shared_ptr<const SampleLibrary> clickLibrary = nullptr)
            {
                OrganEngine e;
                e.prepare(fsr, B);
                e.setLibrary(clickLibrary != nullptr ? clickLibrary : lib);
                if (dist > 0.0f)
                    e.baseDist[0][(size_t)keyA] = dist;
                EngineParams p;
                p.stopOn = {};
                p.stopOn[0] = true;
                p.outputMode = 0;
                p.wind = 0;
                p.stopPull = 0;
                if (!clicks)
                {
                    p.clickLevel = 0;
                    p.releaseClick = 0;
                }
                setup(p);
                const int total = (int)(secs * fsr);
                juce::AudioBuffer<float> outB(2, total), b(2, B);
                outB.clear();
                for (int pos = 0; pos < total; pos += B)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)110), 0);
                    if (relAt > 0 && pos <= (int)(relAt * fsr) && pos + B > (int)(relAt * fsr))
                        mb.addEvent(juce::MidiMessage::noteOff(1, 69), (int)(relAt * fsr) - pos);
                    e.process(b, mb, p);
                    for (int c = 0; c < 2; ++c)
                        outB.copyFrom(c, pos, b, c, 0, juce::jmin(B, total - pos));
                }
                if (file.isNotEmpty())
                    writeWav(outDir.getChildFile(file + ".wav"), outB, fsr);
                return outB;
            };
            auto energy = [](const juce::AudioBuffer<float> &b, double t0, double t1, double sr0)
            {
                double e = 0;
                for (int i = (int)(t0 * sr0); i < juce::jmin(b.getNumSamples(), (int)(t1 * sr0));
                     ++i)
                    e += (double)b.getSample(0, i) * b.getSample(0, i);
                return e;
            };
            auto db = [](double a, double b)
            { return 10.0 * std::log10(juce::jmax(1e-30, a) / juce::jmax(1e-30, b)); };
            // energy above (high) or below a frequency, 6th order (three cascaded biquads), channel
            // 0, t0..t1
            auto bandE =
                [&](const juce::AudioBuffer<float> &b, double f, bool high, double t0, double t1)
            {
                juce::IIRFilter fl[3];
                for (auto &x : fl)
                    x.setCoefficients(high ? juce::IIRCoefficients::makeHighPass(fsr, f)
                                           : juce::IIRCoefficients::makeLowPass(fsr, f));
                double e = 0;
                for (int i = 0; i < juce::jmin(b.getNumSamples(), (int)(t1 * fsr)); ++i)
                {
                    float y = b.getSample(0, i);
                    for (auto &x : fl)
                        y = x.processSingleSampleRaw(y);
                    if (i >= (int)(t0 * fsr))
                        e += (double)y * y;
                }
                return e;
            };
            auto finite = [](const juce::AudioBuffer<float> &b)
            {
                for (int c = 0; c < b.getNumChannels(); ++c)
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        if (!std::isfinite(b.getSample(c, i)))
                            return false;
                return true;
            };
            const auto &pa = lib->pipes[0][(size_t)keyA];
            const double tOn = pa.onset / 48000.0, tAttFull = pa.attackEnd / 48000.0;
            const double tAtt =
                tOn + 0.4 * (tAttFull - tOn); // the chiff: the first part of the measured attack
            std::cout << "pipe sound: Praestant a' attack " << (tAttFull - tOn) * 1000.0 << " ms"
                      << std::endl;
            auto dry = note([](EngineParams &) {}, 2.0, -1, "");
            auto dryLong = note([](EngineParams &) {}, 1.5, -1, "");
            const double dA = energy(dry, tOn, tAtt, fsr), dS = energy(dry, 1.0, 1.8, fsr);
            // Ansprache: louder / softer chiff, the sustain the same
            auto up = note([](EngineParams &p) { p.pipeSnd[0].attGain = 1.0f; }, 2.0, -1,
                           "60_ansprache_plus");
            auto down = note([](EngineParams &p) { p.pipeSnd[0].attGain = -1.0f; }, 2.0, -1, "");
            std::cout << "Ansprache +1: attack " << db(energy(up, tOn, tAtt, fsr), dA)
                      << " dB, sustain " << db(energy(up, 1.0, 1.8, fsr), dS) << " dB; -1: attack "
                      << db(energy(down, tOn, tAtt, fsr), dA) << " dB" << std::endl;
            check(db(energy(up, tOn, tAtt, fsr), dA) > 4.0 &&
                      std::abs(db(energy(up, 1.0, 1.8, fsr), dS)) < 0.05 &&
                      db(energy(down, tOn, tAtt, fsr), dA) < -4.0,
                  "Ansprache: the attack louder / softer, the sustain unchanged");
            // sharpness: more highs in the attack
            {
                auto sh = note([](EngineParams &p) { p.pipeSnd[0].attSharp = 1.0f; }, 1.0, -1, "");
                auto hp = [&](const juce::AudioBuffer<float> &b)
                { return bandE(b, 4000.0, true, tOn, tAtt); };
                std::cout << "Ansprache Schaerfe: highs in the attack " << db(hp(sh), hp(dry))
                          << " dB" << std::endl;
                check(db(hp(sh), hp(dry)) > 3.0,
                      "Ansprache: sharpness lifts the highs of the chiff");
            }
            // isolation: only the chiff
            {
                auto iso = note([](EngineParams &p) { p.pipeSnd[0].attIso = 1.0f; }, 2.0, -1,
                                "61_nur_ansprache");
                std::cout << "nur Ansprache: attack "
                          << db(energy(iso, tOn, tOn + 0.3 * (tAtt - tOn), fsr),
                                energy(dry, tOn, tOn + 0.3 * (tAtt - tOn), fsr))
                          << " dB, sustain " << db(energy(iso, 1.0, 1.8, fsr), dS) << " dB"
                          << std::endl;
                check(db(energy(iso, 1.0, 1.8, fsr), dS) < -60.0 &&
                          db(energy(iso, tOn, tOn + 0.3 * (tAtt - tOn), fsr),
                             energy(dry, tOn, tOn + 0.3 * (tAtt - tOn), fsr)) > -3.0,
                      "Ansprache: isolated, only the chiff remains");
            }
            // repeated chiffs: a burst (count the energy peaks of the isolated attack)
            {
                auto rep = note(
                    [](EngineParams &p)
                    {
                        p.pipeSnd[0].attRep = 1.0f;
                        p.pipeSnd[0].attIso = 1.0f;
                    },
                    1.5, -1, "62_ansprache_wiederholt");
                auto one = note([](EngineParams &p) { p.pipeSnd[0].attIso = 1.0f; }, 1.5, -1, "");
                std::cout << "Ansprache wiederholt: energy after the attack "
                          << db(energy(rep, tAtt, 1.2, fsr), energy(one, tAtt, 1.2, fsr))
                          << " dB more" << std::endl;
                check(finite(rep) &&
                          db(energy(rep, tAtt, 1.2, fsr), energy(one, tAtt, 1.2, fsr)) > 6.0,
                      "Ansprache: the chiff repeats as a burst");
            }
            // Abriss: a puff at the release, stronger with a harder key
            {
                auto pf =
                    note([](EngineParams &p) { p.pipeSnd[0].puff = 1.0f; }, 2.0, 1.0, "63_abriss");
                auto np = note([](EngineParams &) {}, 2.0, 1.0, "");
                double d = 0, ref = 0;
                for (int i = (int)(1.0 * fsr); i < (int)(1.15 * fsr); ++i)
                {
                    const double x = pf.getSample(0, i) - np.getSample(0, i);
                    d += x * x;
                }
                ref = energy(np, 1.0, 1.15, fsr);
                double before = 0;
                for (int i = 0; i < (int)(0.99 * fsr); ++i)
                {
                    const double x = pf.getSample(0, i) - np.getSample(0, i);
                    before += x * x;
                }
                std::cout << "Abriss: puff " << db(d, ref) << " dB against the release, before it "
                          << before << std::endl;
                check(before == 0.0 && db(d, ref) > -30.0 && db(d, ref) < 6.0,
                      "Abriss: a puff when the key is let go (and nothing before)");
            }
            // upper partials: +12 dB above 3 kHz, the lows the same
            {
                auto hi = note(
                    [](EngineParams &p)
                    {
                        p.pipeSnd[0].hiFreq = 3000.0f;
                        p.pipeSnd[0].hiGain = 12.0f;
                    },
                    1.5, -1, "64_obertoene_laut");
                auto band = [&](const juce::AudioBuffer<float> &b, bool high)
                { return bandE(b, high ? 6000.0 : 1200.0, high, 0.8, 1.4); };
                std::cout << "Obertoene +12 dB: highs " << db(band(hi, true), band(dry, true))
                          << " dB, lows " << db(band(hi, false), band(dry, false)) << " dB"
                          << std::endl;
                check(db(band(hi, true), band(dry, true)) > 5.0 &&
                          std::abs(db(band(hi, false), band(dry, false))) < 1.5,
                      "Obertoene: the upper partials alone get louder");
                auto sp = note(
                    [](EngineParams &p)
                    {
                        p.pipeSnd[0].hiFreq = 2000.0f;
                        p.pipeSnd[0].hiShape = 1.0f;
                    },
                    1.5, -1, "");
                check(finite(sp), "Obertoene: transient shaper finite");
            }
            // air inversion: a far pipe gets its highs back
            {
                auto far = [&](float inv)
                {
                    return note(
                        [inv](EngineParams &p)
                        {
                            p.air = 1.0f;
                            p.airInvert = inv;
                            p.distAmount = 1.0f;
                        },
                        1.2, -1, "", false, 40.0f);
                };
                auto f0 = far(0.0f), fH = far(0.5f), f1 = far(1.0f);
                auto hiE = [&](const juce::AudioBuffer<float> &b)
                {
                    return bandE(b, 5000.0, true, 0.6, 1.1) /
                           juce::jmax(1e-30, bandE(b, 1500.0, false, 0.6, 1.1));
                };
                auto noAir = note(
                    [](EngineParams &p)
                    {
                        p.air = 0.0f;
                        p.distAmount = 1.0f;
                    },
                    1.2, -1, "", false, 40.0f);
                std::cout << "(air off vs on at 40 m: highs " << db(hiE(noAir), hiE(f0)) << " dB)"
                          << std::endl;
                std::cout << "Luft umkehren (pipe at 40 m): highs 50 % " << db(hiE(fH), hiE(f0))
                          << " dB, 100 % " << db(hiE(f1), hiE(f0)) << " dB" << std::endl;
                check(db(hiE(fH), hiE(f0)) > 2.0 && db(hiE(f1), hiE(fH)) > 2.0,
                      "Luft umkehren: distant highs restored and exaggerated");
            }
            // resonated clicks: the key click rings at the pitch of the key
            {
                // A recorded click can still contain broadband sound in the tail window.
                // Use a short impulse so this test measures the resonator's own ringing.
                auto clickLibrary = std::make_shared<SampleLibrary>(*lib);
                auto impulse = std::make_shared<Sample>();
                impulse->sampleRate = fsr;
                impulse->data.assign(128, 0.0f);
                impulse->data.front() = 1.0f;
                NoiseSample click;
                click.sample = impulse;
                click.kind = "down";
                clickLibrary->clicksDown = {click};
                clickLibrary->clicksUp.clear();
                auto plain = note([](EngineParams &p) { p.stopOn = {}; }, 1.0, 0.5, "", true, 0.0f,
                                  clickLibrary);
                auto res = note(
                    [](EngineParams &p)
                    {
                        p.stopOn = {};
                        p.resClick = 1.0f;
                        p.resDecay = 0.6f;
                    },
                    1.0, 0.5, "65_klick_resonanz", true, 0.0f, clickLibrary);
                // where is the energy of the ringing? a resonator test at the key's pitch vs. half
                // a tone away
                const double hz = (pa.measuredHz > 0 ? pa.measuredHz : pa.nominalHz);
                auto atF = [&](const juce::AudioBuffer<float> &b, double f)
                {
                    double c = 0, s2 = 0;
                    for (int i = (int)(0.05 * fsr); i < (int)(0.45 * fsr); ++i)
                    {
                        const double ph = juce::MathConstants<double>::twoPi * f * i / fsr;
                        c += b.getSample(0, i) * std::cos(ph);
                        s2 += b.getSample(0, i) * std::sin(ph);
                    }
                    return c * c + s2 * s2;
                };
                const double tail = energy(res, 0.15, 0.45, fsr),
                             tailPlain = energy(plain, 0.15, 0.45, fsr);
                std::cout << "Klick-Resonanz: tail " << db(tail, tailPlain)
                          << " dB over the plain click, at the key's pitch "
                          << db(atF(res, hz), atF(res, hz * 1.06)) << " dB over a semitone up"
                          << std::endl;
                check(finite(res) && tail > 1.0e-12 && db(tail, tailPlain) > 6.0 &&
                          db(atF(res, hz), atF(res, hz * 1.06)) > 10.0,
                      "Klick-Resonanz: the click rings at the key's pitch");
            }
            // glitches: all four, each changes the sound, everything finite
            {
                const char *names[]{"stutter", "splice", "zero", "clip"};
                for (int k = 0; k < 4; ++k)
                {
                    auto g = note(
                        [k](EngineParams &p)
                        {
                            float *f[]{&p.glitch.stutter, &p.glitch.splice, &p.glitch.zero,
                                       &p.glitch.clip};
                            *f[k] = 1.0f;
                        },
                        7.0, 6.5, "66_glitch_" + juce::String(names[k]));
                    auto ref = note([](EngineParams &) {}, 7.0, 6.5, "");
                    double d = 0;
                    for (int i = 0; i < g.getNumSamples(); ++i)
                    {
                        const double x = g.getSample(0, i) - ref.getSample(0, i);
                        d += x * x;
                    }
                    float pk = 0;
                    for (int i = 0; i < g.getNumSamples(); ++i)
                        pk = juce::jmax(pk, std::abs(g.getSample(0, i)));
                    std::cout << "glitch " << names[k] << ": difference "
                              << db(d, energy(ref, 0, 7.0, fsr)) << " dB, peak "
                              << juce::Decibels::gainToDecibels(pk) << " dB" << std::endl;
                    check(finite(g) && db(d, energy(ref, 0, 7.0, fsr)) > -40.0 && pk < 1.0f,
                          juce::String("glitch ") + names[k] + " changes the sound, finite");
                }
            }
            // per-pipe modulation of the new targets: a route from a constant source (LFO 1,
            // square, almost still: +1) must sound exactly like setting the stop's parameter by
            // hand
            {
                auto lfoOne = [](EngineParams &p, int tgt, float amt)
                {
                    p.mod.lfo[0].shape = shSquare;
                    p.mod.lfo[0].rate = 1.0e-4f;
                    p.mod.lfo[0].spread = 0.0f;
                    p.mod.routes[0] = {srcLfo1, tgt, amt, false};
                };
                auto maxDiff =
                    [](const juce::AudioBuffer<float> &a, const juce::AudioBuffer<float> &b)
                {
                    float m = 0.0f;
                    for (int c = 0; c < 2; ++c)
                        for (int i = 0; i < a.getNumSamples(); ++i)
                            m = juce::jmax(m, std::abs(a.getSample(c, i) - b.getSample(c, i)));
                    return m;
                };
                struct Case
                {
                    const char *name;
                    int tgt;
                    float amt;
                    std::function<void(EngineParams &)> direct;
                    bool degMode;
                };
                const std::vector<Case> cases{
                    {"Ansprache", tgtAttack, 0.5f,
                     [](EngineParams &p) { p.pipeSnd[0].attGain = 0.5f; }, false},
                    {"Ansprache Schaerfe", tgtAttSharp, 0.75f,
                     [](EngineParams &p) { p.pipeSnd[0].attSharp = 0.75f; }, false},
                    {"Nur Ansprache", tgtAttIso, 1.0f,
                     [](EngineParams &p) { p.pipeSnd[0].attIso = 1.0f; }, false},
                    {"Ansprache wiederholen", tgtAttRep, 0.6f,
                     [](EngineParams &p) { p.pipeSnd[0].attRep = 0.6f; }, false},
                    {"Abriss", tgtPuff, 1.0f, [](EngineParams &p) { p.pipeSnd[0].puff = 1.0f; },
                     false},
                    {"Obertoene ab", tgtHiFreq, 0.25f,
                     [](EngineParams &p)
                     {
                         p.pipeSnd[0].hiFreq = 8000.0f;
                         p.pipeSnd[0].hiGain = 9.0f;
                     },
                     false},
                    {"Obertoene Pegel", tgtHiGain, 0.25f,
                     [](EngineParams &p) { p.pipeSnd[0].hiGain = 6.0f; }, false},
                    {"Obertoene Anschlag", tgtHiShape, 1.0f,
                     [](EngineParams &p) { p.pipeSnd[0].hiShape = 1.0f; }, false},
                    {"Luft umkehren", tgtAirInvert, 1.0f,
                     [](EngineParams &p) { p.airInvert = 1.0f; }, false},
                    {"Wolke Menge", tgtCloudMix, 0.7f,
                     [](EngineParams &p) { p.cloud[0].mix = 0.7f; }, false},
                    {"Koernung", tgtDegGrain, 0.5f, [](EngineParams &p) { p.deg.grain = 1.0f; },
                     true},
                    {"Bittiefe", tgtDegBits, -1.0f, [](EngineParams &p) { p.deg.bits = 3.0f; },
                     true},
                    {"Abtastrate", tgtDegRate, -0.5f,
                     [](EngineParams &p) { p.deg.rateHz = 2756.25f; }, true},
                };
                for (const auto &cs : cases)
                {
                    const bool hiFreqCase = cs.tgt == tgtHiFreq;
                    auto base = [&](EngineParams &p)
                    {
                        p.airInvert = 0.0f;
                        if (cs.degMode)
                        {
                            p.deg.perPipe = true;
                            p.deg.amount = 1.0f;
                            p.deg.grain = 0.5f;
                            p.deg.bits = 16.0f;
                            p.deg.rateHz = 22050.0f;
                            p.deg.dropouts = 0.0f;
                            p.deg.wobble = 0.0f;
                        }
                        if (hiFreqCase)
                            p.pipeSnd[0].hiGain = 9.0f; // the frequency only matters with a level
                    };
                    const float dist = cs.tgt == tgtAirInvert ? 30.0f : 0.0f;
                    auto viaMod = note(
                        [&](EngineParams &p)
                        {
                            base(p);
                            lfoOne(p, cs.tgt, cs.amt);
                        },
                        1.6, 1.0, "", false, dist);
                    auto byHand = note(
                        [&](EngineParams &p)
                        {
                            base(p);
                            cs.direct(p);
                        },
                        1.6, 1.0, "", false, dist);
                    auto plain = note([&](EngineParams &p) { base(p); }, 1.6, 1.0, "", false, dist);
                    const float d = maxDiff(viaMod, byHand), changed = maxDiff(byHand, plain);
                    std::cout << "per-pipe target " << cs.name << ": route vs by hand max diff "
                              << d << " (the setting itself changes up to " << changed << ")"
                              << std::endl;
                    check(finite(viaMod) && d < 1.0e-6f && changed > 1.0e-4f,
                          juce::String("per-pipe target ") + cs.name + " = the parameter by hand");
                }
                // glitches (random draws differ): the route turns them on, the result changes and
                // stays finite
                const int gt[]{tgtStutter, tgtSplice, tgtZero, tgtClip};
                const char *gn[]{"stutter", "splice", "zero", "clip"};
                auto ref = note([](EngineParams &) {}, 7.0, 6.5, "");
                for (int k = 0; k < 4; ++k)
                {
                    auto g = note([&](EngineParams &p) { lfoOne(p, gt[k], 1.0f); }, 7.0, 6.5,
                                  k == 0 ? "67_glitch_per_pipe_mod" : "");
                    double d2 = 0;
                    for (int i = 0; i < g.getNumSamples(); ++i)
                    {
                        const double x = g.getSample(0, i) - ref.getSample(0, i);
                        d2 += x * x;
                    }
                    std::cout << "per-pipe target glitch " << gn[k] << ": difference "
                              << db(d2, energy(ref, 0, 7.0, fsr)) << " dB" << std::endl;
                    check(finite(g) && db(d2, energy(ref, 0, 7.0, fsr)) > -40.0,
                          juce::String("per-pipe target glitch ") + gn[k] + " works");
                }
                // a different value for every pipe: Ansprache from the key position, low key vs.
                // high key are not the same
                {
                    auto keyed =
                        note([&](EngineParams &p)
                             { p.mod.routes[0] = {srcKey, tgtAttack, 1.0f, false}; }, 1.0, -1, "");
                    auto flat = note([](EngineParams &) {}, 1.0, -1, "");
                    const double kr =
                        db(energy(keyed, tOn, tAtt, fsr), energy(flat, tOn, tAtt, fsr));
                    std::cout << "per-pipe Ansprache from the key position (a' sits a bit above "
                                 "the middle): attack "
                              << kr << " dB" << std::endl;
                    check(std::abs(kr) > 0.3,
                          "per-pipe target: Ansprache follows the key position");
                }
            }
        }

        // ---------------- the tuning in space: beats, Tonnetz, difference tones; ice walls; double
        // / long exposure ----------------
        {
            // two (or more) Praestant notes held together; returns the engine so its state can be
            // read
            auto chord = [&](OrganEngine &e, std::function<void(EngineParams &)> setup,
                             std::vector<int> midis, double secs, int outMode,
                             juce::AudioBuffer<float> *keepOut)
            {
                e.prepare(fsr, B);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[0] = true;
                p.clickLevel = 0;
                p.releaseClick = 0;
                p.wind = 0;
                p.stopPull = 0;
                p.outputMode = outMode;
                setup(p);
                const int ch = outMode == 0 ? 2 : (outMode + 1) * (outMode + 1);
                const int total = (int)(secs * fsr);
                juce::AudioBuffer<float> b(ch, B);
                if (keepOut != nullptr)
                {
                    keepOut->setSize(ch, total);
                    keepOut->clear();
                }
                for (int pos = 0; pos < total; pos += B)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        for (int m : midis)
                            mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), 0);
                    b.clear();
                    e.process(b, mb, p);
                    if (keepOut != nullptr)
                        for (int c = 0; c < ch; ++c)
                            keepOut->copyFrom(c, pos, b, c, 0, juce::jmin(B, total - pos));
                }
            };
            auto finiteB = [](const juce::AudioBuffer<float> &b)
            {
                for (int c = 0; c < b.getNumChannels(); ++c)
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        if (!std::isfinite(b.getSample(c, i)))
                            return false;
                return true;
            };
            // beats in 1/4-comma meantone (ideal): pure third c'-e' still, fifth c'-g' slow, wolf
            // g#-eb' fast
            {
                struct Iv
                {
                    const char *name;
                    int a, b;
                    float lo, hi;
                };
                const Iv ivs[]{{"major third c'-e' (pure)", 60, 64, -0.01f, 0.05f},
                               {"fifth c'-g'", 60, 67, 1.8f, 3.2f},
                               {"minor third e'-g'", 64, 67, 2.0f, 6.5f},
                               {"wolf fifth g#-eb'", 56, 63, 6.0f, 16.0f}};
                float rates[4]{};
                for (int k = 0; k < 4; ++k)
                {
                    OrganEngine e;
                    chord(
                        e, [](EngineParams &p) { p.motionMode[0] = (int)Motion::Beat; },
                        {ivs[k].a, ivs[k].b}, 2.0, 1, nullptr);
                    rates[k] = e.beatMaxNow.load();
                    std::cout << "beats " << ivs[k].name << ": " << rates[k] << " Hz" << std::endl;
                    check(rates[k] >= ivs[k].lo && rates[k] <= ivs[k].hi,
                          juce::String("beats: ") + ivs[k].name);
                }
                // as recorded: the pure third beats a little (the pipes are not exactly in tune)
                OrganEngine er;
                chord(
                    er,
                    [](EngineParams &p)
                    {
                        p.beatIdeal = false;
                        p.motionMode[0] = (int)Motion::Beat;
                    },
                    {60, 64}, 2.0, 1, nullptr);
                std::cout << "beats major third as recorded: " << er.beatMaxNow.load() << " Hz"
                          << std::endl;
                check(er.beatMaxNow.load() > rates[0],
                      "beats: the recorded pipes beat where the tuning is pure");
                // motion "Schwebung": the wolf pipes have turned far, the pure third has not moved
                OrganEngine ew, et;
                auto beatMotion = [](EngineParams &p)
                {
                    p.motionMode[0] = (int)Motion::Beat;
                    p.motionRate[0] = 0.08f;
                };
                chord(ew, beatMotion, {56, 63}, 3.0, 1, nullptr);
                chord(et, beatMotion, {60, 64}, 3.0, 1, nullptr);
                const float turnWolf = ew.beatTurn[0][(size_t)keyIndexForMidi(56)].load(),
                            turnThird = et.beatTurn[0][(size_t)keyIndexForMidi(60)].load();
                std::cout << "Schwebung motion after 3 s: wolf pipe turned to " << turnWolf
                          << " deg, pure third " << turnThird << " deg" << std::endl;
                check(std::abs(std::remainder(turnThird, 360.0f)) < 2.0f && turnWolf > 20.0f,
                      "Schwebung: the wolf spins, the pure third stands still");
            }
            // Tonnetz: meantone keeps c-e-g together and puts g# and eb at opposite edges;
            // Pythagorean lines the fifths up
            {
                auto placeOf = [&](int t, int midi)
                {
                    float sf, st;
                    const auto kt = makeKeyTuning(t);
                    return tonnetzPlaces(kt, tuningStepCents(t), sf,
                                         st)[(size_t)keyIndexForMidi(midi)];
                };
                auto show = [&](int t, const char *name)
                {
                    std::cout << "Tonnetz " << name << ":";
                    for (int m : {60, 62, 64, 65, 67, 69, 71, 63, 68})
                    {
                        auto q = placeOf(t, m);
                        std::cout << " " << helmholtzName(m) << "(" << q.a << "," << q.b << ")";
                    }
                    std::cout << std::endl;
                };
                show(0, "1/4-comma");
                show(5, "Pythagorean");
                show(6, "equal");
                const auto c = placeOf(0, 60), e = placeOf(0, 64), g = placeOf(0, 67),
                           gs = placeOf(0, 68), eb = placeOf(0, 63);
                auto dist = [](TonnetzPlace x, TonnetzPlace y)
                { return std::abs(x.a - y.a) + std::abs(x.b - y.b); };
                bool pythLine = true;
                for (int m = 60; m < 72; ++m)
                    if (keyIndexForMidi(m) >= 0)
                        pythLine = pythLine && placeOf(5, m).b == 0;
                check(dist(c, e) <= 1 && dist(c, g) <= 1 && dist(gs, eb) >= 4 && pythLine,
                      "Tonnetz: meantone triad compact, the wolf a seam, Pythagorean a chain of "
                      "fifths");
                std::array<Pos, kNumKeys> pos{};
                LayoutSettings ls;
                ls.type = (int)Layout::Tonnetz;
                const auto kt = makeKeyTuning(0);
                generateLayout(ls, 0, pos, &kt, 0.0);
                bool ok = true;
                for (auto &q : pos)
                    ok = ok && std::isfinite(q.az) && std::abs(q.el) <= 85.0f;
                check(ok, "Tonnetz: a layout on the sphere");
            }
            // difference tones: a held fifth c'-g' sounds c (an octave below c') between / below
            // the pipes
            {
                const double fC = meantoneHz(60), fG = meantoneHz(67), fd = fG - fC;
                juce::AudioBuffer<float> with, without;
                OrganEngine e1, e0;
                chord(
                    e1,
                    [](EngineParams &p)
                    {
                        p.ghostLevel = 1.0f;
                        p.ghostPlace = 1.0f;
                    },
                    {60, 67}, 2.5, 1, &with);
                chord(e0, [](EngineParams &) {}, {60, 67}, 2.5, 1, &without);
                auto atF = [&](const juce::AudioBuffer<float> &b, int c, double f)
                {
                    double re = 0, im = 0;
                    for (int i = (int)(1.2 * fsr); i < (int)(2.4 * fsr); ++i)
                    {
                        const double ph = juce::MathConstants<double>::twoPi * f * i / fsr;
                        re += b.getSample(c, i) * std::cos(ph);
                        im += b.getSample(c, i) * std::sin(ph);
                    }
                    return re * re + im * im;
                };
                const double gain =
                    10.0 * std::log10(atF(with, 0, fd) / juce::jmax(1e-30, atF(without, 0, fd)));
                // the ghost sits below: Z (ACN 2) negative relative to W at the difference
                // frequency
                double zw = 0;
                for (int i = (int)(1.2 * fsr); i < (int)(2.4 * fsr); ++i)
                    zw += (double)(with.getSample(2, i) - without.getSample(2, i)) *
                          (with.getSample(0, i) - without.getSample(0, i));
                std::cout << "difference tone of c'-g': " << fd << " Hz, " << gain
                          << " dB over the plain fifth at that frequency, ghosts "
                          << e1.ghostsNow.load()
                          << ", below the listener: " << (zw < 0 ? "yes" : "no") << std::endl;
                writeWav(outDir.getChildFile("70_differenzton_quinte.wav"), with, fsr);
                check(e1.ghostsNow.load() >= 1 && gain > 10.0 && zw < 0 && finiteB(with),
                      "difference tones: the fifth's ghost an octave below c', under the floor");
            }
            // ice: the reflections chirp - low frequencies of the early reflections arrive later
            {
                DispersionBank bank;
                const int N = 8192;
                std::vector<float> x((size_t)N, 0.0f);
                x[0] = 1.0f;
                float *px = x.data();
                bank.process(&px, 1, N, 64, -0.9f);
                auto peakAt = [&](bool low)
                {
                    juce::IIRFilter fl;
                    fl.setCoefficients(low ? juce::IIRCoefficients::makeLowPass(fsr, 300.0)
                                           : juce::IIRCoefficients::makeHighPass(fsr, 4000.0));
                    double best = 0;
                    int at = 0, i0 = 0;
                    double acc = 0, tot = 0;
                    for (int i = 0; i < N; ++i)
                    {
                        const double y = fl.processSingleSampleRaw(x[(size_t)i]);
                        acc += y * y * i;
                        tot += y * y;
                        if (std::abs(y) > best)
                        {
                            best = std::abs(y);
                            i0 = i;
                        }
                    }
                    at = (int)(acc / juce::jmax(1e-30, tot));
                    (void)i0;
                    return at;
                };
                const int tl = peakAt(true), th = peakAt(false);
                std::cout << "ice: all-pass chain, energy arrives at " << tl / 48.0
                          << " ms (below 300 Hz) and " << th / 48.0 << " ms (above 4 kHz)"
                          << std::endl;
                juce::AudioBuffer<float> iceB, plainB;
                OrganEngine e1, e0;
                auto room = [](EngineParams &p)
                {
                    p.room.on = true;
                    p.room.width = 12;
                    p.room.length = 18;
                    p.room.height = 8;
                    p.clickLevel = 0.8f;
                };
                chord(
                    e1,
                    [&](EngineParams &p)
                    {
                        room(p);
                        p.ice = 1.0f;
                        p.iceLen = 0.8f;
                    },
                    {69}, 1.5, 3, &iceB);
                chord(e0, room, {69}, 1.5, 3, &plainB);
                double d = 0, e = 0;
                for (int i = 0; i < iceB.getNumSamples(); ++i)
                {
                    const double t = iceB.getSample(0, i) - plainB.getSample(0, i);
                    d += t * t;
                    e += (double)plainB.getSample(0, i) * plainB.getSample(0, i);
                }
                std::cout << "ice in the Saal: difference "
                          << 10.0 * std::log10(d / juce::jmax(1e-30, e)) << " dB" << std::endl;
                writeWav(outDir.getChildFile("71_eis_saal.wav"), iceB, fsr);
                check(tl > th + 48 && finiteB(iceB) && d > 1e-4 * e,
                      "ice: the lows come later (a falling chirp), the Saal changes");
            }
            // double exposure (class): an impulse from the left comes back 0.5 s later, mirrored to
            // the right / turned to the front
            {
                const int N = (int)(1.0 * fsr);
                auto runDex = [&](int mirror, float turn, double &azBack, double &level)
                {
                    DoubleExposure dx;
                    dx.prepare(fsr, 2);
                    std::vector<std::vector<float>> ch(16, std::vector<float>((size_t)N, 0.0f));
                    float enc[kMaxAmbiChannels]{};
                    encodeAmbiX(90.0f, 0.0f, 3, enc); // from the left
                    for (int c = 0; c < 16; ++c)
                        ch[(size_t)c][100] = enc[c];
                    std::vector<float *> pp;
                    for (auto &v : ch)
                        pp.push_back(v.data());
                    DoubleExposureParams dp;
                    dp.mix = 1.0f;
                    dp.time = 0.5f;
                    dp.mirror = mirror;
                    dp.turn = turn;
                    dp.fade = 0.0f;
                    for (int pos = 0; pos < N; pos += 256)
                    {
                        std::vector<float *> q;
                        for (auto *x : pp)
                            q.push_back(x + pos);
                        dx.process(q.data(), 16, false, juce::jmin(256, N - pos), dp);
                    }
                    double w = 0, y = 0, x = 0, e2 = 0;
                    for (int i = (int)(0.45 * fsr); i < N; ++i)
                    {
                        w += ch[0][(size_t)i];
                        y += ch[1][(size_t)i];
                        x += ch[3][(size_t)i];
                        e2 += ch[0][(size_t)i] * ch[0][(size_t)i];
                    }
                    azBack = juce::radiansToDegrees(std::atan2(y, x));
                    level = e2;
                };
                double a0, l0, a1, l1, a2, l2;
                runDex(0, 0.0f, a0, l0);
                runDex(1, 0.0f, a1, l1);
                runDex(0, -90.0f, a2, l2);
                std::cout << "double exposure: the copy comes from " << a0 << " deg (plain), " << a1
                          << " deg (mirrored), " << a2 << " deg (turned -90)" << std::endl;
                check(std::abs(a0 - 90.0) < 3.0 && std::abs(a1 + 90.0) < 3.0 &&
                          std::abs(a2) < 3.0 && l0 > 0.0,
                      "double exposure: delayed, mirrored, turned");
                // in the engine: the field of 1 s ago laid over the present, everything finite
                juce::AudioBuffer<float> dexB;
                OrganEngine ed;
                chord(
                    ed,
                    [](EngineParams &p)
                    {
                        p.dex.mix = 0.8f;
                        p.dex.time = 0.7f;
                        p.dex.turn = 120.0f;
                        p.dex.again = 0.5f;
                    },
                    {60, 64, 67}, 2.0, 3, &dexB);
                std::vector<float *> chp;
                for (int c = 0; c < dexB.getNumChannels(); ++c)
                    chp.push_back(dexB.getWritePointer(c));
                EngineParams pp;
                pp.dex.mix = 0.8f;
                pp.dex.time = 0.7f;
                pp.dex.turn = 120.0f;
                pp.dex.again = 0.5f;
                pp.outputMode = 3;
                for (int pos = 0; pos < dexB.getNumSamples(); pos += 512)
                {
                    std::vector<float *> q;
                    for (auto *x : chp)
                        q.push_back(x + pos);
                    ed.applyPhotoFx(q.data(), 3, juce::jmin(512, dexB.getNumSamples() - pos), pp);
                }
                writeWav(outDir.getChildFile("72_doppelbelichtung.wav"), dexB, fsr);
                check(finiteB(dexB), "double exposure in the engine: finite");
            }
            // long exposure (class): a burst from the front leaves a trail there that fades over
            // the exposure time
            {
                const int N = (int)(3.0 * fsr);
                LongExposure lx;
                lx.prepare(fsr);
                std::vector<std::vector<float>> ch(16, std::vector<float>((size_t)N, 0.0f));
                float enc[kMaxAmbiChannels]{};
                encodeAmbiX(0.0f, 0.0f, 3, enc);
                juce::Random rnd(3);
                for (int i = 0; i < (int)(0.1 * fsr); ++i)
                {
                    const float v = 0.3f * (rnd.nextFloat() * 2.0f - 1.0f);
                    for (int c = 0; c < 16; ++c)
                        ch[(size_t)c][(size_t)i] = v * enc[c];
                }
                std::vector<float *> pp;
                for (auto &v : ch)
                    pp.push_back(v.data());
                LongExposureParams lp;
                lp.mix = 1.0f;
                lp.time = 2.0f;
                lp.bright = 0.6f;
                for (int pos = 0; pos < N; pos += 256)
                {
                    std::vector<float *> q;
                    for (auto *x : pp)
                        q.push_back(x + pos);
                    lx.process(q.data(), 16, false, juce::jmin(256, N - pos), lp);
                }
                auto win = [&](double t0, double t1, double &w2, double &az)
                {
                    double y = 0, x = 0;
                    w2 = 0;
                    for (int i = (int)(t0 * fsr); i < (int)(t1 * fsr); ++i)
                    {
                        const double w = ch[0][(size_t)i];
                        w2 += w * w;
                        y += w * ch[1][(size_t)i];
                        x += w * ch[3][(size_t)i];
                    }
                    az = juce::radiansToDegrees(std::atan2(y, x));
                };
                double eBurst, azB, e05, az05, e15, az15;
                win(0.0, 0.1, eBurst, azB);
                win(0.4, 0.6, e05, az05);
                win(1.4, 1.6, e15, az15);
                const double d05 = 10.0 * std::log10(e05 / eBurst * 0.5),
                             d15 = 10.0 * std::log10(e15 / eBurst * 0.5);
                std::cout << "long exposure (2 s): trail at 0.5 s " << d05 << " dB, at 1.5 s "
                          << d15 << " dB (burst = 0 dB), from " << az05 << " deg" << std::endl;
                check(d05 > -30.0 && d15 < d05 - 10.0 && std::abs(az05) < 15.0,
                      "long exposure: a trail where the sound was, fading");
                // in the engine: a rotating pipe paints a ring
                juce::AudioBuffer<float> exB;
                OrganEngine ex;
                chord(
                    ex,
                    [](EngineParams &p)
                    {
                        p.motionMode[0] = (int)Motion::Rotation;
                        p.motionRate[0] = 0.5f;
                    },
                    {69}, 2.0, 3, &exB);
                std::vector<float *> chp;
                for (int c = 0; c < exB.getNumChannels(); ++c)
                    chp.push_back(exB.getWritePointer(c));
                EngineParams pp2;
                pp2.expo.mix = 0.8f;
                pp2.expo.time = 6.0f;
                pp2.outputMode = 3;
                for (int pos = 0; pos < exB.getNumSamples(); pos += 512)
                {
                    std::vector<float *> q;
                    for (auto *x : chp)
                        q.push_back(x + pos);
                    ex.applyPhotoFx(q.data(), 3, juce::jmin(512, exB.getNumSamples() - pos), pp2);
                }
                writeWav(outDir.getChildFile("73_belichtung_ring.wav"), exB, fsr);
                check(finiteB(exB), "long exposure in the engine: finite");
            }
        }

        // ---------------- "Raumverfall" and "Körnerwolke" ----------------
        {
            // energy per Ambisonics degree (0..3) of a rendered buffer, and the direction of the
            // first-order vector
            auto degreeEnergy =
                [](const juce::AudioBuffer<float> &b, int from, std::array<double, 4> &e)
            {
                e.fill(0.0);
                for (int c = 0; c < juce::jmin(16, b.getNumChannels()); ++c)
                {
                    const int l = (int)std::floor(std::sqrt((double)c) + 1e-9);
                    for (int i = from; i < b.getNumSamples(); ++i)
                        e[(size_t)l] += (double)b.getSample(c, i) * b.getSample(c, i);
                }
            };
            auto direction = [](const juce::AudioBuffer<float> &b, int from, double &az, double &rE)
            {
                double wy = 0, wx = 0, wz = 0, ww = 0, yy = 0, xx = 0, zz = 0;
                for (int i = from; i < b.getNumSamples(); ++i)
                {
                    const double w = b.getSample(0, i), y = b.getSample(1, i),
                                 z = b.getSample(2, i), x = b.getSample(3, i);
                    wy += w * y;
                    wx += w * x;
                    wz += w * z;
                    ww += w * w;
                    yy += y * y;
                    xx += x * x;
                    zz += z * z;
                }
                az = juce::radiansToDegrees(std::atan2(wy, wx));
                rE = std::sqrt(wx * wx + wy * wy + wz * wz) /
                     juce::jmax(1e-30, ww); // 1 = one direction, 0 = all around
            };
            auto place = [&](OrganEngine &e, float az, float el)
            {
                e.baseAz[0][(size_t)keyA] = az;
                e.baseEl[0][(size_t)keyA] = el;
                e.baseDist[0][(size_t)keyA] = 4.0f;
            };
            const int from = (int)(0.3 * fsr);
            std::array<double, 4> e0{}, e1{}, e2{};
            OrganEngine a0, a1, a2, a3;
            for (auto *e : {&a0, &a1, &a2, &a3})
                place(*e, -50.0f, 10.0f);
            auto dry = run([](EngineParams &) {}, 1.5, -1, "", 16, &a0);
            auto half = run([](EngineParams &p) { p.space.erode = 0.5f; }, 1.5, -1,
                            "50_space_erode_half", 16, &a1);
            auto full = run([](EngineParams &p) { p.space.erode = 1.0f; }, 1.5, -1, "", 16, &a2);
            degreeEnergy(dry, from, e0);
            degreeEnergy(half, from, e1);
            degreeEnergy(full, from, e2);
            auto rel = [](double a, double b)
            { return 10.0 * std::log10(juce::jmax(1e-30, a) / juce::jmax(1e-30, b)); };
            std::cout << "space erode 50 %: degree 0 " << rel(e1[0], e0[0]) << " dB, 1 "
                      << rel(e1[1], e0[1]) << " dB, 2 " << rel(e1[2], e0[2]) << " dB, 3 "
                      << rel(e1[3], e0[3]) << " dB; 100 %: degree 1 " << rel(e2[1], e0[1]) << " dB"
                      << std::endl;
            check(std::abs(rel(e1[0], e0[0])) < 0.01 && std::abs(rel(e1[1], e0[1])) < 0.01 &&
                      std::abs(rel(e1[2], e0[2]) + 6.0) < 1.0 && rel(e1[3], e0[3]) < -60.0 &&
                      rel(e2[1], e0[1]) < -60.0,
                  "space: erosion takes the orders away from the top (1.5 of 3 left)");

            // the direction grid: four directions - the pipe is heard from the nearest one
            {
                OrganEngine g;
                place(g, -50.0f, 10.0f);
                auto b = run([](EngineParams &p) { p.space.grid = 1.0f; }, 1.0, -1, "", 16, &g);
                double az, rE;
                direction(b, from, az, rE);
                float gaz[4], gel[4];
                fibonacciDirs(4, gaz, gel);
                double best = 1e9;
                for (int i = 0; i < 4; ++i)
                    best = juce::jmin(best, std::abs(std::remainder(az - (double)gaz[i], 360.0)));
                double azDry, rEDry;
                direction(dry, from, azDry, rEDry);
                std::cout << "space grid (4 directions): heard from az " << az << " (pipe at "
                          << azDry << "), " << best << " deg from a grid direction" << std::endl;
                check(gridCount(1.0f) == 4 && gridCount(0.0f) == 0 && best < 3.0,
                      "space: directions snap to the grid");
            }
            // bit depth / sample-and-hold of the directional channels: W untouched, the higher
            // orders hit hardest
            {
                OrganEngine g;
                place(g, -50.0f, 10.0f);
                auto b = run(
                    [](EngineParams &p)
                    {
                        p.space.bits = 4.0f;
                        p.space.rateHz = 2000.0f;
                    },
                    1.0, -1, "51_space_crush", 16, &g);
                std::array<double, 4> err{}, ref{};
                for (int c = 0; c < 16; ++c)
                {
                    const int l = (int)std::floor(std::sqrt((double)c) + 1e-9);
                    for (int i = from; i < b.getNumSamples(); ++i)
                    {
                        const double d = b.getSample(c, i) - dry.getSample(c, i);
                        err[(size_t)l] += d * d;
                        ref[(size_t)l] += (double)dry.getSample(c, i) * dry.getSample(c, i);
                    }
                }
                std::cout << "space crush: error per degree " << rel(err[0] + 1e-30, ref[0])
                          << " / " << rel(err[1], ref[1]) << " / " << rel(err[2], ref[2]) << " / "
                          << rel(err[3], ref[3]) << " dB" << std::endl;
                check(err[0] < 1e-20 && rel(err[1], ref[1]) < rel(err[3], ref[3]) &&
                          rel(err[3], ref[3]) > -20.0,
                      "space: crushing grows with the order, W stays clean");
            }
            // dropouts: directional channels fall silent now and then, W never
            {
                OrganEngine g;
                place(g, -50.0f, 10.0f);
                auto b = run([](EngineParams &p) { p.space.drops = 1.0f; }, 6.0, -1,
                             "52_space_dropouts", 16, &g);
                int silentFrames = 0, wSilent = 0, frames = 0;
                for (int pos = from; pos + 480 < b.getNumSamples(); pos += 480)
                {
                    ++frames;
                    for (int c = 1; c < 16; ++c)
                        if (b.getRMSLevel(c, pos, 480) <
                            1e-3f *
                                juce::jmax(1e-9f,
                                           dry.getRMSLevel(
                                               c, juce::jmin(pos, dry.getNumSamples() - 480), 480)))
                        {
                            ++silentFrames;
                            break;
                        }
                    if (b.getRMSLevel(0, pos, 480) < 1e-6f)
                        ++wSilent;
                }
                std::cout << "space dropouts: " << silentFrames << " of " << frames
                          << " frames with a silent channel, W silent " << wSilent << std::endl;
                check(silentFrames > 5 && wSilent == 0,
                      "space: dropouts silence directional channels, never W");
            }
            // aliasing: the field sampled at too few points - the level stays, the direction smears
            {
                OrganEngine g;
                place(g, -50.0f, 10.0f);
                auto b = run([](EngineParams &p) { p.space.alias = 1.0f; }, 1.5, -1,
                             "53_space_alias", 16, &g);
                degreeEnergy(b, from, e1);
                double d = 0;
                for (int c = 0; c < 16; ++c)
                    for (int i = from; i < b.getNumSamples(); ++i)
                    {
                        const double x = b.getSample(c, i) - dry.getSample(c, i);
                        d += x * x;
                    }
                const double tot0 = e0[0] + e0[1] + e0[2] + e0[3];
                std::cout << "space alias: W " << rel(e1[0], e0[0]) << " dB, change "
                          << rel(d, tot0) << " dB, degree 3 " << rel(e1[3], e0[3]) << " dB"
                          << std::endl;
                bool fin = true;
                for (int c = 0; c < 16; ++c)
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        fin = fin && std::isfinite(b.getSample(c, i));
                check(fin && std::abs(rel(e1[0], e0[0])) < 4.0 && rel(d, tot0) > -12.0,
                      "space: aliasing changes the field, not the level");
            }
            // weathering: the sphere wears away while it plays (2 s), and grows back in silence
            {
                OrganEngine g;
                place(g, -50.0f, 10.0f);
                auto b = run([](EngineParams &p) { p.space.weather = 2.0f; }, 3.0, -1,
                             "54_space_weather", 16, &g);
                const float worn = g.spaceErosionNow.load();
                std::array<double, 4> early{}, late{};
                juce::AudioBuffer<float> eb(16, (int)(0.3 * fsr)), lb(16, (int)(0.3 * fsr));
                for (int c = 0; c < 16; ++c)
                {
                    eb.copyFrom(c, 0, b, c, (int)(0.2 * fsr), eb.getNumSamples());
                    lb.copyFrom(c, 0, b, c, (int)(2.6 * fsr), lb.getNumSamples());
                }
                degreeEnergy(eb, 0, early);
                degreeEnergy(lb, 0, late);
                std::cout << "space weather 2 s: erosion after 3 s " << worn
                          << ", degree 1 share early " << early[1] / early[0] << ", late "
                          << late[1] / juce::jmax(1e-30, late[0]) << std::endl;
                check(worn > 0.99f &&
                          late[1] / juce::jmax(1e-30, late[0]) < 1e-4 * early[1] / early[0],
                      "space: weathering erodes the sphere over time");
            }
            // default: nothing changes (the shoebox reference files stay bit-identical, see the
            // null test)

            // grain cloud: the pipe dissolves into grains around it
            {
                OrganEngine c1, c2, c3;
                for (auto *e : {&c1, &c2, &c3})
                    place(*e, -50.0f, 0.0f);
                auto wide = run(
                    [](EngineParams &p)
                    {
                        auto &c = p.cloud[0];
                        c.mix = 1.0f;
                        c.spread = 180.0f;
                        c.density = 60.0f;
                        c.sizeMs = 80.0f;
                    },
                    3.0, -1, "55_cloud_sphere", 16, &c1);
                auto narrow = run(
                    [](EngineParams &p)
                    {
                        auto &c = p.cloud[0];
                        c.mix = 1.0f;
                        c.spread = 15.0f;
                        c.density = 60.0f;
                        c.sizeMs = 80.0f;
                    },
                    3.0, -1, "", 16, &c2);
                auto halfMix = run(
                    [](EngineParams &p)
                    {
                        auto &c = p.cloud[0];
                        c.mix = 0.5f;
                        c.spread = 90.0f;
                    },
                    3.0, -1, "55_cloud_half", 16, &c3);
                double azD, rD, azW, rW, azN, rN, azH, rH;
                direction(dry, from, azD, rD);
                direction(wide, from, azW, rW);
                direction(narrow, from, azN, rN);
                direction(halfMix, from, azH, rH);
                std::array<double, 4> ew{}, eh{};
                degreeEnergy(wide, from, ew);
                degreeEnergy(halfMix, from, eh);
                const double dryLen = (double)dry.getNumSamples() - from,
                             wideLen = (double)wide.getNumSamples() - from;
                const double lvlW = rel(ew[0] / wideLen, e0[0] / dryLen),
                             lvlH = rel(eh[0] / wideLen, e0[0] / dryLen);
                std::cout << "cloud: dry az " << azD << " rE " << rD << "; whole sphere rE " << rW
                          << ", level " << lvlW << " dB; 15 deg: az " << azN << " rE " << rN
                          << "; half mix level " << lvlH << " dB, rE " << rH << "; grains "
                          << c1.cloudGrainsNow.load() << std::endl;
                bool fin = true;
                for (auto *b : {&wide, &narrow, &halfMix})
                    for (int c = 0; c < 16; ++c)
                        for (int i = 0; i < b->getNumSamples(); ++i)
                            fin = fin && std::isfinite(b->getSample(c, i));
                check(fin && rW < 0.35 * rD && std::abs(lvlW) < 4.0,
                      "cloud: on the whole sphere the pipe has no direction any more, at about its "
                      "level");
                check(std::abs(std::remainder(azN - azD, 360.0)) < 10.0 && rN > 0.7 * rD,
                      "cloud: a narrow cloud stays at the pipe");
                check(std::abs(lvlH) < 3.0 && rH < rD && rH > rW, "cloud: half dry, half grains");
            }
            { // stereo, and many pipes: finite, cheap enough
                OrganEngine e;
                e.prepare(fsr, B);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[0] = p.stopOn[1] = true;
                p.outputMode = 3;
                for (auto &c : p.cloud)
                {
                    c.mix = 0.7f;
                    c.density = 80.0f;
                    c.sizeMs = 120.0f;
                    c.spread = 120.0f;
                }
                p.space.erode = 0.3f;
                p.space.bits = 8.0f;
                p.space.drops = 0.3f;
                p.space.alias = 0.4f;
                p.space.grid = 0.3f;
                juce::AudioBuffer<float> b(16, B);
                const int blocks = (int)(6.0 * fsr / B);
                bool fin = true;
                auto t0 = std::chrono::high_resolution_clock::now();
                for (int i = 0; i < blocks; ++i)
                {
                    juce::MidiBuffer mb;
                    if (i == 0)
                        for (int m = 48; m < 72; ++m)
                            mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)90), 0);
                    if (i == blocks / 2)
                        for (int m = 48; m < 72; ++m)
                            mb.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                    e.process(b, mb, p);
                    for (int c = 0; c < 16; ++c)
                        fin = fin && std::isfinite(b.getMagnitude(c, 0, B));
                }
                const double rt = 6.0 / std::chrono::duration<double>(
                                            std::chrono::high_resolution_clock::now() - t0)
                                            .count();
                std::cout << "cloud + Raumverfall, 48 pipes, 3rd order: " << rt
                          << " x real time, grains at the end " << e.cloudGrainsNow.load()
                          << std::endl;
                check(fin && rt > 3.0,
                      "cloud + Raumverfall: finite and cheap enough with 48 pipes");
                OrganEngine st;
                st.prepare(fsr, B);
                st.setLibrary(lib);
                EngineParams q = p;
                q.outputMode = 0;
                juce::AudioBuffer<float> b2(2, B);
                bool fin2 = true;
                double en = 0;
                for (int i = 0; i < 200; ++i)
                {
                    juce::MidiBuffer mb;
                    if (i == 0)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)90), 0);
                    st.process(b2, mb, q);
                    fin2 = fin2 && std::isfinite(b2.getMagnitude(0, 0, B));
                    en += b2.getRMSLevel(0, 0, B);
                }
                check(fin2 && en > 0.0, "cloud in stereo works");
            }
        }

        { // long sessions: the reflections must sound the same after the plugin has run for a long
          // time
            // (the read position of the delay lines once lost its fraction after ~6 minutes ->
            // aliasing, high ringing)
            auto setup = [](EngineParams &p)
            {
                p.room.on = true;
                p.room.tailGain = 0.0f;
                p.room.order = 2;
            };
            OrganEngine fresh, aged;
            {
                aged.prepare(fsr, 4096);
                aged.setLibrary(lib);
                EngineParams q;
                q.stopOn = {};
                q.clickLevel = 0;
                q.releaseClick = 0;
                q.wind = 0;
                q.stopPull = 0;
                q.outputMode = 1;
                juce::AudioBuffer<float> b(4, 4096);
                juce::MidiBuffer none;
                const int blocks = (int)(40.0 * 60.0 * fsr / 4096.0); // 40 minutes of silence
                for (int i = 0; i < blocks; ++i)
                    aged.process(b, none, q);
            }
            auto a = run(setup, 2.0, -1, "", 4, &fresh);
            auto b = run(setup, 2.0, -1, "41b_room_after_40_minutes", 4, &aged);
            double e = 0, d = 0;
            for (int i = (int)(0.5 * fsr); i < a.getNumSamples(); ++i)
            {
                const double x = a.getSample(0, i), y = b.getSample(0, i);
                e += x * x;
                d += (x - y) * (x - y);
            }
            const double db = 10.0 * std::log10(d / juce::jmax(1e-30, e) + 1e-30);
            std::cout
                << "room: reflections after 40 minutes of running differ from a fresh start by "
                << db << " dB" << std::endl;
            check(db < -80.0, "room: early reflections unchanged after a long session");
        }

        { // one strong reflection from the left wall: delay and direction
            auto setup = [&](bool on)
            {
                return [on](EngineParams &p)
                {
                    p.stopGain[0] = 0.0f; // the pipe silent: only its key click at the pipe
                    p.fx.custLevel = 1.0f;
                    p.fx.custPlace = 0;
                    p.room.on = on;
                    p.room.width = 20;
                    p.room.length = 20;
                    p.room.height = 20;
                    p.room.listenerX = 0.1f;
                    p.room.listenerY = 0.5f;
                    p.room.absWalls = 0.0f;
                    p.room.absFloor = 0.99f;
                    p.room.scatter = 0.0f;
                    p.room.order = 1;
                    p.room.tailGain = 0.0f;
                };
            };
            OrganEngine probe;
            probe.prepare(fsr, B);
            // put the pipe straight ahead, 5 m
            OrganEngine eOn, eOff;
            for (auto *e : {&eOn, &eOff})
            {
                e->baseAz[0][(size_t)keyA] = 0.0f;
                e->baseEl[0][(size_t)keyA] = 0.0f;
                e->baseDist[0][(size_t)keyA] = 5.0f;
            }
            auto on = run(setup(true), 0.5, -1, "40_room_left_wall", 4, &eOn);
            auto off = run(setup(false), 0.5, -1, "", 4, &eOff);
            juce::AudioBuffer<float> diff(on);
            for (int c = 0; c < 4; ++c)
                diff.addFrom(c, 0, off, c, 0, off.getNumSamples(), -1.0f);
            // lag of the reflection: cross-correlation of the reflections with the dry sound (W)
            const float *a = off.getReadPointer(0);
            const float *d = diff.getReadPointer(0);
            int best = 0;
            double bv = -1e30;
            for (int lag = 0; lag < 960; ++lag)
            {
                double sum = 0;
                for (int i = 0; i < 20000; ++i)
                    sum += (double)a[i] * d[i + lag];
                if (sum > bv)
                {
                    bv = sum;
                    best = lag;
                }
            }
            const double expectMs = (std::sqrt(4.0 * 4.0 + 5.0 * 5.0) - 5.0) / 343.0 * 1000.0;
            // direction: only the first 20 ms after the reflection arrives (the front wall comes
            // after 29 ms)
            int onset = 0;
            {
                float pk = off.getMagnitude(0, 0, off.getNumSamples());
                while (onset < off.getNumSamples() && std::abs(a[onset]) < 0.05f * pk)
                    ++onset;
            }
            double wy = 0, wx = 0;
            for (int i = onset + best - 24; i < onset + best + 960; ++i)
            {
                wy += (double)diff.getSample(0, i) * diff.getSample(1, i);
                wx += (double)diff.getSample(0, i) * diff.getSample(3, i);
            }
            const double az = juce::radiansToDegrees(std::atan2(wy, wx));
            const double expectAz = juce::radiansToDegrees(std::atan2(4.0, 5.0));
            std::cout << "room: left-wall reflection after " << best / 48.0 << " ms (expect "
                      << expectMs << "), from az " << az << " (expect " << expectAz << "), level "
                      << juce::Decibels::gainToDecibels(
                             (float)std::sqrt(diff.getRMSLevel(0, 0, diff.getNumSamples())))
                      << std::endl;
            check(std::abs(best / 48.0 - expectMs) < 0.15 && std::abs(az - expectAz) < 8.0,
                  "early reflection: right delay and direction");
        }
        { // late reverb decays with the computed decay time - both models (convolution, FDN), at
          // the same level
            double energy[2]{};
            for (int model = 0; model < 2; ++model)
            {
                OrganEngine e;
                auto b = run(
                    [model](EngineParams &p)
                    {
                        p.room.on = true;
                        p.room.width = 12;
                        p.room.length = 24;
                        p.room.height = 10;
                        p.room.absWalls = 0.08f;
                        p.room.absFloor = 0.15f;
                        p.room.highDamp = 0.3f;
                        p.room.erGain = 0.0f;
                        p.room.tailModel = model;
                    },
                    5.0, 0.6, model == 0 ? "41_room_tail" : "41_room_tail_fdn", 4, &e);
                const float rt = e.roomRtLow.load();
                // decay of the W energy after the note (10 ms frames), fit -5 .. -25 dB
                const int start = (int)(0.75 * fsr), fr = 480;
                std::vector<double> en;
                for (int pos = start; pos + fr < b.getNumSamples(); pos += fr)
                {
                    double s2 = 0;
                    for (int i = 0; i < fr; ++i)
                        s2 += (double)b.getSample(0, pos + i) * b.getSample(0, pos + i);
                    en.push_back(s2);
                }
                std::vector<double> sch(en.size());
                double acc = 0;
                for (int i = (int)en.size() - 1; i >= 0; --i)
                {
                    acc += en[(size_t)i];
                    sch[(size_t)i] = acc;
                }
                energy[model] = acc;
                double sx = 0, sy = 0, sxx = 0, sxy = 0;
                int nn = 0;
                for (size_t i = 0; i < sch.size(); ++i)
                {
                    const double db = 10 * std::log10(sch[i] / acc);
                    if (db > -5)
                        continue;
                    if (db < -25)
                        break;
                    const double x = static_cast<double>(i) * 0.01;
                    sx += x;
                    sy += db;
                    sxx += x * x;
                    sxy += x * db;
                    ++nn;
                }
                const double slope = (nn * sxy - sx * sy) / (nn * sxx - sx * sx);
                const double measured = -60.0 / slope;
                std::cout << "room: tail (" << (model == 0 ? "convolution" : "FDN")
                          << ") decay measured " << measured << " s, ray-traced " << rt
                          << " s, critical distance " << e.roomCritDist.load() << " m" << std::endl;
                check(nn > 3 && std::abs(measured / rt - 1.0) < 0.25,
                      juce::String("late reverb decays with the room's decay time (") +
                          (model == 0 ? "convolution)" : "FDN)"));
            }
            const double diffDb = 10.0 * std::log10(energy[1] / energy[0]);
            std::cout << "room: FDN tail level against the convolution tail: " << diffDb << " dB"
                      << std::endl;
            check(std::abs(diffDb) < 2.0, "room: both late reverb models equally loud");
        }
        { // FDN: unchanged after a long session as well; switching models while playing rings out
          // without a gap
            auto setup = [](EngineParams &p)
            {
                p.room.on = true;
                p.room.erGain = 0.0f;
                p.room.tailModel = 1;
            };
            OrganEngine fresh, aged;
            {
                aged.prepare(fsr, 4096);
                aged.setLibrary(lib);
                EngineParams q;
                q.stopOn = {};
                q.clickLevel = 0;
                q.releaseClick = 0;
                q.wind = 0;
                q.stopPull = 0;
                q.outputMode = 1;
                q.room.on = true;
                q.room.tailModel = 1; // the FDN runs (silently) all the time
                juce::AudioBuffer<float> b(4, 4096);
                juce::MidiBuffer none;
                const int blocks = (int)(12.0 * 60.0 * fsr / 4096.0);
                for (int i = 0; i < blocks; ++i)
                    aged.process(b, none, q);
            }
            auto a = run(setup, 2.0, -1, "", 4, &fresh);
            auto b = run(setup, 2.0, -1, "", 4, &aged);
            double e2 = 0, d2 = 0;
            for (int i = (int)(0.5 * fsr); i < a.getNumSamples(); ++i)
            {
                const double x = a.getSample(0, i), y = b.getSample(0, i);
                e2 += x * x;
                d2 += (x - y) * (x - y);
            }
            const double db = 10.0 * std::log10(d2 / juce::jmax(1e-30, e2) + 1e-30);
            std::cout << "room: FDN after 12 minutes of running differs from a fresh start by "
                      << db << " dB" << std::endl;
            check(db < -60.0, "room: FDN unchanged after a long session");

            auto sw = run(
                [](EngineParams &p)
                {
                    p.room.on = true;
                    p.room.erGain = 0.0f;
                },
                4.0, 0.5, "41_room_tail_switch", 4);
            (void)sw;
            // switch while the tail rings (note off at 0.5 s, switch at 1.0 s), both ways: no gap,
            // and it keeps decaying (the old one rings out underneath with a silent input - it must
            // not build up)
            for (int from = 0; from < 2; ++from)
            {
                OrganEngine e;
                e.prepare(fsr, B);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[0] = true;
                p.clickLevel = 0;
                p.releaseClick = 0;
                p.wind = 0;
                p.stopPull = 0;
                p.outputMode = 1;
                p.room.on = true;
                p.room.erGain = 0.0f;
                e.setTailIR(TailIR::build(p.room, p.air, p.outputMode, fsr, 1));
                juce::AudioBuffer<float> blk(4, B), all(4, (int)(fsr * 6.0));
                for (int pos = 0; pos < all.getNumSamples(); pos += B)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)110), 0);
                    if (pos == ((int)(0.5 * fsr) / B) * B)
                        mb.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
                    p.room.tailModel = pos >= (int)(1.0 * fsr) ? 1 - from : from;
                    e.process(blk, mb, p);
                    for (int c = 0; c < 4; ++c)
                        all.copyFrom(c, pos, blk, c, 0, juce::jmin(B, all.getNumSamples() - pos));
                }
                writeWav(outDir.getChildFile(from == 0 ? "41_room_tail_switch_to_fdn.wav"
                                                       : "41_room_tail_switch_to_conv.wav"),
                         all, fsr);
                auto rmsAt = [&](double t)
                { return all.getRMSLevel(0, (int)(t * fsr), (int)(0.1 * fsr)); };
                const double jump =
                    juce::Decibels::gainToDecibels(rmsAt(1.0) / juce::jmax(1e-9f, rmsAt(0.9)));
                const double later =
                    juce::Decibels::gainToDecibels(rmsAt(3.0) / juce::jmax(1e-9f, rmsAt(1.0)));
                const double end =
                    juce::Decibels::gainToDecibels(rmsAt(5.8) / juce::jmax(1e-9f, rmsAt(1.0)));
                std::cout << "room: switching "
                          << (from == 0 ? "convolution -> FDN" : "FDN -> convolution")
                          << " while the tail rings: level change " << jump << " dB, 2 s later "
                          << later << " dB, at 5.8 s " << end << " dB" << std::endl;
                check(std::isfinite(jump) && jump > -3.0 && jump < 6.0,
                      "room: switching the late reverb model while playing has no gap");
                check(later < -10.0 && end < later,
                      "room: after switching the late reverb keeps decaying (no build-up)");
            }
        }
        { // listening scenes: default room, note on / off, early reflections only / tail only /
          // both (stereo)
            for (int mode = 0; mode < 4; ++mode)
                run(
                    [mode](EngineParams &p)
                    {
                        p.outputMode = 0;
                        p.clickLevel = 0.2f;
                        p.releaseClick = 0.2f;
                        p.room.on = mode < 3;
                        p.room.width = 14;
                        p.room.length = 28;
                        p.room.height = 11;
                        p.room.absWalls = 0.06f;
                        p.room.absFloor = 0.2f;
                        p.room.highDamp = 0.15f;
                        p.room.scatter = 0.3f;
                        p.room.order = 2;
                        if (mode == 0)
                            p.room.tailGain = 0.0f;
                        if (mode == 1)
                            p.room.erGain = 0.0f;
                    },
                    4.0, 1.0,
                    mode == 0 ? "42_room_er_only"
                              : (mode == 1 ? "42_room_tail_only"
                                           : (mode == 2 ? "42_room_both" : "42_room_dry")),
                    2);
        }
        { // impulse responses of the default room (a key click at the pipe), tail only /
          // reflections only
            for (int mode = 0; mode < 3; ++mode)
                run(
                    [mode](EngineParams &p)
                    {
                        p.outputMode = 0;
                        p.stopGain[0] = 0.0f;
                        p.fx.custLevel = 1.0f;
                        p.fx.custPlace = 0;
                        p.room.on = mode < 2;
                        p.room.width = 14;
                        p.room.length = 28;
                        p.room.height = 11;
                        p.room.absWalls = 0.06f;
                        p.room.absFloor = 0.2f;
                        p.room.highDamp = 0.15f;
                        p.room.scatter = 0.3f;
                        p.room.order = 2;
                        if (mode == 0)
                            p.room.erGain = 0.0f;
                        else
                            p.room.tailGain = 0.0f;
                    },
                    4.0, -1,
                    mode == 0 ? "43_room_click_tail"
                              : (mode == 1 ? "43_room_click_er" : "43_room_click_dry"),
                    2);
        }
        { // the tail's response to a pure impulse (an "own sound" that is one sample): dense, no
          // ringing modes
            auto lib2 = std::make_shared<SampleLibrary>(*lib);
            auto imp = std::make_shared<Sample>();
            imp->sampleRate = 48000.0;
            imp->data.assign(64, 0.0f);
            imp->data[2] = 1.0f;
            NoiseSample ns;
            ns.sample = imp;
            ns.norm = 1.0f;
            lib2->custom = {ns};
            std::shared_ptr<const SampleLibrary> lib2c(lib2);
            OrganEngine e;
            e.prepare(fsr, B);
            e.setLibrary(lib2c);
            EngineParams p;
            p.stopOn = {};
            p.stopOn[0] = true;
            p.stopGain[0] = 0.0f;
            p.clickLevel = 0;
            p.releaseClick = 0;
            p.wind = 0;
            p.stopPull = 0;
            p.outputMode = 0;
            p.fx.custLevel = 1.0f;
            p.fx.custPlace = 0;
            p.room.on = true;
            p.room.width = 14;
            p.room.length = 28;
            p.room.height = 11;
            p.room.absWalls = 0.06f;
            p.room.absFloor = 0.2f;
            p.room.highDamp = 0.15f;
            p.room.scatter = 0.3f;
            p.room.order = 2;
            p.room.erGain = 0.0f;
            e.setTailIR(TailIR::build(p.room, p.air, 0, fsr, 1));
            juce::AudioBuffer<float> b(2, B), all(2, (int)(fsr * 2.0));
            for (int pos = 0; pos < all.getNumSamples(); pos += B)
            {
                juce::MidiBuffer mb;
                if (pos == 0)
                    mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)127), 0);
                e.process(b, mb, p);
                for (int c = 0; c < 2; ++c)
                    all.copyFrom(c, pos, b, c, 0, juce::jmin(B, all.getNumSamples() - pos));
            }
            writeWav(outDir.getChildFile("44_room_impulse_tail.wav"), all, fsr);
            // ringing = narrow spectral peaks far above their neighbourhood. Dense decaying noise:
            // the top 0.1 % of the bins lie ~10-12 dB above the local median; the earlier Hadamard
            // network without modulation: ~22 dB
            const int a0 = (int)(0.3 * fsr), len = 65536;
            juce::dsp::FFT fft(16);
            std::vector<float> w((size_t)(2 * len), 0.0f);
            for (int i = 0; i < len && a0 + i < all.getNumSamples(); ++i)
                w[(size_t)i] = 0.5f * (all.getSample(0, a0 + i) + all.getSample(1, a0 + i)) *
                               (0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi *
                                                       (float)i / (float)len));
            fft.performFrequencyOnlyForwardTransform(w.data(), true);
            std::vector<double> db((size_t)(len / 2));
            for (int k = 0; k < len / 2; ++k)
                db[(size_t)k] = 20.0 * std::log10(w[(size_t)k] + 1e-12);
            std::vector<double> ex;
            const int k0 = (int)(500.0 * len / fsr), k1 = (int)(8000.0 * len / fsr);
            for (int k = k0; k < k1; ++k)
            {
                std::vector<double> nb(db.begin() + (k - 200), db.begin() + (k + 201));
                std::nth_element(nb.begin(), nb.begin() + 200, nb.end());
                ex.push_back(db[(size_t)k] - nb[200]);
            }
            std::sort(ex.begin(), ex.end());
            const double p999 = ex[(size_t)(0.999 * (double)(ex.size() - 1))];
            std::cout << "room: tail impulse response, peaks over local median (99.9 %): " << p999
                      << " dB" << std::endl;
            check(p999 < 14.0, "late reverb is dense (no ringing modes)");
        }
        { // no dispersion in the Saal: a one-sample click through the direct sound and the early
          // reflections - every band arrives at the same time (envelopes of octave bands,
          // cross-correlated). "Eis" (dispersive walls on purpose) shows that the measure sees it.
            auto lib2 = std::make_shared<SampleLibrary>(*lib);
            auto imp = std::make_shared<Sample>();
            imp->sampleRate = fsr;
            imp->data.assign(64, 0.0f);
            imp->data[2] = 1.0f;
            NoiseSample ns;
            ns.sample = imp;
            ns.norm = 1.0f;
            lib2->custom = {ns};
            std::shared_ptr<const SampleLibrary> lib2c(lib2);
            auto renderClick = [&](bool roomOn, float ice)
            {
                OrganEngine e;
                e.prepare(fsr, B);
                e.setLibrary(lib2c);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[0] = true;
                p.stopGain[0] = 0.0f;
                p.clickLevel = 0;
                p.releaseClick = 0;
                p.wind = 0;
                p.stopPull = 0;
                p.outputMode = 0;
                p.fx.custLevel = 1.0f;
                p.fx.custPlace = 0;
                p.room.on = roomOn;
                p.room.width = 14;
                p.room.length = 28;
                p.room.height = 11;
                p.room.absWalls = 0.06f;
                p.room.absFloor = 0.2f;
                p.room.highDamp = 0.15f;
                p.room.scatter = 0.3f;
                p.room.order = 2;
                p.room.tailGain = 0.0f;
                p.ice = ice;
                p.iceLen = 0.7f;
                const int total = (int)(fsr * 0.4);
                juce::AudioBuffer<float> b(2, B);
                std::vector<float> mid((size_t)total, 0.0f);
                for (int pos = 0; pos < total; pos += B)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)127), 0);
                    e.process(b, mb, p);
                    for (int i = 0; i < B && pos + i < total; ++i)
                        mid[(size_t)(pos + i)] = 0.5f * (b.getSample(0, i) + b.getSample(1, i));
                }
                return mid;
            };
            // the envelope of one octave band (zero-phase band filter in the spectrum, analytic
            // signal): a band delayed against another shows as a shift of its envelope
            auto bandEnv = [&](const std::vector<float> &x, double f0)
            {
                const int ord = 15, N = 1 << ord;
                juce::dsp::FFT f(ord);
                std::vector<juce::dsp::Complex<float>> a((size_t)N), s((size_t)N);
                for (int i = 0; i < N && i < (int)x.size(); ++i)
                    a[(size_t)i] = x[(size_t)i];
                f.perform(a.data(), s.data(), false);
                for (int k = 0; k < N; ++k)
                {
                    double w = 0.0;
                    if (k > 0 && k < N / 2)
                    {
                        // a raised cosine over the octave f0 .. 2 f0 (on a log axis), and the
                        // negative frequencies left out
                        const double fr = (double)k * fsr / (double)N;
                        const double u = std::log2(fr / f0);
                        if (u > -0.5 && u < 1.5)
                            w = 2.0 * (0.5 - 0.5 * std::cos(juce::MathConstants<double>::pi * (u + 0.5)));
                    }
                    s[(size_t)k] *= (float)w;
                }
                f.perform(s.data(), a.data(), true);
                std::vector<float> env(x.size());
                for (size_t i = 0; i < x.size(); ++i)
                    env[i] = std::abs(a[i]);
                return env;
            };
            // how much later band b arrives than band a (ms): the lag of the best match of the
            // two envelopes within +-15 ms
            auto lagMs = [&](const std::vector<float> &ea, const std::vector<float> &eb)
            {
                const int maxLag = (int)(0.015 * fsr), n = (int)ea.size();
                double best = -1.0;
                int bestLag = 0;
                for (int L = -maxLag; L <= maxLag; ++L)
                {
                    double c = 0.0;
                    for (int i = juce::jmax(0, -L); i < n && i + L < n; ++i)
                        c += (double)ea[(size_t)i] * eb[(size_t)(i + L)];
                    if (c > best)
                    {
                        best = c;
                        bestLag = L;
                    }
                }
                return 1000.0 * (double)bestLag / fsr;
            };
            auto lags = [&](const std::vector<float> &x, double &lowMs, double &midMs)
            {
                const auto lo = bandEnv(x, 250.0), md = bandEnv(x, 1000.0), hi = bandEnv(x, 4000.0);
                lowMs = lagMs(hi, lo);
                midMs = lagMs(hi, md);
            };
            double dryLo, dryMd, roomLo, roomMd, iceLo, iceMd;
            lags(renderClick(false, 0.0f), dryLo, dryMd);
            lags(renderClick(true, 0.0f), roomLo, roomMd);
            lags(renderClick(true, 1.0f), iceLo, iceMd);
            std::cout << "saal dispersion: 250 Hz / 1 kHz arrive after 4 kHz by (ms): dry " << dryLo
                      << " / " << dryMd << ", direct + reflections " << roomLo << " / " << roomMd
                      << ", with Eis " << iceLo << " / " << iceMd << std::endl;
            check(std::abs(roomLo) < 0.5 && std::abs(roomMd) < 0.5 && std::abs(dryLo) < 0.5,
                  "saal: no dispersion - low, mid and high bands of a click arrive together");
            // (at 250 Hz the direct sound, which Eis leaves alone, dominates the envelope)
            check(juce::jmax(iceLo, iceMd) > 1.0,
                  "saal: the dispersion measure sees Eis (lower bands held back)");
        }
        { // the room changes while a note sounds: the new response is crossfaded from the same
          // history - no gap
            auto render = [&](bool swap)
            {
                OrganEngine e;
                e.prepare(fsr, B);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[0] = true;
                p.clickLevel = 0;
                p.releaseClick = 0;
                p.wind = 0;
                p.stopPull = 0;
                p.outputMode = 0;
                p.room.on = true;
                p.room.erGain = 0.0f;
                e.setTailIR(TailIR::build(p.room, p.air, 0, fsr, 1));
                auto changed = p.room;
                changed.absWalls *= 1.02f; // a small change: the level must stay
                auto nextIr = TailIR::build(changed, p.air, 0, fsr, 2);
                juce::AudioBuffer<float> b(2, B), all(2, (int)(fsr * 2.5));
                for (int pos = 0; pos < all.getNumSamples(); pos += B)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 57, (juce::uint8)100), 0);
                    if (swap && pos == B * 94)
                        e.setTailIR(nextIr); // ~1 s
                    e.process(b, mb, p);
                    for (int c = 0; c < 2; ++c)
                        all.copyFrom(c, pos, b, c, 0, juce::jmin(B, all.getNumSamples() - pos));
                }
                return std::make_pair(all, e.tailSerialInUse.load());
            };
            auto [a1, s1] = render(false);
            auto [a2, s2] = render(true);
            writeWav(outDir.getChildFile("45_room_tail_swap.wav"), a2, fsr);
            double worst = 0;
            for (double t = 0.8; t < 2.3; t += 0.02)
            {
                const double l1 = juce::Decibels::gainToDecibels(
                    a1.getRMSLevel(0, (int)(t * fsr), (int)(0.02 * fsr)));
                const double l2 = juce::Decibels::gainToDecibels(
                    a2.getRMSLevel(0, (int)(t * fsr), (int)(0.02 * fsr)));
                worst = juce::jmax(worst, std::abs(l1 - l2));
            }
            std::cout
                << "room: response swapped while playing: largest level difference to no swap "
                << worst << " dB (in use: " << s2 << ")" << std::endl;
            check(worst < 1.0 && s2 == 2 && s1 == 1,
                  "room change: the reverb turns into the new room without a gap");
        }
        { // CPU, worst case for floor plans: 48 moving pipes with their own reflections, 3rd order,
          // shoebox vs polygons
            double rts[3]{};
            const int shapesB[]{shapeBox, shapeCross, shapeRound};
            for (int si = 0; si < 3; ++si)
            {
                OrganEngine e;
                e.prepare(fsr, B);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[0] = p.stopOn[1] = true;
                p.motionMode.fill((int)Motion::Rotation);
                p.motionRate.fill(0.3f);
                p.outputMode = 3;
                p.room.on = true;
                p.room.order = 3;
                p.room.shape = shapesB[si];
                p.room.shapeAmt = 0.5f;
                e.setTailIR(TailIR::build(p.room, p.air, 3, fsr, 1));
                juce::AudioBuffer<float> b(16, B);
                const int blocks = (int)(6.0 * fsr / B);
                auto t0 = std::chrono::high_resolution_clock::now();
                for (int i = 0; i < blocks; ++i)
                {
                    juce::MidiBuffer mb;
                    if (i == 0)
                        for (int m = 48; m < 72; ++m)
                            mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)90), 0);
                    e.process(b, mb, p);
                }
                rts[si] = 6.0 / std::chrono::duration<double>(
                                    std::chrono::high_resolution_clock::now() - t0)
                                    .count();
                std::cout << "plan CPU: "
                          << (si == 0   ? "shoebox"
                              : si == 1 ? "cross"
                                        : "round")
                          << ", 48 rotating pipes (" << e.erActive.load()
                          << " with own reflections), 3rd order: " << rts[si] << " x real time"
                          << std::endl;
            }
            check(rts[1] > 0.6 * rts[0] && rts[2] > 0.6 * rts[0],
                  "plan: polygon rooms cost about as much as the shoebox");
        }
        { // CPU: a chord on 3 stops (18 pipes) and on all 8 stops (48 pipes), reflections up to 2nd
          // / 3rd order, 3rd-order output
            for (int stops : {3, 8})
                for (int ord : {2, 3})
                {
                    OrganEngine e;
                    e.prepare(fsr, B);
                    e.setLibrary(lib);
                    EngineParams p;
                    p.stopOn = {};
                    for (int st = 0; st < stops; ++st)
                        p.stopOn[(size_t)st] = true;
                    p.outputMode = 3;
                    p.room.on = true;
                    p.room.order = ord;
                    e.setTailIR(TailIR::build(p.room, p.air, 3, fsr, 1));
                    juce::AudioBuffer<float> b(16, B);
                    const int blocks = (int)(6.0 * fsr / B);
                    auto t0 = std::chrono::high_resolution_clock::now();
                    for (int i = 0; i < blocks; ++i)
                    {
                        juce::MidiBuffer mb;
                        if (i == 0)
                            for (int m : {48, 55, 60, 64, 67, 72})
                                mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)90), 0);
                        e.process(b, mb, p);
                    }
                    const double rt = 6.0 / std::chrono::duration<double>(
                                                std::chrono::high_resolution_clock::now() - t0)
                                                .count();
                    std::cout << "room on, reflections up to order " << ord << ", " << stops * 6
                              << " pipes (" << e.erActive.load()
                              << " with own reflections), 3rd-order out: " << rt << " x real time"
                              << std::endl;
                    if (stops == 3 && ord == 2)
                        check(rt > 2.0, "room simulation runs in real time (18 pipes)");
                }
        }

        // ---------------- the living organ: bellows, pallets, the walking listener, wear,
        // sympathetic pipes, pure tuning, the Tonnetz walker ----------------
        {
            // notes held on the Praestant (or what 'setup' pulls); 'each' is called after every
            // block
            auto play = [&](OrganEngine &e, std::function<void(EngineParams &)> setup,
                            std::vector<int> midis, double secs, int outMode, int vel = 100,
                            double offAt = -1.0, std::function<void(int)> each = {})
            {
                e.prepare(fsr, B);
                e.setLibrary(lib);
                EngineParams p;
                p.stopOn = {};
                p.stopOn[0] = true;
                p.clickLevel = 0;
                p.releaseClick = 0;
                p.wind = 0;
                p.stopPull = 0;
                p.outputMode = outMode;
                setup(p);
                const int ch = outMode == 0 ? 2 : (outMode + 1) * (outMode + 1);
                const int total = (int)(secs * fsr);
                juce::AudioBuffer<float> keep(ch, total), b(ch, B);
                keep.clear();
                bool off = false;
                for (int pos = 0; pos < total; pos += B)
                {
                    juce::MidiBuffer mb;
                    if (pos == 0)
                        for (int m : midis)
                            mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)vel), 0);
                    if (offAt >= 0.0 && !off && pos >= (int)(offAt * fsr))
                    {
                        off = true;
                        for (int m : midis)
                            mb.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                    }
                    b.clear();
                    e.process(b, mb, p);
                    for (int c = 0; c < ch; ++c)
                        keep.copyFrom(c, pos, b, c, 0, juce::jmin(B, total - pos));
                    if (each)
                        each(pos);
                }
                return keep;
            };
            auto rmsDb = [&](const juce::AudioBuffer<float> &b, double t0, double t1)
            {
                double s = 0;
                int n = 0;
                for (int i = (int)(t0 * fsr); i < juce::jmin(b.getNumSamples(), (int)(t1 * fsr));
                     ++i)
                {
                    s += (double)b.getSample(0, i) * b.getSample(0, i);
                    ++n;
                }
                return 10.0 * std::log10(juce::jmax(1.0e-30, s / juce::jmax(1, n)));
            };
            auto finiteB = [](const juce::AudioBuffer<float> &b)
            {
                for (int c = 0; c < b.getNumChannels(); ++c)
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        if (!std::isfinite(b.getSample(c, i)))
                            return false;
                return true;
            };
            auto allStops = [](EngineParams &p)
            {
                for (int s = 0; s < 4; ++s)
                    p.stopOn[(size_t)s] = true;
            };

            // Balg, blower: one pipe hardly moves the pressure, a big chord on four stops pulls it
            // down (quieter, flatter)
            {
                OrganEngine e1, e2, e0;
                auto bel = [&](EngineParams &p)
                {
                    allStops(p);
                    p.live.bellows = 1.0f;
                    p.live.bellowsSize = 2.0f;
                };
                play(
                    e1,
                    [](EngineParams &p)
                    {
                        p.live.bellows = 1.0f;
                        p.live.bellowsSize = 2.0f;
                    },
                    {72}, 2.0, 0);
                const auto starved = play(e2, bel, {41, 48, 55, 60, 64, 67}, 3.0, 0);
                const auto full = play(e0, allStops, {41, 48, 55, 60, 64, 67}, 3.0, 0);
                const double drop = rmsDb(starved, 2.0, 3.0) - rmsDb(full, 2.0, 3.0);
                std::cout << "bellows (blower): pressure with one high pipe "
                          << e1.bellowsNow.load() << ", with 24 pipes " << e2.bellowsNow.load()
                          << ", level " << drop << " dB; without the bellows "
                          << e0.bellowsNow.load() << std::endl;
                check(e1.bellowsNow.load() > 0.85f && e2.bellowsNow.load() < 0.6f &&
                          e0.bellowsNow.load() == 1.0f,
                      "bellows: big chords pull the shared wind down");
                check(drop < -3.0 && finiteB(starved), "bellows: the starved organ is quieter");
                writeWav(outDir.getChildFile("80_balg_geblaese.wav"), starved, fsr);
            }
            // Balg, Kalkant: the bellows sinks while the chord sounds and is lifted again by the
            // tread
            {
                OrganEngine e;
                float lo = 2.0f, hiAfter = 0.0f;
                int firstTread = -1;
                const auto b = play(
                    e,
                    [&](EngineParams &p)
                    {
                        allStops(p);
                        p.live.bellows = 1.0f;
                        p.live.bellowsSize = 1.5f;
                        p.live.blower = 1;
                        p.live.treadRate = 20.0f;
                    },
                    {41, 48, 55, 60, 64, 67}, 7.0, 0, 100, -1.0,
                    [&](int pos)
                    {
                        const float f = e.bellowsFillNow.load();
                        if (e.treadCount.load() == 0)
                            lo = juce::jmin(lo, f);
                        else
                        {
                            if (firstTread < 0)
                                firstTread = pos;
                            hiAfter = juce::jmax(hiAfter, f);
                        }
                    });
                const double tTread = firstTread / fsr;
                const double before = rmsDb(b, tTread - 0.4, tTread - 0.05),
                             after = rmsDb(b, tTread + 0.5, tTread + 0.9);
                std::cout << "bellows (Kalkant): fill down to " << lo
                          << " before the first tread at " << tTread << " s, up to " << hiAfter
                          << " after it; level before " << before << " dB, after " << after << " dB"
                          << std::endl;
                check(lo < 0.15f && hiAfter > 0.9f && firstTread > 0,
                      "Kalkant: the bellows empties and is lifted by the tread");
                check(after > before + 6.0 && finiteB(b),
                      "Kalkant: the organ comes back with the tread");
                writeWav(outDir.getChildFile("81_balg_kalkant.wav"), b, fsr);
            }
            // Ventil: a slowly pressed key lets the wind in hesitantly, a fast one speaks at once;
            // both arrive at the same tone
            {
                OrganEngine es, ef;
                auto pal = [](EngineParams &p) { p.live.pallet = 1.0f; };
                const auto slow = play(es, pal, {60}, 3.5, 0, 8),
                           fast = play(ef, pal, {60}, 3.5, 0, 127);
                const double early = rmsDb(slow, 0.1, 0.4) - rmsDb(fast, 0.1, 0.4),
                             late = rmsDb(slow, 2.8, 3.4) - rmsDb(fast, 2.8, 3.4);
                std::cout << "pallet: slow key against fast key, level at 0.1-0.4 s " << early
                          << " dB, at 2.8-3.4 s " << late << " dB" << std::endl;
                check(early < -6.0 && std::abs(late) < 1.5 && finiteB(slow),
                      "pallet: a slow key speaks hesitantly and arrives at the full tone");
                writeWav(outDir.getChildFile("82_ventil_langsam.wav"), slow, fsr);
            }
            // the listener walks to the right: the pipe is heard further to the left; walking in a
            // circle changes the direction all the time
            {
                auto azOf = [&](const juce::AudioBuffer<float> &b, double t0, double t1)
                {
                    double y = 0, x = 0;
                    for (int i = (int)(t0 * fsr); i < (int)(t1 * fsr); ++i)
                    {
                        y += (double)b.getSample(0, i) * b.getSample(1, i);
                        x += (double)b.getSample(0, i) * b.getSample(3, i);
                    }
                    return juce::radiansToDegrees(std::atan2(y, x));
                };
                OrganEngine e0, e1, e2;
                const auto here = play(e0, [](EngineParams &) {}, {69}, 1.5, 1);
                const auto right =
                    play(e1, [](EngineParams &p) { p.live.listenX = 6.0f; }, {69}, 1.5, 1);
                const auto round = play(
                    e2,
                    [](EngineParams &p)
                    {
                        p.live.walkShape = 1;
                        p.live.walkSize = 5.0f;
                        p.live.walkRate = 0.5f;
                    },
                    {69}, 2.0, 1);
                const double a0 = azOf(here, 0.5, 1.4), a1 = azOf(right, 0.8, 1.4),
                             r0 = azOf(round, 0.4, 0.6), r1 = azOf(round, 1.4, 1.6);
                std::cout << "listener: the pipe from " << a0
                          << " deg, after 6 m to the right from " << a1
                          << " deg; walking a circle: " << r0 << " -> " << r1 << " deg"
                          << std::endl;
                check(std::remainder(a1 - a0, 360.0) > 15.0 &&
                          std::abs(std::remainder(r1 - r0, 360.0)) > 20.0 && finiteB(round),
                      "listener: walking changes where the pipes are heard");
                check(std::abs(e1.listenerXNow.load() - 6.0f) < 0.05f,
                      "listener: arrives where the slider says");
            }
            // out of sight: in an L-shaped room the listener walks round the corner; the pipe is
            // heard around it
            {
                auto roomL = [](EngineParams &p, int shape)
                {
                    p.room.on = true;
                    p.room.width = 16;
                    p.room.length = 22;
                    p.room.height = 8;
                    p.room.order = 1;
                    p.room.shape = shape;
                    p.room.shapeAmt = 1.0f;
                    p.room.erGain = 0.0f;
                    p.room.tailGain = 0.0f; // the direct sound alone
                };
                // the geometry first: find a place in the plan from where the pipe (straight ahead,
                // 7 m) is hidden
                RoomParams r;
                r.width = 16;
                r.length = 22;
                r.height = 8;
                r.shape = shapeL;
                r.shapeAmt = 1.0f;
                finishPlan(r, defaultDrawnPlan());
                const auto g = planGeo(r);
                const auto corners = planCorners(g);
                Pos pipe;
                pipe.az = 0.0f;
                pipe.el = 0.0f;
                pipe.dist = 7.0f;
                const Vec3f S = roomPoint(r, pipe), home = listenerPos(r);
                float wx = 0.0f, wy = 0.0f;
                bool found = false;
                Pos heard;
                float lossDb = 0.0f, fc = 20000.0f;
                for (int iy = 0; iy < 44 && !found; ++iy)
                    for (int ix = 0; ix < 32 && !found; ++ix)
                    {
                        Vec3f L{0.25f + 0.5f * (float)ix, 0.25f + 0.5f * (float)iy, home.z};
                        if (!insidePlan(g, L.x, L.y) || planEdgeDist(g, L.x, L.y) < 1.0f)
                            continue;
                        float ld, lf;
                        Pos h;
                        if (diffractedPath(g, corners, L, S, h, ld, lf) && ld > 4.0f)
                        {
                            found = true;
                            wx = L.x - home.x;
                            wy = L.y - home.y;
                            heard = h;
                            lossDb = ld;
                            fc = lf;
                        }
                    }
                std::cout << "out of sight: " << corners.n << " corners to bend around; from " << wx
                          << " / " << wy << " m off the usual place the pipe is hidden, heard from "
                          << heard.az << " deg over " << heard.dist << " m, " << lossDb
                          << " dB quieter, cut above " << fc << " Hz" << std::endl;
                check(found && corners.n > 0 &&
                          heard.dist > std::sqrt((S.x - home.x - wx) * (S.x - home.x - wx) +
                                                 (S.y - home.y - wy) * (S.y - home.y - wy)) -
                                           0.01f,
                      "out of sight: an L-shaped plan hides the pipe from somewhere, and the way "
                      "around is longer");
                // the engine: the same walk, in the L and in a plain rectangle of the same size
                OrganEngine eh, eb;
                for (auto *en : {&eh, &eb})
                    for (int k = 0; k < kNumKeys; ++k)
                    {
                        en->baseAz[0][(size_t)k] = 0.0f;
                        en->baseEl[0][(size_t)k] = 0.0f;
                        en->baseDist[0][(size_t)k] = 7.0f;
                    }
                const auto hidden = play(
                    eh,
                    [&](EngineParams &p)
                    {
                        roomL(p, shapeL);
                        p.live.listenX = wx;
                        p.live.listenY = wy;
                    },
                    {69}, 2.0, 1);
                const int hidCount = eh.hiddenNow.load();
                const auto open = play(
                    eb,
                    [&](EngineParams &p)
                    {
                        roomL(p, shapeBox);
                        p.live.listenX = wx;
                        p.live.listenY = wy;
                    },
                    {69}, 2.0, 1);
                const double dh = rmsDb(hidden, 1.0, 1.8), db = rmsDb(open, 1.0, 1.8);
                std::cout << "out of sight: " << hidCount
                          << " pipe(s) heard around the corner; level " << dh << " dB against "
                          << db << " dB in plain sight" << std::endl;
                check(hidCount > 0 && dh < db - 4.0 && finiteB(hidden),
                      "out of sight: a pipe behind the corner is heard around it, quieter");
                writeWav(outDir.getChildFile("85_um_die_ecke.wav"), hidden, fsr);
                // walking to and fro across the shadow's edge: no jump in level (against the same
                // walk in plain sight)
                {
                    OrganEngine ew, eo;
                    for (auto *en : {&ew, &eo})
                        for (int k = 0; k < kNumKeys; ++k)
                        {
                            en->baseAz[0][(size_t)k] = 0.0f;
                            en->baseEl[0][(size_t)k] = 0.0f;
                            en->baseDist[0][(size_t)k] = 7.0f;
                        }
                    auto walk = [&](int shape)
                    {
                        return [&, shape](EngineParams &p)
                        {
                            roomL(p, shape);
                            p.live.listenX = wx - 2.5f;
                            p.live.listenY = wy;
                            p.live.walkShape = 2;
                            p.live.walkSize = 3.5f;
                            p.live.walkRate = 0.15f;
                        };
                    }; // (running: up to 3.3 m/s)
                    int changes = 0, last = -1;
                    juce::String when;
                    const auto across = play(ew, walk(shapeL), {69}, 8.0, 1, 100, -1.0,
                                             [&](int pos)
                                             {
                                                 const int now = ew.hiddenNow.load() > 0 ? 1 : 0;
                                                 if (last >= 0 && now != last)
                                                 {
                                                     ++changes;
                                                     when << juce::String(pos / fsr, 2)
                                                          << (now ? " in " : " out ");
                                                 }
                                                 last = now;
                                             });
                    const auto plain = play(eo, walk(shapeBox), {69}, 8.0, 1);
                    auto maxJumpDb = [&](const juce::AudioBuffer<float> &b)
                    {
                        // the largest change of level from one 5 ms window to the next (W channel),
                        // after the attack
                        const int win = (int)(0.02 * fsr);
                        double prev = -1000.0, worst = 0.0;
                        double at = 0.0;
                        for (int i = (int)(0.6 * fsr); i + win < b.getNumSamples(); i += win)
                        {
                            double s = 0;
                            for (int j = 0; j < win; ++j)
                                s += (double)b.getSample(0, i + j) * b.getSample(0, i + j);
                            const double db = 10.0 * std::log10(s / win + 1e-12);
                            if (prev > -900.0 && std::abs(db - prev) > worst)
                            {
                                worst = std::abs(db - prev);
                                at = i / fsr;
                            }
                            prev = db;
                        }
                        std::cout << "  (largest step at " << at << " s)" << std::endl;
                        return worst;
                    };
                    const double jA = maxJumpDb(across), jP = maxJumpDb(plain);
                    std::cout << "out of sight: walking across the shadow's edge (" << changes
                              << " times in or out of sight): largest step " << jA
                              << " dB per 20 ms, the same walk in plain sight " << jP
                              << " dB; shadow: " << when << std::endl;
                    check(changes >= 2 && jA < jP + 1.5 && finiteB(across),
                          "out of sight: stepping in and out of the shadow does not jump");
                    // ... and does not click: the sharpest sample-to-sample bend (2nd difference,
                    // all four channels) against the same walk in plain sight
                    auto sharpest = [&](const juce::AudioBuffer<float> &b, double &at)
                    {
                        double worst = 0.0;
                        for (int c = 0; c < juce::jmin(4, b.getNumChannels()); ++c)
                            for (int i = (int)(0.6 * fsr); i < b.getNumSamples(); ++i)
                            {
                                const double d2 =
                                    std::abs((double)b.getSample(c, i) -
                                             2.0 * b.getSample(c, i - 1) + b.getSample(c, i - 2));
                                if (d2 > worst)
                                {
                                    worst = d2;
                                    at = i / fsr;
                                }
                            }
                        return worst;
                    };
                    double atA = 0, atP = 0;
                    const double sA = sharpest(across, atA), sP = sharpest(plain, atP);
                    std::cout << "out of sight: sharpest 2nd difference " << sA << " at " << atA
                              << " s, in plain sight " << sP << " at " << atP << " s" << std::endl;
                    check(sA < 1.3 * sP,
                          "out of sight: stepping in and out of the shadow does not click");
                    // the hard case: a cross-shaped church with its reflections, all stops, a
                    // chord, walking a circle through the arms (pipes vanish behind one corner,
                    // then two) - against the very same run with no hiding
                    OrganEngine ec, en;
                    en.occlusionEnabled = false;
                    auto cross = [](EngineParams &p)
                    {
                        p.room.on = true;
                        p.room.width = 14;
                        p.room.length = 28;
                        p.room.height = 11;
                        p.room.order = 2;
                        p.room.tailGain = 0.0f;
                        p.room.shape = shapeCross;
                        p.room.shapeAmt = 0.8f;
                        p.room.erGain = 1.0f;
                        for (int s = 0; s < kNumStops; ++s)
                            p.stopOn[(size_t)s] = true;
                        p.live.walkShape = 1;
                        p.live.walkSize = 6.0f;
                        p.live.walkRate = 0.08f;
                    };
                    int crossChanges = 0, lastHid = -1, maxHid = 0;
                    const auto withWalls = play(ec, cross, {48, 55, 60, 64, 67}, 12.0, 1, 100, -1.0,
                                                [&](int)
                                                {
                                                    const int h = ec.hiddenNow.load();
                                                    if (lastHid >= 0 && h != lastHid)
                                                        ++crossChanges;
                                                    lastHid = h;
                                                    maxHid = juce::jmax(maxHid, h);
                                                });
                    const auto noWalls = play(en, cross, {48, 55, 60, 64, 67}, 12.0, 1);
                    double atC = 0, atN = 0;
                    const double sC = sharpest(withWalls, atC), sN = sharpest(noWalls, atN);
                    std::cout << "out of sight: cross, walking a circle: up to " << maxHid
                              << " pipes hidden, " << crossChanges
                              << " changes; sharpest 2nd difference " << sC << " at " << atC
                              << " s, without hiding " << sN << " at " << atN << " s" << std::endl;
                    check(maxHid > 0 && crossChanges > 4 && sC < 1.3 * sN && finiteB(withWalls),
                          "out of sight: walking through a cross-shaped church does not click");
                    writeWav(outDir.getChildFile("87_kreuz_rundgang.wav"), withWalls, fsr);
                    // the same chord standing still in the cross: walking must not be sharper than
                    // that (it clicked before: the listener jumped at the inner corners, the pipe
                    // was placed by a ray from where it walked to)
                    {
                        OrganEngine es;
                        const auto standing = play(
                            es,
                            [&](EngineParams &p)
                            {
                                cross(p);
                                p.live.walkShape = 0;
                            },
                            {48, 55, 60, 64, 67}, 12.0, 1);
                        double atS = 0;
                        const double sS = sharpest(standing, atS);
                        std::cout << "out of sight: cross, sharpest 2nd difference walking " << sC
                                  << ", standing still " << sS << std::endl;
                        check(sC < 1.5 * sS, "out of sight: walking through the cross is as smooth "
                                             "as standing in it");
                    }
                    writeWav(outDir.getChildFile("86_schattenkante.wav"), across, fsr);
                }
            }
            // ---- room motion: pipes as bodies in the Saal (Flug, Fall, Schwarm, Stroemung),
            // knocking on its walls
            {
                auto saal = [](EngineParams &p, int shape)
                {
                    p.room.on = true;
                    p.room.width = 14;
                    p.room.length = 28;
                    p.room.height = 11;
                    p.room.order = 2;
                    p.room.tailGain = 0.0f;
                    p.room.shape = shape;
                    p.room.shapeAmt = 0.8f;
                };
                // every mode keeps every pipe inside the walls, between floor and ceiling, and
                // moves it
                for (int mode : {(int)Motion::Flight, (int)Motion::Fall, (int)Motion::Swarm,
                                 (int)Motion::Flow})
                {
                    OrganEngine e;
                    float minZ = 1e9f, maxZ = -1e9f;
                    bool inside = true;
                    int moved = 0;
                    std::array<float, kNumKeys> x0{};
                    RoomParams rp;
                    rp.width = 14;
                    rp.length = 28;
                    rp.height = 11;
                    rp.shape = shapeCross;
                    rp.shapeAmt = 0.8f;
                    finishPlan(rp, defaultDrawnPlan());
                    const auto geo = planGeo(rp);
                    const auto b = play(
                        e,
                        [&](EngineParams &p)
                        {
                            saal(p, shapeCross);
                            p.motionMode[0] = mode;
                            p.motionRate[0] = 0.4f;
                            p.motionDepth[0] = 90.0f;
                            p.live.impLevel = 1.0f;
                            p.live.impAll = true;
                        },
                        {48, 60, 67}, 6.0, 1, 100, -1.0,
                        [&](int pos)
                        {
                            for (int k = 0; k < kNumKeys; ++k)
                            {
                                Vec3f q;
                                if (!e.bodyAt(0, k, q))
                                    continue;
                                if (pos == 0)
                                    x0[(size_t)k] = q.x;
                                if (pos > (int)fsr && !insidePlan(geo, q.x, q.y))
                                    inside = false;
                                minZ = juce::jmin(minZ, q.z);
                                maxZ = juce::jmax(maxZ, q.z);
                            }
                        });
                    for (int k = 0; k < kNumKeys; ++k)
                    {
                        Vec3f q;
                        if (e.bodyAt(0, k, q) && std::abs(q.x - x0[(size_t)k]) > 0.5f)
                            ++moved;
                    }
                    const char *names[]{"Flug", "Fall", "Schwarm", "Stroemung"};
                    std::cout << "room motion " << names[mode - (int)Motion::Flight]
                              << " (cross plan): " << moved << " of " << kNumKeys
                              << " pipes moved, height " << minZ << " .. " << maxZ << " m, "
                              << e.impactCount.load()
                              << " impacts, inside the walls: " << (inside ? "yes" : "no")
                              << std::endl;
                    check(inside && moved > kNumKeys / 2 && minZ >= 0.1f && maxZ <= 11.0f &&
                              finiteB(b),
                          "room motion: the pipes move freely and stay in the Saal");
                    if (mode == (int)Motion::Flight || mode == (int)Motion::Fall)
                        check(e.impactCount.load() > 5,
                              "room motion: flying pipes hit the walls / the floor");
                    if (mode == (int)Motion::Flight)
                        writeWav(outDir.getChildFile("88_flug_im_saal.wav"), b, fsr);
                    if (mode == (int)Motion::Fall)
                        writeWav(outDir.getChildFile("89_fall_im_saal.wav"), b, fsr);
                }
                // an impact: faster = louder and brighter; a soft wall = quieter and duller
                {
                    auto knock = [&](float speed, float absorb, int stop = 0)
                    {
                        OrganEngine e;
                        bool sent = false;
                        return play(
                            e,
                            [](EngineParams &p)
                            {
                                p.live.impLevel = 1.0f;
                                p.live.impRing = 0.6f;
                            },
                            {}, 1.2, 1, 100, -1.0,
                            [&](int)
                            {
                                if (!sent)
                                {
                                    e.testImpact(7.0f, 15.2f, 1.6f, speed, absorb, stop, 12);
                                    sent = true;
                                } // 4 m in front of the listener
                            });
                    };
                    auto bright = [&](const juce::AudioBuffer<float> &b)
                    {
                        // the share of highs: first difference against the signal itself (W, the
                        // first 50 ms)
                        double s = 0, d = 0;
                        for (int i = 1; i < (int)(0.02 * fsr); ++i)
                        {
                            const double x = b.getSample(0, i), xp = b.getSample(0, i - 1);
                            s += x * x;
                            d += (x - xp) * (x - xp);
                        }
                        return 10.0 * std::log10(juce::jmax(1e-30, d) / juce::jmax(1e-30, s));
                    };
                    auto peakDb = [&](const juce::AudioBuffer<float> &b)
                    {
                        return juce::Decibels::gainToDecibels(
                            b.getMagnitude(0, 0, b.getNumSamples()));
                    };
                    const auto slow = knock(1.0f, 0.05f), fast = knock(6.0f, 0.05f),
                               soft = knock(6.0f, 0.6f);
                    std::cout << "impact: slow (1 m/s) peak " << peakDb(slow) << " dB, highs "
                              << bright(slow) << " dB; fast (6 m/s) " << peakDb(fast)
                              << " dB, highs " << bright(fast) << " dB; fast on a curtain "
                              << peakDb(soft) << " dB, highs " << bright(soft) << " dB"
                              << std::endl;
                    check(peakDb(fast) > peakDb(slow) + 8.0 && bright(fast) > bright(slow) + 3.0,
                          "impact: a faster hit is louder and brighter");
                    check(peakDb(soft) < peakDb(fast) - 4.0 && bright(soft) < bright(fast),
                          "impact: an absorbing wall dulls the knock");
                    check(finiteB(fast) && rmsDb(fast, 0.9, 1.2) < rmsDb(fast, 0.0, 0.2) - 20.0,
                          "impact: the struck pipe rings out");
                    // against a sounding pipe (the same place, 0 dB each): a hard knock should be
                    // clearly there, not overwhelming
                    OrganEngine ep;
                    for (int k = 0; k < kNumKeys; ++k)
                    {
                        ep.baseAz[0][(size_t)k] = 0.0f;
                        ep.baseEl[0][(size_t)k] = 0.0f;
                        ep.baseDist[0][(size_t)k] = 4.0f;
                    }
                    const auto pipe = play(ep, [](EngineParams &) {}, {60}, 1.5, 1);
                    const double pipePeak = juce::Decibels::gainToDecibels(
                        pipe.getMagnitude(0, (int)(0.8 * fsr), (int)(0.5 * fsr)));
                    std::cout
                        << "impact: a 3 m/s knock against a pipe at the same distance: knock peak "
                        << peakDb(knock(3.0f, 0.05f)) << " dB, pipe peak " << pipePeak << " dB"
                        << std::endl;
                    writeWav(outDir.getChildFile("90_aufprall_schnell.wav"), fast, fsr);
                    {
                        // the Gedackt is of wood: its knock duller and over sooner than the tin Praestant's
                        // (duller: less energy in the highs - the first difference of the signal - at the same
                        //  speed; its own woody "tock" lies in the middle)
                        auto highs = [&](const juce::AudioBuffer<float> &b)
                        {
                            double d = 0;
                            for (int i = 1; i < (int)(0.05 * fsr); ++i)
                            {
                                const double x = b.getSample(0, i) - b.getSample(0, i - 1);
                                d += x * x;
                            }
                            return 10.0 * std::log10(juce::jmax(1e-30, d));
                        };
                        const auto tin = knock(4.0f, 0.05f, 0), wood = knock(4.0f, 0.05f, 1);
                        const double tinTail = rmsDb(tin, 0.25, 0.6) - rmsDb(tin, 0.0, 0.05),
                                     woodTail = rmsDb(wood, 0.25, 0.6) - rmsDb(wood, 0.0, 0.05);
                        std::cout << "impact: tin highs " << highs(tin) << " dB, tail " << tinTail
                                  << " dB; wood highs " << highs(wood) << " dB, tail " << woodTail << " dB"
                                  << std::endl;
                        check(finiteB(wood) && highs(wood) < highs(tin) - 3.0 && woodTail < tinTail - 6.0,
                              "impact: a wooden pipe knocks dull and short");
                        writeWav(outDir.getChildFile("90_aufprall_holz.wav"), wood, fsr);
                        writeWav(outDir.getChildFile("90_aufprall_zinn.wav"), tin, fsr);
                    }
                }
                // the same paths at any block size (a fixed inner step)
                {
                    auto runBlocks = [&](int block)
                    {
                        OrganEngine e;
                        e.prepare(fsr, block);
                        e.setLibrary(lib);
                        EngineParams p;
                        p.stopOn = {};
                        p.stopOn[0] = true;
                        p.motionMode[0] = (int)Motion::Flight;
                        p.motionRate[0] = 0.5f;
                        p.motionDepth[0] = 60.0f;
                        p.live.impLevel = 1.0f;
                        p.live.impAll = true;
                        saal(p, shapeBox);
                        juce::AudioBuffer<float> b(16, block);
                        for (int pos = 0; pos < 153600; pos += block)
                        {
                            juce::MidiBuffer mb;
                            b.clear();
                            e.process(b, mb, p);
                        } // (3.2 s: whole blocks of both sizes)
                        Vec3f q;
                        e.bodyAt(0, 20, q);
                        return std::make_pair(q, e.impactCount.load());
                    };
                    const auto a = runBlocks(64), c = runBlocks(512);
                    std::cout << "room motion: block 64: pipe at " << a.first.x << " / "
                              << a.first.y << ", " << a.second
                              << " impacts; block 512: " << c.first.x << " / " << c.first.y << ", "
                              << c.second << std::endl;
                    check(std::abs(a.first.x - c.first.x) < 1e-3f &&
                              std::abs(a.first.y - c.first.y) < 1e-3f && a.second == c.second,
                          "room motion: the same paths at any block size");
                }
                // flying pipes in the cross with reflections (no knocks): no clicks - against them
                // standing still
                {
                    OrganEngine ef, es;
                    auto fly = [&](bool moving)
                    {
                        return [&, moving](EngineParams &p)
                        {
                            saal(p, shapeCross);
                            p.room.erGain = 1.0f;
                            for (int s = 0; s < 4; ++s)
                            {
                                p.stopOn[(size_t)s] = true;
                                p.motionMode[(size_t)s] =
                                    moving ? (int)Motion::Flight : (int)Motion::Static;
                                p.motionRate[(size_t)s] = 0.3f;
                            }
                        };
                    };
                    const auto flying = play(ef, fly(true), {48, 55, 60, 64, 67}, 8.0, 1);
                    const auto still = play(es, fly(false), {48, 55, 60, 64, 67}, 8.0, 1);
                    double s1 = 0, s2 = 0;
                    for (int c = 0; c < 4; ++c)
                        for (int i = (int)(0.6 * fsr); i < flying.getNumSamples(); ++i)
                        {
                            s1 = juce::jmax(s1, (double)std::abs(flying.getSample(c, i) -
                                                                 2.0f * flying.getSample(c, i - 1) +
                                                                 flying.getSample(c, i - 2)));
                            s2 = juce::jmax(s2, (double)std::abs(still.getSample(c, i) -
                                                                 2.0f * still.getSample(c, i - 1) +
                                                                 still.getSample(c, i - 2)));
                        }
                    std::cout << "room motion: flying pipes in the cross, sharpest 2nd difference "
                              << s1 << ", standing pipes " << s2 << ", " << ef.hiddenNow.load()
                              << " out of sight" << std::endl;
                    check(s1 < 1.6 * s2 && finiteB(flying),
                          "room motion: flying pipes (reflections, out of sight) do not click");
                }
                // what it costs: all eight stops flocking, a chord
                {
                    OrganEngine e;
                    const auto t0 = std::chrono::high_resolution_clock::now();
                    play(
                        e,
                        [&](EngineParams &p)
                        {
                            saal(p, shapeCross);
                            for (int s = 0; s < kNumStops; ++s)
                            {
                                p.stopOn[(size_t)s] = true;
                                p.motionMode[(size_t)s] = (int)Motion::Swarm;
                                p.motionRate[(size_t)s] = 0.3f;
                            }
                            p.live.impLevel = 1.0f;
                        },
                        {48, 55, 60, 64, 67}, 4.0, 1);
                    const double rtf = 4.0 / std::chrono::duration<double>(
                                                 std::chrono::high_resolution_clock::now() - t0)
                                                 .count();
                    OrganEngine e2;
                    const auto t1 = std::chrono::high_resolution_clock::now();
                    play(
                        e2,
                        [&](EngineParams &p)
                        {
                            saal(p, shapeCross);
                            for (int s = 0; s < kNumStops; ++s)
                            {
                                p.stopOn[(size_t)s] = true;
                                p.motionMode[(size_t)s] = (int)Motion::Rotation;
                                p.motionRate[(size_t)s] = 0.3f;
                            }
                        },
                        {48, 55, 60, 64, 67}, 4.0, 1);
                    const double rtf2 = 4.0 / std::chrono::duration<double>(
                                                  std::chrono::high_resolution_clock::now() - t1)
                                                  .count();
                    std::cout << "room motion: 8 stops flocking " << rtf
                              << " x real time, rotating (moving too, the old way) " << rtf2 << " x"
                              << std::endl;
                    check(rtf > 0.8 * rtf2,
                          "room motion: flocking costs little more than the old motion");
                }
            }
            // ---- the 3D Saal's illusions: Selbstspiel, Wachsen & Brechen, Fluestern, Unmoeglicher
            // Raum, Zeit als Entfernung
            {
                auto hall = [](EngineParams &p)
                {
                    p.room.on = true;
                    p.room.width = 14;
                    p.room.length = 28;
                    p.room.height = 11;
                    p.room.order = 1;
                    p.room.tailGain = 0.0f;
                    p.motionMode[0] = (int)Motion::Flight;
                    p.motionRate[0] = 0.6f;
                    p.motionDepth[0] = 60.0f;
                };
                {
                    // Selbstspiel: nobody plays, the pipes fly - with it they sound by themselves
                    OrganEngine eOff, eOn;
                    int maxVoices = 0;
                    const auto off = play(eOff, [&](EngineParams &p) { hall(p); }, {}, 6.0, 1);
                    const auto on = play(
                        eOn,
                        [&](EngineParams &p)
                        {
                            hall(p);
                            p.live.selfPlay = 0.7f;
                        },
                        {}, 6.0, 1, 100, -1.0,
                        [&](int) { maxVoices = juce::jmax(maxVoices, eOn.activeVoices.load()); });
                    std::cout << "Selbstspiel: off " << rmsDb(off, 0.5, 6.0) << " dB, on "
                              << rmsDb(on, 0.5, 6.0) << " dB, at most " << maxVoices << " voices"
                              << std::endl;
                    check(rmsDb(off, 0.5, 6.0) < -100.0 && rmsDb(on, 0.5, 6.0) > -60.0 &&
                              maxVoices > 0 && maxVoices <= 48 && finiteB(on),
                          "Selbstspiel: knocks become notes (and stay few)");
                }
                {
                    // Wachsen & Brechen: half the length sounds an octave higher; pipes left alone
                    // grow; a hard knock breaks them; switched off they find back
                    auto crossings = [&](const juce::AudioBuffer<float> &b)
                    {
                        int n = 0;
                        for (int i = (int)(0.6 * fsr); i < (int)(1.6 * fsr); ++i)
                            n += (b.getSample(0, i - 1) < 0.0f) != (b.getSample(0, i) < 0.0f) ? 1 : 0;
                        return n;
                    };
                    OrganEngine eWhole, eHalf;
                    for (int k = 0; k < kNumKeys; ++k)
                        eHalf.pipeLen[0][(size_t)k] = 0.5f;
                    auto still = [](EngineParams &p) { p.live.grow = 0.01f; };
                    const int zw = crossings(play(eWhole, still, {60}, 2.0, 0)),
                              zh = crossings(play(eHalf, still, {60}, 2.0, 0));
                    std::cout << "Wachsen & Brechen: zero crossings whole " << zw << ", half as long "
                              << zh << std::endl;
                    check(zw > 50 && std::abs((double)zh / zw - 2.0) < 0.2,
                          "Wachsen & Brechen: half the length sounds an octave higher");
                    OrganEngine e;
                    play(e, [&](EngineParams &p) { p.live.grow = 1.0f; }, {}, 4.0, 0);
                    const float grown = e.pipeLen[0][20].load();
                    play(
                        e,
                        [&](EngineParams &p)
                        {
                            hall(p);
                            p.motionRate[0] = 1.0f;
                            p.motionDepth[0] = 90.0f;
                            p.live.grow = 1.0f;
                        },
                        {}, 8.0, 1);
                    float shortest = 9.0f;
                    for (int k = 0; k < kNumKeys; ++k)
                        shortest = juce::jmin(shortest, e.pipeLen[0][(size_t)k].load());
                    const auto broken = e.breakCount.load();
                    play(e, [&](EngineParams &) {}, {}, 12.0, 0);
                    float worst = 0.0f;
                    for (int k = 0; k < kNumKeys; ++k)
                        worst = juce::jmax(worst, std::abs(e.pipeLen[0][(size_t)k].load() - 1.0f));
                    std::cout << "Wachsen & Brechen: after 4 s alone " << grown << " times as long; "
                              << broken << " breaks, shortest " << shortest
                              << "; switched off, furthest from 1: " << worst << std::endl;
                    check(grown > 1.05f && grown < 1.2f, "Wachsen & Brechen: pipes left alone grow");
                    check(broken > 0 && shortest >= 0.25f && shortest < 1.0f,
                          "Wachsen & Brechen: a hard knock breaks a pipe");
                    check(worst == 0.0f, "Wachsen & Brechen: switched off, every pipe finds back");
                }
                {
                    // Fluestern: a pipe half a metre to the left is much louder in the left ear,
                    // to the right in the right; looked at, in both alike; off: nothing
                    auto ears = [&](float az, float yaw, float whisper, float walk = 0.0f)
                    {
                        OrganEngine e;
                        for (int k = 0; k < kNumKeys; ++k)
                        {
                            e.baseAz[0][(size_t)k] = az;
                            e.baseEl[0][(size_t)k] = 0.0f;
                            e.baseDist[0][(size_t)k] = 0.5f;
                        }
                        e.headYaw = yaw;
                        double l = 0, r = 0;
                        bool any = false;
                        play(
                            e, [&](EngineParams &p) { p.live.whisper = whisper; p.live.listenX = walk; }, {60}, 1.5, 3,
                            100, -1.0,
                            [&](int)
                            {
                                if (e.nearBus(0) == nullptr)
                                    return;
                                any = true;
                                for (int i = 0; i < B; ++i)
                                {
                                    l += (double)e.nearBus(0)[i] * e.nearBus(0)[i];
                                    r += (double)e.nearBus(1)[i] * e.nearBus(1)[i];
                                }
                            });
                        return std::make_tuple(10.0 * std::log10(juce::jmax(1e-30, l)),
                                               10.0 * std::log10(juce::jmax(1e-30, r)), any);
                    };
                    const auto left = ears(90.0f, 0.0f, 1.0f), right = ears(-90.0f, 0.0f, 1.0f),
                               faced = ears(90.0f, 90.0f, 1.0f), none = ears(90.0f, 0.0f, 0.0f),
                               atEar = ears(90.0f, 0.0f, 1.0f, -0.35f);
                    std::cout << "Fluestern: pipe on the left: left - right "
                              << std::get<0>(left) - std::get<1>(left) << " dB; on the right "
                              << std::get<0>(right) - std::get<1>(right) << " dB; looked at "
                              << std::get<0>(faced) - std::get<1>(faced) << " dB; 15 cm from the left ear "
                              << std::get<0>(atEar) - std::get<1>(atEar) << " dB" << std::endl;

                    check(std::get<0>(left) - std::get<1>(left) > 4.0 &&
                              std::get<1>(right) - std::get<0>(right) > 4.0 &&
                              std::get<0>(atEar) - std::get<1>(atEar) > 12.0,
                          "Fluestern: the near ear is much louder");
                    check(std::abs(std::get<0>(faced) - std::get<1>(faced)) < 1.0,
                          "Fluestern: a pipe looked at is in both ears alike");
                    check(std::get<2>(left) && !std::get<2>(none), "Fluestern: off = nothing added");
                    {
                        // a pipe carried in the Saal, held 20 cm to the left of the head
                        OrganEngine e;
                        RoomParams rp;
                        rp.width = 14;
                        rp.length = 28;
                        rp.height = 11;
                        const Vec3f L = listenerPos(rp);
                        int key = 0;
                        for (int k = 0; k < kNumKeys; ++k)
                            if (kPhysKeys[(size_t)k] == 60)
                                key = k;
                        auto hold = [&]
                        {
                            e.grabStop = 0;
                            e.grabKey = key;
                            e.grabX = L.x - 0.2f;
                            e.grabY = L.y;
                            e.grabZ = L.z;
                        };
                        double l = 0, r = 0;
                        Vec3f at{};
                        play(
                            e,
                            [&](EngineParams &p)
                            {
                                hall(p);
                                p.motionMode[0] = (int)Motion::Hover;
                                p.live.whisper = 1.0f;
                            },
                            {60}, 2.5, 3, 100, -1.0,
                            [&](int pos)
                            {
                                hold();
                                e.bodyAt(0, key, at);
                                if (pos < (int)fsr || e.nearBus(0) == nullptr)
                                    return;
                                for (int i = 0; i < B; ++i)
                                {
                                    l += (double)e.nearBus(0)[i] * e.nearBus(0)[i];
                                    r += (double)e.nearBus(1)[i] * e.nearBus(1)[i];
                                }
                            });
                        const double diff = 10.0 * std::log10(juce::jmax(1e-30, l)) - 10.0 * std::log10(juce::jmax(1e-30, r));
                        std::cout << "Fluestern: a carried pipe, " << std::sqrt((at.x - L.x) * (at.x - L.x) + (at.y - L.y) * (at.y - L.y) + (at.z - L.z) * (at.z - L.z))
                                  << " m from the head, to the left: left - right " << diff << " dB" << std::endl;
                        check(diff > 10.0, "Fluestern: a carried pipe at the ear");
                    }
                }
                {
                    // Unmoeglicher Raum: the late reverb of a four times larger room rings longer;
                    // near a wall it grows
                    auto ring = [&](float hear, float pull, float listenX, float &scale)
                    {
                        OrganEngine e;
                        const auto b = play(
                            e,
                            [&](EngineParams &p)
                            {
                                hall(p);
                                p.motionMode[0] = 0;
                                p.room.tailGain = 1.0f;
                                p.room.tailModel = 1;
                                p.live.roomHear = hear;
                                p.live.wallPull = pull;
                                p.live.listenX = listenX;
                            },
                            {60}, 5.0, 1, 100, 2.0);
                        scale = e.heard[0].tailScale.load();
                        return rmsDb(b, 3.5, 5.0);
                    };
                    float s1 = 0, s4 = 0, sWall = 0;
                    const double small = ring(1.0f, 0.0f, 0.0f, s1), large = ring(4.0f, 0.0f, 0.0f, s4);
                    ring(1.0f, 1.0f, 6.5f, sWall);
                    std::cout << "Unmoeglicher Raum: 1.5 - 3 s after the note: as seen " << small
                              << " dB, four times the size " << large << " dB (scale " << s1 << " / "
                              << s4 << "); half a metre from a wall: scale " << sWall << std::endl;
                    check(s1 == 1.0f && std::abs(s4 - 4.0f) < 0.01f && large > small + 6.0,
                          "Unmoeglicher Raum: a larger room is heard than the one seen");
                    check(sWall > 2.5f, "Unmoeglicher Raum: the reverb grows near a wall");
                }
                {
                    // Zeit als Entfernung: the memory. In the present nothing changes; a second
                    // back is a second back; getting there does not click
                    TimeDistance td(2, fsr);
                    const int total = (int)(6.0 * fsr), blk = 256;
                    std::vector<float> a((size_t)total), b((size_t)total), in((size_t)total);
                    for (int i = 0; i < total; ++i)
                        in[(size_t)i] = a[(size_t)i] = b[(size_t)i] =
                            (float)std::sin(juce::MathConstants<double>::twoPi * 220.0 * i / fsr);
                    bool same = true;
                    double worst = 0.0, err = 0.0;
                    for (int pos = 0; pos + blk <= total; pos += blk)
                    {
                        float *io[2]{a.data() + pos, b.data() + pos};
                        const double t = pos / fsr;
                        td.process(io, 2, blk, true, t < 1.0 ? 0.0f : (t < 4.0 ? 1.0f : 0.0f));
                        if (t < 0.9)
                            for (int i = 0; i < blk; ++i)
                                same = same && a[(size_t)(pos + i)] == in[(size_t)(pos + i)];
                        if (t > 3.0 && t < 3.9)
                            for (int i = 0; i < blk; ++i)
                                err = juce::jmax(err, (double)std::abs(a[(size_t)(pos + i)] -
                                                                       in[(size_t)(pos + i) - (size_t)fsr]));
                    }
                    for (int i = 2; i < total; ++i)
                        worst = juce::jmax(worst, (double)std::abs(a[(size_t)i] - 2.0f * a[(size_t)i - 1] +
                                                                   a[(size_t)i - 2]));
                    bool back = true;
                    for (int i = total - 2000; i < total - 300; ++i)
                        back = back && a[(size_t)i] == in[(size_t)i];
                    std::cout << "Zeit als Entfernung: a second back, error " << err
                              << "; sharpest second difference on the way " << worst
                              << " (a 220 Hz sine has 0.0008)" << std::endl;
                    check(same, "Zeit als Entfernung: in the present nothing changes");
                    check(err < 0.01, "Zeit als Entfernung: a second back is a second back");
                    check(worst < 0.06, "Zeit als Entfernung: walking through time does not click");
                    check(back, "Zeit als Entfernung: back in the present, exactly");
                }
            }
            // Verschleiss: a pipe held for half its life is half worn, and sounds different; reset
            // makes it new
            {
                OrganEngine e, en;
                auto wearOn = [](EngineParams &p)
                {
                    p.live.wear = 1.0f;
                    p.live.wearMinutes = 0.1f;
                };
                play(e, wearOn, {60}, 3.0, 0);
                const float w = e.wear[0][(size_t)keyIndexForMidi(60)].load(),
                            other = e.wear[0][(size_t)keyIndexForMidi(62)].load();
                // the same engine again (prepare keeps the wear): the worn pipe against a new one
                const auto worn = play(e, wearOn, {60}, 2.0, 0);
                const auto fresh = play(en, [](EngineParams &) {}, {60}, 2.0, 0);
                double d = 0, r = 0;
                for (int i = (int)(0.5 * fsr); i < worn.getNumSamples(); ++i)
                {
                    const double t = worn.getSample(0, i) - fresh.getSample(0, i);
                    d += t * t;
                    r += (double)fresh.getSample(0, i) * fresh.getSample(0, i);
                }
                std::cout << "wear: after 3 s of a 6 s life " << w
                          << " (a pipe not played: " << other << "); worn against new "
                          << 10.0 * std::log10(d / r) << " dB" << std::endl;
                check(w > 0.45f && w < 0.55f && other == 0.0f, "wear: a pipe ages while it sounds");
                check(10.0 * std::log10(d / r) > -20.0 && finiteB(worn),
                      "wear: the worn pipe sounds different");
                e.wearReset = true;
                play(e, [](EngineParams &) {}, {}, 0.1, 0);
                check(e.wear[0][(size_t)keyIndexForMidi(60)].load() == 0.0f,
                      "wear: reset makes all pipes new");
                writeWav(outDir.getChildFile("83_verschleiss.wav"), worn, fsr);
            }
            // sympathetic pipes: silent pipes ring along (more with them than without), and stop
            // some time after the note
            {
                OrganEngine e, e0;
                int ringing = 0;
                const auto with = play(
                    e,
                    [](EngineParams &p)
                    {
                        p.live.sympLevel = 2.0f;
                        p.live.sympDecay = 0.8f;
                    },
                    {60}, 4.0, 1, 100, 1.5,
                    [&](int pos)
                    {
                        if (pos < (int)(1.4 * fsr))
                            ringing = juce::jmax(ringing, e.sympNow.load());
                    });
                const auto without = play(e0, [](EngineParams &) {}, {60}, 4.0, 1, 100, 1.5);
                double d = 0, r = 0;
                for (int i = (int)(0.5 * fsr); i < (int)(1.4 * fsr); ++i)
                {
                    const double t = with.getSample(0, i) - without.getSample(0, i);
                    d += t * t;
                    r += (double)without.getSample(0, i) * without.getSample(0, i);
                }
                const double rel = 10.0 * std::log10(juce::jmax(1.0e-30, d) / r);
                std::cout << "sympathetic pipes: " << ringing << " ring along, " << rel
                          << " dB against the pipe; still ringing at the end: " << e.sympNow.load()
                          << std::endl;
                check(ringing >= 3 && rel > -50.0 && rel < 0.0 && finiteB(with),
                      "sympathetic pipes ring along");
                check(e.sympNow.load() == 0, "sympathetic pipes fall silent after the note");
                writeWav(outDir.getChildFile("84_mitklingen.wav"), with, fsr);
            }
            // Reinheit: the meantone fifth c'-g' (beating at about 2.5 Hz) pulls itself pure; the
            // wolf is tamed too
            {
                auto beatOf = [&](std::vector<int> notes, float pure, float &cents)
                {
                    OrganEngine e;
                    play(
                        e,
                        [&](EngineParams &p)
                        {
                            p.motionMode[0] = (int)Motion::Beat;
                            p.live.pure = pure;
                            p.live.pureTime = 0.4f;
                        },
                        notes, 3.0, 1);
                    cents = e.pureNow.load();
                    return e.beatMaxNow.load();
                };
                float c0, c1, cw;
                const float b0 = beatOf({60, 67}, 0.0f, c0), b1 = beatOf({60, 67}, 1.0f, c1),
                            w0 = beatOf({56, 63}, 0.0f, c0), w1 = beatOf({56, 63}, 1.0f, cw);
                std::cout << "pure tuning: fifth c'-g' beats " << b0 << " -> " << b1
                          << " Hz (moved " << c1 << " ct), wolf " << w0 << " -> " << w1
                          << " Hz (moved " << cw << " ct)" << std::endl;
                check(b0 > 1.5f && b1 < 0.3f && c1 > 1.0f && c1 < 5.0f,
                      "pure tuning: the fifth stops beating");
                check(w1 < 0.25f * w0 && cw > 10.0f, "pure tuning: the wolf is tamed");
            }
            // Tonnetz walker: one key held, the walker goes along the fifths and sounds other keys;
            // all ends after the key is let go
            {
                OrganEngine e;
                uint64_t seen = 0;
                const auto b = play(
                    e,
                    [](EngineParams &p)
                    {
                        p.live.netOn = true;
                        p.live.netPath = 0;
                        p.live.netRate = 8.0f;
                        p.live.netGate = 0.6f;
                    },
                    {60}, 4.0, 0, 100, 2.0, [&](int) { seen |= e.soundingMask[0].load(); });
                int keys = 0;
                for (int k = 0; k < kNumKeys; ++k)
                    keys += (seen >> k) & 1 ? 1 : 0;
                const bool g = (seen >> keyIndexForMidi(67)) & 1,
                           d = ((seen >> keyIndexForMidi(62)) & 1) ||
                               ((seen >> keyIndexForMidi(74)) & 1);
                std::cout << "Tonnetz walker (fifths from c'): " << keys << " keys sounded, g' "
                          << g << ", d " << d << "; voices left at the end "
                          << e.activeVoices.load() << std::endl;
                check(keys >= 8 && g && d && finiteB(b), "Tonnetz walker: walks along the fifths");
                check(e.activeVoices.load() == 0, "Tonnetz walker: stops when the keys are let go");
                writeWav(outDir.getChildFile("85_tonnetz_wanderer.wav"), b, fsr);
            }
            // the same in free tuning (every tuning but the original one plays note by note): the
            // walker must walk there too
            {
                OrganEngine e;
                uint64_t seen = 0;
                const auto b = play(
                    e,
                    [](EngineParams &p)
                    {
                        p.correctSamplePitch = true;
                        p.live.netOn = true;
                        p.live.netPath = 0;
                        p.live.netRate = 8.0f;
                        p.live.netGate = 0.6f;
                    },
                    {60}, 4.0, 0, 100, 2.0, [&](int) { seen |= e.soundingMask[0].load(); });
                int keys = 0;
                for (int k = 0; k < kNumKeys; ++k)
                    keys += (seen >> k) & 1 ? 1 : 0;
                const bool g = (seen >> keyIndexForMidi(67)) & 1;
                std::cout << "Tonnetz walker in free tuning: " << keys << " keys sounded, g' " << g
                          << "; voices left at the end " << e.activeVoices.load() << std::endl;
                check(keys >= 8 && g && finiteB(b), "Tonnetz walker: walks in free tuning too");
                check(e.activeVoices.load() == 0,
                      "Tonnetz walker: stops in free tuning when the keys are let go");
            }
        }
    }

    std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string(failures)) << std::endl;
    return failures;
}
