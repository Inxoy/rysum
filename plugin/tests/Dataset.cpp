// Samples the plugin into a dataset for training a neural synthesis network: many short clips with
// the notes that were played and the settings they were played with.
//
//   RysumDataset <samples-folder> <out-folder> [options]
//
//   --hours 10        how much audio in total
//   --clip 10         seconds per clip
//   --rate 48000      sample rate
//   --channels 2      2 = stereo, 1 = mono (the middle of left and right)
//   --bits 16         16 or 24
//   --flac            FLAC instead of WAV (lossless, about half the size)
//   --seed 1          another seed = another dataset; the same seed = the same clips
//   --mix 40,40,20    per cent of clips that are: one pipe / a registration playing a phrase /
//                     a factory patch playing a phrase
//   --dry             no room at all (impulse response and Saal off) in every clip
//   --valid 5         per cent of the clips marked as validation set
//
// Written: audio/000000.wav ..., midi/000000.mid ..., metadata.jsonl (one line per clip),
// dataset.json (format, the parameter table with its defaults). Stopped and started again with the
// same options it carries on where it was.

#include "PluginProcessor.h"
#include <iostream>

using namespace rysum;

class DatasetApp : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override
    {
        return "RysumDataset";
    }
    const juce::String getApplicationVersion() override
    {
        return "0.1";
    }

    void initialise(const juce::String &cmd) override
    {
        auto args = juce::StringArray::fromTokens(cmd, true);
        juce::StringArray plain;
        for (int i = 0; i < args.size(); ++i)
        {
            const auto a = args[i].unquoted();
            auto next = [&] { return i + 1 < args.size() ? args[++i].unquoted() : juce::String(); };
            if (a == "--hours")
                hours = next().getDoubleValue();
            else if (a == "--clip")
                clipSeconds = next().getDoubleValue();
            else if (a == "--rate")
                rate = next().getDoubleValue();
            else if (a == "--channels")
                channels = next().getIntValue();
            else if (a == "--bits")
                bits = next().getIntValue();
            else if (a == "--seed")
                seed = next().getLargeIntValue();
            else if (a == "--valid")
                validPercent = next().getDoubleValue();
            else if (a == "--flac")
                flac = true;
            else if (a == "--dry")
                dry = true;
            else if (a == "--mix")
            {
                auto m = juce::StringArray::fromTokens(next(), ",", "");
                for (int k = 0; k < 3 && k < m.size(); ++k)
                    mix[k] = juce::jmax(0.0, m[k].getDoubleValue());
            }
            else if (a.startsWith("--"))
            {
                std::cout << "unknown option " << a << std::endl;
                fail();
                return;
            }
            else
                plain.add(a);
        }
        if (plain.size() < 2 || hours <= 0.0 || clipSeconds < 4.0 || rate < 8000.0 ||
            (channels != 1 && channels != 2) || (bits != 16 && bits != 24) ||
            mix[0] + mix[1] + mix[2] <= 0.0)
        {
            std::cout << "usage: RysumDataset <samples-folder> <out-folder> [--hours 10] [--clip 10]"
                         " [--rate 48000] [--channels 2] [--bits 16] [--flac] [--seed 1]"
                         " [--mix 40,40,20] [--dry] [--valid 5]   (a clip is at least 4 s)"
                      << std::endl;
            fail();
            return;
        }
        samples = juce::File::getCurrentWorkingDirectory().getChildFile(plain[0]);
        outDir = juce::File::getCurrentWorkingDirectory().getChildFile(plain[1]);
        if (!samples.isDirectory())
        {
            std::cout << "no samples folder at " << samples.getFullPathName() << std::endl;
            fail();
            return;
        }
        audioDir = outDir.getChildFile("audio");
        midiDir = outDir.getChildFile("midi");
        if (!audioDir.createDirectory() || !midiDir.createDirectory())
        {
            std::cout << "cannot write to " << outDir.getFullPathName() << std::endl;
            fail();
            return;
        }
        totalClips = (int)std::ceil(hours * 3600.0 / clipSeconds);
        proc = std::make_unique<RysumProcessor>();
        proc->loadSamples(samples);
        std::cout << "loading the pipes from " << samples.getFullPathName() << " ..." << std::endl;
        startTimer(50);
    }

    void shutdown() override
    {
        stopTimer();
        proc.reset();
    }

private:
    void fail()
    {
        setApplicationReturnValue(2);
        quit();
    }

    // ---- the plugin
    void setReal(const juce::String &id, float v)
    {
        if (auto *q = proc->apvts.getParameter(id))
            q->setValueNotifyingHost(q->convertTo0to1(v));
    }
    float getReal(const juce::String &id) const
    {
        if (auto *q = proc->apvts.getParameter(id))
            return q->convertFrom0to1(q->getValue());
        return 0.0f;
    }
    void renderBlock(juce::MidiBuffer &mb)
    {
        work.clear();
        proc->processBlock(work, mb);
        mb.clear();
    }
    void pump(int ms)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
    }
    /** everything as it is when the plugin is opened ("01 Init"), all notes let go, rung out */
    void resetAll()
    {
        if (initPatch.existsAsFile())
            proc->loadPatch(initPatch);
        else
            for (auto *q : proc->getParameters())
                q->setValueNotifyingHost(q->getDefaultValue());
        proc->engine.pipeLenReset = true;
        setReal("output", 0.0f); // stereo
        setReal("inpOn", 0.0f);
    }
    void ringOut()
    {
        juce::MidiBuffer mb;
        for (int ch = 1; ch <= 16; ++ch)
        {
            mb.addEvent(juce::MidiMessage::allNotesOff(ch), 0);
            mb.addEvent(juce::MidiMessage::controllerEvent(ch, 64, 0), 0);
        }
        // until half a second has been silent (at most 8 s: an endless effect is cut there)
        int quiet = 0;
        for (int b = 0; b < (int)(8.0 * rate / block) && quiet < (int)(0.5 * rate / block); ++b)
        {
            renderBlock(mb);
            float peak = 0.0f;
            for (int c = 0; c < 2; ++c)
                peak = juce::jmax(peak, work.getMagnitude(c, 0, block));
            quiet = peak < 1.0e-4f ? quiet + 1 : 0;
        }
    }

    struct Note
    {
        double on = 0.0, off = 0.0;
        int midi = 60, velocity = 100;
    };

    /** one pipe: a single note, held for a while */
    std::vector<Note> singleNote(juce::Random &r) const
    {
        Note n;
        n.midi = kPhysKeys[(size_t)r.nextInt(kNumKeys)];
        n.velocity = 30 + r.nextInt(98);
        n.on = 0.05 + 0.4 * r.nextDouble();
        const double longest = clipSeconds - 1.8 - n.on;
        n.off = n.on + juce::jmin(longest, 0.25 + std::pow(r.nextDouble(), 1.5) * longest);
        return {n};
    }

    /** something played: single lines, chords, or both, on the organ's own keys */
    std::vector<Note> phrase(juce::Random &r) const
    {
        std::vector<Note> notes;
        const double end = clipSeconds - 1.8;
        const int style = r.nextInt(4); // 0 a line, 1 chords, 2 a line over a held chord, 3 clusters / free
        auto key = [&](int index) { return kPhysKeys[(size_t)juce::jlimit(0, kNumKeys - 1, index)]; };
        const int scales[4][7]{{0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10}, {0, 1, 4, 5, 7, 8, 11}};
        const int *scale = scales[r.nextInt(4)];
        const int root = r.nextInt(12);
        auto inScale = [&](int midi)
        {
            for (int k = 0; k < 7; ++k)
                if ((midi - root + 120) % 12 == scale[k])
                    return true;
            return false;
        };
        auto nearest = [&](int midi)
        {
            // the organ's key in the scale nearest to this pitch
            int best = key(0);
            for (int k = 0; k < kNumKeys; ++k)
                if (inScale(key(k)) && std::abs(key(k) - midi) < std::abs(best - midi))
                    best = key(k);
            return best;
        };
        const int lo = key(0), hi = key(kNumKeys - 1);
        auto addLine = [&](double from, int centre)
        {
            double t = from;
            int pitch = nearest(centre);
            const double beat = 0.12 + 0.5 * r.nextDouble();
            const double legato = 0.5 + 0.6 * r.nextDouble();
            while (t < end - 0.2)
            {
                const double len = beat * (double)(1 << r.nextInt(3));
                Note n;
                n.midi = pitch;
                n.velocity = 40 + r.nextInt(88);
                n.on = t;
                n.off = juce::jmin(end, t + juce::jmax(0.06, len * legato));
                if (r.nextDouble() > 0.12) // (a rest now and then)
                    notes.push_back(n);
                t += len;
                pitch = nearest(juce::jlimit(lo, hi, pitch + r.nextInt(9) - 4));
            }
        };
        auto addChords = [&](double from, bool oneHeld)
        {
            double t = from;
            while (t < end - 0.3)
            {
                const double len = oneHeld ? end - t : juce::jmin(end - t, 0.5 + 3.5 * r.nextDouble());
                const int voices = 2 + r.nextInt(4);
                int pitch = nearest(lo + r.nextInt(juce::jmax(1, (hi - lo) / 2)));
                const int vel = 40 + r.nextInt(88);
                for (int v = 0; v < voices; ++v)
                {
                    Note n;
                    n.midi = pitch;
                    n.velocity = vel;
                    n.on = t + 0.02 * r.nextDouble() * v; // (fingers are not a machine)
                    n.off = t + len * (oneHeld ? 1.0 : 0.85 + 0.15 * r.nextDouble());
                    notes.push_back(n);
                    pitch = nearest(pitch + 3 + r.nextInt(5));
                    if (pitch >= hi)
                        break;
                }
                t += len;
                if (oneHeld)
                    break;
            }
        };
        const double start = 0.05 + 0.3 * r.nextDouble();
        if (style == 0)
            addLine(start, lo + r.nextInt(hi - lo));
        else if (style == 1)
            addChords(start, false);
        else if (style == 2)
        {
            addChords(start, true);
            addLine(start + 0.3 + r.nextDouble(), (lo + hi) / 2 + r.nextInt(12));
        }
        else
        {
            // free: notes anywhere, any length
            const int count = 3 + r.nextInt(14);
            for (int k = 0; k < count; ++k)
            {
                Note n;
                n.midi = key(r.nextInt(kNumKeys));
                n.velocity = 20 + r.nextInt(108);
                n.on = start + r.nextDouble() * (end - start - 0.3);
                n.off = juce::jmin(end, n.on + 0.08 + std::pow(r.nextDouble(), 2.0) * 5.0);
                notes.push_back(n);
            }
        }
        // (the same key twice at once: the second one waits for the first)
        std::sort(notes.begin(), notes.end(), [](const Note &a, const Note &b) { return a.on < b.on; });
        std::vector<Note> clean;
        for (auto n : notes)
        {
            for (auto &c : clean)
                if (c.midi == n.midi && c.off > n.on - 0.01)
                    c.off = juce::jmax(c.on + 0.03, n.on - 0.01);
            if (n.off - n.on >= 0.03)
                clean.push_back(n);
        }
        return clean;
    }

    // ---- one clip
    bool makeClip(int index)
    {
        juce::DynamicObject::Ptr meta = new juce::DynamicObject();
        juce::AudioBuffer<float> clip(2, (int)std::round(clipSeconds * rate));
        std::vector<Note> notes;
        juce::String kind, patchName;
        juce::Array<juce::var> stops;
        float peak = 0.0f;
        double rms = 0.0;
        int attempt = 0;
        for (;; ++attempt)
        {
            juce::Random r(seed * 1000003LL + (juce::int64)index * 7919LL + attempt * 104729LL);
            const double pick = r.nextDouble() * (mix[0] + mix[1] + mix[2]);
            const int k = pick < mix[0] ? 0 : (pick < mix[0] + mix[1] ? 1 : 2);
            kind = k == 0 ? "pipe" : (k == 1 ? "registration" : "patch");
            patchName.clear();
            resetAll();
            if (k == 2 && !patches.isEmpty())
            {
                const auto f = patches[r.nextInt(patches.size())];
                proc->loadPatch(f);
                patchName = f.getFileNameWithoutExtension();
                setReal("output", 0.0f);
                setReal("inpOn", 0.0f);
            }
            else
            {
                // which stops are pulled
                std::array<bool, kNumStops> on{};
                const int count = k == 0 ? 1 : 1 + (int)(std::pow(r.nextDouble(), 1.4) * 4.99);
                for (int c = 0; c < count; ++c)
                    on[(size_t)r.nextInt(kNumStops)] = true;
                for (int s = 0; s < kNumStops; ++s)
                    setReal("stop" + juce::String(s + 1), on[(size_t)s] ? 1.0f : 0.0f);
            }
            if (dry)
            {
                setReal("simOn", 0.0f);
                setReal("irWet", -60.0f);
                setReal("irDry", 0.0f);
            }
            stops.clear();
            for (int s = 0; s < kNumStops; ++s)
                if (getReal("stop" + juce::String(s + 1)) > 0.5f)
                    stops.add(juce::String(kStops[(size_t)s].id));
            notes = k == 0 ? singleNote(r) : phrase(r);

            // what a patch loads in the background (reverb, responses) arrives while the last clip
            // rings out and a moment is waited
            pump(k == 2 ? 250 : 5);
            ringOut();
            pump(1);

            // the notes, block by block
            struct Ev
            {
                int at;
                juce::MidiMessage m;
            };
            std::vector<Ev> evs;
            for (const auto &n : notes)
            {
                evs.push_back({(int)(n.on * rate), juce::MidiMessage::noteOn(1, n.midi, (juce::uint8)n.velocity)});
                evs.push_back({(int)(n.off * rate), juce::MidiMessage::noteOff(1, n.midi)});
            }
            std::stable_sort(evs.begin(), evs.end(), [](const Ev &a, const Ev &b) { return a.at < b.at; });
            size_t next = 0;
            clip.clear();
            juce::MidiBuffer mb;
            for (int pos = 0; pos < clip.getNumSamples(); pos += block)
            {
                while (next < evs.size() && evs[next].at < pos + block)
                {
                    mb.addEvent(evs[next].m, juce::jmax(0, evs[next].at - pos));
                    ++next;
                }
                renderBlock(mb);
                const int n = juce::jmin(block, clip.getNumSamples() - pos);
                for (int c = 0; c < 2; ++c)
                    clip.copyFrom(c, pos, work, c, 0, n);
            }
            peak = clip.getMagnitude(0, clip.getNumSamples());
            rms = 0.0;
            bool finite = true;
            for (int c = 0; c < 2; ++c)
            {
                const float *x = clip.getReadPointer(c);
                for (int i = 0; i < clip.getNumSamples(); ++i)
                {
                    finite = finite && std::isfinite(x[i]);
                    rms += (double)x[i] * x[i];
                }
            }
            rms = std::sqrt(rms / (2.0 * clip.getNumSamples()));
            // a silent clip (a stop without a pipe for that key, a patch that needs input) or a
            // broken one: another draw
            if (finite && peak > 0.003f)
                break;
            if (attempt >= 7)
            {
                std::cout << "clip " << index << ": eight silent draws - giving up" << std::endl;
                return false;
            }
            ++redrawn;
        }

        // the settings that differ from the opened plugin (the table with the defaults: dataset.json)
        juce::DynamicObject::Ptr changed = new juce::DynamicObject();
        for (auto *q : proc->getParameters())
            if (auto *rp = dynamic_cast<juce::RangedAudioParameter *>(q))
            {
                const float v = rp->convertFrom0to1(rp->getValue());
                const auto it = defaults.find(rp->getParameterID());
                if (it == defaults.end() || std::abs(it->second - v) > 1.0e-5f)
                    changed->setProperty(rp->getParameterID(), v);
            }

        // loud clips are turned down so nothing clips (how much is noted)
        float gain = 1.0f;
        if (peak > 0.98f)
        {
            gain = 0.98f / peak;
            clip.applyGain(gain);
            ++scaled;
        }
        clip.applyGainRamp(clip.getNumSamples() - (int)(0.02 * rate), (int)(0.02 * rate), 1.0f, 0.0f);
        if (channels == 1)
        {
            clip.applyGain(0, 0, clip.getNumSamples(), 0.5f);
            clip.addFrom(0, 0, clip, 1, 0, clip.getNumSamples(), 0.5f);
        }

        const auto name = juce::String(index).paddedLeft('0', 6);
        const auto ext = flac ? ".flac" : ".wav";
        {
            auto file = audioDir.getChildFile(name + ext), tmp = audioDir.getChildFile(name + ".part");
            tmp.deleteFile();
            std::unique_ptr<juce::AudioFormat> format;
            if (flac)
                format = std::make_unique<juce::FlacAudioFormat>();
            else
                format = std::make_unique<juce::WavAudioFormat>();
            auto stream = std::make_unique<juce::FileOutputStream>(tmp);
            if (!stream->openedOk())
                return false;
            std::unique_ptr<juce::AudioFormatWriter> writer(format->createWriterFor(
                stream.get(), rate, (unsigned int)channels, bits, {}, flac ? 5 : 0));
            if (writer == nullptr)
                return false;
            stream.release();
            if (!writer->writeFromAudioSampleBuffer(clip, 0, clip.getNumSamples()))
                return false;
            writer.reset();
            file.deleteFile();
            if (!tmp.moveFileTo(file))
                return false;
        }
        {
            // the notes as a MIDI file (120 beats per minute: 960 ticks are a second)
            juce::MidiMessageSequence seq;
            seq.addEvent(juce::MidiMessage::tempoMetaEvent(500000), 0.0);
            for (const auto &n : notes)
            {
                seq.addEvent(juce::MidiMessage::noteOn(1, n.midi, (juce::uint8)n.velocity), n.on * 960.0);
                seq.addEvent(juce::MidiMessage::noteOff(1, n.midi), n.off * 960.0);
            }
            seq.updateMatchedPairs();
            juce::MidiFile mf;
            mf.setTicksPerQuarterNote(480);
            mf.addTrack(seq);
            auto file = midiDir.getChildFile(name + ".mid");
            file.deleteFile();
            juce::FileOutputStream os(file);
            if (!os.openedOk() || !mf.writeTo(os))
                return false;
        }
        juce::Array<juce::var> noteList;
        for (const auto &n : notes)
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty("on", std::round(n.on * 10000.0) / 10000.0);
            o->setProperty("off", std::round(n.off * 10000.0) / 10000.0);
            o->setProperty("midi", n.midi);
            o->setProperty("velocity", n.velocity);
            noteList.add(juce::var(o.get()));
        }
        // (the same clips are the validation set whatever else changes)
        juce::Random split(seed * 31LL + (juce::int64)index * 2654435761LL);
        meta->setProperty("index", index);
        meta->setProperty("audio", "audio/" + name + ext);
        meta->setProperty("midi", "midi/" + name + ".mid");
        meta->setProperty("split", split.nextDouble() * 100.0 < validPercent ? "valid" : "train");
        meta->setProperty("kind", kind);
        if (patchName.isNotEmpty())
            meta->setProperty("patch", patchName);
        meta->setProperty("stops", stops);
        meta->setProperty("notes", noteList);
        meta->setProperty("peak_db", 20.0 * std::log10(juce::jmax(1.0e-9f, peak * gain)));
        meta->setProperty("rms_db", 20.0 * std::log10(juce::jmax(1.0e-9, rms * gain)));
        meta->setProperty("gain_db", 20.0 * std::log10(gain));
        meta->setProperty("draw", attempt);
        meta->setProperty("parameters", juce::var(changed.get()));
        juce::FileOutputStream os(outDir.getChildFile("metadata.jsonl"));
        if (!os.openedOk())
            return false;
        os << juce::JSON::toString(juce::var(meta.get()), true) << "\n";
        return true;
    }

    // ---- start (once the pipes are loaded), then clip after clip
    bool begin()
    {
        proc->setRateAndBufferSizeDetails(rate, block);
        proc->prepareToPlay(rate, block);
        work.setSize(juce::jmax(2, proc->getTotalNumInputChannels(), proc->getTotalNumOutputChannels()), block);
        pump(400); // (factory patches written, responses loaded)
        for (auto &f : RysumProcessor::getPatchFiles())
        {
            if (f.getFileNameWithoutExtension().containsIgnoreCase("Init"))
                initPatch = f;
            else
                patches.add(f);
        }
        resetAll();
        pump(200);
        juce::DynamicObject::Ptr table = new juce::DynamicObject();
        for (auto *q : proc->getParameters())
            if (auto *rp = dynamic_cast<juce::RangedAudioParameter *>(q))
            {
                const float v = rp->convertFrom0to1(rp->getValue());
                defaults[rp->getParameterID()] = v;
                juce::DynamicObject::Ptr o = new juce::DynamicObject();
                o->setProperty("name", rp->getName(64));
                o->setProperty("default", v);
                o->setProperty("min", rp->getNormalisableRange().start);
                o->setProperty("max", rp->getNormalisableRange().end);
                table->setProperty(rp->getParameterID(), juce::var(o.get()));
            }

        // carry on where an earlier run stopped (only with the same options)
        juce::DynamicObject::Ptr head = new juce::DynamicObject();
        head->setProperty("plugin", "Rysum");
        head->setProperty("sample_rate", rate);
        head->setProperty("channels", channels);
        head->setProperty("bits", bits);
        head->setProperty("format", flac ? "flac" : "wav");
        head->setProperty("clip_seconds", clipSeconds);
        head->setProperty("seed", (juce::int64)seed);
        head->setProperty("mix_pipe_registration_patch",
                          juce::String(mix[0]) + "," + juce::String(mix[1]) + "," + juce::String(mix[2]));
        head->setProperty("dry", dry);
        head->setProperty("valid_percent", validPercent);
        const auto options = juce::JSON::toString(juce::var(head.get()), true);
        const auto headFile = outDir.getChildFile("dataset.json"), metaFile = outDir.getChildFile("metadata.jsonl");
        done = 0;
        if (headFile.existsAsFile() && metaFile.existsAsFile())
        {
            const auto old = juce::JSON::parse(headFile);
            if (old["options"].toString() != options)
            {
                std::cout << "the folder holds a dataset made with other options:\n  " << old["options"].toString()
                          << "\nchoose another folder, or empty this one" << std::endl;
                return false;
            }
            // (only whole lines whose audio is there count)
            juce::StringArray lines;
            metaFile.readLines(lines);
            juce::String kept;
            for (const auto &l : lines)
            {
                const auto v = juce::JSON::parse(l);
                if (!v.isObject() || (int)v["index"] != done || !outDir.getChildFile(v["audio"].toString()).existsAsFile())
                    break;
                kept << l << "\n";
                ++done;
            }
            metaFile.replaceWithText(kept, false, false, "\n");
            if (done > 0)
                std::cout << "carrying on after clip " << done - 1 << std::endl;
        }
        else
            metaFile.deleteFile();
        head->setProperty("options", options);
        head->setProperty("clips", totalClips);
        head->setProperty("metadata", "metadata.jsonl: one JSON object per line and clip - audio, midi, split "
                                      "(train / valid), kind (pipe / registration / patch), stops, notes (on / off in "
                                      "seconds, midi, velocity), peak_db, rms_db, gain_db (how far a clip was turned "
                                      "down so it does not clip), parameters (those that differ from 'parameters' here)");
        head->setProperty("parameters", juce::var(table.get()));
        headFile.replaceWithText(juce::JSON::toString(juce::var(head.get())), false, false, "\n");
        std::cout << totalClips << " clips of " << clipSeconds << " s = " << juce::String(totalClips * clipSeconds / 3600.0, 2)
                  << " hours, " << rate << " Hz, " << (channels == 1 ? "mono" : "stereo") << ", " << bits << " bit "
                  << (flac ? "FLAC" : "WAV") << (dry ? ", dry" : "") << "  ->  " << outDir.getFullPathName() << std::endl;
        started = juce::Time::getMillisecondCounterHiRes();
        doneAtStart = done;
        return true;
    }

    void timerCallback() override
    {
        if (proc->isLoading())
            return;
        if (busy)
            return;
        if (!begun)
        {
            begun = true;
            busy = true;
            const bool ok = begin();
            busy = false;
            if (!ok)
            {
                stopTimer();
                fail();
                return;
            }
            startTimer(1);
            return;
        }
        if (done >= totalClips)
        {
            stopTimer();
            const double took = (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
            std::cout << "done: " << totalClips << " clips in " << outDir.getFullPathName() << " ("
                      << juce::String(took / 60.0, 1) << " minutes; " << redrawn << " silent draws repeated, " << scaled
                      << " clips turned down)" << std::endl;
            quit();
            return;
        }
        if (busy)
            return; // (a clip waits for the message loop: the timer comes by meanwhile)
        busy = true;
        const bool made1 = makeClip(done);
        busy = false;
        if (!made1)
        {
            std::cout << "stopped at clip " << done << " (could not write, or nothing sounded)" << std::endl;
            stopTimer();
            fail();
            return;
        }
        ++done;
        const int made = done - doneAtStart;
        if (made == 5 || done % 100 == 0 || done == totalClips)
        {
            const double took = (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
            const double perClip = took / juce::jmax(1, made);
            std::cout << done << " / " << totalClips << "   " << juce::String(clipSeconds / perClip, 1)
                      << " x real time, about " << juce::String((totalClips - done) * perClip / 60.0, 0)
                      << " minutes left" << std::endl;
        }
    }

    // options
    double hours = 10.0, clipSeconds = 10.0, rate = 48000.0, validPercent = 5.0;
    int channels = 2, bits = 16;
    juce::int64 seed = 1;
    bool flac = false, dry = false;
    double mix[3]{40.0, 40.0, 20.0};

    juce::File samples, outDir, audioDir, midiDir, initPatch;
    juce::Array<juce::File> patches;
    std::unique_ptr<RysumProcessor> proc;
    juce::AudioBuffer<float> work;
    std::map<juce::String, float> defaults;
    static constexpr int block = 512;
    int totalClips = 0, done = 0, doneAtStart = 0, redrawn = 0, scaled = 0;
    bool begun = false, busy = false;
    double started = 0.0;
};

START_JUCE_APPLICATION(DatasetApp)
