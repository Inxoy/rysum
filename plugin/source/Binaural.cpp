#include "Binaural.h"
#include "SofaGunzip.h"
#include "Spatial.h"
#include <RysumBinaryData.h>
#include <complex>

extern "C"
{
#include "mysofa.h"
}

namespace rysum
{
using cplx = std::complex<double>;

// ---------------------------------------------------------------------------
// HRIR sets
// ---------------------------------------------------------------------------
std::unique_ptr<HrirSet> HrirSet::fabian(juce::String &error)
{
    // resources/fabian_hrir.bin (zlib): "RHRF", version, directions, length, rate (double), scale
    // (float), directions x (az, el) float32, then int16 IRs [direction][ear][sample]
    juce::MemoryInputStream raw(RysumData::fabian_hrir_bin, (size_t)RysumData::fabian_hrir_binSize,
                                false);
    juce::GZIPDecompressorInputStream gz(&raw, false,
                                         juce::GZIPDecompressorInputStream::zlibFormat);
    juce::MemoryBlock mb;
    gz.readIntoMemoryBlock(mb);
    juce::MemoryInputStream in(mb, false);
    char magic[4]{};
    if (in.read(magic, 4) != 4 || std::memcmp(magic, "RHRF", 4) != 0)
    {
        error = "FABIAN data damaged";
        return nullptr;
    }
    in.readInt(); // version
    const int q = in.readInt(), len = in.readInt();
    const double rate = in.readDouble();
    const float scale = in.readFloat();
    if (q <= 0 || len <= 0 ||
        (size_t)mb.getSize() < 28 + (size_t)q * 8 + (size_t)q * 2 * (size_t)len * 2)
    {
        error = "FABIAN data damaged";
        return nullptr;
    }
    auto s = std::make_unique<HrirSet>();
    s->name = "FABIAN (TU Berlin)";
    s->sampleRate = rate;
    s->length = len;
    s->az.resize((size_t)q);
    s->el.resize((size_t)q);
    for (int i = 0; i < q; ++i)
    {
        s->az[(size_t)i] = wrapAz(in.readFloat());
        s->el[(size_t)i] = in.readFloat();
    }
    s->ir.resize((size_t)q * 2 * (size_t)len);
    for (auto &v : s->ir)
        v = (float)in.readShort() * scale;
    s->info = juce::String(q) + " Richtungen, " + juce::String(rate / 1000.0, 1) + " kHz";
    return s;
}

std::unique_ptr<HrirSet> HrirSet::fromSofa(const juce::File &file, juce::String &error)
{
    if (!file.existsAsFile())
    {
        error = "Datei nicht gefunden";
        return nullptr;
    }
    if (file.getSize() > 400 * 1024 * 1024)
    {
        error = juce::String::fromUTF8("Datei zu gro\xc3\x9f");
        return nullptr;
    }
    juce::MemoryBlock data;
    if (!file.loadFileAsData(data))
    {
        error = "Datei nicht lesbar";
        return nullptr;
    }

    installSofaInflate();
    int err = 0;
    MYSOFA_HRTF *h = mysofa_load_data((const char *)data.getData(), data.getSize(), &err);
    if (h == nullptr || err != MYSOFA_OK)
    {
        if (h != nullptr)
            mysofa_free(h);
        error = "kein lesbares SOFA (Fehler " + juce::String(err) + ")";
        return nullptr;
    }
    std::unique_ptr<MYSOFA_HRTF, void (*)(MYSOFA_HRTF *)> guard(h, mysofa_free);
    const int M = (int)h->M, R = (int)h->R, N = (int)h->N;
    if (R != 2 || M < 6 || N < 8 || h->DataIR.values == nullptr ||
        (int)h->DataIR.elements < M * R * N)
    {
        error = "SOFA: erwartet SimpleFreeFieldHRIR mit 2 Ohren (gefunden: " + juce::String(M) +
                " Messungen, " + juce::String(R) + " Empf\xc3\xa4nger)";
        return nullptr;
    }
    if (h->SourcePosition.values == nullptr || (int)h->SourcePosition.elements < 3 * M)
    {
        error = "SOFA: keine Quellpositionen";
        return nullptr;
    }
    const double rate = h->DataSamplingRate.values != nullptr && h->DataSamplingRate.elements > 0
                            ? h->DataSamplingRate.values[0]
                            : 48000.0;
    if (rate < 8000.0 || rate > 384000.0)
    {
        error = "SOFA: unbrauchbare Abtastrate";
        return nullptr;
    }

    mysofa_tocartesian(h);
    // separate delays (some files keep the time of flight apart): samples per measurement and ear
    const int nDelay = h->DataDelay.values != nullptr ? (int)h->DataDelay.elements : 0;
    auto delayOf = [&](int m, int r)
    {
        if (nDelay >= M * R)
            return (int)std::lround(h->DataDelay.values[m * R + r]);
        if (nDelay >= R)
            return (int)std::lround(h->DataDelay.values[r]);
        return 0;
    };
    int maxDelay = 0;
    for (int m = 0; m < M; ++m)
        for (int r = 0; r < R; ++r)
            maxDelay = juce::jmax(maxDelay, juce::jlimit(0, 4096, delayOf(m, r)));
    const int len = juce::jmin(4096, N + maxDelay);

    auto s = std::make_unique<HrirSet>();
    s->name = file.getFileNameWithoutExtension();
    s->sampleRate = rate;
    s->length = len;
    s->az.resize((size_t)M);
    s->el.resize((size_t)M);
    s->ir.assign((size_t)M * 2 * (size_t)len, 0.0f);
    for (int m = 0; m < M; ++m)
    {
        const float *p = h->SourcePosition.values + 3 * m;
        const double x = p[0], y = p[1], z = p[2];
        s->az[(size_t)m] = wrapAz((float)juce::radiansToDegrees(std::atan2(y, x)));
        s->el[(size_t)m] = (float)juce::radiansToDegrees(std::atan2(z, std::sqrt(x * x + y * y)));
        for (int r = 0; r < 2; ++r)
        {
            const int d = juce::jlimit(0, 4096, delayOf(m, r));
            const float *src = h->DataIR.values + ((size_t)m * 2 + (size_t)r) * (size_t)N;
            float *dst = s->ir.data() + ((size_t)m * 2 + (size_t)r) * (size_t)len;
            for (int i = 0; i < N && i + d < len; ++i)
                dst[i + d] = src[i];
        }
    }
    float peak = 0.0f;
    for (auto v : s->ir)
        peak = juce::jmax(peak, std::abs(v));
    if (!std::isfinite(peak) || peak <= 0.0f)
    {
        error = "SOFA: leere Impulsantworten";
        return nullptr;
    }
    s->info = juce::String(M) + " Richtungen, " + juce::String(rate / 1000.0, 1) + " kHz";
    return s;
}

// ---------------------------------------------------------------------------
// MagLS design
// ---------------------------------------------------------------------------
namespace
{
struct Vec3
{
    double x, y, z;
};
Vec3 unitVec(float azDeg, float elDeg)
{
    const double a = juce::degreesToRadians((double)azDeg),
                 e = juce::degreesToRadians((double)elDeg);
    return {std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e)};
}

/** Cholesky solve of a small symmetric positive definite system, in place: A (K x K), B (K x cols)
 */
bool choleskySolve(std::vector<double> &A, int K, std::vector<double> &B, int cols)
{
    for (int j = 0; j < K; ++j)
    {
        double d = A[(size_t)(j * K + j)];
        for (int k = 0; k < j; ++k)
            d -= A[(size_t)(j * K + k)] * A[(size_t)(j * K + k)];
        if (d <= 0.0)
            return false;
        d = std::sqrt(d);
        A[(size_t)(j * K + j)] = d;
        for (int i = j + 1; i < K; ++i)
        {
            double s = A[(size_t)(i * K + j)];
            for (int k = 0; k < j; ++k)
                s -= A[(size_t)(i * K + k)] * A[(size_t)(j * K + k)];
            A[(size_t)(i * K + j)] = s / d;
        }
    }
    for (int c = 0; c < cols; ++c)
    {
        for (int i = 0; i < K; ++i) // L y = b
        {
            double s = B[(size_t)(i * cols + c)];
            for (int k = 0; k < i; ++k)
                s -= A[(size_t)(i * K + k)] * B[(size_t)(k * cols + c)];
            B[(size_t)(i * cols + c)] = s / A[(size_t)(i * K + i)];
        }
        for (int i = K - 1; i >= 0; --i) // L^T x = y
        {
            double s = B[(size_t)(i * cols + c)];
            for (int k = i + 1; k < K; ++k)
                s -= A[(size_t)(k * K + i)] * B[(size_t)(k * cols + c)];
            B[(size_t)(i * cols + c)] = s / A[(size_t)(i * K + i)];
        }
    }
    return true;
}

/** windowed-sinc resampling of a short FIR (gain kept: sum of taps scales with the rate) */
std::vector<float> resampleFir(const float *h, int len, double fromRate, double toRate, int outLen)
{
    std::vector<float> out((size_t)outLen, 0.0f);
    const double ratio = fromRate / toRate; // input samples per output sample
    const double fc = 0.94 * juce::jmin(1.0, toRate / fromRate);
    const int half = 24;
    const double beta = 8.0;
    auto bessel0 = [](double x)
    {
        double s = 1, t = 1;
        for (int k = 1; k < 30; ++k)
        {
            t *= (x / (2 * k)) * (x / (2 * k));
            s += t;
        }
        return s;
    };
    const double i0b = bessel0(beta);
    for (int o = 0; o < outLen; ++o)
    {
        const double x = o * ratio;
        const int j0 = (int)std::floor(x) - half, j1 = (int)std::floor(x) + half;
        double acc = 0.0;
        for (int j = juce::jmax(0, j0); j <= juce::jmin(len - 1, j1); ++j)
        {
            const double t = x - j;
            const double arg = juce::MathConstants<double>::pi * fc * t;
            const double sinc = std::abs(arg) < 1e-9 ? 1.0 : std::sin(arg) / arg;
            const double r = t / (half + 1);
            const double win =
                std::abs(r) >= 1.0 ? 0.0 : bessel0(beta * std::sqrt(1.0 - r * r)) / i0b;
            acc += h[j] * fc * sinc * win;
        }
        out[(size_t)o] = (float)(acc * ratio);
    }
    return out;
}
} // namespace

std::unique_ptr<BinauralFilters> BinauralFilters::design(const HrirSet &set, int order,
                                                         double hostRate, bool diffuseEq,
                                                         std::function<bool()> shouldAbort)
{
    auto aborted = [&] { return shouldAbort && shouldAbort(); };
    order = juce::jlimit(1, kMaxOrder, order);
    const int K = (order + 1) * (order + 1);
    const double fs = set.sampleRate;

    // ---- directions (large sets: an evenly spread subset of ~3000, enough for 5th order)
    std::vector<int> dirs;
    const int Qall = set.numDirections();
    if (Qall > 3000)
    {
        std::vector<Vec3> v((size_t)Qall);
        for (int q = 0; q < Qall; ++q)
            v[(size_t)q] = unitVec(set.az[(size_t)q], set.el[(size_t)q]);
        std::vector<char> used((size_t)Qall, 0);
        const int n = 3000;
        const double golden = juce::MathConstants<double>::pi * (1.0 + std::sqrt(5.0));
        for (int i = 0; i < n; ++i)
        {
            const double phi = std::acos(1.0 - 2.0 * (i + 0.5) / n), th = golden * (i + 0.5);
            const Vec3 p{std::cos(th) * std::sin(phi), std::sin(th) * std::sin(phi), std::cos(phi)};
            int best = 0;
            double bd = -2.0;
            for (int q = 0; q < Qall; ++q)
            {
                const double d = p.x * v[(size_t)q].x + p.y * v[(size_t)q].y + p.z * v[(size_t)q].z;
                if (d > bd)
                {
                    bd = d;
                    best = q;
                }
            }
            if (!used[(size_t)best])
            {
                used[(size_t)best] = 1;
                dirs.push_back(best);
            }
        }
    }
    else
        for (int q = 0; q < Qall; ++q)
            dirs.push_back(q);
    const int Q = (int)dirs.size();
    if (Q < K)
        return nullptr;
    if (aborted())
        return nullptr;

    // ---- quadrature weights from the local density of the directions (works for any grid)
    std::vector<Vec3> u((size_t)Q);
    for (int i = 0; i < Q; ++i)
        u[(size_t)i] = unitVec(set.az[(size_t)dirs[(size_t)i]], set.el[(size_t)dirs[(size_t)i]]);
    std::vector<double> w((size_t)Q, 1.0);
    {
        const double cosR =
            1.0 - 2.0 * juce::jmin(0.5, 12.0 / Q); // a cap that holds ~12 points of an even grid
        double sum = 0.0;
        for (int i = 0; i < Q; ++i)
        {
            int cnt = 0;
            for (int j = 0; j < Q; ++j)
                if (u[(size_t)i].x * u[(size_t)j].x + u[(size_t)i].y * u[(size_t)j].y +
                        u[(size_t)i].z * u[(size_t)j].z >
                    cosR)
                    ++cnt;
            w[(size_t)i] = 1.0 / juce::jmax(1, cnt);
            sum += w[(size_t)i];
        }
        for (auto &x : w)
            x /= sum;
    }

    // ---- spherical harmonics (exactly the plugin's encoder) and the least-squares solver
    std::vector<double> Y((size_t)Q * (size_t)K);
    {
        float tmp[kMaxAmbiChannels];
        for (int i = 0; i < Q; ++i)
        {
            encodeAmbiX(set.az[(size_t)dirs[(size_t)i]], set.el[(size_t)dirs[(size_t)i]], order,
                        tmp);
            for (int k = 0; k < K; ++k)
                Y[(size_t)i * (size_t)K + (size_t)k] = tmp[k];
        }
    }
    std::vector<double> A0((size_t)(K * K), 0.0); // Y^T W Y
    for (int i = 0; i < Q; ++i)
        for (int a = 0; a < K; ++a)
            for (int b = 0; b < K; ++b)
                A0[(size_t)(a * K + b)] +=
                    w[(size_t)i] * Y[(size_t)(i * K + a)] * Y[(size_t)(i * K + b)];
    std::vector<double> P(
        (size_t)K *
        (size_t)
            Q); // (Y^T W Y + reg)^-1 Y^T W  : maps the HRTFs of all directions to SH coefficients
    {
        auto A = A0;
        double tr = 0;
        for (int k = 0; k < K; ++k)
            tr += A0[(size_t)(k * K + k)];
        for (int k = 0; k < K; ++k)
            A[(size_t)(k * K + k)] += 1.0e-3 * tr / K;
        for (int k = 0; k < K; ++k)
            for (int i = 0; i < Q; ++i)
                P[(size_t)k * (size_t)Q + (size_t)i] = Y[(size_t)(i * K + k)] * w[(size_t)i];
        if (!choleskySolve(A, K, P, Q))
            return nullptr;
    }
    if (aborted())
        return nullptr;

    // ---- spectra of the HRIRs
    const int lenIn = juce::jmin(set.length, 2048);
    int fftOrder = 10;
    while ((1 << fftOrder) < 4 * lenIn && fftOrder < 13)
        ++fftOrder;
    const int nfft = 1 << fftOrder, bins = nfft / 2 + 1;
    juce::dsp::FFT fft(fftOrder);
    std::vector<float> buf((size_t)(2 * nfft));
    std::vector<std::complex<float>> H((size_t)2 * (size_t)Q * (size_t)bins); // [ear][dir][bin]

    // Common delay (sound reaching the nearest ear): removed before the fit and put back
    // afterwards. Above the cutoff MagLS keeps the phase of the previous frequency, i.e. zero group
    // delay; without this the high frequencies of the filters would sit at t = 0 and wrap around
    // (non-causal).
    int onset = lenIn;
    for (int i = 0; i < Q; ++i)
        for (int e = 0; e < 2; ++e)
        {
            const float *x = set.get(dirs[(size_t)i], e);
            float pk = 0.0f;
            for (int n = 0; n < lenIn; ++n)
                pk = juce::jmax(pk, std::abs(x[n]));
            for (int n = 0; n < lenIn; ++n)
                if (std::abs(x[n]) > 0.1f * pk)
                {
                    onset = juce::jmin(onset, n);
                    break;
                }
        }
    const double bulk = juce::jmax(0, onset - 2);
    const double after =
        bulk + std::ceil(0.0005 * fs); // put back with 0.5 ms headroom for the pre-ringing
    for (int e = 0; e < 2; ++e)
        for (int i = 0; i < Q; ++i)
        {
            std::fill(buf.begin(), buf.end(), 0.0f);
            const float *src = set.get(dirs[(size_t)i], e);
            std::copy(src, src + lenIn, buf.begin());
            fft.performRealOnlyForwardTransform(buf.data(), true);
            auto *dst = H.data() + ((size_t)e * (size_t)Q + (size_t)i) * (size_t)bins;
            for (int b = 0; b < bins; ++b)
                dst[b] =
                    std::complex<float>(buf[(size_t)(2 * b)], buf[(size_t)(2 * b + 1)]) *
                    std::polar(1.0f, (float)(juce::MathConstants<double>::twoPi * b * bulk / nfft));
        }
    if (aborted())
        return nullptr;

    // ---- MagLS per ear
    const double fCut = juce::jlimit(
        500.0, 4000.0, order * 343.0 / (2.0 * juce::MathConstants<double>::pi * 0.0875));
    const int binCut = juce::jlimit(1, bins - 1, (int)std::lround(fCut * nfft / fs));
    std::vector<cplx> D((size_t)2 * (size_t)bins * (size_t)K); // [ear][bin][k]
    std::vector<double> targetE((size_t)2 * (size_t)bins, 0.0);
    std::vector<cplx> est((size_t)Q), t((size_t)Q);
    for (int e = 0; e < 2; ++e)
    {
        for (int b = 0; b < bins; ++b)
        {
            const auto *h = H.data() + (size_t)e * (size_t)Q * (size_t)bins;
            double eT = 0.0;
            for (int i = 0; i < Q; ++i)
            {
                const std::complex<float> v = h[(size_t)i * (size_t)bins + (size_t)b];
                eT += w[(size_t)i] * std::norm(v);
                if (b < binCut)
                    t[(size_t)i] = cplx(v.real(), v.imag());
                else
                {
                    // magnitude of the HRTF, phase of last frequency's fit
                    const double mag = std::abs(v);
                    const double em = std::abs(est[(size_t)i]);
                    t[(size_t)i] = em > 1e-12 ? est[(size_t)i] * (mag / em) : cplx(mag, 0.0);
                }
            }
            targetE[(size_t)(e * bins + b)] = eT;
            cplx *d = D.data() + ((size_t)e * (size_t)bins + (size_t)b) * (size_t)K;
            for (int k = 0; k < K; ++k)
            {
                cplx s = 0.0;
                const double *pk = P.data() + (size_t)k * (size_t)Q;
                for (int i = 0; i < Q; ++i)
                    s += pk[i] * t[(size_t)i];
                d[k] = s;
            }
            // the fit seen from every direction (= next frequency's phase), and its diffuse-field
            // energy
            double eD = 0.0;
            for (int i = 0; i < Q; ++i)
            {
                cplx s = 0.0;
                const double *yi = Y.data() + (size_t)i * (size_t)K;
                for (int k = 0; k < K; ++k)
                    s += yi[k] * d[k];
                est[(size_t)i] = s;
                eD += w[(size_t)i] * std::norm(s);
            }
            if (b >= binCut && eD > 1e-20)
            {
                const double g = juce::jlimit(0.25, 4.0, std::sqrt(eT / eD));
                for (int k = 0; k < K; ++k)
                    d[k] *= g;
                for (auto &x : est)
                    x *= g;
            }
            if (b == 0 || b == bins - 1) // DC and Nyquist are real
                for (int k = 0; k < K; ++k)
                    d[k] = cplx(d[k].real(), 0.0);
        }
        if (aborted())
            return nullptr;
    }

    // ---- optional diffuse-field equalisation (minimum phase), so the set sounds neutral on
    // headphones
    std::vector<cplx> eq((size_t)bins, cplx(1.0, 0.0));
    if (diffuseEq)
    {
        std::vector<double> df((size_t)bins), sm((size_t)bins);
        for (int b = 0; b < bins; ++b)
            df[(size_t)b] = 0.5 * (targetE[(size_t)b] + targetE[(size_t)(bins + b)]);
        for (int b = 0; b < bins; ++b) // third-octave smoothing
        {
            const int lo = juce::jmax(0, (int)std::floor(b / 1.1225)),
                      hi = juce::jmin(bins - 1, (int)std::ceil(b * 1.1225));
            double s = 0;
            for (int j = lo; j <= hi; ++j)
                s += df[(size_t)j];
            sm[(size_t)b] = s / (hi - lo + 1);
        }
        // reference: the region 200 Hz .. 1 kHz stays where it is
        double ref = 0;
        int nr = 0;
        for (int b = 0; b < bins; ++b)
        {
            const double f = b * fs / nfft;
            if (f >= 200 && f <= 1000)
            {
                ref += sm[(size_t)b];
                ++nr;
            }
        }
        ref = nr > 0 ? ref / nr : 1.0;
        std::vector<float> c((size_t)(2 * nfft), 0.0f);
        const int bLow = juce::jmax(1, (int)std::lround(150.0 * nfft / fs));
        for (int b = 0; b < bins; ++b)
        {
            const int bb = juce::jmax(b, bLow);
            const double gdb = juce::jlimit(
                -12.0, 12.0, 10.0 * std::log10(ref / juce::jmax(1e-20, sm[(size_t)bb])));
            c[(size_t)(2 * b)] = (float)(gdb / 20.0 * std::log(10.0)); // ln |EQ|
        }
        fft.performRealOnlyInverseTransform(c.data()); // real cepstrum (first nfft values)
        std::vector<float> f((size_t)(2 * nfft), 0.0f);
        f[0] = c[0];
        for (int n = 1; n < nfft / 2; ++n)
            f[(size_t)n] = 2.0f * c[(size_t)n];
        f[(size_t)(nfft / 2)] = c[(size_t)(nfft / 2)];
        fft.performRealOnlyForwardTransform(f.data(), true);
        for (int b = 0; b < bins; ++b)
            eq[(size_t)b] = std::exp(cplx(f[(size_t)(2 * b)], f[(size_t)(2 * b + 1)]));
    }

    // ---- level: diffuse field at 300 Hz .. 3 kHz like an equal-power stereo pan (-3 dB per ear)
    double eMid = 0.0;
    int nMid = 0;
    for (int e = 0; e < 2; ++e)
        for (int b = 0; b < bins; ++b)
        {
            const double f = b * fs / nfft;
            if (f < 300 || f > 3000)
                continue;
            const cplx *d = D.data() + ((size_t)e * (size_t)bins + (size_t)b) * (size_t)K;
            double en = 0.0;
            for (int a = 0; a < K; ++a)
                for (int c2 = 0; c2 < K; ++c2)
                    en += (std::conj(d[a]) * A0[(size_t)(a * K + c2)] * d[c2]).real();
            eMid += en * std::norm(eq[(size_t)b]);
            ++nMid;
        }
    const double gain = nMid > 0 && eMid > 0 ? std::sqrt(0.5 / (eMid / nMid)) : 1.0;

    // ---- back to time: 10.7 ms filters (at 48 kHz), soft end
    const int lenOut = juce::jmin(nfft, (int)std::lround(512.0 * fs / 48000.0));
    const int fade = lenOut / 8;
    std::vector<float> filt((size_t)2 * (size_t)K * (size_t)lenOut);
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < K; ++k)
        {
            std::fill(buf.begin(), buf.end(), 0.0f);
            for (int b = 0; b < bins; ++b)
            {
                const cplx v =
                    D[((size_t)e * (size_t)bins + (size_t)b) * (size_t)K + (size_t)k] *
                    eq[(size_t)b] * gain *
                    std::polar(1.0, -juce::MathConstants<double>::twoPi * b * after / nfft);
                buf[(size_t)(2 * b)] = (float)v.real();
                buf[(size_t)(2 * b + 1)] = (float)v.imag();
            }
            fft.performRealOnlyInverseTransform(buf.data());
            float *dst = filt.data() + ((size_t)e * (size_t)K + (size_t)k) * (size_t)lenOut;
            for (int n = 0; n < lenOut; ++n)
            {
                float g = 1.0f;
                if (n >= lenOut - fade)
                    g = 0.5f + 0.5f * std::cos(juce::MathConstants<float>::pi *
                                               (float)(n - (lenOut - fade)) / (float)fade);
                dst[n] = buf[(size_t)n] * g;
            }
        }

    auto out = std::make_unique<BinauralFilters>();
    out->order = order;
    out->numChannels = K;
    out->sampleRate = hostRate;
    if (std::abs(hostRate - fs) < 0.5)
    {
        out->length = lenOut;
        out->h = std::move(filt);
    }
    else
    {
        out->length = (int)std::ceil(lenOut * hostRate / fs);
        out->h.resize((size_t)2 * (size_t)K * (size_t)out->length);
        for (int e = 0; e < 2; ++e)
            for (int k = 0; k < K; ++k)
            {
                auto r =
                    resampleFir(filt.data() + ((size_t)e * (size_t)K + (size_t)k) * (size_t)lenOut,
                                lenOut, fs, hostRate, out->length);
                std::copy(r.begin(), r.end(),
                          out->h.begin() + (std::ptrdiff_t)(((size_t)e * (size_t)K + (size_t)k) *
                                                            (size_t)out->length));
            }
    }
    out->description =
        set.name + juce::String::fromUTF8(" \xc2\xb7 MagLS ") + juce::String(order) + ". Ordnung" +
        (diffuseEq ? juce::String::fromUTF8(" \xc2\xb7 diffusfeld-entzerrt") : juce::String());
    return out;
}

// ---------------------------------------------------------------------------
// real-time decoder
// ---------------------------------------------------------------------------
BinauralDecoder::BinauralDecoder(const BinauralFilters &f)
    : order(f.order), numCh(f.numChannels), description(f.description)
{
    parts = juce::jmax(1, (f.length + kBlock - 1) / kBlock);
    spectra.assign((size_t)2 * (size_t)numCh * (size_t)parts * (size_t)kFft);
    RealFFT fft(kFft);
    AlignedFloats tmp((size_t)kFft);
    for (int e = 0; e < 2; ++e)
        for (int c = 0; c < numCh; ++c)
            for (int p = 0; p < parts; ++p)
            {
                tmp.clear();
                const int start = p * kBlock, n = juce::jmin(kBlock, f.length - start);
                const float *src = f.get(e, c);
                for (int i = 0; i < n; ++i)
                    tmp[(size_t)i] = src[start + i];
                const size_t base =
                    (((size_t)e * (size_t)numCh + (size_t)c) * (size_t)parts + (size_t)p) *
                    (size_t)kFft;
                fft.forwardScaled(tmp.data(), spectra.data() + base);
            }
    // inputs = the Ambisonics channels, outputs = the two ears; partition p = filter samples [128
    // p, 128 p + 128) as a lag-1 stage of the filters delayed by one block
    conv.prepare(kBlock, 1, numCh, parts, 2, numCh);
    std::vector<PartitionedConvolution::Term> t((size_t)numCh);
    for (int e = 0; e < 2; ++e)
    {
        for (int c = 0; c < numCh; ++c)
        {
            t[(size_t)c].input = c;
            t[(size_t)c].spectra = spectra.data() + ((size_t)e * (size_t)numCh + (size_t)c) *
                                                        (size_t)parts * (size_t)kFft;
            t[(size_t)c].first = 0;
            t[(size_t)c].parts = parts;
        }
        conv.setTerms(e, t.data(), numCh);
    }
    conv.reset();
    scratch.assign((size_t)2 * kBlock);
}

void BinauralDecoder::reset() noexcept
{
    conv.reset();
}

void AmbiYawRotator::process(float *const *in, int n, int order, float yawDeg) noexcept
{
    // turning the head by +yaw (to the left) turns the scene by -yaw: only (l, m) / (l, -m) pairs
    // mix
    const float y0 = lastYaw, y1 = yawDeg;
    lastYaw = yawDeg;
    if (n <= 0)
        return;
    if (std::abs(y0) < 1.0e-4f && std::abs(y1) < 1.0e-4f)
        return;
    const float d = wrapAz(y1 - y0);
    for (int i = 0; i < n; ++i)
    {
        const float a = -juce::degreesToRadians(y0 + d * (float)(i + 1) / (float)n);
        const float c1 = std::cos(a), s1 = std::sin(a);
        float cm = 1.0f, sm = 0.0f;
        for (int m = 1; m <= order; ++m)
        {
            const float t = cm * c1 - sm * s1;
            sm = sm * c1 + cm * s1;
            cm = t;
            for (int l = m; l <= order; ++l)
            {
                float *pos_ = in[l * l + l + m];
                float *neg = in[l * l + l - m];
                const float xc = pos_[i], xs = neg[i];
                pos_[i] = cm * xc - sm * xs;
                neg[i] = sm * xc + cm * xs;
            }
        }
    }
}

void BinauralDecoder::process(float *const *in, int n, float *left, float *right,
                              float yawDeg) noexcept
{
    rotator.process(in, n, order, yawDeg);
    int done = 0;
    while (done < n)
    {
        const int len = juce::jmin(n - done, conv.toBoundary());
        const int p = conv.position();
        std::copy_n(conv.output(0) + p, len, scratch.data());
        std::copy_n(conv.output(1) + p, len, scratch.data() + kBlock);
        const float *ins[kMaxAmbiChannels]{};
        for (int c = 0; c < numCh; ++c)
            ins[c] = in[c] + done;
        conv.push(ins, len);
        // (written only now: a mono host passes one of the input channels as 'right')
        std::copy_n(scratch.data(), len, left + done);
        std::copy_n(scratch.data() + kBlock, len, right + done);
        done += len;
    }
}
} // namespace rysum
