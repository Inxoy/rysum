#include "Convolver.h"
#include "Spatial.h"

namespace rysum
{
namespace
{
    inline int acnDegree (int c) { return (int) std::floor (std::sqrt ((double) c) + 1.0e-9); }
    inline int acnOrderM (int c) { const int l = acnDegree (c); return c - l * l - l; }       // -l .. l
    inline int acnIndex (int l, int m) { return l * l + l + m; }

    /** turns an AmbiX IR about the vertical axis by phi (degrees, positive = to the left) */
    void rotateYaw (juce::AudioBuffer<float>& b, int order, float phiDeg)
    {
        const int len = b.getNumSamples();
        for (int l = 1; l <= order; ++l)
            for (int m = 1; m <= l; ++m)
            {
                const int cp = acnIndex (l, m), cn = acnIndex (l, -m);
                if (cp >= b.getNumChannels() || cn >= b.getNumChannels()) continue;
                const float c = std::cos (juce::degreesToRadians (phiDeg) * (float) m), s = std::sin (juce::degreesToRadians (phiDeg) * (float) m);
                float* a = b.getWritePointer (cp);
                float* d = b.getWritePointer (cn);
                for (int i = 0; i < len; ++i)
                {
                    const float x = a[i], y = d[i];
                    a[i] = x * c - y * s;      // cos part
                    d[i] = x * s + y * c;      // sin part
                }
            }
    }

    /** random-phase copy of a signal's time-frequency envelope (STFT 1024 / hop 256) */
    std::vector<float> diffuseCopy (const float* x, int len, uint32_t seed)
    {
        constexpr int N = 1024, H = 256;
        juce::dsp::FFT f (10);
        std::vector<float> out ((size_t) len + N, 0.0f), buf ((size_t) N * 2), win ((size_t) N);
        for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) N);
        XorShift rng (seed);
        for (int start = -N + H; start < len; start += H)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < N; ++i)
            {
                const int j = start + i;
                buf[(size_t) i] = (j >= 0 && j < len) ? x[j] * win[(size_t) i] : 0.0f;
            }
            f.performRealOnlyForwardTransform (buf.data(), true);
            for (int b = 0; b <= N / 2; ++b)
            {
                const float mag = std::hypot (buf[(size_t) (2 * b)], buf[(size_t) (2 * b + 1)]);
                const float ph = juce::MathConstants<float>::twoPi * rng.uni();
                buf[(size_t) (2 * b)] = mag * std::cos (ph);
                buf[(size_t) (2 * b + 1)] = (b == 0 || b == N / 2) ? 0.0f : mag * std::sin (ph);
            }
            f.performRealOnlyInverseTransform (buf.data());
            for (int i = 0; i < N; ++i)
            {
                const int j = start + i;
                if (j >= 0 && j < len) out[(size_t) j] += buf[(size_t) i] * win[(size_t) i] / 1.5f;   // Hann^2 at 75 % overlap sums to 1.5
            }
        }
        out.resize ((size_t) len);
        return out;
    }

    double energy (const float* x, int n) { double e = 0; for (int i = 0; i < n; ++i) e += (double) x[i] * x[i]; return e; }
}

std::unique_ptr<Convolver> Convolver::create (const juce::File& file, const Settings& s, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
    if (r == nullptr) { error = "Datei nicht lesbar: " + file.getFileName(); return nullptr; }

    const int ch = (int) r->numChannels;
    if (ch < 1 || ch > 64) { error = juce::String::fromUTF8 ("zu viele Kan\xc3\xa4le"); return nullptr; }
    const int srcLen = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) (kMaxSeconds * r->sampleRate));
    if (srcLen < 16) { error = "Datei zu kurz"; return nullptr; }

    juce::AudioBuffer<float> src (ch, srcLen);
    r->read (&src, 0, srcLen, 0, true, true);

    // ---- channel format -> AmbiX (ACN / SN3D)
    int order = -1;
    for (int o = 1; o <= 7; ++o) if ((o + 1) * (o + 1) == ch) order = o;
    int keep = ch;
    if (order >= 0) { order = juce::jmin (order, kMaxOrder); keep = (order + 1) * (order + 1); }
    else if (ch > 2) keep = 2;                       // unknown multichannel: first two as stereo

    juce::AudioBuffer<float> ir (keep, srcLen);
    if (order == 1 && s.format == Format::FuMa)
    {
        // FuMa W X Y Z  ->  AmbiX W Y Z X, W scaled by sqrt(2)
        ir.copyFrom (0, 0, src, 0, 0, srcLen); ir.applyGain (0, 0, srcLen, std::sqrt (2.0f));
        ir.copyFrom (1, 0, src, 2, 0, srcLen);
        ir.copyFrom (2, 0, src, 3, 0, srcLen);
        ir.copyFrom (3, 0, src, 1, 0, srcLen);
    }
    else
    {
        for (int c = 0; c < keep; ++c) ir.copyFrom (c, 0, src, c, 0, srcLen);
        if (order >= 1 && s.format == Format::N3D)
            for (int c = 1; c < keep; ++c) ir.applyGain (c, 0, srcLen, 1.0f / std::sqrt ((float) (2 * acnDegree (c) + 1)));
    }

    // ---- sample-rate conversion to the host rate
    juce::AudioBuffer<float> rs;
    const double ratio = r->sampleRate / s.hostRate;
    if (std::abs (ratio - 1.0) > 1.0e-6)
    {
        const int outLen = (int) std::ceil (srcLen / ratio);
        rs.setSize (keep, outLen);
        for (int c = 0; c < keep; ++c)
        {
            juce::LagrangeInterpolator li;
            li.process (ratio, ir.getReadPointer (c), rs.getWritePointer (c), outLen, srcLen, 0);
        }
    }
    else rs = ir;

    // ---- trim trailing silence, normalise: energy of the omni (first) channel = 1
    int len = rs.getNumSamples();
    const float peak = rs.getMagnitude (0, len);
    if (peak <= 0.0f) { error = "Impulsantwort ist still"; return nullptr; }
    while (len > kBlock && rs.getMagnitude (len - 256, 256) < peak * 1.0e-4f) len -= 256;
    {
        const double e = energy (rs.getReadPointer (0), len);
        const float g = e > 0 ? (float) (1.0 / std::sqrt (e)) : 1.0f;
        const int fade = juce::jmin (len / 4, (int) (0.05 * s.hostRate));
        for (int c = 0; c < keep; ++c) { rs.applyGain (c, 0, len, g); rs.applyGainRamp (c, len - fade, fade, 1.0f, 0.0f); }
    }

    std::unique_ptr<Convolver> cv (new Convolver());
    cv->settings = s;
    auto& in = cv->info;
    in.fileName = file.getFileName();
    in.channels = ch;
    in.order = order;
    in.seconds = len / s.hostRate;
    cv->partsTotal = (len + kBlock - 1) / kBlock;
    const int P = cv->partsTotal;

    // the two sets of channels that get partitioned: early (rotated per pipe) and late (all pipes)
    juce::AudioBuffer<float> early, late;
    int outCh = 0;

    if (keep == 2 && s.stereoOut)
    {
        // stereo IR, stereo output: as recorded, L and R
        in.legacyStereo = true;
        outCh = 2;
        cv->partsEarly = 0; cv->lateFirst = 0;
        late.setSize (2, len);
        for (int c = 0; c < 2; ++c) late.copyFrom (c, 0, rs, c, 0, len);
        in.numInputs = 1; in.harmonics = 0;
    }
    else
    {
        const int Lo = juce::jlimit (1, kMaxOrder, s.stereoOut ? 1 : s.outOrder);
        outCh = (Lo + 1) * (Lo + 1);
        const int Li = order >= 0 ? juce::jmin (order, Lo) : -1;

        // omni reference (mono IR: the file; stereo IR: mid)
        std::vector<float> w ((size_t) len);
        for (int i = 0; i < len; ++i) w[(size_t) i] = keep == 2 ? 0.5f * (rs.getSample (0, i) + rs.getSample (1, i)) : rs.getSample (0, i);

        // direct sound: the strongest omni peak; its direction from the first-order intensity
        int td = 0;
        { float best = 0; for (int i = 0; i < len; ++i) if (std::abs (w[(size_t) i]) > best) { best = std::abs (w[(size_t) i]); td = i; } }
        float az0 = 0.0f;
        if (Li >= 1)
        {
            const int a = juce::jmax (0, td - (int) (0.0005 * s.hostRate)), b = juce::jmin (len, td + (int) (0.002 * s.hostRate));
            double sx = 0, sy = 0;
            for (int i = a; i < b; ++i) { sy += (double) w[(size_t) i] * rs.getSample (1, i); sx += (double) w[(size_t) i] * rs.getSample (3, i); }
            az0 = (float) juce::radiansToDegrees (std::atan2 (sy, sx));
        }
        in.baseAzDeg = az0;

        // base field A: measured IR turned so the direct sound is at azimuth 0, or (mono / stereo IR)
        // the omni IR as a point source at azimuth 0 for the early part
        juce::AudioBuffer<float> A (outCh, len);
        A.clear();
        if (Li >= 0)
        {
            for (int c = 0; c < (Li + 1) * (Li + 1); ++c) A.copyFrom (c, 0, rs, c, 0, len);
            rotateYaw (A, Li, -az0);
        }
        else
        {
            std::array<float, kMaxAmbiChannels> y {};
            encodeAmbiX (0.0f, 0.0f, Lo, y.data());
            for (int c = 0; c < outCh; ++c) A.copyFrom (c, 0, w.data(), len, y[(size_t) c]);
        }

        // direct sound of the IR turned down (the dry pipes already are the direct sound)
        {
            const int a = td - (int) (0.001 * s.hostRate), b = td + (int) (0.003 * s.hostRate), ramp = juce::jmax (2, (int) (0.0005 * s.hostRate));
            for (int i = juce::jmax (0, a - ramp); i < juce::jmin (len, b + ramp); ++i)
            {
                float k = 1.0f;
                if (i < a)      k = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (a - i) / (float) ramp);
                else if (i > b) k = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (i - b) / (float) ramp);
                else            k = 0.0f;
                const float gg = k + (1.0f - k) * s.direct;   // k = 1 outside, 0 inside the window
                for (int c = 0; c < outCh; ++c) A.setSample (c, i, A.getSample (c, i) * gg);
                w[(size_t) i] *= gg;
            }
        }

        // early / late split: ~80 ms after the direct sound, crossfaded over one partition
        const int mix = td + (int) (0.080 * s.hostRate);
        cv->partsEarly = juce::jlimit (1, P, mix / kBlock + 1);
        cv->lateFirst = cv->partsEarly - 1;
        const int xf0 = cv->lateFirst * kBlock, xf1 = juce::jmin (len, xf0 + kBlock);
        auto fadeE = [&] (int i)
        {
            if (cv->partsEarly >= P) return 1.0f;
            if (i < xf0) return 1.0f;
            if (i >= xf1) return 0.0f;
            const float t = (float) (i - xf0) / (float) (xf1 - xf0);
            return std::cos (0.5f * juce::MathConstants<float>::pi * t);
        };

        early.setSize (outCh, len);
        for (int c = 0; c < outCh; ++c)
            for (int i = 0; i < len; ++i) early.setSample (c, i, A.getSample (c, i) * fadeE (i));

        // late: W as measured, the other channels blended with an isotropic diffuse field
        late.setSize (outCh, len);
        late.clear();
        if (cv->partsEarly < P)
        {
            const float e = juce::jlimit (0.0f, 1.0f, s.envelop);
            const int L0 = xf0, LN = len - xf0;
            std::vector<float> wl ((size_t) LN);
            for (int i = 0; i < LN; ++i) wl[(size_t) i] = w[(size_t) (L0 + i)];
            const double eW = energy (wl.data(), LN);
            for (int c = 0; c < outCh; ++c)
            {
                const int l = acnDegree (c);
                std::vector<float> lc ((size_t) LN, 0.0f);
                if (c == 0) lc = wl;
                else
                {
                    const bool measured = Li >= 1 && l <= Li;
                    if (measured)      // as recorded (not turned): with 'follow' off the room is exactly the measured one
                        for (int i = 0; i < LN; ++i) lc[(size_t) i] = std::sqrt (1.0f - e) * rs.getSample (c, L0 + i);
                    if (e > 0.0f)
                    {
                        auto d = diffuseCopy (wl.data(), LN, (uint32_t) (0x9e3779b9u * (uint32_t) (c + 1)));
                        const double ed = energy (d.data(), LN);
                        const float target = (float) std::sqrt (eW / (double) (2 * l + 1) / juce::jmax (1.0e-20, ed));
                        const float k = std::sqrt (e) * target;
                        for (int i = 0; i < LN; ++i) lc[(size_t) i] += k * d[(size_t) i];
                    }
                }
                for (int i = 0; i < LN; ++i) late.setSample (c, L0 + i, lc[(size_t) i] * (1.0f - fadeE (L0 + i)));
            }
        }
        in.numInputs = 2 * Lo + 1;
        in.harmonics = Lo;

        // output links: (l, m>0) = h(l,m)*C_m - h(l,-m)*S_m ; (l, -m) = h(l,m)*S_m + h(l,-m)*C_m
        cv->links.resize ((size_t) outCh);
        cv->numLinks.assign ((size_t) outCh, 0);
        for (int c = 0; c < outCh; ++c)
        {
            const int l = acnDegree (c), m = acnOrderM (c);
            auto& L = cv->links[(size_t) c];
            if (m == 0)     { L[0] = { 0, c, 1.0f }; cv->numLinks[(size_t) c] = 1; }
            else if (m > 0) { L[0] = { 2 * m - 1, c, 1.0f }; L[1] = { 2 * m, acnIndex (l, -m), -1.0f }; cv->numLinks[(size_t) c] = 2; }
            else            { const int a = -m; L[0] = { 2 * a, acnIndex (l, a), 1.0f }; L[1] = { 2 * a - 1, c, 1.0f }; cv->numLinks[(size_t) c] = 2; }
        }
    }

    cv->numIn = in.numInputs;
    cv->numOut = outCh;

    // ---- partition + FFT
    std::vector<float> tmp ((size_t) kFft * 2, 0.0f);
    auto partition = [&] (const juce::AudioBuffer<float>& b, int p0, int p1, std::vector<float>& re, std::vector<float>& im)
    {
        const int np = juce::jmax (0, p1 - p0);
        re.assign ((size_t) outCh * (size_t) np * kBins, 0.0f);
        im.assign ((size_t) outCh * (size_t) np * kBins, 0.0f);
        for (int c = 0; c < outCh; ++c)
            for (int p = p0; p < p1; ++p)
            {
                std::fill (tmp.begin(), tmp.end(), 0.0f);
                const int start = p * kBlock, n = juce::jmin (kBlock, len - start);
                for (int i = 0; i < n; ++i) tmp[(size_t) i] = b.getSample (c, start + i);
                cv->fft.performRealOnlyForwardTransform (tmp.data(), true);
                const size_t base = ((size_t) c * (size_t) np + (size_t) (p - p0)) * kBins;
                for (int k = 0; k < kBins; ++k) { re[base + (size_t) k] = tmp[(size_t) (2 * k)]; im[base + (size_t) k] = tmp[(size_t) (2 * k + 1)]; }
            }
    };
    if (cv->partsEarly > 0) partition (early, 0, cv->partsEarly, cv->earlyRe, cv->earlyIm);
    partition (late, cv->lateFirst, P, cv->lateRe, cv->lateIm);

    cv->fdlRe.resize ((size_t) cv->numIn); cv->fdlIm.resize ((size_t) cv->numIn);
    cv->fdlSize.assign ((size_t) cv->numIn, 0); cv->fdlPos.assign ((size_t) cv->numIn, 0);
    for (int k = 0; k < cv->numIn; ++k)
    {
        const int slots = k == 0 ? P : juce::jmax (1, cv->partsEarly);
        cv->fdlSize[(size_t) k] = slots;
        cv->fdlRe[(size_t) k].assign ((size_t) slots * kBins, 0.0f);
        cv->fdlIm[(size_t) k].assign ((size_t) slots * kBins, 0.0f);
    }
    cv->inBuf.assign ((size_t) cv->numIn * kFft, 0.0f);
    cv->work.assign ((size_t) kFft * 2, 0.0f);
    cv->accRe.assign ((size_t) kBins, 0.0f);
    cv->accIm.assign ((size_t) kBins, 0.0f);
    cv->outBuf.assign ((size_t) outCh * kBlock, 0.0f);

    if (order >= 0) in.description << "AmbiX " << order << ". Ord." << (s.format == Format::FuMa && order == 1 ? " (aus FuMa)" : (s.format == Format::N3D ? " (N3D)" : ""));
    else in.description << (keep == 1 ? "Mono" : "Stereo");
    in.description << juce::String::fromUTF8 (" \xc2\xb7 ") << juce::String (in.seconds, 1) << " s";
    return cv;
}

void Convolver::reset() noexcept
{
    for (auto& v : fdlRe) std::fill (v.begin(), v.end(), 0.0f);
    for (auto& v : fdlIm) std::fill (v.begin(), v.end(), 0.0f);
    std::fill (inBuf.begin(), inBuf.end(), 0.0f);
    std::fill (outBuf.begin(), outBuf.end(), 0.0f);
    pos = 0;
}

void Convolver::processBlock() noexcept
{
    // input spectra of [previous block | current block] for every send
    for (int k = 0; k < numIn; ++k)
    {
        float* ib = inBuf.data() + (size_t) k * kFft;
        std::copy (ib, ib + kFft, work.begin());
        std::fill (work.begin() + kFft, work.end(), 0.0f);
        fft.performRealOnlyForwardTransform (work.data(), true);
        auto& p = fdlPos[(size_t) k];
        const int size = fdlSize[(size_t) k];
        p = (p + size - 1) % size;
        float* xr = fdlRe[(size_t) k].data() + (size_t) p * kBins;
        float* xi = fdlIm[(size_t) k].data() + (size_t) p * kBins;
        for (int b = 0; b < kBins; ++b) { xr[b] = work[(size_t) (2 * b)]; xi[b] = work[(size_t) (2 * b + 1)]; }
        std::copy (ib + kBlock, ib + kFft, ib);           // current block becomes "previous"
    }

    const int nLate = partsTotal - lateFirst;
    for (int c = 0; c < numOut; ++c)
    {
        std::fill (accRe.begin(), accRe.end(), 0.0f);
        std::fill (accIm.begin(), accIm.end(), 0.0f);
        float* ar = accRe.data();
        float* ai = accIm.data();
        auto mac = [&] (int input, int part, const float* hr, const float* hi, float sign)
        {
            const int size = fdlSize[(size_t) input];
            const int slot = (fdlPos[(size_t) input] + part) % size;
            const float* sr = fdlRe[(size_t) input].data() + (size_t) slot * kBins;
            const float* si = fdlIm[(size_t) input].data() + (size_t) slot * kBins;
            if (sign > 0)
                for (int b = 0; b < kBins; ++b) { ar[b] += sr[b] * hr[b] - si[b] * hi[b]; ai[b] += sr[b] * hi[b] + si[b] * hr[b]; }
            else
                for (int b = 0; b < kBins; ++b) { ar[b] -= sr[b] * hr[b] - si[b] * hi[b]; ai[b] -= sr[b] * hi[b] + si[b] * hr[b]; }
        };
        // early: rotated per pipe (1 or 2 sends)
        if (partsEarly > 0 && ! links.empty())
            for (int li = 0; li < numLinks[(size_t) c]; ++li)
            {
                const auto& L = links[(size_t) c][(size_t) li];
                for (int p = 0; p < partsEarly; ++p)
                {
                    const size_t base = ((size_t) L.irChannel * (size_t) partsEarly + (size_t) p) * kBins;
                    mac (L.input, p, earlyRe.data() + base, earlyIm.data() + base, L.sign);
                }
            }
        // late: all pipes together
        for (int p = 0; p < nLate; ++p)
        {
            const size_t base = ((size_t) c * (size_t) nLate + (size_t) p) * kBins;
            mac (0, lateFirst + p, lateRe.data() + base, lateIm.data() + base, 1.0f);
        }
        for (int b = 0; b < kBins; ++b) { work[(size_t) (2 * b)] = ar[b]; work[(size_t) (2 * b + 1)] = ai[b]; }
        std::fill (work.begin() + 2 * kBins, work.end(), 0.0f);
        fft.performRealOnlyInverseTransform (work.data());
        std::copy (work.begin() + kBlock, work.begin() + kFft, outBuf.begin() + (size_t) c * kBlock);
    }
}

void Convolver::process (const float* const* inputs, int n, float* const* out, int nOut, float wetGain) noexcept
{
    const bool stereo = settings.stereoOut;
    for (int i = 0; i < n; ++i)
    {
        for (int k = 0; k < numIn; ++k) inBuf[(size_t) k * kFft + (size_t) (kBlock + pos)] = inputs[k][i];

        auto y = [this] (int c) { return outBuf[(size_t) c * kBlock + (size_t) pos]; };
        if (info.legacyStereo)
        {
            out[0][i] += y (0) * wetGain;
            if (nOut > 1) out[1][i] += y (1) * wetGain;
        }
        else if (stereo)
        {
            // first-order field to stereo: two virtual cardioids pointing left / right
            const float w = y (0), l = numOut > 1 ? y (1) : 0.0f;
            out[0][i] += 0.5f * (w + l) * wetGain;
            if (nOut > 1) out[1][i] += 0.5f * (w - l) * wetGain;
        }
        else
        {
            const int m = juce::jmin (numOut, nOut);
            for (int c = 0; c < m; ++c) out[c][i] += y (c) * wetGain;
        }

        if (++pos == kBlock) { pos = 0; processBlock(); }
    }
}
} // namespace rysum
