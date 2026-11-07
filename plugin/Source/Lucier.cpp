#include "Lucier.h"
#include <complex>

namespace rysum
{
namespace
{
using cpx = std::complex<double>;

int orderFor (int n) { int o = 0; while ((1 << o) < n) ++o; return o; }

/** forward real FFT of x (zero-padded to 2^order) -> bins 0..N/2 */
std::vector<cpx> spectrum (const std::vector<float>& x, int order)
{
    const int N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) (2 * N), 0.0f);
    std::copy_n (x.begin(), juce::jmin ((int) x.size(), N), buf.begin());
    fft.performRealOnlyForwardTransform (buf.data(), true);
    std::vector<cpx> X ((size_t) (N / 2 + 1));
    for (int k = 0; k <= N / 2; ++k) X[(size_t) k] = { buf[(size_t) (2 * k)], buf[(size_t) (2 * k + 1)] };
    return X;
}

/** inverse of spectrum(): the first 'len' samples */
std::vector<float> signal (const std::vector<cpx>& X, int order, int len)
{
    const int N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) (2 * N), 0.0f);
    for (int k = 0; k <= N / 2; ++k) { buf[(size_t) (2 * k)] = (float) X[(size_t) k].real(); buf[(size_t) (2 * k + 1)] = (float) X[(size_t) k].imag(); }
    fft.performRealOnlyInverseTransform (buf.data());
    return std::vector<float> (buf.begin(), buf.begin() + juce::jmin (len, N));
}

void fadeOut (std::vector<float>& h, float part)
{
    const int n = (int) h.size(), f = juce::jmax (1, (int) ((float) n * part));
    for (int i = 0; i < f; ++i)
        h[(size_t) (n - f + i)] *= 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) i / (float) f);
}

void normaliseEnergy (std::vector<float>& h)
{
    double e = 0.0;
    for (auto v : h) e += (double) v * v;
    if (e > 0.0) { const float g = (float) (1.0 / std::sqrt (e)); for (auto& v : h) v *= g; }
}

/** loudspeaker + microphone of every pass: about 60 Hz .. 10 kHz (magnitude) */
double transducers (double f)
{
    const double hp = std::pow (f / 60.0, 2.0), lp = f / 10000.0;
    return std::sqrt (hp * hp / (1.0 + hp * hp)) / std::sqrt (1.0 + lp * lp);
}
} // namespace

// ---------------------------------------------------------------------------
std::shared_ptr<const LucierBase> lucierFromRoom (const RoomParams& r, float air, double sr)
{
    auto b = std::make_shared<LucierBase>();
    b->sampleRate = sr;
    const int maxLen = TailIR::maxLength (sr);
    const int order = orderFor (2 * maxLen), N = 1 << order;
    const double df = sr / N;
    const float rtL = eyringRt (r, false, air), rtH = eyringRt (r, true, air);
    const float V = roomVolume (r);

    // ---- diffuse field: noise with the decay of the two bands (as the late reverb, but its own seed)
    const int len = juce::jmin (maxLen, (int) std::ceil (1.3 * juce::jmax (rtL, rtH) * sr) + 256);
    std::vector<float> d ((size_t) len, 0.0f);
    {
        XorShift rng (0x1c1e7u);
        juce::IIRFilter lpF;
        lpF.setCoefficients (juce::IIRCoefficients::makeLowPass (sr, 1500.0));
        const double kL = 6.9078 / (rtL * sr), kH = 6.9078 / (rtH * sr);
        for (int t = 0; t < len; ++t)
        {
            const float nse = 0.866f * (rng.bi() + rng.bi() + rng.bi() + rng.bi());
            const float lo = lpF.processSingleSampleRaw (nse), hi = nse - lo;
            d[(size_t) t] = (float) (lo * std::exp (-kL * t) + hi * std::exp (-kH * t));
        }
        fadeOut (d, 0.1f);
    }
    auto H = spectrum (d, order);

    // ---- eigenmodes of the shoebox below the crossover (strongest where Lucier's room sang)
    const double c = kSpeedOfSoundRoom;
    double Lx = r.W(), Ly = r.L(), Lz = r.H();
    auto rcv = listenerPos (r);
    auto src = roomPoint (r, { 0.0f, 10.0f, 6.0f });                // the organ, in front of the listener
    if (! r.box())
    {
        // a polygon room: the modes of a shoebox with the same floor area (the same mode density; the exact
        // modes of an arbitrary plan would need a wave solver)
        const float k = std::sqrt (juce::jmax (0.05f, planGeo (r).area / (r.W() * r.L())));
        Lx *= k; Ly *= k;
        rcv.x *= k; rcv.y *= k; src.x *= k; src.y *= k;
    }
    double fc = juce::jlimit (60.0, 400.0, 2.0 * 2000.0 * std::sqrt (rtL / V));
    auto countModes = [&] (double f) { return 4.19 * V * std::pow (f / c, 3.0) + 0.785 * 2.0 * (Lx * Ly + Ly * Lz + Lx * Lz) * std::pow (f / c, 2.0) / 2.0 + (Lx + Ly + Lz) * f / (2.0 * c); };
    while (fc > 40.0 && countModes (1.3 * fc) > 4000.0) fc *= 0.9;
    std::vector<cpx> M (H.size(), cpx (0.0, 0.0));
    const double fmax = 1.3 * fc;
    const double delta = 6.9078 / rtL;                                        // amplitude decay rate (1/s)
    const double bw = juce::jmax (3.0, 12.0 * delta / juce::MathConstants<double>::twoPi);
    const int nxMax = (int) (2.0 * fmax * Lx / c), nyMax = (int) (2.0 * fmax * Ly / c), nzMax = (int) (2.0 * fmax * Lz / c);
    const double pi = juce::MathConstants<double>::pi;
    int modes = 0;
    for (int nx = 0; nx <= nxMax; ++nx)
        for (int ny = 0; ny <= nyMax; ++ny)
            for (int nz = 0; nz <= nzMax; ++nz)
            {
                if (nx + ny + nz == 0) continue;
                const double fm = 0.5 * c * std::sqrt (std::pow (nx / Lx, 2.0) + std::pow (ny / Ly, 2.0) + std::pow (nz / Lz, 2.0));
                if (fm > fmax) continue;
                ++modes;
                const double eps = (nx > 0 ? 2.0 : 1.0) * (ny > 0 ? 2.0 : 1.0) * (nz > 0 ? 2.0 : 1.0);
                const double A = eps * std::cos (nx * pi * src.x / Lx) * std::cos (nx * pi * rcv.x / Lx)
                                     * std::cos (ny * pi * src.y / Ly) * std::cos (ny * pi * rcv.y / Ly)
                                     * std::cos (nz * pi * src.z / Lz) * std::cos (nz * pi * rcv.z / Lz);
                const double wm = 2.0 * pi * fm;
                const int k0 = juce::jmax (1, (int) ((fm - bw) / df)), k1 = juce::jmin ((int) M.size() - 1, (int) ((fm + bw) / df) + 1);
                for (int k = k0; k <= k1; ++k)
                {
                    const double w = 2.0 * pi * k * df;
                    M[(size_t) k] += cpx (0.0, w) * A / cpx (wm * wm - w * w, 2.0 * delta * w);
                }
            }
    // level of the modes = level of the diffuse field around the crossover; blend between them
    auto bandEnergy = [&] (const std::vector<cpx>& X, double f0, double f1)
    {
        double e = 0.0; int n = 0;
        for (int k = (int) (f0 / df); k <= (int) (f1 / df) && k < (int) X.size(); ++k) { e += std::norm (X[(size_t) k]); ++n; }
        return n > 0 ? e / n : 0.0;
    };
    const double em = bandEnergy (M, 0.5 * fc, fc), ed = bandEnergy (H, fc, 2.0 * fc);
    const double gm = em > 0.0 ? std::sqrt (ed / em) : 0.0;
    for (size_t k = 0; k < H.size(); ++k)
    {
        const double f = (double) k * df;
        double w = f < 0.8 * fc ? 1.0 : (f > 1.25 * fc ? 0.0 : 0.5 + 0.5 * std::cos (pi * (f - 0.8 * fc) / (0.45 * fc)));
        H[k] = w * gm * M[k] + (1.0 - w) * H[k];
    }
    b->h = signal (H, order, maxLen);
    fadeOut (b->h, 0.1f);
    normaliseEnergy (b->h);
    b->what = "Saal " + juce::String (r.W(), 0) + juce::String::fromUTF8 (" \xc3\x97 ") + juce::String (r.L(), 0) + juce::String::fromUTF8 (" \xc3\x97 ")
            + juce::String (r.H(), 0) + " m, " + juce::String (modes) + " Eigenmoden bis " + juce::String (juce::roundToInt (fc)) + " Hz";
    return b;
}

std::shared_ptr<const LucierBase> lucierFromFile (const juce::File& file, double sr, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr) { error = "Impulsantwort nicht lesbar"; return nullptr; }
    const int maxLen = TailIR::maxLength (sr);
    const int srcLen = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (maxLen * reader->sampleRate / sr) + 1024);
    juce::AudioBuffer<float> in ((int) reader->numChannels, juce::jmax (1, srcLen));
    reader->read (&in, 0, srcLen, 0, true, true);
    // first channel (W of an Ambisonics response, left of a stereo one); from just before the direct sound
    const float* x = in.getReadPointer (0);
    float peak = 0.0f;
    for (int i = 0; i < srcLen; ++i) peak = juce::jmax (peak, std::abs (x[i]));
    if (peak <= 0.0f) { error = "Impulsantwort ist still"; return nullptr; }
    int start = 0;
    while (start < srcLen && std::abs (x[start]) < 0.1f * peak) ++start;
    start = juce::jmax (0, start - (int) (0.002 * reader->sampleRate));
    auto b = std::make_shared<LucierBase>();
    b->sampleRate = sr;
    const double ratio = reader->sampleRate / sr;
    const int outLen = juce::jmin (maxLen, (int) ((srcLen - start) / ratio));
    b->h.assign ((size_t) juce::jmax (1, outLen), 0.0f);
    if (std::abs (ratio - 1.0) < 1.0e-9) std::copy_n (x + start, outLen, b->h.begin());
    else
    {
        juce::LagrangeInterpolator li;
        li.process (ratio, x + start, b->h.data(), outLen);
    }
    fadeOut (b->h, 0.1f);
    normaliseEnergy (b->h);
    b->what = file.getFileNameWithoutExtension() + " (" + juce::String ((double) outLen / sr, 1) + " s)";
    return b;
}

// ---------------------------------------------------------------------------
LucierResult buildLucier (const LucierBase& base, float generation, float tau, uint32_t serial)
{
    LucierResult res;
    const double sr = base.sampleRate;
    const int maxLen = TailIR::maxLength (sr);
    const int order = orderFor (2 * maxLen), N = 1 << order;
    const double df = sr / N;
    const float gen = juce::jlimit (1.0f, 60.0f, generation);
    res.generation = gen;
    const int n0 = (int) std::floor (gen);
    const float frac = gen - (float) n0;
    const int n1 = frac > 0.004f ? n0 + 1 : n0;

    // every pass damped, more for later generations: the swell stays within a few seconds
    const double tauEff = juce::jmin ((double) tau, 2.5 / juce::jmax (1.0, (double) gen - 1.0));
    std::vector<float> hd (base.h);
    if (hd.size() > (size_t) maxLen) hd.resize ((size_t) maxLen);
    const double k = 1.0 / (tauEff * sr);
    for (size_t i = 0; i < hd.size(); ++i) hd[i] *= (float) std::exp (-k * (double) i);
    auto H = spectrum (hd, order);

    // one pass = room x loudspeaker x microphone, unit rms in the audible band
    const int kLo = (int) (60.0 / df), kHi = juce::jmin ((int) H.size() - 1, (int) (10000.0 / df));
    double e = 0.0;
    for (size_t i = 0; i < H.size(); ++i)
    {
        H[i] *= transducers (juce::jmax (1.0, (double) i * df));
        if ((int) i >= kLo && (int) i <= kHi) e += std::norm (H[i]);
    }
    const double rms = std::sqrt (e / juce::jmax (1, kHi - kLo + 1));
    if (rms > 0.0) for (auto& v : H) v /= rms;

    // n passes: H^n (whole numbers: magnitude^n, phase*n - no unwrapping needed), each scaled between
    // "same peak" and "same energy" (the resonance does not explode, the rest does not vanish)
    auto power = [&] (int n)
    {
        std::vector<cpx> G (H.size());
        double pk = 0.0, en = 0.0;
        for (size_t i = 0; i < H.size(); ++i)
        {
            G[i] = std::polar (std::pow (std::abs (H[i]), (double) n), std::arg (H[i]) * n);
            if ((int) i >= kLo && (int) i <= kHi) { pk = juce::jmax (pk, std::abs (G[i])); en += std::norm (G[i]); }
        }
        const double r = std::sqrt (en / juce::jmax (1, kHi - kLo + 1));
        const double g = pk > 0.0 && r > 0.0 ? 1.0 / std::sqrt (pk * r) : 0.0;
        for (auto& v : G) v *= g;
        return G;
    };
    auto G = power (n0);
    if (n1 != n0)
    {
        const auto G1 = power (n1);
        for (size_t i = 0; i < G.size(); ++i) G[i] = (1.0 - frac) * G[i] + (double) frac * G1[i];
    }

    // for the display: the response, and its strongest resonances
    res.curve.assign ((size_t) LucierResult::kCurvePoints, -120.0f);
    float top = -200.0f;
    for (int p = 0; p < LucierResult::kCurvePoints; ++p)
    {
        const double f0 = LucierResult::kCurveLo * std::pow ((double) LucierResult::kCurveHi / LucierResult::kCurveLo, (double) p / LucierResult::kCurvePoints);
        const double f1 = LucierResult::kCurveLo * std::pow ((double) LucierResult::kCurveHi / LucierResult::kCurveLo, (double) (p + 1) / LucierResult::kCurvePoints);
        double m = 0.0;
        for (int i = (int) (f0 / df); i <= (int) (f1 / df) && i < (int) G.size(); ++i) m = juce::jmax (m, std::abs (G[(size_t) i]));
        res.curve[(size_t) p] = (float) juce::Decibels::gainToDecibels (m, -200.0);
        top = juce::jmax (top, res.curve[(size_t) p]);
    }
    for (auto& v : res.curve) v -= top;
    {
        // peaks: local maxima of |G| at least 1/6 octave apart
        std::vector<std::pair<double, double>> cand;
        for (int i = kLo + 1; i < kHi; ++i)
        {
            const double a = std::abs (G[(size_t) i]);
            if (a > std::abs (G[(size_t) i - 1]) && a >= std::abs (G[(size_t) i + 1])) cand.push_back ({ a, i * df });
        }
        std::sort (cand.begin(), cand.end(), [] (auto& x, auto& y) { return x.first > y.first; });
        for (auto& [a, f] : cand)
        {
            bool near = false;
            for (auto q : res.peaks) near = near || std::abs (std::log2 (f / q)) < 1.0 / 6.0;
            if (! near) res.peaks.push_back ((float) f);
            if (res.peaks.size() >= 5 || a < 0.05 * cand.front().first) break;
        }
    }

    auto h = signal (G, order, maxLen);
    fadeOut (h, 0.08f);
    std::vector<std::vector<float>> one { std::move (h) };
    res.ir = TailIR::fromResponses (one, sr, serial);
    return res;
}

// ---------------------------------------------------------------------------
// the piece: whole recordings through the room, pass after pass
// ---------------------------------------------------------------------------
std::vector<float> lucierPassResponse (const LucierBase& base)
{
    const double sr = base.sampleRate;
    std::vector<float> h (base.h);
    const int maxLen = TailIR::maxLength (sr);
    if ((int) h.size() > maxLen) { h.resize ((size_t) maxLen); fadeOut (h, 0.05f); }
    const int len = (int) h.size();
    const int order = orderFor (2 * juce::jmax (64, len)), N = 1 << order;
    const double df = sr / N;
    auto H = spectrum (h, order);
    const int kLo = (int) (60.0 / df), kHi = juce::jmin ((int) H.size() - 1, (int) (10000.0 / df));
    double e = 0.0;
    for (size_t i = 0; i < H.size(); ++i)
    {
        H[i] *= transducers (juce::jmax (1.0, (double) i * df));
        if ((int) i >= kLo && (int) i <= kHi) e += std::norm (H[i]);
    }
    const double rms = std::sqrt (e / juce::jmax (1, kHi - kLo + 1));
    if (rms > 0.0) for (auto& v : H) v /= rms;
    auto out = signal (H, order, len);
    fadeOut (out, 0.03f);
    return out;
}

std::vector<float> lucierPass (const std::vector<float>& x, const std::vector<float>& pass, int maxLen, std::function<bool()> shouldStop)
{
    const int lx = (int) x.size(), lh = (int) pass.size();
    if (lx == 0 || lh == 0) return {};
    const int outLen = juce::jmin (maxLen, lx + lh - 1);
    const int order = juce::jmax (15, orderFor (2 * lh)), N = 1 << order;
    const int B = N - lh + 1;                                   // input samples per block
    juce::dsp::FFT fft (order);
    std::vector<float> Hb ((size_t) (2 * N), 0.0f), buf ((size_t) (2 * N), 0.0f);
    std::copy (pass.begin(), pass.end(), Hb.begin());
    fft.performRealOnlyForwardTransform (Hb.data(), true);
    std::vector<float> y ((size_t) outLen, 0.0f);
    const float scale = 1.0f;                                   // (JUCE's inverse transform is normalised)
    for (int start = 0; start < lx && start < outLen; start += B)
    {
        if (shouldStop && shouldStop()) return {};
        std::fill (buf.begin(), buf.end(), 0.0f);
        const int m = juce::jmin (B, lx - start);
        std::copy_n (x.begin() + start, m, buf.begin());
        fft.performRealOnlyForwardTransform (buf.data(), true);
        for (int k = 0; k <= N / 2; ++k)
        {
            const float ar = buf[(size_t) (2 * k)], ai = buf[(size_t) (2 * k + 1)];
            const float br = Hb[(size_t) (2 * k)], bi = Hb[(size_t) (2 * k + 1)];
            buf[(size_t) (2 * k)] = ar * br - ai * bi;
            buf[(size_t) (2 * k + 1)] = ar * bi + ai * br;
        }
        fft.performRealOnlyInverseTransform (buf.data());
        const int n = juce::jmin (N, outLen - start);
        for (int i = 0; i < n; ++i) y[(size_t) (start + i)] += buf[(size_t) i] * scale;
    }
    return y;
}
} // namespace rysum
