#include <functional>
#include <thread>
// Renders the three views of the editor into PNG files (for checking the UI without a host).
//   RysumUiSnapshot <samples folder> <output folder>
#include "ConsoleView.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SpaceView.h"

class SnapshotApp : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override
    {
        return "RysumUiSnapshot";
    }
    const juce::String getApplicationVersion() override
    {
        return "0.1";
    }

    void initialise(const juce::String &cmd) override
    {
        auto args = juce::StringArray::fromTokens(cmd, true);
        if (args.size() < 2)
        {
            std::cout << "usage: RysumUiSnapshot <samples> <outdir>" << std::endl;
            quit();
            return;
        }
        samples = juce::File::getCurrentWorkingDirectory().getChildFile(args[0].unquoted());
        outDir = juce::File::getCurrentWorkingDirectory().getChildFile(args[1].unquoted());
        outDir.createDirectory();
        proc = std::make_unique<RysumProcessor>();
        proc->loadSamples(samples);
        startTimer(100);
    }

    void timerCallback() override
    {
        if (proc->getLibrary() == nullptr && ++waited < 200)
            return;
        if (stage == 0)
        {
            std::cout << "status: " << proc->getLoadStatus() << std::endl;
            // a synthetic 4-channel AmbiX IR to exercise the room path
            {
                // like a host: main input and side chain stereo, 5th-order output
                juce::AudioProcessor::BusesLayout lay;
                lay.inputBuses.add(juce::AudioChannelSet::stereo());
                lay.inputBuses.add(juce::AudioChannelSet::stereo());
                lay.outputBuses.add(juce::AudioChannelSet::ambisonic(5));
                const bool okLayout = proc->setBusesLayout(lay);
                proc->setRateAndBufferSizeDetails(48000.0, 512);
                std::cout << "bus layout with inputs: "
                          << (okLayout && proc->getTotalNumInputChannels() == 4 ? "ok" : "FAIL")
                          << std::endl;
            }
            proc->prepareToPlay(48000.0, 512);
            juce::AudioBuffer<float> ir(4, 48000);
            juce::Random rnd(5);
            for (int c = 0; c < 4; ++c)
                for (int i = 0; i < 48000; ++i)
                    ir.setSample(c, i,
                                 (rnd.nextFloat() * 2 - 1) *
                                     std::exp(-5.0f * static_cast<float>(i) / 48000.0f));
            irFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("Testkirche_ambix.wav");
            irFile.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os(irFile.createOutputStream());
            if (auto w = wav.createWriterFor(os, juce::AudioFormatWriterOptions()
                                                     .withSampleRate(44100.0)
                                                     .withNumChannels(4)
                                                     .withBitsPerSample(24)))
                w->writeFromAudioSampleBuffer(ir, 0, ir.getNumSamples());
            proc->loadIr(irFile);
            stage = 1;
            waited = 0;
            return;
        }
        if (!proc->getIrStatus().contains("AmbiX") && ++waited < 100)
            return;
        stopTimer();
        std::cout << "ir: " << proc->getIrStatus() << std::endl;

        // play something so held keys / sounding pipes / meters show
        for (auto *id : {"stop0", "stop1", "stop2", "stop6"})
            proc->apvts.getParameter(id)->setValueNotifyingHost(1.0f);
        proc->apvts.getParameter("motion6")->setValueNotifyingHost(
            proc->apvts.getParameter("motion6")->convertTo0to1(1.0f));
        juce::AudioBuffer<float> buf(36, 512);
        for (int b = 0; b < 40; ++b)
        {
            juce::MidiBuffer mb;
            if (b == 2)
                for (int m : {57, 64, 69})
                    mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), 0);
            if (b == 1)
            {
                proc->apvts.getParameter("swind2")->setValueNotifyingHost(0.4f);
                proc->apvts.getParameter("vol1")->setValueNotifyingHost(0.6f);
            }
            buf.clear(), proc->processBlock(buf, mb);
        }
        // the room must add sound: same phrase with and without IR, compare the energy
        {
            proc->apvts.getParameter("wind")->setValueNotifyingHost(0.0f);
            proc->apvts.getParameter("click")->setValueNotifyingHost(0.0f);
            proc->apvts.getParameter("releaseClick")->setValueNotifyingHost(0.0f);
            proc->apvts.getParameter("output")->setValueNotifyingHost(
                proc->apvts.getParameter("output")->convertTo0to1(1.0f));
            auto phrase = [&]()
            {
                double e = 0;
                for (int b = 0; b < 200; ++b)
                {
                    juce::MidiBuffer mb;
                    if (b == 10)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
                    if (b == 40)
                        mb.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                    buf.clear(), proc->processBlock(buf, mb);
                    for (int c = 0; c < 4; ++c)
                        for (int i = 0; i < 512; ++i)
                            e += (double)buf.getSample(c, i) * buf.getSample(c, i);
                }
                return e;
            };
            const double withIr = phrase();
            proc->clearIr();
            {
                juce::MidiBuffer none;
                buf.clear(), proc->processBlock(buf, none);
            }
            const double dry = phrase();
            std::cout << "energy with IR / dry: " << withIr / dry
                      << (withIr > dry * 1.2 ? "  ok" : "  FAIL") << std::endl;
            proc->loadIr(irFile);
        }

        // typed values
        {
            auto P = [&](const char *id) { return proc->apvts.getParameter(id); };
            struct T
            {
                const char *id;
                const char *txt;
                float expect;
            };
            const T tests[]{{"click", "50", 0.5f},     {"click", "35 %", 0.35f},
                            {"lfo1Rate", "1,5", 1.5f}, {"env1Attack", "2 s", 2.0f},
                            {"rate0", "-2.5", -2.5f},  {"degRate", "22", 22000.0f},
                            {"mod1Amt", "-40", -0.4f}, {"master", "-6", -6.0f}};
            bool ok = true;
            for (auto &t : tests)
            {
                const float v = rysum::ui::parseValueText(*P(t.id), t.txt);
                if (std::abs(v - t.expect) > 1e-3f * std::max(1.0f, std::abs(t.expect)))
                {
                    ok = false;
                    std::cout << "  parse " << t.id << " '" << t.txt << "' -> " << v << std::endl;
                }
            }
            // ms when the parameter shows ms
            P("env1Attack")->setValueNotifyingHost(P("env1Attack")->convertTo0to1(0.3f));
            ok = ok && std::abs(rysum::ui::parseValueText(*P("env1Attack"), "500") - 0.5f) < 1e-4f;
            P("env1Attack")->setValueNotifyingHost(P("env1Attack")->getDefaultValue());
            const float cents = rysum::ui::parseValueText(*P("pitch"), "440");
            ok = ok && std::abs(cents - 1200.0f * std::log2(440.0f / (float)rysum::kA4)) < 0.1f;
            std::cout << "typed values: " << (ok ? "ok" : "FAIL") << std::endl;
        }

        // binaural: output on "Binaural", decoder designed in the background, 2-channel host buffer
        {
            auto P = [&](const char *id) { return proc->apvts.getParameter(id); };
            const float outBefore = P("output")->getValue();
            P("output")->setValueNotifyingHost(
                P("output")->convertTo0to1((float)RysumProcessor::kBinauralOutput));
            auto pump = [&](std::function<bool()> done)
            {
                for (int i = 0; i < 100 && !done(); ++i)
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
            };
            pump([&] { return proc->binauralReady(); });
            std::cout << "binaural: " << proc->getBinauralStatus()
                      << (proc->binauralReady() ? "  ok" : "  FAIL") << std::endl;
            for (auto *id : {"stop1", "stop2", "stop6"})
                P(id)->setValueNotifyingHost(0.0f);
            const float irWetBefore = P("irWet")->getValue();
            P("irWet")->setValueNotifyingHost(0.0f);
            auto play = [&](int ch)
            {
                juce::AudioBuffer<float> st(ch, 512);
                double l = 0, r = 0, rest = 0;
                for (int b = 0; b < 260; ++b)
                {
                    juce::MidiBuffer mb;
                    if (b == 0)
                        for (int m : {57, 60, 64, 69})
                            mb.addEvent(juce::MidiMessage::noteOff(1, m),
                                        0); // earlier tests left keys down
                    if (b == 150)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)100), 0);
                    if (b == 250)
                        mb.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
                    st.clear(), proc->processBlock(st, mb);
                    if (b > 170 && b < 250)
                        for (int i = 0; i < 512; ++i)
                        {
                            l += (double)st.getSample(0, i) * st.getSample(0, i);
                            if (ch > 1)
                                r += (double)st.getSample(1, i) * st.getSample(1, i);
                            for (int c = 2; c < ch; ++c)
                                rest += (double)st.getSample(c, i) * st.getSample(c, i);
                        }
                }
                return std::array<double, 3>{l, r, rest};
            };
            const auto e2 = play(2), e36 = play(36);
            const double pipeAz =
                proc->engine.currentPosition(0, rysum::keyIndexForMidi(69), proc->readParams()).az;
            std::cout << "binaural Praestant a' (az " << pipeAz << "): L/R "
                      << 10 * std::log10(e2[0] / e2[1]) << " dB, 36-ch bus: L/R "
                      << 10 * std::log10(e36[0] / e36[1]) << " dB, other channels " << e36[2]
                      << ((e2[0] > 1e-6 &&
                           (pipeAz < 0 ? e2[1] > 1.5 * e2[0] : e2[0] > 1.5 * e2[1]) &&
                           e36[2] == 0.0)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;

            // an own SOFA file (the full FABIAN set)
            const juce::File sofa(
                juce::SystemStats::getEnvironmentVariable("RYSUM_TEST_SOFA", {})); // optional
            if (sofa.existsAsFile())
            {
                proc->setSofaFile(sofa);
                pump([&] { return proc->getBinauralStatus().contains("11950"); });
                std::cout << "own SOFA: " << proc->getBinauralStatus()
                          << (proc->getBinauralStatus().contains("11950") ? "  ok" : "  FAIL")
                          << std::endl;
                juce::MemoryBlock mb;
                proc->getStateInformation(mb);
                std::cout << "SOFA path in state: "
                          << (proc->getXmlFromBinary(mb.getData(), (int)mb.getSize())
                                      ->toString()
                                      .contains("HATO_0_48k.sofa")
                                  ? "ok"
                                  : "FAIL")
                          << std::endl;
                P("binOrder")->setValueNotifyingHost(P("binOrder")->convertTo0to1(4.0f));
                pump([&] { return proc->getBinauralStatus().contains("5. Ordnung"); });
                std::cout << "5th order: " << proc->getBinauralStatus()
                          << (proc->getBinauralStatus().contains("5. Ordnung") ? "  ok" : "  FAIL")
                          << std::endl;
                const auto e5 = play(2);
                std::cout << "5th order own SOFA: L/R " << 10 * std::log10(e5[0] / e5[1]) << " dB"
                          << std::endl;
                proc->setSofaFile({});
                P("binOrder")->setValueNotifyingHost(P("binOrder")->getDefaultValue());
                pump(
                    [&]
                    {
                        return proc->getBinauralStatus().startsWith("FABIAN") &&
                               proc->getBinauralStatus().contains("3. Ordnung");
                    });
            }
            for (auto *id : {"stop1", "stop2", "stop6"})
                P(id)->setValueNotifyingHost(1.0f);
            P("irWet")->setValueNotifyingHost(irWetBefore);
            P("output")->setValueNotifyingHost(outBefore);
        }

        // "Halten": two quick strikes keep the note down, a third strike lets it go; computer
        // keyboard notes
        {
            juce::AudioBuffer<float> st(2, 512);
            auto block = [&](std::initializer_list<juce::MidiMessage> msgs)
            {
                juce::MidiBuffer mb;
                int pos = 0;
                for (auto &m : msgs)
                    mb.addEvent(m, pos += 40);
                st.clear(), proc->processBlock(st, mb);
            };
            const int k62 = rysum::keyIndexForMidi(62);
            auto held = [&] { return (proc->engine.heldMask.load() >> k62) & 1u; };
            proc->latchOn = true;
            block({juce::MidiMessage::noteOn(1, 62, (juce::uint8)90),
                   juce::MidiMessage::noteOff(1, 62),
                   juce::MidiMessage::noteOn(1, 62, (juce::uint8)90)});
            for (int i = 0; i < 5; ++i)
                block({});
            block({juce::MidiMessage::noteOff(1, 62)});
            for (int i = 0; i < 50; ++i)
                block({});
            const bool latchedOk = held() != 0 && proc->latchedCount.load() == 1;
            block({juce::MidiMessage::noteOn(1, 62, (juce::uint8)90)});
            const bool stillDown = held() != 0;
            block({juce::MidiMessage::noteOff(1, 62)});
            const bool released = held() == 0 && proc->latchedCount.load() == 0;
            // single strike: normal
            block({juce::MidiMessage::noteOn(1, 64, (juce::uint8)90)});
            for (int i = 0; i < 40; ++i)
                block({});
            block({juce::MidiMessage::noteOff(1, 64)});
            const bool normal =
                ((proc->engine.heldMask.load() >> rysum::keyIndexForMidi(64)) & 1u) == 0;
            // switching it off lets everything go
            block({juce::MidiMessage::noteOn(1, 62, (juce::uint8)90),
                   juce::MidiMessage::noteOff(1, 62),
                   juce::MidiMessage::noteOn(1, 62, (juce::uint8)90),
                   juce::MidiMessage::noteOff(1, 62)});
            const bool again = held() != 0;
            proc->latchOn = false;
            block({});
            const bool offReleases = held() == 0;
            std::cout << "Halten: latched " << latchedOk << ", third strike keeps it until key up "
                      << stillDown << ", released " << released << ", single strike normal "
                      << normal << ", switching off releases " << (again && offReleases)
                      << ((latchedOk && stillDown && released && normal && again && offReleases)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            proc->keyboardState.noteOn(1, 60, 0.8f);
            block({});
            const bool kbOn =
                ((proc->engine.heldMask.load() >> rysum::keyIndexForMidi(60)) & 1u) != 0;
            proc->keyboardState.noteOff(1, 60, 0.0f);
            block({});
            std::cout << "on-screen / computer keyboard notes reach the engine: "
                      << (kbOn ? "ok" : "FAIL") << std::endl;
        }

        // patches
        {
            auto files = RysumProcessor::getPatchFiles();
            std::cout << "factory patches: " << files.size()
                      << (files.size() >= 6 ? "  ok" : "  FAIL") << std::endl;
            juce::File welle;
            for (auto &f : files)
                if (f.getFileName().contains("Welle"))
                    welle = f;
            const float outBefore = proc->apvts.getRawParameterValue("output")->load();
            const bool loaded = proc->loadPatch(welle);
            std::cout << "load factory patch: "
                      << ((loaded && proc->getRouteSrc(0) == rysum::srcLfo2 &&
                           proc->getRouteTgt(0) == rysum::tgtAz &&
                           proc->apvts.getRawParameterValue("stop0")->load() > 0.5f &&
                           juce::exactlyEqual(proc->apvts.getRawParameterValue("output")->load(),
                                              outBefore))
                              ? "ok"
                              : "FAIL")
                      << std::endl;
            auto *w = proc->apvts.getParameter("wind");
            w->setValueNotifyingHost(0.33f);
            proc->savePatch("Test Patch");
            w->setValueNotifyingHost(0.9f);
            proc->setRoute(0, -1, -1);
            proc->loadPatch(RysumProcessor::getPatchFolder().getChildFile("Test Patch.rysum"));
            std::cout << "save / load patch: "
                      << ((std::abs(w->getValue() - 0.33f) < 0.01f &&
                           proc->getRouteSrc(0) == rysum::srcLfo2 &&
                           proc->getPatchName() == "Test Patch")
                              ? "ok"
                              : "FAIL")
                      << std::endl;
            w->setValueNotifyingHost(0.0f);
        }

        // every factory patch: play a chord through it (with the test room loaded), report the
        // level
        {
            std::cout << "factory warnings: "
                      << (proc->factoryWarnings.isEmpty()
                              ? juce::String("none")
                              : proc->factoryWarnings.joinIntoString("; "))
                      << std::endl;
            proc->loadIr(irFile);
            for (int w = 0; w < 100 && !proc->getIrStatus().contains("AmbiX"); ++w)
                juce::Thread::sleep(20);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            bool allOk = true;
            // RYSUM_QUICK: skip playing every factory patch (for quick looks at the pages)
            const bool quick = std::getenv("RYSUM_QUICK") != nullptr;
            for (auto &f : quick ? juce::Array<juce::File>() : RysumProcessor::getPatchFiles())
            {
                proc->loadPatch(f);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
                float peak = 0.0f;
                double e = 0.0;
                bool finite = true;
                long cnt = 0;
                int maxGrains = 0;
                float maxErode = 0.0f;
                const bool feedInput = proc->apvts.getRawParameterValue("inpOn")->load() > 0.5f;
                rysum::XorShift noise(11);
                // the routes that are on when the patch is loaded must stay on (scenes used to
                // switch them off) (taken from the file: loading has already applied scene A)
                std::vector<int> liveRoutes;
                if (auto xml = juce::XmlDocument::parse(f))
                    for (int r = 0; r < rysum::kNumRoutes; ++r)
                        for (auto *e : xml->getChildWithTagNameIterator("PARAM"))
                            if (e->getStringAttribute("id") == proc->getModAmountId(r) &&
                                std::abs(e->getDoubleAttribute("value")) > 1.0e-4 &&
                                proc->getRouteSrc(r) >= 0)
                                liveRoutes.push_back(r);
                for (int b = 0; b < 600; ++b) // 6.4 s
                {
                    juce::MidiBuffer mb;
                    if (b == 5)
                        for (int m : {41, 53, 57, 60, 65, 69, 72})
                            mb.addEvent(
                                juce::MidiMessage::noteOn(1, m, (juce::uint8)(b % 2 ? 60 : 110)),
                                0);
                    // the hands: mod wheel up and down, pitch wheel swinging, while the chord
                    // sounds
                    if (b >= 60 && b < 420 && b % 6 == 0)
                    {
                        const double ph = (double)(b - 60) / 360.0;
                        mb.addEvent(
                            juce::MidiMessage::controllerEvent(
                                1, 1,
                                juce::jlimit(
                                    0, 127,
                                    (int)std::round(
                                        127.0 * std::sin(juce::MathConstants<double>::pi * ph)))),
                            0);
                        mb.addEvent(
                            juce::MidiMessage::pitchWheel(
                                1, juce::jlimit(
                                       0, 16383,
                                       8192 + (int)(8000.0 *
                                                    std::sin(juce::MathConstants<double>::twoPi *
                                                             2.0 * ph)))),
                            1);
                    }
                    if (b == 420)
                    {
                        mb.addEvent(juce::MidiMessage::controllerEvent(1, 1, 0), 0);
                        mb.addEvent(juce::MidiMessage::pitchWheel(1, 8192), 0);
                    }
                    if (b % 20 == 0)
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(
                            2); // the scene timer (moving through the scenes)
                    if (b == 420)
                        for (int m : {41, 53, 57, 60, 65, 69, 72})
                            mb.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                    buf.clear();
                    if (feedInput)
                        for (int c = 0; c < 2; ++c)
                            for (int i = 0; i < 512; ++i)
                                buf.setSample(
                                    c, i,
                                    0.1f * noise.bi()); // a patch for the input: something to hear
                    proc->processBlock(buf, mb);
                    maxGrains = std::max(maxGrains, proc->engine.cloudGrainsNow.load());
                    maxErode = std::max(maxErode, proc->engine.spaceErosionNow.load());
                    for (int c = 0; c < 4; ++c)
                        for (int i = 0; i < 512; ++i)
                        {
                            const float v = buf.getSample(c, i);
                            if (!std::isfinite(v))
                                finite = false;
                            peak = std::max(peak, std::abs(v));
                            if (c == 0)
                            {
                                e += (double)v * v;
                                ++cnt;
                            }
                        }
                }
                juce::MidiBuffer off;
                for (int m = 36; m < 82; ++m)
                    off.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                buf.clear(), proc->processBlock(buf, off);
                for (int b = 0; b < 300; ++b)
                {
                    juce::MidiBuffer none;
                    buf.clear(), proc->processBlock(buf, none);
                } // let tails / freeze die
                const float rmsDb = juce::Decibels::gainToDecibels(
                    (float)std::sqrt(e / static_cast<double>(juce::jmax(1L, cnt))));
                bool routesKept = true;
                for (int r : liveRoutes)
                    if (std::abs(
                            proc->apvts.getRawParameterValue(proc->getModAmountId(r))->load()) <
                        1.0e-4f)
                        routesKept = false;
                const bool ok = finite && peak > 1.0e-4f && routesKept;
                allOk = allOk && ok;
                if (feedInput && std::getenv("RYSUM_DEBUG_INPUT") != nullptr)
                {
                    const auto pp = proc->readParams();
                    std::cout << "    input: on " << pp.input.on << " level " << pp.input.level
                              << " dist " << pp.input.dist << " shown "
                              << proc->engine.inShowCount.load() << " peak "
                              << proc->engine.inPeak.load() << " out " << pp.outputMode
                              << std::endl;
                }
                if (auto *io = proc->apvts.getParameter("inpOn"))
                    io->setValueNotifyingHost(0.0f); // the next patch: without the input again
                std::cout << "  " << f.getFileNameWithoutExtension()
                          << (feedInput ? " [Eingang]" : "") << ": peak "
                          << juce::Decibels::gainToDecibels(peak) << " dB, rms W " << rmsDb << " dB"
                          << (maxGrains > 0 ? ", grains up to " + juce::String(maxGrains)
                                            : juce::String())
                          << (maxErode > 0.0f ? ", erosion up to " + juce::String(maxErode, 2)
                                              : juce::String())
                          << (routesKept ? "" : ", a route was switched off")
                          << (ok ? "" : "  FAIL") << std::endl;
            }
            std::cout << "all factory patches play: " << (allOk ? "ok" : "FAIL") << std::endl;

            // set 14 (playing with the hands): how much one note a' changes with velocity, mod
            // wheel and pitch wheel (W channel, first 1.5 s: level difference, and the share of the
            // highs (first difference) as brightness)
            for (auto &f : quick ? juce::Array<juce::File>() : RysumProcessor::getPatchFiles())
            {
                const int num = f.getFileNameWithoutExtension().getIntValue();
                if (num < 118 || num > 133)
                    continue;
                auto play = [&](int vel, int wheel, int bend, double &level, double &bright,
                                juce::AudioBuffer<float> *keep)
                {
                    proc->loadPatch(f);
                    juce::MidiBuffer pre;
                    pre.addEvent(juce::MidiMessage::controllerEvent(1, 1, wheel), 0);
                    pre.addEvent(juce::MidiMessage::pitchWheel(1, bend), 0);
                    buf.clear();
                    proc->processBlock(buf, pre);
                    for (int w = 0; w < 6; ++w)
                    {
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
                        juce::MidiBuffer none;
                        buf.clear();
                        proc->processBlock(buf, none);
                    }
                    double e = 0, d = 0;
                    float prev = 0.0f;
                    for (int b = 0; b < 141; ++b)
                    {
                        juce::MidiBuffer mb;
                        if (b == 0)
                            mb.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)vel), 0);
                        buf.clear();
                        proc->processBlock(buf, mb);
                        if (b % 20 == 0)
                            juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
                        for (int i = 0; i < 512; ++i)
                        {
                            const float v = buf.getSample(0, i);
                            e += (double)v * v;
                            d += (double)(v - prev) * (v - prev);
                            prev = v;
                        }
                        if (keep != nullptr)
                            for (int i = 0; i < 512 && b * 512 + i < keep->getNumSamples(); ++i)
                                keep->setSample(0, b * 512 + i, buf.getSample(0, i));
                    }
                    juce::MidiBuffer off;
                    for (int m = 36; m < 82; ++m)
                        off.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                    off.addEvent(juce::MidiMessage::controllerEvent(1, 1, 0), 1);
                    off.addEvent(juce::MidiMessage::pitchWheel(1, 8192), 1);
                    buf.clear();
                    proc->processBlock(buf, off);
                    for (int b = 0; b < 400; ++b)
                    {
                        juce::MidiBuffer none;
                        buf.clear();
                        proc->processBlock(buf, none);
                    }
                    level = 10.0 * std::log10(juce::jmax(1e-30, e));
                    bright = 10.0 * std::log10(juce::jmax(1e-30, d) / juce::jmax(1e-30, e));
                };
                juce::AudioBuffer<float> a(1, 141 * 512), b2(1, 141 * 512);
                auto diffDb =
                    [](const juce::AudioBuffer<float> &x, const juce::AudioBuffer<float> &y)
                {
                    double dd = 0, ee = 0;
                    for (int i = 0; i < x.getNumSamples(); ++i)
                    {
                        const double t = x.getSample(0, i) - y.getSample(0, i);
                        dd += t * t;
                        ee += (double)y.getSample(0, i) * y.getSample(0, i);
                    }
                    return 10.0 * std::log10(juce::jmax(1e-30, dd) / juce::jmax(1e-30, ee));
                };
                double l0, b0, l1, b1, lw, bw, lb, bb;
                play(25, 0, 8192, l0, b0, nullptr);
                play(120, 0, 8192, l1, b1, &a);
                play(120, 127, 8192, lw, bw, &b2);
                const double wheelDiff = diffDb(b2, a);
                play(120, 0, 0, lb, bb, &b2);
                const double bendDiff = diffDb(b2, a);
                std::cout << "  expression " << f.getFileNameWithoutExtension()
                          << ": velocity 25 -> 120: level " << juce::String(l1 - l0, 1)
                          << " dB, brightness " << juce::String(b1 - b0, 1)
                          << " dB; mod wheel 0 -> 127: level " << juce::String(lw - l1, 1)
                          << " dB, brightness " << juce::String(bw - b1, 1) << " dB, change "
                          << juce::String(wheelDiff, 1) << " dB; pitch wheel down: change "
                          << juce::String(bendDiff, 1) << " dB" << std::endl;
            }
            // back to a plain state for the rest of the test
            proc->setRoute(0, -1, -1);
        }

        // modulation: LFO 1 -> global wind (a parameter), LFO 2 -> pipe direction with spread,
        // envelope -> distance
        {
            using namespace rysum;
            auto setP = [&](const char *id, float v)
            {
                auto *a = proc->apvts.getParameter(id);
                a->setValueNotifyingHost(a->convertTo0to1(v));
            };
            const int windIdx = proc->getParamIndex("wind");
            proc->setRoute(0, srcLfo1, kGlobalTargetBase + windIdx);
            setP("mod1Amt", 0.4f);
            proc->setRoute(1, srcLfo2, tgtAz);
            setP("mod2Amt", 0.5f);
            proc->setRoute(2, srcEnv1, tgtDist);
            setP("mod3Amt", 0.3f);
            proc->setRoute(3, srcLfo3, kGlobalTargetBase + proc->getParamIndex("irWet"));
            setP("mod4Amt", -0.3f);
            setP("lfo1Rate", 2.0f);
            setP("lfo2Spread", 1.0f);
            setP("lfo2Rate", 0.7f);
            setP("lfo4Spread", 0.6f);
            setP("lfo3Shape", 4.0f);
            float lo = 2, hi = -1;
            for (int b = 0; b < 120; ++b)
            {
                juce::MidiBuffer mb;
                if (b == 1)
                    for (int m : {45, 52, 57, 64, 69, 72})
                        mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), 0);
                buf.clear(), proc->processBlock(buf, mb);
                const float n = proc->getModulatedNorm(windIdx);
                if (!std::isnan(n))
                {
                    lo = juce::jmin(lo, n);
                    hi = juce::jmax(hi, n);
                }
            }
            for (int b = 0; b < 200 && !(proc->getModulatedNorm(windIdx) > 0.3f); ++b)
            {
                juce::MidiBuffer none;
                buf.clear(), proc->processBlock(buf, none);
            }
            const float base = proc->apvts.getParameter("wind")->getValue();
            std::cout << "wind modulated: base " << base << " range " << lo << " .. " << hi
                      << ((hi - lo > 0.3f && lo <= base + 1e-4f && hi > base) ? "  ok" : "  FAIL")
                      << std::endl;
            {
                auto prm = proc->readParams();
                // a silent pipe (Praestant C, not played) and a sounding one (a)
                float dSilent = 0, dSound = 0;
                for (int k : {0, 20})
                {
                    const auto a = proc->engine.currentPosition(0, k, prm),
                               b = proc->engine.displayPosition(0, k, prm);
                    (k == 0 ? dSilent : dSound) =
                        std::abs(rysum::wrapAz(b.az - a.az)) + std::abs(b.dist - a.dist);
                }
                std::cout << "raum shows modulation: silent " << dSilent << ", sounding " << dSound
                          << ((dSilent > 0.5f && dSound > 0.5f) ? "  ok" : "  FAIL") << std::endl;
                proc->setRoute(19, rysum::srcLfo4, rysum::tgtEl);
                setP("mod20Amt", 0.2f);
                juce::MidiBuffer none;
                buf.clear(), proc->processBlock(buf, none);
                auto prm2 = proc->readParams();
                std::cout << "slot 20: "
                          << ((prm2.mod.routes[19].src == rysum::srcLfo4 &&
                               std::abs(prm2.mod.routes[19].amount - 0.2f) < 1e-4f)
                                  ? "ok"
                                  : "FAIL")
                          << std::endl;
            }
            std::cout << "not modulated -> NaN: "
                      << (std::isnan(proc->getModulatedNorm(proc->getParamIndex("air"))) ? "ok"
                                                                                         : "FAIL")
                      << std::endl;
        }

        // MIDI learn: a knob, a range, a button, and a learned CC wins over the fixed map
        {
            for (int i = 0; i < RysumProcessor::kNumMidiMaps; ++i)
                proc->clearMidiMap(i);
            auto P = [&](const char *id) { return proc->apvts.getParameter(id); };
            juce::AudioBuffer<float> st(2, 512);
            auto cc = [&](int ch, int num, int val)
            {
                juce::MidiBuffer mb;
                mb.addEvent(juce::MidiMessage::controllerEvent(ch, num, val), 10);
                st.clear(), proc->processBlock(st, mb);
                juce::MessageManager::getInstance()->runDispatchLoopUntil(40);
            };
            const int knob = proc->learnMidiFor(proc->getParamIndex("swind0"));
            const bool waiting = proc->getLearningSlot() == knob;
            cc(2, 21, 64);
            const auto m = proc->getMidiMap(knob);
            const bool learned = m.cc == 21 && m.channel == 2 && proc->getLearningSlot() < 0;
            const bool follows = std::abs(P("swind0")->getValue() - 64.0f / 127.0f) < 0.002f;
            auto r = m;
            r.lo = 0.2f;
            r.hi = 0.8f;
            proc->setMidiMap(knob, r);
            cc(2, 21, 127);
            const float top = P("swind0")->getValue();
            cc(2, 21, 0);
            const float bottom = P("swind0")->getValue();
            cc(3, 21, 127);
            const bool otherChannel = std::abs(P("swind0")->getValue() - bottom) < 1e-4f;
            std::cout << "MIDI learn knob: waiting " << waiting << ", learned CC 21 ch 2 "
                      << learned << ", follows " << follows << ", range " << bottom << ".." << top
                      << ", other channel ignored " << otherChannel
                      << ((waiting && learned && follows && std::abs(top - 0.8f) < 0.002f &&
                           std::abs(bottom - 0.2f) < 0.002f && otherChannel)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;

            P("tremOn")->setValueNotifyingHost(0.0f);
            const int button = proc->learnMidiFor(proc->getParamIndex("tremOn"));
            cc(1, 80, 127); // the learning press itself does not switch
            const bool learnNoFlip =
                P("tremOn")->getValue() < 0.5f && proc->getMidiMap(button).toggle;
            cc(1, 80, 0);
            cc(1, 80, 127);
            const bool on = P("tremOn")->getValue() > 0.5f;
            cc(1, 80, 0);
            const bool stays = P("tremOn")->getValue() > 0.5f;
            cc(1, 80, 127);
            const bool off = P("tremOn")->getValue() < 0.5f;
            cc(1, 80, 0);
            std::cout << "MIDI learn button: " << learnNoFlip << on << stays << off
                      << ((learnNoFlip && on && stays && off) ? "  ok" : "  FAIL") << std::endl;

            P("wind")->setValueNotifyingHost(0.0f);
            const int over = proc->learnMidiFor(proc->getParamIndex("click"));
            cc(1, 11, 100);
            cc(1, 11, 127);
            const bool clickMoved = std::abs(P("click")->getValue() - 1.0f) < 0.002f,
                       windStill = P("wind")->getValue() < 0.001f;
            std::cout << "learned CC 11 replaces the fixed map: "
                      << ((clickMoved && windStill) ? "ok" : "FAIL") << std::endl;
            proc->clearMidiMap(over);
            P("click")->setValueNotifyingHost(P("click")->getDefaultValue());
            P("tremOn")->setValueNotifyingHost(0.0f);

            juce::MemoryBlock mbm;
            proc->getStateInformation(mbm);
            const auto xml = proc->getXmlFromBinary(mbm.getData(), (int)mbm.getSize())->toString();
            for (int i = 0; i < 8; ++i)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(
                    100); // saved as the default for new instances
            const auto saved = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                   .getChildFile("Rysum/MIDI-Regler.xml")
                                   .loadFileAsString();
            std::cout << "MIDI assignments in the state " << xml.contains("swind0")
                      << ", saved for new instances " << saved.contains("tremOn")
                      << ((xml.contains("swind0") && saved.contains("tremOn")) ? "  ok" : "  FAIL")
                      << std::endl;
            {
                RysumProcessor fresh;
                const bool inherits =
                    fresh.getMidiMap(knob).cc == 21 &&
                    fresh.getMidiMap(button).param == fresh.getParamIndex("tremOn");
                std::cout << "a new instance starts with them: " << (inherits ? "ok" : "FAIL")
                          << std::endl;
            }
            proc->learnMidiFor(
                proc->getParamIndex("simTail")); // shows "bewegen ..." in the snapshot
        }

        // free tuning: a Scala file (kept in the state), MIDI Tuning Standard SysEx (as Hexatone
        // sends it)
        {
            auto P = [&](const char *id) { return proc->apvts.getParameter(id); };
            const auto scl = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                 .getChildFile("rysum_test_19edo.scl");
            juce::String txt("! 19-EDO\n19 equal\n 19\n");
            for (int i = 1; i <= 19; ++i)
                txt << juce::String(1200.0 * i / 19.0, 4) << "\n";
            scl.replaceWithText(txt);
            const bool loaded = proc->loadTuningFile(scl);
            const int t = juce::roundToInt(proc->apvts.getRawParameterValue("temperament")->load());
            const bool free = proc->readParams().notes != nullptr;
            juce::MemoryBlock mb;
            proc->getStateInformation(mb);
            const bool inState = proc->getXmlFromBinary(mb.getData(), (int)mb.getSize())
                                     ->toString()
                                     .contains("19 equal");
            std::cout << "scala file: loaded " << loaded << ", tuning " << t << ", free notes "
                      << free << ", in the state " << inState << " ("
                      << proc->getTuningStatus().upToFirstOccurrenceOf("\n", false, false) << ")"
                      << ((loaded && t == RysumProcessor::kScalaTuning && free && inState)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;

            // MTS: without a master, a real-time single-note tuning change SysEx retunes note 69 to
            // b-flat (466.16 Hz)
            proc->loadPatch(RysumProcessor::getPatchFolder().getChildFile(
                "01 Init.rysum")); // a clean sound (earlier tests leave all sorts)
            P("temperament")
                ->setValueNotifyingHost(
                    P("temperament")->convertTo0to1((float)RysumProcessor::kMtsTuning));
            juce::AudioBuffer<float> st(2, 512);
            const juce::uint8 sx[]{0xF0, 0x7F, 0x7F, 0x08, 0x02, 0x00,
                                   0x01, 69,   70,   0x00, 0x00, 0xF7};
            for (auto *id : {"stop1", "stop2", "stop6"})
                P(id)->setValueNotifyingHost(0.0f);
            for (auto *id : {"click", "releaseClick", "wind", "irWet"})
                P(id)->setValueNotifyingHost(0.0f);
            for (int r = 0; r < rysum::kNumRoutes; ++r)
                proc->clearRoute(r); // earlier tests modulate the wind (pitch gusts)
            P("output")->setValueNotifyingHost(0.0f);
            {
                juce::MidiBuffer m;
                m.addEvent(juce::MidiMessage(sx, (int)sizeof(sx)), 0);
                st.clear(), proc->processBlock(st, m);
            }
            const auto status = proc->getTuningStatus();
            std::vector<float> rec;
            for (int b = 0; b < 140; ++b)
            {
                juce::MidiBuffer m;
                if (b == 0)
                    m.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8)100), 0);
                if (b == 139)
                    m.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
                st.clear(), proc->processBlock(st, m);
                if (b >= 60)
                    for (int i = 0; i < 512; ++i)
                        rec.push_back(st.getSample(0, i));
            }
            const double sr = proc->getSampleRate() > 0 ? proc->getSampleRate() : 48000.0;
            auto ac = [&](int lag)
            {
                double s2 = 0;
                for (size_t i = 0; i + (size_t)lag < rec.size(); ++i)
                    s2 += (double)rec[i] * rec[i + (size_t)lag];
                return s2;
            };
            int best = 0;
            double bv = -1e18;
            for (int lag = (int)(sr / 520.0); lag <= (int)(sr / 420.0); ++lag)
            {
                const double v = ac(lag);
                if (v > bv)
                {
                    bv = v;
                    best = lag;
                }
            }
            const double y0 = ac(best - 1), y1 = ac(best), y2 = ac(best + 1);
            const double hz = sr / (best + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2));
            const double err = 1200.0 * std::log2(hz / 466.1638);
            std::cout << "MTS SysEx: " << status << "; note 69 sounds at " << hz << " Hz (" << err
                      << " ct from 466.16)"
                      << ((status.contains("SysEx") && std::abs(err) < 6.0) ? "  ok" : "  FAIL")
                      << std::endl;
            for (auto *id : {"stop1", "stop2", "stop6"})
                P(id)->setValueNotifyingHost(1.0f);
            P("temperament")->setValueNotifyingHost(P("temperament")->convertTo0to1(0.0f));
        }

        // drawn trajectory (Trompete): kept in the state, the engine follows it; ellipse settings
        // for the picture
        {
            auto setP = [&](const juce::String &id, float v)
            {
                auto *a = proc->apvts.getParameter(id);
                a->setValueNotifyingHost(a->convertTo0to1(v));
            };
            std::vector<RysumProcessor::DrawnPath::Pt> pts;
            for (int i = 0; i < 120; ++i)
            {
                const float t = (float)i / 119.0f;
                pts.push_back({160.0f * std::sin(6.283f * t), 30.0f + 25.0f * std::sin(12.566f * t),
                               2.0f * t * t}); // a figure eight, drawn faster at the end
            }
            proc->setDrawnPath(6, pts);
            setP(RysumProcessor::motionParamId(6), (float)rysum::Motion::Drawn);
            setP(RysumProcessor::rateParamId(6), 0.05f);
            setP(RysumProcessor::ellipseParamId(6), 0.5f);
            setP(RysumProcessor::ellDirParamId(6), 45.0f);
            setP(RysumProcessor::ellFormParamId(6), 1.0f);
            const auto prm = proc->readParams();
            const auto pos = proc->engine.currentPosition(6, 0, prm);
            juce::MemoryBlock mb;
            proc->getStateInformation(mb);
            proc->setDrawnPath(6, {});
            proc->setStateInformation(mb.getData(), (int)mb.getSize());
            const auto &back = proc->getDrawnPath(6);
            std::cout << "drawn path: " << back.points.size()
                      << " points after the state round trip, pipe 0 at az " << pos.az << " el "
                      << pos.el
                      << ((back.points.size() == 120 &&
                           std::abs(back.points[60].az - pts[60].az) < 0.01f &&
                           proc->engine.getPath(6).n > 100)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
        }

        editor.reset(dynamic_cast<RysumEditor *>(proc->createEditor()));
        if (std::getenv("RYSUM_D2D_BENCH") != nullptr)
        {
            // a real window at full-HD size (the interface 1.5x, as in full screen): how many whole
            // repaints per second each renderer manages per page, and what leaving out single
            // drawing effects gains
            editor->setSize(1920, 1080);
            editor->addToDesktop(juce::ComponentPeer::windowAppearsOnTaskbar);
            editor->setTopLeftPosition(
                juce::SystemStats::getEnvironmentVariable("RYSUM_BENCH_X", "0").getIntValue(), 0);
            editor->setVisible(true);
            auto *c = dynamic_cast<RysumContent *>(editor->getChildComponent(0));
            auto *peer = editor->getPeer();
            if (c == nullptr || peer == nullptr)
            {
                std::cout << "BENCH: no window" << std::endl;
                std::_Exit(1);
            }
            const auto engines = peer->getAvailableRenderingEngines();
            auto measure = [&](int view, int skip, const juce::String &label)
            {
                rysum::ui::dbg::skip = skip;
                c->showView(view);
                auto spin = [&](double ms)
                {
                    const double end = juce::Time::getMillisecondCounterHiRes() + ms;
                    while (juce::Time::getMillisecondCounterHiRes() < end)
                    {
                        editor->repaint();
                        c->tick();
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
                    }
                };
                spin(400);
                const int p0 = c->paintsDone;
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                spin(1500);
                const double fps = (c->paintsDone - p0) /
                                   ((juce::Time::getMillisecondCounterHiRes() - t0) * 0.001);
                std::cout << "BENCH " << engines[peer->getCurrentRenderingEngine()] << "  " << label
                          << ": " << fps << " repaints/s" << std::endl;
            };
            const char *names[]{"Spieltisch", "Raum", "Saal", "Klang",  "Effekte",
                                "Modulation", "",     "",     "Begehen"};
            {
                // as in the app: audio running in real time (a held chord), repaints only from the
                // interface's own timers
                std::atomic<bool> audioRun{true};
                std::thread audio(
                    [&]
                    {
                        juce::AudioBuffer<float> b(16, 512);
                        bool first = true;
                        double next = juce::Time::getMillisecondCounterHiRes();
                        while (audioRun.load())
                        {
                            juce::MidiBuffer mb;
                            if (first)
                            {
                                for (int n : {48, 55, 60, 64, 67})
                                    mb.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8)100),
                                                0);
                                first = false;
                            }
                            b.clear();
                            proc->processBlock(b, mb);
                            next += 512.0 / 48.0;
                            const double wait = next - juce::Time::getMillisecondCounterHiRes();
                            if (wait > 0)
                                std::this_thread::sleep_for(
                                    std::chrono::microseconds((int)(wait * 1000.0)));
                        }
                    });
                for (int e = 0; e < engines.size(); ++e)
                {
                    peer->setCurrentRenderingEngine(e);
                    for (int v : {0, 5, 8})
                    {
                        c->showView(v);
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(600);
                        const int p0 = c->paintsDone;
                        const double t0 = juce::Time::getMillisecondCounterHiRes();
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(2000);
                        const double fps =
                            (c->paintsDone - p0) /
                            ((juce::Time::getMillisecondCounterHiRes() - t0) * 0.001);
                        std::cout << "BENCH natural, audio running, " << engines[e] << "  "
                                  << names[v] << ": " << fps << " paints/s" << std::endl;
                    }
                }
                // the full-screen window (kiosk mode), as F11 opens it
                if (c->onToggleFullscreen)
                    c->onToggleFullscreen();
                juce::MessageManager::getInstance()->runDispatchLoopUntil(800);
                if (auto *fp = c->getPeer())
                {
                    const auto fe = fp->getAvailableRenderingEngines();
                    for (int e = 0; e < fe.size(); ++e)
                    {
                        fp->setCurrentRenderingEngine(e);
                        for (int v : {0, 5, 8})
                        {
                            c->showView(v);
                            juce::MessageManager::getInstance()->runDispatchLoopUntil(600);
                            const int p0 = c->paintsDone;
                            const double t0 = juce::Time::getMillisecondCounterHiRes();
                            juce::MessageManager::getInstance()->runDispatchLoopUntil(2000);
                            const double fps =
                                (c->paintsDone - p0) /
                                ((juce::Time::getMillisecondCounterHiRes() - t0) * 0.001);
                            std::cout << "BENCH full screen, audio running, " << fe[e] << "  "
                                      << names[v] << ": " << fps << " paints/s   (screen "
                                      << fp->getBounds().getWidth() << " x "
                                      << fp->getBounds().getHeight() << ", scale "
                                      << fp->getPlatformScaleFactor() << ")" << std::endl;
                        }
                    }
                    fp->setCurrentRenderingEngine(
                        fe.indexOf("Direct2D") >= 0 ? fe.indexOf("Direct2D") : 0);
                }
                if (c->onToggleFullscreen)
                    c->onToggleFullscreen();
                juce::MessageManager::getInstance()->runDispatchLoopUntil(800);
                audioRun = false;
                audio.join();
                for (int e = 0; e < engines.size(); ++e)
                {
                    peer->setCurrentRenderingEngine(e);
                    for (int v : {0, 5})
                    {
                        c->showView(v);
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(600);
                        const int p0 = c->paintsDone;
                        const double t0 = juce::Time::getMillisecondCounterHiRes();
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(2000);
                        const double fps =
                            (c->paintsDone - p0) /
                            ((juce::Time::getMillisecondCounterHiRes() - t0) * 0.001);
                        std::cout << "BENCH natural, no audio, " << engines[e] << "  " << names[v]
                                  << ": " << fps << " paints/s" << std::endl;
                    }
                }
            }
            for (int e = 0; e < engines.size(); ++e)
            {
                peer->setCurrentRenderingEngine(e);
                for (int v : {0, 1, 2, 3, 4, 5, 8})
                    measure(v, 0, names[v]);
            }
            peer->setCurrentRenderingEngine(
                engines.indexOf("Direct2D") >= 0 ? engines.indexOf("Direct2D") : 0);
            namespace D = rysum::ui::dbg;
            for (int v : {0, 5})
            {
                measure(v, D::skipOverlay, juce::String(names[v]) + " without the overlay");
                measure(v, D::skipCover, juce::String(names[v]) + " without photos (drawCover)");
                measure(v, D::skipGrain, juce::String(names[v]) + " without grain tiles");
                measure(v, D::skipClipPath, juce::String(names[v]) + " without round clips");
                measure(v, D::skipTranslucent, juce::String(names[v]) + " without the faint wall");
                measure(v, 31, juce::String(names[v]) + " without all of these");
            }
            std::cout.flush();
            std::_Exit(0);
        }
        editor->setSize(1280, 720);
        auto *content = editor->getChildComponent(0);
        auto shoot = [&](int view, const juce::String &name)
        {
            if (auto *c = dynamic_cast<RysumContent *>(content))
            {
                c->showView(view);
                c->tick();
            }
            juce::MessageManager::getInstance()->runDispatchLoopUntil(
                120); // slider timers (modulation, MIDI tags)
            auto img = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            auto f = outDir.getChildFile(name);
            f.deleteFile();
            juce::FileOutputStream os(f);
            png.writeImageToStream(img, os);
            std::cout << "wrote " << f.getFullPathName() << std::endl;
        };
        shoot(0, "ui_spieltisch.png");
        {
            // the spatial degradation tab and the grain cloud tab (found by their button text)
            std::function<juce::Button *(juce::Component *, const juce::String &)> findButton =
                [&](juce::Component *c, const juce::String &t) -> juce::Button *
            {
                if (!c->isVisible())
                    return nullptr;
                if (auto *b = dynamic_cast<juce::TextButton *>(c))
                    if (b->getButtonText() == t)
                        return b;
                for (auto *ch : c->getChildren())
                    if (auto *f = findButton(ch, t))
                        return f;
                return nullptr;
            };
            auto setN = [&](const juce::String &id, float v)
            {
                if (auto *prm = proc->apvts.getParameter(id))
                    prm->setValueNotifyingHost(prm->convertTo0to1(v));
            };
            setN("spcErode", 0.35f);
            setN("spcGrid", 0.4f);
            setN("spcDrops", 0.2f);
            if (auto *c = dynamic_cast<RysumContent *>(content))
            {
                c->showView(0);
                c->tick();
            }
            juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
            std::function<juce::Component *(juce::Component *, bool)> findView =
                [&](juce::Component *c, bool console) -> juce::Component *
            {
                if (console ? dynamic_cast<rysum::ui::ConsoleView *>(c) != nullptr
                            : dynamic_cast<rysum::ui::SpaceView *>(c) != nullptr)
                    return c;
                for (auto *ch : c->getChildren())
                    if (auto *f = findView(ch, console))
                        return f;
                return nullptr;
            };
            // the buttons of the Spieltisch must not cover one another ("pro Pfeife" / "Summe" used
            // to lie over the Glitch tab)
            if (auto *cv = findView(content, true))
            {
                bool clear = true;
                for (auto *a : cv->getChildren())
                    for (auto *b : cv->getChildren())
                        if (a != b && a->isVisible() && b->isVisible() &&
                            dynamic_cast<juce::TextButton *>(a) != nullptr &&
                            dynamic_cast<juce::TextButton *>(b) != nullptr &&
                            a->getBounds().intersects(b->getBounds()))
                            clear = false;
                std::cout << "Spieltisch buttons do not overlap: " << (clear ? "ok" : "FAIL")
                          << std::endl;
            }
            if (auto *b =
                    findView(content, true) ? findButton(findView(content, true), "Raum") : nullptr)
                b->triggerClick();
            else
                std::cout << "Raum tab button not found  FAIL" << std::endl;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
            shoot(0, "ui_spieltisch_raumverfall.png");
            if (auto *b = findView(content, true) ? findButton(findView(content, true), "Glitch")
                                                  : nullptr)
                b->triggerClick();
            else
                std::cout << "Glitch tab button not found  FAIL" << std::endl;
            if (auto *b = findView(content, true) ? findButton(findView(content, true), "Resonanz")
                                                  : nullptr)
                b->triggerClick();
            else
                std::cout << "Resonanz tab button not found  FAIL" << std::endl;
            setN("degStutter", 0.4f);
            setN("clickRes", 0.6f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
            shoot(0, "ui_spieltisch_glitch_resonanz.png");
            setN("degStutter", 0.0f);
            setN("clickRes", 0.0f);
            if (auto *b = findView(content, true)
                              ? findButton(findView(content, true),
                                           juce::String::fromUTF8("Ger\xc3\xa4usche"))
                              : nullptr)
                b->triggerClick();
            if (auto *b = findView(content, true) ? findButton(findView(content, true), "Klang")
                                                  : nullptr)
                b->triggerClick();
            for (auto *id : {"spcErode", "spcGrid", "spcDrops"})
                if (auto *prm = proc->apvts.getParameter(id))
                    prm->setValueNotifyingHost(prm->getDefaultValue());
            setN(RysumProcessor::cloudParamId("Mix", 6), 0.6f);
            if (auto *c = dynamic_cast<RysumContent *>(content))
            {
                c->showView(1);
                c->tick();
            }
            juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
            if (auto *b = findView(content, false) ? findButton(findView(content, false), "Wolke")
                                                   : nullptr)
                b->triggerClick();
            else
                std::cout << "Wolke tab button not found  FAIL" << std::endl;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
            auto img = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
            {
                juce::PNGImageFormat png;
                auto f = outDir.getChildFile("ui_raum_wolke.png");
                f.deleteFile();
                juce::FileOutputStream os(f);
                png.writeImageToStream(img, os);
            }
            setN(RysumProcessor::pipeSndParamId("Att", 6), 0.5f);
            setN(RysumProcessor::pipeSndParamId("HiGain", 6), 6.0f);
            if (auto *b = findView(content, false) ? findButton(findView(content, false), "Pfeife")
                                                   : nullptr)
                b->triggerClick();
            else
                std::cout << "Pfeife tab button not found  FAIL" << std::endl;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
            {
                auto img2 = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
                juce::PNGImageFormat png;
                auto f = outDir.getChildFile("ui_raum_pfeife.png");
                f.deleteFile();
                juce::FileOutputStream os(f);
                png.writeImageToStream(img2, os);
            }
            setN(RysumProcessor::pipeSndParamId("Att", 6), 0.0f);
            setN(RysumProcessor::pipeSndParamId("HiGain", 6), 0.0f);
            if (auto *b = findView(content, false)
                              ? findButton(findView(content, false), "Bewegung")
                              : nullptr)
                b->triggerClick();
            setN(RysumProcessor::cloudParamId("Mix", 6), 0.0f);
        }
        for (int k = 0; k < rysum::kNumKeys; k += 3)
            proc->setPipeMuted(6, k, true); // some Trompete pipes muted
        shoot(1, "ui_raum.png");
        {
            // the arrangement "Tonnetz" and the motion "Schwebung" (their buttons, found by text)
            std::function<juce::Button *(juce::Component *, const juce::String &)> fb =
                [&](juce::Component *c, const juce::String &t) -> juce::Button *
            {
                if (!c->isVisible())
                    return nullptr;
                if (auto *b = dynamic_cast<juce::TextButton *>(c))
                    if (b->getButtonText() == t)
                        return b;
                for (auto *ch : c->getChildren())
                    if (auto *f = fb(ch, t))
                        return f;
                return nullptr;
            };
            const int before = proc->layouts[0].type;
            bool found = true;
            if (auto *b = fb(content, "Tonnetz"))
                b->triggerClick();
            else
                found = false;
            if (auto *b = fb(content, "Schwebung"))
                b->triggerClick();
            else
                found = false;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
            shoot(1, "ui_raum_tonnetz.png");
            std::cout << "Raum: Tonnetz and Schwebung buttons "
                      << (found ? "ok" : "not found  FAIL") << std::endl;
            (void)before;
        }
        for (int k = 0; k < rysum::kNumKeys; ++k)
            proc->setPipeMuted(6, k, false);
        {
            // the audio input: on, stereo, kreisend; some noise through the processor so the meter
            // and the places show
            auto setN = [&](const char *id, float v)
            {
                if (auto *prm = proc->apvts.getParameter(id))
                    prm->setValueNotifyingHost(prm->convertTo0to1(v));
            };
            setN("inpOn", 1.0f);
            setN("inpSpin", 20.0f);
            setN("inpWidth", 90.0f);
            setN("inpAz", 30.0f);
            setN("inpDist", 3.0f);
            juce::AudioBuffer<float> io(36, 512);
            rysum::XorShift r(5);
            float meanAbs = 0.0f;
            for (int b = 0; b < 40; ++b)
            {
                io.clear();
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < 512; ++i)
                        io.setSample(c, i, 0.2f * r.bi());
                juce::MidiBuffer mb;
                proc->processBlock(io, mb);
                if (b > 30)
                    meanAbs += io.getMagnitude(0, 0, 512) + io.getMagnitude(1, 0, 512);
            }
            std::cout << "input through the processor: out peak " << meanAbs / 18.0f
                      << ", shown sources " << proc->engine.inShowCount.load()
                      << ((proc->engine.inShowCount.load() == 2 && meanAbs > 0.01f) ? "  ok"
                                                                                    : "  FAIL")
                      << std::endl;
            rysum::ui::SpaceView *sv = nullptr;
            std::function<void(juce::Component *)> find = [&](juce::Component *c)
            {
                if (auto *v = dynamic_cast<rysum::ui::SpaceView *>(c))
                    sv = v;
                for (auto *ch : c->getChildren())
                    if (sv == nullptr)
                        find(ch);
            };
            find(editor.get());
            if (sv != nullptr)
                sv->selectInput();
            shoot(1, "ui_eingang.png");
            if (sv != nullptr)
                sv->selectStop(6);
            setN("inpOn", 0.0f);
            setN("inpSpin", 0.0f);
            setN("inpWidth", 60.0f);
            setN("inpAz", 0.0f);
            setN("inpDist", 4.0f);
        }
        shoot(3, "ui_klang.png");
        {
            // two loudspeakers (transaural): the output switched, the HRTF decoder and then the
            // crosstalk filters designed
            if (auto *o = proc->apvts.getParameter("output"))
                o->setValueNotifyingHost(
                    o->convertTo0to1((float)RysumProcessor::kTransauralOutput));
            for (int t = 0; t < 300 && proc->getTransauralFilters() == nullptr; ++t)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
            shoot(3, "ui_klang_transaural.png");
            const auto f = proc->getTransauralFilters();
            std::cout << "transaural in the plugin: "
                      << (f != nullptr ? f->description : juce::String("not designed  FAIL"))
                      << std::endl;
            if (auto *o = proc->apvts.getParameter("output"))
                o->setValueNotifyingHost(o->getDefaultValue());
            juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        }
        {
            // loudspeakers: 7.1.4 at 3rd order through the processor - sound on the layout's
            // channels only, the LFE channel silent
            auto l714 = rysum::speakerPresets()[3];
            proc->setSpeakerLayout(l714, false);
            auto *o = proc->apvts.getParameter("output");
            const float outBefore = o->getValue();
            o->setValueNotifyingHost(o->convertTo0to1((float)RysumProcessor::kSpeakerOutput));
            juce::AudioBuffer<float> io(36, 512);
            double e[36]{};
            for (int b = 0; b < 60; ++b)
            {
                io.clear();
                juce::MidiBuffer mb;
                if (b == 2)
                    for (int m : {48, 55, 60, 64})
                        mb.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)100), 0);
                proc->processBlock(io, mb);
                if (b > 20)
                    for (int c = 0; c < 36; ++c)
                        e[c] += io.getRMSLevel(c, 0, 512);
            }
            int sounding = 0;
            for (int c = 0; c < 12; ++c)
                sounding += e[c] > 1.0e-4 ? 1 : 0;
            double rest = 0;
            for (int c = 12; c < 36; ++c)
                rest += e[c];
            proc->speakerTest = 0;
            io.clear();
            {
                juce::MidiBuffer none;
                proc->processBlock(io, none);
            }
            proc->speakerTest = -1;
            std::cout << "loudspeakers 7.1.4: " << sounding << " of 12 channels sound, LFE " << e[3]
                      << ", above 12: " << rest
                      << ((sounding == 11 && e[3] == 0.0 && rest == 0.0) ? "  ok" : "  FAIL")
                      << std::endl;
            juce::MemoryBlock mb;
            proc->getStateInformation(mb);
            proc->setSpeakerLayout(rysum::speakerPresets()[0], false);
            proc->setStateInformation(mb.getData(), (int)mb.getSize());
            std::cout << "loudspeaker layout in the state: "
                      << (proc->getSpeakerLayout().speakers.size() == 11 ? "ok" : "FAIL")
                      << std::endl;
            shoot(7, "ui_lautsprecher.png");
            for (int b = 0; b < 20; ++b)
            {
                io.clear();
                juce::MidiBuffer noteOffs;
                if (b == 0)
                    for (int m : {48, 55, 60, 64})
                        noteOffs.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                proc->processBlock(io, noteOffs);
            }
            o->setValueNotifyingHost(outBefore);
            proc->setSpeakerLayout(rysum::speakerPresets()[4], false);
        }
        shoot(5, "ui_mod.png");
        {
            proc->apvts.getParameter("windModel")->setValueNotifyingHost(1.0f);
            shoot(3, "ui_klang_wind2.png");
            proc->apvts.getParameter("windModel")->setValueNotifyingHost(0.0f);
            if (auto *c = dynamic_cast<RysumContent *>(content))
                c->openMidiMap();
            shoot(5, "ui_midi.png");
            for (int i = 0; i < RysumProcessor::kNumMidiMaps; ++i)
                proc->clearMidiMap(i);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(400);
        }
        {
            // right-click menus are top-level windows without a parent: they must get the plugin's
            // look by default
            auto &lf = juce::LookAndFeel::getDefaultLookAndFeel();
            const bool ours = &lf == &editor->getLookAndFeel();
            std::cout << "default look = plugin look (menus, dialogs): " << (ours ? "ok" : "FAIL")
                      << std::endl;
            // paint a menu the way JUCE does, to look at it
            juce::Image img(juce::Image::ARGB, 260, 150, true);
            {
                juce::Graphics g(img);
                lf.drawPopupMenuBackground(g, 260, 150);
                const char *items[]{"Verstimmung", "Modulieren mit", "Entfernen: LFO 1 (+25 %)",
                                    "Modulationsseite \xc3\xb6"
                                    "ffnen"};
                for (int i = 0; i < 4; ++i)
                    lf.drawPopupMenuItem(g, {4, 6 + i * 34, 252, 30}, false, true, i == 2, false,
                                         i == 1, juce::String::fromUTF8(items[i]), {}, nullptr,
                                         nullptr);
            }
            juce::PNGImageFormat png;
            auto f = outDir.getChildFile("ui_menu.png");
            f.deleteFile();
            juce::FileOutputStream os(f);
            png.writeImageToStream(img, os);
            std::cout << "wrote " << f.getFullPathName() << std::endl;
        }
        {
            // effects page with some effects switched on (so the diagrams show something)
            for (auto [id, v] : {std::pair<const char *, float>{"fxEchoLevel", 0.5f},
                                 {"fxPartSpread", 70.0f},
                                 {"fxEchoTilt", 10.0f}})
                if (auto *prm = proc->apvts.getParameter(id))
                    prm->setValueNotifyingHost(prm->convertTo0to1(v));
            {
                // own sounds from a chosen folder (not <sample-library>/noise/eigene): read, kept
                // in the state, back to standard
                auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("rysum_own_sounds_test");
                dir.deleteRecursively();
                dir.getChildFile("sub").createDirectory();
                juce::AudioBuffer<float> snd(1, 9600);
                for (int i = 0; i < 9600; ++i)
                    snd.setSample(0, i,
                                  0.3f * std::exp(-static_cast<float>(i) / 900.0f) *
                                      std::sin(static_cast<float>(i) * 0.7f));
                for (auto name : {"a.wav", "sub/b.wav"})
                {
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::OutputStream> stream(
                        dir.getChildFile(name).createOutputStream());
                    const auto options = juce::AudioFormatWriterOptions{}
                                             .withSampleRate(48000.0)
                                             .withNumChannels(1)
                                             .withBitsPerSample(24);
                    auto writer = wav.createWriterFor(stream, options);
                    if (writer == nullptr || !writer->writeFromAudioSampleBuffer(snd, 0, 9600))
                    {
                        std::cerr << "Failed to write custom sound fixture: " << name << std::endl;
                        setApplicationReturnValue(1);
                        quit();
                        return;
                    }
                }
                auto waitFor = [&](int n)
                {
                    for (int k = 0; k < 150 && proc->getNumCustomSounds() != n; ++k)
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
                };
                const int before = proc->getNumCustomSounds();
                proc->setCustomFolder(dir);
                waitFor(2);
                const int got = proc->getNumCustomSounds();
                juce::MemoryBlock mb;
                proc->getStateInformation(mb);
                proc->setCustomFolder({});
                waitFor(before);
                proc->setStateInformation(mb.getData(), (int)mb.getSize());
                waitFor(2);
                const bool restored =
                    proc->getCustomFolder() == dir && proc->getNumCustomSounds() == 2;
                shoot(4, "ui_effekte_ordner.png");
                proc->setCustomFolder({});
                waitFor(before);
                std::cout << "own sounds from a chosen folder: read " << got << ", from the state "
                          << restored << ", back to standard "
                          << (!proc->hasOwnCustomFolder() && proc->getNumCustomSounds() == before)
                          << ((got == 2 && restored && !proc->hasOwnCustomFolder()) ? "  ok"
                                                                                    : "  FAIL")
                          << std::endl;
                dir.deleteRecursively();
            }
            shoot(4, "ui_effekte.png");
            for (auto *id : {"fxEchoLevel", "fxPartSpread", "fxEchoTilt"})
                if (auto *prm = proc->apvts.getParameter(id))
                    prm->setValueNotifyingHost(prm->getDefaultValue());
            {
                // second tab: Stimmung & Belichtung
                std::function<juce::Button *(juce::Component *, const juce::String &)> fb =
                    [&](juce::Component *c, const juce::String &t) -> juce::Button *
                {
                    if (!c->isVisible())
                        return nullptr;
                    if (auto *b = dynamic_cast<juce::TextButton *>(c))
                        if (b->getButtonText() == t)
                            return b;
                    for (auto *ch : c->getChildren())
                        if (auto *f = fb(ch, t))
                            return f;
                    return nullptr;
                };
                auto setV = [&](const char *id, float v)
                {
                    if (auto *prm = proc->apvts.getParameter(id))
                        prm->setValueNotifyingHost(prm->convertTo0to1(v));
                };
                setV("fxGhostLevel", -6.0f);
                setV("fxDexMix", 0.5f);
                setV("fxExpoMix", 0.4f);
                if (auto *c = dynamic_cast<RysumContent *>(content))
                {
                    c->showView(4);
                    c->tick();
                }
                juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
                if (auto *b = fb(content, juce::String::fromUTF8("Stimmung & Belichtung")))
                    b->triggerClick();
                else
                    std::cout << "Stimmung & Belichtung tab button not found  FAIL" << std::endl;
                juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
                shoot(4, "ui_effekte_stimmung_belichtung.png");
                {
                    // third tab: the living organ - a Kalkant, a walking listener, some worn pipes
                    setV("lvBellows", 0.8f);
                    setV("lvBlower", 1.0f);
                    setV("lvWalkShape", 4.0f);
                    setV("lvWalkSize", 5.0f);
                    setV("lvWear", 1.0f);
                    for (int k = 0; k < rysum::kNumKeys; ++k)
                        proc->engine.wear[0][(size_t)k] = (float)((k * 7) % 11) / 11.0f;
                    {
                        // the wear belongs to the project, not to a patch
                        juce::MemoryBlock ws;
                        proc->getStateInformation(ws);
                        proc->engine.wear[0][5] = 0.0f;
                        proc->setStateInformation(ws.getData(), (int)ws.getSize());
                        std::cout << "wear in the project state: "
                                  << (std::abs(proc->engine.wear[0][5].load() - 2.0f / 11.0f) <
                                              0.001f
                                          ? "ok"
                                          : "FAIL")
                                  << std::endl;
                    }
                    juce::AudioBuffer<float> lb(36, 512);
                    for (int b2 = 0; b2 < 300; ++b2)
                    {
                        juce::MidiBuffer mb2;
                        if (b2 == 0)
                            for (int m : {48, 55, 60, 64, 67})
                                mb2.addEvent(juce::MidiMessage::noteOn(1, m, (juce::uint8)90), 0);
                        lb.clear(), proc->processBlock(lb, mb2);
                    }
                    if (auto *b = fb(content, "Lebendige Orgel"))
                        b->triggerClick();
                    else
                        std::cout << "Lebendige Orgel tab button not found  FAIL" << std::endl;
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
                    shoot(4, "ui_effekte_lebendige_orgel.png");
                    std::cout << "living organ in the plugin: bellows pressure "
                              << proc->engine.bellowsNow.load() << ", listener at "
                              << proc->engine.listenerXNow.load() << " / "
                              << proc->engine.listenerYNow.load() << " m"
                              << (proc->engine.bellowsNow.load() < 0.999f ? "  ok" : "  FAIL")
                              << std::endl;
                    juce::MidiBuffer offs;
                    for (int m : {48, 55, 60, 64, 67})
                        offs.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                    lb.clear(), proc->processBlock(lb, offs);
                    for (auto *id :
                         {"lvBellows", "lvBlower", "lvWalkShape", "lvWalkSize", "lvWear"})
                        if (auto *prm = proc->apvts.getParameter(id))
                            prm->setValueNotifyingHost(prm->getDefaultValue());
                    proc->resetWear();
                    for (int b2 = 0; b2 < 60; ++b2)
                    {
                        juce::MidiBuffer none;
                        lb.clear(), proc->processBlock(lb, none);
                    }
                }
                if (auto *b = fb(content, juce::String::fromUTF8("Kl\xc3\xa4nge")))
                    b->triggerClick();
                for (auto *id : {"fxGhostLevel", "fxDexMix", "fxExpoMix"})
                    if (auto *prm = proc->apvts.getParameter(id))
                        prm->setValueNotifyingHost(prm->getDefaultValue());
            }
        }
        {
            // the simulated room: on, a note played, ray tracing done in the background
            auto *P = proc->apvts.getParameter("simOn");
            P->setValueNotifyingHost(1.0f);
            juce::AudioBuffer<float> st(16, 512);
            for (int b = 0; b < 60; ++b)
            {
                juce::MidiBuffer mb;
                if (b == 2)
                    mb.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8)100), 0);
                st.clear(), proc->processBlock(st, mb);
            }
            for (int i = 0; i < 40 && !proc->getRoomStatus().contains("Strahlen"); ++i)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
            for (int b = 0; b < 20; ++b)
            {
                juce::MidiBuffer mb;
                st.clear(), proc->processBlock(st, mb);
            } // the engine takes the response
            std::cout << "Saal: " << proc->getRoomStatus()
                      << (proc->getRoomStatus().contains("Strahlen") ? "  ok" : "  FAIL")
                      << std::endl;
            std::cout << "Saal: engine RT " << proc->engine.roomRtLow.load() << " s, tail in use "
                      << proc->engine.tailSerialInUse.load() << ", " << proc->engine.erActive.load()
                      << " sounds with reflections"
                      << (proc->engine.erActive.load() > 0 &&
                                  proc->engine.tailSerialInUse.load() != 0xffffffffu
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            // Lucier mode on: the response of 12 passes is built in the background, the engine
            // takes it
            auto *L = proc->apvts.getParameter("lucOn");
            if (auto *md = proc->apvts.getParameter("lucMode"))
                md->setValueNotifyingHost(1.0f); // live
            L->setValueNotifyingHost(1.0f);
            if (auto *g = proc->apvts.getParameter("lucGen"))
                g->setValueNotifyingHost(g->convertTo0to1(12.0f));
            for (int i = 0; i < 60 && proc->getLucierView().curve.empty(); ++i)
                juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
            for (int b = 0; b < 200; ++b)
            {
                juce::MidiBuffer mb;
                st.clear(), proc->processBlock(st, mb);
            }
            const auto &lv = proc->getLucierView();
            std::cout << "Lucier: " << lv.what << ", gen " << lv.generation << ", "
                      << lv.peaks.size() << " resonances, first "
                      << (lv.peaks.empty() ? 0.0f : lv.peaks[0]) << " Hz, engine level "
                      << proc->engine.lucierLevel.load()
                      << (lv.curve.size() > 10 &&
                                  proc->engine.lucierSerialInUse.load() != 0xffffffffu
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            shoot(2, "ui_saal.png");
            {
                // the piece: record 3 s of the organ, then it plays: the recording, generation 1, 2
                // - by itself, then it ends
                auto setReal = [&](const char *id, float v)
                {
                    if (auto *q = proc->apvts.getParameter(id))
                        q->setValueNotifyingHost(q->convertTo0to1(v));
                };
                setReal("lucGen", 2.0f);
                setReal("lucGap", 0.5f);
                setReal("lucMix", 0.5f);
                setReal("lucRecSrc", 0.0f);
                proc->lucierRecord();
                auto &pc = proc->getLucierPiece();
                const bool modeOk = proc->isLucierPieceMode();
                for (int b = 0; b < 280; ++b) // 3 s
                {
                    juce::MidiBuffer mb;
                    if (b == 10)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
                    if (b == 200)
                        mb.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                    st.clear(), proc->processBlock(st, mb);
                }
                for (int i = 0; i < 20 && proc->getLucierView().curve.empty(); ++i)
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
                proc->lucierStopRecording();
                const double recSec = (double)pc.getRecording().size() / pc.recordingRate();
                const bool saved = juce::File(pc.recordingFile).existsAsFile();
                // play it through: blocks of audio, the message thread computing the generations in
                // between
                int maxGen = 0;
                float peak1 = 0.0f;
                bool ended = false;
                for (int b = 0; b < 4000 && !ended; ++b)
                {
                    juce::MidiBuffer mb;
                    st.clear(), proc->processBlock(st, mb);
                    if (b % 8 == 0)
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
                    maxGen = std::max(maxGen, pc.playingGen.load());
                    if (pc.playingGen.load() >= 1)
                        peak1 = std::max(peak1, st.getMagnitude(0, 0, 512));
                    ended = pc.state.load() == rysum::LucierPiece::stFinished;
                }
                std::cout << "Lucier piece: mode " << modeOk << ", recorded " << recSec
                          << " s, saved " << saved << ", reached generation " << maxGen
                          << ", level from gen 1 " << peak1 << ", ended " << ended
                          << ((modeOk && recSec > 1.0 && saved && maxGen == 2 && peak1 > 0.01f &&
                               ended)
                                  ? "  ok"
                                  : "  FAIL")
                          << std::endl;
                proc->lucierPlay();
                for (int b = 0; b < 300; ++b)
                {
                    juce::MidiBuffer mb;
                    st.clear(), proc->processBlock(st, mb);
                    if (b % 8 == 0)
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
                }
                shoot(2, "ui_saal_stueck.png");
                proc->lucierStop();
                juce::File(pc.recordingFile).deleteFile();
                if (auto *md = proc->apvts.getParameter("lucMode"))
                    md->setValueNotifyingHost(1.0f);
            }
            {
                // several rooms: the Gedackt in Saal 2 (a chapel) - its own late reverb is built
                // and used
                auto setReal = [&](const juce::String &id, float v)
                {
                    if (auto *q = proc->apvts.getParameter(id))
                        q->setValueNotifyingHost(q->convertTo0to1(v));
                };
                setReal(RysumProcessor::stopRoomParamId(1), 1.0f);
                setReal("stop1", 1.0f);
                for (int b = 0; b < 40; ++b)
                {
                    juce::MidiBuffer mb;
                    if (b == 1)
                        mb.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
                    st.clear(), proc->processBlock(st, mb);
                }
                for (int i = 0; i < 80 && proc->engine.tailSerialInUseR[1].load() == 0xffffffffu;
                     ++i)
                {
                    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
                    juce::MidiBuffer mb;
                    st.clear(), proc->processBlock(st, mb);
                }
                const int used = proc->engine.roomsUsed.load();
                std::cout << "rooms: used mask " << used << ", room 2 RT "
                          << proc->engine.heard[1].rtLow.load() << " s, tail in use "
                          << proc->engine.tailSerialInUseR[1].load()
                          << (((used & 2) != 0 &&
                               proc->engine.tailSerialInUseR[1].load() != 0xffffffffu &&
                               proc->engine.heard[1].rtLow.load() > 1.0f)
                                  ? "  ok"
                                  : "  FAIL")
                          << std::endl;
                shoot(2, "ui_saal_raeume.png");
                {
                    juce::MidiBuffer mb;
                    mb.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                    st.clear(), proc->processBlock(st, mb);
                }
                setReal(RysumProcessor::stopRoomParamId(1), 0.0f);
            }
            {
                // floor plans: L-shape, cross, drawn - the late reverb is rebuilt for each, the
                // page shows the plan
                L->setValueNotifyingHost(0.0f);
                auto setReal = [&](const juce::String &id, float v)
                {
                    if (auto *q = proc->apvts.getParameter(id))
                        q->setValueNotifyingHost(q->convertTo0to1(v));
                };
                auto settle = [&]
                {
                    for (int b = 0; b < 30; ++b)
                    {
                        juce::MidiBuffer mb;
                        st.clear(), proc->processBlock(st, mb);
                    }
                    for (int i = 0; i < 40; ++i)
                    {
                        juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
                        juce::MidiBuffer mb;
                        st.clear(), proc->processBlock(st, mb);
                    }
                };
                const float rtBox = proc->engine.roomRtLow.load();
                const juce::String names[]{"l", "kreuz", "rund", "gezeichnet"};
                const int shapes[]{rysum::shapeL, rysum::shapeCross, rysum::shapeRound,
                                   rysum::shapeDrawn};
                for (int i = 0; i < 4; ++i)
                {
                    setReal("simShape", (float)shapes[i]);
                    setReal("simShapeAmt", 0.7f);
                    {
                        juce::MidiBuffer mb;
                        mb.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8)100), 0);
                        st.clear(), proc->processBlock(st, mb);
                    }
                    settle();
                    std::cout << "Saal plan " << names[i] << ": RT "
                              << proc->engine.roomRtLow.load() << " s (shoebox " << rtBox << " s), "
                              << proc->engine.erActive.load() << " sounds with reflections, "
                              << proc->getRoomStatus() << std::endl;
                    shoot(2, "ui_saal_plan_" + names[i] + ".png");
                }
                // the drawn plan goes through the project state
                rysum::FloorPlan d;
                d.n = 5;
                const float xs[]{0.0f, 1.0f, 0.8f, 0.3f, 0.1f}, ys[]{0.1f, 0.0f, 1.0f, 0.85f, 0.6f};
                for (int k = 0; k < 5; ++k)
                {
                    d.x[(size_t)k] = xs[k];
                    d.y[(size_t)k] = ys[k];
                }
                proc->setDrawnPlan(2, d);
                juce::MemoryBlock mb2;
                proc->getStateInformation(mb2);
                proc->setDrawnPlan(2, rysum::defaultDrawnPlan());
                proc->setStateInformation(mb2.getData(), (int)mb2.getSize());
                const auto &back = proc->getDrawnPlan(2);
                std::cout << "drawn floor plan: " << back.n << " corners after the state round trip"
                          << ((back.n == 5 && std::abs(back.x[2] - 0.8f) < 1e-3f &&
                               proc->engine.getDrawnPlan(2).n == 5)
                                  ? "  ok"
                                  : "  FAIL")
                          << std::endl;
                setReal("simShape", 0.0f);
                setReal("simShapeAmt", 0.5f);
                L->setValueNotifyingHost(1.0f);
            }
            L->setValueNotifyingHost(0.0f);
            {
                juce::MidiBuffer mb;
                mb.addEvent(juce::MidiMessage::noteOff(1, 64), 0);
                st.clear(), proc->processBlock(st, mb);
            }
            P->setValueNotifyingHost(0.0f);
        }

        {
            // "Begehen": the Saal in 3D. A chord sounds; walking moves the listener in the engine;
            // a model loads and goes through the project state
            auto setReal = [&](const char *id, float v)
            {
                if (auto *q = proc->apvts.getParameter(id))
                    q->setValueNotifyingHost(q->convertTo0to1(v));
            };
            setReal("simOn", 1.0f);
            juce::AudioBuffer<float> st(16, 512);
            auto run = [&](int blocks, std::initializer_list<int> on)
            {
                for (int b = 0; b < blocks; ++b)
                {
                    juce::MidiBuffer mb;
                    if (b == 0)
                        for (int n : on)
                            mb.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8)100), 0);
                    st.clear(), proc->processBlock(st, mb);
                }
            };
            std::function<rysum::ui::WalkView *(juce::Component *)> findWalk =
                [&](juce::Component *c) -> rysum::ui::WalkView *
            {
                if (auto *w = dynamic_cast<rysum::ui::WalkView *>(c))
                    return w;
                for (auto *ch : c->getChildren())
                    if (auto *f = findWalk(ch))
                        return f;
                return nullptr;
            };
            auto *wv = findWalk(content);
            if (wv != nullptr)
                wv->setTestMode(true);
            for (int i = 0; i < 4; ++i)
                proc->walkPose[(size_t)i] = 0.0f;
            proc->clearWalkModel();
            if (auto *c = dynamic_cast<RysumContent *>(content))
                c->showView(8);
            run(30, {48, 55, 60, 64, 67});
            shoot(8, "ui_begehen.png");
            std::cout << "Begehen: view on " << proc->engine.viewWalkOn.load()
                      << (proc->engine.viewWalkOn.load() && wv != nullptr ? "  ok" : "  FAIL")
                      << std::endl;
            if (wv != nullptr)
            {
                // two seconds forwards, then turned to the left by 90 degrees: the engine's
                // listener follows
                for (int i = 0; i < 40; ++i)
                    wv->stepForTest(0.05, true, false, false, false, 0.0f);
                run(40, {});
                const float wy = proc->engine.viewY.load(), ly = proc->engine.listenerYNow.load();
                std::cout << "Begehen: walked " << wy << " m forwards, listener in the engine "
                          << ly << " m"
                          << (wy > 2.5f && std::abs(ly - wy) < 0.1f ? "  ok" : "  FAIL")
                          << std::endl;
                wv->stepForTest(0.05, false, false, false, false, 90.0f);
                std::cout << "Begehen: yaw " << proc->engine.viewYaw.load()
                          << (std::abs(proc->engine.viewYaw.load() - 90.0f) < 0.01f ? "  ok"
                                                                                    : "  FAIL")
                          << std::endl;
                shoot(8, "ui_begehen_gedreht.png");
                // a room model (OBJ, z up): the small chapel from Walk3D
                auto objFile = outDir.getChildFile("test_kapelle.obj");
                objFile.replaceWithText(rysum::makeTestObj());
                proc->walkZUp = true;
                wv->loadModelForTest(objFile);
                for (int i = 0; i < 4; ++i)
                    proc->walkPose[(size_t)i] = 0.0f;
                if (auto *c = dynamic_cast<RysumContent *>(content))
                {
                    c->showView(2);
                    c->showView(8);
                }
                auto m = proc->getWalkMesh();
                std::cout << "Begehen: model " << (m ? (int)m->tris.size() : 0) << " triangles, "
                          << (m ? m->size.x : 0.0f) << " x " << (m ? m->size.y : 0.0f) << " x "
                          << (m ? m->size.z : 0.0f) << " m"
                          << (m && m->tris.size() == 16 * 12 &&
                                      std::abs(m->size.x - 12.8f) < 0.01f &&
                                      std::abs(m->size.z - 9.2f) < 0.01f
                                  ? "  ok"
                                  : "  FAIL")
                          << std::endl;
                shoot(8, "ui_begehen_modell.png");
                juce::MemoryBlock mb;
                proc->getStateInformation(mb);
                proc->clearWalkModel();
                proc->setStateInformation(mb.getData(), (int)mb.getSize());
                std::cout << "Begehen: model after the state round trip "
                          << (proc->getWalkMesh() != nullptr ? "loaded" : "missing")
                          << (proc->getWalkMesh() != nullptr && proc->walkZUp ? "  ok" : "  FAIL")
                          << std::endl;
                {
                    // the same file read with Y up (the usual OBJ convention): its height becomes
                    // the length
                    juce::String e;
                    auto m2 = rysum::loadObjRoom(objFile, false, 1.0f, e);
                    std::cout << "Begehen: read with Y up " << (m2 ? m2->size.x : 0.0f) << " x "
                              << (m2 ? m2->size.y : 0.0f) << " x " << (m2 ? m2->size.z : 0.0f)
                              << " m"
                              << (m2 && std::abs(m2->size.x - 12.8f) < 0.01f &&
                                          std::abs(m2->size.y - 9.2f) < 0.01f &&
                                          std::abs(m2->size.z - 24.8f) < 0.01f
                                      ? "  ok"
                                      : "  FAIL")
                              << std::endl;
                    // no line end after the last face, decimals (read the same whatever the locale)
                    auto f2 = outDir.getChildFile("test_dreieck.obj");
                    f2.replaceWithText("v 0 0 0\nv 1.5 0 0\nv 0 2.25 0.5\nf 1 2 3");
                    auto m3 = rysum::loadObjRoom(f2, true, 1.0f, e);
                    std::cout << "Begehen: one triangle without a final line end: "
                              << (m3 ? (int)m3->tris.size() : 0) << ", " << (m3 ? m3->size.x : 0.0f)
                              << " x " << (m3 ? m3->size.y : 0.0f) << " x "
                              << (m3 ? m3->size.z : 0.0f)
                              << (m3 && m3->tris.size() == 1 &&
                                          std::abs(m3->size.x - 1.5f) < 1e-4f &&
                                          std::abs(m3->size.y - 2.25f) < 1e-4f
                                      ? "  ok"
                                      : "  FAIL")
                              << std::endl;
                }
                {
                    // speed: a model at the limit (200 x 200 quads = 80,000 triangles, a wavy floor
                    // over 24 m), painted 20 times
                    juce::MemoryOutputStream os;
                    const int N = 200;
                    for (int y = 0; y <= N; ++y)
                        for (int x = 0; x <= N; ++x)
                            os << "v " << juce::String(x * 0.12f, 3) << " "
                               << juce::String(y * 0.12f, 3) << " "
                               << juce::String(0.4f * std::sin(x * 0.2f) * std::cos(y * 0.17f), 3)
                               << "\n";
                    for (int y = 0; y < N; ++y)
                        for (int x = 0; x < N; ++x)
                        {
                            const int a = y * (N + 1) + x + 1;
                            os << "f " << a << " " << a + 1 << " " << a + N + 2 << " " << a + N + 1
                               << "\n";
                        }
                    auto big = outDir.getChildFile("test_gross.obj");
                    big.replaceWithText(os.toString());
                    proc->loadWalkModel(big, true, 1.0f);
                    auto mb3 = proc->getWalkMesh();
                    juce::Image frame(juce::Image::ARGB, wv->getWidth(), wv->getHeight(), true);
                    const double t0 = juce::Time::getMillisecondCounterHiRes();
                    for (int i = 0; i < 20; ++i)
                    {
                        juce::Graphics g(frame);
                        wv->paintEntireComponent(g, false);
                    }
                    const double ms = (juce::Time::getMillisecondCounterHiRes() - t0) / 20.0;
                    std::cout << "Begehen: " << (mb3 ? (int)mb3->tris.size() : 0)
                              << " triangles: " << ms << " ms per frame (" << wv->renderWidth()
                              << " px wide)"
                              << (mb3 && mb3->tris.size() == 80000 ? "  ok" : "  FAIL")
                              << std::endl;
                }
                proc->clearWalkModel();
                {
                    // the Saal's own room with all eight stops on and a chord sounding: the frame
                    // time at full resolution, then the way the app runs it (the resolution
                    // adapting), for the Saal and for the largest model
                    std::array<float, rysum::kNumStops> stopWas{};
                    for (int s = 0; s < rysum::kNumStops; ++s)
                    {
                        stopWas[(size_t)s] =
                            proc->apvts.getRawParameterValue(RysumProcessor::stopParamId(s))
                                ->load();
                        setReal(RysumProcessor::stopParamId(s).toRawUTF8(), 1.0f);
                    }
                    run(20, {48, 55, 60, 64, 67});
                    if (auto *c = dynamic_cast<RysumContent *>(content))
                    {
                        c->showView(2);
                        c->showView(8);
                    }
                    juce::Image frame(juce::Image::ARGB, wv->getWidth(), wv->getHeight(), true);
                    double t0 = juce::Time::getMillisecondCounterHiRes();
                    for (int i = 0; i < 20; ++i)
                    {
                        juce::Graphics g(frame);
                        wv->paintEntireComponent(g, false);
                    }
                    double ms = (juce::Time::getMillisecondCounterHiRes() - t0) / 20.0;
                    std::cout << "Begehen: all stops: " << ms << " ms per frame (geometry "
                              << wv->geometryMs() << " ms, 3D total " << wv->frameMs() << " ms, "
                              << wv->renderStats() << ", " << wv->renderWidth() << " px wide)"
                              << (wv->renderWidth() >= wv->getWidth() ? "  ok" : "  FAIL")
                              << std::endl;
                    wv->setTestMode(false);
                    for (int i = 0; i < 100; ++i)
                    {
                        juce::Graphics g(frame);
                        wv->paintEntireComponent(g, false);
                    }
                    std::cout << "Begehen: adapting, the Saal settles at " << wv->renderWidth()
                              << " px wide (3D " << wv->frameMs() << " ms, geometry "
                              << wv->geometryMs() << " ms)"
                              << (wv->renderWidth() >= (int)(0.4f * (float)wv->getWidth())
                                      ? "  ok"
                                      : "  FAIL")
                              << std::endl;
                    proc->loadWalkModel(outDir.getChildFile("test_gross.obj"), true, 1.0f);
                    for (int i = 0; i < 100; ++i)
                    {
                        juce::Graphics g(frame);
                        wv->paintEntireComponent(g, false);
                    }
                    std::cout << "Begehen: adapting, 80,000 triangles settle at "
                              << wv->renderWidth() << " px wide (3D " << wv->frameMs()
                              << " ms, geometry " << wv->geometryMs() << " ms)"
                              << (wv->renderWidth() >= (int)(0.4f * (float)wv->getWidth())
                                      ? "  ok"
                                      : "  FAIL")
                              << std::endl;
                    proc->clearWalkModel();
                    wv->setTestMode(true);
                    for (int s = 0; s < rysum::kNumStops; ++s)
                        setReal(RysumProcessor::stopParamId(s).toRawUTF8(), stopWas[(size_t)s]);
                    shoot(8, "ui_begehen_alle.png");
                }
                // what the walls look like follows the absorption (the Saal page's materials)
                {
                    const bool okM =
                        rysum::wallMaterial(0.5f, 0.85f, 0.3f).kind == rysum::Material::curtain &&
                        rysum::wallMaterial(0.12f, 0.55f, 0.3f).kind == rysum::Material::wood &&
                        rysum::wallMaterial(0.02f, 0.3f, 0.3f).kind == rysum::Material::stone &&
                        rysum::wallMaterial(0.04f, 0.1f, 0.3f).kind == rysum::Material::glass &&
                        rysum::wallMaterial(0.0f, 0.0f, 0.0f).kind == rysum::Material::mirror &&
                        rysum::wallMaterial(0.5f, 0.85f, 0.3f).tone <
                            rysum::wallMaterial(0.02f, 0.3f, 0.3f).tone;
                    std::cout << "Begehen: materials from the absorption (curtain, wood, stone, "
                                 "glass, mirror; curtain darker than stone)"
                              << (okM ? "  ok" : "  FAIL") << std::endl;
                    const struct
                    {
                        float aw, af, hi;
                        const char *name;
                    } looks[]{{0.5f, 0.35f, 0.85f, "vorhang"},
                              {0.12f, 0.15f, 0.55f, "holz"},
                              {0.04f, 0.06f, 0.1f, "glas"}};
                    for (auto &lk : looks)
                    {
                        setReal("simAbs", lk.aw);
                        setReal("simAbsFloor", lk.af);
                        setReal("simHigh", lk.hi);
                        run(4, {});
                        shoot(8, juce::String("ui_begehen_") + lk.name + ".png");
                    }
                    setReal("simAbs", 0.1f);
                    setReal("simAbsFloor", 0.25f);
                    setReal("simHigh", 0.5f);
                }
                // a cross-shaped church: walked (held inside the walls, as when walking) into an
                // arm until pipes are out of sight
                {
                    setReal("simShape", (float)rysum::shapeCross);
                    setReal("simShapeAmt", 0.8f);
                    for (int i = 0; i < 4; ++i)
                        proc->walkPose[(size_t)i] = 0.0f;
                    if (auto *c = dynamic_cast<RysumContent *>(content))
                    {
                        c->showView(2);
                        c->showView(8);
                    }
                    run(10, {});
                    int bestHidden = 0;
                    float bx = 0, by = 0;
                    // (a snake over the grid: the listener walks - at most 6 m/s - from one point
                    // to the next)
                    for (int ix = 0; ix <= 14; ++ix)
                        for (int j = 0; j <= 13; ++j)
                        {
                            const float ox = -7.0f + (float)ix,
                                        oy = (ix % 2 == 0) ? -10.0f + (float)j : 3.0f - (float)j;
                            wv->setPoseForTest(ox, oy, 0.0f);
                            run(24, {});
                            const int hid = proc->engine.hiddenNow.load();
                            if (hid > bestHidden)
                            {
                                bestHidden = hid;
                                bx = proc->walkPose[0].load();
                                by = proc->walkPose[1].load();
                            }
                        }
                    // look towards the crossing (the middle of the plan)
                    const float W = proc->engine.heard[0].W.load(),
                                L = proc->engine.heard[0].L.load();
                    const float hx = proc->engine.heard[0].Lx.load() * W,
                                hy = proc->engine.heard[0].Ly.load() * L;
                    const float dx = 0.5f * W - (hx + bx), dy = 0.5f * L - (hy + by);
                    wv->setPoseForTest(bx, by, juce::radiansToDegrees(std::atan2(-dx, dy)));
                    run(400, {}); // (walks back there)
                    std::cout << "Begehen: cross-shaped plan, at " << bx << " / " << by << " m "
                              << proc->engine.hiddenNow.load()
                              << " sounding pipes heard around the corner"
                              << (proc->engine.hiddenNow.load() > 0 ? "  ok" : "  FAIL")
                              << std::endl;
                    shoot(8, "ui_begehen_kreuz.png");
                    setReal("simShape", 0.0f);
                    setReal("simShapeAmt", 0.5f);
                    for (int i = 0; i < 4; ++i)
                        proc->walkPose[(size_t)i] = 0.0f;
                }
            }
            if (auto *c = dynamic_cast<RysumContent *>(content))
                c->showView(2);
            std::cout << "Begehen: leaving the view gives the listener back "
                      << (proc->engine.viewWalkOn.load() ? "no" : "yes")
                      << (!proc->engine.viewWalkOn.load() ? "  ok" : "  FAIL") << std::endl;
            juce::MidiBuffer off;
            for (int n : {48, 55, 60, 64, 67})
                off.addEvent(juce::MidiMessage::noteOff(1, n), 0);
            st.clear(), proc->processBlock(st, off);
            setReal("simOn", 0.0f);
        }

        {
            // scenes: A with a soft Gedackt, B louder with more wind leaks; half way glides,
            // switches change at the middle
            auto setReal = [&](const char *id, float v)
            {
                if (auto *q = proc->apvts.getParameter(id))
                    q->setValueNotifyingHost(q->convertTo0to1(v));
            };
            auto real = [&](const char *id)
            { return proc->apvts.getRawParameterValue(id)->load(); };
            setReal("vol1", -10.0f);
            setReal("fxLeakLevel", 0.0f);
            setReal("tremOn", 0.0f);
            setReal("sceneMorph", 0.0f);
            proc->storeScene(0);
            setReal("vol1", 2.0f);
            setReal("fxLeakLevel", 0.8f);
            setReal("tremOn", 1.0f);
            proc->storeScene(1);
            const float master0 = real("master");
            setReal("sceneMorph", 0.25f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
            const float v25 = real("vol1"), t25 = real("tremOn");
            setReal("sceneMorph", 0.75f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
            const float v75 = real("vol1"), t75 = real("tremOn"), l75 = real("fxLeakLevel");
            // stored in the state
            juce::MemoryBlock mb;
            proc->getStateInformation(mb);
            proc->clearScene(0);
            proc->clearScene(1);
            proc->setStateInformation(mb.getData(), (int)mb.getSize());
            const bool kept =
                proc->isSceneStored(0) && proc->isSceneStored(1) && !proc->isSceneStored(2);
            std::cout << "scenes: at 25 % vol " << v25 << " dB trem " << t25 << ", at 75 % vol "
                      << v75 << " dB trem " << t75 << " leaks " << l75 << ", master unchanged "
                      << juce::exactlyEqual(real("master"), master0) << ", kept in the state "
                      << kept
                      << ((std::abs(v25 - (-7.0f)) < 0.6f && t25 < 0.5f &&
                           std::abs(v75 - (-1.0f)) < 0.6f && t75 > 0.5f && kept)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            shoot(0, "ui_spieltisch.png");
            proc->clearScene(0);
            proc->clearScene(1);
            setReal("sceneMorph", 0.0f);
            setReal("vol1", 0.0f);
            setReal("fxLeakLevel", 0.0f);
            setReal("tremOn", 0.0f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil(100);

            // dice: wild changes things, keeps master/output, at least one stop; undo restores
            juce::MemoryBlock before;
            proc->getStateInformation(before);
            std::vector<float> normBefore;
            for (auto *q : proc->getParameters())
                normBefore.push_back(q->getValue());
            const float m0 = real("master");
            proc->dice(true);
            int stops = 0, changed = 0;
            for (int s2 = 0; s2 < rysum::kNumStops; ++s2)
                stops += real(RysumProcessor::stopParamId(s2).toRawUTF8()) > 0.5f;
            juce::MemoryBlock after;
            proc->getStateInformation(after);
            changed = after == before ? 0 : 1;
            proc->undoDice();
            juce::MemoryBlock undone;
            proc->getStateInformation(undone);
            float worstUndo = 0.0f;
            {
                int i = 0;
                for (auto *q : proc->getParameters())
                    worstUndo =
                        std::max(worstUndo, std::abs(q->getValue() - normBefore[(size_t)i++]));
            }
            const bool restored = worstUndo < 1.0e-4f;
            std::cout << "dice: changed " << changed << ", stops pulled " << stops
                      << ", master kept " << juce::exactlyEqual(real("master"), m0)
                      << ", undo restores (worst " << worstUndo << ")"
                      << ((changed && stops > 0 && juce::exactlyEqual(real("master"), m0) &&
                           restored)
                              ? "  ok"
                              : "  FAIL")
                      << std::endl;
            if (!restored)
            {
                auto xa =
                    proc->getXmlFromBinary(before.getData(), (int)before.getSize())->toString();
                auto xb =
                    proc->getXmlFromBinary(undone.getData(), (int)undone.getSize())->toString();
                juce::StringArray la, lb;
                la.addLines(xa);
                lb.addLines(xb);
                for (int i = 0, shown = 0; i < juce::jmax(la.size(), lb.size()) && shown < 6; ++i)
                    if (la[i] != lb[i])
                    {
                        std::cout << "- " << la[i].substring(0, 200) << "\n+ "
                                  << lb[i].substring(0, 200) << std::endl;
                        ++shown;
                    }
            }
            shoot(6, "ui_orgel.png");
        }

        // full screen: the interface moves into a window covering the screen, and back
        if (std::getenv("RYSUM_SKIP_FULLSCREEN") == nullptr)
        {
            auto *c = dynamic_cast<RysumContent *>(content);
            c->showView(0);
            editor->toggleFullscreen();
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            auto *top = c->getTopLevelComponent();
            const auto screen =
                juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->totalArea;
            const bool moved = editor->isFullscreen() && top != editor.get() &&
                               top->getBounds() == screen && c->isShowing();
            if (moved)
            {
                auto img = top->createComponentSnapshot(top->getLocalBounds(), true, 1.0f);
                juce::PNGImageFormat png;
                auto f = outDir.getChildFile("ui_fullscreen.png");
                f.deleteFile();
                juce::FileOutputStream os(f);
                png.writeImageToStream(img, os);
            }
            // F11 / Esc are handled by the interface itself
            c->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            const bool back = !editor->isFullscreen() && c->getParentComponent() == editor.get();
            std::cout << "full screen: " << screen.toString() << ", moved " << moved
                      << ", back with Esc " << back << ((moved && back) ? "  ok" : "  FAIL")
                      << std::endl;
        }

        // state round trip
        juce::MemoryBlock mbk;
        proc->getStateInformation(mbk);
        proc->setStateInformation(mbk.getData(), (int)mbk.getSize());
        juce::MemoryBlock mbk2;
        proc->getStateInformation(mbk2);
        std::cout << "state bytes: " << mbk.getSize()
                  << (mbk == mbk2 ? "  round trip identical" : "  ROUND TRIP DIFFERS") << std::endl;
        // pluginval-style: wiggle each parameter, restore, compare
        {
            juce::Random rnd(1);
            for (auto *prm : proc->getParameters())
            {
                prm->setValue(rnd.nextFloat());
                proc->setStateInformation(mbk.getData(), (int)mbk.getSize());
                juce::MemoryBlock m3;
                proc->getStateInformation(m3);
                if (!(m3 == mbk))
                {
                    std::cout << "state differs after wiggling " << prm->getName(64) << std::endl;
                    mbk2 = m3;
                    break;
                }
            }
        }
        if (!(mbk == mbk2))
        {
            auto a = proc->getXmlFromBinary(mbk.getData(), (int)mbk.getSize())->toString();
            auto b = proc->getXmlFromBinary(mbk2.getData(), (int)mbk2.getSize())->toString();
            juce::StringArray la, lb;
            la.addLines(a);
            lb.addLines(b);
            for (int i = 0; i < juce::jmax(la.size(), lb.size()); ++i)
                if (la[i] != lb[i])
                    std::cout << "- " << la[i].substring(0, 300) << "\n+ "
                              << lb[i].substring(0, 300) << std::endl;
        }

        editor.reset();
        proc.reset();
        std::cout << "done" << std::endl;
        quit();
    }

    void shutdown() override
    {
        editor.reset();
        proc.reset();
    }

private:
    juce::File samples, outDir;
    std::unique_ptr<RysumProcessor> proc;
    std::unique_ptr<RysumEditor> editor;
    int waited = 0, stage = 0;
    juce::File irFile;
};

START_JUCE_APPLICATION(SnapshotApp)
