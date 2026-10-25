#include "SampleLibrary.h"

namespace rysum
{
static std::shared_ptr<const Sample> readMono (juce::AudioFormatManager& fm, const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr || r->lengthInSamples <= 0 || r->lengthInSamples > (1 << 26))
        return nullptr;

    const int n = (int) r->lengthInSamples;
    const int ch = (int) juce::jmax (1u, r->numChannels);
    juce::AudioBuffer<float> buf (ch, n);
    r->read (&buf, 0, n, 0, true, ch > 1);

    auto s = std::make_shared<Sample>();
    s->sampleRate = r->sampleRate;
    s->data.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        float v = 0.0f;
        for (int c = 0; c < ch; ++c) v += buf.getSample (c, i);
        s->data[(size_t) i] = v / (float) ch;
    }
    return s;
}

static bool isBadPipe (const Pipe& p)
{
    if (! p.valid) return true;
    for (auto& f : p.flags)
    {
        if (f.startsWith ("short sustain")) return true;
        if (f.startsWith ("unsteady"))
        {
            // "unsteady (5.0 dB)"
            const float db = f.fromFirstOccurrenceOf ("(", false, false).getFloatValue();
            if (db > 4.0f) return true;
        }
    }
    return false;
}

juce::Array<juce::File> SampleLibrary::candidateFolders()
{
    juce::Array<juce::File> c;
    auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    // next to the plugin / app (VST3 bundles: <...>/Rysum.vst3/Contents/x86_64-win/Rysum.vst3)
    for (auto d = exe.getParentDirectory(); d.exists() && ! d.isRoot(); d = d.getParentDirectory())
    {
        c.add (d.getChildFile ("RysumSamples"));
        c.add (d.getChildFile ("samples"));
        if (d.getFileName().endsWithIgnoreCase (".vst3")) { c.add (d.getParentDirectory().getChildFile ("RysumSamples")); break; }
    }
    c.add (juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Rysum").getChildFile ("samples"));
    c.add (juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("RysumSamples"));
   #if JUCE_WINDOWS
    c.add (juce::File ("D:\\RYSUM_plugin\\rysum_plugin\\samples"));
   #endif
    return c;
}

std::unique_ptr<SampleLibrary> SampleLibrary::load (const juce::File& folder, juce::String& error,
                                                    std::function<bool()> shouldExit)
{
    auto jsonFile = folder.getChildFile ("rysum_samples.json");
    if (! jsonFile.existsAsFile())
    {
        error = "rysum_samples.json not found in " + folder.getFullPathName();
        return nullptr;
    }

    const juce::var root = juce::JSON::parse (jsonFile);
    if (! root.isObject())
    {
        error = "rysum_samples.json could not be read";
        return nullptr;
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    auto lib = std::make_unique<SampleLibrary>();
    lib->folder = folder;

    for (int s = 0; s < kNumStops; ++s)
        for (int k = 0; k < kNumKeys; ++k)
            lib->pipes[(size_t) s][(size_t) k].midi = kPhysKeys[(size_t) k];

    if (auto* stops = root["stops"].getArray())
    {
        for (auto& st : *stops)
        {
            const int si = stopIndexForId (st["id"].toString());
            if (si < 0) continue;
            if (auto* pipes = st["pipes"].getArray())
            {
                for (auto& pv : *pipes)
                {
                    if (shouldExit && shouldExit()) { error = "cancelled"; return nullptr; }
                    const int ki = keyIndexForMidi ((int) pv["key"]);
                    if (ki < 0) continue;
                    auto& p = lib->pipes[(size_t) si][(size_t) ki];
                    p.file = pv["file"].toString();
                    p.sample = readMono (fm, folder.getChildFile (p.file));
                    if (p.sample == nullptr) { lib->warnings.add ("missing file " + p.file); continue; }

                    const int len = p.sample->size();
                    auto clampI = [len] (const juce::var& v, int def) { return v.isVoid() || v.isUndefined() ? def : juce::jlimit (0, len - 1, (int) v); };
                    p.onset     = clampI (pv["onset"], 0);
                    p.attackEnd = clampI (pv["attack_end"], p.onset);
                    p.release   = clampI (pv["release"], len - 1);
                    p.loopStart = pv["loop_start"].isVoid() ? -1 : clampI (pv["loop_start"], -1);
                    p.loopEnd   = pv["loop_end"].isVoid()   ? -1 : clampI (pv["loop_end"], -1);
                    if (p.loopEnd > p.release) p.loopEnd = p.release;
                    p.rate = p.sample->sampleRate / kSampleFileRate;    // 1.0 normally
                    p.levelDb = (float) (double) pv["level_db"];
                    p.loopNcc = pv["loop_ncc"].isVoid() ? 0.0f : (float) (double) pv["loop_ncc"];
                    p.nominalHz = (double) pv["nominal_hz"];
                    p.measuredHz = pv["measured_hz"].isVoid() ? 0.0 : (double) pv["measured_hz"];
                    p.hasCents = ! pv["cents_vs_nominal"].isVoid();
                    p.cents = p.hasCents ? (double) pv["cents_vs_nominal"] : 0.0;
                    p.source = pv["source"].toString();
                    p.sourceTime = (double) pv["source_time"];
                    if (auto* m = pv["mono"].getDynamicObject()) p.monoMethod = m->getProperty ("method").toString();
                    if (auto* fl = pv["flags"].getArray())
                        for (auto& f : *fl)
                        {
                            p.flags.add (f.toString());
                            if (f.toString().endsWith ("takes")) p.takes = f.toString().getIntValue();
                        }
                    p.valid = true;
                    ++lib->loadedPipes;
                }
            }
        }
    }

    // replace missing / unusable pipes by the nearest good neighbour, pitch-shifted to
    // the meantone pitch of the missing key
    for (int s = 0; s < kNumStops; ++s)
    {
        auto& row = lib->pipes[(size_t) s];
        std::array<bool, kNumKeys> bad {};
        for (int k = 0; k < kNumKeys; ++k) bad[(size_t) k] = isBadPipe (row[(size_t) k]);

        for (int k = 0; k < kNumKeys; ++k)
        {
            if (! bad[(size_t) k]) continue;
            const int midi = kPhysKeys[(size_t) k];
            int best = -1, bestDist = 1000;
            for (int j = 0; j < kNumKeys; ++j)
            {
                if (bad[(size_t) j]) continue;
                const int d = std::abs (kPhysKeys[(size_t) j] - midi);
                // prefer the neighbour above (shifting down sounds more natural) on ties
                if (d < bestDist || (d == bestDist && kPhysKeys[(size_t) j] > midi)) { bestDist = d; best = j; }
            }
            if (best < 0 || bestDist > 3) continue;
            auto src = row[(size_t) best];       // copy of the source pipe
            auto& dst = row[(size_t) k];
            const auto keepFlags = dst.flags;
            const auto keepFile = dst.file;
            const bool hadOwn = dst.valid;
            dst = src;
            dst.midi = midi;
            dst.rate = src.rate * meantoneHz (midi) / meantoneHz (kPhysKeys[(size_t) best]);
            dst.substituted = true;
            dst.substituteFrom = best;
            dst.flags = keepFlags;
            dst.flags.add ("replaced by " + helmholtzName (kPhysKeys[(size_t) best]) + (hadOwn ? " (own sample unusable)" : " (no sample)"));
            dst.file = keepFile.isNotEmpty() ? keepFile : src.file;
            dst.hasCents = false;
            ++lib->substitutedPipes;
        }
    }

    // noise layer
    if (auto* noise = root["noise"].getDynamicObject())
    {
        auto readList = [&] (const juce::var& arr, bool isClicks)
        {
            if (auto* a = arr.getArray())
                for (auto& e : *a)
                {
                    if (shouldExit && shouldExit()) return;
                    NoiseSample ns;
                    ns.sample = readMono (fm, folder.getChildFile (e["file"].toString()));
                    if (ns.sample == nullptr) continue;
                    ns.kind = e["kind"].toString();
                    ns.peakDb = (float) (double) e["peak_db"];
                    if (! isClicks) lib->stopPulls.push_back (ns);
                    else if (ns.kind == "down") lib->clicksDown.push_back (ns);
                    else if (ns.kind == "up") lib->clicksUp.push_back (ns);
                    else lib->clicksOther.push_back (ns);
                }
        };
        readList (noise->getProperty ("clicks"), true);
        readList (noise->getProperty ("stop_pulls"), false);
        if (auto* w = noise->getProperty ("wind").getArray())
            for (auto& e : *w)
                if (auto s = readMono (fm, folder.getChildFile (e["file"].toString())))
                    lib->wind.push_back (s);
    }

    if (lib->loadedPipes == 0)
    {
        error = "no pipe samples could be loaded from " + folder.getFullPathName();
        return nullptr;
    }
    return lib;
}
} // namespace rysum
