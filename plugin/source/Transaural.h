#pragma once

#include "Common.h"
#include "Binaural.h"
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <complex>
#include <memory>
#include <vector>

namespace rysum
{
/** "Transaural": the binaural signal over two loudspeakers, with crosstalk cancellation.

    Each ear hears both speakers. The speaker on the far side reaches an ear later (the interaural time
    difference) and quieter (the head shadow): per frequency the ratio R = far ear / near ear. With the
    two speakers symmetric to the listener the 2x2 speaker-to-ear matrix is Hn * [[1, R], [R, 1]], and its
    inverse splits into a sum and a difference filter:

        mid  = (L + R) / 2   ->  Cs = 1 / (1 + R)
        side = (L - R) / 2   ->  Cd = 1 / (1 - R)        speaker left = mid' + side', right = mid' - side'

    R is taken from the HRTF in use (FABIAN or the own SOFA file) at the speakers' angle: the measured
    complex ratio with the ITD (a pure delay) taken out, smoothed over 1/12 octave (real and imaginary part:
    the sound bends around the head, its delay to the far ear changes with frequency - a pure delay alone
    cancels only ~6 dB), the delay put back. The inversion is regularised (Cx = conj X /
    (|X|^2 + beta)), so no frequency is boosted by more than 'maxBoostDb' - at low frequencies 1 - R gets
    small (the head is small against the wavelength), the full inverse would boost the side signal there
    without limit. Below ~80 Hz and above ~12 kHz the filters fade to 'plain' (cancellation does not work
    there anyway: the ears are too close in wavelengths / a head is never exactly where it was measured).

    Optionally ('sideEq') the colour of the speakers' own direction (the near-ear HRTF at the angle, 1/3
    octave smoothed, at most +-10 dB) is taken out as well, so the ears get the binaural signal and not
    binaural x side HRTF.

    amount: 0 = plain binaural over the speakers (only the filters' delay), 1 = full cancellation.
    The filters are delayed by 'delay' samples (about 5 ms) so their non-causal part fits in. */
struct TransauralFilters
{
    double sampleRate = 48000.0;
    int length = 0, delay = 0;
    std::vector<float> cs, cd;               // sum and difference filters
    float angle = 90.0f, itdMs = 0.0f;
    float crossPlainDb = 0.0f;               // crosstalk without cancellation (far / near ear energy, 250 Hz .. 6 kHz)
    float crossDb = 0.0f;                    // with these filters (measured against the unsmoothed HRTF)
    juce::String description;

    static std::shared_ptr<const TransauralFilters> design (const HrirSet& set, float angleDeg, float amount, float maxBoostDb,
                                                            bool sideEq, double hostRate)
    {
        if (set.numDirections() < 2 || set.length < 8) return {};
        auto out = std::make_shared<TransauralFilters>();
        out->sampleRate = hostRate;
        out->angle = angleDeg;
        // the HRIRs nearest to the two speakers (AmbiX: +azimuth = left)
        auto nearest = [&] (float az, float el)
        {
            const float a = juce::degreesToRadians (az), e = juce::degreesToRadians (el);
            const float x = std::cos (e) * std::cos (a), y = std::cos (e) * std::sin (a), z = std::sin (e);
            int best = 0; float bd = -2.0f;
            for (int d = 0; d < set.numDirections(); ++d)
            {
                const float a2 = juce::degreesToRadians (set.az[(size_t) d]), e2 = juce::degreesToRadians (set.el[(size_t) d]);
                const float dot = x * std::cos (e2) * std::cos (a2) + y * std::cos (e2) * std::sin (a2) + z * std::sin (e2);
                if (dot > bd) { bd = dot; best = d; }
            }
            return best;
        };
        const int dl = nearest (angleDeg, 0.0f), dr = nearest (-angleDeg, 0.0f);
        // near / far ear of the left speaker: left / right ear; of the right speaker: right / left ear
        const float* nearL = set.get (dl, 0); const float* farL = set.get (dl, 1);
        const float* nearR = set.get (dr, 1); const float* farR = set.get (dr, 0);

        // ---- spectra of the measured set
        constexpr int kOrderM = 12, NM = 1 << kOrderM;
        juce::dsp::FFT fftM (kOrderM);
        auto spectrum = [&] (const float* ir)
        {
            std::vector<float> b ((size_t) (2 * NM), 0.0f);
            std::copy_n (ir, juce::jmin (set.length, NM), b.data());
            fftM.performRealOnlyForwardTransform (b.data(), true);
            std::vector<std::complex<float>> c ((size_t) (NM / 2 + 1));
            for (int k = 0; k <= NM / 2; ++k) c[(size_t) k] = { b[(size_t) (2 * k)], b[(size_t) (2 * k + 1)] };
            return c;
        };
        const auto snL = spectrum (nearL), sfL = spectrum (farL), snR = spectrum (nearR), sfR = spectrum (farR);
        const int binsM = NM / 2 + 1;
        const double fsM = set.sampleRate;
        std::vector<double> nearMag ((size_t) binsM);
        std::vector<std::complex<double>> rMeas ((size_t) binsM);       // the measured (complex) ratio, for the check
        for (int k = 0; k < binsM; ++k)
        {
            const double a = std::abs (snL[(size_t) k]) + 1e-9, b = std::abs (snR[(size_t) k]) + 1e-9;
            nearMag[(size_t) k] = 0.5 * (a + b);
            const auto r1 = std::complex<double> (sfL[(size_t) k]) / std::complex<double> (snL[(size_t) k] + std::complex<float> (1e-9f));
            const auto r2 = std::complex<double> (sfR[(size_t) k]) / std::complex<double> (snR[(size_t) k] + std::complex<float> (1e-9f));
            rMeas[(size_t) k] = 0.5 * (r1 + r2);
        }
        // fractional-octave smoothing (on dB)
        auto smooth = [&] (const std::vector<double>& v, double fraction)
        {
            std::vector<double> lg ((size_t) binsM), o ((size_t) binsM);
            for (int k = 0; k < binsM; ++k) lg[(size_t) k] = 20.0 * std::log10 (v[(size_t) k] + 1e-12);
            std::vector<double> cum ((size_t) binsM + 1, 0.0);
            for (int k = 0; k < binsM; ++k) cum[(size_t) k + 1] = cum[(size_t) k] + lg[(size_t) k];
            const double h = std::pow (2.0, fraction / 2.0);
            for (int k = 0; k < binsM; ++k)
            {
                const int lo = juce::jmax (0, (int) std::floor (k / h)), hi = juce::jmin (binsM - 1, juce::jmax (lo, (int) std::ceil (k * h)));
                o[(size_t) k] = std::pow (10.0, (cum[(size_t) hi + 1] - cum[(size_t) lo]) / (double) (hi - lo + 1) / 20.0);
            }
            return o;
        };
        const auto nearS = smooth (nearMag, 1.0 / 3.0);

        // ---- ITD: the lag where the far ear's IR fits the near ear's best (both speakers, averaged)
        auto itdOf = [&] (const float* nr, const float* fr)
        {
            const int maxLag = (int) std::ceil (0.0015 * fsM);
            double best = -1e30; int bl = 0;
            std::vector<double> xc ((size_t) maxLag + 1);
            for (int lag = 0; lag <= maxLag; ++lag)
            {
                double s = 0;
                for (int i = 0; i + lag < set.length; ++i) s += (double) nr[i] * fr[i + lag];
                xc[(size_t) lag] = s;
                if (s > best) { best = s; bl = lag; }
            }
            double frac = 0.0;
            if (bl > 0 && bl < maxLag)
            {
                const double a = xc[(size_t) bl - 1], b = xc[(size_t) bl], c = xc[(size_t) bl + 1];
                const double den = a - 2.0 * b + c;
                if (std::abs (den) > 1e-20) frac = 0.5 * (a - c) / den;
            }
            return ((double) bl + frac) / fsM;
        };
        const double tau = 0.5 * (itdOf (nearL, farL) + itdOf (nearR, farR));
        out->itdMs = (float) (tau * 1000.0);
        auto interp = [&] (const std::vector<double>& v, double f)
        {
            const double x = juce::jlimit (0.0, (double) (binsM - 1), f / fsM * NM);
            const int i0 = (int) x; const int i1 = juce::jmin (binsM - 1, i0 + 1);
            return v[(size_t) i0] + (x - i0) * (v[(size_t) i1] - v[(size_t) i0]);
        };
        // the ratio without the ITD, smoothed (complex, 1/12 octave), at most 0.97
        std::vector<std::complex<double>> rRes ((size_t) binsM), rResS ((size_t) binsM);
        for (int k = 0; k < binsM; ++k)
            rRes[(size_t) k] = rMeas[(size_t) k] * std::polar (1.0, juce::MathConstants<double>::twoPi * (k * fsM / NM) * tau);
        {
            std::vector<std::complex<double>> cum ((size_t) binsM + 1);
            for (int k = 0; k < binsM; ++k) cum[(size_t) k + 1] = cum[(size_t) k] + rRes[(size_t) k];
            const double h = std::pow (2.0, (1.0 / 12.0) / 2.0);
            for (int k = 0; k < binsM; ++k)
            {
                const int lo = juce::jmax (0, (int) std::floor (k / h)), hi = juce::jmin (binsM - 1, juce::jmax (lo, (int) std::ceil (k * h)));
                auto v = (cum[(size_t) hi + 1] - cum[(size_t) lo]) / (double) (hi - lo + 1);
                if (std::abs (v) > 0.97) v *= 0.97 / std::abs (v);
                rResS[(size_t) k] = v;
            }
        }
        auto interpC = [&] (double f)
        {
            const double x = juce::jlimit (0.0, (double) (binsM - 1), f / fsM * NM);
            const int i0 = (int) x; const int i1 = juce::jmin (binsM - 1, i0 + 1);
            return rResS[(size_t) i0] + (x - i0) * (rResS[(size_t) i1] - rResS[(size_t) i0]);
        };
        // the level the side colour is normalised to (500 Hz .. 2 kHz)
        double nRef = 0; int nRefN = 0;
        for (int k = 0; k < binsM; ++k) { const double f = k * fsM / NM; if (f >= 500 && f <= 2000) { nRef += nearS[(size_t) k]; ++nRefN; } }
        nRef = nRefN > 0 ? nRef / nRefN : 1.0;

        // ---- the filters at the host rate
        constexpr int kOrderH = 13, NH = 1 << kOrderH;
        const int binsH = NH / 2 + 1;
        const double fs = hostRate;
        const double g = std::pow (10.0, juce::jlimit (0.0f, 30.0f, maxBoostDb) / 20.0), beta = 1.0 / (4.0 * g * g);
        out->delay = (int) std::round (256.0 * fs / 48000.0);
        out->length = (int) std::round (1024.0 * fs / 48000.0);
        const double a = juce::jlimit (0.0f, 1.0f, amount);
        auto band = [] (double f)
        {
            auto rc = [] (double x) { return x <= 0 ? 0.0 : x >= 1 ? 1.0 : 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * x); };
            return rc ((f - 40.0) / 40.0) * (1.0 - rc ((f - 12000.0) / 6000.0));
        };
        std::vector<std::complex<double>> Cs ((size_t) binsH), Cd ((size_t) binsH);
        for (int k = 0; k < binsH; ++k)
        {
            const double f = k * fs / NH, w = juce::MathConstants<double>::twoPi * f;
            const std::complex<double> R = interpC (juce::jmin (f, 0.5 * fsM)) * std::polar (1.0, -w * tau);
            const std::complex<double> Xs = 1.0 + R, Xd = 1.0 - R;
            std::complex<double> cs = std::conj (Xs) / (std::norm (Xs) + beta), cd = std::conj (Xd) / (std::norm (Xd) + beta);
            const double wb = band (f) * a;
            cs = 1.0 + wb * (cs - 1.0);
            cd = 1.0 + wb * (cd - 1.0);
            if (sideEq)
            {
                // the speakers' own direction taken out (both ears the same: it commutes with the matrix)
                const double e = juce::jlimit (std::pow (10.0, -10.0 / 20.0), std::pow (10.0, 10.0 / 20.0),
                                               nRef / juce::jmax (1e-9, interp (nearS, juce::jmin (f, 0.5 * fsM))));
                const double we = band (f) * a;
                const double eq = 1.0 + we * (e - 1.0);
                cs *= eq; cd *= eq;
            }
            const auto shift = std::polar (1.0, -w * out->delay / fs);
            Cs[(size_t) k] = cs * shift; Cd[(size_t) k] = cd * shift;
        }
        juce::dsp::FFT fftH (kOrderH);
        auto toIr = [&] (const std::vector<std::complex<double>>& C)
        {
            std::vector<float> b ((size_t) (2 * NH), 0.0f);
            for (int k = 0; k < binsH; ++k) { b[(size_t) (2 * k)] = (float) C[(size_t) k].real(); b[(size_t) (2 * k + 1)] = (float) C[(size_t) k].imag(); }
            fftH.performRealOnlyInverseTransform (b.data());
            std::vector<float> h ((size_t) out->length);
            const int fadeIn = juce::jmax (1, out->delay / 4), fadeOut = juce::jmax (1, out->length / 6);
            for (int i = 0; i < out->length; ++i)
            {
                float wv = 1.0f;
                if (i < fadeIn) wv = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) i / (float) fadeIn);
                if (i > out->length - fadeOut) wv = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (out->length - i) / (float) fadeOut);
                h[(size_t) i] = b[(size_t) i] * wv;
            }
            return h;
        };
        out->cs = toIr (Cs);
        out->cd = toIr (Cd);

        // ---- how well it cancels: the truncated filters against the measured (unsmoothed, complex) ratio
        {
            auto spec = [&] (const std::vector<float>& h)
            {
                std::vector<float> b ((size_t) (2 * NH), 0.0f);
                std::copy (h.begin(), h.end(), b.begin());
                fftH.performRealOnlyForwardTransform (b.data(), true);
                return b;
            };
            const auto S = spec (out->cs), D = spec (out->cd);
            double near2 = 0, far2 = 0, plain = 0; int cnt = 0;
            for (int k = 0; k < binsH; ++k)
            {
                const double f = k * fs / NH;
                if (f < 250.0 || f > 6000.0) continue;
                const double x = juce::jlimit (0.0, (double) (binsM - 1), f / fsM * NM);
                const auto R = rMeas[(size_t) juce::roundToInt (x)];
                const std::complex<double> cs (S[(size_t) (2 * k)], S[(size_t) (2 * k + 1)]), cd (D[(size_t) (2 * k)], D[(size_t) (2 * k + 1)]);
                const auto eNear = 0.5 * ((1.0 + R) * cs + (1.0 - R) * cd), eFar = 0.5 * ((1.0 + R) * cs - (1.0 - R) * cd);
                near2 += std::norm (eNear); far2 += std::norm (eFar); plain += std::norm (R); ++cnt;
            }
            out->crossPlainDb = (float) (10.0 * std::log10 (juce::jmax (1e-12, plain / juce::jmax (1, cnt))));
            out->crossDb = (float) (10.0 * std::log10 (juce::jmax (1e-12, far2 / juce::jmax (1e-12, near2))));
        }
        out->description = juce::String::fromUTF8 ("\xc2\xb1") + juce::String (juce::roundToInt (angleDeg)) + juce::String::fromUTF8 ("\xc2\xb0 \xc2\xb7 \xc3\x9c" "bersprechen ")
                         + juce::String (juce::roundToInt (out->crossDb)) + " dB (ohne " + juce::String (juce::roundToInt (out->crossPlainDb)) + ") "
                         + juce::String::fromUTF8 ("\xc2\xb7 ITD ") + juce::String (out->itdMs, 2) + " ms " + juce::String::fromUTF8 ("\xc2\xb7 +")
                         + juce::String (1000.0 * out->delay / fs, 1) + " ms";
        return out;
    }
};

/** real-time part: two FIR convolutions (sum / difference) on the binaural pair, changes crossfaded */
class TransauralCtc
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) juce::jmax (1, maxBlock), 1 };
        convS.prepare (spec); convD.prepare (spec);
        mid.assign ((size_t) juce::jmax (1, maxBlock), 0.0f);
        side.assign ((size_t) juce::jmax (1, maxBlock), 0.0f);
        prepared = true;
    }
    void reset() noexcept { convS.reset(); convD.reset(); }
    /** message thread: load new filters (they are crossfaded in by juce::dsp::Convolution) */
    void load (const TransauralFilters& f)
    {
        auto toBuf = [&] (const std::vector<float>& h)
        {
            juce::AudioBuffer<float> b (1, (int) h.size());
            std::copy (h.begin(), h.end(), b.getWritePointer (0));
            return b;
        };
        convS.loadImpulseResponse (toBuf (f.cs), f.sampleRate, juce::dsp::Convolution::Stereo::no, juce::dsp::Convolution::Trim::no,
                                   juce::dsp::Convolution::Normalise::no);
        convD.loadImpulseResponse (toBuf (f.cd), f.sampleRate, juce::dsp::Convolution::Stereo::no, juce::dsp::Convolution::Trim::no,
                                   juce::dsp::Convolution::Normalise::no);
        loaded = true;
    }
    bool isLoaded() const noexcept { return loaded; }
    /** l / r: the binaural pair in, the two loudspeaker feeds out (in place) */
    void process (float* l, float* r, int n) noexcept
    {
        if (! prepared || ! loaded || n <= 0 || n > (int) mid.size()) return;
        for (int i = 0; i < n; ++i) { mid[(size_t) i] = 0.5f * (l[i] + r[i]); side[(size_t) i] = 0.5f * (l[i] - r[i]); }
        float* pm = mid.data(); float* ps = side.data();
        juce::dsp::AudioBlock<float> bm (&pm, 1, (size_t) n), bs (&ps, 1, (size_t) n);
        convS.process (juce::dsp::ProcessContextReplacing<float> (bm));
        convD.process (juce::dsp::ProcessContextReplacing<float> (bs));
        for (int i = 0; i < n; ++i) { l[i] = mid[(size_t) i] + side[(size_t) i]; r[i] = mid[(size_t) i] - side[(size_t) i]; }
    }
private:
    juce::dsp::Convolution convS, convD;
    std::vector<float> mid, side;
    bool prepared = false;
    std::atomic<bool> loaded { false };            // set on the message thread, read on the audio thread
};
} // namespace rysum
