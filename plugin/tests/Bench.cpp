// CPU benchmark of the whole plugin (processor + engine + room + decoders), no host needed.
//   RysumBench <samples folder> [block size] [seconds per scenario] [scenario filter]
// For every scenario: average load (% of real time, one core) and the worst single block (% of its
// real-time budget) - the worst block is what causes dropouts in a host.
#include "PluginProcessor.h"
#if __has_include("Kernels.h")
#include "Kernels.h"
#define RYSUM_BENCH_KERNELS 1
#endif
#include <chrono>
#if __has_include("WindChaosOld.h")
#include "WindChaosOld.h" // (a copy of an earlier WindChaos, renamed, for comparing by ear: not in the repository)
#define RYSUM_BENCH_WIND_OLD 1
#endif
#if JUCE_WINDOWS
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread();
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void *, int);
#endif

class BenchApp : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override
    {
        return "RysumBench";
    }
    const juce::String getApplicationVersion() override
    {
        return "0.1";
    }

    void initialise(const juce::String &cmd) override
    {
        auto args = juce::StringArray::fromTokens(cmd, true);
        if (args.size() < 1)
        {
            std::cout << "usage: RysumBench <samples> [block] [seconds] [filter]" << std::endl;
            quit();
            return;
        }
        samples = juce::File::getCurrentWorkingDirectory().getChildFile(args[0].unquoted());
        if (args.size() > 1)
            block = juce::jlimit(16, 4096, args[1].getIntValue());
        if (args.size() > 2)
            seconds = juce::jmax(1.0, args[2].getDoubleValue());
        if (args.size() > 3)
            filter = args[3];
        proc = std::make_unique<RysumProcessor>();
        proc->loadSamples(samples);
        startTimer(50);
    }

    void shutdown() override
    {
        proc.reset();
    }

    juce::RangedAudioParameter *P(const char *id)
    {
        return proc->apvts.getParameter(id);
    }
    void set(const char *id, float plain)
    {
        if (auto *p = P(id))
            p->setValueNotifyingHost(p->convertTo0to1(plain));
    }
    void pump(int ms)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
    }

    void writeIr(const juce::File &f, int ch, double secs)
    {
        const int len = (int)(secs * 48000.0);
        juce::AudioBuffer<float> ir(ch, len);
        juce::Random rnd(5);
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < len; ++i)
                ir.setSample(c, i,
                             (rnd.nextFloat() * 2 - 1) * std::exp(-4.0f * (float)i / (float)len) *
                                 (c == 0 ? 1.0f : 0.5f));
        for (int c = 0; c < ch; ++c)
            ir.setSample(c, 200, c == 0 ? 1.0f : 0.3f);
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os(f.createOutputStream());
        if (auto w = wav.createWriterFor(os, juce::AudioFormatWriterOptions()
                                                 .withSampleRate(48000.0)
                                                 .withNumChannels(ch)
                                                 .withBitsPerSample(24)))
            w->writeFromAudioSampleBuffer(ir, 0, len);
    }

    void resetParams()
    {
        for (auto *prm : proc->getParameters())
            if (auto *r = dynamic_cast<juce::RangedAudioParameter *>(prm))
                r->setValueNotifyingHost(r->getDefaultValue());
        proc->clearIr();
        pump(100);
    }

    struct Result
    {
        double avg = 0, worst = 0, p99 = 0, p999 = 0;
        float rms = 0;
    };

    Result run(const juce::String &name, int outChannels)
    {
        juce::AudioProcessor::BusesLayout lay;
        lay.outputBuses.add(outChannels == 2 ? juce::AudioChannelSet::stereo()
                                             : juce::AudioChannelSet::ambisonic(5));
        proc->setBusesLayout(lay);
        proc->setRateAndBufferSizeDetails(48000.0, block);
        proc->prepareToPlay(48000.0, block);
        pump(50);
        juce::AudioBuffer<float> buf(proc->getTotalNumOutputChannels(), block);
        const int chord[]{41, 48, 53, 57, 60, 64, 65, 69, 72, 76};
        // warm up: let background builds (IR, HRTF decoder, room tail) finish while playing
        for (int b = 0; b < (int)(2.0 * 48000 / block); ++b)
        {
            juce::MidiBuffer mb;
            if (b == 0)
                for (int m : chord)
                    mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), 0);
            buf.clear();
            proc->processBlock(buf, mb);
            if (b % 8 == 0)
                pump(5);
        }
        const int blocks = (int)(seconds * 48000 / block);
#if JUCE_WINDOWS
        SetThreadPriority(GetCurrentThread(), 15); // (time critical, as a host's audio thread)
#endif
        proc->engine.prof.fill(0.0);
        proc->engine.profOn = true;
        std::vector<double> t;
        t.reserve((size_t)blocks);
        // optional: the measured part as a WAV file (comparing two builds): environment variable
        // RYSUM_BENCH_WAV = folder
        const auto wavDir = juce::SystemStats::getEnvironmentVariable("RYSUM_BENCH_WAV", {});
        juce::AudioBuffer<float> rec(wavDir.isNotEmpty() ? buf.getNumChannels() : 0,
                                     wavDir.isNotEmpty() ? blocks * block : 0);
        double e = 0;
        long cnt = 0;
        for (int b = 0; b < blocks; ++b)
        {
            juce::MidiBuffer mb;
            // re-strike a few notes now and then (attacks, clicks, releases)
            if (b % (int)(0.7 * 48000 / block) == 0)
            {
                const int m = chord[(size_t)((b / 7) % 10)];
                mb.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)90), block / 2);
            }
            buf.clear();
            const auto t0 = std::chrono::steady_clock::now();
            proc->processBlock(buf, mb);
            const auto t1 = std::chrono::steady_clock::now();
            t.push_back(std::chrono::duration<double>(t1 - t0).count());
            for (int i = 0; i < block; ++i)
            {
                const float v = buf.getSample(0, i);
                e += (double)v * v;
                ++cnt;
            }
            for (int c = 0; c < rec.getNumChannels(); ++c)
                rec.copyFrom(c, b * block, buf, c, 0, block);
        }
        if (rec.getNumChannels() > 0)
        {
            auto f = juce::File(wavDir).getChildFile(name + ".wav");
            f.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os(f.createOutputStream());
            if (auto w = wav.createWriterFor(
                    os, juce::AudioFormatWriterOptions()
                            .withSampleRate(48000.0)
                            .withNumChannels(rec.getNumChannels())
                            .withBitsPerSample(32)
                            .withSampleFormat(
                                juce::AudioFormatWriterOptions::SampleFormat::floatingPoint)))
                w->writeFromAudioSampleBuffer(rec, 0, rec.getNumSamples());
        }
        juce::MidiBuffer off;
        for (int m = 36; m < 82; ++m)
            off.addEvent(juce::MidiMessage::noteOff(1, m), 0);
        buf.clear();
        proc->processBlock(buf, off);
        const double budget = block / 48000.0;
        Result r;
        double sum = 0;
        for (double x : t)
            sum += x;
        r.avg = 100.0 * sum / (blocks * budget);
        auto s = t;
        std::sort(s.begin(), s.end());
        r.worst = 100.0 * s.back() / budget;
        r.p99 = 100.0 * s[(size_t)((double)(s.size() - 1) * 0.99)] / budget;
        r.p999 = 100.0 * s[(size_t)((double)(s.size() - 1) * 0.999)] / budget;
        r.rms = (float)std::sqrt(e / juce::jmax(1L, cnt));
        std::cout << juce::String(name).paddedRight(' ', 26) << "  avg "
                  << juce::String(r.avg, 1).paddedLeft(' ', 6) << " %"
                  << "   p99 " << juce::String(r.p99, 1).paddedLeft(' ', 6) << " %"
                  << "   p99.9 " << juce::String(r.p999, 1).paddedLeft(' ', 6) << " %"
                  << "   worst " << juce::String(r.worst, 1).paddedLeft(' ', 6) << " %"
                  << "   rms " << juce::String(juce::Decibels::gainToDecibels(r.rms), 2) << " dB"
                  << std::endl;
        {
            // where the time went, in % of real time: the engine's stages, and what follows it
            // (impulse response, decoders, ...)
            proc->engine.profOn = false;
            const auto &pf = proc->engine.prof;
            const double toPct = 100.0 / (1000.0 * blocks * budget);
            double eng = 0;
            for (int i = 0; i < 5; ++i)
                eng += pf[(size_t)i];
            auto pc = [&](double ms) { return juce::String(ms * toPct, 1).paddedLeft(' ', 5); };
            std::cout << "      before " << pc(pf[0]) << " (motion " << pc(pf[6]) << ", places "
                      << pc(pf[7]) << ")  pipes " << pc(pf[1]) << "  extras "
                      << pc(pf[2]) << "  reflections " << pc(pf[3]) << " (geometry " << pc(pf[5])
                      << ")  tail " << pc(pf[4]) << "  after the engine "
                      << juce::String(r.avg - eng * toPct, 1).paddedLeft(' ', 5) << "   voices "
                      << proc->engine.activeVoices.load() << ", reflected " << proc->engine.erActive.load()
                      << std::endl;
            if (juce::SystemStats::getEnvironmentVariable("RYSUM_BENCH_PLACES", {}).isNotEmpty())
                std::cout << "      places: where " << pc(pf[8]) << "  around the corner " << pc(pf[9])
                          << "  pitch " << pc(pf[10]) << "  gains " << pc(pf[11]) << std::endl;
        }
        return r;
    }

    void allStops(bool moving)
    {
        for (int s = 0; s < rysum::kNumStops; ++s)
        {
            P(RysumProcessor::stopParamId(s).toRawUTF8())->setValueNotifyingHost(1.0f);
            if (moving)
                set(RysumProcessor::motionParamId(s).toRawUTF8(), (float)(1 + s % 3));
        }
    }

    bool want(const char *name) const
    {
        return filter.isEmpty() || juce::String(name).contains(filter);
    }

    void timerCallback() override
    {
        if (proc->getLibrary() == nullptr && ++waited < 400)
            return;
        stopTimer();
        std::cout << "status: " << proc->getLoadStatus() << "   block " << block << ", " << seconds
                  << " s per scenario";
#if RYSUM_BENCH_KERNELS
        std::cout << ", kernels " << rysum::kern::get().name;
#endif
        std::cout << std::endl;

        auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory);
        auto irFoa = tmp.getChildFile("RysumBench_foa.wav");
        auto irMono = tmp.getChildFile("RysumBench_mono.wav");
        writeIr(irFoa, 4, 3.0);
        writeIr(irMono, 1, 3.0);
        auto loadIr = [&](const juce::File &f)
        {
            proc->loadIr(f);
            for (int w = 0; w < 200 && !(proc->getIrStatus().contains("AmbiX") ||
                                         proc->getIrStatus().contains("Mono"));
                 ++w)
                pump(20);
        };

        if (want("stereo_dry"))
        {
            resetParams();
            allStops(false);
            set("output", 0);
            run("stereo_dry", 2);
        }
        if (want("stereo_move_ir"))
        {
            resetParams();
            allStops(true);
            set("output", 0);
            loadIr(irMono);
            run("stereo_move_ir", 2);
        }
        if (want("ambi3_move_ir"))
        {
            resetParams();
            allStops(true);
            set("output", 3);
            loadIr(irFoa);
            run("ambi3_move_ir", 36);
        }
        if (want("ambi5_move_ir"))
        {
            resetParams();
            allStops(true);
            set("output", 5);
            set("irOrder", 4);
            loadIr(irFoa);
            run("ambi5_move_ir", 36);
        }
        if (want("binaural3_ir"))
        {
            resetParams();
            allStops(true);
            set("output", 6);
            loadIr(irFoa);
            run("binaural3_ir", 2);
        }
        if (want("binaural5_ir"))
        {
            resetParams();
            allStops(true);
            set("output", 6);
            set("binOrder", 4);
            set("irOrder", 4);
            loadIr(irFoa);
            run("binaural5_ir", 2);
        }
        if (want("ambi3_saal"))
        {
            resetParams();
            allStops(true);
            set("output", 3);
            set("simOn", 1);
            run("ambi3_saal", 36);
        }
        if (want("ambi3_saal_er"))
        {
            resetParams();
            allStops(true);
            set("output", 3);
            set("simOn", 1);
            set("simOrder", 2);
            set("simTail", -60.0f);
            run("ambi3_saal_er", 36);
        }
        if (want("binaural3_saal"))
        {
            resetParams();
            allStops(true);
            set("output", 6);
            set("simOn", 1);
            set("simOrder", 2);
            run("binaural3_saal", 2);
        }
        if (want("ambi5_fx_deg"))
        {
            resetParams();
            allStops(true);
            set("output", 5);
            set("degAmount", 0.5f);
            set("degGrain", 0.4f);
            for (auto *id : {"fxCrackLevel", "fxPartSpread", "fxEchoLevel", "fxLeakLevel"})
                if (auto *p = P(id))
                    p->setValueNotifyingHost(0.5f);
            set("wind", 0.5f);
            set("tremOn", 1);
            loadIr(irFoa);
            run("ambi5_fx_deg", 36);
        }
        if (want("saal_shape"))
        {
            // a shaped room (the reflections' paths are searched for), the pipes flying in it
            resetParams();
            allStops(false);
            for (int s = 0; s < rysum::kNumStops; ++s)
                set(RysumProcessor::motionParamId(s).toRawUTF8(), (float)(8 + s % 4));
            set("output", 3);
            set("simOn", 1);
            set("simShape", (float)rysum::shapeL);
            set("simShapeAmt", 0.9f);
            run("ambi3_saal_shape", 36);
            set("simIce", 0.6f);
            run("ambi3_saal_shape_eis", 36);
        }
        if (want("wind") && juce::SystemStats::getEnvironmentVariable("RYSUM_BENCH_WAV", {}).isNotEmpty())
        {
            // what the unstable wind model adds, alone (no pipe): for four pipes first a steady
            // wind of 25 %, then the wind sinking from 50 % to 5 % - mono, 48 kHz
            const juce::File dir(juce::SystemStats::getEnvironmentVariable("RYSUM_BENCH_WAV", {}));
            auto render = [&](auto &chaos, const juce::String &name)
            {
                const float sr = 48000.0f;
                juce::AudioBuffer<float> out(1, (int)(4 * 7.5f * sr));
                out.clear();
                int at = 0;
                int pipeNo = 0;
                for (float f0 : {131.0f, 262.0f, 523.0f, 1046.0f})
                {
                    chaos.reset(77u + (uint32_t)pipeNo, (uint32_t)(3 * 64 + 12 * pipeNo + 5), f0, sr);
                    const int len = (int)(7.0f * sr);
                    for (int i = 0; i < len; ++i)
                    {
                        const float t = (float)i / sr;
                        const float w = t < 3.0f ? 0.25f : 0.5f - 0.45f * (t - 3.0f) / 4.0f;
                        if ((i & 15) == 0)
                            chaos.control(w, f0 * (1.0f - 0.03f * (1.0f - w)), false, false, true, sr);
                        out.setSample(0, at + i, chaos.idle ? 0.0f : 0.25f * chaos.sample());
                    }
                    at += len + (int)(0.5f * sr);
                    ++pipeNo;
                }
                auto f = dir.getChildFile(name);
                f.deleteFile();
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::OutputStream> os(f.createOutputStream());
                if (auto wr = wav.createWriterFor(os, juce::AudioFormatWriterOptions()
                                                          .withSampleRate(48000.0)
                                                          .withNumChannels(1)
                                                          .withBitsPerSample(16)))
                    wr->writeFromAudioSampleBuffer(out, 0, out.getNumSamples());
            };
            rysum::WindChaos now;
            render(now, "mundton_neu.wav");
#if RYSUM_BENCH_WIND_OLD
            rysum::WindChaosOld old;
            render(old, "mundton_alt.wav");
#endif
        }
        if (want("wind"))
        {
            // half-drawn stops, dry, in both wind models (for listening and comparing: RYSUM_BENCH_WAV)
            for (int model = 0; model < 2; ++model)
                for (float w : {0.45f, 0.25f, 0.12f})
                {
                    resetParams();
                    set("output", 0);
                    set("windModel", (float)model);
                    for (int s : {0, 1, 3})
                    {
                        P(RysumProcessor::stopParamId(s).toRawUTF8())->setValueNotifyingHost(1.0f);
                        set(RysumProcessor::windParamId(s).toRawUTF8(), w);
                    }
                    run(juce::String(model == 0 ? "wind_klassisch_" : "wind_instabil_") +
                            juce::String(juce::roundToInt(100.0f * w)),
                        2);
                }
        }
        if (want("walk3d_big"))
        {
            // a scene as it is played: a large shaped room, every pipe a body in it (some very fast),
            // knocks from all pipes, wear, 1st-order reflections, binaural
            resetParams();
            allStops(false);
            const float rates[]{0.2f, 1.3f, 38.0f, 19.0f, 2.8f, 5.9f, 0.5f, 0.2f};
            for (int s = 0; s < rysum::kNumStops; ++s)
            {
                set(RysumProcessor::motionParamId(s).toRawUTF8(), (float)(8 + s % 4));
                set(RysumProcessor::rateParamId(s).toRawUTF8(), rates[s % 8]);
            }
            set("output", 6);
            set("simOn", 1);
            set("simOrder", 0);
            set("simW", 40.0f);
            set("simL", 100.0f);
            set("simH", 40.0f);
            set("simShape", (float)rysum::shapeL);
            set("simShapeAmt", 0.7f);
            set("doppler", 3.2f);
            set("lvImpLevel", 0.7f);
            set("lvImpSrc", 1);
            set("lvSelfPlay", 0.7f);
            set("lvWhisper", 1.0f);
            set("lvWear", 1.0f);
            run("walk3d_big_shape", 2);
            set("simShape", 0);
            run("walk3d_big_box", 2);
        }
        if (want("walk3d"))
        {
            // the 3D view with everything on: the pipes as bodies in the Saal, knocking on its
            // walls, playing themselves, growing; the listener walking, whispering pipes, time
            // as distance, the room growing at the walls; binaural
            resetParams();
            allStops(false);
            for (int s = 0; s < rysum::kNumStops; ++s)
                set(RysumProcessor::motionParamId(s).toRawUTF8(), (float)(8 + s % 4));
            set("output", 6);
            set("simOn", 1);
            set("lvImpLevel", 0.7f);
            set("lvSelfPlay", 0.7f);
            set("lvGrow", 0.5f);
            set("lvWhisper", 1.0f);
            set("lvTimeDepth", 8.0f);
            set("lvWallPull", 0.6f);
            set("lvWalkShape", 1);
            set("lvWalkSize", 0.8f);
            run("walk3d_all", 2);
        }
        if (want("walk3d"))
        {
            // the same heard over loudspeakers / as Ambisonics, reflections of 1st order
            resetParams();
            allStops(false);
            for (int s = 0; s < rysum::kNumStops; ++s)
                set(RysumProcessor::motionParamId(s).toRawUTF8(), (float)(8 + s % 4));
            set("output", 3);
            set("simOn", 1);
            set("simOrder", 0);
            set("lvImpLevel", 0.7f);
            set("lvSelfPlay", 0.7f);
            run("walk3d_ambi3_er1", 36);
        }
        irFoa.deleteFile();
        irMono.deleteFile();
        std::cout << std::flush;
        std::_Exit(0); // (no need to tear the processor down)
    }

    juce::File samples;
    juce::String filter;
    int block = 128;
    double seconds = 8.0;
    std::unique_ptr<RysumProcessor> proc;
    int waited = 0;
};

START_JUCE_APPLICATION(BenchApp)
