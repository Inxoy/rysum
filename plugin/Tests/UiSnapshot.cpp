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
        stopTimer();
        std::cout << "status: " << proc->getLoadStatus() << std::endl;

        // play something so held keys / sounding pipes / meters show
        proc->setPlayConfigDetails (0, 36, 48000.0, 512);
        proc->prepareToPlay (48000.0, 512);
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

        // state round trip
        juce::MemoryBlock mbk;
        proc->getStateInformation (mbk);
        proc->setStateInformation (mbk.getData(), (int) mbk.getSize());
        std::cout << "state bytes: " << mbk.getSize() << std::endl;

        editor.reset();
        proc.reset();
        quit();
    }

    void shutdown() override { editor.reset(); proc.reset(); }

private:
    juce::File samples, outDir;
    std::unique_ptr<RysumProcessor> proc;
    std::unique_ptr<RysumEditor> editor;
    int waited = 0;
};

START_JUCE_APPLICATION (SnapshotApp)
