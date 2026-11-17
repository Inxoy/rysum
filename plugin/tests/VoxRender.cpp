// Vox humana through the whole plugin (processor, engine, room, output), no host needed.
//   RysumVoxRender <samples folder> <input.wav> <out folder> [seconds]
// Renders stereo WAVs:
//   vox_keys.wav       the Vox humana alone, played by MIDI (a chord, a melody)
//   vox_input.wav      the input plays the organ: Vox humana only
//   organ_input.wav    the input plays the organ: Gedackt 8' + Octave 4' (samples), no Vox humana
//   both_input.wav     the input plays Gedackt 8' and the Vox humana
// and prints the CPU load of each.
#include "PluginProcessor.h"

#include <chrono>

class VoxRenderApp : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "RysumVoxRender"; }
    const juce::String getApplicationVersion() override { return "0.1"; }

    void initialise(const juce::String &cmd) override
    {
        auto args = juce::StringArray::fromTokens(cmd, true);
        if (args.size() < 3)
        {
            std::cout << "usage: RysumVoxRender <samples> <input.wav> <out folder> [seconds]" << std::endl;
            setApplicationReturnValue(2);
            quit();
            return;
        }
        auto cwd = juce::File::getCurrentWorkingDirectory();
        samples = cwd.getChildFile(args[0].unquoted());
        inputFile = cwd.getChildFile(args[1].unquoted());
        outDir = cwd.getChildFile(args[2].unquoted());
        if (args.size() > 3)
            seconds = juce::jmax(2.0, args[3].getDoubleValue());
        outDir.createDirectory();
        proc = std::make_unique<RysumProcessor>();
        std::cout << "vox model: " << (proc->voxError.isEmpty() ? juce::String("ok") : proc->voxError) << std::endl;
        proc->loadSamples(samples);
        startTimer(50);
    }

    void shutdown() override { proc.reset(); }

private:
    void set(const char *id, float plain)
    {
        if (auto *p = proc->apvts.getParameter(id))
            p->setValueNotifyingHost(p->convertTo0to1(plain));
    }
    void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

    void resetParams()
    {
        for (auto *prm : proc->getParameters())
            if (auto *r = dynamic_cast<juce::RangedAudioParameter *>(prm))
                r->setValueNotifyingHost(r->getDefaultValue());
        for (int s = 0; s < rysum::kNumStops; ++s)
            set(RysumProcessor::stopParamId(s).toRawUTF8(), 0.0f);
        pump(50);
    }

    bool loadInput(juce::AudioBuffer<float> &in)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(inputFile));
        if (r == nullptr)
            return false;
        const int len = (int)juce::jmin<juce::int64>(r->lengthInSamples, (juce::int64)(seconds * r->sampleRate));
        juce::AudioBuffer<float> raw((int)juce::jmax(1u, r->numChannels), len);
        r->read(&raw, 0, len, 0, true, true);
        // to 48 kHz stereo (linear interpolation is enough for a test input)
        const double ratio = r->sampleRate / 48000.0;
        const int n = (int)(len / ratio);
        in.setSize(2, n);
        for (int c = 0; c < 2; ++c)
        {
            const float *src = raw.getReadPointer(juce::jmin(c, raw.getNumChannels() - 1));
            for (int i = 0; i < n; ++i)
            {
                const double p = i * ratio;
                const int k = (int)p;
                const float f = (float)(p - k);
                in.setSample(c, i, src[k] * (1.0f - f) + src[juce::jmin(k + 1, len - 1)] * f);
            }
        }
        return true;
    }

    /** renders `in` (or silence) with `midiAt(block)` notes; returns the stereo result */
    void render(const juce::String &name, const juce::AudioBuffer<float> *in,
                const std::function<void(int, juce::MidiBuffer &)> &midiAt)
    {
        juce::AudioProcessor::BusesLayout lay;
        lay.inputBuses.add(juce::AudioChannelSet::stereo());
        lay.inputBuses.add(juce::AudioChannelSet::stereo());
        lay.outputBuses.add(juce::AudioChannelSet::stereo());
        proc->setBusesLayout(lay);
        proc->setRateAndBufferSizeDetails(48000.0, block);
        proc->prepareToPlay(48000.0, block);
        pump(100);
        const int total = (int)(seconds * 48000.0) / block * block;
        juce::AudioBuffer<float> out(2, total);
        juce::AudioBuffer<float> buf(juce::jmax(proc->getTotalNumInputChannels(), proc->getTotalNumOutputChannels()),
                                     block);
        double busy = 0.0;
        for (int b = 0; b * block < total; ++b)
        {
            buf.clear();
            if (in != nullptr)
                for (int c = 0; c < 2; ++c)
                    if (b * block + block <= in->getNumSamples())
                        buf.copyFrom(c, 0, *in, c, b * block, block);
            juce::MidiBuffer mb;
            midiAt(b, mb);
            const auto t0 = std::chrono::steady_clock::now();
            proc->processBlock(buf, mb);
            busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            for (int c = 0; c < 2; ++c)
                out.copyFrom(c, b * block, buf, c, 0, block);
            if (b % 16 == 0)
                pump(1);
        }
        const float peak = out.getMagnitude(0, total);
        std::cout << name.paddedRight(' ', 18) << " peak " << juce::String(juce::Decibels::gainToDecibels(peak), 1)
                  << " dBFS, rms " << juce::String(juce::Decibels::gainToDecibels(out.getRMSLevel(0, 0, total)), 1)
                  << " dBFS, CPU " << juce::String(100.0 * busy / (total / 48000.0), 1) << " % of real time"
                  << std::endl;
        auto f = outDir.getChildFile(name + ".wav");
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os(f.createOutputStream());
        if (auto w = wav.createWriterFor(os, juce::AudioFormatWriterOptions()
                                                 .withSampleRate(48000.0)
                                                 .withNumChannels(2)
                                                 .withBitsPerSample(24)))
            w->writeFromAudioSampleBuffer(out, 0, total);
    }

    void timerCallback() override
    {
        if (proc->getLibrary() == nullptr && ++waited < 600)
            return;
        stopTimer();
        std::cout << "samples: " << proc->getLoadStatus() << std::endl;
        juce::AudioBuffer<float> in;
        if (!loadInput(in))
        {
            std::cout << "cannot read " << inputFile.getFullPathName() << std::endl;
            setApplicationReturnValue(2);
            quit();
            return;
        }
        const int bps = 48000 / block; // blocks per second
        auto noMidi = [](int, juce::MidiBuffer &) {};

        // 1. the Vox humana by itself, from the keys: a chord, then a melody
        resetParams();
        set("voxOn", 1.0f);
        render("vox_keys", nullptr,
               [bps](int b, juce::MidiBuffer &mb)
               {
                   const int chord[]{48, 55, 60, 64};
                   if (b == bps / 4)
                       for (int m : chord)
                           mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), 0);
                   if (b == 3 * bps)
                       for (int m : chord)
                           mb.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                   const int mel[]{60, 62, 64, 65, 67, 69, 71, 72};
                   for (int i = 0; i < 8; ++i)
                   {
                       const int on = 4 * bps + i * bps / 3;
                       if (b == on)
                           mb.addEvent(juce::MidiMessage::noteOn(1, mel[i], (juce::uint8)100), 0);
                       if (b == on + bps / 3 - 2)
                           mb.addEvent(juce::MidiMessage::noteOff(1, mel[i]), 0);
                   }
               });

        // 2. - 4. the input plays the organ
        resetParams();
        set("voxOn", 1.0f);
        set("inPlays", 1.0f);
        render("vox_input", &in, noMidi);

        resetParams();
        set(RysumProcessor::stopParamId(1).toRawUTF8(), 1.0f); // Gedackt 8'
        set(RysumProcessor::stopParamId(2).toRawUTF8(), 1.0f); // Octave 4'
        set("inPlays", 1.0f);
        render("organ_input", &in, noMidi);

        resetParams();
        set(RysumProcessor::stopParamId(1).toRawUTF8(), 1.0f);
        set("voxOn", 1.0f);
        set("inPlays", 1.0f);
        set("voxFollow", 1.0f);
        render("both_input", &in, noMidi);

        quit();
    }

    std::unique_ptr<RysumProcessor> proc;
    juce::File samples, inputFile, outDir;
    double seconds = 20.0;
    int block = 256, waited = 0;
};

START_JUCE_APPLICATION(VoxRenderApp)
