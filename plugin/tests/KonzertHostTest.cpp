// Hosts the "Rysum Konzert" VST3 the way a venue's Reaper would: 36 output channels (AmbiX 5),
// joins a player on this computer (RysumUiSnapshot with RYSUM_CONCERT_SERVE=port) and checks that
// the sound comes out on every order. Its window is opened once and written as a picture.
//   RysumKonzertHostTest <Rysum Konzert.vst3> <out folder> [port]
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdlib>
#include <iostream>
#include <windows.h>

namespace
{
/** the window as Windows draws it, plugin's own child window included (PrintWindow) */
juce::Image grabWindow(void *hwnd)
{
    RECT r{};
    GetWindowRect((HWND)hwnd, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return {};
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    auto *old = SelectObject(dc, bmp);
    PrintWindow((HWND)hwnd, dc, 2 /* PW_RENDERFULLCONTENT */);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint32_t> px((size_t)w * (size_t)h);
    GetDIBits(dc, bmp, 0, (UINT)h, px.data(), &bi, DIB_RGB_COLORS);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    juce::Image img(juce::Image::RGB, w, h, false);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.setPixelAt(x, y, juce::Colour(px[(size_t)y * (size_t)w + (size_t)x] | 0xff000000u));
    return img;
}
} // namespace

int main(int argc, char **argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 3)
    {
        std::cout << "usage: RysumKonzertHostTest <plugin.vst3> <out folder> [port]" << std::endl;
        return 2;
    }
    const juce::File plugin(juce::String::fromUTF8(argv[1]));
    const juce::File out(juce::String::fromUTF8(argv[2]));
    const int port = argc > 3 ? juce::String(argv[3]).getIntValue() : 8792;
    out.createDirectory();
    bool passed = true;
    auto check = [&](bool ok, const juce::String &what)
    {
        std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
        passed = passed && ok;
    };

    juce::VST3PluginFormat fmt;
    juce::OwnedArray<juce::PluginDescription> found;
    fmt.findAllTypesForFile(found, plugin.getFullPathName());
    check(found.size() == 1, "the VST3 describes one plugin");
    if (found.isEmpty())
        return 1;
    std::cout << "  plugin: " << found[0]->name << " (" << found[0]->category << "), outputs "
              << found[0]->numOutputChannels << ", inputs " << found[0]->numInputChannels << std::endl;
    check(found[0]->numInputChannels == 0, "it has no inputs");

    // (joins the player as soon as it exists)
    _putenv_s("RYSUM_KONZERT_JOIN", ("127.0.0.1:" + juce::String(port)).toRawUTF8());
    constexpr double sr = 48000.0;
    constexpr int bs = 512;
    juce::String err;
    auto inst = fmt.createInstanceFromDescription(*found[0], sr, bs, err);
    check(inst != nullptr, "the host loads it" + (err.isNotEmpty() ? " (" + err + ")" : juce::String()));
    if (inst == nullptr)
        return 1;
    auto layout = inst->getBusesLayout();
    if (layout.outputBuses.size() > 0)
        layout.outputBuses.getReference(0) = juce::AudioChannelSet::ambisonic(5);
    check(inst->setBusesLayout(layout), "it takes a 36-channel AmbiX 5 output");
    check(inst->getTotalNumOutputChannels() == 36, "36 output channels (" + juce::String(inst->getTotalNumOutputChannels()) + ")");
    juce::String outChoice;
    for (auto *p : inst->getParameters())
        if (p->getName(64) == "Ausgabe")
            outChoice = p->getCurrentValueAsText();
    check(outChoice == "AmbiX 5", "it starts in AmbiX 5 (" + outChoice + ")");

    inst->prepareToPlay(sr, bs);
    // its window, once, on the screen (a VST3 window is the plugin's own native window: the host cannot
    // draw it into a picture, so the bounds are printed for a screen grab; RYSUM_KONZERT_SHOW=seconds)
    if (auto *ed = inst->createEditorIfNeeded())
    {
        juce::DocumentWindow holder("Rysum Konzert (host test)", juce::Colours::black, 0, true);
        holder.setUsingNativeTitleBar(true);
        holder.setContentNonOwned(ed, true);
        holder.setTopLeftPosition(60, 60);
        holder.setVisible(true);
        holder.toFront(true);
        const double show = juce::SystemStats::getEnvironmentVariable("RYSUM_KONZERT_SHOW", "1").getDoubleValue();
        for (int i = 0; i < (int)(show * 20.0); ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
        {
            auto img = grabWindow(holder.getWindowHandle());
            auto f = out.getChildFile("konzert_plugin.png");
            f.deleteFile();
            juce::FileOutputStream os(f);
            juce::PNGImageFormat().writeImageToStream(img, os);
        }
        const auto sb = ed->getScreenBounds();
        std::cout << "  window on screen at " << sb.getX() << " " << sb.getY() << " " << sb.getWidth() << " "
                  << sb.getHeight() << std::endl;
        check(ed->getWidth() >= 800, "its window opens (" + juce::String(ed->getWidth()) + " x " + juce::String(ed->getHeight()) + ")");
        holder.clearContentComponent();
        delete ed;
    }
    else
        check(false, "its window opens");

    // play in real time for a while, as a host does; the host's own MIDI must not play it
    juce::AudioBuffer<float> buf(36, bs);
    std::array<double, 6> energy{}; // per order
    const double seconds = 22.0;
    const auto blocks = (long long)(seconds * sr / bs);
    const double start = juce::Time::getMillisecondCounterHiRes();
    double hostNoteEnergy = 0.0;
    for (long long k = 0; k < blocks; ++k)
    {
        juce::MidiBuffer midi;
        const bool hostTry = k == 0; // (before the player plays anything)
        if (hostTry)
            midi.addEvent(juce::MidiMessage::noteOn(1, 72, (juce::uint8)120), 0);
        buf.clear();
        inst->processBlock(buf, midi);
        if (k < (long long)(0.8 * sr / bs))
            for (int c = 0; c < 36; ++c)
                hostNoteEnergy += buf.getRMSLevel(c, 0, bs);
        for (int c = 0; c < 36; ++c)
        {
            const int order = (int)std::floor(std::sqrt((double)c) + 1e-9);
            const double r = buf.getRMSLevel(c, 0, bs);
            energy[(size_t)order] += r * r;
        }
        const double due = start + (double)(k + 1) * bs / sr * 1000.0;
        while (juce::Time::getMillisecondCounterHiRes() < due)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    }
    check(hostNoteEnergy < 1e-6, "a note from the host does not play (" + juce::String(hostNoteEnergy, 8) + ")");
    juce::String levels;
    for (int o = 0; o <= 5; ++o)
        levels << "order " << o << ": " << juce::String(std::sqrt(energy[(size_t)o] / (double)blocks), 6) << "   ";
    std::cout << "  " << levels << std::endl;
    for (int o = 0; o <= 5; ++o)
        check(energy[(size_t)o] > 1e-6, "the player's sound comes out on order " + juce::String(o));
    inst->releaseResources();
    inst.reset();
    std::cout << (passed ? "KONZERT PLUGIN OK" : "KONZERT PLUGIN FAILED") << std::endl;
    return passed ? 0 : 1;
}
