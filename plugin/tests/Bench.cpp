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
