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
