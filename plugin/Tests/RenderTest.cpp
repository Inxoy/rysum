// Offline test: loads the sample folder, renders a few MIDI scenes through the engine
// in stereo and in AmbiX 5th order, writes WAV files and checks the results.
//   RysumRenderTest <samples folder> <output folder>
#include "OrganEngine.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <chrono>
#include <iostream>

using namespace rysum;

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
    if (! ok) ++failures;
}

static void testEncoder()
{
    std::cout << "encoder" << std::endl;
    float g[kMaxAmbiChannels];
    encodeAmbiX (90.0f, 0.0f, 1, g);      // left
    check (std::abs (g[0] - 1) < 1e-5 && std::abs (g[1] - 1) < 1e-5 && std::abs (g[2]) < 1e-5 && std::abs (g[3]) < 1e-5, "order 1, left: W=1 Y=1 Z=0 X=0");
    encodeAmbiX (0.0f, 90.0f, 1, g);      // up
    check (std::abs (g[2] - 1) < 1e-5 && std::abs (g[1]) < 1e-5, "order 1, zenith: Z=1");
    encodeAmbiX (30.0f, 20.0f, 2, g);
    const double az = juce::degreesToRadians (30.0), el = juce::degreesToRadians (20.0);
    check (std::abs (g[4] - std::sqrt (3.0) / 2 * std::cos (el) * std::cos (el) * std::sin (2 * az)) < 1e-5, "ACN4 = sqrt(3)/2 cos^2(el) sin(2az)");
    check (std::abs (g[6] - 0.5 * (3 * std::sin (el) * std::sin (el) - 1)) < 1e-5, "ACN6 = (3 sin^2 el - 1)/2");

    // SN3D orthogonality: mean over the sphere of Y_i*Y_j = delta_ij / (2l+1)
    const int N = 200;
    double M[36][36] {};
    double wsum = 0;
    for (int a = 0; a < N; ++a)
        for (int b = 0; b < N; ++b)
        {
            const float azd = -180.0f + 360.0f * (a + 0.5f) / N;
            const double u = -1.0 + 2.0 * (b + 0.5) / N;                     // sin(el) uniform -> equal area
            const float eld = (float) juce::radiansToDegrees (std::asin (u));
            encodeAmbiX (azd, eld, 5, g);
            for (int i = 0; i < 36; ++i) for (int j = 0; j < 36; ++j) M[i][j] += (double) g[i] * g[j];
            wsum += 1;
        }
    double maxErr = 0;
    for (int i = 0; i < 36; ++i)
        for (int j = 0; j < 36; ++j)
        {
            const int l = (int) std::floor (std::sqrt ((double) i));
            const double expect = i == j ? 1.0 / (2 * l + 1) : 0.0;
            maxErr = std::max (maxErr, std::abs (M[i][j] / wsum - expect));
        }
    check (maxErr < 2e-3, "SN3D orthogonality up to order 5 (max err " + juce::String (maxErr, 5) + ")");

    float s[2];
    encodeStereo (90.0f, s);  check (s[0] > 0.999f && s[1] < 0.001f, "stereo: +90 = left");
    encodeStereo (0.0f, s);   check (std::abs (s[0] - s[1]) < 1e-5 && std::abs (s[0] * s[0] + s[1] * s[1] - 1) < 1e-5, "stereo: centre equal power");
}

static void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
    auto opts = juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (b.getNumChannels()).withBitsPerSample (24);
    if (auto w = wav.createWriterFor (os, opts))
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

struct Scene
{
    juce::String name;
    int outputMode;
    int channels;
    std::function<void (EngineParams&, double t)> params;
    juce::MidiBuffer midiAll;        // positions in samples from the start
    double seconds;
};

static juce::AudioBuffer<float> render (OrganEngine& e, Scene& sc, double sr, int block, double& rtFactor, int& maxVoices)
{
    const int total = (int) (sc.seconds * sr);
    juce::AudioBuffer<float> out (sc.channels, total), tmp (sc.channels, block);
    EngineParams p;
    p.outputMode = sc.outputMode;
    auto t0 = std::chrono::high_resolution_clock::now();
    maxVoices = 0;
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = std::min (block, total - pos);
        tmp.setSize (sc.channels, n, false, false, true);
        juce::MidiBuffer mb;
        for (const auto m : sc.midiAll)
            if (m.samplePosition >= pos && m.samplePosition < pos + n)
                mb.addEvent (m.getMessage(), m.samplePosition - pos);
        sc.params (p, pos / sr);
        e.process (tmp, mb, p);
        maxVoices = std::max (maxVoices, e.activeVoices.load());
        for (int c = 0; c < sc.channels; ++c) out.copyFrom (c, pos, tmp, c, 0, n);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    rtFactor = sc.seconds / std::chrono::duration<double> (t1 - t0).count();
    return out;
}

int main (int argc, char** argv)
{
    testEncoder();
    if (argc < 3) { std::cout << "usage: RysumRenderTest <samples> <outdir>" << std::endl; return failures; }

    juce::File folder (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    juce::File outDir (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    outDir.createDirectory();

    std::cout << "loading " << folder.getFullPathName() << std::endl;
    juce::String err;
    auto t0 = std::chrono::high_resolution_clock::now();
    std::shared_ptr<const SampleLibrary> lib (SampleLibrary::load (folder, err).release());
    auto t1 = std::chrono::high_resolution_clock::now();
    check (lib != nullptr, "library loaded " + err);
    if (lib == nullptr) return 1;
    std::cout << "  " << lib->loadedPipes << " pipes, " << lib->substitutedPipes << " substituted, "
              << lib->clicksDown.size() << "/" << lib->clicksUp.size() << "/" << lib->clicksOther.size() << " clicks, "
              << lib->stopPulls.size() << " pulls, " << lib->wind.size() << " wind, "
              << std::chrono::duration<double> (t1 - t0).count() << " s" << std::endl;
    for (int s = 0; s < kNumStops; ++s)
        for (int k = 0; k < kNumKeys; ++k)
        {
            const auto& p = lib->pipes[(size_t) s][(size_t) k];
            if (p.substituted) std::cout << "  substituted " << kStops[(size_t) s].id << " " << helmholtzName (p.midi) << " <- " << p.flags.joinIntoString (", ") << std::endl;
            if (! p.valid) std::cout << "  INVALID " << kStops[(size_t) s].id << " " << helmholtzName (p.midi) << std::endl;
        }

    const double sr = 48000.0; const int block = 256;
    auto note = [sr] (juce::MidiBuffer& mb, int midi, double on, double off)
    {
        mb.addEvent (juce::MidiMessage::noteOn (1, midi, (juce::uint8) 100), (int) (on * sr));
        mb.addEvent (juce::MidiMessage::noteOff (1, midi), (int) (off * sr));
    };

    std::vector<Scene> scenes;
    {   // 1. Praestant alone: a slow walk through all keys (stereo)
        Scene s { "01_praestant_scale_stereo", 0, 2, nullptr, {}, 0 };
        double t = 0.2;
        for (int k = 0; k < kNumKeys; ++k) { note (s.midiAll, kPhysKeys[(size_t) k], t, t + 0.55); t += 0.6; }
        s.seconds = t + 1.5;
        s.params = [] (EngineParams& p, double) { p.stopOn = {}; p.stopOn[0] = true; };
        scenes.push_back (s);
    }
    {   // 2. held chord, stops pulled one after another, long hold (loops!), stereo
        Scene s { "02_plenum_build_stereo", 0, 2, nullptr, {}, 14.0 };
        for (int m : { 48, 55, 60, 64, 67 }) note (s.midiAll, m, 0.3, 12.0);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {};
            for (int i = 0; i < kNumStops; ++i) p.stopOn[(size_t) i] = t > 0.2 + 1.3 * i;
        };
        scenes.push_back (s);
    }
    {   // 3. same in AmbiX 5th order with motion
        Scene s { "03_plenum_ambix5_motion", 5, 36, nullptr, {}, 12.0 };
        for (int m : { 41, 53, 60, 65, 69, 72 }) note (s.midiAll, m, 0.3, 10.0);
        s.params = [] (EngineParams& p, double)
        {
            p.stopOn.fill (true);
            p.motionMode.fill ((int) Motion::Rotation);
            p.motionRate.fill (0.15f);
            p.motionDepth.fill (40.0f);
            p.motionMode[1] = (int) Motion::Drift;
        };
        scenes.push_back (s);
    }
    {   // 4. degradation per pipe, then on the sum (stereo)
        Scene s { "04_verfall_stereo", 0, 2, nullptr, {}, 12.0 };
        for (int m : { 45, 52, 57, 61, 64 }) note (s.midiAll, m, 0.2, 5.5);
        for (int m : { 45, 52, 57, 61, 64 }) note (s.midiAll, m, 6.2, 11.0);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {}; p.stopOn[0] = p.stopOn[1] = p.stopOn[2] = true;
            p.deg.amount = 0.9f; p.deg.grain = 0.6f; p.deg.bits = 7.0f; p.deg.rateHz = 6000.0f; p.deg.wobble = 0.7f; p.deg.dropouts = 0.4f;
            p.deg.perPipe = t < 6.0;
        };
        scenes.push_back (s);
    }
    {   // 5. stress: all stops, all 41 keys at once, AmbiX 5
        Scene s { "05_stress_all_keys_ambix5", 5, 36, nullptr, {}, 6.0 };
        for (int k = 0; k < kNumKeys; ++k) note (s.midiAll, kPhysKeys[(size_t) k], 0.1 + 0.01 * k, 4.0);
        s.params = [] (EngineParams& p, double) { p.stopOn.fill (true); p.motionMode.fill ((int) Motion::Drift); p.motionRate.fill (0.3f); p.motionDepth.fill (60.0f);
                                                  p.deg.amount = 0.5f; p.deg.perPipe = true; };
        scenes.push_back (s);
        Scene s2 = s; s2.name = "06_stress_all_keys_ambix5_clean";
        s2.params = [] (EngineParams& p, double) { p.stopOn.fill (true); };
        scenes.push_back (s2);
    }

    for (auto& sc : scenes)
    {
        OrganEngine e;
        e.prepare (sr, block);
        e.setLibrary (lib);
        double rt = 0; int maxV = 0;
        auto b = render (e, sc, sr, block, rt, maxV);
        bool finite = true;
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! std::isfinite (b.getSample (c, i))) { finite = false; break; }
        const float peak = b.getMagnitude (0, b.getNumSamples());
        const float rms0 = b.getRMSLevel (0, 0, b.getNumSamples());
        std::cout << sc.name << ": peak " << juce::Decibels::gainToDecibels (peak) << " dBFS, rms ch0 "
                  << juce::Decibels::gainToDecibels (rms0) << " dB, " << rt << "x realtime, max voices " << maxV << std::endl;
        check (finite, sc.name + " finite");
        check (peak > 0.001f, sc.name + " not silent");
        check (rt > 1.0, sc.name + " faster than realtime");
        writeWav (outDir.getChildFile (sc.name + ".wav"), b, sr);
    }
    // pitch check at a different host sample rate: Praestant a' must stay at its recorded pitch,
    // the substituted Gedackt f must land on meantone f
    for (double hostSr : { 44100.0, 48000.0, 96000.0 })
    for (auto [stop, midi] : { std::pair<int, int> { 0, 69 }, std::pair<int, int> { 1, 53 } })
    {
        OrganEngine e;
        e.prepare (hostSr, 512);
        e.setLibrary (lib);
        EngineParams p; p.stopOn = {}; p.stopOn[(size_t) stop] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0;
        juce::AudioBuffer<float> b (2, 512), all (1, (int) (hostSr * 2));
        for (int pos = 0; pos < all.getNumSamples(); pos += 512)
        {
            juce::MidiBuffer mb;
            if (pos == 0) mb.addEvent (juce::MidiMessage::noteOn (1, midi, (juce::uint8) 100), 0);
            e.process (b, mb, p);
            all.copyFrom (0, pos, b, 0, 0, juce::jmin (512, all.getNumSamples() - pos));
        }
        // autocorrelation pitch on 0.5..1.5 s
        const float* x = all.getReadPointer (0) + (int) (0.5 * hostSr);
        const int N = (int) hostSr;
        const double f0nom = meantoneHz (midi);
        int bestLag = 0; double best = -1e9;
        for (int lag = (int) (hostSr / (f0nom * 1.1)); lag <= (int) (hostSr / (f0nom / 1.1)); ++lag)
        {
            double s = 0; for (int i = 0; i < N - lag; ++i) s += (double) x[i] * x[i + lag];
            if (s > best) { best = s; bestLag = lag; }
        }
        // parabolic refinement
        auto ac = [&] (int lag) { double s = 0; for (int i = 0; i < N - lag; ++i) s += (double) x[i] * x[i + lag]; return s; };
        const double y0 = ac (bestLag - 1), y1 = ac (bestLag), y2 = ac (bestLag + 1);
        const double lagF = bestLag + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2);
        const double f = hostSr / lagF;
        const double cents = 1200.0 * std::log2 (f / f0nom);
        std::cout << "pitch " << kStops[(size_t) stop].id << " " << helmholtzName (midi) << " @" << hostSr << ": " << f << " Hz (" << cents << " ct vs meantone)" << std::endl;
        check (std::abs (cents) < 8.0, "pitch within 8 ct of meantone");
    }

    std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string (failures)) << std::endl;
    return failures;
}
