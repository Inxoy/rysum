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
            proc->setPlayConfigDetails (0, 36, 48000.0, 512);
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
            proc->processBlock (buf, mb);
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
                    proc->processBlock (buf, mb);
                    for (int c = 0; c < 4; ++c) for (int i = 0; i < 512; ++i) e += (double) buf.getSample (c, i) * buf.getSample (c, i);
                }
                return e;
            };
            const double withIr = phrase();
            proc->clearIr();
            { juce::MidiBuffer none; proc->processBlock (buf, none); }
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
                for (int b = 0; b < 600; ++b)          // 6.4 s
                {
                    juce::MidiBuffer mb;
                    if (b == 5)   for (int m : { 41, 53, 57, 60, 65, 69, 72 }) mb.addEvent (juce::MidiMessage::noteOn (1, m, (juce::uint8) (b % 2 ? 60 : 110)), 0);
                    if (b == 420) for (int m : { 41, 53, 57, 60, 65, 69, 72 }) mb.addEvent (juce::MidiMessage::noteOff (1, m), 0);
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
                proc->processBlock (buf, off);
                for (int b = 0; b < 300; ++b) { juce::MidiBuffer none; proc->processBlock (buf, none); }   // let tails / freeze die
                const float rmsDb = juce::Decibels::gainToDecibels ((float) std::sqrt (e / juce::jmax (1L, cnt)));
                const bool ok = finite && peak > 1.0e-4f;
                allOk = allOk && ok;
                std::cout << "  " << f.getFileNameWithoutExtension() << ": peak " << juce::Decibels::gainToDecibels (peak) << " dB, rms W "
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
                proc->processBlock (buf, mb);
                const float n = proc->getModulatedNorm (windIdx);
                if (! std::isnan (n)) { lo = juce::jmin (lo, n); hi = juce::jmax (hi, n); }
            }
            for (int b = 0; b < 200 && ! (proc->getModulatedNorm (windIdx) > 0.3f); ++b) { juce::MidiBuffer none; proc->processBlock (buf, none); }
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
                juce::MidiBuffer none; proc->processBlock (buf, none);
                auto prm2 = proc->readParams();
                std::cout << "slot 20: " << ((prm2.mod.routes[19].src == rysum::srcLfo4 && std::abs (prm2.mod.routes[19].amount - 0.2f) < 1e-4f) ? "ok" : "FAIL") << std::endl;
            }
            std::cout << "not modulated -> NaN: " << (std::isnan (proc->getModulatedNorm (proc->getParamIndex ("air"))) ? "ok" : "FAIL") << std::endl;
        }

        editor.reset (dynamic_cast<RysumEditor*> (proc->createEditor()));
        editor->setSize (1120, 720);
        auto* content = editor->getChildComponent (0);
        auto shoot = [&] (int view, const juce::String& name)
        {
            if (auto* c = dynamic_cast<RysumContent*> (content)) { c->showView (view); c->tick(); }
            auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
            juce::PNGImageFormat png;
            auto f = outDir.getChildFile (name);
            f.deleteFile();
            juce::FileOutputStream os (f);
            png.writeImageToStream (img, os);
            std::cout << "wrote " << f.getFullPathName() << std::endl;
        };
        shoot (0, "ui_spieltisch.png");
        shoot (1, "ui_raum.png");
        shoot (2, "ui_klang.png");
        shoot (4, "ui_mod.png");
        {
            // effects page with some effects switched on (so the diagrams show something)
            for (auto [id, v] : { std::pair<const char*, float> { "fxEchoLevel", 0.5f }, { "fxPartSpread", 70.0f }, { "fxEchoTilt", 10.0f } })
                if (auto* prm = proc->apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (v));
            shoot (3, "ui_effekte.png");
            for (auto* id : { "fxEchoLevel", "fxPartSpread", "fxEchoTilt" })
                if (auto* prm = proc->apvts.getParameter (id)) prm->setValueNotifyingHost (prm->getDefaultValue());
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
