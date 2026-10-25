#include "Convolver.h"

namespace rysum
{
std::unique_ptr<Convolver> Convolver::create (const juce::File& file, double hostRate, Format format, juce::String& error)
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

    // ---- channel format
    int order = -1;
    for (int o = 1; o <= 7; ++o) if ((o + 1) * (o + 1) == ch) order = o;
    int keep = ch;
    if (order >= 0) keep = juce::jmin (ch, kMaxAmbiChannels), order = juce::jmin (order, kMaxOrder);
    else if (ch > 2) keep = 2;                       // unknown multichannel: use the first two as stereo

    juce::AudioBuffer<float> ir (keep, srcLen);
    if (order == 1 && format == Format::FuMa)
    {
        // FuMa W X Y Z  ->  AmbiX (ACN/SN3D) W Y Z X, W scaled by sqrt(2)
        ir.copyFrom (0, 0, src, 0, 0, srcLen); ir.applyGain (0, 0, srcLen, std::sqrt (2.0f));
        ir.copyFrom (1, 0, src, 2, 0, srcLen);
        ir.copyFrom (2, 0, src, 3, 0, srcLen);
        ir.copyFrom (3, 0, src, 1, 0, srcLen);
    }
    else
        for (int c = 0; c < keep; ++c) ir.copyFrom (c, 0, src, c, 0, srcLen);

    // ---- sample-rate conversion to the host rate
    juce::AudioBuffer<float> rs;
    const double ratio = r->sampleRate / hostRate;
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
    double e = 0;
    for (int i = 0; i < len; ++i) e += (double) rs.getSample (0, i) * rs.getSample (0, i);
    const float g = e > 0 ? (float) (1.0 / std::sqrt (e)) : 1.0f;
    // short fade-out to avoid a hard cut
    const int fade = juce::jmin (len / 4, (int) (0.05 * hostRate));
    for (int c = 0; c < keep; ++c)
    {
        rs.applyGain (c, 0, len, g);
        rs.applyGainRamp (c, len - fade, fade, 1.0f, 0.0f);
    }

    // ---- partition + FFT
    std::unique_ptr<Convolver> cv (new Convolver());
    cv->numIr = keep;
    cv->numParts = (len + kBlock - 1) / kBlock;
    const size_t perCh = (size_t) cv->numParts * kBins;
    cv->irRe.assign ((size_t) keep * perCh, 0.0f);
    cv->irIm.assign ((size_t) keep * perCh, 0.0f);
    std::vector<float> tmp ((size_t) kFft * 2, 0.0f);
    for (int c = 0; c < keep; ++c)
        for (int p = 0; p < cv->numParts; ++p)
        {
            std::fill (tmp.begin(), tmp.end(), 0.0f);
            const int start = p * kBlock, n = juce::jmin (kBlock, len - start);
            for (int i = 0; i < n; ++i) tmp[(size_t) i] = rs.getSample (c, start + i);
            cv->fft.performRealOnlyForwardTransform (tmp.data(), true);
            const size_t base = (size_t) c * perCh + (size_t) p * kBins;
            for (int b = 0; b < kBins; ++b)
            {
                cv->irRe[base + (size_t) b] = tmp[(size_t) (2 * b)];
                cv->irIm[base + (size_t) b] = tmp[(size_t) (2 * b + 1)];
            }
        }

    cv->fdlRe.assign ((size_t) cv->numParts * kBins, 0.0f);
    cv->fdlIm.assign ((size_t) cv->numParts * kBins, 0.0f);
    cv->inBuf.assign ((size_t) kFft, 0.0f);
    cv->work.assign ((size_t) kFft * 2, 0.0f);
    cv->accRe.assign ((size_t) kBins, 0.0f);
    cv->accIm.assign ((size_t) kBins, 0.0f);
    cv->outBuf.assign ((size_t) keep * kBlock, 0.0f);

    auto& in = cv->info;
    in.fileName = file.getFileName();
    in.channels = ch;
    in.order = order;
    in.seconds = len / hostRate;
    if (order >= 0) in.description << "AmbiX " << order << ". Ord." << (format == Format::FuMa && order == 1 ? " (aus FuMa)" : "");
    else in.description << (keep == 1 ? "Mono" : "Stereo");
    in.description << juce::String::fromUTF8 (" \xc2\xb7 ") << juce::String (in.seconds, 1) << " s";
    return cv;
}

void Convolver::reset() noexcept
{
    std::fill (fdlRe.begin(), fdlRe.end(), 0.0f);
    std::fill (fdlIm.begin(), fdlIm.end(), 0.0f);
    std::fill (inBuf.begin(), inBuf.end(), 0.0f);
    std::fill (outBuf.begin(), outBuf.end(), 0.0f);
    pos = 0;
}

void Convolver::processBlock() noexcept
{
    // input spectrum of [previous block | current block]
    std::copy (inBuf.begin(), inBuf.end(), work.begin());
    std::fill (work.begin() + kFft, work.end(), 0.0f);
    fft.performRealOnlyForwardTransform (work.data(), true);
    fdlPos = (fdlPos + numParts - 1) % numParts;               // newest spectrum goes to fdlPos
    float* xr = fdlRe.data() + (size_t) fdlPos * kBins;
    float* xi = fdlIm.data() + (size_t) fdlPos * kBins;
    for (int b = 0; b < kBins; ++b) { xr[b] = work[(size_t) (2 * b)]; xi[b] = work[(size_t) (2 * b + 1)]; }

    const size_t perCh = (size_t) numParts * kBins;
    for (int c = 0; c < numIr; ++c)
    {
        std::fill (accRe.begin(), accRe.end(), 0.0f);
        std::fill (accIm.begin(), accIm.end(), 0.0f);
        float* ar = accRe.data();
        float* ai = accIm.data();
        for (int p = 0; p < numParts; ++p)
        {
            const int slot = (fdlPos + p) % numParts;          // input from p blocks ago
            const float* sr = fdlRe.data() + (size_t) slot * kBins;
            const float* si = fdlIm.data() + (size_t) slot * kBins;
            const float* hr = irRe.data() + (size_t) c * perCh + (size_t) p * kBins;
            const float* hi = irIm.data() + (size_t) c * perCh + (size_t) p * kBins;
            for (int b = 0; b < kBins; ++b)
            {
                ar[b] += sr[b] * hr[b] - si[b] * hi[b];
                ai[b] += sr[b] * hi[b] + si[b] * hr[b];
            }
        }
        for (int b = 0; b < kBins; ++b) { work[(size_t) (2 * b)] = ar[b]; work[(size_t) (2 * b + 1)] = ai[b]; }
        std::fill (work.begin() + 2 * kBins, work.end(), 0.0f);
        fft.performRealOnlyInverseTransform (work.data());
        // overlap-save: the second half is the valid linear-convolution output
        std::copy (work.begin() + kBlock, work.begin() + kFft, outBuf.begin() + (size_t) c * kBlock);
    }

    // slide input: current block becomes "previous"
    std::copy (inBuf.begin() + kBlock, inBuf.end(), inBuf.begin());
}

void Convolver::process (const float* input, int n, float* const* out, int numOut, bool stereoOut, float wetGain) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        inBuf[(size_t) (kBlock + pos)] = input[i];

        // wet output of the previous block (latency kBlock)
        auto y = [this] (int c) { return outBuf[(size_t) c * kBlock + (size_t) pos]; };
        if (stereoOut)
        {
            if (numIr == 1)      { const float m = y (0) * 0.7071f * wetGain; out[0][i] += m; if (numOut > 1) out[1][i] += m; }
            else if (numIr == 2) { out[0][i] += y (0) * wetGain; if (numOut > 1) out[1][i] += y (1) * wetGain; }
            else
            {
                // Ambisonic IR to stereo: two virtual cardioids pointing left / right
                const float w = y (0), l = y (1);
                out[0][i] += 0.5f * (w + l) * wetGain;
                if (numOut > 1) out[1][i] += 0.5f * (w - l) * wetGain;
            }
        }
        else
        {
            if (numIr == 2)
            {
                // stereo IR into Ambisonics: W = mid, Y = side
                out[0][i] += 0.5f * (y (0) + y (1)) * wetGain;
                if (numOut > 1) out[1][i] += 0.5f * (y (0) - y (1)) * wetGain;
            }
            else
            {
                const int m = juce::jmin (numIr, numOut);
                for (int c = 0; c < m; ++c) out[c][i] += y (c) * wetGain;
            }
        }

        if (++pos == kBlock) { pos = 0; processBlock(); }
    }
}
} // namespace rysum
