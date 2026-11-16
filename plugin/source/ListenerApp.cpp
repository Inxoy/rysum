// "Rysum Konzert": the app for listeners. It is the organ without its hands: the 3D Saal and a
// line to join a player. Everything the player plays is rendered here, from wherever one walks;
// one cannot play it oneself (no MIDI is opened, the 3D view only walks and looks). The samples
// come with it (the folder rysum-samples next to the program).
#include "PluginProcessor.h"
#include "Theme.h"
#include "WalkView.h"
#include <juce_audio_utils/juce_audio_utils.h>

using namespace rysum;
using namespace rysum::ui;

namespace
{
juce::String u(const char *s)
{
    return juce::String::fromUTF8(s);
}

juce::PropertiesFile::Options settingsOptions()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "Rysum Konzert";
    o.filenameSuffix = ".settings";
    o.folderName = "Rysum";
    o.osxLibrarySubFolder = "Application Support";
    return o;
}

class ListenerMain : public juce::Component, private juce::Timer
{
public:
    ListenerMain(RysumProcessor &p, juce::AudioDeviceManager &dm, juce::PropertiesFile &s)
        : proc(p), devices(dm), settings(s), walk(p)
    {
        walk.setAlwaysSpectator(true);
        walk.onImmersive = [this](bool on)
        {
            immersive = on;
            if (auto *w = findParentComponentOfClass<juce::DocumentWindow>())
                w->setFullScreen(on);
            for (auto *c : bar)
                c->setVisible(!on);
            resized();
        };
        address.setText(settings.getValue("host"), false);
        address.setTextToShowWhenEmpty(u("Adresse des Spielers, z. B. 192.168.1.20"), col::faint);
        address.setFont(Assets::get().monoFont(13.0f));
        address.onReturnKey = [this] { join.triggerClick(); };
        join.setTooltip(u("Beim Spieler zuh\xc3\xb6ren (die Adresse steht in seinem Konzert-Fenster)"));
        join.onClick = [this]
        {
            if (proc.concert.role() == ConcertLink::Role::listener)
                proc.concert.stop();
            else if (address.getText().trim().isNotEmpty())
            {
                settings.setValue("host", address.getText().trim());
                proc.concert.startListener(address.getText().trim());
                walk.grabKeyboardFocus();
            }
            refresh();
        };
        {
            const int saved = settings.getIntValue("delay", 250);
            int pick = 2;
            for (int i = 0; i < (int)std::size(kDelays); ++i)
            {
                delay.addItem(juce::String(kDelays[i]) + u(" ms Verz\xc3\xb6gerung"), i + 1);
                if (kDelays[i] == saved)
                    pick = i + 1;
            }
            delay.setSelectedId(pick, juce::dontSendNotification);
            proc.concert.setDelayMs(kDelays[pick - 1]);
        }
        delay.onChange = [this]
        {
            const int ms = kDelays[juce::jlimit(0, (int)std::size(kDelays) - 1, delay.getSelectedId() - 1)];
            proc.concert.setDelayMs(ms);
            settings.setValue("delay", ms);
        };
        delay.setTooltip(u("Wie weit du hinter dem Spieler liegst: Platz f\xc3\xbcr ein ungleichm\xc3\xa4\xc3\x9figes Netz. "
                           "Mehr = sicherer (keine Aussetzer), weniger = n\xc3\xa4her dran. Gilt beim n\xc3\xa4" "chsten Verbinden."));
        delay.setWantsKeyboardFocus(false);
        if (auto *prm = dynamic_cast<juce::AudioParameterChoice *>(proc.apvts.getParameter("output")))
            output.addItemList(prm->choices, 1);
        output.setTooltip(u("Wie du h\xc3\xb6rst: Kopfh\xc3\xb6rer (binaural), Stereo, Lautsprecher \xe2\x80\xa6"));
        output.setWantsKeyboardFocus(false);
        outputAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(proc.apvts, "output", output);
        volume.setSliderStyle(juce::Slider::LinearBar);
        volume.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        volume.setTooltip(u("Lautst\xc3\xa4rke"));
        volume.setWantsKeyboardFocus(false);
        volumeAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, "master", volume);
        audio.setTooltip(u("Ausgabeger\xc3\xa4t, Abtastrate, Puffergr\xc3\xb6\xc3\x9f" "e"));
        audio.setWantsKeyboardFocus(false);
        audio.onClick = [this] { showAudioSettings(); };
        join.setWantsKeyboardFocus(false);
        for (auto *c : bar)
            addAndMakeVisible(c);
        addAndMakeVisible(walk);
        refresh();
        startTimerHz(4);
    }

    void paint(juce::Graphics &g) override
    {
        g.fillAll(col::bg);
        if (immersive)
            return;
        auto &A = Assets::get();
        auto r = getLocalBounds().removeFromTop(kBar).reduced(14, 0);
        {
            // "Rysum" in blackletter, the R rubricated (as the organ's own window)
            const auto f = A.frakturFont(28.0f);
            const int rw = juce::roundToInt(juce::GlyphArrangement::getStringWidth(f, "R"));
            const int yw = juce::roundToInt(juce::GlyphArrangement::getStringWidth(f, "ysum"));
            g.setFont(f);
            g.setColour(col::rubric.brighter(0.2f));
            g.drawText("R", r.removeFromLeft(rw + 1).translated(0, 2), juce::Justification::centredLeft, false);
            g.setColour(col::ink);
            g.drawText("ysum", r.removeFromLeft(yw + 8).translated(0, 2), juce::Justification::centredLeft, false);
            g.setFont(A.sansFont(15.0f));
            g.setColour(col::dim);
            g.drawText("Konzert", r.removeFromLeft(80).translated(0, 3), juce::Justification::centredLeft, false);
        }
        // what is going on, under the line
        auto line = getLocalBounds().removeFromTop(kBar).withTrimmedTop(kBar - 1);
        g.setColour(col::line);
        g.fillRect(line);
        g.setFont(A.monoFont(11.0f));
        g.setColour(col::dim);
        g.drawText(statusNow, statusArea, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (immersive)
        {
            walk.setBounds(r);
            return;
        }
        auto top = r.removeFromTop(kBar).reduced(14, 9);
        top.removeFromLeft(170);
        address.setBounds(top.removeFromLeft(220));
        top.removeFromLeft(6);
        join.setBounds(top.removeFromLeft(96));
        top.removeFromLeft(10);
        delay.setBounds(top.removeFromLeft(170));
        audio.setBounds(top.removeFromRight(78));
        top.removeFromRight(6);
        volume.setBounds(top.removeFromRight(90).reduced(0, 4));
        top.removeFromRight(6);
        output.setBounds(top.removeFromRight(150));
        top.removeFromRight(10);
        statusArea = top.withTrimmedLeft(10);
        walk.setBounds(r);
    }

    void visibilityChanged() override
    {
        if (isShowing())
            walk.grabKeyboardFocus();
    }

private:
    static constexpr int kBar = 46;
    static constexpr int kDelays[]{120, 180, 250, 400, 600, 1000};
    RysumProcessor &proc;
    juce::AudioDeviceManager &devices;
    juce::PropertiesFile &settings;
    WalkView walk;
    juce::TextEditor address;
    juce::TextButton join{u("Zuh\xc3\xb6ren")}, audio{u("Audio \xe2\x80\xa6")};
    juce::Slider volume;
    juce::ComboBox delay;
    juce::ComboBox output;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> outputAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volumeAtt;
    std::vector<juce::Component *> bar{&address, &join, &delay, &output, &volume, &audio};
    juce::Rectangle<int> statusArea;
    juce::String statusNow;
    bool immersive = false;

    void timerCallback() override
    {
        refresh();
    }

    void refresh()
    {
        const bool listening = proc.concert.role() == ConcertLink::Role::listener;
        join.setButtonText(listening ? u("Verlassen") : u("Zuh\xc3\xb6ren"));
        address.setEnabled(!listening);
        juce::String s;
        if (proc.getLibrary() == nullptr)
            s = proc.getLoadStatus();
        else if (listening)
            s = proc.concert.status();
        else
            s = u("Adresse eingeben, Zuh\xc3\xb6ren \xe2\x80\x93 dann gehen: WASD, Maus ziehen, Leertaste, I immersiv");
        if (s != statusNow)
        {
            statusNow = s;
            repaint(statusArea);
        }
    }

    void showAudioSettings()
    {
        auto sel = std::make_unique<juce::AudioDeviceSelectorComponent>(devices, 0, 0, 2, 64, false, false, true, false);
        sel->setSize(520, 380);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned(sel.release());
        o.dialogTitle = u("Audio");
        o.dialogBackgroundColour = col::panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }
};

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
        window = std::make_unique<ListenerWindow>(getApplicationName(), new ListenerMain(*proc, devices, *settings));
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
