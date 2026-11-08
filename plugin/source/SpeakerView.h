#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

namespace rysum::ui
{
/** top view of the loudspeaker layout: loudspeakers, the triangulation and the energy of the decoder */
class SpeakerMap : public juce::Component
{
public:
    explicit SpeakerMap (RysumProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    int selected = 0;
    std::function<void (int)> onSelect;
    std::function<void (int, float, float)> onMove;       // index, az, el
    std::function<void()> onMoveDone;
private:
    juce::Point<float> toScreen (float az, float el) const;
    void fromScreen (juce::Point<float> s, float& az, float& el) const;
    float radius() const { return (float) juce::jmin (getWidth(), getHeight()) * 0.5f / 1.56f; }
    juce::Point<float> centre() const { return getLocalBounds().toFloat().getCentre(); }
    RysumProcessor& proc;
    int dragging = -1;
};

/** one row of the loudspeaker list: number, channel, azimuth, elevation, distance, gain, imaginary */
class SpeakerRow : public juce::Component
{
public:
    SpeakerRow();
    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { if (onSelect) onSelect(); }
    void set (int index, const Speaker& s, bool selected);
    std::function<void()> onSelect;
    std::function<void (int field, const juce::String&)> onEdit;     // 0 channel, 1 az, 2 el, 3 radius, 4 gain
    std::function<void (bool)> onImaginary;
private:
    int index = 0;
    bool sel = false;
    std::array<juce::Label, 5> fields;
    juce::TextButton imag { "imag." };
};

class SpeakerView : public juce::Component
{
public:
    explicit SpeakerView (RysumProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();
private:
    void applyLayout (bool remember = true);    // the edited layout -> processor (rebuilds the decoder)
    void select (int i);
    void syncRows();
    RysumProcessor& proc;
    SpeakerLayout edit;
    int selected = 0, shownVersion = -1;
    SpeakerMap map;
    juce::ComboBox presets, order;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> orderAtt;
    juce::TextButton loadJson { juce::String::fromUTF8 ("IEM-Datei laden \xe2\x80\xa6") }, saveJson { juce::String::fromUTF8 ("Speichern \xe2\x80\xa6") },
                     onButton { "Lautsprecher an" }, distComp { "Abstand ausgleichen" },
                     addBtn { "+ Lautsprecher" }, removeBtn { "Entfernen" }, testBtn { "Test" }, tourBtn { "Rundgang" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> distAtt;
    juce::Viewport listView;
    juce::Component listContent;
    std::vector<std::unique_ptr<SpeakerRow>> rows;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> left, right, headBox, listHead, infoBox;
    double tourNext = 0.0;
};
} // namespace rysum::ui
