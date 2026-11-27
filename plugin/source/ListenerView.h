// The listener's window (the 3D Saal and a line to join a player), shared by the app
// "Rysum Konzert" (ListenerApp.cpp) and the plugin "Rysum Konzert" (KonzertPlugin.cpp).
#pragma once
#include "PluginProcessor.h"
#include "Theme.h"
#include "WalkView.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace rysum::ui::konzert
{
inline juce::String u(const char *s)
{
    return juce::String::fromUTF8(s);
}

inline juce::PropertiesFile::Options settingsOptions()
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
    ListenerMain(RysumProcessor &p, juce::AudioDeviceManager *dm, juce::PropertiesFile &s)
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
        nameField.setText(settings.getValue("name", "Gast"), false);
        nameField.setTextToShowWhenEmpty(u("Dein Name"), col::faint);
        nameField.setFont(Assets::get().monoFont(13.0f));
        nameField.setTooltip(u("So sehen dich die anderen im Saal"));
        proc.concert.setName(nameField.getText());
        nameField.onTextChange = [this]
        {
            proc.concert.setName(nameField.getText());
            settings.setValue("name", nameField.getText().trim());
        };
        nameField.onReturnKey = [this] { walk.grabKeyboardFocus(); };
        address.setText(settings.getValue("host"), false);
        address.setTextToShowWhenEmpty(u("Adresse: 192.168.1.20 oder server/raum"), col::faint);
        address.setTooltip(u("Die Adresse aus dem Konzert-Fenster des Spielers: im Heimnetz seine IP, "
                             "\xc3\xbc" "ber das Internet server[:port]/raum (ein Relay)"));
        password.setPasswordCharacter((juce::juce_wchar)0x2022);
        password.setTextToShowWhenEmpty(u("Passwort"), col::faint);
        password.setFont(Assets::get().monoFont(13.0f));
        password.setTooltip(u("Das Passwort des Konzerts (leer, wenn der Spieler keins hat)"));
        password.onReturnKey = [this] { join.triggerClick(); };
        voice.setSliderStyle(juce::Slider::LinearBar);
        voice.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        voice.setRange(0.0, 2.0);
        voice.setValue(settings.getDoubleValue("voice", 1.0), juce::dontSendNotification);
        proc.concert.setVoiceGain((float)voice.getValue());
        voice.setTooltip(u("Wie laut du die Stimme des Spielers h\xc3\xb6rst"));
        voice.setWantsKeyboardFocus(false);
        voice.onValueChange = [this]
        {
            proc.concert.setVoiceGain((float)voice.getValue());
            settings.setValue("voice", voice.getValue());
        };
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
                proc.concert.setPassword(password.getText());
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
        if (devices == nullptr)
            bar.erase(std::find(bar.begin(), bar.end(), &audio));
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
        nameField.setBounds(top.removeFromLeft(110));
        top.removeFromLeft(6);
        address.setBounds(top.removeFromLeft(200));
        top.removeFromLeft(6);
        password.setBounds(top.removeFromLeft(100));
        top.removeFromLeft(6);
        join.setBounds(top.removeFromLeft(96));
        top.removeFromLeft(10);
        delay.setBounds(top.removeFromLeft(170));
        if (devices != nullptr)
        {
            audio.setBounds(top.removeFromRight(78));
            top.removeFromRight(6);
        }
        volume.setBounds(top.removeFromRight(90).reduced(0, 4));
        top.removeFromRight(6);
        voice.setBounds(top.removeFromRight(60).reduced(0, 4));
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
    juce::AudioDeviceManager *devices; // (none in the plugin: the host has the audio)
    juce::PropertiesFile &settings;
    WalkView walk;
    juce::TextEditor address, nameField, password;
    juce::TextButton join{u("Zuh\xc3\xb6ren")}, audio{u("Audio \xe2\x80\xa6")};
    juce::Slider volume, voice;
    juce::ComboBox delay;
    juce::ComboBox output;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> outputAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volumeAtt;
    std::vector<juce::Component *> bar{&nameField, &address, &password, &join, &delay, &output, &voice, &volume, &audio};
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
        password.setEnabled(!listening);
        juce::String s;
        if (proc.getLibrary() == nullptr)
            s = proc.getLoadStatus();
        else if (listening)
        {
            s = proc.concert.status();
            const int hands = proc.concert.allowedHands();
            if (hands > 0)
                s << (hands == 1 ? u("  \xc2\xb7  du darfst anklopfen") : u("  \xc2\xb7  du darfst die Pfeifen bewegen"));
            if (proc.concert.voiceHeard())
                s << u("  \xc2\xb7  der Spieler spricht");
        }
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
        if (devices == nullptr)
            return;
        auto sel = std::make_unique<juce::AudioDeviceSelectorComponent>(*devices, 0, 0, 2, 64, false, false, true, false);
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

} // namespace rysum::ui::konzert
