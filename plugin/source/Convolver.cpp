#include "Convolver.h"
#include "Spatial.h"

namespace rysum
{
namespace
{
inline int acnDegree(int c)
{
    return (int)std::floor(std::sqrt((double)c) + 1.0e-9);
}
inline int acnOrderM(int c)
{
    const int l = acnDegree(c);
    return c - l * l - l;
} // -l .. l
inline int acnIndex(int l, int m)
{
    return l * l + l + m;
}

/** turns an AmbiX IR about the vertical axis by phi (degrees, positive = to the left) */
void rotateYaw(juce::AudioBuffer<float> &b, int order, float phiDeg)
{
    const int len = b.getNumSamples();
    for (int l = 1; l <= order; ++l)
        for (int m = 1; m <= l; ++m)
        {
            const int cp = acnIndex(l, m), cn = acnIndex(l, -m);
            if (cp >= b.getNumChannels() || cn >= b.getNumChannels())
                continue;
            const float c = std::cos(juce::degreesToRadians(phiDeg) * (float)m),
                        s = std::sin(juce::degreesToRadians(phiDeg) * (float)m);
            float *a = b.getWritePointer(cp);
            float *d = b.getWritePointer(cn);
            for (int i = 0; i < len; ++i)
            {
                const float x = a[i], y = d[i];
                a[i] = x * c - y * s; // cos part
                d[i] = x * s + y * c; // sin part
            }
        }
}

/** random-phase copy of a signal's time-frequency envelope (STFT 1024 / hop 256) */
std::vector<float> diffuseCopy(const float *x, int len, uint32_t seed)
{
    constexpr int N = 1024, H = 256;
    juce::dsp::FFT f(10);
    std::vector<float> out((size_t)len + N, 0.0f), buf((size_t)N * 2), win((size_t)N);
    for (int i = 0; i < N; ++i)
        win[(size_t)i] =
            0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * (float)i / (float)N);
    XorShift rng(seed);
    for (int start = -N + H; start < len; start += H)
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < N; ++i)
        {
            const int j = start + i;
            buf[(size_t)i] = (j >= 0 && j < len) ? x[j] * win[(size_t)i] : 0.0f;
        }
        f.performRealOnlyForwardTransform(buf.data(), true);
        for (int b = 0; b <= N / 2; ++b)
        {
            const float mag = std::hypot(buf[(size_t)(2 * b)], buf[(size_t)(2 * b + 1)]);
            const float ph = juce::MathConstants<float>::twoPi * rng.uni();
            buf[(size_t)(2 * b)] = mag * std::cos(ph);
            buf[(size_t)(2 * b + 1)] = (b == 0 || b == N / 2) ? 0.0f : mag * std::sin(ph);
        }
        f.performRealOnlyInverseTransform(buf.data());
        for (int i = 0; i < N; ++i)
        {
            const int j = start + i;
            if (j >= 0 && j < len)
                out[(size_t)j] +=
                    buf[(size_t)i] * win[(size_t)i] / 1.5f; // Hann^2 at 75 % overlap sums to 1.5
        }
    }
    out.resize((size_t)len);
    return out;
}

double energy(const float *x, int n)
{
    double e = 0;
    for (int i = 0; i < n; ++i)
        e += (double)x[i] * x[i];
    return e;
}
} // namespace

std::unique_ptr<Convolver> Convolver::create(const juce::File &file, const Settings &s,
                                             juce::String &error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(file));
    if (r == nullptr)
    {
        error = "Datei nicht lesbar: " + file.getFileName();
        return nullptr;
    }

    const int ch = (int)r->numChannels;
    if (ch < 1 || ch > 64)
    {
        error = juce::String::fromUTF8("zu viele Kan\xc3\xa4le");
        return nullptr;
    }
    const int srcLen =
        (int)std::min<juce::int64>(r->lengthInSamples, (juce::int64)(kMaxSeconds * r->sampleRate));
    if (srcLen < 16)
    {
        error = "Datei zu kurz";
        return nullptr;
    }

    juce::AudioBuffer<float> src(ch, srcLen);
    r->read(&src, 0, srcLen, 0, true, true);

    // ---- channel format -> AmbiX (ACN / SN3D)
    int order = -1;
    for (int o = 1; o <= 7; ++o)
        if ((o + 1) * (o + 1) == ch)
            order = o;
    int keep = ch;
    if (order >= 0)
    {
        order = juce::jmin(order, kMaxOrder);
        keep = (order + 1) * (order + 1);
    }
    else if (ch > 2)
        keep = 2; // unknown multichannel: first two as stereo

    juce::AudioBuffer<float> ir(keep, srcLen);
    if (order == 1 && s.format == Format::FuMa)
    {
        // FuMa W X Y Z  ->  AmbiX W Y Z X, W scaled by sqrt(2)
        ir.copyFrom(0, 0, src, 0, 0, srcLen);
        ir.applyGain(0, 0, srcLen, std::sqrt(2.0f));
        ir.copyFrom(1, 0, src, 2, 0, srcLen);
        ir.copyFrom(2, 0, src, 3, 0, srcLen);
        ir.copyFrom(3, 0, src, 1, 0, srcLen);
    }
    else
    {
        for (int c = 0; c < keep; ++c)
            ir.copyFrom(c, 0, src, c, 0, srcLen);
        if (order >= 1 && s.format == Format::N3D)
            for (int c = 1; c < keep; ++c)
                ir.applyGain(c, 0, srcLen, 1.0f / std::sqrt((float)(2 * acnDegree(c) + 1)));
    }

    // ---- sample-rate conversion to the host rate
    juce::AudioBuffer<float> rs;
    const double ratio = r->sampleRate / s.hostRate;
    if (std::abs(ratio - 1.0) > 1.0e-6)
    {
        const int outLen = (int)std::ceil(srcLen / ratio);
        rs.setSize(keep, outLen);
        for (int c = 0; c < keep; ++c)
        {
            juce::LagrangeInterpolator li;
            li.process(ratio, ir.getReadPointer(c), rs.getWritePointer(c), outLen, srcLen, 0);
        }
    }
    else
        rs = ir;

    // ---- trim trailing silence, normalise: energy of the omni (first) channel = 1
    int len = rs.getNumSamples();
    const float peak = rs.getMagnitude(0, len);
    if (peak <= 0.0f)
    {
        error = "Impulsantwort ist still";
        return nullptr;
    }
    while (len > kBlock && rs.getMagnitude(len - 256, 256) < peak * 1.0e-4f)
        len -= 256;
    {
        const double e = energy(rs.getReadPointer(0), len);
        const float g = e > 0 ? (float)(1.0 / std::sqrt(e)) : 1.0f;
        // a response that still sounds where it ends (a file longer than kMaxSeconds, cut there):
        // a long smooth fade instead of a cut; one that has died away: a short one
        const bool loudEnd = rs.getMagnitude(len - juce::jmin(len, 2048), juce::jmin(len, 2048)) >
                             peak * 1.0e-3f;
        const int fade = loudEnd ? juce::jmin(len / 4, (int)(2.0 * s.hostRate))
                                 : juce::jmin(len / 4, (int)(0.05 * s.hostRate));
        for (int c = 0; c < keep; ++c)
        {
            rs.applyGain(c, 0, len, g);
            if (!loudEnd)
                rs.applyGainRamp(c, len - fade, fade, 1.0f, 0.0f);
            else
            {
                // (cosine squared: the level falls ever faster, no corner where it starts)
                float *d = rs.getWritePointer(c);
                for (int i = 0; i < fade; ++i)
                {
                    const float w = 0.5f + 0.5f * std::cos(juce::MathConstants<float>::pi *
                                                           (float)i / (float)fade);
                    d[len - fade + i] *= w * w;
                }
            }
        }
    }

    std::unique_ptr<Convolver> cv(new Convolver());
    cv->settings = s;
    auto &in = cv->info;
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
        cv->partsEarly = 0;
        cv->lateFirst = 0;
        late.setSize(2, len);
        for (int c = 0; c < 2; ++c)
            late.copyFrom(c, 0, rs, c, 0, len);
        in.numInputs = 1;
        in.harmonics = 0;
    }
    else
    {
        const int Lo = juce::jlimit(1, kMaxOrder, s.stereoOut ? 1 : s.outOrder);
        outCh = (Lo + 1) * (Lo + 1);
        const int Li = order >= 0 ? juce::jmin(order, Lo) : -1;

        // omni reference (mono IR: the file; stereo IR: mid)
        std::vector<float> w((size_t)len);
        for (int i = 0; i < len; ++i)
            w[(size_t)i] =
                keep == 2 ? 0.5f * (rs.getSample(0, i) + rs.getSample(1, i)) : rs.getSample(0, i);

        // direct sound: the strongest omni peak; its direction from the first-order intensity
        int td = 0;
        {
            float best = 0;
            for (int i = 0; i < len; ++i)
                if (std::abs(w[(size_t)i]) > best)
                {
                    best = std::abs(w[(size_t)i]);
                    td = i;
                }
        }
        float az0 = 0.0f;
        if (Li >= 1)
        {
            const int a = juce::jmax(0, td - (int)(0.0005 * s.hostRate)),
                      b = juce::jmin(len, td + (int)(0.002 * s.hostRate));
            double sx = 0, sy = 0;
            for (int i = a; i < b; ++i)
            {
                sy += (double)w[(size_t)i] * rs.getSample(1, i);
                sx += (double)w[(size_t)i] * rs.getSample(3, i);
            }
            az0 = (float)juce::radiansToDegrees(std::atan2(sy, sx));
        }
        in.baseAzDeg = az0;

        // base field A: measured IR turned so the direct sound is at azimuth 0, or (mono / stereo
        // IR) the omni IR as a point source at azimuth 0 for the early part
        juce::AudioBuffer<float> A(outCh, len);
        A.clear();
        if (Li >= 0)
        {
            for (int c = 0; c < (Li + 1) * (Li + 1); ++c)
                A.copyFrom(c, 0, rs, c, 0, len);
            rotateYaw(A, Li, -az0);
        }
        else
        {
            std::array<float, kMaxAmbiChannels> y{};
            encodeAmbiX(0.0f, 0.0f, Lo, y.data());
            for (int c = 0; c < outCh; ++c)
                A.copyFrom(c, 0, w.data(), len, y[(size_t)c]);
        }

        // direct sound of the IR turned down (the dry pipes already are the direct sound)
        {
            const int a = td - (int)(0.001 * s.hostRate), b = td + (int)(0.003 * s.hostRate),
                      ramp = juce::jmax(2, (int)(0.0005 * s.hostRate));
            for (int i = juce::jmax(0, a - ramp); i < juce::jmin(len, b + ramp); ++i)
            {
                float k = 1.0f;
                if (i < a)
                    k = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * (float)(a - i) /
                                               (float)ramp);
                else if (i > b)
                    k = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * (float)(i - b) /
                                               (float)ramp);
                else
                    k = 0.0f;
                const float gg = k + (1.0f - k) * s.direct; // k = 1 outside, 0 inside the window
                for (int c = 0; c < outCh; ++c)
                    A.setSample(c, i, A.getSample(c, i) * gg);
                w[(size_t)i] *= gg;
            }
        }

        // early / late split: ~80 ms after the direct sound, crossfaded over one partition
        const int mix = td + (int)(0.080 * s.hostRate);
        cv->partsEarly = juce::jlimit(1, P, mix / kBlock + 1);
        cv->lateFirst = cv->partsEarly - 1;
        const int xf0 = cv->lateFirst * kBlock, xf1 = juce::jmin(len, xf0 + kBlock);
        auto fadeE = [&](int i)
        {
            if (cv->partsEarly >= P)
                return 1.0f;
            if (i < xf0)
                return 1.0f;
            if (i >= xf1)
                return 0.0f;
            const float t = (float)(i - xf0) / (float)(xf1 - xf0);
            return std::cos(0.5f * juce::MathConstants<float>::pi * t);
        };

        early.setSize(outCh, len);
        for (int c = 0; c < outCh; ++c)
            for (int i = 0; i < len; ++i)
                early.setSample(c, i, A.getSample(c, i) * fadeE(i));

        // late: W as measured, the other channels blended with an isotropic diffuse field
        late.setSize(outCh, len);
        late.clear();
        if (cv->partsEarly < P)
        {
            const float e = juce::jlimit(0.0f, 1.0f, s.envelop);
            const int L0 = xf0, LN = len - xf0;
            std::vector<float> wl((size_t)LN);
            for (int i = 0; i < LN; ++i)
                wl[(size_t)i] = w[(size_t)(L0 + i)];
            const double eW = energy(wl.data(), LN);
            for (int c = 0; c < outCh; ++c)
            {
                const int l = acnDegree(c);
                std::vector<float> lc((size_t)LN, 0.0f);
                if (c == 0)
                    lc = wl;
                else
                {
                    const bool measured = Li >= 1 && l <= Li;
                    if (measured) // as recorded (not turned): with 'follow' off the room is exactly
                                  // the measured one
                        for (int i = 0; i < LN; ++i)
                            lc[(size_t)i] = std::sqrt(1.0f - e) * rs.getSample(c, L0 + i);
                    if (e > 0.0f)
                    {
                        auto d =
                            diffuseCopy(wl.data(), LN, (uint32_t)(0x9e3779b9u * (uint32_t)(c + 1)));
                        const double ed = energy(d.data(), LN);
                        const float target =
                            (float)std::sqrt(eW / (double)(2 * l + 1) / juce::jmax(1.0e-20, ed));
                        const float k = std::sqrt(e) * target;
                        for (int i = 0; i < LN; ++i)
                            lc[(size_t)i] += k * d[(size_t)i];
                    }
                }
                for (int i = 0; i < LN; ++i)
                    late.setSample(c, L0 + i, lc[(size_t)i] * (1.0f - fadeE(L0 + i)));
            }
        }
        in.numInputs = 2 * Lo + 1;
        in.harmonics = Lo;

        // output links: (l, m>0) = h(l,m)*C_m - h(l,-m)*S_m ; (l, -m) = h(l,m)*S_m + h(l,-m)*C_m
        cv->links.resize((size_t)outCh);
        cv->numLinks.assign((size_t)outCh, 0);
        for (int c = 0; c < outCh; ++c)
        {
            const int l = acnDegree(c), m = acnOrderM(c);
            auto &L = cv->links[(size_t)c];
            if (m == 0)
            {
                L[0] = {0, c, 1.0f};
                cv->numLinks[(size_t)c] = 1;
            }
            else if (m > 0)
            {
                L[0] = {2 * m - 1, c, 1.0f};
                L[1] = {2 * m, acnIndex(l, -m), -1.0f};
                cv->numLinks[(size_t)c] = 2;
            }
            else
            {
                const int a = -m;
                L[0] = {2 * a, acnIndex(l, a), 1.0f};
                L[1] = {2 * a - 1, c, 1.0f};
                cv->numLinks[(size_t)c] = 2;
            }
        }
    }

    cv->numIn = in.numInputs;
    cv->numOut = outCh;

    // ---- partition + FFT (PFFFT order, scaled by 1 / N)
    // stage A (block 512): partition p = IR samples [512 p, 512 p + 512). It runs one block late
    // (as before: the wet latency is one block), i.e. a lag-1 stage of the response delayed by 512.
    // stages B / C: the tail after 7 * 512 samples, delayed by the same block: g(t) = late(t -
    // 512), partition q of a stage with block b covers g[(q + 2) b, (q + 3) b) - stage B from 4096
    // (= late from 3584), stage C from 16384.
    auto partitionInto = [&](const juce::AudioBuffer<float> &b, int B, int segStart, int numParts,
                             AlignedFloats &dst)
    {
        // segment q of channel c: b[segStart + q B, segStart + (q + 1) B)
        const int N = 2 * B;
        RealFFT f(N);
        AlignedFloats tmp((size_t)N);
        dst.assign((size_t)outCh * (size_t)juce::jmax(1, numParts) * (size_t)N);
        for (int c = 0; c < outCh; ++c)
            for (int q = 0; q < numParts; ++q)
            {
                tmp.clear();
                const int start = segStart + q * B;
                for (int i = 0; i < B; ++i)
                {
                    const int t = start + i;
                    if (t >= 0 && t < len)
                        tmp[(size_t)i] = b.getSample(c, t);
                }
                f.forwardScaled(tmp.data(),
                                dst.data() +
                                    ((size_t)c * (size_t)numParts + (size_t)q) * (size_t)N);
            }
    };
    const int lateStart = cv->lateFirst * kBlock; // the late buffer is silent before this
    const bool anyLate = cv->partsEarly < P;
    // stage A: early part, and the tail's partitions before 7 * 512
    if (cv->partsEarly > 0)
        partitionInto(early, kBlock, 0, cv->partsEarly, cv->earlySpec);
    const int nA = anyLate ? juce::jmax(0, juce::jmin(P, kTailA) - cv->lateFirst) : 0;
    if (nA > 0)
        partitionInto(late, kBlock, lateStart, nA, cv->lateSpecA);
    // stages B and C (partitions that are entirely before the late part starts are left out)
    const int bStart = kTailA * kBlock;      // 3584 = 2 * 2048 - 512
    const int cStart = 2 * kBlockC - kBlock; // 15872
    int qB0 = 0, nB = 0, qC0 = 0, nC = 0;
    if (anyLate && len > bStart)
    {
        qB0 = juce::jmax(0, (lateStart - bStart) / kBlockB);
        const int qEnd =
            juce::jmin((cStart - bStart) / kBlockB, (len - bStart + kBlockB - 1) / kBlockB);
        nB = juce::jmax(0, qEnd - qB0);
        if (nB > 0)
            partitionInto(late, kBlockB, bStart + qB0 * kBlockB, nB, cv->lateSpecB);
    }
    if (anyLate && len > cStart)
    {
        qC0 = juce::jmax(0, (lateStart - cStart) / kBlockC);
        const int qEnd = (len - cStart + kBlockC - 1) / kBlockC;
        nC = juce::jmax(0, qEnd - qC0);
        if (nC > 0)
            partitionInto(late, kBlockC, cStart + qC0 * kBlockC, nC, cv->lateSpecC);
    }

    // ---- the convolution stages and their terms
    using Term = PartitionedConvolution::Term;
    const int depthA = juce::jmax(1, juce::jmax(cv->partsEarly, nA > 0 ? cv->lateFirst + nA : 0));
    cv->convA.prepare(kBlock, 1, cv->numIn, depthA, outCh, 3);
    cv->hasB = nB > 0;
    cv->hasC = nC > 0;
    if (cv->hasB)
        cv->convB.prepare(kBlockB, 2, 1, qB0 + nB, outCh, 1);
    if (cv->hasC)
        cv->convC.prepare(kBlockC, 2, 1, qC0 + nC, outCh, 1);
    for (int c = 0; c < outCh; ++c)
    {
        Term t[3];
        int nt = 0;
        if (cv->partsEarly > 0 && !cv->links.empty())
            for (int li = 0; li < cv->numLinks[(size_t)c]; ++li)
            {
                const auto &L = cv->links[(size_t)c][(size_t)li];
                t[nt].input = L.input;
                t[nt].spectra = cv->earlySpec.data() +
                                (size_t)L.irChannel * (size_t)cv->partsEarly * (size_t)kFft;
                t[nt].first = 0;
                t[nt].parts = cv->partsEarly;
                t[nt].negate = L.sign < 0.0f;
                ++nt;
            }
        if (nA > 0)
        {
            t[nt].input = 0;
            t[nt].spectra = cv->lateSpecA.data() + (size_t)c * (size_t)nA * (size_t)kFft;
            t[nt].first = cv->lateFirst;
            t[nt].parts = nA;
            t[nt].negate = false;
            ++nt;
        }
        cv->convA.setTerms(c, t, nt);
        if (cv->hasB)
        {
            Term tb;
            tb.input = 0;
            tb.first = qB0;
            tb.parts = nB;
            tb.spectra = cv->lateSpecB.data() + (size_t)c * (size_t)nB * (size_t)(2 * kBlockB);
            cv->convB.setTerms(c, &tb, 1);
        }
        if (cv->hasC)
        {
            Term tc;
            tc.input = 0;
            tc.first = qC0;
            tc.parts = nC;
            tc.spectra = cv->lateSpecC.data() + (size_t)c * (size_t)nC * (size_t)(2 * kBlockC);
            cv->convC.setTerms(c, &tc, 1);
        }
    }
    cv->reset(); // (also applies the terms)

    if (order >= 0)
        in.description << "AmbiX " << order << ". Ord."
                       << (s.format == Format::FuMa && order == 1
                               ? " (aus FuMa)"
                               : (s.format == Format::N3D ? " (N3D)" : ""));
    else
        in.description << (keep == 1 ? "Mono" : "Stereo");
    in.description << juce::String::fromUTF8(" \xc2\xb7 ") << juce::String(in.seconds, 1) << " s";
    return cv;
}

void Convolver::reset() noexcept
{
    convA.reset();
    if (hasB)
        convB.reset();
    if (hasC)
        convC.reset();
}

void Convolver::process(const float *const *inputs, int n, float *const *out, int nOut,
                        float wetGain) noexcept
{
    const bool stereo = settings.stereoOut;
    int done = 0;
    while (done < n)
    {
        int len = juce::jmin(n - done, convA.toBoundary());
        if (hasB)
            len = juce::jmin(len, convB.toBoundary());
        if (hasC)
            len = juce::jmin(len, convC.toBoundary());
        const int pA = convA.position(), pB = hasB ? convB.position() : 0,
                  pC = hasC ? convC.position() : 0;

        // dst += g * (output of channel c), the three stages summed
        auto addOut = [&](int c, float *dst, float g)
        {
            const float *a = convA.output(c) + pA;
            if (hasB && hasC)
            {
                const float *b = convB.output(c) + pB;
                const float *d = convC.output(c) + pC;
                for (int i = 0; i < len; ++i)
                    dst[i] += g * (a[i] + b[i] + d[i]);
            }
            else if (hasB)
            {
                const float *b = convB.output(c) + pB;
                for (int i = 0; i < len; ++i)
                    dst[i] += g * (a[i] + b[i]);
            }
            else
                for (int i = 0; i < len; ++i)
                    dst[i] += g * a[i];
        };

        if (info.legacyStereo)
        {
            addOut(0, out[0] + done, wetGain);
            if (nOut > 1)
                addOut(1, out[1] + done, wetGain);
        }
        else if (stereo)
        {
            // first-order field to stereo: two virtual cardioids pointing left / right
            const float h = 0.5f * wetGain;
            addOut(0, out[0] + done, h);
            if (numOut > 1)
                addOut(1, out[0] + done, h);
            if (nOut > 1)
            {
                addOut(0, out[1] + done, h);
                if (numOut > 1)
                    addOut(1, out[1] + done, -h);
            }
        }
        else
        {
            const int m = juce::jmin(numOut, nOut);
            for (int c = 0; c < m; ++c)
                addOut(c, out[c] + done, wetGain);
        }

        const float *ins[2 * kMaxOrder + 1]{};
        for (int k = 0; k < numIn; ++k)
            ins[k] = inputs[k] + done;
        convA.push(ins, len);
        if (hasB)
            convB.push(ins, len);
        if (hasC)
            convC.push(ins, len);
        done += len;
    }
}
} // namespace rysum
