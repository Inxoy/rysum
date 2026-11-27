// "Rysum Konzert": the app for listeners. It is the organ without its hands: the 3D Saal and a
// line to join a player. Everything the player plays is rendered here, from wherever one walks;
// one cannot play it oneself (no MIDI is opened, the 3D view only walks and looks). The samples
// come with it (the folder rysum-samples next to the program).
#include "ListenerView.h"

using namespace rysum;
using namespace rysum::ui;
using namespace rysum::ui::konzert;

namespace
{
class ListenerWindow : public juce::DocumentWindow
{
public:
    ListenerWindow(const juce::String &name, juce::Component *content)
        : juce::DocumentWindow(name, col::bg, juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(content, false);
        setResizable(true, false);
        setResizeLimits(800, 480, 4000, 3000);
        centreWithSize(1280, 760);
        setVisible(true);
    }
    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class ListenerApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override
    {
        return JucePlugin_Name " Konzert";
    }
    const juce::String getApplicationVersion() override
    {
        return "0.1";
    }
    bool moreThanOneInstanceAllowed() override
    {
        return true;
    }

    void initialise(const juce::String &) override
    {
        assets = std::make_unique<juce::SharedResourcePointer<Assets>>();
        lnf = std::make_unique<juce::SharedResourcePointer<LookAndFeel>>();
        juce::LookAndFeel::setDefaultLookAndFeel(&lnf->get());
        settings = std::make_unique<juce::PropertiesFile>(settingsOptions());
        proc = std::make_unique<RysumProcessor>();
        // what is the listener's own: how they hear (headphones by default) and how loud
        if (auto *o = proc->apvts.getParameter("output"))
        {
            const auto saved = settings->getValue("output");
            o->setValueNotifyingHost(saved.isNotEmpty() ? (float)saved.getDoubleValue()
                                                        : o->convertTo0to1((float)RysumProcessor::kBinauralOutput));
        }
        if (auto *m = proc->apvts.getParameter("master"); m != nullptr && settings->containsKey("master"))
            m->setValueNotifyingHost((float)settings->getDoubleValue("master"));
        // the sound card: outputs only (no input, no MIDI - nobody plays here)
        auto devState = settings->getXmlValue("audio");
        devices.initialise(0, 2, devState.get(), true);
        player.setProcessor(proc.get());
        devices.addAudioCallback(&player);
        window = std::make_unique<ListenerWindow>(getApplicationName(), new ListenerMain(*proc, &devices, *settings));
        window->setLookAndFeel(&lnf->get());
        const auto testDir = juce::SystemStats::getEnvironmentVariable("RYSUM_KONZERT_TEST", {});
        if (testDir.isNotEmpty())
            selfTest(juce::File(testDir));
    }

    /** RYSUM_KONZERT_TEST=<folder> (RYSUM_KONZERT_PORT=port): join a player on this computer,
        listen for a while, write what came (konzert_app.txt) and the window (konzert_app.png), quit */
    void selfTest(const juce::File &dir)
    {
        dir.createDirectory();
        const int port = juce::SystemStats::getEnvironmentVariable("RYSUM_KONZERT_PORT", "8792").getIntValue();
        const double seconds = juce::SystemStats::getEnvironmentVariable("RYSUM_KONZERT_SECONDS", "14").getDoubleValue();
        auto peak = std::make_shared<float>(0.0f);
        auto level = devices.getOutputLevelGetter();
        auto joinAt = std::make_shared<double>(0.0);
        juce::Timer::callAfterDelay(2500, [this, port, joinAt]
                                    {
                                        proc->concert.startListener("127.0.0.1", port);
                                        *joinAt = juce::Time::getMillisecondCounterHiRes();
                                    });
        struct Poll : juce::Timer
        {
            std::function<void()> f;
            void timerCallback() override { f(); }
        };
        auto poll = std::make_shared<Poll>();
        poll->f = [this, peak, level] { *peak = juce::jmax(*peak, (float)level->getCurrentLevel()); };
        poll->startTimer(20);
        juce::Timer::callAfterDelay((int)(seconds * 1000.0), [this, dir, peak, poll]
        {
            poll->stopTimer();
            auto &en = proc->engine;
            rysum::Vec3f p{};
            const bool body = en.bodyAt(0, 12, p);
            juce::String r;
            r << "samples: " << (proc->getLibrary() != nullptr ? proc->getSamplesFolder().getFullPathName() : "none") << "\n";
            r << "audio device: " << (devices.getCurrentAudioDevice() != nullptr ? devices.getCurrentAudioDevice()->getName() : "none") << "\n";
            r << "joined: " << (int)proc->concert.joined() << "   status: " << proc->concert.status() << "\n";
            r << "notes heard: " << (int)proc->concert.testNoteTimes().size() << "   knocks heard: " << proc->concert.knocksHeardForTest() << "\n";
            r << "peak output level: " << juce::String(*peak, 4) << "\n";
            r << "pipe 0/12 is a body: " << (int)body << " at " << p.x << " " << p.y << " " << p.z << "\n";
            dir.getChildFile("konzert_app.txt").replaceWithText(r);
            if (window != nullptr)
            {
                auto img = window->getContentComponent()->createComponentSnapshot(window->getContentComponent()->getLocalBounds());
                auto f = dir.getChildFile("konzert_app.png");
                f.deleteFile();
                juce::FileOutputStream os(f);
                juce::PNGImageFormat().writeImageToStream(img, os);
            }
            quit();
        });
    }

    void shutdown() override
    {
        if (proc != nullptr)
        {
            proc->concert.stop();
            if (auto *o = proc->apvts.getParameter("output"))
                settings->setValue("output", o->getValue());
            if (auto *m = proc->apvts.getParameter("master"))
                settings->setValue("master", m->getValue());
        }
        if (auto xml = devices.createStateXml())
            settings->setValue("audio", xml.get());
        settings->saveIfNeeded();
        devices.removeAudioCallback(&player);
        player.setProcessor(nullptr);
        if (window != nullptr)
            window->setLookAndFeel(nullptr);
        window.reset();
        proc.reset();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        lnf.reset();
        assets.reset();
        settings.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    std::unique_ptr<juce::SharedResourcePointer<Assets>> assets;
    std::unique_ptr<juce::SharedResourcePointer<LookAndFeel>> lnf;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<RysumProcessor> proc;
    juce::AudioDeviceManager devices;
    juce::AudioProcessorPlayer player;
    std::unique_ptr<ListenerWindow> window;
};
} // namespace

START_JUCE_APPLICATION(ListenerApp)
