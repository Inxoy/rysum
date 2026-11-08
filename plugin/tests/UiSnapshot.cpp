// Renders the three views of the editor into PNG files (for checking the UI without a host).
//   RysumUiSnapshot <samples folder> <output folder>
#include "PluginProcessor.h"
#include "PluginEditor.h"

class SnapshotApp : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "RysumUiSnapshot"; }
    const juce::String getApplicationVersion() override { return "0.1"; }

    void initialise (const juce::String& cmd) override
    {
        auto args = juce::StringArray::fromTokens (cmd, true);
        if (args.size() < 2) { std::cout << "usage: RysumUiSnapshot <samples> <outdir>" << std::endl; quit(); return; }
        samples = juce::File::getCurrentWorkingDirectory().getChildFile (args[0].unquoted());
        outDir = juce::File::getCurrentWorkingDirectory().getChildFile (args[1].unquoted());
        outDir.createDirectory();
        proc = std::make_unique<RysumProcessor>();
        proc->loadSamples (samples);
        startTimer (100);
    }

    void timerCallback() override
    {
        if (proc->getLibrary() == nullptr && ++waited < 200) return;
        if (stage == 0)
        {
            std::cout << "status: " << proc->getLoadStatus() << std::endl;
            // a synthetic 4-channel AmbiX IR to exercise the room path
            {
                // like a host: main input and side chain stereo, 5th-order output
                juce::AudioProcessor::BusesLayout lay;
                lay.inputBuses.add (juce::AudioChannelSet::stereo());
                lay.inputBuses.add (juce::AudioChannelSet::stereo());
                lay.outputBuses.add (juce::AudioChannelSet::ambisonic (5));
                const bool okLayout = proc->setBusesLayout (lay);
                proc->setRateAndBufferSizeDetails (48000.0, 512);
                std::cout << "bus layout with inputs: " << (okLayout && proc->getTotalNumInputChannels() == 4 ? "ok" : "FAIL") << std::endl;
            }
            proc->prepareToPlay (48000.0, 512);
            juce::AudioBuffer<float> ir (4, 48000);
            juce::Random rnd (5);
            for (int c = 0; c < 4; ++c) for (int i = 0; i < 48000; ++i) ir.setSample (c, i, (rnd.nextFloat() * 2 - 1) * std::exp (-5.0f * i / 48000.0f));
            irFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Testkirche_ambix.wav");
            irFile.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (irFile.createOutputStream());
            if (auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (4).withBitsPerSample (24)))
                w->writeFromAudioSampleBuffer (ir, 0, ir.getNumSamples());
            proc->loadIr (irFile);
            stage = 1; waited = 0;
            return;
        }
        if (! proc->getIrStatus().contains ("AmbiX") && ++waited < 100) return;
        stopTimer();
        std::cout << "ir: " << proc->getIrStatus() << std::endl;

        // play something so held keys / sounding pipes / meters show
        for (auto* id : { "stop0", "stop1", "stop2", "stop6" })
            proc->apvts.getParameter (id)->setValueNotifyingHost (1.0f);
        proc->apvts.getParameter ("motion6")->setValueNotifyingHost (proc->apvts.getParameter ("motion6")->convertTo0to1 (1.0f));
        juce::AudioBuffer<float> buf (36, 512);
        for (int b = 0; b < 40; ++b)
        {
            juce::MidiBuffer mb;
            if (b == 2) for (int m : { 57, 64, 69 }) mb.addEvent (juce::MidiMessage::noteOn (1, m, (juce::uint8) 100), 0);
            if (b == 1) { proc->apvts.getParameter ("swind2")->setValueNotifyingHost (0.4f); proc->apvts.getParameter ("vol1")->setValueNotifyingHost (0.6f); }
            buf.clear(), proc->processBlock (buf, mb);
        }
        // the room must add sound: same phrase with and without IR, compare the energy
        {
            proc->apvts.getParameter ("wind")->setValueNotifyingHost (0.0f);
            proc->apvts.getParameter ("click")->setValueNotifyingHost (0.0f);
            proc->apvts.getParameter ("releaseClick")->setValueNotifyingHost (0.0f);
            proc->apvts.getParameter ("output")->setValueNotifyingHost (proc->apvts.getParameter ("output")->convertTo0to1 (1.0f));
            auto phrase = [&]()
            {
                double e = 0;
                for (int b = 0; b < 200; ++b)
                {
                    juce::MidiBuffer mb;
                    if (b == 10) mb.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                    if (b == 40) mb.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                    buf.clear(), proc->processBlock (buf, mb);
                    for (int c = 0; c < 4; ++c) for (int i = 0; i < 512; ++i) e += (double) buf.getSample (c, i) * buf.getSample (c, i);
                }
                return e;
            };
            const double withIr = phrase();
            proc->clearIr();
            { juce::MidiBuffer none; buf.clear(), proc->processBlock (buf, none); }
            const double dry = phrase();
            std::cout << "energy with IR / dry: " << withIr / dry << (withIr > dry * 1.2 ? "  ok" : "  FAIL") << std::endl;
            proc->loadIr (irFile);
        }

        // typed values
        {
            auto P = [&] (const char* id) { return proc->apvts.getParameter (id); };
            struct T { const char* id; const char* txt; float expect; };
            const T tests[] { { "click", "50", 0.5f }, { "click", "35 %", 0.35f }, { "lfo1Rate", "1,5", 1.5f }, { "env1Attack", "2 s", 2.0f },
                              { "rate0", "-2.5", -2.5f }, { "degRate", "22", 22000.0f }, { "mod1Amt", "-40", -0.4f }, { "master", "-6", -6.0f } };
            bool ok = true;
            for (auto& t : tests)
            {
                const float v = rysum::ui::parseValueText (*P (t.id), t.txt);
                if (std::abs (v - t.expect) > 1e-3f * std::max (1.0f, std::abs (t.expect))) { ok = false; std::cout << "  parse " << t.id << " '" << t.txt << "' -> " << v << std::endl; }
            }
            // ms when the parameter shows ms
            P ("env1Attack")->setValueNotifyingHost (P ("env1Attack")->convertTo0to1 (0.3f));
            ok = ok && std::abs (rysum::ui::parseValueText (*P ("env1Attack"), "500") - 0.5f) < 1e-4f;
            P ("env1Attack")->setValueNotifyingHost (P ("env1Attack")->getDefaultValue());
            const float cents = rysum::ui::parseValueText (*P ("pitch"), "440");
            ok = ok && std::abs (cents - 1200.0f * std::log2 (440.0f / (float) rysum::kA4)) < 0.1f;
            std::cout << "typed values: " << (ok ? "ok" : "FAIL") << std::endl;
        }

        // binaural: output on "Binaural", decoder designed in the background, 2-channel host buffer
        {
            auto P = [&] (const char* id) { return proc->apvts.getParameter (id); };
            const float outBefore = P ("output")->getValue();
            P ("output")->setValueNotifyingHost (P ("output")->convertTo0to1 ((float) RysumProcessor::kBinauralOutput));
            auto pump = [&] (std::function<bool()> done) { for (int i = 0; i < 100 && ! done(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (100); };
            pump ([&] { return proc->binauralReady(); });
            std::cout << "binaural: " << proc->getBinauralStatus() << (proc->binauralReady() ? "  ok" : "  FAIL") << std::endl;
            for (auto* id : { "stop1", "stop2", "stop6" }) P (id)->setValueNotifyingHost (0.0f);
            const float irWetBefore = P ("irWet")->getValue();
            P ("irWet")->setValueNotifyingHost (0.0f);
            auto play = [&] (int ch)
            {
                juce::AudioBuffer<float> st (ch, 512);
                double l = 0, r = 0, rest = 0;
                for (int b = 0; b < 260; ++b)
                {
                    juce::MidiBuffer mb;
                    if (b == 0) for (int m : { 57, 60, 64, 69 }) mb.addEvent (juce::MidiMessage::noteOff (1, m), 0);   // earlier tests left keys down
                    if (b == 150) mb.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 100), 0);
                    if (b == 250) mb.addEvent (juce::MidiMessage::noteOff (1, 69), 0);
                    st.clear(), proc->processBlock (st, mb);
                    if (b > 170 && b < 250)
                        for (int i = 0; i < 512; ++i)
                        {
                            l += (double) st.getSample (0, i) * st.getSample (0, i);
                            if (ch > 1) r += (double) st.getSample (1, i) * st.getSample (1, i);
                            for (int c = 2; c < ch; ++c) rest += (double) st.getSample (c, i) * st.getSample (c, i);
                        }
                }
                return std::array<double, 3> { l, r, rest };
            };
            const auto e2 = play (2), e36 = play (36);
            const double pipeAz = proc->engine.currentPosition (0, rysum::keyIndexForMidi (69), proc->readParams()).az;
            std::cout << "binaural Praestant a' (az " << pipeAz << "): L/R " << 10 * std::log10 (e2[0] / e2[1]) << " dB, 36-ch bus: L/R "
                      << 10 * std::log10 (e36[0] / e36[1]) << " dB, other channels " << e36[2]
                      << ((e2[0] > 1e-6 && (pipeAz < 0 ? e2[1] > 1.5 * e2[0] : e2[0] > 1.5 * e2[1]) && e36[2] == 0.0) ? "  ok" : "  FAIL") << std::endl;

            // an own SOFA file (the full FABIAN set)
            const juce::File sofa (juce::SystemStats::getEnvironmentVariable ("RYSUM_TEST_SOFA", {}));          // optional
            if (sofa.existsAsFile())
            {
                proc->setSofaFile (sofa);
                pump ([&] { return proc->getBinauralStatus().contains ("11950"); });
                std::cout << "own SOFA: " << proc->getBinauralStatus() << (proc->getBinauralStatus().contains ("11950") ? "  ok" : "  FAIL") << std::endl;
                juce::MemoryBlock mb; proc->getStateInformation (mb);
                std::cout << "SOFA path in state: " << (proc->getXmlFromBinary (mb.getData(), (int) mb.getSize())->toString().contains ("HATO_0_48k.sofa") ? "ok" : "FAIL") << std::endl;
                P ("binOrder")->setValueNotifyingHost (P ("binOrder")->convertTo0to1 (4.0f));
                pump ([&] { return proc->getBinauralStatus().contains ("5. Ordnung"); });
                std::cout << "5th order: " << proc->getBinauralStatus() << (proc->getBinauralStatus().contains ("5. Ordnung") ? "  ok" : "  FAIL") << std::endl;
                const auto e5 = play (2);
                std::cout << "5th order own SOFA: L/R " << 10 * std::log10 (e5[0] / e5[1]) << " dB" << std::endl;
                proc->setSofaFile ({});
                P ("binOrder")->setValueNotifyingHost (P ("binOrder")->getDefaultValue());
                pump ([&] { return proc->getBinauralStatus().startsWith ("FABIAN") && proc->getBinauralStatus().contains ("3. Ordnung"); });
            }
            for (auto* id : { "stop1", "stop2", "stop6" }) P (id)->setValueNotifyingHost (1.0f);
            P ("irWet")->setValueNotifyingHost (irWetBefore);
            P ("output")->setValueNotifyingHost (outBefore);
        }

        // "Halten": two quick strikes keep the note down, a third strike lets it go; computer keyboard notes
        {
            juce::AudioBuffer<float> st (2, 512);
            auto block = [&] (std::initializer_list<juce::MidiMessage> msgs)
            {
                juce::MidiBuffer mb; int pos = 0;
                for (auto& m : msgs) mb.addEvent (m, pos += 40);
                st.clear(), proc->processBlock (st, mb);
            };
            const int k62 = rysum::keyIndexForMidi (62);
            auto held = [&] { return (proc->engine.heldMask.load() >> k62) & 1u; };
            proc->latchOn = true;
            block ({ juce::MidiMessage::noteOn (1, 62, (juce::uint8) 90), juce::MidiMessage::noteOff (1, 62), juce::MidiMessage::noteOn (1, 62, (juce::uint8) 90) });
            for (int i = 0; i < 5; ++i) block ({});
            block ({ juce::MidiMessage::noteOff (1, 62) });
            for (int i = 0; i < 50; ++i) block ({});
            const bool latchedOk = held() != 0 && proc->latchedCount.load() == 1;
            block ({ juce::MidiMessage::noteOn (1, 62, (juce::uint8) 90) });
            const bool stillDown = held() != 0;
            block ({ juce::MidiMessage::noteOff (1, 62) });
            const bool released = held() == 0 && proc->latchedCount.load() == 0;
            // single strike: normal
            block ({ juce::MidiMessage::noteOn (1, 64, (juce::uint8) 90) });
            for (int i = 0; i < 40; ++i) block ({});
            block ({ juce::MidiMessage::noteOff (1, 64) });
            const bool normal = ((proc->engine.heldMask.load() >> rysum::keyIndexForMidi (64)) & 1u) == 0;
            // switching it off lets everything go
            block ({ juce::MidiMessage::noteOn (1, 62, (juce::uint8) 90), juce::MidiMessage::noteOff (1, 62), juce::MidiMessage::noteOn (1, 62, (juce::uint8) 90), juce::MidiMessage::noteOff (1, 62) });
            const bool again = held() != 0;
            proc->latchOn = false;
            block ({});
            const bool offReleases = held() == 0;
            std::cout << "Halten: latched " << latchedOk << ", third strike keeps it until key up " << stillDown << ", released " << released
                      << ", single strike normal " << normal << ", switching off releases " << (again && offReleases)
                      << ((latchedOk && stillDown && released && normal && again && offReleases) ? "  ok" : "  FAIL") << std::endl;
            proc->keyboardState.noteOn (1, 60, 0.8f);
            block ({});
            const bool kbOn = ((proc->engine.heldMask.load() >> rysum::keyIndexForMidi (60)) & 1u) != 0;
            proc->keyboardState.noteOff (1, 60, 0.0f);
            block ({});
            std::cout << "on-screen / computer keyboard notes reach the engine: " << (kbOn ? "ok" : "FAIL") << std::endl;
        }

        // patches
        {
            auto files = RysumProcessor::getPatchFiles();
            std::cout << "factory patches: " << files.size() << (files.size() >= 6 ? "  ok" : "  FAIL") << std::endl;
            juce::File welle;
            for (auto& f : files) if (f.getFileName().contains ("Welle")) welle = f;
            const float outBefore = proc->apvts.getRawParameterValue ("output")->load();
            const bool loaded = proc->loadPatch (welle);
            std::cout << "load factory patch: " << ((loaded && proc->getRouteSrc (0) == rysum::srcLfo2 && proc->getRouteTgt (0) == rysum::tgtAz
                                                      && proc->apvts.getRawParameterValue ("stop0")->load() > 0.5f
                                                      && proc->apvts.getRawParameterValue ("output")->load() == outBefore) ? "ok" : "FAIL") << std::endl;
            auto* w = proc->apvts.getParameter ("wind");
            w->setValueNotifyingHost (0.33f);
            proc->savePatch ("Test Patch");
            w->setValueNotifyingHost (0.9f);
            proc->setRoute (0, -1, -1);
            proc->loadPatch (RysumProcessor::getPatchFolder().getChildFile ("Test Patch.rysum"));
            std::cout << "save / load patch: " << ((std::abs (w->getValue() - 0.33f) < 0.01f && proc->getRouteSrc (0) == rysum::srcLfo2
                                                    && proc->getPatchName() == "Test Patch") ? "ok" : "FAIL") << std::endl;
            w->setValueNotifyingHost (0.0f);
        }

        // every factory patch: play a chord through it (with the test room loaded), report the level
        {
            std::cout << "factory warnings: " << (proc->factoryWarnings.isEmpty() ? juce::String ("none") : proc->factoryWarnings.joinIntoString ("; ")) << std::endl;
            proc->loadIr (irFile);
            for (int w = 0; w < 100 && ! proc->getIrStatus().contains ("AmbiX"); ++w) juce::Thread::sleep (20);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
            bool allOk = true;
            for (auto& f : RysumProcessor::getPatchFiles())
            {
                proc->loadPatch (f);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
                float peak = 0.0f; double e = 0.0; bool finite = true; long cnt = 0;
                const bool feedInput = proc->apvts.getRawParameterValue ("inpOn")->load() > 0.5f;
                rysum::XorShift noise (11);
                for (int b = 0; b < 600; ++b)          // 6.4 s
                {
                    juce::MidiBuffer mb;
                    if (b == 5)   for (int m : { 41, 53, 57, 60, 65, 69, 72 }) mb.addEvent (juce::MidiMessage::noteOn (1, m, (juce::uint8) (b % 2 ? 60 : 110)), 0);
                    if (b == 420) for (int m : { 41, 53, 57, 60, 65, 69, 72 }) mb.addEvent (juce::MidiMessage::noteOff (1, m), 0);
                    buf.clear();
                    if (feedInput) for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) buf.setSample (c, i, 0.1f * noise.bi());   // a patch for the input: something to hear
                    proc->processBlock (buf, mb);
                    for (int c = 0; c < 4; ++c)
                        for (int i = 0; i < 512; ++i)
                        {
                            const float v = buf.getSample (c, i);
                            if (! std::isfinite (v)) finite = false;
                            peak = std::max (peak, std::abs (v));
                            if (c == 0) { e += (double) v * v; ++cnt; }
                        }
                }
                juce::MidiBuffer off;
                for (int m = 36; m < 82; ++m) off.addEvent (juce::MidiMessage::noteOff (1, m), 0);
                buf.clear(), proc->processBlock (buf, off);
                for (int b = 0; b < 300; ++b) { juce::MidiBuffer none; buf.clear(), proc->processBlock (buf, none); }   // let tails / freeze die
                const float rmsDb = juce::Decibels::gainToDecibels ((float) std::sqrt (e / juce::jmax (1L, cnt)));
                const bool ok = finite && peak > 1.0e-4f;
                allOk = allOk && ok;
                if (feedInput && std::getenv ("RYSUM_DEBUG_INPUT") != nullptr)
                {
                    const auto pp = proc->readParams();
                    std::cout << "    input: on " << pp.input.on << " level " << pp.input.level << " dist " << pp.input.dist << " shown " << proc->engine.inShowCount.load()
                              << " peak " << proc->engine.inPeak.load() << " out " << pp.outputMode << std::endl;
                }
                if (auto* io = proc->apvts.getParameter ("inpOn")) io->setValueNotifyingHost (0.0f);    // the next patch: without the input again
                std::cout << "  " << f.getFileNameWithoutExtension() << (feedInput ? " [Eingang]" : "") << ": peak " << juce::Decibels::gainToDecibels (peak) << " dB, rms W "
                          << rmsDb << " dB" << (ok ? "" : "  FAIL") << std::endl;
            }
            std::cout << "all factory patches play: " << (allOk ? "ok" : "FAIL") << std::endl;
            // back to a plain state for the rest of the test
            proc->setRoute (0, -1, -1);
        }

        // modulation: LFO 1 -> global wind (a parameter), LFO 2 -> pipe direction with spread, envelope -> distance
        {
            using namespace rysum;
            auto setP = [&] (const char* id, float v) { auto* a = proc->apvts.getParameter (id); a->setValueNotifyingHost (a->convertTo0to1 (v)); };
            const int windIdx = proc->getParamIndex ("wind");
            proc->setRoute (0, srcLfo1, kGlobalTargetBase + windIdx);         setP ("mod1Amt", 0.4f);
            proc->setRoute (1, srcLfo2, tgtAz);                               setP ("mod2Amt", 0.5f);
            proc->setRoute (2, srcEnv1, tgtDist);                             setP ("mod3Amt", 0.3f);
            proc->setRoute (3, srcLfo3, kGlobalTargetBase + proc->getParamIndex ("irWet")); setP ("mod4Amt", -0.3f);
            setP ("lfo1Rate", 2.0f); setP ("lfo2Spread", 1.0f); setP ("lfo2Rate", 0.7f); setP ("lfo4Spread", 0.6f);
            setP ("lfo3Shape", 4.0f);
            float lo = 2, hi = -1;
            for (int b = 0; b < 120; ++b)
            {
                juce::MidiBuffer mb;
                if (b == 1) for (int m : { 45, 52, 57, 64, 69, 72 }) mb.addEvent (juce::MidiMessage::noteOn (1, m, (juce::uint8) 100), 0);
                buf.clear(), proc->processBlock (buf, mb);
                const float n = proc->getModulatedNorm (windIdx);
                if (! std::isnan (n)) { lo = juce::jmin (lo, n); hi = juce::jmax (hi, n); }
            }
            for (int b = 0; b < 200 && ! (proc->getModulatedNorm (windIdx) > 0.3f); ++b) { juce::MidiBuffer none; buf.clear(), proc->processBlock (buf, none); }
            const float base = proc->apvts.getParameter ("wind")->getValue();
            std::cout << "wind modulated: base " << base << " range " << lo << " .. " << hi
                      << ((hi - lo > 0.3f && lo <= base + 1e-4f && hi > base) ? "  ok" : "  FAIL") << std::endl;
            {
                auto prm = proc->readParams();
                // a silent pipe (Praestant C, not played) and a sounding one (a)
                float dSilent = 0, dSound = 0;
                for (int k : { 0, 20 })
                {
                    const auto a = proc->engine.currentPosition (0, k, prm), b = proc->engine.displayPosition (0, k, prm);
                    (k == 0 ? dSilent : dSound) = std::abs (rysum::wrapAz (b.az - a.az)) + std::abs (b.dist - a.dist);
                }
                std::cout << "raum shows modulation: silent " << dSilent << ", sounding " << dSound
                          << ((dSilent > 0.5f && dSound > 0.5f) ? "  ok" : "  FAIL") << std::endl;
                proc->setRoute (19, rysum::srcLfo4, rysum::tgtEl);
                setP ("mod20Amt", 0.2f);
                juce::MidiBuffer none; buf.clear(), proc->processBlock (buf, none);
                auto prm2 = proc->readParams();
                std::cout << "slot 20: " << ((prm2.mod.routes[19].src == rysum::srcLfo4 && std::abs (prm2.mod.routes[19].amount - 0.2f) < 1e-4f) ? "ok" : "FAIL") << std::endl;
            }
            std::cout << "not modulated -> NaN: " << (std::isnan (proc->getModulatedNorm (proc->getParamIndex ("air"))) ? "ok" : "FAIL") << std::endl;
        }

        // MIDI learn: a knob, a range, a button, and a learned CC wins over the fixed map
        {
            for (int i = 0; i < RysumProcessor::kNumMidiMaps; ++i) proc->clearMidiMap (i);
            auto P = [&] (const char* id) { return proc->apvts.getParameter (id); };
            juce::AudioBuffer<float> st (2, 512);
            auto cc = [&] (int ch, int num, int val)
            {
                juce::MidiBuffer mb; mb.addEvent (juce::MidiMessage::controllerEvent (ch, num, val), 10);
                st.clear(), proc->processBlock (st, mb);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (40);
            };
            const int knob = proc->learnMidiFor (proc->getParamIndex ("swind0"));
            const bool waiting = proc->getLearningSlot() == knob;
            cc (2, 21, 64);
            const auto m = proc->getMidiMap (knob);
            const bool learned = m.cc == 21 && m.channel == 2 && proc->getLearningSlot() < 0;
            const bool follows = std::abs (P ("swind0")->getValue() - 64.0f / 127.0f) < 0.002f;
            auto r = m; r.lo = 0.2f; r.hi = 0.8f; proc->setMidiMap (knob, r);
            cc (2, 21, 127); const float top = P ("swind0")->getValue();
            cc (2, 21, 0);   const float bottom = P ("swind0")->getValue();
            cc (3, 21, 127); const bool otherChannel = std::abs (P ("swind0")->getValue() - bottom) < 1e-4f;
            std::cout << "MIDI learn knob: waiting " << waiting << ", learned CC 21 ch 2 " << learned << ", follows " << follows
                      << ", range " << bottom << ".." << top << ", other channel ignored " << otherChannel
                      << ((waiting && learned && follows && std::abs (top - 0.8f) < 0.002f && std::abs (bottom - 0.2f) < 0.002f && otherChannel) ? "  ok" : "  FAIL") << std::endl;

            P ("tremOn")->setValueNotifyingHost (0.0f);
            const int button = proc->learnMidiFor (proc->getParamIndex ("tremOn"));
            cc (1, 80, 127);                     // the learning press itself does not switch
            const bool learnNoFlip = P ("tremOn")->getValue() < 0.5f && proc->getMidiMap (button).toggle;
            cc (1, 80, 0);
            cc (1, 80, 127); const bool on = P ("tremOn")->getValue() > 0.5f;
            cc (1, 80, 0);   const bool stays = P ("tremOn")->getValue() > 0.5f;
            cc (1, 80, 127); const bool off = P ("tremOn")->getValue() < 0.5f;
            cc (1, 80, 0);
            std::cout << "MIDI learn button: " << learnNoFlip << on << stays << off << ((learnNoFlip && on && stays && off) ? "  ok" : "  FAIL") << std::endl;

            P ("wind")->setValueNotifyingHost (0.0f);
            const int over = proc->learnMidiFor (proc->getParamIndex ("click"));
            cc (1, 11, 100);
            cc (1, 11, 127);
            const bool clickMoved = std::abs (P ("click")->getValue() - 1.0f) < 0.002f, windStill = P ("wind")->getValue() < 0.001f;
            std::cout << "learned CC 11 replaces the fixed map: " << ((clickMoved && windStill) ? "ok" : "FAIL") << std::endl;
            proc->clearMidiMap (over);
            P ("click")->setValueNotifyingHost (P ("click")->getDefaultValue());
            P ("tremOn")->setValueNotifyingHost (0.0f);

            juce::MemoryBlock mbm; proc->getStateInformation (mbm);
            const auto xml = proc->getXmlFromBinary (mbm.getData(), (int) mbm.getSize())->toString();
            for (int i = 0; i < 8; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (100);      // saved as the default for new instances
            const auto saved = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Rysum/MIDI-Regler.xml").loadFileAsString();
            std::cout << "MIDI assignments in the state " << xml.contains ("swind0") << ", saved for new instances " << saved.contains ("tremOn")
                      << ((xml.contains ("swind0") && saved.contains ("tremOn")) ? "  ok" : "  FAIL") << std::endl;
            {
                RysumProcessor fresh;
                const bool inherits = fresh.getMidiMap (knob).cc == 21 && fresh.getMidiMap (button).param == fresh.getParamIndex ("tremOn");
                std::cout << "a new instance starts with them: " << (inherits ? "ok" : "FAIL") << std::endl;
            }
            proc->learnMidiFor (proc->getParamIndex ("simTail"));      // shows "bewegen ..." in the snapshot
        }

        // free tuning: a Scala file (kept in the state), MIDI Tuning Standard SysEx (as Hexatone sends it)
        {
            auto P = [&] (const char* id) { return proc->apvts.getParameter (id); };
            const auto scl = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rysum_test_19edo.scl");
            juce::String txt ("! 19-EDO\n19 equal\n 19\n");
            for (int i = 1; i <= 19; ++i) txt << juce::String (1200.0 * i / 19.0, 4) << "\n";
            scl.replaceWithText (txt);
            const bool loaded = proc->loadTuningFile (scl);
            const int t = juce::roundToInt (proc->apvts.getRawParameterValue ("temperament")->load());
            const bool free = proc->readParams().notes != nullptr;
            juce::MemoryBlock mb; proc->getStateInformation (mb);
            const bool inState = proc->getXmlFromBinary (mb.getData(), (int) mb.getSize())->toString().contains ("19 equal");
            std::cout << "scala file: loaded " << loaded << ", tuning " << t << ", free notes " << free << ", in the state " << inState
                      << " (" << proc->getTuningStatus().upToFirstOccurrenceOf ("\n", false, false) << ")"
                      << ((loaded && t == RysumProcessor::kScalaTuning && free && inState) ? "  ok" : "  FAIL") << std::endl;

            // MTS: without a master, a real-time single-note tuning change SysEx retunes note 69 to b-flat (466.16 Hz)
            proc->loadPatch (RysumProcessor::getPatchFolder().getChildFile ("01 Init.rysum"));      // a clean sound (earlier tests leave all sorts)
            P ("temperament")->setValueNotifyingHost (P ("temperament")->convertTo0to1 ((float) RysumProcessor::kMtsTuning));
            juce::AudioBuffer<float> st (2, 512);
            const juce::uint8 sx[] { 0xF0, 0x7F, 0x7F, 0x08, 0x02, 0x00, 0x01, 69, 70, 0x00, 0x00, 0xF7 };
            for (auto* id : { "stop1", "stop2", "stop6" }) P (id)->setValueNotifyingHost (0.0f);
            for (auto* id : { "click", "releaseClick", "wind", "irWet" }) P (id)->setValueNotifyingHost (0.0f);
            for (int r = 0; r < rysum::kNumRoutes; ++r) proc->clearRoute (r);       // earlier tests modulate the wind (pitch gusts)
            P ("output")->setValueNotifyingHost (0.0f);
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage (sx, (int) sizeof (sx)), 0); st.clear(), proc->processBlock (st, m); }
            const auto status = proc->getTuningStatus();
            std::vector<float> rec;
            for (int b = 0; b < 140; ++b)
            {
                juce::MidiBuffer m;
                if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 100), 0);
                if (b == 139) m.addEvent (juce::MidiMessage::noteOff (1, 69), 0);
                st.clear(), proc->processBlock (st, m);
                if (b >= 60) for (int i = 0; i < 512; ++i) rec.push_back (st.getSample (0, i));
            }
            const double sr = proc->getSampleRate() > 0 ? proc->getSampleRate() : 48000.0;
            auto ac = [&] (int lag) { double s2 = 0; for (size_t i = 0; i + (size_t) lag < rec.size(); ++i) s2 += (double) rec[i] * rec[i + (size_t) lag]; return s2; };
            int best = 0; double bv = -1e18;
            for (int lag = (int) (sr / 520.0); lag <= (int) (sr / 420.0); ++lag) { const double v = ac (lag); if (v > bv) { bv = v; best = lag; } }
            const double y0 = ac (best - 1), y1 = ac (best), y2 = ac (best + 1);
            const double hz = sr / (best + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2));
            const double err = 1200.0 * std::log2 (hz / 466.1638);
            std::cout << "MTS SysEx: " << status << "; note 69 sounds at " << hz << " Hz (" << err << " ct from 466.16)"
                      << ((status.contains ("SysEx") && std::abs (err) < 6.0) ? "  ok" : "  FAIL") << std::endl;
            for (auto* id : { "stop1", "stop2", "stop6" }) P (id)->setValueNotifyingHost (1.0f);
            P ("temperament")->setValueNotifyingHost (P ("temperament")->convertTo0to1 (0.0f));
        }

        // drawn trajectory (Trompete): kept in the state, the engine follows it; ellipse settings for the picture
        {
            auto setP = [&] (const juce::String& id, float v) { auto* a = proc->apvts.getParameter (id); a->setValueNotifyingHost (a->convertTo0to1 (v)); };
            std::vector<RysumProcessor::DrawnPath::Pt> pts;
            for (int i = 0; i < 120; ++i)
            {
                const float t = (float) i / 119.0f;
                pts.push_back ({ 160.0f * std::sin (6.283f * t) , 30.0f + 25.0f * std::sin (12.566f * t), 2.0f * t * t });   // a figure eight, drawn faster at the end
            }
            proc->setDrawnPath (6, pts);
            setP (RysumProcessor::motionParamId (6), (float) rysum::Motion::Drawn);
            setP (RysumProcessor::rateParamId (6), 0.05f);
            setP (RysumProcessor::ellipseParamId (6), 0.5f);
            setP (RysumProcessor::ellDirParamId (6), 45.0f);
            setP (RysumProcessor::ellFormParamId (6), 1.0f);
            const auto prm = proc->readParams();
            const auto pos = proc->engine.currentPosition (6, 0, prm);
            juce::MemoryBlock mb; proc->getStateInformation (mb);
            proc->setDrawnPath (6, {});
            proc->setStateInformation (mb.getData(), (int) mb.getSize());
            const auto& back = proc->getDrawnPath (6);
            std::cout << "drawn path: " << back.points.size() << " points after the state round trip, pipe 0 at az " << pos.az << " el " << pos.el
                      << ((back.points.size() == 120 && std::abs (back.points[60].az - pts[60].az) < 0.01f && proc->engine.getPath (6).n > 100) ? "  ok" : "  FAIL") << std::endl;
        }

        editor.reset (dynamic_cast<RysumEditor*> (proc->createEditor()));
        editor->setSize (1280, 720);
        auto* content = editor->getChildComponent (0);
        auto shoot = [&] (int view, const juce::String& name)
        {
            if (auto* c = dynamic_cast<RysumContent*> (content)) { c->showView (view); c->tick(); }
            juce::MessageManager::getInstance()->runDispatchLoopUntil (120);     // slider timers (modulation, MIDI tags)
            auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            auto f = outDir.getChildFile (name);
            f.deleteFile();
            juce::FileOutputStream os (f);
            png.writeImageToStream (img, os);
            std::cout << "wrote " << f.getFullPathName() << std::endl;
        };
        shoot (0, "ui_spieltisch.png");
        for (int k = 0; k < rysum::kNumKeys; k += 3) proc->setPipeMuted (6, k, true);      // some Trompete pipes muted
        shoot (1, "ui_raum.png");
        for (int k = 0; k < rysum::kNumKeys; ++k) proc->setPipeMuted (6, k, false);
        {
            // the audio input: on, stereo, kreisend; some noise through the processor so the meter and the places show
            auto setN = [&] (const char* id, float v) { if (auto* prm = proc->apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
            setN ("inpOn", 1.0f); setN ("inpSpin", 20.0f); setN ("inpWidth", 90.0f); setN ("inpAz", 30.0f); setN ("inpDist", 3.0f);
            juce::AudioBuffer<float> io (36, 512);
            rysum::XorShift r (5);
            float meanAbs = 0.0f;
            for (int b = 0; b < 40; ++b)
            {
                io.clear();
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) io.setSample (c, i, 0.2f * r.bi());
                juce::MidiBuffer mb;
                proc->processBlock (io, mb);
                if (b > 30) meanAbs += io.getMagnitude (0, 0, 512) + io.getMagnitude (1, 0, 512);
            }
            std::cout << "input through the processor: out peak " << meanAbs / 18.0f << ", shown sources " << proc->engine.inShowCount.load()
                      << ((proc->engine.inShowCount.load() == 2 && meanAbs > 0.01f) ? "  ok" : "  FAIL") << std::endl;
            rysum::ui::SpaceView* sv = nullptr;
            std::function<void (juce::Component*)> find = [&] (juce::Component* c)
            {
                if (auto* v = dynamic_cast<rysum::ui::SpaceView*> (c)) sv = v;
                for (auto* ch : c->getChildren()) if (sv == nullptr) find (ch);
            };
            find (editor.get());
            if (sv != nullptr) sv->selectInput();
            shoot (1, "ui_eingang.png");
            if (sv != nullptr) sv->selectStop (6);
            setN ("inpOn", 0.0f); setN ("inpSpin", 0.0f); setN ("inpWidth", 60.0f); setN ("inpAz", 0.0f); setN ("inpDist", 4.0f);
        }
        shoot (3, "ui_klang.png");
        {
            // loudspeakers: 7.1.4 at 3rd order through the processor - sound on the layout's channels only, the LFE channel silent
            auto l714 = rysum::speakerPresets()[3];
            proc->setSpeakerLayout (l714, false);
            auto* o = proc->apvts.getParameter ("output");
            const float outBefore = o->getValue();
            o->setValueNotifyingHost (o->convertTo0to1 ((float) RysumProcessor::kSpeakerOutput));
            juce::AudioBuffer<float> io (36, 512);
            double e[36] {};
            for (int b = 0; b < 60; ++b)
            {
                io.clear();
                juce::MidiBuffer mb;
                if (b == 2) for (int m : { 48, 55, 60, 64 }) mb.addEvent (juce::MidiMessage::noteOn (1, m, (juce::uint8) 100), 0);
                proc->processBlock (io, mb);
                if (b > 20) for (int c = 0; c < 36; ++c) e[c] += io.getRMSLevel (c, 0, 512);
            }
            int sounding = 0; for (int c = 0; c < 12; ++c) sounding += e[c] > 1.0e-4 ? 1 : 0;
            double rest = 0; for (int c = 12; c < 36; ++c) rest += e[c];
            proc->speakerTest = 0;
            io.clear(); { juce::MidiBuffer none; proc->processBlock (io, none); }
            proc->speakerTest = -1;
            std::cout << "loudspeakers 7.1.4: " << sounding << " of 12 channels sound, LFE " << e[3] << ", above 12: " << rest
                      << ((sounding == 11 && e[3] == 0.0 && rest == 0.0) ? "  ok" : "  FAIL") << std::endl;
            juce::MemoryBlock mb; proc->getStateInformation (mb);
            proc->setSpeakerLayout (rysum::speakerPresets()[0], false);
            proc->setStateInformation (mb.getData(), (int) mb.getSize());
            std::cout << "loudspeaker layout in the state: " << (proc->getSpeakerLayout().speakers.size() == 11 ? "ok" : "FAIL") << std::endl;
            shoot (7, "ui_lautsprecher.png");
            for (int b = 0; b < 20; ++b) { io.clear(); juce::MidiBuffer mb; if (b == 0) for (int m : { 48, 55, 60, 64 }) mb.addEvent (juce::MidiMessage::noteOff (1, m), 0); proc->processBlock (io, mb); }
            o->setValueNotifyingHost (outBefore);
            proc->setSpeakerLayout (rysum::speakerPresets()[4], false);
        }
        shoot (5, "ui_mod.png");
        {
            proc->apvts.getParameter ("windModel")->setValueNotifyingHost (1.0f);
            shoot (3, "ui_klang_wind2.png");
            proc->apvts.getParameter ("windModel")->setValueNotifyingHost (0.0f);
            if (auto* c = dynamic_cast<RysumContent*> (content)) c->openMidiMap();
            shoot (5, "ui_midi.png");
            for (int i = 0; i < RysumProcessor::kNumMidiMaps; ++i) proc->clearMidiMap (i);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
        }
        {
            // right-click menus are top-level windows without a parent: they must get the plugin's look by default
            auto& lf = juce::LookAndFeel::getDefaultLookAndFeel();
            const bool ours = &lf == &editor->getLookAndFeel();
            std::cout << "default look = plugin look (menus, dialogs): " << (ours ? "ok" : "FAIL") << std::endl;
            // paint a menu the way JUCE does, to look at it
            juce::Image img (juce::Image::ARGB, 260, 150, true);
            {
                juce::Graphics g (img);
                lf.drawPopupMenuBackground (g, 260, 150);
                const char* items[] { "Verstimmung", "Modulieren mit", "Entfernen: LFO 1 (+25 %)", "Modulationsseite \xc3\xb6" "ffnen" };
                for (int i = 0; i < 4; ++i)
                    lf.drawPopupMenuItem (g, { 4, 6 + i * 34, 252, 30 }, false, true, i == 2, false, i == 1, juce::String::fromUTF8 (items[i]), {}, nullptr, nullptr);
            }
            juce::PNGImageFormat png;
            auto f = outDir.getChildFile ("ui_menu.png");
            f.deleteFile();
            juce::FileOutputStream os (f);
            png.writeImageToStream (img, os);
            std::cout << "wrote " << f.getFullPathName() << std::endl;
        }
        {
            // effects page with some effects switched on (so the diagrams show something)
            for (auto [id, v] : { std::pair<const char*, float> { "fxEchoLevel", 0.5f }, { "fxPartSpread", 70.0f }, { "fxEchoTilt", 10.0f } })
                if (auto* prm = proc->apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (v));
            {
                // own sounds from a chosen folder (not <sample-library>/noise/eigene): read, kept in the state, back to standard
                auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rysum_own_sounds_test");
                dir.deleteRecursively(); dir.getChildFile ("sub").createDirectory();
                juce::AudioBuffer<float> snd (1, 9600);
                for (int i = 0; i < 9600; ++i) snd.setSample (0, i, 0.3f * std::exp (-i / 900.0f) * std::sin (i * 0.7f));
                for (auto name : { "a.wav", "sub/b.wav" })
                {
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (dir.getChildFile (name)), 48000.0, 1, 24, {}, 0));
                    w->writeFromAudioSampleBuffer (snd, 0, 9600);
                }
                auto waitFor = [&] (int n) { for (int k = 0; k < 150 && proc->getNumCustomSounds() != n; ++k) juce::MessageManager::getInstance()->runDispatchLoopUntil (20); };
                const int before = proc->getNumCustomSounds();
                proc->setCustomFolder (dir);
                waitFor (2);
                const int got = proc->getNumCustomSounds();
                juce::MemoryBlock mb; proc->getStateInformation (mb);
                proc->setCustomFolder ({});
                waitFor (before);
                proc->setStateInformation (mb.getData(), (int) mb.getSize());
                waitFor (2);
                const bool restored = proc->getCustomFolder() == dir && proc->getNumCustomSounds() == 2;
                shoot (4, "ui_effekte_ordner.png");
                proc->setCustomFolder ({});
                waitFor (before);
                std::cout << "own sounds from a chosen folder: read " << got << ", from the state " << restored << ", back to standard "
                          << (! proc->hasOwnCustomFolder() && proc->getNumCustomSounds() == before)
                          << ((got == 2 && restored && ! proc->hasOwnCustomFolder()) ? "  ok" : "  FAIL") << std::endl;
                dir.deleteRecursively();
            }
            shoot (4, "ui_effekte.png");
            for (auto* id : { "fxEchoLevel", "fxPartSpread", "fxEchoTilt" })
                if (auto* prm = proc->apvts.getParameter (id)) prm->setValueNotifyingHost (prm->getDefaultValue());
        }
        {
            // the simulated room: on, a note played, ray tracing done in the background
            auto* P = proc->apvts.getParameter ("simOn");
            P->setValueNotifyingHost (1.0f);
            juce::AudioBuffer<float> st (16, 512);
            for (int b = 0; b < 60; ++b)
            {
                juce::MidiBuffer mb;
                if (b == 2) mb.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0);
                st.clear(), proc->processBlock (st, mb);
            }
            for (int i = 0; i < 40 && ! proc->getRoomStatus().contains ("Strahlen"); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            for (int b = 0; b < 20; ++b) { juce::MidiBuffer mb; st.clear(), proc->processBlock (st, mb); }        // the engine takes the response
            std::cout << "Saal: " << proc->getRoomStatus() << (proc->getRoomStatus().contains ("Strahlen") ? "  ok" : "  FAIL") << std::endl;
            std::cout << "Saal: engine RT " << proc->engine.roomRtLow.load() << " s, tail in use " << proc->engine.tailSerialInUse.load() << ", "
                      << proc->engine.erActive.load() << " sounds with reflections"
                      << (proc->engine.erActive.load() > 0 && proc->engine.tailSerialInUse.load() != 0xffffffffu ? "  ok" : "  FAIL") << std::endl;
            // Lucier mode on: the response of 12 passes is built in the background, the engine takes it
            auto* L = proc->apvts.getParameter ("lucOn");
            if (auto* md = proc->apvts.getParameter ("lucMode")) md->setValueNotifyingHost (1.0f);      // live
            L->setValueNotifyingHost (1.0f);
            if (auto* g = proc->apvts.getParameter ("lucGen")) g->setValueNotifyingHost (g->convertTo0to1 (12.0f));
            for (int i = 0; i < 60 && proc->getLucierView().curve.empty(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            for (int b = 0; b < 200; ++b) { juce::MidiBuffer mb; st.clear(), proc->processBlock (st, mb); }
            const auto& lv = proc->getLucierView();
            std::cout << "Lucier: " << lv.what << ", gen " << lv.generation << ", " << lv.peaks.size() << " resonances, first "
                      << (lv.peaks.empty() ? 0.0f : lv.peaks[0]) << " Hz, engine level " << proc->engine.lucierLevel.load()
                      << (lv.curve.size() > 10 && proc->engine.lucierSerialInUse.load() != 0xffffffffu ? "  ok" : "  FAIL") << std::endl;
            shoot (2, "ui_saal.png");
            {
                // the piece: record 3 s of the organ, then it plays: the recording, generation 1, 2 - by itself, then it ends
                auto setReal = [&] (const char* id, float v) { if (auto* q = proc->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v)); };
                setReal ("lucGen", 2.0f); setReal ("lucGap", 0.5f); setReal ("lucMix", 0.5f); setReal ("lucRecSrc", 0.0f);
                proc->lucierRecord();
                auto& pc = proc->getLucierPiece();
                const bool modeOk = proc->isLucierPieceMode();
                for (int b = 0; b < 280; ++b)        // 3 s
                {
                    juce::MidiBuffer mb;
                    if (b == 10) mb.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                    if (b == 200) mb.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
                    st.clear(), proc->processBlock (st, mb);
                }
                for (int i = 0; i < 20 && proc->getLucierView().curve.empty(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
                proc->lucierStopRecording();
                const double recSec = (double) pc.getRecording().size() / pc.recordingRate();
                const bool saved = juce::File (pc.recordingFile).existsAsFile();
                // play it through: blocks of audio, the message thread computing the generations in between
                int maxGen = 0; float peak1 = 0.0f; bool ended = false;
                for (int b = 0; b < 4000 && ! ended; ++b)
                {
                    juce::MidiBuffer mb;
                    st.clear(), proc->processBlock (st, mb);
                    if (b % 8 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil (2);
                    maxGen = std::max (maxGen, pc.playingGen.load());
                    if (pc.playingGen.load() >= 1) peak1 = std::max (peak1, st.getMagnitude (0, 0, 512));
                    ended = pc.state.load() == rysum::LucierPiece::stFinished;
                }
                std::cout << "Lucier piece: mode " << modeOk << ", recorded " << recSec << " s, saved " << saved << ", reached generation " << maxGen
                          << ", level from gen 1 " << peak1 << ", ended " << ended
                          << ((modeOk && recSec > 1.0 && saved && maxGen == 2 && peak1 > 0.01f && ended) ? "  ok" : "  FAIL") << std::endl;
                proc->lucierPlay();
                for (int b = 0; b < 300; ++b) { juce::MidiBuffer mb; st.clear(), proc->processBlock (st, mb); if (b % 8 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil (2); }
                shoot (2, "ui_saal_stueck.png");
                proc->lucierStop();
                juce::File (pc.recordingFile).deleteFile();
                if (auto* md = proc->apvts.getParameter ("lucMode")) md->setValueNotifyingHost (1.0f);
            }
            {
                // several rooms: the Gedackt in Saal 2 (a chapel) - its own late reverb is built and used
                auto setReal = [&] (const juce::String& id, float v) { if (auto* q = proc->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v)); };
                setReal (RysumProcessor::stopRoomParamId (1), 1.0f);
                setReal ("stop1", 1.0f);
                for (int b = 0; b < 40; ++b) { juce::MidiBuffer mb; if (b == 1) mb.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0); st.clear(), proc->processBlock (st, mb); }
                for (int i = 0; i < 80 && proc->engine.tailSerialInUseR[1].load() == 0xffffffffu; ++i)
                {
                    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
                    juce::MidiBuffer mb; st.clear(), proc->processBlock (st, mb);
                }
                const int used = proc->engine.roomsUsed.load();
                std::cout << "rooms: used mask " << used << ", room 2 RT " << proc->engine.heard[1].rtLow.load() << " s, tail in use "
                          << proc->engine.tailSerialInUseR[1].load()
                          << (((used & 2) != 0 && proc->engine.tailSerialInUseR[1].load() != 0xffffffffu && proc->engine.heard[1].rtLow.load() > 1.0f) ? "  ok" : "  FAIL") << std::endl;
                shoot (2, "ui_saal_raeume.png");
                { juce::MidiBuffer mb; mb.addEvent (juce::MidiMessage::noteOff (1, 60), 0); st.clear(), proc->processBlock (st, mb); }
                setReal (RysumProcessor::stopRoomParamId (1), 0.0f);
            }
            {
                // floor plans: L-shape, cross, drawn - the late reverb is rebuilt for each, the page shows the plan
                L->setValueNotifyingHost (0.0f);
                auto setReal = [&] (const juce::String& id, float v) { if (auto* q = proc->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v)); };
                auto settle = [&]
                {
                    for (int b = 0; b < 30; ++b) { juce::MidiBuffer mb; st.clear(), proc->processBlock (st, mb); }
                    for (int i = 0; i < 40; ++i) { juce::MessageManager::getInstance()->runDispatchLoopUntil (60); juce::MidiBuffer mb; st.clear(), proc->processBlock (st, mb); }
                };
                const float rtBox = proc->engine.roomRtLow.load();
                const juce::String names[] { "l", "kreuz", "rund", "gezeichnet" };
                const int shapes[] { rysum::shapeL, rysum::shapeCross, rysum::shapeRound, rysum::shapeDrawn };
                for (int i = 0; i < 4; ++i)
                {
                    setReal ("simShape", (float) shapes[i]);
                    setReal ("simShapeAmt", 0.7f);
                    { juce::MidiBuffer mb; mb.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0); st.clear(), proc->processBlock (st, mb); }
                    settle();
                    std::cout << "Saal plan " << names[i] << ": RT " << proc->engine.roomRtLow.load() << " s (shoebox " << rtBox << " s), "
                              << proc->engine.erActive.load() << " sounds with reflections, " << proc->getRoomStatus() << std::endl;
                    shoot (2, "ui_saal_plan_" + names[i] + ".png");
                }
                // the drawn plan goes through the project state
                rysum::FloorPlan d; d.n = 5;
                const float xs[] { 0.0f, 1.0f, 0.8f, 0.3f, 0.1f }, ys[] { 0.1f, 0.0f, 1.0f, 0.85f, 0.6f };
                for (int k = 0; k < 5; ++k) { d.x[(size_t) k] = xs[k]; d.y[(size_t) k] = ys[k]; }
                proc->setDrawnPlan (2, d);
                juce::MemoryBlock mb2; proc->getStateInformation (mb2);
                proc->setDrawnPlan (2, rysum::defaultDrawnPlan());
                proc->setStateInformation (mb2.getData(), (int) mb2.getSize());
                const auto& back = proc->getDrawnPlan (2);
                std::cout << "drawn floor plan: " << back.n << " corners after the state round trip"
                          << ((back.n == 5 && std::abs (back.x[2] - 0.8f) < 1e-3f && proc->engine.getDrawnPlan (2).n == 5) ? "  ok" : "  FAIL") << std::endl;
                setReal ("simShape", 0.0f);
                setReal ("simShapeAmt", 0.5f);
                L->setValueNotifyingHost (1.0f);
            }
            L->setValueNotifyingHost (0.0f);
            { juce::MidiBuffer mb; mb.addEvent (juce::MidiMessage::noteOff (1, 64), 0); st.clear(), proc->processBlock (st, mb); }
            P->setValueNotifyingHost (0.0f);
        }

        {
            // scenes: A with a soft Gedackt, B louder with more wind leaks; half way glides, switches change at the middle
            auto setReal = [&] (const char* id, float v) { if (auto* q = proc->apvts.getParameter (id)) q->setValueNotifyingHost (q->convertTo0to1 (v)); };
            auto real = [&] (const char* id) { return proc->apvts.getRawParameterValue (id)->load(); };
            setReal ("vol1", -10.0f); setReal ("fxLeakLevel", 0.0f); setReal ("tremOn", 0.0f); setReal ("sceneMorph", 0.0f);
            proc->storeScene (0);
            setReal ("vol1", 2.0f); setReal ("fxLeakLevel", 0.8f); setReal ("tremOn", 1.0f);
            proc->storeScene (1);
            const float master0 = real ("master");
            setReal ("sceneMorph", 0.25f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
            const float v25 = real ("vol1"), t25 = real ("tremOn");
            setReal ("sceneMorph", 0.75f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
            const float v75 = real ("vol1"), t75 = real ("tremOn"), l75 = real ("fxLeakLevel");
            // stored in the state
            juce::MemoryBlock mb; proc->getStateInformation (mb);
            proc->clearScene (0); proc->clearScene (1);
            proc->setStateInformation (mb.getData(), (int) mb.getSize());
            const bool kept = proc->isSceneStored (0) && proc->isSceneStored (1) && ! proc->isSceneStored (2);
            std::cout << "scenes: at 25 % vol " << v25 << " dB trem " << t25 << ", at 75 % vol " << v75 << " dB trem " << t75 << " leaks " << l75
                      << ", master unchanged " << (real ("master") == master0) << ", kept in the state " << kept
                      << ((std::abs (v25 - (-7.0f)) < 0.6f && t25 < 0.5f && std::abs (v75 - (-1.0f)) < 0.6f && t75 > 0.5f && kept) ? "  ok" : "  FAIL") << std::endl;
            shoot (0, "ui_spieltisch.png");
            proc->clearScene (0); proc->clearScene (1);
            setReal ("sceneMorph", 0.0f); setReal ("vol1", 0.0f); setReal ("fxLeakLevel", 0.0f); setReal ("tremOn", 0.0f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

            // dice: wild changes things, keeps master/output, at least one stop; undo restores
            juce::MemoryBlock before; proc->getStateInformation (before);
            std::vector<float> normBefore;
            for (auto* q : proc->getParameters()) normBefore.push_back (q->getValue());
            const float m0 = real ("master");
            proc->dice (true);
            int stops = 0, changed = 0;
            for (int s2 = 0; s2 < rysum::kNumStops; ++s2) stops += real (RysumProcessor::stopParamId (s2).toRawUTF8()) > 0.5f;
            juce::MemoryBlock after; proc->getStateInformation (after);
            changed = after == before ? 0 : 1;
            proc->undoDice();
            juce::MemoryBlock undone; proc->getStateInformation (undone);
            float worstUndo = 0.0f;
            { int i = 0; for (auto* q : proc->getParameters()) worstUndo = std::max (worstUndo, std::abs (q->getValue() - normBefore[(size_t) i++])); }
            const bool restored = worstUndo < 1.0e-4f;
            std::cout << "dice: changed " << changed << ", stops pulled " << stops << ", master kept " << (real ("master") == m0)
                      << ", undo restores (worst " << worstUndo << ")" << ((changed && stops > 0 && real ("master") == m0 && restored) ? "  ok" : "  FAIL") << std::endl;
            if (! restored)
            {
                auto xa = proc->getXmlFromBinary (before.getData(), (int) before.getSize())->toString();
                auto xb = proc->getXmlFromBinary (undone.getData(), (int) undone.getSize())->toString();
                juce::StringArray la, lb; la.addLines (xa); lb.addLines (xb);
                for (int i = 0, shown = 0; i < juce::jmax (la.size(), lb.size()) && shown < 6; ++i)
                    if (la[i] != lb[i]) { std::cout << "- " << la[i].substring (0, 200) << "\n+ " << lb[i].substring (0, 200) << std::endl; ++shown; }
            }
            shoot (6, "ui_orgel.png");
        }

        // full screen: the interface moves into a window covering the screen, and back
        if (std::getenv ("RYSUM_SKIP_FULLSCREEN") == nullptr)
        {
            auto* c = dynamic_cast<RysumContent*> (content);
            c->showView (0);
            editor->toggleFullscreen();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
            auto* top = c->getTopLevelComponent();
            const auto screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->totalArea;
            const bool moved = editor->isFullscreen() && top != editor.get() && top->getBounds() == screen && c->isShowing();
            if (moved)
            {
                auto img = top->createComponentSnapshot (top->getLocalBounds(), true, 1.0f);
                juce::PNGImageFormat png;
                auto f = outDir.getChildFile ("ui_fullscreen.png");
                f.deleteFile();
                juce::FileOutputStream os (f);
                png.writeImageToStream (img, os);
            }
            // F11 / Esc are handled by the interface itself
            c->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
            juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
            const bool back = ! editor->isFullscreen() && c->getParentComponent() == editor.get();
            std::cout << "full screen: " << screen.toString() << ", moved " << moved << ", back with Esc " << back
                      << ((moved && back) ? "  ok" : "  FAIL") << std::endl;
        }

        // state round trip
        juce::MemoryBlock mbk;
        proc->getStateInformation (mbk);
        proc->setStateInformation (mbk.getData(), (int) mbk.getSize());
        juce::MemoryBlock mbk2;
        proc->getStateInformation (mbk2);
        std::cout << "state bytes: " << mbk.getSize() << (mbk == mbk2 ? "  round trip identical" : "  ROUND TRIP DIFFERS") << std::endl;
        // pluginval-style: wiggle each parameter, restore, compare
        {
            juce::Random rnd (1);
            for (auto* prm : proc->getParameters())
            {
                prm->setValue (rnd.nextFloat());
                proc->setStateInformation (mbk.getData(), (int) mbk.getSize());
                juce::MemoryBlock m3;
                proc->getStateInformation (m3);
                if (! (m3 == mbk))
                {
                    std::cout << "state differs after wiggling " << prm->getName (64) << std::endl;
                    mbk2 = m3;
                    break;
                }
            }
        }
        if (! (mbk == mbk2))
        {
            auto a = proc->getXmlFromBinary (mbk.getData(), (int) mbk.getSize())->toString();
            auto b = proc->getXmlFromBinary (mbk2.getData(), (int) mbk2.getSize())->toString();
            juce::StringArray la, lb; la.addLines (a); lb.addLines (b);
            for (int i = 0; i < juce::jmax (la.size(), lb.size()); ++i)
                if (la[i] != lb[i]) std::cout << "- " << la[i].substring (0, 300) << "\n+ " << lb[i].substring (0, 300) << std::endl;
        }

        editor.reset();
        proc.reset();
        std::cout << "done" << std::endl;
        quit();
    }

    void shutdown() override { editor.reset(); proc.reset(); }

private:
    juce::File samples, outDir;
    std::unique_ptr<RysumProcessor> proc;
    std::unique_ptr<RysumEditor> editor;
    int waited = 0, stage = 0;
    juce::File irFile;
};

START_JUCE_APPLICATION (SnapshotApp)
