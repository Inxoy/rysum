#include "LucierPiece.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

namespace rysum
{
void lucierLevelMatch (std::vector<float>& x, double targetRms)
{
    double e = 0.0; float pk = 0.0f;
    for (auto v : x) { e += (double) v * v; pk = juce::jmax (pk, std::abs (v)); }
    if (x.empty() || e <= 0.0) return;
    const double rms = std::sqrt (e / (double) x.size());
    double g = targetRms / rms;
    if (pk * g > 0.89) g = 0.89 / pk;                       // never above -1 dBFS
    for (auto& v : x) v = (float) (v * g);
}

void LucierPiece::prepare (double sampleRate)
{
    sr = sampleRate > 0 ? sampleRate : 48000.0;
}

int64_t LucierPiece::maxLen() const { return (int64_t) recording.size() + (int64_t) (3.0 * recRate); }

void LucierPiece::process (const float* rec, float* const* out, const float* outGain, int numOut, int n, float mix) noexcept
{
    const int st = state.load (std::memory_order_acquire);
    // ---- recording
    if (st == stRecording && rec != nullptr && ! recFull.load())
    {
        const int64_t pos = recPos.load (std::memory_order_relaxed);
        const int m = (int) juce::jmin ((int64_t) n, recCap - pos);
        if (m > 0) std::copy_n (rec, m, recBuf.data() + pos);
        recPos.store (pos + m, std::memory_order_release);
        if (pos + m >= recCap) recFull = true;
    }
    // ---- playback
    const bool on = st == stPlaying;
    const int epoch = playEpoch.load (std::memory_order_acquire);
    if (epoch != seenEpoch) { seenEpoch = epoch; curValid = false; nextSlot = 0; gapLeft = 0; }
    const float wetT = on ? std::sin (juce::jlimit (0.0f, 1.0f, mix) * juce::MathConstants<float>::halfPi) : 0.0f;
    const float dryT = on ? std::cos (juce::jlimit (0.0f, 1.0f, mix) * juce::MathConstants<float>::halfPi) : 1.0f;
    if (! on && wetGain < 1.0e-5f && std::abs (dryGain - 1.0f) < 1.0e-5f) { wetGain = 0.0f; dryGain = 1.0f; level = level.load() * 0.9f; return; }
    const float k = 1.0f - std::exp (-(float) n / (0.05f * (float) sr));
    const float w0 = wetGain, w1 = wetGain + (wetT - wetGain) * k;
    const float d0 = dryGain, d1 = dryGain + (dryT - dryGain) * k;
    wetGain = w1; dryGain = d1;
    float pk = 0.0f;
    const int gapTotal = gapSamples.load();
    for (int i = 0; i < n; ++i)
    {
        float y = 0.0f;
        if (on)
        {
            if (! curValid)
            {
                if (gapLeft > 0) --gapLeft;
                else if (slotReady[(size_t) nextSlot].load (std::memory_order_acquire))
                {
                    const auto& s = slots[(size_t) nextSlot];
                    curData = s.data->data();
                    curLen = (int64_t) s.data->size();
                    curPos = 0;
                    curValid = curLen > 0;
                    playingGen = s.gen;
                    playSlot = nextSlot;
                    slotReady[(size_t) nextSlot].store (false, std::memory_order_release);
                    nextSlot ^= 1;
                    playLen = curLen;
                }
            }
            if (curValid)
            {
                y = curData[curPos++];
                if (curPos >= curLen)
                {
                    curValid = false;
                    gapLeft = gapTotal;
                    if (playingGen.load() >= lastGen.load()) state.store (stFinished, std::memory_order_release);
                }
            }
        }
        const float t = (float) (i + 1) / (float) n;
        const float dg = d0 + (d1 - d0) * t, wg = w0 + (w1 - w0) * t;
        for (int c = 0; c < numOut; ++c) out[c][i] = out[c][i] * dg + y * wg * outGain[c];
        pk = juce::jmax (pk, std::abs (y));
    }
    playPos = curValid ? curPos : 0;
    level = juce::jmax (pk, level.load() * 0.9f);
}

// ---------------------------------------------------------------------------
void LucierPiece::retire()
{
    for (auto& s : slots) if (s.data != nullptr) retired.push_back (s.data);
    retiredAt = juce::Time::getMillisecondCounterHiRes();
}

bool LucierPiece::startRecording (double maxSeconds)
{
    stop();
    // the old buffer may still be written by an audio block in flight: kept, a new one is made
    if (recBuf.size() > 0) retiredRec.push_back (std::make_shared<std::vector<float>> (std::move (recBuf)));
    recCap = (int64_t) (maxSeconds * sr);
    recBuf.assign ((size_t) recCap, 0.0f);
    recPos = 0;
    recFull = false;
    recRate = sr;
    state.store (stRecording, std::memory_order_release);
    return true;
}

void LucierPiece::stopRecording (bool thenPlay)
{
    if (state.load() != stRecording) return;
    state.store (stIdle, std::memory_order_release);
    const int64_t n = recPos.load (std::memory_order_acquire);
    std::vector<float> r (recBuf.begin(), recBuf.begin() + (std::ptrdiff_t) n);
    setRecording (std::move (r), sr);
    if (thenPlay) play();
}

void LucierPiece::setRecording (std::vector<float> data, double rate)
{
    // silence at both ends goes (more than a fifth of a second)
    float pk = 0.0f;
    for (auto v : data) pk = juce::jmax (pk, std::abs (v));
    if (pk > 0.0f)
    {
        const float thr = pk * 0.003f;
        size_t a = 0, b = data.size();
        while (a < b && std::abs (data[a]) < thr) ++a;
        while (b > a && std::abs (data[b - 1]) < thr) --b;
        const size_t keep = (size_t) (0.2 * rate);
        a = a > keep ? a - keep : 0;
        b = juce::jmin (data.size(), b + keep);
        data = std::vector<float> (data.begin() + (std::ptrdiff_t) a, data.begin() + (std::ptrdiff_t) b);
    }
    recording = std::move (data);
    recRate = rate;
    ++recordingVersion;
    double e = 0.0;
    for (auto v : recording) e += (double) v * v;
    recRms = recording.empty() ? 0.0 : std::sqrt (e / (double) recording.size());
}

void LucierPiece::play()
{
    if (recording.empty()) return;
    if (state.load() == stRecording) state.store (stIdle, std::memory_order_release);
    if (jobCancel) jobCancel->store (true);
    jobCancel.reset();
    computingGen = -1;
    retire();
    slotReady[0] = false; slotReady[1] = false;
    slots[0] = { std::make_shared<std::vector<float>> (recording), 0 };
    slots[1] = {};
    lastComputed = slots[0].data;
    lastComputedGen = 0;
    playingGen = 0;
    slotReady[0].store (true, std::memory_order_release);
    playEpoch.fetch_add (1, std::memory_order_acq_rel);
    state.store (stPlaying, std::memory_order_release);
}

void LucierPiece::stop()
{
    if (state.load() == stRecording) { stopRecording (false); return; }
    state.store (stIdle, std::memory_order_release);
    if (jobCancel) jobCancel->store (true);
    jobCancel.reset();
    computingGen = -1;
}

void LucierPiece::tick (std::shared_ptr<const LucierBase> base, int lastGeneration, float gapSeconds, juce::ThreadPool& pool,
                        std::shared_ptr<std::atomic<bool>> alive)
{
    lastGen = juce::jmax (0, lastGeneration);
    gapSamples = (int) (juce::jmax (0.0f, gapSeconds) * sr);
    if (! retired.empty() && juce::Time::getMillisecondCounterHiRes() - retiredAt > 1000.0) retired.clear();
    if (state.load() == stRecording && recFull.load()) stopRecording (true);
    if (state.load() != stPlaying || base == nullptr || computingGen.load() >= 0) return;
    const int nextGen = lastComputedGen + 1;
    if (nextGen > lastGeneration || lastComputed == nullptr) return;
    const int free = playSlot.load() ^ 1;
    if (slotReady[(size_t) free].load() || (slots[(size_t) free].data != nullptr && slots[(size_t) free].data == lastComputed)) return;
    // the room's pass (cached per room)
    if (base != passBase) { passBase = base; pass = std::make_shared<std::vector<float>> (lucierPassResponse (*base)); }
    computingGen = nextGen;
    auto cancel = std::make_shared<std::atomic<bool>> (false);
    jobCancel = cancel;
    auto src = lastComputed;
    auto ps = pass;
    const int64_t ml = maxLen();
    const double target = recRms;
    pool.addJob ([this, src, ps, ml, target, nextGen, free, cancel, alive]
    {
        auto y = std::make_shared<std::vector<float>> (lucierPass (*src, *ps, (int) ml, [cancel] { return cancel->load(); }));
        if (cancel->load() || y->empty()) return;
        lucierLevelMatch (*y, target);
        juce::MessageManager::callAsync ([this, y, nextGen, free, cancel, alive]
        {
            if (! alive->load() || cancel->load()) return;
            computingGen = -1;
            if (state.load() != stPlaying || slotReady[(size_t) free].load() || playSlot.load() == free) return;
            if (slots[(size_t) free].data != nullptr) retired.push_back (slots[(size_t) free].data);
            retiredAt = juce::Time::getMillisecondCounterHiRes();
            slots[(size_t) free] = { y, nextGen };
            lastComputed = y;
            lastComputedGen = nextGen;
            slotReady[(size_t) free].store (true, std::memory_order_release);
        });
    });
}

void LucierPiece::exportPiece (const juce::File& f, std::shared_ptr<const LucierBase> base, int lastGeneration, float gapSeconds, juce::ThreadPool& pool)
{
    if (recording.empty() || base == nullptr || exportProgress.load() >= 0.0f) return;
    exportProgress = 0.0f;
    auto rec = std::make_shared<std::vector<float>> (recording);
    const double rate = recRate, target = recRms;
    const int64_t ml = maxLen();
    pool.addJob ([this, f, base, lastGeneration, gapSeconds, rec, rate, target, ml]
    {
        const auto passResp = lucierPassResponse (*base);
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = f.createOutputStream();
        if (stream == nullptr) { exportProgress = -1.0f; return; }
        const auto options = juce::AudioFormatWriterOptions{}
            .withSampleRate (rate).withNumChannels (1).withBitsPerSample (24);
        auto w = wav.createWriterFor (stream, options);
        if (w == nullptr) { exportProgress = -1.0f; return; }
        std::vector<float> gap ((size_t) (juce::jmax (0.0f, gapSeconds) * rate), 0.0f);
        auto write = [&] (const std::vector<float>& x)
        {
            const float* ch[] { x.data() };
            w->writeFromFloatArrays (ch, 1, (int) x.size());
        };
        std::vector<float> cur = *rec;
        write (cur);
        for (int g = 1; g <= lastGeneration; ++g)
        {
            if (exportCancel.load()) break;
            auto next = lucierPass (cur, passResp, (int) ml, [this] { return exportCancel.load(); });
            if (next.empty()) break;
            lucierLevelMatch (next, target);
            write (gap);
            write (next);
            cur = std::move (next);
            exportProgress = (float) g / (float) juce::jmax (1, lastGeneration);
        }
        w.reset();
        exportProgress = -1.0f;
    });
}
} // namespace rysum
