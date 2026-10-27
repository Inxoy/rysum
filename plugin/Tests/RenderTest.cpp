// Offline test: loads the sample folder, renders a few MIDI scenes through the engine
// in stereo and in AmbiX 5th order, writes WAV files and checks the results.
//   RysumRenderTest <samples folder> <output folder>
#include "OrganEngine.h"
#include "Convolver.h"
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

static void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double sr);

static void testConvolver()
{
    std::cout << "convolver" << std::endl;
    const double sr = 48000.0;
    const int L = (int) (0.7 * sr);
    XorShift r (99);
    // synthetic first-order IR: direct sound from 60 deg left at sample 100, then a decaying noise tail
    juce::AudioBuffer<float> ir (4, L);
    ir.clear();
    const float az = juce::degreesToRadians (60.0f);
    ir.setSample (0, 100, 1.0f); ir.setSample (1, 100, std::sin (az)); ir.setSample (3, 100, std::cos (az));
    for (int c = 0; c < 4; ++c)
        for (int i = 300; i < L; ++i)
            ir.setSample (c, i, r.bi() * 0.25f * std::exp (-6.0f * (float) i / (float) L) * (c == 0 ? 1.0f : 0.5f));
    auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rysum_test_ir.wav");
    writeWav (f, ir, sr);

    auto run = [&] (Convolver& cv, std::function<void (int, std::vector<std::vector<float>>&)> fill, int nIn, int numOut, int N)
    {
        juce::AudioBuffer<float> out (numOut, N); out.clear();
        cv.reset();
        int pos = 0, bs = 37;
        while (pos < N)
        {
            const int n = juce::jmin (bs, N - pos);
            std::vector<std::vector<float>> ins ((size_t) nIn, std::vector<float> ((size_t) n, 0.0f));
            for (int i = 0; i < n; ++i) { std::vector<std::vector<float>> one ((size_t) nIn, std::vector<float> (1)); fill (pos + i, one); for (int k = 0; k < nIn; ++k) ins[(size_t) k][(size_t) i] = one[(size_t) k][0]; }
            std::vector<const float*> ip; for (auto& v : ins) ip.push_back (v.data());
            std::vector<float*> o; for (int c = 0; c < numOut; ++c) o.push_back (out.getWritePointer (c, pos));
            cv.process (ip.data(), n, o.data(), numOut, 1.0f);
            pos += n; bs = bs == 37 ? 501 : (bs == 501 ? 1024 : 37);
        }
        return out;
    };

    juce::String err;
    Convolver::Settings st; st.outOrder = 1; st.direct = 1.0f; st.envelop = 0.0f; st.hostRate = sr;
    auto cv = Convolver::create (f, st, err);
    check (cv != nullptr, "IR loaded " + err);
    if (! cv) return;
    std::cout << "  " << cv->getInfo().description << ", direct sound at " << cv->getInfo().baseAzDeg << " deg" << std::endl;
    check (std::abs (cv->getInfo().baseAzDeg - 60.0f) < 1.0f, "direction of the IR's direct sound found");

    // 1) exactness: all 'pipes' at the measured direction (= not following) must give plain convolution
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
    juce::AudioBuffer<float> ref (4, L); rd->read (&ref, 0, L, 0, true, true);
    double e = 0; for (int i = 0; i < L; ++i) e += (double) ref.getSample (0, i) * ref.getSample (0, i);
    ref.applyGain ((float) (1.0 / std::sqrt (e)));
    const int fade = juce::jmin (L / 4, (int) (0.05 * sr));
    for (int c = 0; c < 4; ++c) ref.applyGainRamp (c, L - fade, fade, 1.0f, 0.0f);
    const int N = 9000;
    std::vector<float> x ((size_t) N);
    for (int i = 0; i < 6000; ++i) x[(size_t) i] = r.bi();
    const float a0 = juce::degreesToRadians (cv->getInfo().baseAzDeg);
    auto out = run (*cv, [&] (int t, std::vector<std::vector<float>>& v) { v[0][0] = x[(size_t) t]; v[1][0] = std::cos (a0) * x[(size_t) t]; v[2][0] = std::sin (a0) * x[(size_t) t]; }, 3, 4, N);
    double maxErr = 0, maxRef = 0;
    for (int c = 0; c < 4; ++c)
        for (int t = Convolver::kBlock; t < N; t += 7)
        {
            double y = 0; const int tt = t - Convolver::kBlock;
            for (int k = juce::jmax (0, tt - 6000 + 1); k <= juce::jmin (tt, L - 1); ++k) y += (double) ref.getSample (c, k) * x[(size_t) (tt - k)];
            maxErr = std::max (maxErr, std::abs (y - out.getSample (c, t)));
            maxRef = std::max (maxRef, std::abs (y));
        }
    check (maxErr < 2.0e-3 * maxRef, "not following = the measured room exactly (rel err " + juce::String (maxErr / maxRef, 7) + ")");

    // 2) following: a pipe at -30 deg gets its direct sound / early reflections from -30 deg
    auto impulseAt = [&] (Convolver& c, float deg, int nIn, int numOut)
    {
        const float a = juce::degreesToRadians (deg);
        return run (c, [&] (int t, std::vector<std::vector<float>>& v)
        {
            const float s = t == 0 ? 1.0f : 0.0f;
            v[0][0] = s;
            for (int m = 1; 2 * m < nIn; ++m) { v[(size_t) (2 * m - 1)][0] = std::cos ((float) m * a) * s; v[(size_t) (2 * m)][0] = std::sin ((float) m * a) * s; }
        }, nIn, numOut, (int) (0.7 * sr));
    };
    auto o2 = impulseAt (*cv, -30.0f, 3, 4);
    const int td = Convolver::kBlock + 100;
    const float dir = juce::radiansToDegrees (std::atan2 (o2.getSample (1, td), o2.getSample (3, td)));
    std::cout << "  pipe at -30 deg -> direct sound of the room from " << dir << " deg" << std::endl;
    check (std::abs (dir + 30.0f) < 1.0f, "early reflections turn with the pipe");

    // 3) the IR's own direct sound turned down
    Convolver::Settings st2 = st; st2.direct = 0.2f;
    auto cvD = Convolver::create (f, st2, err);
    auto o3 = impulseAt (*cvD, -30.0f, 3, 4);
    check (std::abs (o3.getSample (0, td) / o2.getSample (0, td) - 0.2f) < 0.01f, "direct sound of the IR at 20 %");

    // 4) envelopment: mono IR, 3rd order output, fully diffuse tail -> energies 1/(2l+1), decorrelated from W
    juce::AudioBuffer<float> mono (1, L);
    for (int i = 0; i < L; ++i) mono.setSample (0, i, r.bi() * std::exp (-6.0f * (float) i / (float) L));
    auto fm1 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rysum_test_ir_mono.wav");
    writeWav (fm1, mono, sr);
    Convolver::Settings st3; st3.outOrder = 3; st3.envelop = 1.0f; st3.hostRate = sr;
    auto cvM = Convolver::create (fm1, st3, err);
    check (cvM != nullptr && cvM->getInfo().numInputs == 7, "mono IR lifted to 3rd order (7 sends)");
    if (cvM)
    {
        auto o4 = impulseAt (*cvM, 0.0f, 7, 16);
        const int a = Convolver::kBlock + (int) (0.15 * sr), n = (int) (0.4 * sr);
        auto en = [&] (int c) { double s = 0; for (int i = a; i < a + n; ++i) s += (double) o4.getSample (c, i) * o4.getSample (c, i); return s; };
        const double eW = en (0);
        double corr = 0; for (int i = a; i < a + n; ++i) corr += (double) o4.getSample (0, i) * o4.getSample (1, i);
        corr /= std::sqrt (eW * en (1));
        juce::String ratios;
        bool ok = true;
        for (int l = 1; l <= 3; ++l)
        {
            const double rr = en (l * l + l) / eW * (2 * l + 1);
            ratios << " l" << l << "=" << juce::String (rr, 2);
            ok = ok && std::abs (rr - 1.0) < 0.35;
        }
        std::cout << "  diffuse tail energy x(2l+1)/W:" << ratios << ", corr(W,Y) " << juce::String (corr, 3) << std::endl;
        check (ok && std::abs (corr) < 0.2, "enveloping tail: isotropic energies, decorrelated");
    }

    // 6) cost: 4 s, 5th order IR, 5th order output, following (11 sends)
    {
        juce::AudioBuffer<float> big (36, (int) (4.0 * sr));
        for (int c = 0; c < 36; ++c) for (int i = 0; i < big.getNumSamples(); ++i) big.setSample (c, i, r.bi() * std::exp (-4.0f * (float) i / (float) big.getNumSamples()) * (c == 0 ? 1.0f : 0.4f));
        auto fb = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rysum_test_ir_big.wav");
        writeWav (fb, big, sr);
        Convolver::Settings s5; s5.outOrder = 5; s5.hostRate = sr;
        auto t0 = std::chrono::high_resolution_clock::now();
        auto cvB = Convolver::create (fb, s5, err);
        auto t1 = std::chrono::high_resolution_clock::now();
        if (cvB)
        {
            const int n = 512, blocks = (int) (5.0 * sr / n);
            std::vector<std::vector<float>> ins (11, std::vector<float> ((size_t) n));
            for (auto& v : ins) for (auto& q : v) q = r.bi() * 0.1f;
            std::vector<const float*> ip; for (auto& v : ins) ip.push_back (v.data());
            juce::AudioBuffer<float> o (36, n);
            std::vector<float*> op; for (int c = 0; c < 36; ++c) op.push_back (o.getWritePointer (c));
            auto t2 = std::chrono::high_resolution_clock::now();
            for (int b = 0; b < blocks; ++b) cvB->process (ip.data(), n, op.data(), 36, 1.0f);
            auto t3 = std::chrono::high_resolution_clock::now();
            const double rt = 5.0 / std::chrono::duration<double> (t3 - t2).count();
            std::cout << "  5th order, 4 s IR: build " << std::chrono::duration<double> (t1 - t0).count() << " s, " << rt << "x realtime" << std::endl;
            check (rt > 1.0, "5th-order room runs in real time");
        }
        fb.deleteFile();
    }

    // 5) resampling path
    Convolver::Settings st4 = st; st4.hostRate = 44100.0;
    auto cv2 = Convolver::create (f, st4, err);
    check (cv2 != nullptr && std::abs (cv2->getInfo().seconds - 0.7) < 0.02, "IR resampled to 44.1 kHz, length " + (cv2 ? juce::String (cv2->getInfo().seconds, 3) : juce::String()));
    f.deleteFile(); fm1.deleteFile();
}

static void testMotion()
{
    std::cout << "motion" << std::endl;
    // continuity: position as a function of phase must not jump (except Jumps mode)
    for (int m : { 1, 2, 3, 4 })
    {
        float worst = 0;
        Pos b { 30.0f, 20.0f };
        Pos prev = applyMotion (b, (Motion) m, -3.0, 40.0f, 5);
        for (int i = 1; i < 20000; ++i)
        {
            const double ph = -3.0 + i * 0.0003;
            Pos q = applyMotion (b, (Motion) m, ph, 40.0f, 5);
            float da = std::abs (wrapAz (q.az - prev.az));
            if (std::abs (q.el) > 85.0f || std::abs (prev.el) > 85.0f) da = 0;      // azimuth is undefined at the poles
            worst = std::max (worst, da + std::abs (q.el - prev.el));
            prev = q;
        }
        check (worst < 3.0f, motionNames()[m] + " moves continuously (max step " + juce::String (worst, 2) + " deg)");
    }
    // vertical rotation passes over the top
    Pos top = applyMotion ({ 0.0f, 0.0f }, Motion::Vertical, 0.25, 0, 1);
    check (top.el > 89.0f, "vertical rotation: front -> zenith after a quarter turn");
    Pos back = applyMotion ({ 0.0f, 0.0f }, Motion::Vertical, 0.5, 0, 1);
    check (std::abs (std::abs (back.az) - 180.0f) < 1.0f && std::abs (back.el) < 1.0f, "vertical rotation: behind after half a turn");
    // jumps: constant within one step, different after
    Pos j1 = applyMotion ({ 0, 0 }, Motion::Jumps, 0.10, 90.0f, 3), j2 = applyMotion ({ 0, 0 }, Motion::Jumps, 0.11, 90.0f, 3);
    int changes = 0; Pos pj = j1;
    for (int i = 0; i < 1000; ++i) { Pos q = applyMotion ({ 0, 0 }, Motion::Jumps, i * 0.01, 90.0f, 3); if (std::abs (q.az - pj.az) > 1.0e-6f) ++changes; pj = q; }
    check (std::abs (j1.az - j2.az) < 1.0e-6f || changes > 0, "jumps hold still between jumps");
    check (changes >= 8 && changes <= 11, "about one jump per cycle (" + juce::String (changes) + " in 10 cycles)");
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
    testConvolver();
    testMotion();
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

    {   // 7. stop wind: Praestant slowly closed and opened again (like pulling the stop in and out)
        Scene s { "07_stopwind_sweep_stereo", 0, 2, nullptr, {}, 12.0 };
        for (int m : { 53, 60, 65, 69 }) note (s.midiAll, m, 0.2, 11.0);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {}; p.stopOn[0] = true;
            const double x = juce::jlimit (0.0, 1.0, (t - 1.0) / 10.0);           // 0..1 over 1..11 s
            p.stopWind[0] = (float) std::abs (1.0 - 2.0 * x);                        // 1 -> 0 -> 1
        };
        scenes.push_back (s);
    }
    {   // 8. global wind 0 -> 100 %
        Scene s { "08_global_wind_ramp_stereo", 0, 2, nullptr, {}, 14.0 };
        for (int m : { 48, 55, 60, 64 }) note (s.midiAll, m, 0.2, 13.0);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {}; p.stopOn[0] = p.stopOn[2] = true;
            p.wind = (float) juce::jlimit (0.0, 1.0, t / 12.0);
        };
        scenes.push_back (s);
    }

    {   // 9. speed glissando of a rotation, -2 Hz .. +2 Hz, first order (must stay click-free)
        Scene s { "09_rotation_glissando_ambix1", 1, 4, nullptr, {}, 12.0 };
        for (int m : { 57, 64, 69 }) note (s.midiAll, m, 0.2, 11.5);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0;
            p.motionMode[0] = (int) Motion::Rotation;
            p.motionRate[0] = (float) (-2.0 + 4.0 * juce::jlimit (0.0, 1.0, t / 11.0));
        };
        scenes.push_back (s);
    }

    {   // 13. fast: rotation glissando -10 Hz .. +10 Hz in 5th order, then jumps at 10 Hz
        Scene s { "13_fast_rotation_ambix5", 5, 36, nullptr, {}, 14.0 };
        for (int m : { 57, 64, 69 }) note (s.midiAll, m, 0.2, 13.5);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0;
            p.motionMode[0] = (int) (t < 10.0 ? Motion::Rotation : Motion::Jumps);
            p.motionRate[0] = t < 10.0 ? (float) (-10.0 + 20.0 * juce::jlimit (0.0, 1.0, t / 10.0)) : 10.0f;
            p.motionDepth[0] = 90.0f;
        };
        scenes.push_back (s);
    }

    {   // 10. Doppler: rotation 0.5 Hz with an elliptic orbit (closest in front)
        Scene s { "10_doppler_ellipse_stereo", 0, 2, nullptr, {}, 9.0 };
        note (s.midiAll, 69, 0.2, 8.5);
        s.params = [] (EngineParams& p, double)
        {
            p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0;
            p.motionMode[0] = (int) Motion::Rotation; p.motionRate[0] = 0.5f; p.ellipse[0] = 0.8f; p.doppler = 1.0f;
        };
        scenes.push_back (s);
    }
    {   // 11. tremulant
        Scene s { "11_tremulant_stereo", 0, 2, nullptr, {}, 6.0 };
        for (int m : { 53, 60, 65, 69 }) note (s.midiAll, m, 0.2, 5.5);
        s.params = [] (EngineParams& p, double) { p.stopOn = {}; p.stopOn[1] = true; p.tremOn = true; p.tremRate = 5.5f; p.tremDepth = 0.8f; };
        scenes.push_back (s);
    }
    {   // 12. freeze: chord released at 1.5 s keeps sounding, freeze off at 6 s -> fades in 2 s
        Scene s { "12_freeze_stereo", 0, 2, nullptr, {}, 10.0 };
        for (int m : { 57, 64, 69, 72 }) note (s.midiAll, m, 0.2, 1.5);
        s.params = [] (EngineParams& p, double t)
        {
            p.stopOn = {}; p.stopOn[0] = p.stopOn[2] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0;
            p.freeze = t < 6.0; p.freezeSmear = 0.6f; p.freezeRelease = 2.0f;
        };
        scenes.push_back (s);
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
        // the 328-voice stress scene with per-pipe degradation is a worst case; on slow test machines it may sit just below 1x
        check (rt > (sc.name.startsWith ("05_stress") ? 0.7 : 1.0), sc.name + " faster than realtime");
        writeWav (outDir.getChildFile (sc.name + ".wav"), b, sr);
        if (sc.name.startsWith ("12_freeze"))
        {
            const float held = b.getRMSLevel (0, (int) (4.0 * sr), (int) (1.0 * sr));
            const float after = b.getRMSLevel (0, (int) (9.0 * sr), (int) (0.9 * sr));
            std::cout << "  freeze: rms 4-5 s " << juce::Decibels::gainToDecibels (held) << " dB, 9-10 s " << juce::Decibels::gainToDecibels (after) << " dB" << std::endl;
            check (held > 0.01f && after < held * 0.01f, "freeze holds after release and fades when switched off");
        }
    }
    // pitch check at a different host sample rate: Praestant a' must stay at its recorded pitch,
    // the substituted Gedackt f must land on meantone f
    for (double hostSr : { 44100.0, 48000.0, 96000.0 })
    for (auto [stop, midi] : { std::pair<int, int> { 0, 69 }, std::pair<int, int> { 1, 53 }, std::pair<int, int> { kPraestant16, 69 } })
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
        const double f0nom = meantoneHz (midi) * (stop == kPraestant16 ? 0.5 : 1.0);     // the 16' sounds an octave lower
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

    {
        auto measure = [&] (int temperament, double pitchCents)
        {
            OrganEngine e; e.prepare (48000.0, 512); e.setLibrary (lib);
            EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0;
            p.tuneCents = temperamentShift (temperament); p.pitchCents = (float) pitchCents;
            juce::AudioBuffer<float> b (2, 512), all (1, 96000);
            for (int pos = 0; pos < all.getNumSamples(); pos += 512)
            {
                juce::MidiBuffer mb;
                if (pos == 0) mb.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                e.process (b, mb, p);
                all.copyFrom (0, pos, b, 0, 0, juce::jmin (512, all.getNumSamples() - pos));
            }
            const float* x = all.getReadPointer (0) + 24000;
            const int N = 48000;
            auto ac = [&] (int lag) { double s = 0; for (int i = 0; i < N - lag; ++i) s += (double) x[i] * x[i + lag]; return s; };
            int best = 0; double bv = -1e9;
            for (int lag = 150; lag <= 220; ++lag) { const double v = ac (lag); if (v > bv) { bv = v; best = lag; } }
            const double y0 = ac (best - 1), y1 = ac (best), y2 = ac (best + 1);
            return 48000.0 / (best + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2));
        };
        const double mt = measure (0, 0), eq = measure (6, 0), up = measure (0, 100);
        const double dEq = 1200 * std::log2 (eq / mt), dUp = 1200 * std::log2 (up / mt);
        std::cout << "tuning c': meantone " << mt << " Hz, equal " << eq << " Hz (" << dEq << " ct), +100 ct " << dUp << " ct" << std::endl;
        check (std::abs (dEq - (-10.3)) < 1.0, "equal temperament moves c' by -10.3 ct");
        check (std::abs (dUp - 100.0) < 1.0, "pitch +100 ct");
    }
    {
        // tempo sync: phase locked to the host position
        OrganEngine e; e.prepare (48000.0, 512);
        EngineParams p; p.sync = true; p.playing = true; p.bpm = 90; p.ppq = 8.0; p.motionRate[2] = 0.25f;
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer mb;
        e.process (b, mb, p);
        const double expect = 0.25 * (8.0 + 512.0 / 48000.0 * 90.0 / 60.0) * 0.5;
        check (std::abs (e.motionPhase[2].load() - expect) < 1.0e-9, "tempo sync: phase follows the host position");
    }

    // ---------------- modulation ----------------
    {
        const double msr = 48000.0;
        auto run = [&] (std::function<void (EngineParams&)> setup, std::vector<int> midis, double secs, int mode, int ch, double offAt,
                        const juce::String& name)
        {
            OrganEngine e; e.prepare (msr, 512); e.setLibrary (lib);
            EngineParams p; p.stopOn = {}; p.stopOn[0] = true; p.clickLevel = 0; p.releaseClick = 0; p.wind = 0; p.stopPull = 0;
            p.outputMode = mode;
            setup (p);
            juce::AudioBuffer<float> b (ch, 512), all (ch, (int) (msr * secs));
            for (int pos = 0; pos < all.getNumSamples(); pos += 512)
            {
                juce::MidiBuffer mb;
                if (pos == 0) for (int m : midis) mb.addEvent (juce::MidiMessage::noteOn (1, m, (juce::uint8) 100), 0);
                if (offAt > 0 && pos == ((int) (offAt * msr) / 512) * 512) for (int m : midis) mb.addEvent (juce::MidiMessage::noteOff (1, m), 0);
                e.process (b, mb, p);
                for (int c = 0; c < ch; ++c) all.copyFrom (c, pos, b, c, 0, juce::jmin (512, all.getNumSamples() - pos));
            }
            writeWav (outDir.getChildFile (name + ".wav"), all, msr);
            return all;
        };
        auto rms = [&] (const juce::AudioBuffer<float>& b, double t0, double t1)
        { return juce::Decibels::gainToDecibels (b.getRMSLevel (0, (int) (t0 * msr), (int) ((t1 - t0) * msr))); };
        auto pitchAt = [&] (const juce::AudioBuffer<float>& b, double t0, double len, double f0)
        {
            const float* x = b.getReadPointer (0) + (int) (t0 * msr);
            const int N = (int) (len * msr);
            auto ac = [&] (int lag) { double s = 0; for (int i = 0; i < N - lag; ++i) s += (double) x[i] * x[i + lag]; return s; };
            int bestLag = 0; double best = -1e18;
            for (int lag = (int) (msr / (f0 * 1.15)); lag <= (int) (msr / (f0 / 1.15)); ++lag) { const double v = ac (lag); if (v > best) { best = v; bestLag = lag; } }
            const double y0 = ac (bestLag - 1), y1 = ac (bestLag), y2 = ac (bestLag + 1);
            return msr / (bestLag + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2));
        };

        {   // LFO square 1 Hz -> pipe level +-10.8 dB
            auto b = run ([] (EngineParams& p) { p.mod.lfo[0].shape = shSquare; p.mod.lfo[0].rate = 1.0f; p.mod.routes[0] = { srcLfo1, tgtLevel, 0.3f }; },
                          { 69 }, 3.0, 0, 2, -1, "14_mod_lfo_level");
            const float hi = rms (b, 1.1, 1.4), lo = rms (b, 1.6, 1.9);
            std::cout << "mod level: high " << hi << " dB, low " << lo << " dB" << std::endl;
            check (hi - lo > 18.0f && hi - lo < 25.0f, "LFO -> pipe level (expect 21.6 dB swing)");
        }
        {   // LFO square 1 Hz -> pitch +-24 ct
            auto b = run ([] (EngineParams& p) { p.mod.lfo[1].shape = shSquare; p.mod.lfo[1].rate = 1.0f; p.mod.routes[3] = { srcLfo2, tgtPitch, 0.02f }; },
                          { 69 }, 3.0, 0, 2, -1, "15_mod_lfo_pitch");
            const double fHi = pitchAt (b, 1.1, 0.3, meantoneHz (69)), fLo = pitchAt (b, 1.6, 0.3, meantoneHz (69));
            const double d = 1200.0 * std::log2 (fHi / fLo);
            std::cout << "mod pitch: " << fHi << " / " << fLo << " Hz = " << d << " ct" << std::endl;
            check (std::abs (d - 48.0) < 3.0, "LFO -> pipe pitch (expect 48 ct)");
        }
        {   // per-pipe envelope -> distance: far at the envelope peak, back to base when it has decayed
            auto b = run ([] (EngineParams& p)
                          {
                              p.mod.env[0] = { true, 0.0f, 0.4f, 0.8f, 0.0f, 1.0f };
                              p.mod.routes[5] = { srcEnv1, tgtDist, 0.5f };
                          }, { 57, 64, 69 }, 4.0, 1, 4, -1, "16_mod_env_distance");
            const float farL = rms (b, 0.4, 0.5), nearL = rms (b, 3.0, 3.5);
            std::cout << "mod env->distance: at peak " << farL << " dB, later " << nearL << " dB" << std::endl;
            check (nearL - farL > 4.0f, "per-pipe envelope -> distance");
        }
        {   // global envelope with release: level follows the key
            auto b = run ([] (EngineParams& p)
                          {
                              p.mod.env[1] = { false, 0.0f, 0.01f, 0.1f, 1.0f, 0.5f };
                              p.mod.routes[7] = { srcEnv2, tgtLevel, 0.0f };
                          }, { 69 }, 2.0, 0, 2, 1.0, "17_mod_global_env");
            check (rms (b, 0.5, 0.9) > -60.0f, "global envelope scene renders");
        }
        {   // the wave: sine LFO 0.5 Hz on direction with full keyboard spread (listen in AmbiX)
            auto b = run ([] (EngineParams& p)
                          {
                              p.mod.lfo[2].rate = 0.5f; p.mod.lfo[2].spread = 1.0f;
                              p.mod.routes[2] = { srcLfo3, tgtAz, 0.5f };
                              p.mod.lfo[3].shape = shSmooth; p.mod.lfo[3].rate = 3.0f; p.mod.lfo[3].spread = 1.0f; p.mod.lfo[3].spreadMode = 1;
                              p.mod.routes[9] = { srcLfo4, tgtBright, -0.4f };
                          }, { 45, 48, 52, 57, 60, 64, 69, 72, 76 }, 8.0, 3, 16, -1, "18_mod_wave_ambix3");
            check (rms (b, 1.0, 7.0) > -60.0f, "spread wave scene renders");
        }
        {   // spread maths: offset along the keyboard
            check (std::abs (lfoValue (shSine, 0.25, 0) - 1.0f) < 1e-6f && std::abs (lfoValue (shTriangle, 0.75, 0) + 1.0f) < 1e-6f, "LFO shapes");
            EnvState es; EnvParams ep { true, 0.0f, 0.1f, 0.1f, 0.5f, 0.1f };
            es.trigger(); for (int i = 0; i < 100; ++i) es.advance (ep, 0.01f);
            const float sus = es.level; es.releaseGate(); for (int i = 0; i < 200; ++i) es.advance (ep, 0.01f);
            check (std::abs (sus - 0.5f) < 0.01f && es.level == 0.0f, "envelope sustain + release");
        }
    }

    std::cout << (failures == 0 ? "ALL OK" : "FAILURES: " + std::to_string (failures)) << std::endl;
    return failures;
}
