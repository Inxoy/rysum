// AllRAD loudspeaker decoding (as in the IEM AllRADecoder): layouts, IEM JSON files, the decoder and its renderer
#include "Speakers.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <algorithm>
#include <cmath>

namespace rysum
{
namespace
{
    using V3 = std::array<double, 3>;
    V3 toVec (double azDeg, double elDeg)
    {
        const double a = juce::degreesToRadians (azDeg), e = juce::degreesToRadians (elDeg);
        return { std::cos (e) * std::cos (a), std::cos (e) * std::sin (a), std::sin (e) };
    }
    double dot (const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    V3 sub (const V3& a, const V3& b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
    V3 cross (const V3& a, const V3& b) { return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
    double len (const V3& a) { return std::sqrt (dot (a, a)); }
    void dirOf (const V3& v, float& az, float& el)
    {
        az = (float) juce::radiansToDegrees (std::atan2 (v[1], v[0]));
        el = (float) juce::radiansToDegrees (std::asin (juce::jlimit (-1.0, 1.0, v[2] / juce::jmax (1.0e-12, len (v)))));
    }

    /** Legendre polynomial P_n(x) */
    double legendre (int n, double x)
    {
        double p0 = 1.0, p1 = x;
        if (n == 0) return p0;
        for (int k = 2; k <= n; ++k) { const double p2 = ((2 * k - 1) * x * p1 - (k - 1) * p0) / k; p0 = p1; p1 = p2; }
        return p1;
    }

    /** nearly uniform directions on the sphere (Fibonacci lattice) - the virtual loudspeakers */
    std::vector<V3> virtualDirections (int n)
    {
        std::vector<V3> d ((size_t) n);
        const double ga = juce::MathConstants<double>::pi * (3.0 - std::sqrt (5.0));
        for (int i = 0; i < n; ++i)
        {
            const double z = 1.0 - 2.0 * (i + 0.5) / n, r = std::sqrt (1.0 - z * z), a = ga * i;
            d[(size_t) i] = { r * std::cos (a), r * std::sin (a), z };
        }
        return d;
    }

    struct Tri { std::array<int, 3> idx; double inv[3][3]; };

    /** convex hull of unit vectors (brute force: fine for up to ~100 points). Coplanar points give overlapping
        triangles - harmless for VBAP, any triangle containing a direction works. */
    std::vector<Tri> hull (const std::vector<V3>& P)
    {
        std::vector<Tri> out;
        const int n = (int) P.size();
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                for (int k = j + 1; k < n; ++k)
                {
                    V3 nn = cross (sub (P[(size_t) j], P[(size_t) i]), sub (P[(size_t) k], P[(size_t) i]));
                    const double l = len (nn);
                    if (l < 1.0e-6) continue;
                    for (auto& x : nn) x /= l;
                    int pos = 0, neg = 0;
                    for (int m = 0; m < n && ! (pos && neg); ++m)
                    {
                        const double d = dot (nn, sub (P[(size_t) m], P[(size_t) i]));
                        if (d > 1.0e-6) ++pos; else if (d < -1.0e-6) ++neg;
                    }
                    if (pos && neg) continue;
                    // inverse of the matrix with the three directions as columns (direction = M g)
                    const V3& a = P[(size_t) i]; const V3& b = P[(size_t) j]; const V3& c = P[(size_t) k];
                    const double det = dot (a, cross (b, c));
                    if (std::abs (det) < 1.0e-6) continue;          // plane through the listener: no use for panning
                    Tri t;
                    t.idx = { i, j, k };
                    const V3 r0 = cross (b, c), r1 = cross (c, a), r2 = cross (a, b);
                    for (int q = 0; q < 3; ++q) { t.inv[0][q] = r0[(size_t) q] / det; t.inv[1][q] = r1[(size_t) q] / det; t.inv[2][q] = r2[(size_t) q] / det; }
                    out.push_back (t);
                }
        return out;
    }

    /** VBAP: the triangle containing 'p' and its gains (unit energy); false = not covered */
    bool vbap (const std::vector<Tri>& tris, const V3& p, std::array<int, 3>& idx, std::array<double, 3>& g)
    {
        for (const auto& t : tris)
        {
            double gg[3];
            for (int q = 0; q < 3; ++q) gg[q] = t.inv[q][0] * p[0] + t.inv[q][1] * p[1] + t.inv[q][2] * p[2];
            if (gg[0] < -1.0e-5 || gg[1] < -1.0e-5 || gg[2] < -1.0e-5) continue;
            const double e = std::sqrt (gg[0] * gg[0] + gg[1] * gg[1] + gg[2] * gg[2]);
            if (e < 1.0e-12) continue;
            for (int q = 0; q < 3; ++q) g[(size_t) q] = juce::jmax (0.0, gg[q]) / e;
            idx = t.idx;
            return true;
        }
        return false;
    }

    SpeakerLayout ring (const char* name, std::initializer_list<std::pair<float, float>> dirs, std::initializer_list<int> channels = {})
    {
        SpeakerLayout L; L.name = juce::String::fromUTF8 (name);
        int i = 0;
        auto ch = channels.begin();
        for (auto& d : dirs)
        {
            Speaker s; s.az = d.first; s.el = d.second; s.channel = ch != channels.end() ? *ch++ : i + 1;
            L.speakers.push_back (s); ++i;
        }
        return L;
    }
}

std::vector<SpeakerLayout> speakerPresets()
{
    std::vector<SpeakerLayout> p;
    p.push_back (ring ("Stereo (\xc2\xb1" "30\xc2\xb0)", { { 30, 0 }, { -30, 0 } }));
    p.push_back (ring ("Quadro (\xc2\xb1" "45\xc2\xb0, \xc2\xb1" "135\xc2\xb0)", { { 45, 0 }, { -45, 0 }, { 135, 0 }, { -135, 0 } }));
    p.push_back (ring ("5.1 (ITU, Kanal 4 = LFE frei)", { { 30, 0 }, { -30, 0 }, { 0, 0 }, { 110, 0 }, { -110, 0 } }, { 1, 2, 3, 5, 6 }));
    p.push_back (ring ("7.1.4 (Kanal 4 = LFE frei)", { { 30, 0 }, { -30, 0 }, { 0, 0 }, { 90, 0 }, { -90, 0 }, { 150, 0 }, { -150, 0 },
                                                       { 45, 45 }, { -45, 45 }, { 135, 45 }, { -135, 45 } }, { 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12 }));
    p.push_back (ring ("Achteck (vorn links, gegen den Uhrzeigersinn)", { { 22.5f, 0 }, { 67.5f, 0 }, { 112.5f, 0 }, { 157.5f, 0 },
                                                                          { -157.5f, 0 }, { -112.5f, 0 }, { -67.5f, 0 }, { -22.5f, 0 } }));
    p.push_back (ring ("W\xc3\xbcrfel (8)", { { 45, 35.26f }, { 135, 35.26f }, { -135, 35.26f }, { -45, 35.26f },
                                              { 45, -35.26f }, { 135, -35.26f }, { -135, -35.26f }, { -45, -35.26f } }));
    p.push_back (ring ("Achteck + 4 oben (12)", { { 22.5f, 0 }, { 67.5f, 0 }, { 112.5f, 0 }, { 157.5f, 0 }, { -157.5f, 0 }, { -112.5f, 0 },
                                                  { -67.5f, 0 }, { -22.5f, 0 }, { 45, 45 }, { 135, 45 }, { -135, 45 }, { -45, 45 } }));
    {
        SpeakerLayout L; L.name = juce::String::fromUTF8 ("Halbkugel 12 + 6 + 1 (19)");
        int ch = 1;
        for (int i = 0; i < 12; ++i) { Speaker s; s.az = (float) wrapAz (15.0f + 30.0f * i); s.el = 0; s.channel = ch++; L.speakers.push_back (s); }
        for (int i = 0; i < 6; ++i)  { Speaker s; s.az = (float) wrapAz (30.0f + 60.0f * i); s.el = 40; s.channel = ch++; L.speakers.push_back (s); }
        { Speaker s; s.az = 0; s.el = 90; s.channel = ch++; L.speakers.push_back (s); }
        p.push_back (L);
    }
    return p;
}

bool speakerLayoutFromJson (const juce::String& json, SpeakerLayout& out, juce::String& error)
{
    const auto v = juce::JSON::parse (json);
    if (! v.isObject()) { error = "keine JSON-Datei"; return false; }
    auto ls = v["LoudspeakerLayout"];
    const auto arr = ls.isObject() ? ls["Loudspeakers"] : v["Loudspeakers"];
    if (! arr.isArray() || arr.size() == 0) { error = "kein \"Loudspeakers\"-Eintrag (IEM-Format)"; return false; }
    SpeakerLayout L;
    L.name = ls.isObject() && ls["Name"].toString().isNotEmpty() ? ls["Name"].toString() : v["Name"].toString();
    int i = 0;
    for (auto& e : *arr.getArray())
    {
        Speaker s;
        s.az = (float) (double) e.getProperty ("Azimuth", 0.0);
        s.el = (float) (double) e.getProperty ("Elevation", 0.0);
        s.radius = (float) juce::jmax (0.1, (double) e.getProperty ("Radius", 1.0));
        s.imaginary = (bool) e.getProperty ("IsImaginary", false);
        s.channel = (int) e.getProperty ("Channel", i + 1);
        const double g = (double) e.getProperty ("Gain", 1.0);
        s.gainDb = g > 1.0e-6 ? (float) (20.0 * std::log10 (g)) : -60.0f;
        L.speakers.push_back (s);
        ++i;
    }
    out = L;
    return true;
}

juce::String speakerLayoutToJson (const SpeakerLayout& layout)
{
    juce::Array<juce::var> arr;
    for (auto& s : layout.speakers)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("Azimuth", s.az);
        o->setProperty ("Elevation", s.el);
        o->setProperty ("Radius", s.radius);
        o->setProperty ("IsImaginary", s.imaginary);
        o->setProperty ("Channel", s.channel);
        o->setProperty ("Gain", std::pow (10.0, s.gainDb / 20.0));
        arr.add (juce::var (o));
    }
    auto* lay = new juce::DynamicObject();
    lay->setProperty ("Name", layout.name);
    lay->setProperty ("Loudspeakers", arr);
    auto* root = new juce::DynamicObject();
    root->setProperty ("Name", layout.name);
    root->setProperty ("Description", "Rysum");
    root->setProperty ("LoudspeakerLayout", juce::var (lay));
    return juce::JSON::toString (juce::var (root));
}

std::shared_ptr<AllRadDecoder> buildAllRad (const SpeakerLayout& layout, int order, bool distanceCompensation, uint32_t serial)
{
    auto D = std::make_shared<AllRadDecoder>();
    D->serial = serial;
    D->order = juce::jlimit (1, kMaxOrder, order);
    D->numIn = (D->order + 1) * (D->order + 1);
    std::vector<Speaker> all;
    std::vector<int> realIdx;
    for (auto& s : layout.speakers)
    {
        if (! s.imaginary)
        {
            if (s.channel < 1 || s.channel > 64) { D->error = "Kanal " + juce::String (s.channel) + juce::String::fromUTF8 (" ung\xc3\xbcltig (1\xe2\x80\x93" "64)"); return D; }
            for (int r : realIdx) if (all[(size_t) r].channel == s.channel) { D->error = "Kanal " + juce::String (s.channel) + " doppelt"; return D; }
            realIdx.push_back ((int) all.size());
        }
        all.push_back (s);
    }
    if (realIdx.size() < 2) { D->error = "mindestens zwei Lautsprecher"; return D; }
    // the same direction twice cannot be triangulated
    for (size_t a = 0; a < all.size(); ++a)
        for (size_t b = a + 1; b < all.size(); ++b)
            if (dot (toVec (all[a].az, all[a].el), toVec (all[b].az, all[b].el)) > 0.99995)
            { D->error = juce::String::fromUTF8 ("zwei Lautsprecher in derselben Richtung (") + juce::String ((int) a + 1) + ", " + juce::String ((int) b + 1) + ")"; return D; }

    // nothing above / below: imaginary loudspeakers at the poles (as one does with the IEM AllRADecoder)
    float maxEl = -90, minEl = 90;
    for (auto& s : all) { maxEl = juce::jmax (maxEl, s.el); minEl = juce::jmin (minEl, s.el); }
    auto addImag = [&] (float az, float el) { Speaker s; s.az = az; s.el = el; s.imaginary = true; s.channel = 0; all.push_back (s); ++D->addedImaginary; };
    if (maxEl < 20.0f) addImag (0.0f, 90.0f);
    if (minEl > -20.0f) addImag (0.0f, -90.0f);

    const auto virt = virtualDirections (5000);
    std::vector<V3> P;
    std::vector<Tri> tris;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        P.clear();
        for (auto& s : all) P.push_back (toVec (s.az, s.el));
        tris = hull (P);
        // gaps (e.g. only frontal loudspeakers): one more imaginary loudspeaker in the middle of the uncovered part
        V3 mean { 0, 0, 0 };
        int uncovered = 0, firstGap = -1;
        std::array<int, 3> idx; std::array<double, 3> g;
        for (size_t j = 0; j < virt.size(); ++j)
            if (! vbap (tris, virt[j], idx, g)) { ++uncovered; if (firstGap < 0) firstGap = (int) j; for (int q = 0; q < 3; ++q) mean[(size_t) q] += virt[j][(size_t) q]; }
        if (uncovered == 0) break;
        if (attempt == 7) { D->error = juce::String::fromUTF8 ("L\xc3\xbc" "cke in der Triangulation"); return D; }
        V3 dir = len (mean) > 0.3 * uncovered ? mean : virt[(size_t) firstGap];
        float az, el; dirOf (dir, az, el);
        addImag (az, el);
    }
    for (auto& t : tris) D->triangles.push_back (t.idx);
    D->all = all;

    // max-rE weights and the sampling decoder of the virtual layout (SN3D input)
    const int N = D->order, K = D->numIn;
    const double thetaE = juce::degreesToRadians (137.9 / (N + 1.51));
    std::vector<double> wn ((size_t) N + 1);
    for (int n = 0; n <= N; ++n) wn[(size_t) n] = (2 * n + 1) * legendre (n, std::cos (thetaE));
    std::vector<int> degreeOf ((size_t) K);
    for (int n = 0; n <= N; ++n) for (int m = -n; m <= n; ++m) degreeOf[(size_t) (n * n + n + m)] = n;

    const size_t R = realIdx.size();
    std::vector<std::vector<double>> M (R, std::vector<double> ((size_t) K, 0.0));
    std::vector<int> realOf (all.size(), -1);
    for (size_t r = 0; r < R; ++r) realOf[(size_t) realIdx[r]] = (int) r;
    float y[kMaxAmbiChannels];
    for (const auto& v : virt)
    {
        std::array<int, 3> idx; std::array<double, 3> g;
        if (! vbap (tris, v, idx, g)) continue;
        float az, el; dirOf (v, az, el);
        encodeAmbiX (az, el, N, y);
        for (int q = 0; q < 3; ++q)
        {
            const int r = realOf[(size_t) idx[(size_t) q]];
            if (r < 0 || g[(size_t) q] <= 0.0) continue;          // imaginary: dropped
            for (int k = 0; k < K; ++k) M[(size_t) r][(size_t) k] += g[(size_t) q] * wn[(size_t) degreeOf[(size_t) k]] * y[k];
        }
    }
    // energy: a plane wave from every direction, normalised to unit mean (as loud as the stereo / binaural output)
    auto energyAt = [&] (float az, float el, V3* rE)
    {
        encodeAmbiX (az, el, N, y);
        double e = 0; V3 re { 0, 0, 0 };
        for (size_t r = 0; r < R; ++r)
        {
            double s = 0;
            for (int k = 0; k < K; ++k) s += M[r][(size_t) k] * y[k];
            e += s * s;
            const auto u = toVec (all[(size_t) realIdx[r]].az, all[(size_t) realIdx[r]].el);
            for (int q = 0; q < 3; ++q) re[(size_t) q] += s * s * u[(size_t) q];
        }
        if (rE != nullptr) *rE = re;
        return e;
    };
    // where real loudspeakers are (not the parts covered by imaginary ones, e.g. below a dome)
    std::vector<char> realHere (virt.size(), 0);
    for (size_t j = 0; j < virt.size(); ++j)
    {
        std::array<int, 3> idx; std::array<double, 3> g;
        if (! vbap (tris, virt[j], idx, g)) continue;
        double im = 0; for (int q = 0; q < 3; ++q) if (all[(size_t) idx[(size_t) q]].imaginary) im += g[(size_t) q] * g[(size_t) q];
        realHere[j] = im <= 0.5 ? 1 : 0;
    }
    double meanE = 0; int nE = 0;
    for (size_t j = 0; j < virt.size(); ++j)
        if (realHere[j]) { float az, el; dirOf (virt[j], az, el); meanE += energyAt (az, el, nullptr); ++nE; }
    meanE /= (double) juce::jmax (1, nE);
    if (meanE < 1.0e-20) { D->error = "Decoder ohne Energie"; return D; }
    const double norm = 1.0 / std::sqrt (meanE);
    for (auto& row : M) for (auto& x : row) x *= norm;

    // quality: energy spread and localisation (rE direction) error
    double errSum = 0; float errMax = 0; float eMin = 1e9f, eMax = -1e9f; int cnt = 0;
    for (size_t j = 0; j < virt.size(); j += 5)
    {
        if (! realHere[j]) continue;
        float az, el; dirOf (virt[j], az, el);
        V3 re; const double e = energyAt (az, el, &re);
        const float db = (float) (10.0 * std::log10 (e + 1.0e-12));
        eMin = juce::jmin (eMin, db); eMax = juce::jmax (eMax, db);
        const float ang = len (re) > 1.0e-12 ? (float) juce::radiansToDegrees (std::acos (juce::jlimit (-1.0, 1.0, dot (re, virt[j]) / len (re)))) : 180.0f;
        errSum += ang; errMax = juce::jmax (errMax, ang); ++cnt;
    }
    D->energyMinDb = juce::jmax (-60.0f, eMin); D->energyMaxDb = eMax;
    D->angleErrMean = (float) (errSum / juce::jmax (1, cnt)); D->angleErrMax = errMax;
    D->energyMap.resize ((size_t) (AllRadDecoder::kMapAz * AllRadDecoder::kMapEl));
    for (int b = 0; b < AllRadDecoder::kMapEl; ++b)
        for (int a = 0; a < AllRadDecoder::kMapAz; ++a)
            D->energyMap[(size_t) (b * AllRadDecoder::kMapAz + a)] = (float) (10.0 * std::log10 (energyAt (-180.0f + 5.0f * a, -90.0f + 5.0f * b, nullptr) + 1.0e-12));

    // the matrix for the audio thread, with distance compensation (the farthest loudspeaker sets the time) and own gain
    float rMax = 0.1f;
    for (int r : realIdx) rMax = juce::jmax (rMax, all[(size_t) r].radius);
    for (size_t r = 0; r < R; ++r)
    {
        const auto& s = all[(size_t) realIdx[r]];
        float g = juce::Decibels::decibelsToGain (s.gainDb, -100.0f);
        float delay = 0.0f;
        if (distanceCompensation) { g *= s.radius / rMax; delay = (rMax - s.radius) / 343.0f; }
        std::vector<float> row ((size_t) K);
        for (int k = 0; k < K; ++k) row[(size_t) k] = (float) M[r][(size_t) k] * g;
        D->matrix.push_back (row);
        D->outChannel.push_back (s.channel - 1);
        D->delaySec.push_back (delay);
        D->gain.push_back (g);
        D->numOutChannels = juce::jmax (D->numOutChannels, s.channel);
    }
    return D;
}

// ---------------------------------------------------------------------------
void SpeakerRenderer::prepare (double sampleRate, int maxBlock)
{
    sr = sampleRate > 0 ? sampleRate : 48000.0;
    delayBuf.assign ((size_t) 64 * (size_t) kDelayLen, 0.0f);
    tmp.assign ((size_t) juce::jmax (512, maxBlock), 0.0f);
    tmpPrev.assign (tmp.size(), 0.0f);
    delayPos = 0;
    fadeLeft = 0;
    prev.reset();
}

void SpeakerRenderer::process (const float* const* ambi, int numAmbi, float* const* out, int numOut, int n) noexcept
{
    if (pending != nullptr)
    {
        prev = cur;
        cur = std::move (pending);
        pending.reset();
        fadeLen = juce::jmax (1, (int) (0.03 * sr));
        fadeLeft = prev != nullptr ? fadeLen : 0;
    }
    for (int c = 0; c < numOut; ++c) std::fill_n (out[c], n, 0.0f);
    if (cur == nullptr || n > (int) tmp.size()) { serialInUse = cur != nullptr ? cur->serial : 0; return; }
    const auto& D = *cur;
    const int K = juce::jmin (D.numIn, numAmbi);
    for (size_t r = 0; r < D.matrix.size() && r < 64; ++r)
    {
        const int ch = D.outChannel[r];
        std::fill_n (tmp.data(), n, 0.0f);
        for (int k = 0; k < K; ++k)
            if (std::abs (D.matrix[r][(size_t) k]) > 1.0e-9f) juce::FloatVectorOperations::addWithMultiply (tmp.data(), ambi[k], D.matrix[r][(size_t) k], n);
        if (fadeLeft > 0 && prev != nullptr)
        {
            // the old decoder (same loudspeaker = same output channel) fades out
            std::fill_n (tmpPrev.data(), n, 0.0f);
            for (size_t q = 0; q < prev->matrix.size(); ++q)
                if (prev->outChannel[q] == ch)
                    for (int k = 0; k < juce::jmin (prev->numIn, numAmbi); ++k)
                        juce::FloatVectorOperations::addWithMultiply (tmpPrev.data(), ambi[k], prev->matrix[q][(size_t) k], n);
            for (int i = 0; i < n; ++i)
            {
                const float t = juce::jlimit (0.0f, 1.0f, 1.0f - (float) (fadeLeft - i) / (float) fadeLen);
                tmp[(size_t) i] = t * tmp[(size_t) i] + (1.0f - t) * tmpPrev[(size_t) i];
            }
        }
        // distance compensation: a plain delay per loudspeaker
        float* ring = delayBuf.data() + r * (size_t) kDelayLen;
        const uint32_t d = (uint32_t) juce::jlimit (0, kDelayLen - 1 - n, (int) std::lround (D.delaySec[r] * sr));
        for (int i = 0; i < n; ++i) ring[(delayPos + (uint32_t) i) & (kDelayLen - 1)] = tmp[(size_t) i];
        if (ch >= 0 && ch < numOut)
            for (int i = 0; i < n; ++i) out[ch][i] += ring[(delayPos + (uint32_t) i - d) & (kDelayLen - 1)];
    }
    // loudspeakers of the old decoder that the new one does not have
    if (fadeLeft > 0 && prev != nullptr)
        for (size_t q = 0; q < prev->matrix.size(); ++q)
        {
            const int ch = prev->outChannel[q];
            if (std::find (D.outChannel.begin(), D.outChannel.end(), ch) != D.outChannel.end() || ch < 0 || ch >= numOut) continue;
            for (int k = 0; k < juce::jmin (prev->numIn, numAmbi); ++k)
                for (int i = 0; i < n; ++i)
                    out[ch][i] += prev->matrix[q][(size_t) k] * ambi[k][i] * juce::jlimit (0.0f, 1.0f, (float) (fadeLeft - i) / (float) fadeLen);
        }
    if (fadeLeft > 0 && (fadeLeft -= n) <= 0) { fadeLeft = 0; prev.reset(); }
    delayPos += (uint32_t) n;
    serialInUse = prev != nullptr ? juce::jmin (prev->serial, cur->serial) : cur->serial;
}
} // namespace rysum
