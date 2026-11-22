#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "RoomModels.h"
#include "RysumBinaryData.h"
#include "libMTSClient.h"

using namespace rysum;

// ---------------------------------------------------------------------------
// background sample loading
// ---------------------------------------------------------------------------
class RysumProcessor::Loader : public juce::Thread
{
public:
    Loader(RysumProcessor &p, juce::File f)
        : juce::Thread("Rysum sample loader"), owner(p), folder(std::move(f))
    {
    }
    ~Loader() override
    {
        stopThread(10000);
    }

    void run() override
    {
        juce::String err;
        auto lib = SampleLibrary::load(folder, err, [this] { return threadShouldExit(); });
        if (threadShouldExit())
            return;
        std::shared_ptr<const SampleLibrary> shared(lib.release());
        juce::MessageManager::callAsync(
            [proc = &owner, shared, err, loadedFolder = folder, alive = owner.aliveFlag]
            {
                if (!alive->load())
                    return;
                proc->onLoaded(shared, err, loadedFolder);
            });
    }

private:
    RysumProcessor &owner;
    juce::File folder;
};

// ---------------------------------------------------------------------------
juce::AudioProcessorValueTreeState::ParameterLayout RysumProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    auto pct = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int) { return String(juce::roundToInt(v * 100)) + " %"; });

    for (int s = 0; s < kNumStops; ++s)
        l.add(std::make_unique<AudioParameterBool>(ParameterID{stopParamId(s), 1},
                                                   prettyLabel(kStops[(size_t)s].label), s == 0));
    for (int s = 0; s < kNumStops; ++s)
    {
        const String name = prettyLabel(kStops[(size_t)s].label);
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{volParamId(s), 1}, name + u8(" Lautst\xc3\xa4rke"),
            NormalisableRange<float>(-48.0f, 12.0f, 0.1f), kDefaultStopVolume[(size_t)s],
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int) { return (v > 0 ? "+" : "") + String(v, 1) + " dB"; })));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{windParamId(s), 1}, name + " Wind",
                                                    NormalisableRange<float>(0, 1), 1.0f, pct));
    }

    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"master", 1}, u8("Lautst\xc3\xa4rke"),
                                                NormalisableRange<float>(-60.0f, 12.0f, 0.1f),
                                                -12.0f,
                                                AudioParameterFloatAttributes().withLabel("dB")));
    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID{"output", 1}, "Ausgabe",
        StringArray{"Stereo", "AmbiX 1", "AmbiX 2", "AmbiX 3", "AmbiX 4", "AmbiX 5", "Binaural",
                    "Lautsprecher", "Transaural (2 LS)"},
        0));
    // loudspeakers: AllRAD decoding of this order to the layout set up on the page "Lautsprecher"
    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID{"spkOrder", 1}, "Lautsprecher-Ordnung",
        StringArray{"1. Ordnung", "2. Ordnung", "3. Ordnung", "4. Ordnung", "5. Ordnung"}, 2));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"spkDistComp", 1},
                                               "Lautsprecher-Abstand ausgleichen", true));
    // binaural: Ambisonics of this order, decoded with HRTFs (FABIAN or an own SOFA file)
    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID{"binOrder", 1}, "Binaural-Ordnung",
        StringArray{"1. Ordnung", "2. Ordnung", "3. Ordnung", "4. Ordnung", "5. Ordnung"}, 2));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"binYaw", 1}, "Kopfdrehung", NormalisableRange<float>(-180.0f, 180.0f, 0.1f),
        0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            {
                return std::abs(v) < 0.05f
                           ? String("geradeaus")
                           : String(roundToInt(std::abs(v))) + juce::String::fromUTF8("\xc2\xb0 ") +
                                 (v > 0 ? "links" : "rechts");
            })));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"binDfEq", 1}, "Diffusfeld-Entzerrung",
                                               true));
    // transaural: the binaural signal over two loudspeakers, symmetric at +-angle (90 = on opposite
    // sides of the room)
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"trAngle", 1}, "Transaural Lautsprecherwinkel",
        NormalisableRange<float>(10.0f, 150.0f, 1.0f), 90.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            {
                return juce::String::fromUTF8("\xc2\xb1") + String(roundToInt(v)) +
                       juce::String::fromUTF8("\xc2\xb0");
            })));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"trAmount", 1},
                                                juce::String::fromUTF8("Transaural \xc3\x9c"
                                                                       "bersprech-Unterdr\xc3\xbc"
                                                                       "ckung"),
                                                NormalisableRange<float>(0, 1), 1.0f, pct));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"trBoost", 1},
        juce::String::fromUTF8("Transaural h\xc3\xb6"
                               "chste Anhebung"),
        NormalisableRange<float>(3.0f, 20.0f, 0.1f), 10.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return String(v, 1) + " dB"; })));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"trSideEq", 1},
                                               "Transaural Seitenklang ausgleichen", true));

    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"click", 1}, "Tastenklick",
                                                NormalisableRange<float>(0, 1), 0.2f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"clickBlend", 1},
                                                "Klick ins Anschlagen",
                                                NormalisableRange<float>(0, 1), 0.45f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"releaseClick", 1}, "Loslassklick",
                                                NormalisableRange<float>(0, 1), 0.2f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"stopPull", 1}, "Registerzug",
                                                NormalisableRange<float>(0, 1), 0.70f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"wind", 1}, "Wind",
                                                NormalisableRange<float>(0, 1), 0.0f, pct));

    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"degAmount", 1}, "Verfall Menge",
                                                NormalisableRange<float>(0, 1), 0.0f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"degGrain", 1}, u8("K\xc3\xb6rnung"),
                                                NormalisableRange<float>(0, 1), 0.0f, pct));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"degBits", 1}, "Bittiefe", NormalisableRange<float>(3.0f, 16.0f, 0.1f), 16.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return String(v, 1) + " bit"; })));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"degRate", 1}, "Abtastrate",
        NormalisableRange<float>(600.0f, 48000.0f, 1.0f, 0.35f), 48000.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            {
                return v >= 1000 ? String(v / 1000.0f, 2) + " kHz" : String(roundToInt(v)) + " Hz";
            })));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"degWobble", 1}, "Windschwankung",
                                                NormalisableRange<float>(0, 1), 0.2f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"degDropouts", 1}, "Aussetzer",
                                                NormalisableRange<float>(0, 1), 0.08f, pct));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"degMode", 1}, "Verfall auf",
                                                 StringArray{"pro Pfeife", "Summe"}, 0));

    auto dbAttr = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int)
        { return v <= -59.9f ? String("aus") : (v > 0 ? "+" : "") + String(v, 1) + " dB"; });
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"irDry", 1}, "Direkt",
                                                NormalisableRange<float>(-60.0f, 6.0f, 0.1f, 2.0f),
                                                0.0f, dbAttr));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"irWet", 1}, "Hall",
                                                NormalisableRange<float>(-60.0f, 12.0f, 0.1f, 2.0f),
                                                -6.0f, dbAttr));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"irFuma", 1}, "IR FuMa", false));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"irN3D", 1}, "IR N3D", false));
    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID{"irOrder", 1}, "Hall-Ordnung",
        StringArray{"1. Ordnung", "2. Ordnung", "3. Ordnung", "4. Ordnung", "5. Ordnung"}, 2));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"irFollow", 1},
                                               u8("Hall folgt den Pfeifen"), true));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"irEnvelop", 1}, u8("Umh\xc3\xbcllung"),
                                                NormalisableRange<float>(0, 1), 0.6f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"irDirect", 1}, "Direktschall der IR",
                                                NormalisableRange<float>(0, 1), 0.2f, pct));

    // distance
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"doppler", 1}, "Doppler", NormalisableRange<float>(0.0f, 4.0f, 0.01f), 1.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            {
                return v < 0.005f ? String("aus")
                                  : String(v, 2) + juce::String::fromUTF8(" \xc3\x97");
            })));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"air", 1}, "Luft",
                                                NormalisableRange<float>(0, 1), 0.5f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"distAmount", 1}, "Entfernung",
                                                NormalisableRange<float>(0, 1), 0.7f, pct));

    // tuning
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"temperament", 1}, "Stimmung",
                                                 temperamentNames(), 0));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"pitch", 1}, "Stimmton", NormalisableRange<float>(-200.0f, 200.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            {
                return "A4 = " +
                       String(kA4 * std::exp2(effectiveStimmtonCents(v) / 1200.0), 1) + " Hz";
            })));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"detune", 1}, "Verstimmung", NormalisableRange<float>(0.0f, 60.0f, 0.1f, 0.6f),
        0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            { return juce::String::fromUTF8("\xc2\xb1") + String(v, 1) + " ct"; })));

    // tremulant
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"tremOn", 1}, "Tremulant", false));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"tremRate", 1}, "Tremulant Tempo",
        NormalisableRange<float>(1.5f, 12.0f, 0.01f, 0.7f), 5.5f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return String(v, 2) + " Hz"; })));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"tremDepth", 1}, "Tremulant Tiefe",
                                                NormalisableRange<float>(0, 1), 0.5f, pct));

    // freeze
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"freeze", 1}, "Freeze", false));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"freezeSmear", 1}, "Verwischen",
                                                NormalisableRange<float>(0, 1), 0.5f, pct));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"freezeRelease", 1}, "Freeze Ausklang",
        NormalisableRange<float>(0.1f, 12.0f, 0.01f, 0.5f), 2.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return String(v, 1) + " s"; })));

    // MIDI / tempo
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"motionSync", 1}, "Bewegung im Tempo",
                                               false));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"midiCC", 1}, "MIDI-CC", true));

    for (int s = 0; s < kNumStops; ++s)
    {
        const String name = prettyLabel(kStops[(size_t)s].label);
        l.add(std::make_unique<AudioParameterChoice>(ParameterID{motionParamId(s), 1},
                                                     name + " Bewegung", motionNames(), 0));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{rateParamId(s), 1}, name + " Geschwindigkeit",
            NormalisableRange<float>(-500.0f, 500.0f, 0.0001f, 0.2f, true), 0.08f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int)
                { return (v > 0 ? "+" : "") + String(v, std::abs(v) < 1.0f ? 3 : 2) + " Hz"; })));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{ellipseParamId(s), 1}, name + " Ellipse",
            NormalisableRange<float>(0.0f, 0.9f, 0.001f), 0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{depthParamId(s), 1}, name + " Tiefe",
            NormalisableRange<float>(0.0f, 180.0f, 0.1f), 40.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int)
                {
                    return String(roundToInt(v)) + juce::String(juce::CharPointer_UTF8("\xc2\xb0"));
                })));
    }

    // ---- modulation
    auto hz = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int) { return String(v, v < 1.0f ? 3 : 2) + " Hz"; });
    auto secs = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int)
        { return v < 1.0f ? String(roundToInt(v * 1000.0f)) + " ms" : String(v, 2) + " s"; });
    for (int i = 0; i < kNumLfos; ++i)
    {
        const String n = "LFO " + String(i + 1);
        const String id = "lfo" + String(i + 1);
        const float rates[]{0.25f, 0.5f, 2.0f, 0.1f};
        l.add(std::make_unique<AudioParameterChoice>(ParameterID{id + "Shape", 1}, n + " Form",
                                                     lfoShapeNames(),
                                                     i == 3 ? (int)shSmooth : (int)shSine));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{id + "Rate", 1}, n + " Tempo",
            NormalisableRange<float>(0.01f, 20.0f, 0.001f, 0.3f), rates[i], hz));
        l.add(
            std::make_unique<AudioParameterBool>(ParameterID{id + "Sync", 1}, n + " Sync", false));
        l.add(std::make_unique<AudioParameterChoice>(ParameterID{id + "Div", 1}, n + " Notenwert",
                                                     lfoDivNames(), 2));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{id + "Spread", 1}, n + " Versatz",
                                                    NormalisableRange<float>(0, 1), 0.0f, pct));
        l.add(std::make_unique<AudioParameterChoice>(ParameterID{id + "SpreadMode", 1},
                                                     n + " Versatz-Art",
                                                     StringArray{"Tastatur", "Zufall"}, 0));
    }
    for (int j = 0; j < kNumEnvs; ++j)
    {
        const String n = String::fromUTF8("H\xc3\xbcllkurve ") + String(j + 1);
        const String id = "env" + String(j + 1);
        l.add(std::make_unique<AudioParameterChoice>(ParameterID{id + "Mode", 1}, n + " Modus",
                                                     StringArray{"Pro Pfeife", "Global"}, 0));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{id + "Delay", 1}, n + String::fromUTF8(" Verz\xc3\xb6gerung"),
            NormalisableRange<float>(0.0f, 10.0f, 0.001f, 0.35f), 0.0f, secs));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{id + "Attack", 1}, n + " Anstieg",
            NormalisableRange<float>(0.001f, 20.0f, 0.001f, 0.3f), j == 0 ? 0.3f : 3.0f, secs));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{id + "Decay", 1}, n + " Abfall",
            NormalisableRange<float>(0.001f, 20.0f, 0.001f, 0.3f), j == 0 ? 1.0f : 4.0f, secs));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{id + "Sustain", 1}, n + " Halten",
                                                    NormalisableRange<float>(0, 1),
                                                    j == 0 ? 0.6f : 0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{id + "Release", 1}, n + " Ausklang",
            NormalisableRange<float>(0.001f, 20.0f, 0.001f, 0.3f), j == 0 ? 1.5f : 3.0f, secs));
    }
    for (int r = 0; r < kNumRoutes; ++r)
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"mod" + String(r + 1) + "Amt", 1}, "Modulation " + String(r + 1) + " Menge",
            NormalisableRange<float>(-1.0f, 1.0f, 0.001f), 0.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int)
                { return (v > 0.0005f ? "+" : "") + String(roundToInt(v * 100.0f)) + " %"; })));

    // ---- effects (page "Effekte"); all off by default, so older patches sound the same
    auto deg = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int) { return String(roundToInt(v)) + String::fromUTF8("\xc2\xb0"); });
    auto degS = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int) { return String(roundToInt(v)) + String::fromUTF8("\xc2\xb0/s"); });
    auto sdeg = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int)
        { return (v > 0.5f ? "+" : "") + String(roundToInt(v)) + String::fromUTF8("\xc2\xb0"); });
    auto cnt = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int) { return String(roundToInt(v)); });
    auto ms = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int) { return String(v, v < 1.0f ? 2 : 1) + " ms"; });
    auto times = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int) { return String(v, 1) + String::fromUTF8(" \xc3\x97"); });
    auto shz = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int)
        {
            return std::abs(v) < 0.0005f ? String("steht")
                                         : (v > 0 ? "+" : "") + String(v, 2) + " Hz";
        });
    auto fhz = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int)
        { return v >= 1000 ? String(v / 1000.0f, 1) + " kHz" : String(roundToInt(v)) + " Hz"; });
    auto F = [&](const char *id, const String &name, NormalisableRange<float> r, float def,
                 AudioParameterFloatAttributes a)
    { l.add(std::make_unique<AudioParameterFloat>(ParameterID{id, 1}, name, r, def, a)); };
    const String ae = String::fromUTF8("\xc3\xa4"), oe = String::fromUTF8("\xc3\xb6"),
                 ue = String::fromUTF8("\xc3\xbc");

    F("fxCustLevel", "Eigene Ger" + ae + "usche", NormalisableRange<float>(0, 1), 0.0f, pct);
    F("fxCustDown", "Eigene: beim Anschlag", NormalisableRange<float>(0, 1), 1.0f, pct);
    F("fxCustUp", "Eigene: beim Loslassen", NormalisableRange<float>(0, 1), 0.0f, pct);
    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID{"fxCustPlace", 1}, "Eigene: Ort",
        StringArray{"an der Pfeife", "irgendwo im Raum", "am Spieltisch", "kreisend"}, 0));
    F("fxCustPitch", "Eigene: Tonh" + oe + "he folgt", NormalisableRange<float>(0, 1), 0.0f, pct);
    F("fxCustScatter", "Eigene: Streuung", NormalisableRange<float>(0, 1), 0.0f, pct);
    F("fxCustWander", "Eigene: Wandern", NormalisableRange<float>(0, 360, 0.1f, 0.5f), 0.0f, degS);

    F("fxPathLevel", "Trakturweg", NormalisableRange<float>(0, 1), 0.0f, pct);
    F("fxPathTime", "Trakturweg Laufzeit", NormalisableRange<float>(0.01f, 0.6f, 0.001f, 0.5f),
      0.12f, secs);
    F("fxPathTicks", "Trakturweg Schritte", NormalisableRange<float>(2, 16, 1), 6, cnt);
    F("fxPathSpread", "Trakturweg Streuung", NormalisableRange<float>(0, 90, 0.1f), 12.0f, deg);
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"fxPathWait", 1},
                                               "Pfeifen warten auf die Traktur", false));

    F("fxCrackLevel", "Knistern", NormalisableRange<float>(0, 1), 0.0f, pct);
    F("fxCrackRate", "Knistern Dichte", NormalisableRange<float>(0.2f, 80.0f, 0.01f, 0.4f), 6.0f,
      hz);
    F("fxCrackLen", "Knistern L" + ae + "nge", NormalisableRange<float>(0.1f, 8.0f, 0.01f, 0.5f),
      1.2f, ms);

    F("fxLeakLevel", "Windlecks", NormalisableRange<float>(0, 1), 0.0f, pct);
    F("fxLeakCount", "Windlecks Anzahl", NormalisableRange<float>(1, 8, 1), 3, cnt);
    F("fxLeakFreq", "Windlecks Tonh" + oe + "he",
      NormalisableRange<float>(800.0f, 12000.0f, 1.0f, 0.4f), 4000.0f, fhz);
    F("fxLeakWander", "Windlecks Wandern", NormalisableRange<float>(0, 90, 0.1f, 0.6f), 0.0f, degS);
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"fxWindRound", 1},
                                               "Geb" + ae + "se ringsum", false));

    F("fxPartSpread", "Obert" + oe + "ne verteilen", NormalisableRange<float>(0, 180, 0.1f), 0.0f,
      deg);
    F("fxPartSplit", "Obert" + oe + "ne ab", NormalisableRange<float>(1.5f, 12.0f, 0.01f, 0.6f),
      3.0f, times);
    F("fxPartTurn", "Obert" + oe + "ne kreisen",
      NormalisableRange<float>(-2.0f, 2.0f, 0.001f, 0.4f, true), 0.0f, shz);

    F("fxEchoLevel", "Echos", NormalisableRange<float>(0, 1), 0.0f, pct);
    F("fxEchoTime", "Echo Zeit", NormalisableRange<float>(0.03f, 2.0f, 0.001f, 0.5f), 0.35f, secs);
    l.add(
        std::make_unique<AudioParameterBool>(ParameterID{"fxEchoSync", 1}, "Echo im Tempo", false));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"fxEchoDiv", 1}, "Echo Notenwert",
                                                 lfoDivNames(), 4));
    F("fxEchoRepeats", "Echo Wiederholungen", NormalisableRange<float>(1, 8, 1), 4, cnt);
    F("fxEchoFeedback", "Echo Abnahme", NormalisableRange<float>(0.0f, 0.95f), 0.6f, pct);
    F("fxEchoTurn", "Echo Drehung", NormalisableRange<float>(-180, 180, 0.1f), 90.0f, sdeg);
    F("fxEchoTilt", "Echo Neigung", NormalisableRange<float>(-45, 45, 0.1f), 0.0f, sdeg);
    F("fxEchoDamp", "Echo D" + ae + "mpfung", NormalisableRange<float>(0, 1), 0.3f, pct);

    // ---- simulated room ("Saal"): off by default
    auto metres = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int) { return String(v, v < 10.0f ? 1 : 0) + " m"; });
    auto dbOff = AudioParameterFloatAttributes().withStringFromValueFunction(
        [](float v, int)
        { return v <= -59.9f ? String("aus") : (v > 0 ? "+" : "") + String(v, 1) + " dB"; });
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"simOn", 1}, "Saal-Simulation", false));
    F("simW", "Saal Breite", NormalisableRange<float>(2.0f, 40.0f, 0.01f, 0.6f), 14.0f, metres);
    F("simL", ("Saal L" + ae + "nge"), NormalisableRange<float>(2.0f, 100.0f, 0.01f, 0.5f), 28.0f,
      metres);
    F("simH", ("Saal H" + oe + "he"), NormalisableRange<float>(2.0f, 40.0f, 0.01f, 0.6f), 11.0f,
      metres);
    F("simSize", ("Saal Gr" + oe + String::fromUTF8("\xc3\x9f") + "e"),
      NormalisableRange<float>(0.25f, 3.0f, 0.001f, 0.5f), 1.0f, times);
    F("simLx", ("Saal H" + oe + "rer quer"), NormalisableRange<float>(0, 1), 0.5f, pct);
    F("simLy", ("Saal H" + oe + "rer l" + ae + "ngs"), NormalisableRange<float>(0, 1), 0.4f, pct);
    F("simAbs", ("Saal Absorption W" + ae + "nde"), NormalisableRange<float>(0, 1, 0.001f, 0.5f),
      0.06f, pct);
    F("simAbsFloor", "Saal Absorption Boden/Decke", NormalisableRange<float>(0, 1, 0.001f, 0.5f),
      0.2f, pct);
    F("simHigh", ("Saal H" + oe + "hend" + ae + "mpfung"), NormalisableRange<float>(0, 1), 0.15f,
      pct);
    F("simScatter", "Saal Streuung", NormalisableRange<float>(0, 1), 0.3f, pct);
    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID{"simOrder", 1}, "Saal Reflexionen",
        StringArray{"bis 1. Ordnung", "bis 2. Ordnung", "bis 3. Ordnung"}, 1));
    F("simEr", ("Saal fr" + ue + "he Reflexionen"),
      NormalisableRange<float>(-60.0f, 12.0f, 0.1f, 2.0f), 0.0f, dbOff);
    F("simTail", "Saal Nachhall", NormalisableRange<float>(-60.0f, 12.0f, 0.1f, 2.0f), 0.0f, dbOff);
    // rooms 2..4: every stop (and the input) can sound in its own room; all start in room 1, so
    // older projects sound the same
    {
        struct Def
        {
            float w, l, h, abs, absF, high, scat;
        };
        const Def defs[]{{8.0f, 14.0f, 7.0f, 0.05f, 0.12f, 0.4f, 0.4f},
                         {30.0f, 90.0f, 32.0f, 0.04f, 0.10f, 0.35f, 0.5f},
                         {4.0f, 5.0f, 3.0f, 0.15f, 0.25f, 0.5f, 0.3f}};
        for (int r = 1; r < rysum::kNumRooms; ++r)
        {
            const auto &d = defs[r - 1];
            const String pre = "Saal " + String(r + 1) + " ";
            F(roomParamId(r, "W").toRawUTF8(), pre + "Breite",
              NormalisableRange<float>(2.0f, 40.0f, 0.01f, 0.6f), d.w, metres);
            F(roomParamId(r, "L").toRawUTF8(), pre + "L" + ae + "nge",
              NormalisableRange<float>(2.0f, 100.0f, 0.01f, 0.5f), d.l, metres);
            F(roomParamId(r, "H").toRawUTF8(), pre + "H" + oe + "he",
              NormalisableRange<float>(2.0f, 40.0f, 0.01f, 0.6f), d.h, metres);
            F(roomParamId(r, "Size").toRawUTF8(),
              pre + "Gr" + oe + String::fromUTF8("\xc3\x9f") + "e",
              NormalisableRange<float>(0.25f, 3.0f, 0.001f, 0.5f), 1.0f, times);
            F(roomParamId(r, "Lx").toRawUTF8(), pre + "H" + oe + "rer quer",
              NormalisableRange<float>(0, 1), 0.5f, pct);
            F(roomParamId(r, "Ly").toRawUTF8(), pre + "H" + oe + "rer l" + ae + "ngs",
              NormalisableRange<float>(0, 1), 0.4f, pct);
            F(roomParamId(r, "Abs").toRawUTF8(), pre + "Absorption W" + ae + "nde",
              NormalisableRange<float>(0, 1, 0.001f, 0.5f), d.abs, pct);
            F(roomParamId(r, "AbsFloor").toRawUTF8(), pre + "Absorption Boden/Decke",
              NormalisableRange<float>(0, 1, 0.001f, 0.5f), d.absF, pct);
            F(roomParamId(r, "High").toRawUTF8(), pre + "H" + oe + "hend" + ae + "mpfung",
              NormalisableRange<float>(0, 1), d.high, pct);
            F(roomParamId(r, "Scatter").toRawUTF8(), pre + "Streuung",
              NormalisableRange<float>(0, 1), d.scat, pct);
        }
        const StringArray roomNames{"Saal 1", "Saal 2", "Saal 3", "Saal 4"};
        for (int st = 0; st < kNumStops; ++st)
            l.add(std::make_unique<AudioParameterChoice>(
                ParameterID{stopRoomParamId(st), 1},
                prettyLabel(kStops[(size_t)st].label) + " im Saal", roomNames, 0));
        l.add(std::make_unique<AudioParameterChoice>(ParameterID{"simRoomIn", 1}, "Eingang im Saal",
                                                     roomNames, 0));
    }

    // ---- wind model of the stops (page "Klang"); classic by default, so older patches sound the
    // same
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"windModel", 1}, "Windmodell",
                                                 StringArray{"klassisch", "instabil"}, 0));
    // late reverb of the simulated room: convolution (default) or the feedback delay network
    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID{"simTailModel", 1}, "Saal Nachhall-Modell", StringArray{"Faltung", "FDN"}, 0));
    // MTS-ESP: held notes follow a changing tuning (or keep the pitch they started with)
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"mtsContinuous", 1},
                                               u8("MTS: gehaltene T\xc3\xb6ne folgen"), true));
    // Lucier mode (off by default): the output passes through the room again and again
    // scenes: where between the stored scenes A..D the sound is (0 = A .. 3 = D)
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"sceneMorph", 1}, "Szene", NormalisableRange<float>(0.0f, 3.0f, 0.001f), 0.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction(
                [](float v, int)
                {
                    const char *n[]{"A", "B", "C", "D"};
                    const int i = juce::jlimit(0, 3, (int)std::floor(v + 1.0e-4f));
                    const float t = v - (float)i;
                    if (t < 0.005f || i >= 3)
                        return String(n[i]);
                    if (t > 0.995f)
                        return String(n[i + 1]);
                    return String(n[i]) + String::fromUTF8(" \xe2\x86\x92 ") + n[i + 1] + " " +
                           String(roundToInt(t * 100.0f)) + " %";
                })
            .withValueFromStringFunction(
                [](const String &t)
                {
                    const auto c = t.trim().toUpperCase();
                    if (c.length() == 1 && c[0] >= 'A' && c[0] <= 'D')
                        return (float)(c[0] - 'A');
                    return t.getFloatValue();
                })));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"lucOn", 1}, "Lucier an", false));
    // "Stueck": record, then the recording goes through the room again and again by itself
    // (Lucier's piece); "Live": everything that sounds goes through n passes at once
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"lucMode", 1}, "Lucier Art",
                                                 StringArray{String::fromUTF8("St\xc3\xbc"
                                                                              "ck (Aufnahme)"),
                                                             "Live"},
                                                 0));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lucGap", 1}, "Lucier Pause",
        NormalisableRange<float>(0.0f, 10.0f, 0.01f, 0.6f), 1.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return String(v, 1) + " s"; })
            .withValueFromStringFunction([](const String &t) { return t.getFloatValue(); })));
    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID{"lucRecSrc", 1}, "Lucier Aufnahme von",
        StringArray{"alles (Orgel + Eingang)", "nur Eingang"}, 0));
    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID{"lucSource", 1}, "Lucier Raum",
        StringArray{"Saal (simuliert)", "Impulsantwort (Seite Raum)"}, 0));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lucGen", 1}, "Lucier Generation",
        NormalisableRange<float>(1.0f, 40.0f, 0.01f, 0.6f), 6.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return String(v, v < 10.0f ? 2 : 1); })
            .withValueFromStringFunction([](const String &t) { return t.getFloatValue(); })));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lucGrow", 1}, "Lucier Wachsen",
        NormalisableRange<float>(0.0f, 20.0f, 0.01f, 0.5f), 0.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction(
                [](float v, int) { return v < 0.005f ? String("steht") : String(v, 2) + " / min"; })
            .withValueFromStringFunction([](const String &t) { return t.getFloatValue(); })));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lucTime", 1}, "Lucier Nachklingen",
        NormalisableRange<float>(0.03f, 1.5f, 0.001f, 0.5f), 0.3f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return String(v, 2) + " s"; })
            .withValueFromStringFunction([](const String &t) { return t.getFloatValue(); })));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lucMix", 1}, "Lucier Anteil", NormalisableRange<float>(0.0f, 1.0f), 0.5f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int)
                                         { return String(roundToInt(v * 100.0f)) + " %"; })
            .withValueFromStringFunction([](const String &t)
                                         { return t.getFloatValue() / 100.0f; })));
    // MPE (off by default): note channels bend, press and slide their own notes
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"mpe", 1}, "MPE", false));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"mpeBend", 1}, "MPE Bend-Bereich", NormalisableRange<float>(1.0f, 96.0f, 1.0f),
        48.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return String(roundToInt(v)) + " HT"; })
            .withValueFromStringFunction([](const String &t) { return t.getFloatValue(); })));

    // notes to single pipes: every note only on one of the pulled stops (off by default: an organ)
    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID{"noteSpread", 1}, String::fromUTF8("T\xc3\xb6ne verteilen"),
        StringArray{String::fromUTF8("aus \xe2\x80\x93 alle gezogenen Register"), "der Reihe nach",
                    String::fromUTF8("nach Lage (tief \xe2\x86\x92 links)"),
                    String::fromUTF8("zuf\xc3\xa4llig")},
        0));

    // ---- audio input ("Eingang", page "Raum"): live sound placed and moved like a pipe. In the
    // instrument off by default (older projects / patches sound the same); in the effect version
    // "Rysum FX" the input is the point, so on.
#if RYSUM_FX
    const bool inputDefault = true;
#else
    const bool inputDefault = false;
#endif
    {
        auto dbIn =
            AudioParameterFloatAttributes()
                .withStringFromValueFunction(
                    [](float v, int)
                    {
                        return v <= -59.9f ? String("aus")
                                           : (v > 0 ? "+" : "") + String(v, 1) + " dB";
                    })
                .withValueFromStringFunction([](const String &t) { return t.getFloatValue(); });
        auto degIn =
            AudioParameterFloatAttributes()
                .withStringFromValueFunction(
                    [](float v, int)
                    {
                        return (v > 0.5f ? "+" : "") + String(roundToInt(v)) +
                               String::fromUTF8("\xc2\xb0");
                    })
                .withValueFromStringFunction([](const String &t) { return t.getFloatValue(); });
        auto spinIn =
            AudioParameterFloatAttributes()
                .withStringFromValueFunction(
                    [](float v, int)
                    {
                        return std::abs(v) < 0.05f
                                   ? String("steht")
                                   : (v > 0 ? "+" : "") + String(v, std::abs(v) < 10.0f ? 1 : 0) +
                                         String::fromUTF8("\xc2\xb0/s");
                    })
                .withValueFromStringFunction([](const String &t) { return t.getFloatValue(); });
        l.add(std::make_unique<AudioParameterBool>(ParameterID{"inpOn", 1}, "Eingang an",
                                                   inputDefault));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"inpLevel", 1}, "Eingang Pegel",
                                                    NormalisableRange<float>(-60.0f, 12.0f, 0.1f),
                                                    0.0f, dbIn));
        l.add(std::make_unique<AudioParameterChoice>(ParameterID{"inpMode", 1},
                                                     String::fromUTF8("Eingang Kan\xc3\xa4le"),
                                                     StringArray{"Mono", "Stereo"}, 1));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"inpAz", 1}, "Eingang Richtung",
                                                    NormalisableRange<float>(-180.0f, 180.0f, 0.1f),
                                                    0.0f, degIn));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"inpEl", 1}, String::fromUTF8("Eingang H\xc3\xb6he"),
            NormalisableRange<float>(-15.0f, 90.0f, 0.1f), 10.0f, degIn));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"inpDist", 1}, "Eingang Abstand",
            NormalisableRange<float>(0.5f, 40.0f, 0.01f, 0.4f), 4.0f,
            AudioParameterFloatAttributes()
                .withStringFromValueFunction([](float v, int)
                                             { return String(v, v < 10.0f ? 1 : 0) + " m"; })
                .withValueFromStringFunction([](const String &t) { return t.getFloatValue(); })));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"inpWidth", 1}, "Eingang Breite",
                                                    NormalisableRange<float>(0.0f, 180.0f, 0.1f),
                                                    60.0f, degIn));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"inpSpin", 1}, "Eingang Kreisen",
            NormalisableRange<float>(-360.0f, 360.0f, 0.01f, 0.4f, true), 0.0f, spinIn));
        StringArray follow{"eigener Ort"};
        for (int s = 0; s < kNumStops; ++s)
            follow.add(String::fromUTF8("auf ") + prettyLabel(kStops[(size_t)s].label));
        l.add(std::make_unique<AudioParameterChoice>(ParameterID{"inpFollow", 1}, "Eingang Ort",
                                                     follow, 0));
        l.add(std::make_unique<AudioParameterBool>(ParameterID{"inpGate", 1},
                                                   String::fromUTF8("Eingang: Tasten \xc3\xb6"
                                                                    "ffnen"),
                                                   false));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"inpRelease", 1}, "Eingang Ausklang",
            NormalisableRange<float>(0.01f, 4.0f, 0.001f, 0.4f), 0.3f,
            AudioParameterFloatAttributes()
                .withStringFromValueFunction(
                    [](float v, int)
                    {
                        return v < 1.0f ? String(roundToInt(v * 1000.0f)) + " ms"
                                        : String(v, 2) + " s";
                    })
                .withValueFromStringFunction([](const String &t) { return t.getFloatValue(); })));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"inpSend", 1}, "Eingang Hall-Anteil",
            NormalisableRange<float>(-60.0f, 12.0f, 0.1f), 0.0f, dbIn));
    }

    // ---- floor plans of the simulated rooms (page "Saal"): shoebox by default, so older projects
    // sound the same
    for (int r = 0; r < rysum::kNumRooms; ++r)
    {
        const String pre = r == 0 ? String("Saal ") : "Saal " + String(r + 1) + " ";
        l.add(std::make_unique<AudioParameterChoice>(ParameterID{roomParamId(r, "Shape"), 1},
                                                     pre + "Grundriss", roomShapeNames(), 0));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{roomParamId(r, "ShapeAmt"), 1}, pre + "Grundriss Form",
            NormalisableRange<float>(0.0f, 1.0f), 0.5f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int) { return String(juce::roundToInt(v * 100)) + " %"; })));
    }

    // ---- shape of the motion per stop (page "Raum"): ellipse direction / tilt / form, drawn path,
    // hard jumps
    for (int s = 0; s < kNumStops; ++s)
    {
        const String name = prettyLabel(kStops[(size_t)s].label);
        auto sdeg2 = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            {
                return (v > 0.5f ? "+" : "") + String(roundToInt(v)) + String::fromUTF8("\xc2\xb0");
            });
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{ellDirParamId(s), 1}, name + " Ellipse Richtung",
            NormalisableRange<float>(-180.0f, 180.0f, 0.1f), 0.0f, sdeg2));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{ellTiltParamId(s), 1}, name + " Ellipse Neigung",
            NormalisableRange<float>(-60.0f, 60.0f, 0.1f), 0.0f, sdeg2));
        l.add(std::make_unique<AudioParameterChoice>(
            ParameterID{ellFormParamId(s), 1}, name + " Ellipse Form", ellipseFormNames(), 0));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{pathSpreadParamId(s), 1}, name + " Verteilung auf der Bahn",
            NormalisableRange<float>(0.0f, 1.0f), 1.0f, pct));
        l.add(std::make_unique<AudioParameterBool>(ParameterID{jumpHardParamId(s), 1},
                                                   name + u8(" harte Spr\xc3\xbcnge"), false));
    }

    // ---- "Raumverfall": degradation of the spatial image (Spieltisch, Verfall · Raum); everything
    // off by default
    {
        auto secs =
            AudioParameterFloatAttributes()
                .withStringFromValueFunction(
                    [](float v, int)
                    {
                        return v < 0.05f   ? String("aus")
                               : v < 60.0f ? String(v, 1) + " s"
                                           : String(v / 60.0f, 1) + " min";
                    })
                .withValueFromStringFunction([](const String &t) { return t.getFloatValue(); });
        auto gridTxt = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            {
                const int n = rysum::gridCount(v);
                return n == 0 ? String("aus") : String(n) + " Richtungen";
            });
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"spcErode", 1}, "Raum abtragen",
                                                    NormalisableRange<float>(0, 1), 0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"spcWeather", 1}, "Raum verwittern",
            NormalisableRange<float>(0.0f, 600.0f, 0.01f, 0.35f), 0.0f, secs));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"spcGrid", 1}, "Richtungsraster",
                                                    NormalisableRange<float>(0, 1), 0.0f, gridTxt));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"spcBits", 1}, "Raum-Bittiefe", NormalisableRange<float>(2.0f, 16.0f, 0.1f),
            16.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int) { return v >= 15.95f ? String("aus") : String(v, 1) + " bit"; })));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"spcRate", 1}, "Raum-Abtastrate",
            NormalisableRange<float>(200.0f, 48000.0f, 1.0f, 0.35f), 48000.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int)
                {
                    return v >= 47999.0f ? String("aus")
                           : v >= 1000   ? String(v / 1000.0f, 2) + " kHz"
                                         : String(roundToInt(v)) + " Hz";
                })));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"spcDrops", 1}, "Raum-Aussetzer",
                                                    NormalisableRange<float>(0, 1), 0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"spcAlias", 1}, "Raum-Aliasing",
                                                    NormalisableRange<float>(0, 1), 0.0f, pct));
    }
    // ---- the sound of the pipe per stop (Raum page, tab "Pfeife"): neutral by default
    for (int s = 0; s < kNumStops; ++s)
    {
        const String name = prettyLabel(kStops[(size_t)s].label) + " ";
        auto signedPct = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            {
                const int k = roundToInt(v * 100);
                return (k > 0 ? "+" : "") + String(k) + " %";
            });
        auto hzA = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            {
                return v >= 1000 ? String(v / 1000.0f, 1) + " kHz" : String(roundToInt(v)) + " Hz";
            });
        auto dbA = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return (v > 0.05f ? "+" : "") + String(v, 1) + " dB"; });
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{pipeSndParamId("Att", s), 1}, name + "Ansprache",
            NormalisableRange<float>(-1, 1), 0.0f, signedPct));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{pipeSndParamId("Sharp", s), 1},
            name + String::fromUTF8("Ansprache Sch\xc3\xa4rfe"), NormalisableRange<float>(0, 1),
            0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{pipeSndParamId("Iso", s), 1},
                                                    name + "Nur Ansprache",
                                                    NormalisableRange<float>(0, 1), 0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{pipeSndParamId("Rep", s), 1},
                                                    name + "Ansprache wiederholen",
                                                    NormalisableRange<float>(0, 1), 0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{pipeSndParamId("Puff", s), 1},
                                                    name + "Abriss", NormalisableRange<float>(0, 1),
                                                    0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{pipeSndParamId("HiFreq", s), 1},
            name + String::fromUTF8("Obert\xc3\xb6ne ab"),
            NormalisableRange<float>(500.0f, 16000.0f, 1.0f, 0.4f), 4000.0f, hzA));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{pipeSndParamId("HiGain", s), 1},
            name + String::fromUTF8("Obert\xc3\xb6ne Pegel"),
            NormalisableRange<float>(-24.0f, 18.0f, 0.1f), 0.0f, dbA));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{pipeSndParamId("HiShape", s), 1},
            name + String::fromUTF8("Obert\xc3\xb6ne Anschlag"), NormalisableRange<float>(-1, 1),
            0.0f, signedPct));
    }
    // ---- air inversion (Klang), resonated clicks (Spieltisch, Traktur · Resonanz), glitches
    // (Spieltisch, Verfall · Glitch)
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"airInvert", 1}, "Luft umkehren",
                                                NormalisableRange<float>(0, 1), 0.0f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"clickRes", 1}, "Klick-Resonanz",
                                                NormalisableRange<float>(0, 1), 0.0f, pct));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"clickResDecay", 1}, "Klick-Resonanz Nachklang",
        NormalisableRange<float>(0.02f, 3.0f, 0.001f, 0.4f), 0.4f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            { return v < 1.0f ? String(roundToInt(v * 1000)) + " ms" : String(v, 2) + " s"; })));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"clickResBright", 1},
                                                "Klick-Resonanz Helligkeit",
                                                NormalisableRange<float>(0, 1), 0.5f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"degStutter", 1}, "Glitch Stottern",
                                                NormalisableRange<float>(0, 1), 0.0f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"degSplice", 1}, "Glitch Bandschnitt",
                                                NormalisableRange<float>(0, 1), 0.0f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"degZero", 1}, "Glitch harte Kanten",
                                                NormalisableRange<float>(0, 1), 0.0f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"degClip", 1}, "Glitch Clipping",
                                                NormalisableRange<float>(0, 1), 0.0f, pct));
    // ---- the tuning in space (Effekte, "Stimmung & Belichtung"): beats from ..., difference
    // tones; Eis (Saal);
    //      Doppelbelichtung, Belichtung (Effekte) - all off by default
    {
        auto dbOff = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            { return v <= -59.9f ? String("aus") : (v > 0 ? "+" : "") + String(v, 1) + " dB"; });
        auto secsA = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            { return v < 1.0f ? String(roundToInt(v * 1000.0f)) + " ms" : String(v, 2) + " s"; });
        auto degA = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            {
                return (v > 0.5f ? "+" : "") + String(roundToInt(v)) + String::fromUTF8("\xc2\xb0");
            });
        l.add(std::make_unique<AudioParameterChoice>(
            ParameterID{"fxBeatIdeal", 1}, "Schwebungen aus",
            StringArray{"der Stimmung (ideal)", "den Pfeifen (wie aufgenommen)"}, 0));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"fxGhostLevel", 1}, String::fromUTF8("Differenzt\xc3\xb6ne"),
            NormalisableRange<float>(-60.0f, 12.0f, 0.1f, 2.0f), -60.0f, dbOff));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"fxGhostPlace", 1},
                                                    String::fromUTF8("Differenzt\xc3\xb6ne Ort"),
                                                    NormalisableRange<float>(0, 1), 0.3f, pct));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"fxGhostCons", 1}, String::fromUTF8("Differenzt\xc3\xb6ne nur Konsonanzen"),
            NormalisableRange<float>(0, 1), 0.5f, pct));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"simIce", 1}, "Eis (Reflexionen)",
                                                    NormalisableRange<float>(0, 1), 0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"simIceLen", 1},
                                                    String::fromUTF8("Eis L\xc3\xa4nge"),
                                                    NormalisableRange<float>(0, 1), 0.5f, pct));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"fxDexMix", 1}, "Doppelbelichtung",
                                                    NormalisableRange<float>(0, 1), 0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"fxDexTime", 1}, "Doppelbelichtung Abstand",
            NormalisableRange<float>(0.05f, 8.0f, 0.001f, 0.45f), 2.0f, secsA));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"fxDexTurn", 1}, "Doppelbelichtung Drehung",
            NormalisableRange<float>(-180.0f, 180.0f, 0.1f), 0.0f, degA));
        l.add(std::make_unique<AudioParameterChoice>(
            ParameterID{"fxDexMirror", 1}, "Doppelbelichtung Spiegeln",
            StringArray{"nicht gespiegelt", "gespiegelt links / rechts", "gespiegelt vorn / hinten",
                        "gespiegelt oben / unten"},
            0));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"fxDexFade", 1},
                                                    "Doppelbelichtung Verblassen",
                                                    NormalisableRange<float>(0, 1), 0.3f, pct));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"fxDexAgain", 1}, "Doppelbelichtung mehrfach",
            NormalisableRange<float>(0.0f, 0.85f), 0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"fxExpoMix", 1}, "Belichtung",
                                                    NormalisableRange<float>(0, 1), 0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"fxExpoTime", 1}, "Belichtungszeit",
            NormalisableRange<float>(0.2f, 60.0f, 0.001f, 0.4f), 4.0f, secsA));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"fxExpoBright", 1},
                                                    "Belichtung Helligkeit",
                                                    NormalisableRange<float>(0, 1), 0.5f, pct));
    }

    // ---- "Körnerwolke" per stop (Raum page, tab "Wolke"): off by default
    for (int s = 0; s < kNumStops; ++s)
    {
        const String name = prettyLabel(kStops[(size_t)s].label) + " Wolke ";
        auto hz = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return String(v, v < 10.0f ? 1 : 0) + " /s"; });
        auto msA = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return String(roundToInt(v)) + " ms"; });
        auto degA = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return String(roundToInt(v)) + String::fromUTF8("\xc2\xb0"); });
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{cloudParamId("Mix", s), 1},
                                                    name + "Menge", NormalisableRange<float>(0, 1),
                                                    0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{cloudParamId("Dens", s), 1}, name + "Dichte",
            NormalisableRange<float>(1.0f, 200.0f, 0.01f, 0.4f), 30.0f, hz));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{cloudParamId("Spread", s), 1}, name + "Streuung",
            NormalisableRange<float>(0.0f, 180.0f, 0.1f), 60.0f, degA));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{cloudParamId("Size", s), 1},
            name + String::fromUTF8("Korngr\xc3\xb6\xc3\x9f"
                                    "e"),
            NormalisableRange<float>(10.0f, 500.0f, 0.1f, 0.5f), 80.0f, msA));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{cloudParamId("Scatter", s), 1}, name + "Zeitstreuung",
            NormalisableRange<float>(0.0f, 150.0f, 0.1f), 40.0f, msA));
    }
    // ---- "Lebendige Orgel" (Effekte, third tab): everything off by default
    {
        auto lvSecs = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            {
                return v < 1.0f ? String(roundToInt(v * 1000.0f)) + " ms"
                                : String(v, v < 10.0f ? 2 : 1) + " s";
            });
        auto lvMetres = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return (v > 0.005f ? "+" : "") + String(v, 2) + " m"; });
        auto lvDbOff = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            { return v <= -59.9f ? String("aus") : (v > 0 ? "+" : "") + String(v, 1) + " dB"; });
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"lvBellows", 1},
                                                    "Balg Windverbrauch",
                                                    NormalisableRange<float>(0, 1), 0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvBellowsSize", 1},
            String::fromUTF8("Balg Gr\xc3\xb6\xc3\x9f"
                             "e"),
            NormalisableRange<float>(0.3f, 30.0f, 0.01f, 0.4f), 4.0f, lvSecs));
        l.add(std::make_unique<AudioParameterChoice>(
            ParameterID{"lvBlower", 1}, "Balg Windquelle",
            StringArray{String::fromUTF8("Gebl\xc3\xa4se (stetig)"), "Kalkant tritt (frei)",
                        "Kalkant tritt im Tempo"},
            0));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvTreadRate", 1}, "Kalkant Tritte",
            NormalisableRange<float>(2.0f, 120.0f, 0.1f, 0.5f), 20.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int) { return String(v, 1) + " /min"; })));
        l.add(std::make_unique<AudioParameterChoice>(
            ParameterID{"lvTreadDiv", 1}, "Kalkant Tritt alle",
            StringArray{"8 Takte", "4 Takte", "2 Takte", "1 Takt", "1/2", "1/4"}, 3));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvTreadNoise", 1}, String::fromUTF8("Kalkant Trittger\xc3\xa4usch"),
            NormalisableRange<float>(0, 1), 0.3f, pct));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"lvPallet", 1}, "Ventil",
                                                    NormalisableRange<float>(0, 1), 0.0f, pct));
        l.add(std::make_unique<AudioParameterChoice>(
            ParameterID{"lvPalletSrc", 1},
            String::fromUTF8("Ventil \xc3\xb6"
                             "ffnet durch"),
            StringArray{String::fromUTF8("Anschlag (langsame Taste \xc3\xb6"
                                         "ffnet langsam)"),
                        "Druck (MPE / Aftertouch)", "Gleiten (MPE, CC 74)"},
            0));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvListenX", 1}, String::fromUTF8("H\xc3\xb6rer rechts / links"),
            NormalisableRange<float>(-20.0f, 20.0f, 0.01f), 0.0f, lvMetres));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvListenY", 1},
            String::fromUTF8("H\xc3\xb6rer vor / zur\xc3\xbc"
                             "ck"),
            NormalisableRange<float>(-20.0f, 20.0f, 0.01f), 0.0f, lvMetres));
        l.add(std::make_unique<AudioParameterChoice>(
            ParameterID{"lvWalkShape", 1}, String::fromUTF8("H\xc3\xb6rer geht"),
            StringArray{"steht", "im Kreis", "hin und her (links / rechts)",
                        String::fromUTF8("vor und zur\xc3\xbc"
                                         "ck"),
                        "eine Acht"},
            0));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvWalkRate", 1}, String::fromUTF8("H\xc3\xb6rer Tempo"),
            NormalisableRange<float>(0.002f, 2.0f, 0.0001f, 0.35f), 0.05f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int)
                {
                    return v < 0.1f ? String(1.0f / v, 1) + " s je Runde" : String(v, 2) + " Hz";
                })));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvWalkSize", 1}, String::fromUTF8("H\xc3\xb6rer Weite"),
            NormalisableRange<float>(0.0f, 20.0f, 0.01f, 0.6f), 3.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int) { return String(v, 1) + " m"; })));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"lvWear", 1},
                                                    String::fromUTF8("Verschlei\xc3\x9f"),
                                                    NormalisableRange<float>(0, 1), 0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvWearTime", 1}, String::fromUTF8("Verschlei\xc3\x9f Lebensdauer"),
            NormalisableRange<float>(0.1f, 240.0f, 0.01f, 0.35f), 5.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int)
                {
                    return v < 1.0f ? String(roundToInt(v * 60.0f)) + " s" : String(v, 1) + " min";
                })));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvSympLevel", 1}, "Mitklingende Pfeifen",
            NormalisableRange<float>(-60.0f, 12.0f, 0.1f, 2.0f), -60.0f, lvDbOff));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvSympDecay", 1}, "Mitklingen Nachklang",
            NormalisableRange<float>(0.05f, 6.0f, 0.001f, 0.4f), 0.6f, lvSecs));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"lvPure", 1}, "Reinheit",
                                                    NormalisableRange<float>(0, 1), 0.0f, pct));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvPureTime", 1}, "Reinheit Zeit",
            NormalisableRange<float>(0.03f, 20.0f, 0.001f, 0.35f), 1.5f, lvSecs));
        l.add(std::make_unique<AudioParameterBool>(ParameterID{"lvNetOn", 1}, "Tonnetz-Wanderer",
                                                   false));
        l.add(std::make_unique<AudioParameterChoice>(
            ParameterID{"lvNetPath", 1}, "Wanderer Weg",
            StringArray{
                "Quinten (bis zum Wolf)",
                String::fromUTF8("Terzkette (gro\xc3\x9f, klein, gro\xc3\x9f \xe2\x80\xa6)"),
                String::fromUTF8("zuf\xc3\xa4llig"),
                String::fromUTF8("zuf\xc3\xa4llig mit Heimweh"), "Spirale"},
            0));
        l.add(std::make_unique<AudioParameterChoice>(ParameterID{"lvNetDiv", 1}, "Wanderer Schritt",
                                                     StringArray{"frei (Tempo-Regler)", "1/1",
                                                                 "1/2", "1/4", "1/8", "1/16",
                                                                 "1/8 T", "1/16 T"},
                                                     0));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvNetRate", 1}, "Wanderer Tempo",
            NormalisableRange<float>(0.1f, 20.0f, 0.01f, 0.4f), 3.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int) { return String(v, 2) + " /s"; })));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvNetGate", 1}, String::fromUTF8("Wanderer Tonl\xc3\xa4nge"),
            NormalisableRange<float>(0.05f, 8.0f, 0.01f, 0.5f), 0.9f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int) { return String(v, 2) + String::fromUTF8(" Schritte"); })));

    }

    // Independent dry reference tone. Append parameters to preserve existing indices.
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"dsfOn", 1}, "Testton", false));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"dsfFrequency", 1}, "Testton Frequenz",
        NormalisableRange<float>(20.0f, 20000.0f, 0.1f, 0.25f), 440.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float value, int) { return String(value, 1) + " Hz"; })));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID{"dsfSpectrum", 1}, "Testton Spektrum",
                                                NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.0f,
                                                pct));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"dsfLevel", 1}, "Testton Pegel", NormalisableRange<float>(-60.0f, -6.0f, 0.1f),
        -32.0f, AudioParameterFloatAttributes().withLabel("dB")));
    // Legacy parameter retained for saved state/host indices; playback always uses trapezoidal.
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"dsfMethod", 1}, "Testton Integration",
                                                 StringArray{"Trapez", "Rechteck"}, 0));
    l.add(std::make_unique<AudioParameterBool>(ParameterID{"dsfMidi", 1}, "Testton folgt MIDI",
                                               true));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"dsfOffset", 1}, "Testton Feinstimmung",
        NormalisableRange<float>(-50.0f, 50.0f, 0.01f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float value, int)
            {
                // Normalised float round trips can leave a tiny negative residue at zero.
                return String(std::abs(value) < 0.005f ? 0.0f : value, 2) + " ct";
            })));

    l.add(std::make_unique<AudioParameterInt>(ParameterID{"dsfPartials", 1},
                                              "Number of Harmonic Partials", 1, 1024, 1024));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"dsfFootage", 1}, "Testton Oktavlage",
                                                 StringArray{"32'", "16'", "8'", "4'", "2'"}, 2));
    l.add(std::make_unique<AudioParameterInt>(ParameterID{"dsfMixtur", 1}, "Testton Mixtur", 1, 9,
                                              1));
    juce::StringArray referenceRegisters;
    for (const int stop : kDsfReferenceStops)
        referenceRegisters.add(kStops[static_cast<std::size_t>(stop)].label);
    referenceRegisters.add("1/4-Komma MT A-444,5");
    l.add(std::make_unique<AudioParameterChoice>(ParameterID{"dsfRegister", 1}, "Testton Register",
                                                 referenceRegisters, 0));
    // Append new controls after Testton to preserve saved routing and host parameter indices.
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lvImpLevel", 1}, "Aufprall",
        NormalisableRange<float>(-60.0f, 12.0f, 0.1f, 2.0f), -60.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            { return v <= -59.9f ? String("aus") : (v > 0 ? "+" : "") + String(v, 1) + " dB"; })));
    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID{"lvImpSrc", 1}, "Aufprall von",
        StringArray{"klingenden Pfeifen", "allen Pfeifen"}, 0));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lvImpRing", 1}, "Aufprall Nachklang",
        NormalisableRange<float>(0.05f, 4.0f, 0.001f, 0.4f), 0.6f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int)
            {
                return v < 1.0f ? String(roundToInt(v * 1000.0f)) + " ms"
                                : String(v, v < 10.0f ? 2 : 1) + " s";
            })));
    // Spieler: the walked listener moves the pipes that are bodies in the Saal
    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID{"lvPlayerMode", 1}, "Spieler bewegt Pfeifen",
        StringArray{"aus", "zieht an", String::fromUTF8("st\xc3\xb6\xc3\x9ft ab"), "Umlaufbahn",
                    "Sog"},
        0));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lvPlayerForce", 1}, "Spieler Kraft", NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.5f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return String(roundToInt(v * 100.0f)) + " %"; })));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lvPlayerRange", 1}, "Spieler Reichweite",
        NormalisableRange<float>(0.5f, 20.0f, 0.01f, 0.5f), 4.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return String(v, 1) + " m"; })));
    // the 3D Saal's illusions (appended: see above)
    auto percent = [](float v, int)
    { return v <= 0.0005f ? String("aus") : String(roundToInt(v * 100.0f)) + " %"; };
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lvSelfPlay", 1}, "Selbstspiel", NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.0f, AudioParameterFloatAttributes().withStringFromValueFunction(percent)));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lvGrow", 1}, "Wachsen und Brechen",
        NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(percent)));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lvWhisper", 1}, String::fromUTF8("Fl\xc3\xbcstern"),
        NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(percent)));
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lvTimeDepth", 1}, "Zeit als Entfernung",
        NormalisableRange<float>(0.0f, rysum::TimeDistance::kMaxSeconds, 0.01f, 0.6f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return v <= 0.005f ? String("aus") : String(v, 1) + " s"; })));
    {
        NormalisableRange<float> size(0.25f, 8.0f, 0.001f);
        size.setSkewForCentre(1.0f);
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"lvRoomHear", 1}, String::fromUTF8("H\xc3\xb6rraum"), size, 1.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int)
                {
                    return std::abs(v - 1.0f) < 0.005f ? String("wie gesehen")
                                                       : String::fromUTF8("\xc3\x97 ") + String(v, 2);
                })));
    }
    l.add(std::make_unique<AudioParameterFloat>(
        ParameterID{"lvWallPull", 1}, String::fromUTF8("Wandn\xc3\xa4he"),
        NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction(percent)));
    // how many processor cores the audio may use (the Saal's reflections are spread over them)
    l.add(std::make_unique<AudioParameterChoice>(
        ParameterID{"audioThreads", 1}, "Kerne f" + String::fromUTF8("\xc3\xbc") + "r den Ton",
        StringArray{"automatisch", "1", "2", "3", "4"}, 0));

    // "Summe": EQ and compressor on the output (Effekte page, tab "Summe")
    {
        auto hz = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return v >= 1000.0f ? String(v / 1000.0f, 2) + " kHz" : String(juce::roundToInt(v)) + " Hz"; });
        auto db = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return (v > 0.05f ? "+" : "") + String(v, 1) + " dB"; });
        auto ms = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return String(v, v < 10.0f ? 1 : 0) + " ms"; });
        l.add(std::make_unique<AudioParameterBool>(ParameterID{"mstEqOn", 1}, "Summe EQ", false));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"mstLowHz", 1}, "Summe Tiefen Frequenz",
                                                    NormalisableRange<float>(30.0f, 600.0f, 0.0f, 0.4f), 120.0f, hz));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"mstLowDb", 1}, "Summe Tiefen",
                                                    NormalisableRange<float>(-15.0f, 15.0f), 0.0f, db));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"mstMidHz", 1}, "Summe Mitten Frequenz",
                                                    NormalisableRange<float>(150.0f, 8000.0f, 0.0f, 0.3f), 1000.0f, hz));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"mstMidDb", 1}, "Summe Mitten",
                                                    NormalisableRange<float>(-15.0f, 15.0f), 0.0f, db));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"mstMidQ", 1}, String::fromUTF8("Summe Mitten G\xc3\xbcte"),
            NormalisableRange<float>(0.2f, 6.0f, 0.0f, 0.5f), 0.7f,
            AudioParameterFloatAttributes().withStringFromValueFunction([](float v, int) { return "Q " + String(v, 2); })));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"mstHighHz", 1}, String::fromUTF8("Summe H\xc3\xb6hen Frequenz"),
                                                    NormalisableRange<float>(1500.0f, 18000.0f, 0.0f, 0.5f), 8000.0f, hz));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"mstHighDb", 1}, String::fromUTF8("Summe H\xc3\xb6hen"),
                                                    NormalisableRange<float>(-15.0f, 15.0f), 0.0f, db));
        l.add(std::make_unique<AudioParameterBool>(ParameterID{"mstCompOn", 1}, "Summe Kompressor", false));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"mstThresh", 1}, "Summe Schwelle",
                                                    NormalisableRange<float>(-48.0f, 0.0f), -12.0f, db));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"mstRatio", 1}, String::fromUTF8("Summe Verh\xc3\xa4ltnis"),
            NormalisableRange<float>(1.0f, 20.0f, 0.0f, 0.4f), 2.5f,
            AudioParameterFloatAttributes().withStringFromValueFunction([](float v, int) { return String(v, v < 10.0f ? 1 : 0) + " : 1"; })));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"mstAttack", 1}, "Summe Ansprache",
                                                    NormalisableRange<float>(0.1f, 100.0f, 0.0f, 0.35f), 10.0f, ms));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"mstRelease", 1}, "Summe Ausklang",
                                                    NormalisableRange<float>(10.0f, 1500.0f, 0.0f, 0.4f), 150.0f, ms));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"mstMakeup", 1}, "Summe Aufholen",
                                                    NormalisableRange<float>(0.0f, 24.0f), 0.0f, db));
    }
    // each stop's own effects (Raum page, tab "Pfeife"), on top of the global ones
    {
        auto pct = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return String(juce::roundToInt(v * 100)) + " %"; });
        for (int s = 0; s < kNumStops; ++s)
        {
            const String name = prettyLabel(kStops[(size_t)s].label) + " ";
            l.add(std::make_unique<AudioParameterFloat>(ParameterID{pipeSndParamId("Trem", s), 1}, name + "Tremulant",
                                                        NormalisableRange<float>(0, 1), 0.0f, pct));
            l.add(std::make_unique<AudioParameterFloat>(ParameterID{pipeSndParamId("Crack", s), 1}, name + "Knistern",
                                                        NormalisableRange<float>(0, 1), 0.0f, pct));
            l.add(std::make_unique<AudioParameterFloat>(ParameterID{pipeSndParamId("Leak", s), 1}, name + "Windleck",
                                                        NormalisableRange<float>(0, 1), 0.0f, pct));
            l.add(std::make_unique<AudioParameterFloat>(ParameterID{pipeSndParamId("Spread", s), 1},
                                                        name + "Tempo-Streuung", NormalisableRange<float>(0, 1), 0.0f, pct));
        }
    }
    // Vox humana: the neural stop (NeuralRysum v2), and the input playing the organ. Appended last:
    // older projects keep their parameter indices.
    {
        auto vpct = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return String(juce::roundToInt(v * 100)) + " %"; });
        auto signedVal = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return (v > 0.005f ? "+" : "") + String(v, 2); });
        auto vdeg = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return String(juce::roundToInt(v)) + String::fromUTF8("\xc2\xb0"); });
        l.add(std::make_unique<AudioParameterBool>(ParameterID{"voxOn", 1}, "Vox humana", false));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"voxVol", 1}, String::fromUTF8("Vox humana Lautst\xc3\xa4rke"),
            NormalisableRange<float>(-48.0f, 12.0f, 0.1f), 0.0f,
            AudioParameterFloatAttributes().withLabel("dB")));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"voxWind", 1}, "Vox humana Wind",
                                                    NormalisableRange<float>(0.0f, 1.0f, 0.001f), 1.0f, vpct));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"voxX", 1}, "Vox humana Klangfarbe X",
                                                    NormalisableRange<float>(-3.0f, 3.0f, 0.001f), 0.0f, signedVal));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"voxY", 1}, "Vox humana Klangfarbe Y",
                                                    NormalisableRange<float>(-3.0f, 3.0f, 0.001f), 0.0f, signedVal));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"voxBright", 1}, "Vox humana Helligkeit",
                                                    NormalisableRange<float>(-1.0f, 1.0f, 0.001f), 0.0f, signedVal));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"voxNoise", 1}, "Vox humana Hauch",
                                                    NormalisableRange<float>(-1.0f, 1.0f, 0.001f), 0.0f, signedVal));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"voxSpread", 1}, "Vox humana Breite",
                                                    NormalisableRange<float>(-1.0f, 1.0f, 0.001f), 0.0f, signedVal));
        l.add(std::make_unique<AudioParameterBool>(ParameterID{"voxFollow", 1},
                                                   "Vox humana Klang folgt Eingang", false));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"voxRoom", 1}, "Vox humana eigener Raum",
                                                    NormalisableRange<float>(0.0f, 1.0f, 0.001f), 1.0f, vpct));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"voxAz", 1}, "Vox humana Richtung",
                                                    NormalisableRange<float>(-180.0f, 180.0f, 0.1f), 0.0f, vdeg));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"voxEl", 1}, String::fromUTF8("Vox humana H\xc3\xb6he"),
                                                    NormalisableRange<float>(-30.0f, 90.0f, 0.1f), 12.0f, vdeg));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"voxDist", 1}, "Vox humana Abstand", NormalisableRange<float>(1.0f, 40.0f, 0.01f, 0.5f), 6.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int) { return String(v, 1) + " m"; })));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"voxSend", 1}, "Vox humana Hall-Anteil",
                                                    NormalisableRange<float>(0.0f, 2.0f, 0.001f), 0.6f, vpct));
        l.add(std::make_unique<AudioParameterBool>(ParameterID{"inPlays", 1}, "Eingang spielt die Orgel", false));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"inThresh", 1}, "Eingang Erkennungsschwelle", NormalisableRange<float>(0.3f, 0.95f, 0.001f), 0.6f,
            vpct));
        l.add(std::make_unique<AudioParameterInt>(ParameterID{"inConfirm", 1}, String::fromUTF8("Eingang Best\xc3\xa4tigung"),
                                                  1, 16, 4,
                                                  AudioParameterIntAttributes().withStringFromValueFunction(
                                                      [](int v, int) { return String(v * 4 / 3.0f, 1) + " ms"; })));
        l.add(std::make_unique<AudioParameterInt>(ParameterID{"inVoices", 1}, "Eingang Stimmen", 1, 8, 8));
        // its eight pipes in the room: how they stand and move (the motions on the sphere, as for the stops)
        l.add(std::make_unique<AudioParameterChoice>(
            ParameterID{"voxMotion", 1}, "Vox humana Bewegung",
            StringArray{"Statisch", "Rotation", "Pendel", "Drift", "Vertikal", String::fromUTF8("Spr\xc3\xbcnge")}, 0));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"voxRate", 1}, "Vox humana Geschwindigkeit",
            NormalisableRange<float>(-500.0f, 500.0f, 0.0001f, 0.2f, true), 0.08f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int) { return (v > 0 ? "+" : "") + String(v, std::abs(v) < 1.0f ? 3 : 2) + " Hz"; })));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"voxDepth", 1}, "Vox humana Tiefe",
                                                    NormalisableRange<float>(0.0f, 180.0f, 0.1f), 40.0f, vdeg));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"voxWidth", 1}, "Vox humana Breite (Pfeifen)",
                                                    NormalisableRange<float>(0.0f, 360.0f, 0.1f), 40.0f, vdeg));
        // its Saal (page Saal): its own, apart from the audio input's
        StringArray saals;
        for (int r = 0; r < rysum::kNumRooms; ++r)
            saals.add("Saal " + String(r + 1));
        l.add(std::make_unique<AudioParameterChoice>(ParameterID{"simRoomVox", 1}, "Vox humana im Saal", saals, 0));
        // velocity of the notes the input plays (its level), and how much the Vox humana's voices follow it
        auto vdb = AudioParameterFloatAttributes().withStringFromValueFunction(
            [](float v, int) { return (v > 0.05f ? "+" : "") + String(v, 1) + " dB"; });
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"inVelGain", 1}, "Eingang Anschlag Empfindlichkeit",
                                                    NormalisableRange<float>(-24.0f, 36.0f, 0.1f), 0.0f, vdb));
        l.add(std::make_unique<AudioParameterFloat>(
            ParameterID{"inVelRange", 1}, "Eingang Anschlag Bereich", NormalisableRange<float>(12.0f, 72.0f, 0.1f), 48.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction(
                [](float v, int) { return String(v, 0) + " dB"; })));
        l.add(std::make_unique<AudioParameterFloat>(ParameterID{"voxDyn", 1}, "Vox humana Dynamik",
                                                    NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.0f, vpct));
    }
    return l;
}

rysum::MasterSettings RysumProcessor::masterSettingsNow() const
{
    // (audio thread: the values as the Verlauf and the modulation set them, where they do)
    auto v = [this](int k)
    {
        const int i = masterIdx[(size_t)k];
        if (i < 0)
            return 0.0f;
        if (haveModVals && i < (int)modVals.size())
            return modVals[(size_t)i];
        return rawList[(size_t)i]->load();
    };
    rysum::MasterSettings s;
    s.eqOn = v(0) > 0.5f;
    s.lowHz = v(1);
    s.lowDb = v(2);
    s.midHz = v(3);
    s.midDb = v(4);
    s.midQ = v(5);
    s.highHz = v(6);
    s.highDb = v(7);
    s.compOn = v(8) > 0.5f;
    s.threshDb = v(9);
    s.ratio = v(10);
    s.attackMs = v(11);
    s.releaseMs = v(12);
    s.makeupDb = v(13);
    return s;
}

// the audio input: the instrument has a main input (Reaper: the track's own audio; the standalone
// app: the audio interface) and a side chain (Ableton Live and others route other tracks there);
// the effect "Rysum FX" sits on an audio track
juce::AudioProcessor::BusesProperties RysumProcessor::makeBuses()
{
    auto b = BusesProperties();
#if RYSUM_FX
    b = b.withInput("Input", juce::AudioChannelSet::stereo(), true);
#else
    b = b.withInput(juce::String::fromUTF8("Eingang"), juce::AudioChannelSet::stereo(), true)
            .withInput("Sidechain", juce::AudioChannelSet::stereo(), true);
#endif
    return b.withOutput("Output", juce::AudioChannelSet::ambisonic(kMaxOrder), true);
}

RysumProcessor::RysumProcessor()
    : AudioProcessor(makeBuses()), apvts(*this, nullptr, "RYSUM", createLayout())
{
    // the Vox humana's network is built in (resources/vox_humana.nrv2, NeuralRysum v2)
    if (!voxStop.load(RysumData::vox_humana_nrv2, (size_t)RysumData::vox_humana_nrv2Size, voxError) &&
        voxError.isEmpty())
        voxError = "Vox humana: Modell fehlt";
    for (auto &v : voxShown)
        v = -1;
    for (size_t i = 0; i < kVoxIds.size(); ++i)
        voxRaw[i] = apvts.getRawParameterValue(kVoxIds[i]);
    for (int s = 0; s < kNumStops; ++s)
    {
        stopParams[(size_t)s] = apvts.getRawParameterValue(stopParamId(s));
        volParams[(size_t)s] = apvts.getRawParameterValue(volParamId(s));
        windParams[(size_t)s] = apvts.getRawParameterValue(windParamId(s));
        motionParams[(size_t)s] = apvts.getRawParameterValue(motionParamId(s));
        rateParams[(size_t)s] = apvts.getRawParameterValue(rateParamId(s));
        depthParams[(size_t)s] = apvts.getRawParameterValue(depthParamId(s));
        ellipseParams[(size_t)s] = apvts.getRawParameterValue(ellipseParamId(s));
        layouts[(size_t)s] = defaultLayout(s);
    }
    master = apvts.getRawParameterValue("master");
    output = apvts.getRawParameterValue("output");
    click = apvts.getRawParameterValue("click");
    clickBlend = apvts.getRawParameterValue("clickBlend");
    releaseClick = apvts.getRawParameterValue("releaseClick");
    stopPull = apvts.getRawParameterValue("stopPull");
    wind = apvts.getRawParameterValue("wind");
    degAmount = apvts.getRawParameterValue("degAmount");
    degGrain = apvts.getRawParameterValue("degGrain");
    degBits = apvts.getRawParameterValue("degBits");
    degRate = apvts.getRawParameterValue("degRate");
    degWobble = apvts.getRawParameterValue("degWobble");
    degDropouts = apvts.getRawParameterValue("degDropouts");
    degMode = apvts.getRawParameterValue("degMode");
    irDry = apvts.getRawParameterValue("irDry");
    irWet = apvts.getRawParameterValue("irWet");
    irFumaParam = apvts.getRawParameterValue("irFuma");
    irN3DParam = apvts.getRawParameterValue("irN3D");
    irFollowParam = apvts.getRawParameterValue("irFollow");
    irEnvelopParam = apvts.getRawParameterValue("irEnvelop");
    irDirectParam = apvts.getRawParameterValue("irDirect");
    for (int t = 0; t < temperamentNames().size(); ++t)
        tunings.push_back(makeKeyTuning(t));
    binOrderParam = apvts.getRawParameterValue("binOrder");
    binYawParam = apvts.getRawParameterValue("binYaw");
    audioThreadsParam = apvts.getRawParameterValue("audioThreads");
    binDfParam = apvts.getRawParameterValue("binDfEq");
    // the standalone app is mostly used with headphones: start in binaural mode
    if (juce::PluginHostType::getPluginLoadedAs() == wrapperType_Standalone)
        if (auto *o = apvts.getParameter("output"))
            o->setValueNotifyingHost(o->convertTo0to1((float)kBinauralOutput));
    for (auto *id :
         {"doppler", "air", "distAmount", "temperament", "pitch", "detune", "tremOn", "tremRate",
          "tremDepth", "freeze", "freezeSmear", "freezeRelease", "motionSync", "midiCC"})
        extraParams[id] = apvts.getRawParameterValue(id);

    // MIDI CC targets (see handleCc)
    ccTargets.push_back(apvts.getParameter("wind"));
    for (int s = 0; s < kNumStops; ++s)
        ccTargets.push_back(apvts.getParameter(stopParamId(s)));
    for (int s = 0; s < kNumStops; ++s)
        ccTargets.push_back(apvts.getParameter(windParamId(s)));
    ccTargets.push_back(apvts.getParameter("freeze"));
    ccTargets.push_back(apvts.getParameter("tremDepth"));
    for (auto &c : ccPending)
        c = std::numeric_limits<float>::quiet_NaN();
    for (int i = 0; i < kNumMidiMaps; ++i)
    {
        mmCc[(size_t)i] = -1;
        mmChan[(size_t)i] = 0;
        mmParam[(size_t)i] = -1;
        mmLo[(size_t)i] = 0.0f;
        mmHi[(size_t)i] = 1.0f;
        mmToggle[(size_t)i] = false;
        mmPending[(size_t)i] = std::numeric_limits<float>::quiet_NaN();
    }
    for (int i = 0; i < kNumButtonMaps; ++i)
    {
        bmCode[(size_t)i] = -1;
        bmChan[(size_t)i] = 0;
        bmOn[(size_t)i] = false;
    }
    noteScratch.ensureSize(8192);

    // ---- parameter table (modulation works on indices into it)
    for (auto *prm : getParameters())
        if (auto *rp = dynamic_cast<juce::RangedAudioParameter *>(prm))
        {
            paramIndex[rp->getParameterID()] = (int)prmList.size();
            prmList.push_back(rp);
            rawList.push_back(apvts.getRawParameterValue(rp->getParameterID()));
            isFloatParam.push_back(dynamic_cast<juce::AudioParameterFloat *>(rp) != nullptr &&
                                           rp->getParameterID() != "output"
                                       ? 1
                                       : 0);
        }
    const int N = (int)prmList.size();
    for (size_t k = 0; k < kMasterIds.size(); ++k)
        masterIdx[k] = getParamIndex(kMasterIds[k]);
    modVals.assign((size_t)N, 0.0f);
    modOff.assign((size_t)N, 0.0f);
    modHitFlag.assign((size_t)N, 0);
    modNorm.reset(new std::atomic<float>[(size_t)N]);
    for (int i = 0; i < N; ++i)
        modNorm[static_cast<size_t>(i)] = std::numeric_limits<float>::quiet_NaN();
    hits.reserve(kNumRoutes);
    prevHits.reserve(kNumRoutes);
    // Verlauf: the curve values, which controls a hand holds, the slices' bookkeeping
    autoNorm.reset(new std::atomic<float>[(size_t)N]);
    autoTouched.reset(new std::atomic<uint8_t>[(size_t)N]);
    for (int i = 0; i < N; ++i)
    {
        autoNorm[(size_t)i] = std::numeric_limits<float>::quiet_NaN();
        autoTouched[(size_t)i] = 0;
    }
    autoFlag.assign((size_t)N, 0);
    autoHits.reserve(rysum::kMaxAutoLanes);
    prevAutoHits.reserve(rysum::kMaxAutoLanes);
    hostToList.assign((size_t)getParameters().size(), -1);
    for (int i = 0; i < N; ++i)
    {
        const int h = prmList[(size_t)i]->getParameterIndex();
        if (h >= 0 && h < (int)hostToList.size())
            hostToList[(size_t)h] = i;
        prmList[(size_t)i]->addListener(&gestureWatch);
    }
    auto I = [this](const juce::String &id)
    {
        jassert(paramIndex.count(id) > 0);
        return paramIndex.at(id);
    };
    for (int s = 0; s < kNumStops; ++s)
    {
        ix.stop[(size_t)s] = I(stopParamId(s));
        ix.vol[(size_t)s] = I(volParamId(s));
        ix.swind[(size_t)s] = I(windParamId(s));
        ix.motion[(size_t)s] = I(motionParamId(s));
        ix.rate[(size_t)s] = I(rateParamId(s));
        ix.depth[(size_t)s] = I(depthParamId(s));
        ix.ellipse[(size_t)s] = I(ellipseParamId(s));
        ix.ellDir[(size_t)s] = I(ellDirParamId(s));
        ix.ellTilt[(size_t)s] = I(ellTiltParamId(s));
        ix.ellForm[(size_t)s] = I(ellFormParamId(s));
        ix.pathSpread[(size_t)s] = I(pathSpreadParamId(s));
        ix.jumpHard[(size_t)s] = I(jumpHardParamId(s));
    }
    ix.doppler = I("doppler");
    ix.air = I("air");
    ix.distAmount = I("distAmount");
    ix.temperament = I("temperament");
    ix.pitch = I("pitch");
    ix.detune = I("detune");
    ix.tremOn = I("tremOn");
    ix.tremRate = I("tremRate");
    ix.tremDepth = I("tremDepth");
    ix.freeze = I("freeze");
    ix.freezeSmear = I("freezeSmear");
    ix.freezeRelease = I("freezeRelease");
    ix.motionSync = I("motionSync");
    ix.master = I("master");
    ix.output = I("output");
    ix.click = I("click");
    ix.clickBlend = I("clickBlend");
    ix.releaseClick = I("releaseClick");
    ix.stopPull = I("stopPull");
    ix.wind = I("wind");
    ix.degAmount = I("degAmount");
    ix.degGrain = I("degGrain");
    ix.degBits = I("degBits");
    ix.degRate = I("degRate");
    ix.degWobble = I("degWobble");
    ix.degDropouts = I("degDropouts");
    ix.degMode = I("degMode");
    ix.irDry = I("irDry");
    ix.irWet = I("irWet");
    ix.binYaw = I("binYaw");
    ix.windModel = I("windModel");
    ix.mtsCont = I("mtsContinuous");
    ix.mpe = I("mpe");
    ix.mpeBend = I("mpeBend");
    ix.lucOn = I("lucOn");
    ix.lucMix = I("lucMix");
    ix.lucMode = I("lucMode");
    for (int i = 0; i < kNumLfos; ++i)
    {
        const juce::String id = "lfo" + juce::String(i + 1);
        ix.lfoShape[(size_t)i] = I(id + "Shape");
        ix.lfoRate[(size_t)i] = I(id + "Rate");
        ix.lfoSync[(size_t)i] = I(id + "Sync");
        ix.lfoDiv[(size_t)i] = I(id + "Div");
        ix.lfoSpread[(size_t)i] = I(id + "Spread");
        ix.lfoSpreadMode[(size_t)i] = I(id + "SpreadMode");
    }
    for (int j = 0; j < kNumEnvs; ++j)
    {
        const juce::String id = "env" + juce::String(j + 1);
        ix.envMode[(size_t)j] = I(id + "Mode");
        ix.envDelay[(size_t)j] = I(id + "Delay");
        ix.envAttack[(size_t)j] = I(id + "Attack");
        ix.envDecay[(size_t)j] = I(id + "Decay");
        ix.envSustain[(size_t)j] = I(id + "Sustain");
        ix.envRelease[(size_t)j] = I(id + "Release");
    }
    for (int r = 0; r < kNumRoutes; ++r)
    {
        ix.modAmt[(size_t)r] = I("mod" + juce::String(r + 1) + "Amt");
        routeSrc[(size_t)r] = -1;
        routeTgt[(size_t)r] = -1;
    }
    for (int k = 0; k < kNumFx; ++k)
        ix.fx[(size_t)k] = I(kFxIds[k]);
    for (int k = 0; k < kNumSim; ++k)
        ix.sim[(size_t)k] = I(kSimIds[k]);
    for (int k = 0; k < kNumInp; ++k)
        ix.inp[(size_t)k] = I(kInpIds[k]);
    {
        const char *keys[]{"W",   "L",        "H",    "Size",    "Lx",    "Ly",
                           "Abs", "AbsFloor", "High", "Scatter", "Shape", "ShapeAmt"};
        for (int r = 0; r < rysum::kNumRooms; ++r)
            for (int k = 0; k < 12; ++k)
                ix.room[(size_t)r][(size_t)k] = I(roomParamId(r, keys[k]));
        for (int r = 0; r < rysum::kNumRooms; ++r)
        {
            drawnPlans[(size_t)r] = rysum::defaultDrawnPlan();
            engine.setDrawnPlan(r, drawnPlans[(size_t)r]);
        }
        for (int st = 0; st < kNumStops; ++st)
            ix.stopRoom[(size_t)st] = I(stopRoomParamId(st));
        ix.inputRoom = I("simRoomIn");
        ix.noteSpread = I("noteSpread");
        for (int k = 0; k < 7; ++k)
            ix.spc[(size_t)k] = I(kSpcIds[k]);
        for (int st = 0; st < kNumStops; ++st)
            for (int k = 0; k < 8; ++k)
                ix.pipeSnd[(size_t)st][(size_t)k] = I(pipeSndParamId(kPipeSndKeys[k], st));
        for (int st = 0; st < kNumStops; ++st)
            for (int k = 0; k < 4; ++k)
                ix.stopFx[(size_t)st][(size_t)k] = I(pipeSndParamId(kStopFxKeys[k], st));
        ix.airInvert = I("airInvert");
        ix.clickRes = I("clickRes");
        ix.clickResDecay = I("clickResDecay");
        ix.clickResBright = I("clickResBright");
        {
            const char *g[]{"degStutter", "degSplice", "degZero", "degClip"};
            for (int k = 0; k < 4; ++k)
                ix.glitch[(size_t)k] = I(g[k]);
        }
        for (int k = 0; k < (int)ix.space2.size(); ++k)
            ix.space2[(size_t)k] = I(kSpace2Ids[k]);
        for (int k = 0; k < kNumLive; ++k)
            ix.live[(size_t)k] = I(kLiveIds[k]);
        for (int st = 0; st < kNumStops; ++st)
            for (int k = 0; k < 5; ++k)
                ix.cloud[(size_t)st][(size_t)k] = I(cloudParamId(kCloudKeys[k], st));
    }
    engine.setModHook(this);
    engine.spaceDegInProcess =
        false; // processBlock applies the spatial degradation after the impulse response

    // MIDI controller assignments: every new instance starts with the ones saved last
    // (Documents/Rysum/MIDI-Regler.xml); a saved project / session brings its own
    if (auto xml = juce::XmlDocument::parse(midiMapFile()))
        applyMidiMapState(juce::ValueTree::fromXml(*xml));
    savedMapVersion = midiMapVersion.load();

    {
        // loudspeakers: the layout used last (Documents/Rysum/Lautsprecher.json), otherwise an
        // octagon
        rysum::SpeakerLayout l;
        juce::String err;
        if (!(speakerDefaultFile().existsAsFile() &&
              rysum::speakerLayoutFromJson(speakerDefaultFile().loadFileAsString(), l, err)))
            l = rysum::speakerPresets()[4];
        setSpeakerLayout(l, false);
    }

    mts = MTS_RegisterClient(); // microtuning from an MTS-ESP master (if the MTS-ESP library is
                                // installed)

    startState = makeState(false); // (what "Alles zuruecksetzen" goes back to)
    startTimer(250); // first tick: find + load the samples (after a possible setStateInformation)
    morphTimer.startTimerHz(40);
}

void RysumProcessor::resetEverything()
{
    auto s = startState.createCopy();
    auto extra = s.getChildWithName("EXTRA");
    // the samples stay where they are found (no new loading); the room goes back to the first built-in
    // impulse response, as in a new instance
    extra.setProperty("samplesFolder", samplesFolder.getFullPathName(), nullptr);
    extra.removeProperty("irFile", nullptr);
    applyState(s, false);
    irChosen = false;
    if (auto l = getLibrary(); l != nullptr && !l->irs.empty())
        loadIr(l->irs.front().file);
    ++walkSerial; // (the 3D view stands where the state says)
    ++patchVersion; // (the header: "kein Patch")
    ++changeCounter;
}

bool RysumProcessor::setAudience(bool on, bool loopbackOnly)
{
    if (!on)
    {
        audience.reset();
        return true;
    }
    if (audience == nullptr)
        audience = std::make_unique<rysum::AudienceHands>(*this);
    if (audience->running())
        return true;
    if (!audience->start(loopbackOnly))
    {
        audience.reset();
        return false;
    }
    return true;
}

RysumProcessor::~RysumProcessor()
{
    audience.reset(); // (the port closes before anything it plays to goes)
    for (auto *prm : prmList)
        prm->removeListener(&gestureWatch);
    if (mts != nullptr)
    {
        MTS_DeregisterClient(mts);
        mts = nullptr;
    }
    aliveFlag->store(false);
    stopTimer();
    morphTimer.stopTimer();
    loader.reset();
    irPool.removeAllJobs(true, 10000);
    lucPiece.cancelAll();
    piecePool.removeAllJobs(true, 10000);
    lucPool.removeAllJobs(true, 10000);
}

bool RysumProcessor::isBusesLayoutSupported(const BusesLayout &busLayout) const
{
    // inputs: off, mono or stereo
    for (const auto &in : busLayout.inputBuses)
        if (in.size() > 2)
            return false;
    const int n = busLayout.getMainOutputChannels();
    return n >= 1 && n <= kMaxOutChannels;
}

void RysumProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    engine.prepare(sampleRate, samplesPerBlock);
    masterBus.prepare(sampleRate);
    referenceTone.prepare(sampleRate);
    keyboardState.reset();
    for (auto &b : irInputs)
        b.assign((size_t)juce::jmax(512, samplesPerBlock), 0.0f);
    latchMidi.ensureSize(8192);
    lastOnAt.fill(std::numeric_limits<int64_t>::min() / 2);
    const bool rateChanged = std::abs(sampleRate - hostRate.load()) > 0.5;
    hostRate = sampleRate;
    if (irActive != nullptr)
        irActive->reset();
    binBuf.setSize(kMaxAmbiChannels, juce::jmax(512, samplesPerBlock), false, true,
                   false); // reserved once (see processBlock)
    spkRenderer.prepare(sampleRate, juce::jmax(512, samplesPerBlock));
    trCtc.prepare(sampleRate, juce::jmax(512, samplesPerBlock));
    trBuiltKey.clear(); // (re)loaded by the timer at this rate
    lucPiece.prepare(sampleRate);
    lucRecScratch.assign((size_t)juce::jmax(512, samplesPerBlock), 0.0f);
    inBuf.setSize(2, juce::jmax(512, samplesPerBlock), false, true, false);
    voxStop.prepare(sampleRate, juce::jmax(8192, samplesPerBlock));
    for (size_t i = 0; i < kVoxIds.size(); ++i)
        voxIdx[i] = getParamIndex(kVoxIds[i]);
    voxMidi.ensureSize(4096);
    if (binActive != nullptr)
        binActive->reset();
    if (rateChanged && irFile.existsAsFile())
        juce::MessageManager::callAsync(
            [this, alive = aliveFlag]
            {
                if (alive->load())
                    loadIr(irFile);
            }); // resample IR
}

EngineParams RysumProcessor::readParams(const float *vals) const
{
    auto V = [this, vals](int i) { return vals != nullptr ? vals[i] : rawList[(size_t)i]->load(); };
    EngineParams p;
    if (audioThreadsParam != nullptr)
        p.audioThreads = juce::roundToInt(audioThreadsParam->load());
    for (int s = 0; s < kNumStops; ++s)
    {
        p.stopOn[(size_t)s] = V(ix.stop[(size_t)s]) > 0.5f;
        p.stopGain[(size_t)s] = dbToGain(V(ix.vol[(size_t)s]));
        p.stopWind[(size_t)s] = V(ix.swind[(size_t)s]);
        p.motionMode[(size_t)s] = (int)V(ix.motion[(size_t)s]);
        p.motionRate[(size_t)s] = V(ix.rate[(size_t)s]);
        p.motionDepth[(size_t)s] = V(ix.depth[(size_t)s]);
        p.ellipse[(size_t)s] = V(ix.ellipse[(size_t)s]);
        p.ellipseDir[(size_t)s] = V(ix.ellDir[(size_t)s]);
        p.ellipseTilt[(size_t)s] = V(ix.ellTilt[(size_t)s]);
        p.ellipseLobes[(size_t)s] = 1 + juce::roundToInt(V(ix.ellForm[(size_t)s]));
        p.pathSpread[(size_t)s] = V(ix.pathSpread[(size_t)s]);
        p.jumpHard[(size_t)s] = V(ix.jumpHard[(size_t)s]) > 0.5f;
    }
    p.doppler = V(ix.doppler);
    p.air = V(ix.air);
    p.distAmount = V(ix.distAmount);
    p.tuning = tunings[(size_t)juce::jlimit(
        0, (int)tunings.size() - 1,
        juce::roundToInt(V(ix.temperament)))]; // precomputed (UI + audio thread)
    {
        // free tuning: every MIDI note its own pitch (the table is filled on the audio thread, see
        // processBlock)
        const int t = juce::roundToInt(V(ix.temperament));
        if (t == kScalaTuning || t == kMtsTuning)
            p.notes = &noteTable;
        p.correctSamplePitch =
            p.notes != nullptr || (t != 0 && t != kScalaTuning && t != kMtsTuning);
        p.extendedRange = p.correctSamplePitch && extendedRange.load();
        p.pitchCents = (t == 0 || t == kScalaTuning || t == kMtsTuning)
                          ? 0.0
                          : effectiveStimmtonCents(V(ix.pitch));
        p.detune = t == 0 ? 0.0f : V(ix.detune);
        p.notesContinuous = V(ix.mtsCont) > 0.5f;
        p.mpe = V(ix.mpe) > 0.5f;
        p.mpeBendRange = V(ix.mpeBend);
        p.lucierOn = V(ix.lucOn) > 0.5f &&
                     V(ix.lucMode) > 0.5f; // the piece is played by the processor (lucPiece)
        p.lucierMix = V(ix.lucMix);
    }
    p.tremOn = V(ix.tremOn) > 0.5f;
    p.tremRate = V(ix.tremRate);
    p.tremDepth = V(ix.tremDepth);
    p.freeze = V(ix.freeze) > 0.5f;
    p.freezeSmear = V(ix.freezeSmear);
    p.freezeRelease = V(ix.freezeRelease);
    p.sync = V(ix.motionSync) > 0.5f;
    p.masterGain = dbToGain(V(ix.master));
    p.outputMode = juce::roundToInt(V(ix.output));
    if (p.outputMode == kBinauralOutput || p.outputMode == kTransauralOutput)
        p.outputMode =
            1 + juce::roundToInt(binOrderParam->load()); // processBlock uses the decoder's order
    if (p.outputMode == kSpeakerOutput)
        p.outputMode = spkOrderNow.load(); // processBlock uses the decoder's order
    p.clickLevel = V(ix.click);
    p.clickBlend = V(ix.clickBlend);
    p.releaseClick = V(ix.releaseClick);
    p.stopPull = V(ix.stopPull);
    p.wind = V(ix.wind);
    p.deg.amount = V(ix.degAmount);
    p.deg.grain = V(ix.degGrain);
    p.deg.bits = V(ix.degBits);
    p.deg.rateHz = V(ix.degRate);
    p.deg.wobble = V(ix.degWobble);
    p.deg.dropouts = V(ix.degDropouts);
    p.deg.perPipe = V(ix.degMode) < 0.5f;
    {
        auto Sp = [&](int k) { return V(ix.spc[(size_t)k]); };
        p.space.erode = Sp(0);
        p.space.weather = Sp(1) < 0.05f ? 0.0f : Sp(1);
        p.space.grid = Sp(2);
        p.space.bits = Sp(3);
        p.space.rateHz = Sp(4);
        p.space.drops = Sp(5);
        p.space.alias = Sp(6);
        for (int st = 0; st < kNumStops; ++st)
        {
            auto C = [&](int k) { return V(ix.cloud[(size_t)st][(size_t)k]); };
            auto &c = p.cloud[(size_t)st];
            c.mix = C(0);
            c.density = C(1);
            c.spread = C(2);
            c.sizeMs = C(3);
            c.scatterMs = C(4);
            auto Q = [&](int k) { return V(ix.pipeSnd[(size_t)st][(size_t)k]); };
            auto &q = p.pipeSnd[(size_t)st];
            q.attGain = Q(0);
            q.attSharp = Q(1);
            q.attIso = Q(2);
            q.attRep = Q(3);
            q.puff = Q(4);
            q.hiFreq = Q(5);
            q.hiGain = Q(6);
            q.hiShape = Q(7);
            // the stop's own tremulant, crackle, leaks, speed spread
            p.stopTrem[(size_t)st] = V(ix.stopFx[(size_t)st][0]);
            p.stopCrack[(size_t)st] = V(ix.stopFx[(size_t)st][1]);
            p.stopLeak[(size_t)st] = V(ix.stopFx[(size_t)st][2]);
            p.rateSpread[(size_t)st] = V(ix.stopFx[(size_t)st][3]);
        }
        p.airInvert = V(ix.airInvert);
        p.resClick = V(ix.clickRes);
        p.resDecay = V(ix.clickResDecay);
        p.resBright = V(ix.clickResBright);
        p.glitch.stutter = V(ix.glitch[0]);
        p.glitch.splice = V(ix.glitch[1]);
        p.glitch.zero = V(ix.glitch[2]);
        p.glitch.clip = V(ix.glitch[3]);
        auto S2 = [&](int k) { return V(ix.space2[(size_t)k]); };
        p.beatIdeal = S2(0) < 0.5f;
        p.ghostLevel = S2(1) <= -59.9f ? 0.0f : dbToGain(S2(1));
        p.ghostPlace = S2(2);
        p.ghostCons = S2(3);
        p.ice = S2(4);
        p.iceLen = S2(5);
        p.dex.mix = S2(6);
        p.dex.time = S2(7);
        p.dex.turn = S2(8);
        p.dex.mirror = juce::roundToInt(S2(9));
        p.dex.fade = S2(10);
        p.dex.again = S2(11);
        p.expo.mix = S2(12);
        p.expo.time = S2(13);
        p.expo.bright = S2(14);
        auto Lv = [&](int k) { return V(ix.live[(size_t)k]); };
        auto &lv = p.live;
        lv.bellows = Lv(lvBellows);
        lv.bellowsSize = Lv(lvBellowsSize);
        lv.blower = juce::roundToInt(Lv(lvBlower));
        lv.treadRate = Lv(lvTreadRate);
        lv.treadDiv = juce::roundToInt(Lv(lvTreadDiv));
        lv.treadNoise = Lv(lvTreadNoise);
        lv.pallet = Lv(lvPallet);
        lv.palletSrc = juce::roundToInt(Lv(lvPalletSrc));
        lv.listenX = Lv(lvListenX);
        lv.listenY = Lv(lvListenY);
        lv.walkShape = juce::roundToInt(Lv(lvWalkShape));
        lv.walkRate = Lv(lvWalkRate);
        lv.walkSize = Lv(lvWalkSize);
        lv.wear = Lv(lvWear);
        lv.wearMinutes = Lv(lvWearTime);
        lv.sympLevel = Lv(lvSympLevel) <= -59.9f ? 0.0f : dbToGain(Lv(lvSympLevel));
        lv.sympDecay = Lv(lvSympDecay);
        lv.pure = Lv(lvPure);
        lv.pureTime = Lv(lvPureTime);
        lv.netOn = Lv(lvNetOn) > 0.5f;
        lv.netPath = juce::roundToInt(Lv(lvNetPath));
        lv.netDiv = juce::roundToInt(Lv(lvNetDiv));
        lv.netRate = Lv(lvNetRate);
        lv.netGate = Lv(lvNetGate);
        lv.impLevel = Lv(lvImpLevel) <= -59.9f ? 0.0f : dbToGain(Lv(lvImpLevel));
        lv.impAll = Lv(lvImpSrc) > 0.5f;
        lv.impRing = Lv(lvImpRing);
        lv.playerMode = juce::roundToInt(Lv(lvPlayerMode));
        lv.playerForce = Lv(lvPlayerForce);
        lv.playerRange = Lv(lvPlayerRange);
        lv.selfPlay = Lv(lvSelfPlay);
        lv.grow = Lv(lvGrow);
        lv.whisper = Lv(lvWhisper);
        lv.timeDepth = Lv(lvTimeDepth);
        lv.roomHear = Lv(lvRoomHear);
        lv.wallPull = Lv(lvWallPull);
    }
    p.windModel = juce::roundToInt(V(ix.windModel));

    for (int i = 0; i < kNumLfos; ++i)
    {
        auto &L = p.mod.lfo[(size_t)i];
        L.shape = (int)V(ix.lfoShape[(size_t)i]);
        L.rate = V(ix.lfoRate[(size_t)i]);
        L.sync = V(ix.lfoSync[(size_t)i]) > 0.5f;
        L.div = (int)V(ix.lfoDiv[(size_t)i]);
        L.spread = V(ix.lfoSpread[(size_t)i]);
        L.spreadMode = (int)V(ix.lfoSpreadMode[(size_t)i]);
    }
    for (int j = 0; j < kNumEnvs; ++j)
    {
        auto &E = p.mod.env[(size_t)j];
        E.perPipe = V(ix.envMode[(size_t)j]) < 0.5f;
        E.delay = V(ix.envDelay[(size_t)j]);
        E.attack = V(ix.envAttack[(size_t)j]);
        E.decay = V(ix.envDecay[(size_t)j]);
        E.sustain = V(ix.envSustain[(size_t)j]);
        E.release = V(ix.envRelease[(size_t)j]);
    }
    for (int r = 0; r < kNumRoutes; ++r)
        p.mod.routes[(size_t)r] = {routeSrc[(size_t)r].load(), routeTgt[(size_t)r].load(),
                                   V(ix.modAmt[(size_t)r]), routeUni[(size_t)r].load()};

    auto X = [&](int k) { return V(ix.fx[(size_t)k]); };
    auto &f = p.fx;
    f.custLevel = X(fxCustLevel);
    f.custDown = X(fxCustDown);
    f.custUp = X(fxCustUp);
    f.custPlace = juce::roundToInt(X(fxCustPlace));
    f.custPitch = X(fxCustPitch);
    f.custScatter = X(fxCustScatter);
    f.custWander = X(fxCustWander);
    f.pathLevel = X(fxPathLevel);
    f.pathTime = X(fxPathTime);
    f.pathTicks = juce::roundToInt(X(fxPathTicks));
    f.pathSpread = X(fxPathSpread);
    f.pathWait = X(fxPathWait) > 0.5f;
    f.crackLevel = X(fxCrackLevel);
    f.crackRate = X(fxCrackRate);
    f.crackLen = X(fxCrackLen);
    f.leakLevel = X(fxLeakLevel);
    f.leakCount = juce::roundToInt(X(fxLeakCount));
    f.leakFreq = X(fxLeakFreq);
    f.leakWander = X(fxLeakWander);
    f.windRound = X(fxWindRound) > 0.5f;
    f.partSpread = X(fxPartSpread);
    f.partSplit = X(fxPartSplit);
    f.partTurn = X(fxPartTurn);
    f.echoLevel = X(fxEchoLevel);
    f.echoTime = X(fxEchoTime);
    f.echoSync = X(fxEchoSync) > 0.5f;
    f.echoDiv = juce::roundToInt(X(fxEchoDiv));
    f.echoRepeats = juce::roundToInt(X(fxEchoRepeats));
    f.echoFeedback = X(fxEchoFeedback);
    f.echoTurn = X(fxEchoTurn);
    f.echoTilt = X(fxEchoTilt);
    f.echoDamp = X(fxEchoDamp);

    {
        auto In = [&](int k) { return V(ix.inp[(size_t)k]); };
        auto &ip = p.input;
        ip.on = In(inpOn) > 0.5f;
        ip.level = In(inpLevel) <= -59.9f ? 0.0f : dbToGain(In(inpLevel));
        ip.stereo = In(inpMode) > 0.5f;
        ip.az = In(inpAz);
        ip.el = In(inpEl);
        ip.dist = In(inpDist);
        ip.width = In(inpWidth);
        ip.spin = In(inpSpin);
        ip.follow = juce::roundToInt(In(inpFollow)) - 1;
        ip.gate = In(inpGate) > 0.5f;
        ip.release = In(inpRelease);
        ip.send = In(inpSend) <= -59.9f ? 0.0f : dbToGain(In(inpSend));
    }

    auto S = [&](int k) { return V(ix.sim[(size_t)k]); };
    auto &r = p.room;
    r.on = S(simOn) > 0.5f;
    r.width = S(simW);
    r.length = S(simL);
    r.height = S(simH);
    r.size = S(simSize);
    r.listenerX = S(simLx);
    r.listenerY = S(simLy);
    r.absWalls = S(simAbs);
    r.absFloor = S(simAbsFloor);
    r.highDamp = S(simHigh);
    r.scatter = S(simScatter);
    r.order = 1 + juce::roundToInt(S(simOrder));
    r.erGain = S(simEr) <= -59.9f ? 0.0f : dbToGain(S(simEr));
    r.tailGain = S(simTail) <= -59.9f ? 0.0f : dbToGain(S(simTail));
    r.tailModel = juce::roundToInt(S(simTailModel));
    r.shape = juce::jlimit(0, rysum::kNumRoomShapes - 1, juce::roundToInt(V(ix.room[0][10])));
    r.shapeAmt = V(ix.room[0][11]);
    // the other rooms: own size, surfaces and listener; reflections order and levels as room 1
    for (int ri = 1; ri < rysum::kNumRooms; ++ri)
    {
        auto R = [&](int k) { return V(ix.room[(size_t)ri][(size_t)k]); };
        auto &q = p.moreRooms[(size_t)(ri - 1)];
        q = r;
        q.width = R(0);
        q.length = R(1);
        q.height = R(2);
        q.size = R(3);
        q.listenerX = R(4);
        q.listenerY = R(5);
        q.absWalls = R(6);
        q.absFloor = R(7);
        q.highDamp = R(8);
        q.scatter = R(9);
        q.shape = juce::jlimit(0, rysum::kNumRoomShapes - 1, juce::roundToInt(R(10)));
        q.shapeAmt = R(11);
    }
    for (int st = 0; st < kNumStops; ++st)
        p.stopRoom[(size_t)st] = juce::roundToInt(V(ix.stopRoom[(size_t)st]));
    p.inputRoom = juce::roundToInt(V(ix.inputRoom));
    p.noteSpread = juce::roundToInt(V(ix.noteSpread));
    return p;
}

// ---------------------------------------------------------------------------
// modulation of plugin parameters (audio thread, every 64 samples)
// ---------------------------------------------------------------------------
void RysumProcessor::modulate(EngineParams &p, const float *src)
{
    const int N = (int)rawList.size();
    for (int i = 0; i < N; ++i)
        modVals[(size_t)i] = rawList[(size_t)i]->load();
    // Verlauf first: a lane takes its parameter over, the routings below add on top of the curve
    applyAutomation(p);
    hits.clear();
    for (int r = 0; r < kNumRoutes; ++r)
    {
        const int sIdx = routeSrc[(size_t)r].load(),
                  t = routeTgt[(size_t)r].load() - kGlobalTargetBase;
        if (sIdx < 0 || sIdx >= kNumSources || t < 0 || t >= N || !isFloatParam[(size_t)t])
            continue;
        const float a = modVals[(size_t)ix.modAmt[(size_t)r]];
        if (a == 0.0f)
            continue;
        if (!modHitFlag[(size_t)t])
        {
            modHitFlag[(size_t)t] = 1;
            modOff[(size_t)t] = 0.0f;
            hits.push_back(t);
        }
        modOff[(size_t)t] += a * shapeSource(sIdx, src[sIdx], routeUni[(size_t)r].load());
    }
    for (int t : prevHits)
        if (!autoFlag[(size_t)t])
            modNorm[static_cast<size_t>(t)] = std::numeric_limits<float>::quiet_NaN();
    // automated parameters without a routing: their value is the curve's
    for (int t : prevAutoHits)
        if (!autoFlag[(size_t)t])
        {
            autoNorm[(size_t)t] = std::numeric_limits<float>::quiet_NaN();
            if (!modHitFlag[(size_t)t])
                modNorm[static_cast<size_t>(t)] = std::numeric_limits<float>::quiet_NaN();
        }
    for (int t : autoHits)
        if (!modHitFlag[(size_t)t])
            modNorm[static_cast<size_t>(t)] = autoNorm[(size_t)t].load();
    for (int t : autoHits)
        autoFlag[(size_t)t] = 0;
    std::swap(autoHits, prevAutoHits);
    for (int t : hits)
    {
        auto *prm = prmList[(size_t)t];
        const float n =
            juce::jlimit(0.0f, 1.0f, prm->convertTo0to1(modVals[(size_t)t]) + modOff[(size_t)t]);
        modVals[(size_t)t] = prm->convertFrom0to1(n);
        modNorm[static_cast<size_t>(t)] = n;
        modHitFlag[(size_t)t] = 0;
    }
    std::swap(hits, prevHits);
    haveModVals = true;
    if (prevHits.empty() && prevAutoHits.empty())
        return; // nothing global is modulated or automated: p already holds these values

    EngineParams q = readParams(modVals.data());
    applyCcOverrides(q);
    q.bpm = p.bpm;
    q.ppq = p.ppq;
    q.playing = p.playing;
    q.autoBeats = p.autoBeats;
    q.autoBeatRate = p.autoBeatRate;
    q.noteBeatRate = p.noteBeatRate;
    q.noteClock = p.noteClock;
    q.lastKeyClock = p.lastKeyClock;
    q.autoView = p.autoView;
    q.roomHarmonics = p.roomHarmonics;
    q.roomFollow = p.roomFollow;
    q.roomBaseAz = p.roomBaseAz;
    q.outputMode = p.outputMode;
    // the Vox humana is not in readParams: keep it, with its place and level as modulated now
    q.vox = p.vox;
    fillVoxPlace(q.vox, modVals.data());
    p = q;
}

void RysumProcessor::fillVoxPlace(VoxParams &vp, const float *vals) const noexcept
{
    auto v = [&](int i)
    {
        const int k = voxIdx[(size_t)i];
        return vals != nullptr && k >= 0 ? vals[(size_t)k] : voxRaw[(size_t)i]->load();
    };
    const float wnd = juce::jlimit(0.0f, 1.0f, v(2));
    vp.level = (v(1) <= -47.9f ? 0.0f : dbToGain(v(1))) * wnd * std::sqrt(wnd) * kVoxGain;
    vp.az = v(10);
    vp.el = v(11);
    vp.dist = v(12);
    vp.send = v(13);
    vp.motion = juce::roundToInt(v(18));
    vp.rate = v(19);
    vp.depth = v(20);
    vp.width = v(21);
    vp.room = juce::jlimit(0, rysum::kNumRooms - 1, juce::roundToInt(v(22)));
}

int RysumProcessor::getParamIndex(const juce::String &id) const
{
    auto it = paramIndex.find(id);
    return it == paramIndex.end() ? -1 : it->second;
}

float RysumProcessor::getModulatedNorm(int index) const
{
    return index >= 0 && index < (int)prmList.size() ? modNorm[static_cast<size_t>(index)].load()
                                                     : std::numeric_limits<float>::quiet_NaN();
}

juce::String RysumProcessor::targetId(int target) const
{
    if (target < 0)
        return {};
    if (target < kNumPipeTargets)
        return pipeTargetId(target);
    const int i = target - kGlobalTargetBase;
    return i >= 0 && i < (int)prmList.size() ? prmList[(size_t)i]->getParameterID()
                                             : juce::String();
}

int RysumProcessor::targetFromId(const juce::String &id) const
{
    if (id.startsWith("pipe:"))
    {
        const int t = id.fromFirstOccurrenceOf(":", false, false).getIntValue();
        return t >= 0 && t < kNumPipeTargets ? t : -1;
    }
    const int i = getParamIndex(id);
    return i >= 0 && isFloatParam[(size_t)i] ? kGlobalTargetBase + i : -1;
}

void RysumProcessor::setRoute(int r, int src, int target)
{
    if (r < 0 || r >= kNumRoutes)
        return;
    routeSrc[(size_t)r] = src;
    routeTgt[(size_t)r] = target;
    ++routeVersion;
}

int RysumProcessor::addRoute(int src, int target, float amount)
{
    for (int r = 0; r < kNumRoutes; ++r)
        if (routeSrc[(size_t)r].load() < 0 && routeTgt[(size_t)r].load() < 0)
        {
            setRoute(r, src, target);
            if (auto *a = prmList[(size_t)ix.modAmt[(size_t)r]])
            {
                a->beginChangeGesture();
                a->setValueNotifyingHost(a->convertTo0to1(amount));
                a->endChangeGesture();
            }
            return r;
        }
    return -1;
}

void RysumProcessor::setRouteUni(int r, bool uni)
{
    if (r < 0 || r >= kNumRoutes)
        return;
    routeUni[(size_t)r] = uni;
    ++routeVersion;
}

void RysumProcessor::clearRoute(int r)
{
    if (r < 0 || r >= kNumRoutes)
        return;
    routeUni[(size_t)r] = false;
    setRoute(r, -1, -1);
    if (auto *a = prmList[(size_t)ix.modAmt[(size_t)r]])
    {
        a->beginChangeGesture();
        a->setValueNotifyingHost(a->convertTo0to1(0.0f));
        a->endChangeGesture();
    }
}

void RysumProcessor::runVox(juce::MidiBuffer &midi, int n, EngineParams &params) noexcept
{
    // modulated values where the modulation page (or the Verlauf) moves them, else the parameter
    auto v = [this](int i)
    {
        const int k = voxIdx[(size_t)i];
        if (haveModVals && k >= 0 && k < (int)modVals.size())
            return modVals[(size_t)k];
        return voxRaw[(size_t)i]->load();
    };
    VoxStop::Settings s;
    s.stopOn = v(0) > 0.5f;
    s.wind = v(2);
    s.x = v(3);
    s.y = v(4);
    s.bright = v(5);
    s.noisy = v(6);
    s.spread = v(7);
    s.follow = v(8) > 0.5f;
    s.room = v(9);
    s.inputPlays = v(14) > 0.5f;
    s.thresh = v(15);
    s.confirm = juce::roundToInt(v(16));
    s.voices = juce::roundToInt(v(17));
    s.velGainDb = v(23);
    s.velRangeDb = v(24);
    s.dynamics = v(25);
    const bool haveIn = inputThisBlock;
    voxMidi.clear();
    voxStop.process(haveIn ? inBuf.getReadPointer(0) : nullptr, haveIn ? inBuf.getReadPointer(1) : nullptr, n,
                    midi, voxMidi, s, params);
    if (!voxMidi.isEmpty())
        midi.addEvents(voxMidi, 0, n, 0);
    for (int i = 0; i < (int)voxShown.size(); ++i)
        voxShown[(size_t)i] = s.inputPlays ? voxStop.trackedNote(i) : -1;

    // the stop's level (a half-drawn stop is quieter too, at 0 % silent like the others) and place
    auto &vp = params.vox;
    vp.on = s.stopOn && voxStop.isLoaded();
    fillVoxPlace(vp, haveModVals ? modVals.data() : nullptr);
    std::array<const float *, VoxStop::kOut> chans{};
    for (int c = 0; c < VoxStop::kOut; ++c)
        chans[(size_t)c] = voxStop.pipeOutput(c);
    engine.setVox(chans.data(), VoxStop::kOut, n);
}

void RysumProcessor::processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midi)
{
    juce::ScopedNoDenormals noDenormals;
    // how much of its time this block takes (the readout, the benchmark)
    struct LoadScope
    {
        RysumProcessor &p;
        double t0;
        int n;
        ~LoadScope()
        {
            const double sr = p.getSampleRate() > 0 ? p.getSampleRate() : 48000.0;
            const float load = (float)((juce::Time::getMillisecondCounterHiRes() - t0) * 0.001 *
                                       sr / (double)juce::jmax(1, n));
            p.cpuLoad = p.cpuLoad.load() + 0.05f * (load - p.cpuLoad.load());
            p.cpuPeak = juce::jmax(p.cpuPeak.load(), load);
        }
    } loadScope{*this, juce::Time::getMillisecondCounterHiRes(), buffer.getNumSamples()};

    if (auto pending =
            std::atomic_exchange(&pendingLibrary, std::shared_ptr<const SampleLibrary>()))
        engine.setLibrary(std::move(pending)); // the old one is still referenced by retiredLibrary

    // Konzert: the audio clock everything is timed by; a listener plays only what the player plays
    const double concertSr = getSampleRate() > 0 ? getSampleRate() : 48000.0;
    const double concertBlockTime = concertAudioTime;
    concertAudioTime += buffer.getNumSamples() / concertSr;
    concert.setAudioTime(concertBlockTime);
    const auto concertRole = concert.role();
    // (the player: knocks, claps and bowed pipes go to the listeners too)
    engine.tapSounds = concertRole == rysum::ConcertLink::Role::performer;
    if (!engine.tapSounds)
        engine.numTapped = 0;
    if (concertRole == rysum::ConcertLink::Role::listener)
        midi.clear();

    // notes that are MIDI buttons (pads mapped to a switch or to a button on screen) do not play
    if (noteMaps.load() || bmLearn.load() >= 0 || mmLearn.load() >= 0)
    {
        noteScratch.clear();
        bool any = false;
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            if (m.isNoteOnOrOff() &&
                handleTrigger(kNoteCode + m.getNoteNumber(), m.getChannel(),
                              m.isNoteOn() ? juce::jmax(1, (int)m.getVelocity()) : 0))
            {
                any = true;
                continue;
            }
            noteScratch.addEvent(meta.data, meta.numBytes, meta.samplePosition);
        }
        if (any)
        {
            // (copied back, not swapped: both buffers keep the memory they have)
            midi.clear();
            midi.addEvents(noteScratch, 0, -1, 0);
            triggerAsyncUpdate();
        }
    }

    keyboardState.processNextMidiBuffer(midi, 0, buffer.getNumSamples(), true);
    applyLatch(midi, buffer.getNumSamples());

    // MIDI controllers -> parameters: own assignments (MIDI learn) first, then the fixed map
    // (the fixed map is applied right away, the parameters follow on the message thread)
    {
        const bool fixedMap = extraParams.at("midiCC")->load() > 0.5f;
        bool any = false;
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            if (!m.isController())
                continue;
            const int cc = m.getControllerNumber(), v = m.getControllerValue();
            if (handleLearnedCc(cc, m.getChannel(), v))
            {
                any = true;
                continue;
            }
            if (!fixedMap)
                continue;
            int target = -1;
            float value = 0.0f;
            const int ns = kNumStops;
            if (cc == 11)
            {
                target = 0;
                value = static_cast<float>(v) / 127.0f;
            } // expression -> global wind
            else if (cc >= 20 && cc <= 26)
            {
                target = 1 + (cc - 20);
                value = v >= 64 ? 1.0f : 0.0f;
            } // stops on / off
            else if (cc == 19)
            {
                target = 1 + kPraestant16;
                value = v >= 64 ? 1.0f : 0.0f;
            } // Praestant 16'
            else if (cc >= 27 && cc <= 33)
            {
                target = 1 + ns + (cc - 27);
                value = static_cast<float>(v) / 127.0f;
            } // stop wind
            else if (cc == 34)
            {
                target = 1 + ns + kPraestant16;
                value = static_cast<float>(v) / 127.0f;
            } // Praestant 16' wind
            else if (cc == 66)
            {
                target = 1 + 2 * ns;
                value = v >= 64 ? 1.0f : 0.0f;
            } // sostenuto -> freeze
            else if (cc == 1)
            {
                target = 2 + 2 * ns;
                value = static_cast<float>(v) / 127.0f;
            } // mod wheel -> tremulant depth
            if (target >= 0)
            {
                ccPending[(size_t)target] = value;
                any = true;
            }
        }
        if (any)
            triggerAsyncUpdate();
    }

    // Konzert: the player sends what it plays now (after Halten and the controller assignments);
    // a listener plays what is due from the player instead of its own keys
    if (concertRole == rysum::ConcertLink::Role::performer)
        concert.captureMidi(midi, concertBlockTime, concertSr);
    else
    {
        if (concertRole == rysum::ConcertLink::Role::listener)
            midi.clear();
        concert.playInto(midi, concertBlockTime, buffer.getNumSamples(), concertSr);
    }

    // free tuning: MIDI Tuning Standard SysEx (e.g. from Hexatone) goes to the MTS-ESP client (used
    // when no master is connected); then the note table the engine plays from
    if (mts != nullptr)
        for (const auto meta : midi)
            if (meta.getMessage().isSysEx())
                MTS_ParseMIDIDataU(mts, meta.data, meta.numBytes);
    {
        const int t = juce::roundToInt(extraParams.at("temperament")->load());
        if (t == kMtsTuning)
        {
            scalaVersionSeen = -1; // Restore the Scala table when returning from MIDI tuning.
            // The client supplies 12edo at A4 = 440 Hz until it receives tuning.
            // Its frequency/filter calls also provide that fallback for a null client.
            for (int ch = 0; ch < 16; ++ch)
                for (int n = 0; n < 128; ++n)
                {
                    const size_t i = (size_t)(ch * 128 + n);
                    noteTable.hz[i] = (float)MTS_NoteToFrequency(mts, (char)n, (signed char)ch);
                    noteTable.mapped[i] =
                        MTS_ShouldFilterNote(mts, (char)n, (signed char)ch) ? 0 : 1;
                }
        }
        else if (t == kScalaTuning && !scalaReady.load())
        {
            // An empty Scala selection uses an absolute 12edo A4 = 440 Hz table.
            for (int ch = 0; ch < 16; ++ch)
                for (int n = 0; n < 128; ++n)
                {
                    const size_t i = (size_t)(ch * 128 + n);
                    noteTable.hz[i] = (float)(440.0 * std::exp2((n - 69) / 12.0));
                    noteTable.mapped[i] = 1;
                }
            scalaVersionSeen = -1;
        }
        else if (t == kScalaTuning && scalaVersion.load() != scalaVersionSeen)
        {
            const juce::SpinLock::ScopedTryLockType tl(
                scalaLock); // (the message thread writes it rarely)
            if (tl.isLocked())
            {
                noteTable = scalaShared;
                scalaVersionSeen = scalaVersion.load();
            }
        }
    }

    auto params = readParams();
    applyCcOverrides(params);

    // host tempo / position for tempo-synced movement
    bool hostPpq = false;
    if (auto *ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm())
                params.bpm = *bpm;
            if (auto ppq = pos->getPpqPosition())
                params.ppq = *ppq;
            params.playing = pos->getIsPlaying() && pos->getPpqPosition().hasValue();
            hostPpq = pos->getPpqPosition().hasValue();
        }

    // Verlauf: the automation's clock - the host's song position (also standing still, at the
    // cursor), without a host or by choice the own clock
    {
        if (auto pend = std::atomic_exchange(&autoPending, std::shared_ptr<const AutoCompiled>()))
            autoActive = std::move(pend); // the one before stays alive in autoKeep
        const auto *ac = autoActive.get();
        const double sr = getSampleRate() > 0 ? getSampleRate() : 48000.0;
        const double seek = autoSeekReq.exchange(-1.0);
        if (seek >= 0.0)
            autoOwnBeats = seek;
        if (ac != nullptr && ac->startOnKey && !autoRun.load())
            for (const auto meta : midi)
                if (meta.getMessage().isNoteOn())
                {
                    autoRun = true;
                    break;
                }
        const bool host = hostPpq && (ac == nullptr || ac->clock == 0);
        if (host)
        {
            params.autoBeats = params.ppq;
            params.autoBeatRate = params.playing ? params.bpm / 60.0 : 0.0;
        }
        else
        {
            params.autoBeats = autoOwnBeats;
            // the tempo: the tempo lane where there is one, else the page's
            double bpm = ac != nullptr ? ac->tempo : 60.0;
            if (ac != nullptr && ac->hasTempo)
                if (const float tv = rysum::autoValue(ac->tempoLane, autoOwnBeats); !std::isnan(tv))
                    bpm = rysum::autoTempoBpm(tv);
            params.autoBeatRate = autoRun.load() ? bpm / 60.0 : 0.0;
            const double before = autoOwnBeats;
            autoOwnBeats += buffer.getNumSamples() / sr * params.autoBeatRate;
            // a repeated section: at its end the clock goes back to its start (only when it runs
            // into the end from inside - a jump past it stays there)
            if (ac != nullptr && ac->cycle && before < ac->cycleEnd && before >= ac->cycleStart - 1.0e-6 &&
                autoOwnBeats >= ac->cycleEnd)
                autoOwnBeats = ac->cycleStart + std::fmod(autoOwnBeats - ac->cycleEnd,
                                                          ac->cycleEnd - ac->cycleStart);
        }
        // the clock of the lanes "mit jedem Anschlag neu": at the tempo, also while the host stands
        // still or the own clock is stopped (the host's tempo, else the page's or the tempo lane's)
        if (host)
            params.noteBeatRate = params.bpm / 60.0;
        else
        {
            double bpm = ac != nullptr ? ac->tempo : 60.0;
            if (ac != nullptr && ac->hasTempo)
                if (const float tv = rysum::autoValue(ac->tempoLane, params.autoBeats); !std::isnan(tv))
                    bpm = rysum::autoTempoBpm(tv);
            params.noteBeatRate = bpm / 60.0;
        }
        params.autoView = ac != nullptr && ac->on ? &ac->view : nullptr;
        autoNow = params.autoBeats;
        autoHostNow = host;
        autoMovingNow = params.autoBeatRate > 0.0;
    }

    // room: take a new impulse response before the engine runs (the sends depend on it)
    if (irClearRequest.exchange(false))
        irActive.reset(); // still referenced by retiredIr
    for (int ri = 0; ri < rysum::kNumRooms; ++ri)
        if (auto t = std::atomic_exchange(&pendingTail[(size_t)ri],
                                          std::shared_ptr<const rysum::TailIR>()))
            engine.setTailIR(std::move(t), ri); // still referenced by tailKeepAlive
    if (auto t = std::atomic_exchange(&pendingLucier, std::shared_ptr<const rysum::TailIR>()))
        engine.setLucierIR(std::move(t)); // still referenced by lucKeepAlive
    if (auto pending = std::atomic_exchange(&pendingIr, std::shared_ptr<rysum::Convolver>()))
        irActive = std::move(pending);
    if (irActive != nullptr && !params.room.on)
    {
        params.roomHarmonics = irActive->getInfo().harmonics;
        params.roomFollow = irFollowParam->load() > 0.5f;
        params.roomBaseAz = irActive->getInfo().baseAzDeg;
    }

    // binaural: the engine renders Ambisonics into an own buffer (the host usually has only 2
    // channels), the decoder turns it into left / right at the end. Until the first decoder is
    // ready: stereo.
    if (auto pending = std::atomic_exchange(&pendingBin, std::shared_ptr<rysum::BinauralDecoder>()))
        binActive = std::move(pending); // the old one is still referenced by retiredBin
    const int n = buffer.getNumSamples();
    const bool binaural = isBinaural() && binActive != nullptr && buffer.getNumChannels() > 0;
    if (isBinaural())
        params.outputMode = binaural ? binActive->getOrder() : 0;
    const bool transaural = binaural && isTransaural() && buffer.getNumChannels() > 1;
    // loudspeakers: the same Ambisonics buffer, decoded with AllRAD to the layout's channels
    if (auto pending =
            std::atomic_exchange(&pendingSpk, std::shared_ptr<const rysum::AllRadDecoder>()))
        spkRenderer.setDecoder(std::move(pending)); // still referenced by spkKeepAlive
    const bool speakers = juce::roundToInt(output->load()) == kSpeakerOutput &&
                          spkRenderer.next() != nullptr && spkRenderer.next()->error.isEmpty() &&
                          buffer.getNumChannels() > 0;
    if (juce::roundToInt(output->load()) == kSpeakerOutput)
        params.outputMode = speakers ? spkRenderer.next()->order : 0;
    // (setSize with avoidReallocating: no allocation as long as the block fits what prepareToPlay
    // reserved)
    if (binaural)
        binBuf.setSize(binActive->getNumChannels(), n, false, false, true);
    if (speakers)
        binBuf.setSize((params.outputMode + 1) * (params.outputMode + 1), n, false, false, true);
    juce::AudioBuffer<float> &work = binaural || speakers ? binBuf : buffer;

    // the audio input (main input + side chain, summed) is copied first: the engine overwrites the
    // buffer
    {
        bool any = false;
        if (inBuf.getNumSamples() < n)
            inBuf.setSize(2, n, false, false,
                          true); // (only if the host sends a longer block than announced)
        for (int b = 0; b < getBusCount(true); ++b)
        {
            const auto *bus = getBus(true, b);
            if (bus == nullptr || !bus->isEnabled())
                continue;
            const int nc = bus->getNumberOfChannels();
            for (int c = 0; c < juce::jmin(2, nc); ++c)
            {
                const int idx = bus->getChannelIndexInProcessBlockBuffer(c);
                if (idx < 0 || idx >= buffer.getNumChannels())
                    continue;
                for (int dst = (nc == 1 ? 0 : c); dst <= (nc == 1 ? 1 : c); ++dst)
                {
                    if (!any)
                        inBuf.clear(0, n);
                    any = true;
                    juce::FloatVectorOperations::add(inBuf.getWritePointer(dst),
                                                     buffer.getReadPointer(idx), n);
                }
            }
        }
        // Konzert: the player's input goes to the listeners; a listener hears the player's input
        // (in time with the notes), never its own
        if (concertRole == rysum::ConcertLink::Role::performer && any &&
            apvts.getRawParameterValue("inpOn")->load() > 0.5f)
            concert.captureAudio(inBuf.getReadPointer(0), inBuf.getReadPointer(1), n, concertBlockTime, concertSr);
        else if (concertRole == rysum::ConcertLink::Role::listener)
            any = concert.playAudioInto(inBuf.getWritePointer(0), inBuf.getWritePointer(1), n, concertBlockTime,
                                        concertSr);
        inputThisBlock = any;
        if (!any)
            engine.setInput(nullptr, nullptr, 0);
        else
            engine.setInput(inBuf.getReadPointer(0), inBuf.getReadPointer(1), n);
    }

    {
        // Fluestern: where the head looks (as the decoder below turns it)
        float yaw = 0.0f;
        if (binaural)
        {
            yaw = haveModVals ? modVals[(size_t)ix.binYaw] : binYawParam->load();
            if (engine.viewWalkOn.load())
                yaw = rysum::wrapAz(yaw + engine.viewYaw.load());
        }
        engine.headYaw = yaw;
        engine.nearHeard = buffer.getNumChannels() > 1 && (binaural || (params.outputMode == 0 && !speakers));
    }
    runVox(midi, n, params); // the Vox humana, and the notes the input plays (into `midi`)
    engine.process(work, midi, params);
    engine.setInput(nullptr, nullptr, 0);
    engine.setVox(nullptr, 0, 0);
    if (concertRole == rysum::ConcertLink::Role::performer)
    {
        // the clocks that move things, as they are at the end of this block
        std::array<double, rysum::kNumStops> mph{};
        std::array<double, rysum::kNumLfos> lph{};
        for (int s = 0; s < rysum::kNumStops; ++s)
            mph[(size_t)s] = engine.motionPhase[(size_t)s].load();
        for (int i = 0; i < rysum::kNumLfos; ++i)
            lph[(size_t)i] = engine.lfoPhaseOut[(size_t)i].load();
        concert.captureClocks(concertAudioTime, mph.data(), lph.data(),
                              params.autoBeats + buffer.getNumSamples() / concertSr * params.autoBeatRate,
                              params.autoBeatRate > 0.0);
    }

    // ---- room: dry level + convolution with the loaded impulse response

    const bool stereo = params.outputMode == 0;
    const int used = juce::jmin(work.getNumChannels(),
                                stereo ? 2 : (params.outputMode + 1) * (params.outputMode + 1));
    const float dryDb = haveModVals ? modVals[(size_t)ix.irDry] : irDry->load();
    const float wetDb = haveModVals ? modVals[(size_t)ix.irWet] : irWet->load();
    const float dryTarget = dryDb <= -59.9f ? 0.0f : dbToGain(dryDb);
    const float wetTarget = wetDb <= -59.9f ? 0.0f : dbToGain(wetDb);

    if (irActive != nullptr && used > 0 &&
        !params.room.on) // a simulated room replaces the impulse response
    {
        // sends to the room (sum + direction harmonics, far pipes relatively louder); wet level
        // ramped here
        const int numIn = irActive->getInfo().numInputs;
        const int chunk = (int)irInputs[0].size();
        for (int start = 0; start < n; start += chunk)
        {
            const int m = juce::jmin(chunk, n - start);
            const float *ins[rysum::OrganEngine::kMaxRoomSends]{};
            const float wStep = (wetTarget - wetSmoothed) / (float)n;
            for (int k = 0; k < numIn; ++k)
            {
                const float *a = engine.getRoomSend(k) + start;
                float *dst = irInputs[(size_t)k].data();
                for (int i = 0; i < m; ++i)
                    dst[i] = a[i] * (wetSmoothed + wStep * (float)(start + i));
                ins[k] = dst;
            }
            // apply the dry level to this chunk before the wet signal is added
            const float d0 = drySmoothed + (dryTarget - drySmoothed) * (float)start / (float)n;
            const float d1 =
                drySmoothed + (dryTarget - drySmoothed) * (float)(start + m) / (float)n;
            for (int c = 0; c < used; ++c)
                work.applyGainRamp(c, start, m, d0, d1);
            float *outs[rysum::kMaxOutChannels]{};
            for (int c = 0; c < used; ++c)
                outs[c] = work.getWritePointer(c, start);
            irActive->process(ins, m, outs, used, 1.0f);
        }
    }
    else if (std::abs(dryTarget - 1.0f) > 1.0e-6f || std::abs(drySmoothed - 1.0f) > 1.0e-6f)
    {
        for (int c = 0; c < used; ++c)
            work.applyGainRamp(c, 0, n, drySmoothed, dryTarget);
    }
    drySmoothed = dryTarget;
    wetSmoothed = wetTarget;

    // ---- Lucier as a piece: recording, and the generations played back (mono: W / the middle of
    // L+R)
    if (used > 0 && n <= (int)lucRecScratch.size())
    {
        const bool fromInput =
            juce::roundToInt(apvts.getRawParameterValue("lucRecSrc")->load()) == 1;
        float *rec = lucRecScratch.data();
        if (fromInput)
        {
            if (inputThisBlock)
                for (int i = 0; i < n; ++i)
                    rec[i] = 0.5f * (inBuf.getSample(0, i) + inBuf.getSample(1, i));
            else
                std::fill_n(rec, n, 0.0f);
        }
        else if (stereo && used > 1)
            for (int i = 0; i < n; ++i)
                rec[i] = 0.5f * (work.getSample(0, i) + work.getSample(1, i));
        else
            std::copy_n(work.getReadPointer(0), n, rec);
        float gains[rysum::kMaxOutChannels]{};
        if (stereo)
        {
            gains[0] = used > 1 ? 0.7071f : 1.0f;
            if (used > 1)
                gains[1] = 0.7071f;
        }
        else
            gains[0] = 1.0f;
        lucPiece.process(rec, work.getArrayOfWritePointers(), gains, used, n,
                         apvts.getRawParameterValue("lucMix")->load());
    }

    // ---- Doppelbelichtung / Belichtung: on the whole field (after the room), before it wears away
    if (used > 0)
        engine.applyPhotoFx(work.getArrayOfWritePointers(), params.outputMode, n, params);

    // ---- "Raumverfall": the spatial image wears away (after the impulse response, so the whole
    // sphere does)
    if (!stereo && used > 0)
        engine.applySpaceDegradation(work.getArrayOfWritePointers(), params.outputMode, n, params);

    // ---- Fluestern (the two ear signals of pipes right at the head) and Zeit als Entfernung (the
    // far end of the room plays the past: everything, the ear signals too, read from a memory)
    bool nearAfterDecode = false;
    {
        if (auto pending = std::atomic_exchange(&pendingTime, std::shared_ptr<rysum::TimeDistance>()))
            timeActive = std::move(pending); // the old one is still referenced by timeKeep
        const bool twoOut = buffer.getNumChannels() > 1;
        const float *nl = engine.nearBus(0), *nr = engine.nearBus(1);
        if (nl != nullptr && stereo && !binaural && !speakers && used > 1)
        {
            // plain stereo: straight into left and right
            juce::FloatVectorOperations::add(work.getWritePointer(0), nl, n);
            juce::FloatVectorOperations::add(work.getWritePointer(1), nr, n);
        }
        const float depth = params.live.timeDepth;
        const bool armed = depth > 0.005f;
        float *chans[rysum::kMaxOutChannels + 2]{};
        int nc = 0;
        for (; nc < used; ++nc)
            chans[nc] = work.getWritePointer(nc);
        const bool timeRuns = timeActive != nullptr && timeActive->numChannels() >= used + 2 &&
                              (armed || timeActive->secondsNow() > 0.0f);
        if (binaural && twoOut && (nl != nullptr || timeRuns))
        {
            // (after the decoding - through the same memory, so they are of the same moment)
            if (nl == nullptr)
            {
                std::fill_n(engine.nearBusWritable(0), n, 0.0f);
                std::fill_n(engine.nearBusWritable(1), n, 0.0f);
            }
            chans[nc++] = engine.nearBusWritable(0);
            chans[nc++] = engine.nearBusWritable(1);
            nearAfterDecode = true;
        }
        timeChannels = armed ? used + 2 : 0;
        if (timeRuns)
        {
            float back = 0.0f;
            if (armed)
            {
                // from the usual place (the present) to the front wall (the full depth)
                const auto &h = engine.heard[0];
                const float ahead = juce::jmax(1.0f, (1.0f - h.Ly.load()) * h.L.load() - 0.5f);
                back = depth * juce::jlimit(0.0f, 1.0f, engine.listenerYNow.load() / ahead);
            }
            timeActive->process(chans, nc, n, armed, back);
            timeBackNow = timeActive->secondsNow();
        }
        else
            timeBackNow = 0.0f;
    }

    // Add the reference after organ effects, before output decoding. It uses its
    // own level; MIDI follows nominal global tuning, manual Hz is independent.
    DsfReferenceTuning referenceTuning;
    referenceTuning.tuning = params.tuning;
    std::array<double, kNumKeys> recordedHz{};
    if (params.tuning.temperament == 0)
    {
        const int selection = juce::roundToInt(apvts.getRawParameterValue("dsfRegister")->load());
        if (selection == static_cast<int>(kDsfReferenceStops.size()))
        {
            // The nominal reference shares the recorded organ's short octave and compass.
            const auto nominalTuning = makeKeyTuning(kQuarterCommaMeantone);
            for (std::size_t key = 0; key < kPhysKeys.size(); ++key)
                recordedHz[key] = keyboardTuningHz(nominalTuning, kPhysKeys[key]);
        }
        else
        {
            const auto index = static_cast<std::size_t>(
                juce::jlimit(0, static_cast<int>(kDsfReferenceStops.size()) - 1, selection));
            recordedHz = engine.recordedPipeFrequencies(kDsfReferenceStops[index]);
        }
        referenceTuning.recordedHz = &recordedHz;
    }
    referenceTuning.pitchCents = params.pitchCents;
    referenceTuning.mpe = params.mpe;
    referenceTuning.mpeBendRange = params.mpeBendRange;
    if (params.notes != nullptr)
    {
        referenceTuning.noteHz = params.notes->hz.data();
        referenceTuning.mapped = params.notes->mapped.data();
    }
    referenceTone.process(
        work, midi, referenceTuning, apvts.getRawParameterValue("dsfOn")->load() > 0.5f,
        apvts.getRawParameterValue("dsfMidi")->load() > 0.5f,
        apvts.getRawParameterValue("dsfFrequency")->load(),
        apvts.getRawParameterValue("dsfOffset")->load() +
            1200.0 * (juce::roundToInt(apvts.getRawParameterValue("dsfFootage")->load()) - 2) +
            1200.0 * std::log2(apvts.getRawParameterValue("dsfMixtur")->load()),
        apvts.getRawParameterValue("dsfSpectrum")->load(),
        apvts.getRawParameterValue("dsfLevel")->load(), DsfIntegrationMethod::trapezoidal,

        !stereo, juce::roundToInt(apvts.getRawParameterValue("dsfPartials")->load()));

    // ---- the 360° recording: the scene's first order, fixed in the room (before the head's turn
    //      below, and before the headphone or loudspeaker decoder)
    if (pano360.isRecording())
        pano360.pushAudio(work.getArrayOfReadPointers(),
                          params.outputMode >= 1 ? juce::jmin(4, work.getNumChannels()) : 0, n);

    // ---- AmbiX out: looking around in the 3D view turns the scene that is sent out, as the
    //      headphones below hear it (the room included). The view closed: it glides back to the front.
    if (!binaural && !speakers && params.outputMode > 0)
    {
        int order = juce::jmin(params.outputMode, 5);
        while (order > 0 && (order + 1) * (order + 1) > buffer.getNumChannels())
            --order; // (a host bus with fewer channels: only the orders it carries)
        const float yaw = engine.viewWalkOn.load() ? engine.viewYaw.load() : 0.0f;
        if (order > 0)
            ambiViewRot.process(buffer.getArrayOfWritePointers(), n, order, yaw);
    }
    else
        ambiViewRot.reset();

    // ---- binaural decoding (head rotation + HRTF filters)
    if (binaural)
    {
        float yaw = haveModVals ? modVals[(size_t)ix.binYaw] : binYawParam->load();
        if (engine.viewWalkOn.load())
            yaw = rysum::wrapAz(yaw + engine.viewYaw.load()); // looking around in the 3D view
        float *l = buffer.getWritePointer(0);
        if (buffer.getNumChannels() > 1)
            binActive->process(binBuf.getArrayOfWritePointers(), n, l, buffer.getWritePointer(1),
                               yaw);
        else
        {
            // mono host: decode in place pairwise (rare); the right ear goes through the last
            // Ambisonics channel's already consumed samples, see BinauralDecoder::process (reads a
            // sample before writing it)
            float *r = binBuf.getWritePointer(binBuf.getNumChannels() - 1);
            binActive->process(binBuf.getArrayOfWritePointers(), n, l, r, yaw);
            for (int i = 0; i < n; ++i)
                l[i] = 0.5f * (l[i] + r[i]);
        }
        for (int c = 2; c < buffer.getNumChannels(); ++c)
            buffer.clear(c, 0, n);
        if (nearAfterDecode)
        {
            // Fluestern: the pipes right at the head, to the ears themselves
            juce::FloatVectorOperations::add(buffer.getWritePointer(0), engine.nearBusWritable(0), n);
            juce::FloatVectorOperations::add(buffer.getWritePointer(1), engine.nearBusWritable(1), n);
        }
        // transaural: the ear signals become two loudspeaker feeds (each speaker's way to the far
        // ear cancelled)
        if (transaural)
            trCtc.process(buffer.getWritePointer(0), buffer.getWritePointer(1), n);
    }

    // ---- loudspeakers (AllRAD)
    if (speakers)
        spkRenderer.process(binBuf.getArrayOfReadPointers(), binBuf.getNumChannels(),
                            buffer.getArrayOfWritePointers(), buffer.getNumChannels(), n);

    // ---- "Summe": EQ and compressor on everything that goes out
    {
        const auto ms = masterSettingsNow();
        if (ms.eqOn || ms.compOn)
            masterBus.process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), n, ms);
        else
            masterBus.reductionDb.store(0.0f, std::memory_order_relaxed);
    }

    midi.clear();

    // ---- test noise on one loudspeaker (setting up a layout); straight to its channel, about -20
    // dBFS, pink-ish
    {
        const int t = speakerTest.load();
        const auto *d = speakers ? spkRenderer.current() : nullptr;
        if (t >= 0 && d != nullptr && t < (int)d->all.size())
        {
            const int ch = d->all[(size_t)t].channel - 1;
            if (!d->all[(size_t)t].imaginary && ch >= 0 && ch < buffer.getNumChannels())
            {
                float *o = buffer.getWritePointer(ch);
                for (int i = 0; i < n; ++i)
                {
                    const float w = testNoise.bi();
                    testLp1 += 0.05f * (w - testLp1);
                    testLp2 = 0.6f * testLp2 + 0.4f * w;
                    o[i] += 0.1f * (0.7f * testLp2 + 2.5f * testLp1);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// loudspeakers
// ---------------------------------------------------------------------------
juce::File RysumProcessor::speakerDefaultFile()
{
    return getPatchFolder().getParentDirectory().getChildFile("Lautsprecher.json");
}

void RysumProcessor::setSpeakerLayout(const rysum::SpeakerLayout &l, bool rememberAsDefault)
{
    spkLayout = l;
    if (rememberAsDefault)
    {
        speakerDefaultFile().getParentDirectory().createDirectory();
        speakerDefaultFile().replaceWithText(rysum::speakerLayoutToJson(l));
    }
    rebuildSpeakers(true);
}

void RysumProcessor::rebuildSpeakers(bool force)
{
    const int order = 1 + juce::roundToInt(apvts.getRawParameterValue("spkOrder")->load());
    const bool comp = apvts.getRawParameterValue("spkDistComp")->load() > 0.5f;
    const juce::String key = juce::String(order) + (comp ? "c" : "-");
    if (!force && key == spkBuiltKey)
        return;
    spkBuiltKey = key;
    auto d =
        rysum::buildAllRad(spkLayout, order, comp, ++spkSerial); // a few ms: on the message thread
    spkUi = d;
    spkOrderNow = d->error.isEmpty() ? d->order : 0;
    spkKeepAlive.push_back(d);
    std::atomic_store(&pendingSpk, std::shared_ptr<const rysum::AllRadDecoder>(d));
    ++spkVersion;
    ++changeCounter;
}

juce::ValueTree RysumProcessor::speakersState() const
{
    juce::ValueTree t("SPEAKERS");
    t.setProperty("json", rysum::speakerLayoutToJson(spkLayout), nullptr);
    return t;
}

// ---------------------------------------------------------------------------
// impulse response
// ---------------------------------------------------------------------------
void RysumProcessor::applyCcOverrides(EngineParams &p) const
{
    auto get = [this](int i) { return ccPending[(size_t)i].load(); };
    float v;
    if (!std::isnan(v = get(0)))
        p.wind = v;
    for (int s = 0; s < kNumStops; ++s)
    {
        if (!std::isnan(v = get(1 + s)))
            p.stopOn[(size_t)s] = v > 0.5f;
        if (!std::isnan(v = get(1 + kNumStops + s)))
            p.stopWind[(size_t)s] = v;
    }
    if (!std::isnan(v = get(1 + 2 * kNumStops)))
        p.freeze = v > 0.5f;
    if (!std::isnan(v = get(2 + 2 * kNumStops)))
        p.tremDepth = v;
}

bool RysumProcessor::handleLearnedCc(int cc, int channel, int value)
{
    if (cc >= 120)
        return false; // channel mode messages
    lastCc = cc | (channel << 8) | (value << 16);
    return handleTrigger(cc, channel, value);
}

bool RysumProcessor::handleTrigger(int cc, int channel, int value)
{
    // (cc: a controller number, or kNoteCode + note with the velocity as value, 0 = released)
    const bool note = cc >= kNoteCode;
    const bool isDown = note ? value > 0 : value >= 64;
    bool mapped = false;
    int learning = mmLearn.load();
    // (a note is only taken for a button-like assignment, and by its press: playing the organ
    // while a fader waits for its controller must not assign a key to it)
    if (learning >= 0 && learning < kNumMidiMaps &&
        (!note || (isDown && mmToggle[(size_t)learning].load())) &&
        mmLearn.compare_exchange_strong(learning, -1))
    {
        mmCc[(size_t)learning] = cc;
        mmChan[(size_t)learning] = channel;
        mmPressed[(size_t)learning] = isDown;
        if (note)
            noteMaps = mapped = true;
        ++midiMapVersion;
    }
    // a button on screen waits for its MIDI button
    int bl = bmLearn.load();
    if (bl >= 0 && bl < kNumButtonMaps && isDown && bmLearn.compare_exchange_strong(bl, -1))
    {
        bmCode[(size_t)bl] = cc;
        bmChan[(size_t)bl] = channel;
        bmPressed[(size_t)bl] = true; // (this press only assigns)
        if (note)
            noteMaps = true;
        mapped = true;
        ++midiMapVersion;
    }
    for (int i = 0; i < kNumButtonMaps; ++i)
    {
        if (!bmOn[(size_t)i].load() || bmCode[(size_t)i].load() != cc)
            continue;
        const int ch = bmChan[(size_t)i].load();
        if (ch != 0 && ch != channel)
            continue;
        mapped = true;
        if (isDown == bmPressed[(size_t)i])
            continue;
        bmPressed[(size_t)i] = isDown;
        int s1, n1, s2, n2;
        bmFifo.prepareToWrite(1, s1, n1, s2, n2);
        if (n1 > 0)
        {
            bmEvents[(size_t)s1] = i | (isDown ? 256 : 0);
            bmFifo.finishedWrite(1);
        }
    }
    for (int i = 0; i < kNumMidiMaps; ++i)
    {
        const int pi = mmParam[(size_t)i].load();
        if (mmCc[(size_t)i].load() != cc || pi < 0)
            continue;
        const int ch = mmChan[(size_t)i].load();
        if (ch != 0 && ch != channel)
            continue;
        mapped = true;
        const float lo = mmLo[(size_t)i].load(), hi = mmHi[(size_t)i].load();
        if (mmToggle[(size_t)i].load())
        {
            // a button: each press switches to the other end of the range (from where the parameter
            // is now)
            const bool down = isDown, wasDown = mmPressed[(size_t)i];
            mmPressed[(size_t)i] = down;
            if (!down || wasDown)
                continue;
            const float cur = prmList[(size_t)pi]->convertTo0to1(rawList[(size_t)pi]->load());
            const bool atHi = std::abs(cur - hi) < std::abs(cur - lo);
            mmPending[(size_t)i] = atHi ? lo : hi;
        }
        else
            mmPending[(size_t)i] = juce::jlimit(0.0f, 1.0f, lo + (hi - lo) * (float)value / 127.0f);
    }
    return mapped;
}

void RysumProcessor::handleAsyncUpdate()
{
    for (int i = 0; i < kNumMidiMaps; ++i)
    {
        const float n = mmPending[(size_t)i].exchange(std::numeric_limits<float>::quiet_NaN());
        if (std::isnan(n))
            continue;
        if (auto *prm = getParamAt(mmParam[(size_t)i].load()))
            if (std::abs(prm->getValue() - n) > 1.0e-6f)
            {
                prm->beginChangeGesture();
                prm->setValueNotifyingHost(n);
                prm->endChangeGesture();
            }
    }
    for (size_t i = 0; i < ccTargets.size(); ++i)
    {
        const float v = ccPending[i].exchange(std::numeric_limits<float>::quiet_NaN());
        if (std::isnan(v) || ccTargets[i] == nullptr)
            continue;
        ccTargets[i]->beginChangeGesture();
        ccTargets[i]->setValueNotifyingHost(ccTargets[i]->convertTo0to1(v));
        ccTargets[i]->endChangeGesture();
    }
}

void RysumProcessor::loadIr(const juce::File &file)
{
    irFile = file;
    irChosen = true;
    const auto settings = currentIrSettings();
    builtIrKey = irSettingsKey(settings);
    setIrStatus(u8("lade ") + file.getFileName() + u8(" \xe2\x80\xa6"));
    irPool.addJob(
        [this, file, settings, alive = aliveFlag]
        {
            juce::String err;
            std::shared_ptr<rysum::Convolver> cv(
                rysum::Convolver::create(file, settings, err).release());
            juce::MessageManager::callAsync(
                [this, cv, err, file, alive]
                {
                    if (!alive->load() || file != irFile)
                        return;
                    if (cv == nullptr)
                    {
                        setIrStatus(err);
                        return;
                    }
                    retiredIr = irUi;
                    retireCountdown = 20;
                    irUi = cv;
                    std::atomic_store(&pendingIr, cv);
                    const auto &in = cv->getInfo();
                    setIrStatus(in.fileName + u8(" \xc2\xb7 ") + in.description);
                });
        });
}

// ---------------------------------------------------------------------------
// "Halten": strike a note twice quickly -> it stays down until it is struck again
// ---------------------------------------------------------------------------
void RysumProcessor::applyLatch(juce::MidiBuffer &midi, int numSamples)
{
    const bool on = latchOn.load();
    const bool letGo = releaseLatched.exchange(false) || (latchWasOn && !on);
    latchWasOn = on;
    if (!on && !letGo)
    {
        sampleClock += numSamples;
        return;
    }

    latchMidi.clear();
    if (letGo)
        for (int n = 0; n < 128; ++n)
            if (latched[(size_t)n])
            {
                latched[(size_t)n] = false;
                latchMidi.addEvent(juce::MidiMessage::noteOff(1, n), 0);
            }
    const int64_t window = (int64_t)(0.4 * (getSampleRate() > 0 ? getSampleRate() : 48000.0));
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (on && m.isNoteOn())
        {
            const int n = m.getNoteNumber();
            const int64_t t = sampleClock + meta.samplePosition;
            if (latched[(size_t)n])
            {
                // struck again: no new attack, the note ends when this key comes up
                latched[(size_t)n] = false;
                lastOnAt[(size_t)n] = std::numeric_limits<int64_t>::min() / 2;
                continue;
            }
            if (t - lastOnAt[(size_t)n] < window)
                latched[(size_t)n] = true;
            lastOnAt[(size_t)n] = t;
        }
        else if (on && m.isNoteOff() && latched[(size_t)m.getNoteNumber()])
            continue; // held: the key coming up is ignored
        latchMidi.addEvent(m, meta.samplePosition);
    }
    midi.swapWith(latchMidi);
    int count = 0;
    for (auto b : latched)
        count += b ? 1 : 0;
    latchedCount = count;
    sampleClock += numSamples;
}

// ---------------------------------------------------------------------------
// simulated room: ray tracing (message thread starts it, the pool thread runs it)
// ---------------------------------------------------------------------------
rysum::RoomParams RysumProcessor::heardRoom(int room) const
{
    rysum::RoomParams r;
    const auto &h = engine.heard[(size_t)juce::jlimit(0, rysum::kNumRooms - 1, room)];
    r.on = engine.roomOn.load();
    r.width = h.W.load();
    r.length = h.L.load();
    r.height = h.H.load();
    r.size = 1.0f;
    r.listenerX = h.Lx.load();
    r.listenerY = h.Ly.load();
    r.absWalls = h.absW.load();
    r.absFloor = h.absF.load();
    r.highDamp = h.highD.load();
    r.scatter = h.scat.load();
    r.shape = h.shape.load();
    r.shapeAmt = h.shapeAmt.load();
    rysum::finishPlan(r, drawnPlans[(size_t)juce::jlimit(0, rysum::kNumRooms - 1, room)]);
    return r;
}

rysum::RoomParams RysumProcessor::tailRoom(int room) const
{
    auto r = heardRoom(room);
    const float s = juce::jlimit(
        0.2f, 40.0f,
        engine.heard[(size_t)juce::jlimit(0, rysum::kNumRooms - 1, room)].tailScale.load());
    r.width *= s;
    r.length *= s;
    r.height *= s;
    return r;
}

juce::StringArray RysumProcessor::roomShapeNames()
{
    // (new shapes only at the end: the index is stored)
    return {"Quader",
            "L-Form",
            juce::String::fromUTF8("F\xc3\xa4"
                                   "cher"),
            "Sechseck",
            "Achteck",
            "Rund",
            "Kreuz",
            "Apsis",
            "Gezeichnet"};
}

void RysumProcessor::setDrawnPlan(int room, const rysum::FloorPlan &f)
{
    if (room < 0 || room >= rysum::kNumRooms)
        return;
    // (the materials of the surfaces belong to the room: a new outline keeps them)
    auto g = f;
    g.surf = drawnPlans[(size_t)room].surf;
    drawnPlans[(size_t)room] = g;
    engine.setDrawnPlan(room, g);
    ++changeCounter;
}

void RysumProcessor::setSurfaceMaterial(int room, int surf, int material)
{
    if (room < 0 || room >= rysum::kNumRooms || surf >= rysum::kMaxSurf)
        return;
    auto &d = drawnPlans[(size_t)room];
    const auto m = (int8_t)juce::jlimit(-1, rysum::kNumSurfMaterials - 1, material);
    if (surf < 0)
        d.surf.fill(m); // all of them
    else
        d.surf[(size_t)surf] = m;
    engine.setDrawnPlan(room, d);
    ++changeCounter;
}

void RysumProcessor::setWalkHear(bool on)
{
    walkHear = on;
    engine.setMesh(on ? getWalkAcoustics() : nullptr);
    ++changeCounter;
}

void RysumProcessor::applyWalkAcoustics()
{
    // the model's triangles in a tree for ray casts (line of sight, the reverb's rays, walking)
    const auto mesh = getWalkMesh();
    std::shared_ptr<const rysum::MeshAcoustics> ac;
    if (mesh != nullptr && !mesh->tris.empty())
    {
        std::vector<float> v;
        v.reserve(mesh->tris.size() * 9);
        for (const auto &t : mesh->tris)
            for (const auto &q : {t.a, t.b, t.c})
            {
                v.push_back(q.x);
                v.push_back(q.y);
                v.push_back(q.z);
            }
        ac = rysum::MeshAcoustics::build(v.data(), (int)mesh->tris.size(),
                                         {mesh->size.x, mesh->size.y, mesh->size.z}, ++walkAcSerial);
    }
    const juce::ScopedLock sl(walkAcLock);
    walkAcKeep[(size_t)(walkAcKeepPos++ & 3u)] = ac;
    std::atomic_store(&walkAc, ac);
    engine.setMesh(walkHear ? ac : nullptr);
    engine.setBodyMesh(ac); // (the flying pipes bounce off it, heard or not)
    ++changeCounter;
}

static juce::String surfString(const rysum::FloorPlan &f)
{
    juce::StringArray v;
    for (auto m : f.surf)
        v.add(juce::String((int)m));
    return v.joinIntoString(",");
}

static juce::String planString(const rysum::FloorPlan &f)
{
    juce::StringArray pts;
    for (int i = 0; i < f.n; ++i)
        pts.add(juce::String(f.x[(size_t)i], 4) + " " + juce::String(f.y[(size_t)i], 4));
    return pts.joinIntoString(",");
}

static rysum::FloorPlan planFromString(const juce::String &s)
{
    rysum::FloorPlan f;
    f.n = 0;
    juce::StringArray pts;
    pts.addTokens(s, ",", {});
    for (auto &t : pts)
    {
        juce::StringArray v;
        v.addTokens(t, " ", {});
        if (v.size() >= 2 && f.n < rysum::kMaxPlanVerts)
        {
            f.x[(size_t)f.n] = juce::jlimit(0.0f, 1.0f, v[0].getFloatValue());
            f.y[(size_t)f.n] = juce::jlimit(0.0f, 1.0f, v[1].getFloatValue());
            ++f.n;
        }
    }
    if (f.n < 3 || rysum::planSelfIntersects(f))
        return rysum::defaultDrawnPlan();
    return f;
}

juce::String RysumProcessor::roomKey(int room) const
{
    // rounded so that tiny modulation steps do not rebuild all the time (1.5 % in size, 0.004 in
    // absorption)
    const auto r = tailRoom(room);
    auto q = [](float v, float step) { return juce::String(juce::roundToInt(v / step)); };
    auto ql = [](float v)
    { return juce::String(juce::roundToInt(std::log(juce::jmax(0.1f, v)) / 0.015f)); };
    return ql(r.W()) + "|" + ql(r.L()) + "|" + ql(r.H()) + "|" + q(r.absWalls, 0.004f) + "|" +
           q(r.absFloor, 0.004f) + "|" + q(r.highDamp, 0.01f) + "|" + q(r.scatter, 0.02f) + "|" +
           q(engine.roomAir.load(), 0.02f) + "|" +
           juce::String(juce::jmin(2, engine.roomOutOrder.load())) + "|" +
           juce::String(hostRate.load(), 0) +
           (r.box() ? juce::String() : "|" + juce::String(r.shape) + "|" + planString(r.plan)) +
           (r.plan.anySurf() ? "|s" + surfString(r.plan) : juce::String()) +
           // (Saal 1 with a model that is heard: the rays go through the model)
           (room == 0 && walkHear && getWalkAcoustics() != nullptr
                ? "|m" + juce::String(getWalkAcoustics()->serial)
                : juce::String());
}

void RysumProcessor::buildTailNow(int room, const juce::String &key)
{
    builtTailKey[(size_t)room] = key;
    tailBuilding[(size_t)room] = true;
    const auto r = tailRoom(room);
    const float air = engine.roomAir.load();
    const int ord = engine.roomOutOrder.load();
    const double rate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
    const uint32_t serial = ++tailSerial;
    const auto mesh = room == 0 && walkHear ? getWalkAcoustics()
                                            : std::shared_ptr<const rysum::MeshAcoustics>();
    const float meshScale = juce::jlimit(
        0.2f, 40.0f, engine.heard[(size_t)juce::jlimit(0, rysum::kNumRooms - 1, room)].tailScale.load());
    irPool.addJob(
        [this, room, r, air, ord, rate, serial, mesh, meshScale, alive = aliveFlag]
        {
            auto ir = rysum::TailIR::build(r, air, ord, rate, serial, mesh.get(), meshScale);
            juce::MessageManager::callAsync(
                [this, room, ir, alive]
                {
                    if (!alive->load())
                        return;
                    tailBuilding[(size_t)room] = false;
                    tailKeepAlive[(size_t)room].push_back(
                        ir); // the engine's copies are never the last ones
                    std::atomic_store(&pendingTail[(size_t)room], ir);
                    if (room == 0)
                        setRoomStatus("Nachhall: " + juce::String(ir->rays) +
                                      juce::String::fromUTF8(" Strahlen \xc2\xb7 T30 ") +
                                      juce::String(ir->rtLow, 2) + " s tief / " +
                                      juce::String(ir->rtHigh, 2) +
                                      juce::String::fromUTF8(" s hoch \xc2\xb7 Antwort ") +
                                      juce::String((double)ir->length / ir->sampleRate, 1) +
                                      " s, " +
                                      (ir->order == 0 ? juce::String("Stereo")
                                                      : juce::String(ir->order) + ". Ordnung") +
                                      ", " + juce::String(ir->channels) +
                                      juce::String::fromUTF8(" dekorrelierte Kan\xc3\xa4le"));
                });
        });
}

// ---------------------------------------------------------------------------
// Lucier mode: the response of n passes through the room, rebuilt in the background whenever the
// generation (growing by itself, or modulated), the room or the damping changes
// ---------------------------------------------------------------------------
void RysumProcessor::lucierRecord()
{
    if (auto *p = apvts.getParameter("lucMode"))
        p->setValueNotifyingHost(0.0f);
    if (auto *p = apvts.getParameter("lucOn"))
        p->setValueNotifyingHost(1.0f);
    lucPiece.startRecording(300.0);
}

void RysumProcessor::lucierStopRecording()
{
    lucPiece.stopRecording(true);
    saveLucierRecording();
}

void RysumProcessor::saveLucierRecording()
{
    // every new recording is kept as a file (Documents/Rysum/Lucier), so the project can play it
    // again later
    const auto &r = lucPiece.getRecording();
    if (lucPiece.recordingVersion == savedRecordingVersion)
        return;
    savedRecordingVersion = lucPiece.recordingVersion;
    if (r.empty() || savedFromFile)
    {
        savedFromFile = false;
        return;
    }
    auto dir = getPatchFolder().getParentDirectory().getChildFile("Lucier");
    dir.createDirectory();
    auto f = dir.getChildFile("Aufnahme " +
                              juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M-%S") + ".wav");
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = f.createOutputStream();
    if (stream != nullptr)
    {
        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate(lucPiece.recordingRate())
                                 .withNumChannels(1)
                                 .withBitsPerSample(24);
        auto w = wav.createWriterFor(stream, options);
        if (w != nullptr)
        {
            const float *ch[]{r.data()};
            w->writeFromFloatArrays(ch, 1, (int)r.size());
            lucPiece.recordingFile = f.getFullPathName();
        }
    }
}

bool RysumProcessor::loadLucierRecording(const juce::File &f, juce::String &error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(f));
    if (rd == nullptr)
    {
        error = juce::String::fromUTF8("keine lesbare Audiodatei");
        return false;
    }
    if (rd->lengthInSamples > (juce::int64)(rd->sampleRate * 1200.0))
    {
        error = juce::String::fromUTF8("l\xc3\xa4nger als 20 Minuten");
        return false;
    }
    juce::AudioBuffer<float> b((int)rd->numChannels, (int)rd->lengthInSamples);
    rd->read(&b, 0, b.getNumSamples(), 0, true, true);
    std::vector<float> mono((size_t)b.getNumSamples(), 0.0f);
    for (int c = 0; c < b.getNumChannels(); ++c)
        juce::FloatVectorOperations::addWithMultiply(
            mono.data(), b.getReadPointer(c), 1.0f / (float)b.getNumChannels(), b.getNumSamples());
    const double rate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
    if (std::abs(rd->sampleRate - rate) > 0.5)
    {
        // to the host's rate
        std::vector<float> out((size_t)((double)mono.size() * rate / rd->sampleRate), 0.0f);
        juce::LagrangeInterpolator li;
        li.process(rd->sampleRate / rate, mono.data(), out.data(), (int)out.size());
        mono = std::move(out);
    }
    lucPiece.stop();
    lucPiece.setRecording(std::move(mono), rate);
    lucPiece.recordingFile = f.getFullPathName();
    savedFromFile = true;
    return true;
}

void RysumProcessor::exportLucierPiece(const juce::File &f)
{
    lucPiece.exportPiece(f, lucBase, juce::roundToInt(apvts.getRawParameterValue("lucGen")->load()),
                         apvts.getRawParameterValue("lucGap")->load(), piecePool);
}

void RysumProcessor::updateLucier(double dt)
{
    auto raw = [this](const char *id) { return apvts.getRawParameterValue(id)->load(); };
    const bool on = raw("lucOn") > 0.5f;
    // the generation as modulated right now, plus what it has grown by
    float gen = raw("lucGen");
    {
        const int idx = getParamIndex("lucGen");
        const float mn = getModulatedNorm(idx);
        if (!std::isnan(mn))
            gen = prmList[(size_t)idx]->convertFrom0to1(mn);
    }
    const bool piece = raw("lucMode") < 0.5f;
    if (piece)
    {
        gen = (float)juce::jmax(1, lucPiece.playingGen.load());
        lucGrown = 0.0f;
    } // the display follows the piece
    else if (on)
        lucGrown += (float)(raw("lucGrow") / 60.0 * dt);
    lucEffective = juce::jlimit(1.0f, 40.0f, gen + lucGrown);
    if (gen + lucGrown > 40.0f)
        lucGrown = juce::jmax(0.0f, 40.0f - gen);
    if (!on || lucBuilding)
        return;

    const int source = juce::roundToInt(raw("lucSource"));
    const double rate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
    rysum::RoomParams r;
    r.width = raw("simW");
    r.length = raw("simL");
    r.height = raw("simH");
    r.size = raw("simSize");
    r.listenerX = raw("simLx");
    r.listenerY = raw("simLy");
    r.absWalls = raw("simAbs");
    r.absFloor = raw("simAbsFloor");
    r.highDamp = raw("simHigh");
    r.scatter = raw("simScatter");
    r.shape = juce::jlimit(0, rysum::kNumRoomShapes - 1, juce::roundToInt(raw("simShape")));
    r.shapeAmt = raw("simShapeAmt");
    rysum::finishPlan(r, drawnPlans[0]);
    const float air = raw("air");
    auto q = [](float v, float step) { return juce::String(juce::roundToInt(v / step)); };
    const juce::String baseKey =
        source == 1 ? "ir|" + irFile.getFullPathName() + "|" + juce::String(rate, 0)
                    : "saal|" + q(r.W(), 0.05f) + "|" + q(r.L(), 0.05f) + "|" + q(r.H(), 0.05f) +
                          "|" + q(r.listenerX, 0.01f) + "|" + q(r.listenerY, 0.01f) + "|" +
                          q(r.absWalls, 0.004f) + "|" + q(r.absFloor, 0.004f) + "|" +
                          q(r.highDamp, 0.01f) + "|" + q(air, 0.02f) + "|" + juce::String(rate, 0) +
                          (r.box() ? juce::String()
                                   : "|" + juce::String(r.shape) + "|" + planString(r.plan));
    const float tau = raw("lucTime");
    const juce::String key = baseKey + "#" + q(lucEffective, 0.02f) + "|" + q(tau, 0.005f);
    if (key == builtLucKey)
        return;
    if (source == 1 && !irFile.existsAsFile())
    {
        if (lucView.status != "keine Impulsantwort geladen (Seite Raum)")
        {
            lucView.status = "keine Impulsantwort geladen (Seite Raum)";
            ++lucView.version;
        }
        return;
    }
    builtLucKey = key;
    lucBuilding = true;
    const uint32_t serial = ++lucSerial;
    auto base = baseKey == lucBaseKey ? lucBase : nullptr;
    const float g = lucEffective;
    const juce::File file = irFile;
    lucPool.addJob(
        [this, base, baseKey, source, r, air, rate, file, g, tau, serial, alive = aliveFlag]
        {
            auto b = base;
            juce::String err;
            if (b == nullptr)
                b = source == 1 ? rysum::lucierFromFile(file, rate, err)
                                : rysum::lucierFromRoom(r, air, rate);
            std::shared_ptr<rysum::LucierResult> res;
            if (b != nullptr)
                res = std::make_shared<rysum::LucierResult>(rysum::buildLucier(*b, g, tau, serial));
            juce::MessageManager::callAsync(
                [this, b, baseKey, res, err, alive]
                {
                    if (!alive->load())
                        return;
                    lucBuilding = false;
                    if (b == nullptr || res == nullptr)
                    {
                        lucView.status = err;
                        builtLucKey.clear();
                        ++lucView.version;
                        return;
                    }
                    lucBase = b;
                    lucBaseKey = baseKey;
                    lucKeepAlive.push_back(res->ir);
                    std::atomic_store(&pendingLucier, res->ir);
                    lucView.curve = res->curve;
                    lucView.peaks = res->peaks;
                    lucView.generation = res->generation;
                    lucView.what = b->what;
                    lucView.status.clear();
                    ++lucView.version;
                });
        });
}

// ---------------------------------------------------------------------------
// binaural
// ---------------------------------------------------------------------------
juce::String RysumProcessor::binauralKey() const
{
    return sofaFile.getFullPathName() + "|" +
           juce::String(1 + juce::roundToInt(binOrderParam->load())) + "|" +
           juce::String((int)(binDfParam->load() > 0.5f)) + "|" +
           juce::String(hostRate.load() > 0 ? hostRate.load() : 48000.0, 0);
}

void RysumProcessor::setSofaFile(const juce::File &f)
{
    sofaFile = f;
    builtBinKey.clear(); // the timer designs it (only while binaural is selected)
    if (!isBinaural())
        setBinStatus(
            juce::String::fromUTF8("gew\xc3\xa4hlt: ") +
            (f == juce::File() ? juce::String("FABIAN") : f.getFileName()) +
            juce::String::fromUTF8(" \xe2\x80\x93 aktiv, sobald die Ausgabe auf Binaural steht"));
}

void RysumProcessor::rebuildBinaural()
{
    const auto key = binauralKey();
    builtBinKey = key;
    const int order = 1 + juce::roundToInt(binOrderParam->load());
    const bool df = binDfParam->load() > 0.5f;
    const double rate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
    const auto file = sofaFile;
    auto set = (hrirSet != nullptr && hrirSetFile == file)
                   ? hrirSet
                   : std::shared_ptr<const rysum::HrirSet>();
    setBinStatus(juce::String::fromUTF8("berechne Filter \xe2\x80\xa6"));
    irPool.addJob(
        [this, key, order, df, rate, file, set, alive = aliveFlag]
        {
            juce::String err;
            auto useSet = set;
            if (useSet == nullptr)
            {
                std::unique_ptr<rysum::HrirSet> loaded(file == juce::File()
                                                           ? rysum::HrirSet::fabian(err)
                                                           : rysum::HrirSet::fromSofa(file, err));
                useSet.reset(loaded.release());
            }
            std::shared_ptr<rysum::BinauralDecoder> dec;
            if (useSet != nullptr)
                if (auto f = rysum::BinauralFilters::design(*useSet, order, rate, df,
                                                            [alive] { return !alive->load(); }))
                    dec = std::make_shared<rysum::BinauralDecoder>(*f);
            if (useSet != nullptr && dec == nullptr && err.isEmpty())
                err = "zu wenige Richtungen in der HRTF";
            juce::MessageManager::callAsync(
                [this, key, file, useSet, dec, err, alive]
                {
                    if (!alive->load())
                        return;
                    if (useSet != nullptr)
                    {
                        hrirSet = useSet;
                        hrirSetFile = file;
                    }
                    if (key != builtBinKey)
                        return; // settings changed meanwhile
                    if (dec == nullptr)
                    {
                        setBinStatus(
                            (file == juce::File() ? juce::String("FABIAN") : file.getFileName()) +
                            ": " + err);
                        return;
                    }
                    retiredBin = binUi;
                    retireCountdown = 20;
                    binUi = dec;
                    std::atomic_store(&pendingBin, dec);
                    setBinStatus(dec->getDescription() + juce::String::fromUTF8(" \xc2\xb7 ") +
                                 useSet->info);
                });
        });
}

juce::String RysumProcessor::transauralKey() const
{
    auto v = [&](const char *id)
    { return juce::String(apvts.getRawParameterValue(id)->load(), 3); };
    return juce::String::toHexString((juce::pointer_sized_int)hrirSet.get()) + "|" + v("trAngle") +
           "|" + v("trAmount") + "|" + v("trBoost") + "|" + v("trSideEq") + "|" +
           juce::String(hostRate.load() > 0 ? hostRate.load() : 48000.0, 0);
}

void RysumProcessor::rebuildTransaural()
{
    const auto key = transauralKey();
    trBuiltKey = key;
    auto set = hrirSet;
    if (set == nullptr)
        return;
    const float angle = apvts.getRawParameterValue("trAngle")->load(),
                amount = apvts.getRawParameterValue("trAmount")->load();
    const float boost = apvts.getRawParameterValue("trBoost")->load();
    const bool eq = apvts.getRawParameterValue("trSideEq")->load() > 0.5f;
    const double rate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
    irPool.addJob(
        [this, key, set, angle, amount, boost, eq, rate, alive = aliveFlag]
        {
            auto f = rysum::TransauralFilters::design(*set, angle, amount, boost, eq, rate);
            juce::MessageManager::callAsync(
                [this, key, f, alive]
                {
                    if (!alive->load() || key != trBuiltKey || f == nullptr)
                        return;
                    trUi = f;
                    trCtc.load(*f);
                    const juce::ScopedLock sl(statusLock);
                    trStatus = f->description;
                });
        });
}

void RysumProcessor::clearIr()
{
    irFile = juce::File();
    irChosen = true;
    retiredIr = irUi;
    retireCountdown = 20;
    irUi.reset();
    std::atomic_store(&pendingIr, std::shared_ptr<rysum::Convolver>());
    irClearRequest = true;
    setIrStatus(u8("keine \xe2\x80\x93 nur trocken"));
}

// ---------------------------------------------------------------------------
// samples
// ---------------------------------------------------------------------------
void RysumProcessor::loadSamples(const juce::File &folder)
{
    loader.reset();
    samplesFolder = folder;
    loading = true;
    setStatus("Lade Samples aus " + folder.getFullPathName() + " ...");
    loader = std::make_unique<Loader>(*this, folder);
    loader->startThread();
}

juce::File RysumProcessor::getCustomFolder() const
{
    if (customOverride != juce::File())
        return customOverride;
    return samplesFolder != juce::File() ? SampleLibrary::customFolder(samplesFolder)
                                         : juce::File();
}

void RysumProcessor::setCustomFolder(const juce::File &dir)
{
    if (dir == customOverride)
    {
        reloadCustomSounds();
        return;
    }
    customOverride = dir.isDirectory() ? dir : juce::File();
    reloadCustomSounds();
    ++changeCounter;
}

int RysumProcessor::getNumCustomSounds() const
{
    auto lib = std::atomic_load(&uiLibrary);
    return lib != nullptr ? (int)lib->custom.size() : 0;
}

void RysumProcessor::reloadCustomSounds()
{
    auto old = std::atomic_load(&uiLibrary);
    if (old == nullptr || samplesFolder == juce::File())
        return;
    const auto folder = samplesFolder;
    const auto dir = getCustomFolder();
    customReloading = true;
    irPool.addJob(
        [this, old, folder, dir, alive = aliveFlag]
        {
            auto lib = std::make_shared<SampleLibrary>(
                *old); // pipes / clicks are shared, only the own sounds are new
            lib->custom = SampleLibrary::loadCustomDir(dir);
            std::shared_ptr<const SampleLibrary> shared(lib);
            juce::MessageManager::callAsync(
                [this, shared, folder, alive]
                {
                    if (alive->load())
                        onLoaded(shared, {}, folder);
                });
        });
}

void RysumProcessor::onLoaded(std::shared_ptr<const SampleLibrary> lib, const juce::String &err,
                              const juce::File &folder)
{
    loading = false;
    const bool wasCustomReload = std::exchange(customReloading, false);
    if (lib == nullptr)
    {
        setStatus("Keine Samples: " + err);
        ++changeCounter;
        return;
    }
    retiredLibrary = std::atomic_load(&uiLibrary);
    retireCountdown = 20; // ~5 s, then freed on the message thread
    std::atomic_store(&uiLibrary, lib);
    std::atomic_store(&pendingLibrary, lib);
    samplesFolder = folder;
    juce::String s;
    s << lib->loadedPipes << " Pfeifen geladen";
    if (lib->substitutedPipes > 0)
        s << ", " << lib->substitutedPipes << " ersetzt";
    if (!lib->custom.empty())
        s << ", " << (int)lib->custom.size() << juce::String::fromUTF8(" eigene Ger\xc3\xa4usche");
    setStatus(s);
    ++changeCounter;
    // a new instance starts in the measured room: the first built-in impulse response (claps)
    if (!irChosen && irFile == juce::File() && !lib->irs.empty())
        loadIr(lib->irs.front().file);
    // a full load reads the standard folder: a chosen one is read right after
    if (!wasCustomReload && customOverride != juce::File())
        reloadCustomSounds();
}

void RysumProcessor::startAutoLoad()
{
    if (samplesFolder != juce::File() && SampleLibrary::looksLikeSampleFolder(samplesFolder))
    {
        loadSamples(samplesFolder);
        return;
    }
    for (auto &f : SampleLibrary::candidateFolders())
        if (SampleLibrary::looksLikeSampleFolder(f))
        {
            loadSamples(f);
            return;
        }
    setStatus(u8("Samples-Ordner nicht gefunden \xe2\x80\x93 bitte w\xc3\xa4hlen"));
    ++changeCounter;
}

void RysumProcessor::timerCallback()
{
    {
        // Zeit als Entfernung: its memory, once it is wanted (for the channels and the rate in use)
        const int need = timeChannels.load();
        const double rate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
        const auto *have = timeKeep.empty() ? nullptr : timeKeep.back().get();
        if (need > 0 && (have == nullptr || have->numChannels() < need ||
                         std::abs(have->sampleRate() - rate) > 0.5))
        {
            auto t = std::make_shared<rysum::TimeDistance>(need, rate);
            timeBackMax = t->maxSeconds();
            timeKeep.push_back(t);
            std::atomic_store(&pendingTime, t);
            while (timeKeep.size() > 3)
                timeKeep.pop_front();
        }
    }
    if (!autoLoadDone)
    {
        autoLoadDone = true;
        writeFactoryPatches();
        startAutoLoad();
        if (irFile.existsAsFile())
            loadIr(irFile);
    }
    if (retireCountdown > 0 && --retireCountdown == 0)
    {
        retiredLibrary.reset();
        retiredIr.reset();
        retiredBin.reset();
    }
    rebuildSpeakers(false);
    {
        // decoders the audio thread no longer uses (the latest one always stays)
        const uint32_t inUse = spkRenderer.serialInUse.load();
        while (spkKeepAlive.size() > 1 && spkKeepAlive.front()->serial < inUse &&
               std::atomic_load(&pendingSpk) == nullptr)
            spkKeepAlive.pop_front();
        while (spkKeepAlive.size() > 8)
            spkKeepAlive.pop_front();
    }
    // MIDI controller assignments changed (edited, learned, or a project brought its own): the next
    // new instance starts with them
    if (midiMapVersion.load() != savedMapVersion && mmLearn.load() < 0 && bmLearn.load() < 0)
    {
        savedMapVersion = midiMapVersion.load();
        saveMidiMapDefault();
    }
    // simulated room: (re)build the late reverb whenever the room as heard has changed (also while
    // it is modulated: then about four times a second, each new response crossfaded in by the
    // engine)
    if (autoLoadDone && engine.roomOn.load())
    {
        const int used = engine.roomsUsed.load();
        for (int ri = 0; ri < rysum::kNumRooms; ++ri)
        {
            if (!((used >> ri) & 1))
                continue;
            const auto key = roomKey(ri);
            if (key != builtTailKey[(size_t)ri] && !tailBuilding[(size_t)ri])
                buildTailNow(ri, key);
        }
    }
    {
        // responses the engine no longer uses are freed here, not on the audio thread
        for (int ri = 0; ri < rysum::kNumRooms; ++ri)
        {
            const uint32_t inUse = engine.tailSerialInUseR[(size_t)ri].load();
            auto &ka = tailKeepAlive[(size_t)ri];
            while (ka.size() > 1 && ka.front()->serial < inUse)
                ka.pop_front();
        }
        const uint32_t lucInUse = engine.lucierSerialInUse.load();
        while (lucKeepAlive.size() > 1 && lucKeepAlive.front()->serial < lucInUse)
            lucKeepAlive.pop_front();
    }
    if (autoLoadDone)
        updateLucier(getTimerInterval() * 0.001);
    lucPiece.tick(lucBase, juce::roundToInt(apvts.getRawParameterValue("lucGen")->load()),
                  apvts.getRawParameterValue("lucGap")->load(), piecePool, aliveFlag);
    if (lucPiece.state.load() != rysum::LucierPiece::stRecording &&
        lucPiece.recordingVersion != savedRecordingVersion)
        saveLucierRecording();
    // binaural: (re)design the decoder when the mode is on and its settings changed (~0.5 s
    // debounce)
    if (autoLoadDone && isBinaural())
    {
        const auto key = binauralKey();
        if (key != builtBinKey)
        {
            if (binUi == nullptr && builtBinKey.isEmpty())
                rebuildBinaural(); // first time: right away
            else if (key != pendingBinKey)
            {
                pendingBinKey = key;
                binKeyTicks = 0;
            }
            else if (++binKeyTicks >= 2)
            {
                rebuildBinaural();
                pendingBinKey.clear();
            }
        }
    }
    // transaural: the crosstalk filters follow the HRTF set and the settings (~0.5 s debounce)
    if (autoLoadDone && isTransaural() && hrirSet != nullptr)
    {
        const auto key = transauralKey();
        if (key != trBuiltKey)
        {
            if (trUi == nullptr && trBuiltKey.isEmpty())
                rebuildTransaural();
            else if (key != trPendingKey)
            {
                trPendingKey = key;
                trKeyTicks = 0;
            }
            else if (++trKeyTicks >= 2)
            {
                rebuildTransaural();
                trPendingKey.clear();
            }
        }
    }
    // IR settings / output format changed: rebuild the room once the values have settled (~0.5 s)
    if (irFile.existsAsFile() && autoLoadDone)
    {
        const auto key = irSettingsKey(currentIrSettings());
        if (key != builtIrKey)
        {
            if (key != pendingIrKey)
            {
                pendingIrKey = key;
                irKeyTicks = 0;
            }
            else if (++irKeyTicks >= 2)
            {
                loadIr(irFile);
                pendingIrKey.clear();
            }
        }
    }
}

rysum::Convolver::Settings RysumProcessor::currentIrSettings() const
{
    rysum::Convolver::Settings s;
    s.format = irFumaParam->load() > 0.5f
                   ? rysum::Convolver::Format::FuMa
                   : (irN3DParam->load() > 0.5f ? rysum::Convolver::Format::N3D
                                                : rysum::Convolver::Format::AmbiX);
    const int out = (int)output->load();
    s.stereoOut = out == 0;
    s.outOrder = juce::jlimit(
        1, kMaxOrder,
        out == 0 ? 1 : juce::jmin(out, 1 + (int)apvts.getRawParameterValue("irOrder")->load()));
    s.envelop = irEnvelopParam->load();
    s.direct = irDirectParam->load();
    s.hostRate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
    return s;
}

juce::String RysumProcessor::irSettingsKey(const rysum::Convolver::Settings &s)
{
    return juce::String((int)s.format) + "|" + juce::String(s.outOrder) + "|" +
           juce::String((int)s.stereoOut) + "|" + juce::String(s.envelop, 2) + "|" +
           juce::String(s.direct, 2) + "|" + juce::String(s.hostRate, 0);
}

// ---------------------------------------------------------------------------
// layouts / positions
// ---------------------------------------------------------------------------
void RysumProcessor::applyLayout(int stop)
{
    std::array<Pos, kNumKeys> pos{};
    const int t = juce::jlimit(0, (int)tunings.size() - 1,
                               juce::roundToInt(apvts.getRawParameterValue("temperament")->load()));
    generateLayout(layouts[(size_t)stop], stop, pos, &tunings[(size_t)t], tuningStepCents(t));
    layoutTuning = t;
    for (int k = 0; k < kNumKeys; ++k)
        setPipePosition(stop, k, pos[(size_t)k]);
}

void RysumProcessor::followTuningLayouts()
{
    const int t = juce::roundToInt(apvts.getRawParameterValue("temperament")->load());
    if (t == layoutTuning)
        return;
    layoutTuning = t;
    for (int s = 0; s < kNumStops; ++s)
        if ((Layout)layouts[(size_t)s].type == Layout::Tonnetz)
            applyLayout(s);
}

void RysumProcessor::setPipePosition(int stop, int key, Pos p)
{
    engine.baseAz[(size_t)stop][(size_t)key] = wrapAz(p.az);
    engine.baseEl[(size_t)stop][(size_t)key] = juce::jlimit(-90.0f, 90.0f, p.el);
    engine.baseDist[(size_t)stop][(size_t)key] = juce::jlimit(kMinDist, kMaxDist, p.dist);
    ++changeCounter;
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
juce::ValueTree RysumProcessor::makeState(bool forPatch)
{
    auto state = apvts.copyState();
    juce::ValueTree extra("EXTRA");
    if (!forPatch)
    {
        extra.setProperty("samplesFolder", samplesFolder.getFullPathName(), nullptr);
        extra.setProperty("loadPresetTuning", loadPresetTuning.load(), nullptr);
        extra.setProperty("extendedRange", extendedRange.load(), nullptr);
    }
    if (!forPatch)
    {
        // the 3D view: the loaded room model and where one stands in it (the project's, not a
        // patch's)
        juce::ValueTree wk("WALK");
        wk.setProperty("model", walkModelFile.getFullPathName(), nullptr);
        wk.setProperty("zUp", walkZUp, nullptr);
        wk.setProperty("scale", walkScale, nullptr);
        wk.setProperty("hear", walkHear, nullptr);
        wk.setProperty("dark", walkDark, nullptr);
        wk.setProperty("x", walkPose[0].load(), nullptr);
        wk.setProperty("y", walkPose[1].load(), nullptr);
        wk.setProperty("yaw", walkPose[2].load(), nullptr);
        wk.setProperty("pitch", walkPose[3].load(), nullptr);
        {
            juce::String lt;
            for (const auto &l : walkLanterns)
                lt << juce::String(l.x, 3) << ' ' << juce::String(l.y, 3) << ' ' << juce::String(l.z, 3) << ' ';
            wk.setProperty("lanterns", lt.trimEnd(), nullptr);
        }
        for (int slot = 0; slot < (int)walkLayouts.size(); ++slot)
        {
            const auto &lay = walkLayouts[(size_t)slot];
            if (lay.empty())
                continue;
            juce::ValueTree lt("LAYOUT");
            lt.setProperty("slot", slot, nullptr);
            for (int s = 0; s < rysum::kNumStops; ++s)
            {
                if (lay.has[(size_t)s] == 0)
                    continue;
                // "key x y z key x y z ..."
                juce::String text;
                for (int k = 0; k < rysum::kNumKeys; ++k)
                    if (((lay.has[(size_t)s] >> k) & 1u) != 0u)
                    {
                        const auto &p = lay.at[(size_t)s][(size_t)k];
                        text << k << ' ' << juce::String(p.x, 3) << ' ' << juce::String(p.y, 3) << ' '
                             << juce::String(p.z, 3) << ' ';
                    }
                lt.setProperty("s" + juce::String(s), text.trimEnd(), nullptr);
            }
            wk.appendChild(lt, nullptr);
        }
        extra.appendChild(wk, nullptr);
        extra.appendChild(rysum::autoToTree(autoSet), nullptr);
        extra.setProperty("pieceName", pieceName, nullptr);
        extra.setProperty("pieceFile", pieceFile.getFullPathName(), nullptr);
        extra.setProperty("pieceChanged", isPieceChanged(), nullptr);
    }
    else if (rysum::builtInRoomIndex(walkModelFile.getFileName()) >= 0 && getWalkMesh() != nullptr)
    {
        // a patch keeps a built-in room (any computer has it) and the light; a model of one's own
        // stays with the project
        juce::ValueTree s3("SAAL3D");
        s3.setProperty("room", walkModelFile.getFileName(), nullptr);
        s3.setProperty("dark", walkDark, nullptr);
        extra.appendChild(s3, nullptr);
    }
    extra.setProperty("irFile", irFile.getFullPathName(), nullptr);
    extra.setProperty("customFolder", customOverride.getFullPathName(), nullptr);
    if (!forPatch)
        extra.setProperty("sofaFile", sofaFile.getFullPathName(), nullptr);
    extra.setProperty("patch", patchName, nullptr);
    if (forPatch && apvts.getRawParameterValue("inpOn")->load() > 0.5f)
        extra.setProperty("input", true, nullptr); // a patch made for the input
    if (sclText.isNotEmpty())
    {
        // the tuning files themselves (not only their paths): the project / patch works on any
        // computer
        extra.setProperty("scl", sclText, nullptr);
        extra.setProperty("sclName", sclName, nullptr);
        if (kbmText.isNotEmpty())
        {
            extra.setProperty("kbm", kbmText, nullptr);
            extra.setProperty("kbmName", kbmName, nullptr);
        }
    }
    for (int s = 0; s < kNumStops; ++s)
    {
        juce::ValueTree st("STOP");
        st.setProperty("id", kStops[(size_t)s].id, nullptr);
        const auto &L = layouts[(size_t)s];
        st.setProperty("layout", L.type, nullptr);
        st.setProperty("width", L.width, nullptr);
        st.setProperty("center", L.center, nullptr);
        st.setProperty("elevation", L.elevation, nullptr);
        st.setProperty("spread", L.spread, nullptr);
        juce::StringArray az, el, mu, di;
        for (int k = 0; k < kNumKeys; ++k)
        {
            di.add(juce::String(engine.baseDist[(size_t)s][(size_t)k].load(), 2));
            az.add(juce::String(engine.baseAz[(size_t)s][(size_t)k].load(), 2));
            el.add(juce::String(engine.baseEl[(size_t)s][(size_t)k].load(), 2));
            mu.add(engine.muted[(size_t)s][(size_t)k].load() ? "1" : "0");
        }
        st.setProperty("az", az.joinIntoString(","), nullptr);
        st.setProperty("el", el.joinIntoString(","), nullptr);
        st.setProperty("mute", mu.joinIntoString(","), nullptr);
        st.setProperty("dist", di.joinIntoString(","), nullptr);
        st.setProperty("distance", L.distance, nullptr);
        if (!forPatch)
        {
            // Verschleiss: how worn every pipe is belongs to the project (a patch leaves the
            // instrument's age alone)
            juce::StringArray we;
            bool any = false;
            for (int k = 0; k < kNumKeys; ++k)
            {
                const float w = engine.wear[(size_t)s][(size_t)k].load();
                any = any || w > 0.0f;
                we.add(juce::String(w, 4));
            }
            if (any)
                st.setProperty("wear", we.joinIntoString(","), nullptr);
        }
        const auto &dp = drawnPaths[(size_t)s];
        if (!dp.points.empty())
        {
            juce::StringArray pts;
            for (auto &q : dp.points)
                pts.add(juce::String(q.az, 2) + " " + juce::String(q.el, 2) + " " +
                        juce::String(q.t, 3));
            st.setProperty("path", pts.joinIntoString(","), nullptr);
            st.setProperty("pathPingPong", dp.pingPong, nullptr);
            st.setProperty("pathTiming", dp.drawnTiming, nullptr);
        }
        extra.appendChild(st, nullptr);
    }
    {
        juce::ValueTree mod("MOD");
        for (int r = 0; r < kNumRoutes; ++r)
        {
            mod.setProperty("src" + juce::String(r), routeSrc[(size_t)r].load(), nullptr);
            mod.setProperty("tgt" + juce::String(r), targetId(routeTgt[(size_t)r].load()), nullptr);
            if (routeUni[(size_t)r].load())
                mod.setProperty("uni" + juce::String(r), true, nullptr);
        }
        extra.appendChild(mod, nullptr);
    }
    if (!forPatch)
        extra.appendChild(midiMapState(),
                          nullptr); // controller assignments belong to the setup, not to the sound
    if (!forPatch)
        extra.appendChild(speakersState(), nullptr); // so does the loudspeaker layout
    if (!forPatch && lucPiece.recordingFile.isNotEmpty())
        extra.setProperty("lucRecording", lucPiece.recordingFile, nullptr);
    extra.appendChild(scenesState(), nullptr);
    {
        // the drawn floor plans (only the ones that were drawn)
        juce::ValueTree plans("SAALPLAN");
        for (int r = 0; r < rysum::kNumRooms; ++r)
        {
            // (compared without the materials: they are stored on their own)
            auto outline = drawnPlans[(size_t)r];
            outline.surf = rysum::FloorPlan::noSurf();
            if (!(outline == rysum::defaultDrawnPlan()))
                plans.setProperty("room" + juce::String(r), planString(drawnPlans[(size_t)r]),
                                  nullptr);
            if (drawnPlans[(size_t)r].anySurf())
                plans.setProperty("surf" + juce::String(r), surfString(drawnPlans[(size_t)r]),
                                  nullptr);
        }
        extra.appendChild(plans, nullptr);
    }
    if (!forPatch)
    {
        juce::String dg;
        for (int g = 0; g < kNumDiceGroups; ++g)
            dg << (diceGroups[(size_t)g] ? "1" : "0");
        extra.setProperty("diceGroups", dg, nullptr);
    }
    state.removeChild(state.getChildWithName("EXTRA"), nullptr);
    state.appendChild(extra, nullptr);
    return state;
}

void RysumProcessor::getStateInformation(juce::MemoryBlock &destData)
{
    // listening in a concert: what is saved is the listener's own setup, not the player's piece
    if (const auto own = concert.ownSetupWhileListening(); own.isValid())
    {
        if (auto xml = own.createXml())
            copyXmlToBinary(*xml, destData);
        return;
    }
    if (auto xml = makeState(false).createXml())
        copyXmlToBinary(*xml, destData);
}

void RysumProcessor::setStateInformation(const void *data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr)
        return;
    applyState(juce::ValueTree::fromXml(*xml), false);
}

void RysumProcessor::applyState(juce::ValueTree state, bool forPatch)
{
    if (!state.hasType(apvts.state.getType()))
        return;
    auto extra = state.getChildWithName("EXTRA");
    const bool loadTuning = !forPatch || loadPresetTuning.load();
    if (!forPatch)
    {
        loadPresetTuning.store(static_cast<bool>(extra.getProperty("loadPresetTuning", true)));
        extendedRange.store(static_cast<bool>(extra.getProperty("extendedRange", false)));
    }
    const float keepOutput = output->load();
    // kept when a patch loads: headphone settings and the controller setup (MPE) belong to the
    // player, not the sound
    static const char *const keepIds[]{
        "binOrder",    "binYaw",      "binDfEq",   "mpe",          "mpeBend",     "inpOn",
        "spkOrder",    "spkDistComp", "dsfOn",     "dsfFrequency", "dsfSpectrum", "dsfLevel",
        "dsfMethod",   "dsfMidi",     "dsfOffset", "dsfPartials",  "dsfFootage",  "dsfMixtur",
        "dsfRegister", "trAngle",     "trAmount",  "trBoost",      "trSideEq"};
    float keepVals[std::size(keepIds)]{};
    for (size_t i = 0; i < std::size(keepIds); ++i)
        keepVals[i] = apvts.getRawParameterValue(keepIds[i])->load();
    if (forPatch)
    {
        // a patch sets every parameter: ones missing in the file (older patches) go to their
        // defaults
        state = state.createCopy();
        for (auto *prm : prmList)
            if (!state.getChildWithProperty("id", prm->getParameterID()).isValid())
            {
                juce::ValueTree t("PARAM");
                t.setProperty("id", prm->getParameterID(), nullptr);
                t.setProperty("value", prm->convertFrom0to1(prm->getDefaultValue()), nullptr);
                state.appendChild(t, nullptr);
            }
        if (!loadTuning)
            for (const auto *id : {"temperament", "pitch", "mtsContinuous"})
            {
                auto parameter = state.getChildWithProperty("id", id);
                parameter.setProperty("value", apvts.getRawParameterValue(id)->load(), nullptr);
            }
    }
    apvts.replaceState(state);
    if (forPatch)
    {
        // the output format belongs to the track, not to the sound
        if (auto *o = apvts.getParameter("output"))
            o->setValueNotifyingHost(o->convertTo0to1(keepOutput));
        // so do the headphone settings and MPE
        for (size_t i = 0; i < std::size(keepIds); ++i)
            if (auto *o = apvts.getParameter(keepIds[i]))
                o->setValueNotifyingHost(o->convertTo0to1(keepVals[i]));
        // the input stays as it is (it is part of the setup) - unless the patch is made for it:
        // then it comes on
        if ((bool)extra.getProperty("input", false))
            if (auto *o = apvts.getParameter("inpOn"))
                o->setValueNotifyingHost(1.0f);
        // stops the patch does not describe get their default layout
        for (int s = 0; s < kNumStops; ++s)
        {
            bool found = false;
            for (auto st : extra)
                found = found || (st.hasType("STOP") && stopIndexForId(st["id"].toString()) == s);
            if (!found)
            {
                layouts[(size_t)s] = defaultLayout(s);
                applyLayout(s);
                drawnPaths[(size_t)s] = {};
                sendPath(s);
                for (auto &m : engine.muted[(size_t)s])
                    m = false;
            }
        }
    }
    patchName = extra["patch"].toString();
    {
        const auto plans = extra.getChildWithName("SAALPLAN");
        for (int r = 0; r < rysum::kNumRooms; ++r)
        {
            const auto key = "room" + juce::String(r);
            setDrawnPlan(r, plans.isValid() && plans.hasProperty(key)
                                ? planFromString(plans[key].toString())
                                : rysum::defaultDrawnPlan());
            // the materials of the single surfaces (none stored: all as the sliders say)
            auto surf = rysum::FloorPlan::noSurf();
            if (plans.isValid() && plans.hasProperty("surf" + juce::String(r)))
            {
                juce::StringArray v;
                v.addTokens(plans["surf" + juce::String(r)].toString(), ",", {});
                for (int i = 0; i < juce::jmin(v.size(), rysum::kMaxSurf); ++i)
                    surf[(size_t)i] =
                        (int8_t)juce::jlimit(-1, rysum::kNumSurfMaterials - 1, v[i].getIntValue());
            }
            if (surf != drawnPlans[(size_t)r].surf)
            {
                drawnPlans[(size_t)r].surf = surf;
                engine.setDrawnPlan(r, drawnPlans[(size_t)r]);
            }
        }
    }
    if (loadTuning && extra.hasProperty("scl"))
        setScala(extra["scl"].toString(), extra["sclName"].toString(), extra["kbm"].toString(),
                 extra["kbmName"].toString());
    else if (!forPatch && sclText.isNotEmpty())
        setScala({}, {}, {},
                 {}); // a project without a Scala file (a patch without one keeps the current file)

    {
        const auto mod = extra.getChildWithName("MOD");
        for (int r = 0; r < kNumRoutes; ++r)
        {
            const int src = mod.isValid() ? (int)mod.getProperty("src" + juce::String(r), -1) : -1;
            const int tgt = mod.isValid()
                                ? targetFromId(mod.getProperty("tgt" + juce::String(r)).toString())
                                : -1;
            setRoute(r, src >= 0 && src < kNumSources ? src : -1, tgt);
            routeUni[(size_t)r] =
                mod.isValid() && (bool)mod.getProperty("uni" + juce::String(r), false);
        }
    }
    if (extra.isValid())
    {
        for (auto st : extra)
        {
            if (!st.hasType("STOP"))
                continue;
            const int s = stopIndexForId(st["id"].toString());
            if (s < 0)
                continue;
            auto &L = layouts[(size_t)s];
            L.type = (int)st.getProperty("layout", L.type);
            L.width = (float)st.getProperty("width", L.width);
            L.center = (float)st.getProperty("center", L.center);
            L.elevation = (float)st.getProperty("elevation", L.elevation);
            L.spread = (float)st.getProperty("spread", L.spread);
            L.distance = (float)st.getProperty("distance", L.distance);
            const bool hasPositions =
                st.hasProperty("az"); // (a factory patch may give only the layout)
            {
                auto &dp = drawnPaths[(size_t)s];
                dp.points.clear();
                juce::StringArray pts;
                pts.addTokens(st["path"].toString(), ",", {});
                for (auto &t : pts)
                {
                    juce::StringArray v;
                    v.addTokens(t, " ", {});
                    if (v.size() >= 3)
                        dp.points.push_back(
                            {v[0].getFloatValue(), v[1].getFloatValue(), v[2].getFloatValue()});
                }
                dp.pingPong = (bool)st.getProperty("pathPingPong", false);
                dp.drawnTiming = (bool)st.getProperty("pathTiming", false);
                sendPath(s);
            }
            juce::StringArray di;
            di.addTokens(st["dist"].toString(), ",", {});
            for (int k = 0; k < juce::jmin(kNumKeys, di.size()); ++k)
                engine.baseDist[(size_t)s][(size_t)k] =
                    juce::jlimit(kMinDist, kMaxDist, di[k].getFloatValue());
            juce::StringArray az, el, mu;
            az.addTokens(st["az"].toString(), ",", {});
            el.addTokens(st["el"].toString(), ",", {});
            mu.addTokens(st["mute"].toString(), ",", {});
            for (int k = 0; k < kNumKeys; ++k)
            {
                if (k < az.size())
                    engine.baseAz[(size_t)s][(size_t)k] = az[k].getFloatValue();
                if (k < el.size())
                    engine.baseEl[(size_t)s][(size_t)k] = el[k].getFloatValue();
                if (k < mu.size())
                    engine.muted[(size_t)s][(size_t)k] = mu[k] == "1";
            }
            if (!forPatch)
            {
                juce::StringArray we;
                we.addTokens(st["wear"].toString(), ",", {});
                for (int k = 0; k < kNumKeys; ++k)
                    engine.wear[(size_t)s][(size_t)k] =
                        k < we.size() ? juce::jlimit(0.0f, 1.0f, we[k].getFloatValue()) : 0.0f;
            }
            if (!hasPositions)
                applyLayout(s);
        }
        if (!forPatch)
        {
            const auto wk = extra.getChildWithName("WALK");
            if (wk.isValid())
            {
                const bool zUp = (bool)wk.getProperty("zUp", false);
                const float scale = (float)wk.getProperty("scale", 1.0f);
                for (int i = 0; i < 4; ++i)
                    walkPose[(size_t)i] =
                        (float)wk.getProperty(juce::StringArray{"x", "y", "yaw", "pitch"}[i], 0.0f);
                const juce::File mf(wk["model"].toString());
                const bool same = mf == walkModelFile && zUp == walkZUp &&
                                  std::abs(scale - walkScale) < 1.0e-6f && getWalkMesh() != nullptr;
                walkZUp = zUp;
                walkScale = scale;
                walkHear = (bool)wk.getProperty("hear", true);
                walkDark = (bool)wk.getProperty("dark", false);
                walkLanterns.clear();
                {
                    const auto parts = juce::StringArray::fromTokens(wk["lanterns"].toString(), " ", "");
                    for (int i = 0; i + 2 < parts.size() && walkLanterns.size() < 16; i += 3)
                        walkLanterns.push_back({parts[i].getFloatValue(), parts[i + 1].getFloatValue(), parts[i + 2].getFloatValue()});
                }
                walkLayouts = {};
                for (const auto &lt : wk)
                {
                    if (!lt.hasType("LAYOUT"))
                        continue;
                    const int slot = (int)lt.getProperty("slot", -1);
                    if (slot < 0 || slot >= (int)walkLayouts.size())
                        continue;
                    auto &lay = walkLayouts[(size_t)slot];
                    for (int s = 0; s < rysum::kNumStops; ++s)
                    {
                        const auto parts = juce::StringArray::fromTokens(lt["s" + juce::String(s)].toString(), " ", "");
                        for (int i = 0; i + 3 < parts.size(); i += 4)
                        {
                            const int k = parts[i].getIntValue();
                            if (k < 0 || k >= rysum::kNumKeys)
                                continue;
                            lay.has[(size_t)s] |= (uint64_t)1 << k;
                            lay.at[(size_t)s][(size_t)k] = {parts[i + 1].getFloatValue(), parts[i + 2].getFloatValue(),
                                                             parts[i + 3].getFloatValue()};
                        }
                    }
                }
                if (same)
                    setWalkHear(walkHear);
                if (mf == juce::File())
                    clearWalkModel();
                else if (!same)
                {
                    if (!mf.existsAsFile() || loadWalkModel(mf, zUp, scale).isNotEmpty())
                    {
                        // gone (another computer) or unreadable: nothing shown, the name kept so
                        // saving does not lose it
                        clearWalkModel();
                        walkModelFile = mf;
                    }
                }
            }
        }
        const juce::File irf(extra["irFile"].toString());
        if (!forPatch && extra.hasProperty("irFile"))
            irChosen = true; // a project knows its room (also "none"): no built-in default
        if (irf != juce::File() && irf != irFile && irf.existsAsFile())
        {
            irFile = irf;
            if (autoLoadDone)
                loadIr(irf); // otherwise the first timer tick loads it
        }
        else if (irf == juce::File() && irFile != juce::File() && autoLoadDone && !forPatch)
            clearIr(); // patches without IR keep the room
        {
            // own sounds: a project always says which folder; a patch only if it was saved with a
            // chosen one
            const juce::File cf(extra["customFolder"].toString());
            if (cf != juce::File() && cf.isDirectory())
            {
                if (cf != customOverride)
                {
                    customOverride = cf;
                    if (autoLoadDone)
                        reloadCustomSounds();
                }
            }
            else if (!forPatch && customOverride != juce::File())
            {
                customOverride = juce::File();
                if (autoLoadDone)
                    reloadCustomSounds();
            }
        }
        applyScenesState(extra.getChildWithName("SCENES"));
        if (forPatch)
        {
            // (only patches that bring a room change it: the others keep the model and the light)
            const auto s3 = extra.getChildWithName("SAAL3D");
            if (s3.isValid())
                applyPatchRoom(s3);
        }
        if (!forPatch && extra.hasProperty("diceGroups"))
        {
            const auto dg = extra["diceGroups"].toString();
            for (int g = 0; g < juce::jmin((int)kNumDiceGroups, dg.length()); ++g)
                diceGroups[(size_t)g] = dg[g] == '1';
        }
        if (!forPatch)
        {
            const auto mm = extra.getChildWithName("MIDIMAP");
            if (mm.isValid())
                applyMidiMapState(mm);
            // Verlauf: the project's piece (patches leave it as it is)
            autoSet = rysum::autoFromTree(extra.getChildWithName("VERLAUF"));
            ++autoVersion;
            pieceName = extra["pieceName"].toString();
            pieceFile = juce::File(extra["pieceFile"].toString());
            pieceSavedVersion = (bool)extra.getProperty("pieceChanged", false) ? -1 : autoVersion.load();
            autoPublish();
            {
                const juce::File lr(extra["lucRecording"].toString());
                juce::String err;
                if (lr.existsAsFile() && lr.getFullPathName() != lucPiece.recordingFile)
                    loadLucierRecording(lr, err);
            }
            const auto sp = extra.getChildWithName("SPEAKERS");
            rysum::SpeakerLayout sl;
            juce::String err;
            if (sp.isValid() && rysum::speakerLayoutFromJson(sp["json"].toString(), sl, err))
                setSpeakerLayout(sl, false);
            const juce::File sf(extra["sofaFile"].toString());
            if (sf != sofaFile)
                setSofaFile(sf.existsAsFile() ? sf : juce::File());
        }
        const juce::File f(forPatch ? juce::File() : juce::File(extra["samplesFolder"].toString()));
        if (f != juce::File() && f != samplesFolder && SampleLibrary::looksLikeSampleFolder(f))
        {
            samplesFolder = f;
            if (autoLoadDone)
                loadSamples(f); // otherwise the auto-load picks it up
        }
    }
    ++changeCounter;
}

// ---------------------------------------------------------------------------
// drawn trajectories
// ---------------------------------------------------------------------------
void RysumProcessor::setDrawnPath(int stop, std::vector<DrawnPath::Pt> points)
{
    if (stop < 0 || stop >= kNumStops)
        return;
    drawnPaths[(size_t)stop].points = std::move(points);
    sendPath(stop);
    ++changeCounter;
}

void RysumProcessor::setPathOptions(int stop, bool pingPong, bool drawnTiming)
{
    if (stop < 0 || stop >= kNumStops)
        return;
    drawnPaths[(size_t)stop].pingPong = pingPong;
    drawnPaths[(size_t)stop].drawnTiming = drawnTiming;
    sendPath(stop);
    ++changeCounter;
}

void RysumProcessor::sendPath(int stop)
{
    // the drawn points -> the engine's path: unit vectors, at most kMax points, the time (0..1)
    // every point is reached
    const auto &dp = drawnPaths[(size_t)stop];
    MotionPath mp;
    mp.pingPong = dp.pingPong;
    const int count = (int)dp.points.size();
    if (count >= 2)
    {
        const int maxPts = MotionPath::kMax - 1; // room for closing the loop
        const int stepN = juce::jmax(1, (count + maxPts - 1) / maxPts);
        std::vector<DrawnPath::Pt> pts;
        for (int i = 0; i < count; i += stepN)
            pts.push_back(dp.points[(size_t)i]);
        if ((count - 1) % stepN != 0)
            pts.push_back(dp.points.back());
        pts.resize((size_t)juce::jmin((int)pts.size(), maxPts));
        auto vec = [](const DrawnPath::Pt &q, float &x, float &y, float &z)
        {
            const float a = juce::degreesToRadians(q.az), e = juce::degreesToRadians(q.el);
            x = std::cos(e) * std::cos(a);
            y = std::cos(e) * std::sin(a);
            z = std::sin(e);
        };
        const int n = (int)pts.size();
        for (int i = 0; i < n; ++i)
            vec(pts[(size_t)i], mp.x[(size_t)i], mp.y[(size_t)i], mp.z[(size_t)i]);
        const bool loop = !dp.pingPong;
        int m = n;
        if (loop)
        {
            mp.x[(size_t)n] = mp.x[0];
            mp.y[(size_t)n] = mp.y[0];
            mp.z[(size_t)n] = mp.z[0];
            m = n + 1;
        }
        // the weight of every segment: its length (even speed) or the time it took to draw
        std::vector<double> w((size_t)(m - 1), 0.0);
        double totalLen = 0.0, totalT = 0.0;
        for (int i = 0; i + 1 < m; ++i)
        {
            const double dx = mp.x[(size_t)i + 1] - mp.x[(size_t)i],
                         dy = mp.y[(size_t)i + 1] - mp.y[(size_t)i],
                         dz = mp.z[(size_t)i + 1] - mp.z[(size_t)i];
            w[(size_t)i] = std::sqrt(dx * dx + dy * dy + dz * dz);
            totalLen += w[(size_t)i];
            if (i + 1 < n)
                totalT += juce::jmax(0.0f, pts[(size_t)i + 1].t - pts[(size_t)i].t);
        }
        if (dp.drawnTiming && totalT > 1.0e-3)
        {
            // drawing time per segment; the closing segment at the average drawing speed
            const double speed =
                totalLen / juce::jmax(1.0e-6, totalLen - (loop ? w.back() : 0.0)) * totalT;
            for (int i = 0; i + 1 < m; ++i)
                w[(size_t)i] =
                    (i + 1 < n)
                        ? juce::jmax(0.0, (double)(pts[(size_t)i + 1].t - pts[(size_t)i].t)) +
                              1.0e-6
                        : w[(size_t)i] / juce::jmax(1.0e-6, totalLen) * speed;
        }
        double acc = 0.0, sum = 0.0;
        for (double x : w)
            sum += x;
        mp.u[0] = 0.0f;
        for (int i = 0; i + 1 < m; ++i)
        {
            acc += w[(size_t)i];
            mp.u[(size_t)i + 1] = (float)(acc / juce::jmax(1.0e-9, sum));
        }
        mp.n = m;
    }
    engine.setPath(stop, mp);
}

// ---------------------------------------------------------------------------
// free tuning: Scala files, MTS-ESP
// ---------------------------------------------------------------------------
bool RysumProcessor::mtsActive() const
{
    return mts != nullptr && (MTS_HasMaster(mts) || MTS_HasReceivedMTSSysEx(mts));
}

bool RysumProcessor::mtsHasMaster() const
{
    return mts != nullptr && MTS_HasMaster(mts);
}

bool RysumProcessor::setScala(const juce::String &scl, const juce::String &sName,
                              const juce::String &kbm, const juce::String &kName)
{
    if (scl.isEmpty())
    {
        sclText = sclName = kbmText = kbmName = tuningError = {};
        scala = {};
        scalaReady = false;
        return true;
    }
    ScalaTuning t;
    juce::String err;
    if (!ScalaTuning::parseScl(scl, t, err))
    {
        tuningError = sName + ": " + err;
        return false;
    }
    if (kbm.isNotEmpty() && !ScalaTuning::parseKbm(kbm, t, err))
    {
        tuningError = kName + ": " + err;
        return false;
    }
    scala = std::move(t);
    sclText = scl;
    sclName = sName;
    kbmText = kbm.isNotEmpty() ? kbm : juce::String();
    kbmName = kbm.isNotEmpty() ? kName : juce::String();
    tuningError.clear();
    const auto f = scala.frequencies();
    {
        const juce::SpinLock::ScopedLockType sl(scalaLock);
        for (int ch = 0; ch < 16; ++ch)
            for (int n = 0; n < 128; ++n)
            {
                scalaShared.hz[(size_t)(ch * 128 + n)] = (float)f[(size_t)n];
                scalaShared.mapped[(size_t)(ch * 128 + n)] = f[(size_t)n] > 0.0 ? 1 : 0;
            }
        ++scalaVersion;
    }
    scalaReady = true;
    ++changeCounter;
    return true;
}

bool RysumProcessor::loadTuningFile(const juce::File &f)
{
    const auto text = f.loadFileAsString();
    bool ok;
    if (f.hasFileExtension("kbm"))
    {
        if (sclText.isEmpty())
        {
            tuningError = u8("Erst eine Skala (.scl) laden, dann die Tastenzuordnung (.kbm)");
            return false;
        }
        ok = setScala(sclText, sclName, text, f.getFileName());
    }
    else
        ok = setScala(text, f.getFileName(), kbmText, kbmName);
    if (ok)
        if (auto *t = apvts.getParameter("temperament"))
        {
            t->beginChangeGesture();
            t->setValueNotifyingHost(t->convertTo0to1((float)kScalaTuning));
            t->endChangeGesture();
        }
    return ok;
}

void RysumProcessor::clearKbm()
{
    if (sclText.isNotEmpty())
        setScala(sclText, sclName, {}, {});
}

juce::String RysumProcessor::getScalaReferenceText() const
{
    if (!scalaReady.load())
        return "Stimmton C4 (MIDI 60)\n" +
               juce::String(rysum::kScalaDefaultReferenceHz, 6) + " Hz";
    static const char *const names[]{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const auto note = juce::String(names[scala.refNote % 12]) + juce::String(scala.refNote / 12 - 1);
    return "Stimmton " + note + " (MIDI " + juce::String(scala.refNote) + ")\n" +
           juce::String(scala.refFreq, 6) + " Hz";
}

juce::String RysumProcessor::getTuningStatus() const
{
    const int t = juce::roundToInt(extraParams.at("temperament")->load());
    const auto dot = u8(" \xc2\xb7 ");
    juce::String s;
    if (tuningError.isNotEmpty() && t == kScalaTuning)
        return u8("Fehler: ") + tuningError;
    if (t == kScalaTuning)
    {
        if (!scalaReady.load())
            return "Keine Scala-Datei geladen: 12edo, A4 = 440 Hz";
        s = sclName + dot + juce::String(scala.size()) + " Stufen" + dot + "Periode " +
            juce::String(scala.cents.back(), 1) + " ct";
        if (scala.description.isNotEmpty())
            s << "\n" << scala.description;
        if (kbmText.isNotEmpty())
            s << "\nKBM: " << kbmName << "\nMittelnote: MIDI " << scala.middleNote;
        else
            s << "\nStandard: Stufe 0 = C4 (MIDI 60), 12edo-Referenz bei A4 = 440 Hz";
        return s;
    }
    if (t == kMtsTuning)
    {
        if (mts == nullptr)
            return u8("MTS-ESP nicht verf\xc3\xbcgbar \xe2\x80\x93 12edo, A4 = 440 Hz");
        if (MTS_HasMaster(mts) || MTS_HasReceivedMTSSysEx(mts))
            return "MIDI Tuning (MTS-ESP) empfangen";
        return u8("Kein MTS-ESP-Master verbunden, keine MIDI-Stimmung empfangen \xe2\x80\x93 "
                  "12edo, A4 = 440 Hz");
    }
    if (mtsHasMaster())
        return u8("Ein MTS-ESP-Master ist verbunden \xe2\x80\x93 \xe2\x80\x9eMTS-ESP\xe2\x80\x9c "
                  "w\xc3\xa4hlen, um ihm zu folgen");
    return {};
}

// ---------------------------------------------------------------------------
// MIDI learn
// ---------------------------------------------------------------------------
RysumProcessor::MidiMap RysumProcessor::getMidiMap(int i) const
{
    MidiMap m;
    if (i < 0 || i >= kNumMidiMaps)
        return m;
    m.cc = mmCc[(size_t)i].load();
    m.channel = mmChan[(size_t)i].load();
    m.param = mmParam[(size_t)i].load();
    m.lo = mmLo[(size_t)i].load();
    m.hi = mmHi[(size_t)i].load();
    m.toggle = mmToggle[(size_t)i].load();
    return m;
}

void RysumProcessor::setMidiMap(int i, const MidiMap &m)
{
    if (i < 0 || i >= kNumMidiMaps)
        return;
    mmParam[(size_t)i] = -1; // the audio thread skips the slot while it changes
    mmCc[(size_t)i] = juce::jlimit(-1, kNoteCode + 127, m.cc);
    mmChan[(size_t)i] = juce::jlimit(0, 16, m.channel);
    mmLo[(size_t)i] = juce::jlimit(0.0f, 1.0f, m.lo);
    mmHi[(size_t)i] = juce::jlimit(0.0f, 1.0f, m.hi);
    mmToggle[(size_t)i] = m.toggle;
    mmParam[(size_t)i] = m.param >= 0 && m.param < getNumParams() ? m.param : -1;
    updateNoteMaps();
    ++midiMapVersion;
}

void RysumProcessor::clearMidiMap(int i)
{
    if (i < 0 || i >= kNumMidiMaps)
        return;
    int l = i;
    mmLearn.compare_exchange_strong(l, -1);
    setMidiMap(i, MidiMap{});
}

void RysumProcessor::learnMidi(int slot)
{
    if (slot >= 0)
        stopButtonLearn();
    mmLearn = slot >= 0 && slot < kNumMidiMaps ? slot : -1;
    ++midiMapVersion;
}

int RysumProcessor::learnMidiFor(int prmIdx)
{
    if (getParamAt(prmIdx) == nullptr)
        return -1;
    for (int i = 0; i < kNumMidiMaps; ++i)
        if (!getMidiMap(i).used())
        {
            MidiMap m;
            m.param = prmIdx;
            // switches: a button press flips them
            m.toggle = dynamic_cast<juce::AudioParameterBool *>(getParamAt(prmIdx)) != nullptr;
            setMidiMap(i, m);
            learnMidi(i);
            return i;
        }
    return -1;
}

juce::Array<int> RysumProcessor::midiMapsFor(int prmIdx) const
{
    juce::Array<int> r;
    for (int i = 0; i < kNumMidiMaps; ++i)
        if (mmParam[(size_t)i].load() == prmIdx && prmIdx >= 0)
            r.add(i);
    return r;
}

// ---- MIDI buttons -> buttons on screen
RysumProcessor::ButtonMap RysumProcessor::getButtonMap(int i) const
{
    ButtonMap m;
    if (i < 0 || i >= kNumButtonMaps)
        return m;
    m.code = bmCode[(size_t)i].load();
    m.channel = bmChan[(size_t)i].load();
    m.target = bmTarget[(size_t)i];
    return m;
}

void RysumProcessor::setButtonMap(int i, const ButtonMap &m)
{
    if (i < 0 || i >= kNumButtonMaps)
        return;
    bmOn[(size_t)i] = false; // the audio thread skips the slot while it changes
    bmCode[(size_t)i] = juce::jlimit(-1, kNoteCode + 127, m.code);
    bmChan[(size_t)i] = juce::jlimit(0, 16, m.channel);
    bmTarget[(size_t)i] = m.target;
    bmOn[(size_t)i] = m.target.isNotEmpty();
    updateNoteMaps();
    ++midiMapVersion;
}

void RysumProcessor::clearButtonMap(int i)
{
    if (i < 0 || i >= kNumButtonMaps)
        return;
    int l = i;
    bmLearn.compare_exchange_strong(l, -1);
    setButtonMap(i, ButtonMap{});
}

void RysumProcessor::stopButtonLearn()
{
    const int l = bmLearn.exchange(-1);
    if (l < 0)
        return;
    if (bmCode[(size_t)l].load() < 0)
        setButtonMap(l, ButtonMap{}); // nothing was pressed: no empty assignment stays behind
    ++midiMapVersion;
}

int RysumProcessor::learnButton(const juce::String &target)
{
    if (target.isEmpty())
        return -1;
    stopButtonLearn();
    mmLearn = -1;
    for (int i = 0; i < kNumButtonMaps; ++i)
        if (!bmOn[(size_t)i].load())
        {
            ButtonMap m;
            m.target = target;
            setButtonMap(i, m);
            bmLearn = i;
            return i;
        }
    return -1;
}

juce::Array<int> RysumProcessor::buttonMapsFor(const juce::String &target) const
{
    juce::Array<int> r;
    for (int i = 0; i < kNumButtonMaps; ++i)
        if (bmOn[(size_t)i].load() && bmTarget[(size_t)i] == target)
            r.add(i);
    return r;
}

bool RysumProcessor::nextButtonEvent(int &slot, bool &down)
{
    int s1, n1, s2, n2;
    bmFifo.prepareToRead(1, s1, n1, s2, n2);
    if (n1 <= 0)
        return false;
    const int e = bmEvents[(size_t)s1];
    bmFifo.finishedRead(1);
    slot = e & 255;
    down = (e & 256) != 0;
    return true;
}

void RysumProcessor::updateNoteMaps()
{
    bool any = false;
    for (int i = 0; i < kNumMidiMaps; ++i)
        any = any || (mmParam[(size_t)i].load() >= 0 && mmCc[(size_t)i].load() >= kNoteCode);
    for (int i = 0; i < kNumButtonMaps; ++i)
        any = any || (bmOn[(size_t)i].load() && bmCode[(size_t)i].load() >= kNoteCode);
    noteMaps = any;
}

juce::String RysumProcessor::triggerName(int code, int channel)
{
    if (code < 0)
        return {};
    auto s = code >= kNoteCode
                 ? "Note " + juce::MidiMessage::getMidiNoteName(code - kNoteCode, true, true, 4)
                 : "CC " + juce::String(code);
    if (channel > 0)
        s << juce::String::fromUTF8(" \xc2\xb7 K") << channel;
    return s;
}

juce::ValueTree RysumProcessor::midiMapState() const
{
    juce::ValueTree t("MIDIMAP");
    for (int i = 0; i < kNumMidiMaps; ++i)
    {
        const auto m = getMidiMap(i);
        if (!m.used())
            continue;
        juce::ValueTree e("MAP");
        e.setProperty("slot", i, nullptr);
        e.setProperty("cc", m.cc, nullptr);
        e.setProperty("channel", m.channel, nullptr);
        e.setProperty("param",
                      m.param >= 0 ? prmList[(size_t)m.param]->getParameterID() : juce::String(),
                      nullptr);
        e.setProperty("lo", m.lo, nullptr);
        e.setProperty("hi", m.hi, nullptr);
        e.setProperty("toggle", m.toggle, nullptr);
        t.appendChild(e, nullptr);
    }
    for (int i = 0; i < kNumButtonMaps; ++i)
    {
        const auto m = getButtonMap(i);
        if (!m.used() || m.code < 0)
            continue;
        juce::ValueTree e("BUTTON");
        e.setProperty("code", m.code, nullptr);
        e.setProperty("channel", m.channel, nullptr);
        e.setProperty("target", m.target, nullptr);
        t.appendChild(e, nullptr);
    }
    return t;
}

void RysumProcessor::applyMidiMapState(const juce::ValueTree &t)
{
    if (!t.hasType("MIDIMAP"))
        return;
    for (int i = 0; i < kNumMidiMaps; ++i)
        setMidiMap(i, MidiMap{});
    bmLearn = -1;
    for (int i = 0; i < kNumButtonMaps; ++i)
        setButtonMap(i, ButtonMap{});
    int nextButton = 0;
    for (auto e : t)
    {
        if (e.hasType("BUTTON"))
        {
            ButtonMap b;
            b.code = (int)e.getProperty("code", -1);
            b.channel = (int)e.getProperty("channel", 0);
            b.target = e["target"].toString();
            if (b.used() && b.code >= 0 && nextButton < kNumButtonMaps)
                setButtonMap(nextButton++, b);
            continue;
        }
        const int slot = (int)e.getProperty("slot", -1);
        if (slot < 0 || slot >= kNumMidiMaps)
            continue;
        MidiMap m;
        m.cc = (int)e.getProperty("cc", -1);
        m.channel = (int)e.getProperty("channel", 0);
        m.param = getParamIndex(e["param"].toString());
        m.lo = (float)e.getProperty("lo", 0.0f);
        m.hi = (float)e.getProperty("hi", 1.0f);
        m.toggle = (bool)e.getProperty("toggle", false);
        if (m.used())
            setMidiMap(slot, m);
    }
    mmLearn = -1;
}

juce::File RysumProcessor::midiMapFile()
{
    return getPatchFolder().getParentDirectory().getChildFile("MIDI-Regler.xml");
}

void RysumProcessor::saveMidiMapDefault()
{
    if (auto xml = midiMapState().createXml())
    {
        midiMapFile().getParentDirectory().createDirectory();
        xml->writeTo(midiMapFile());
    }
}

// ---------------------------------------------------------------------------
// patches: XML files in Documents/Rysum/Patches
// ---------------------------------------------------------------------------
juce::File RysumProcessor::getPatchFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Rysum")
        .getChildFile("Patches");
}

juce::Array<juce::File> RysumProcessor::getPatchFiles()
{
    auto files = getPatchFolder().findChildFiles(juce::File::findFiles, false, "*.rysum");
    std::sort(files.begin(), files.end(),
              [](const juce::File &a, const juce::File &b)
              {
                  return a.getFileNameWithoutExtension().compareNatural(
                             b.getFileNameWithoutExtension()) < 0;
              });
    return files;
}

juce::String RysumProcessor::cleanPatchName(const juce::String &name)
{
    return juce::File::createLegalFileName(name.trim()).trim();
}

bool RysumProcessor::savePatch(const juce::String &name)
{
    const auto clean = cleanPatchName(name);
    if (clean.isEmpty())
        return false;
    auto folder = getPatchFolder();
    if (!folder.createDirectory())
        return false;
    patchName = clean;
    auto xml = makeState(true).createXml();
    if (xml == nullptr)
        return false;
    const bool ok = xml->writeTo(folder.getChildFile(clean + ".rysum"));
    ++patchVersion;
    return ok;
}

bool RysumProcessor::loadPatch(const juce::File &f)
{
    auto xml = juce::XmlDocument::parse(f);
    if (xml == nullptr)
        return false;
    auto state = juce::ValueTree::fromXml(*xml);
    if (!state.hasType(apvts.state.getType()))
        return false;
    applyState(state, true);
    patchName = f.getFileNameWithoutExtension();
    ++patchVersion;
    return true;
}

bool RysumProcessor::fitSaalToWalkModel()
{
    auto m = getWalkMesh();
    if (m == nullptr)
        return false;
    auto set = [this](const char *key, float v)
    {
        if (auto *prm = apvts.getParameter(roomParamId(0, key)))
            prm->setValueNotifyingHost(prm->convertTo0to1(v));
    };
    set("W", juce::jlimit(2.0f, 40.0f, m->size.x));
    set("L", juce::jlimit(2.0f, 100.0f, m->size.y));
    set("H", juce::jlimit(2.0f, 40.0f, m->size.z));
    set("Size", 1.0f);
    set("Shape", (float)rysum::shapeBox);
    return true;
}

void RysumProcessor::applyPatchRoom(const juce::ValueTree &s3)
{
    if (s3.hasProperty("dark"))
        walkDark = (bool)s3["dark"];
    const int idx = rysum::builtInRoomIndex(s3["room"].toString());
    if (idx >= 0)
    {
        const auto f = rysum::writeBuiltInRoom(
            idx, getPatchFolder().getParentDirectory().getChildFile("Modelle"));
        // (made z up, in metres - as the 3D view's menu loads them)
        const bool same = f == walkModelFile && walkZUp && std::abs(walkScale - 1.0f) < 1.0e-6f &&
                          getWalkMesh() != nullptr;
        if (!same && f.existsAsFile())
            loadWalkModel(f, true, 1.0f);
        if (walkModelFile == f && getWalkMesh() != nullptr)
        {
            if (!walkHear)
                setWalkHear(true);
            fitSaalToWalkModel();
            const auto &room = rysum::builtInRooms()[(size_t)idx];
            for (auto [key, v] : {std::pair<const char *, float>{"Lx", room.listenX},
                                  std::pair<const char *, float>{"Ly", room.listenY}})
                if (auto *prm = apvts.getParameter(roomParamId(0, key)))
                    prm->setValueNotifyingHost(prm->convertTo0to1(v));
            // a new room: one starts at its listening place, looking ahead
            for (auto &q : walkPose)
                q = 0.0f;
        }
    }
    ++walkSerial;
}

void RysumProcessor::writeFactoryPatches()
{
    auto folder = getPatchFolder();
    // written once per factory-set version, so deleted patches stay deleted until a new set comes
    // each patch belongs to a set; only sets newer than the last one written are added
    // (deleted patches stay deleted, edited ones are not overwritten)
    constexpr int kFactoryVersion = 18;
    const auto marker = folder.getChildFile(".factory_written");
    const int written = marker.existsAsFile() ? marker.loadFileAsString().trim().getIntValue() : 0;
    if (written >= kFactoryVersion)
        return;
    int currentSet = 1;
    if (!folder.createDirectory())
        return;

    struct Route
    {
        int src;
        juce::String tgt;
        float amt;
        bool uni = false;
    };
    using Values = std::vector<std::pair<juce::String, double>>;
    // factory patches revised after their set was written: rewritten once (when the last version
    // written is older than the revision), but only where the file is still there and still holds
    // the old values of what changed - a deleted or edited patch stays as it is
    struct Revision
    {
        juce::String name;
        int version;
        Values oldValues;
    };
    const std::vector<Revision> revisions{
        // v18: the Kalkant's bellows empties within one tread also for a triad (it used to last
        // nearly the whole cycle, a high triad never ran out)
        {u8("148 Kalkant \xe2\x80\x93 Der Atem geht aus"), 18, {{"lvBellows", 0.8}, {"lvBellowsSize", 3.0}}}};
    auto revisedSinceWritten = [&](const juce::String &name)
    {
        for (const auto &rv : revisions)
        {
            if (rv.name != name || written >= rv.version)
                continue;
            auto xml = juce::XmlDocument::parse(folder.getChildFile(name + ".rysum"));
            if (xml == nullptr)
                return false;
            for (const auto &[id, old] : rv.oldValues)
            {
                bool same = false;
                for (auto *e : xml->getChildWithTagNameIterator("PARAM"))
                    if (e->getStringAttribute("id") == id)
                        same = std::abs(e->getDoubleAttribute("value") - old) < 1.0e-3;
                if (!same)
                    return false;
            }
            return true;
        }
        return false;
    };
    // scenes: each one = the patch's values with these changes on top; 'input' = made for the audio
    // input (switches it on) layoutsIn: (stop, layout type) - the stop gets that layout (default
    // width etc.), its pipes placed when the patch loads
    auto makeFull = [&](const juce::String &name, Values values, std::vector<Route> routesIn,
                        std::vector<Values> scenesIn, bool input,
                        std::vector<std::pair<int, int>> layoutsIn = {},
                        const char *room = nullptr, int dark = -1)
    {
        // room: a built-in model's file name for the 3D view (loaded, the Saal fitted to it);
        // dark: 1 = the light off, 0 = on, -1 = as it is
        if (room != nullptr && rysum::builtInRoomIndex(room) < 0)
            factoryWarnings.add(name + ": unknown room " + room);
        for (auto &sc : scenesIn)
            for (auto &kv : sc)
                if (paramIndex.count(kv.first) == 0)
                    factoryWarnings.add(name + ": unknown scene parameter " + kv.first);
        for (auto &kv : values)
            if (paramIndex.count(kv.first) == 0)
                factoryWarnings.add(name + ": unknown parameter " + kv.first);
        for (auto &rt : routesIn)
            if (targetFromId(rt.tgt) < 0)
                factoryWarnings.add(name + ": unknown target " + rt.tgt);
        if (currentSet <= written && !revisedSinceWritten(name))
            return;
        juce::ValueTree state(apvts.state.getType());
        for (auto *prm : prmList)
        {
            float v = prm->convertFrom0to1(prm->getDefaultValue());
            for (auto &kv : values)
                if (kv.first == prm->getParameterID())
                    v = (float)kv.second;
            juce::ValueTree t("PARAM");
            t.setProperty("id", prm->getParameterID(), nullptr);
            t.setProperty("value", v, nullptr);
            state.appendChild(t, nullptr);
        }
        juce::ValueTree extra("EXTRA"), mod("MOD");
        extra.setProperty("patch", name,
                          nullptr); // no IR: a factory patch keeps the room you loaded
        for (int r = 0; r < kNumRoutes; ++r)
        {
            const bool used = r < (int)routesIn.size();
            mod.setProperty("src" + juce::String(r), used ? routesIn[(size_t)r].src : -1, nullptr);
            mod.setProperty("tgt" + juce::String(r),
                            used ? routesIn[(size_t)r].tgt : juce::String(), nullptr);
            if (used && routesIn[(size_t)r].uni)
                mod.setProperty("uni" + juce::String(r), true, nullptr);
            if (used)
                for (int i = 0; i < state.getNumChildren(); ++i)
                    if (state.getChild(i)["id"].toString() == getModAmountId(r))
                        state.getChild(i).setProperty("value", routesIn[(size_t)r].amt, nullptr);
        }
        extra.appendChild(mod, nullptr);
        if (input)
            extra.setProperty("input", true, nullptr);
        if (room != nullptr || dark >= 0)
        {
            juce::ValueTree s3("SAAL3D");
            if (room != nullptr)
                s3.setProperty("room", juce::String(room), nullptr);
            if (dark >= 0)
                s3.setProperty("dark", dark > 0, nullptr);
            extra.appendChild(s3, nullptr);
        }
        for (auto [ls, lt] : layoutsIn)
        {
            auto L = defaultLayout(ls);
            L.type = lt;
            if (lt == (int)Layout::Tonnetz)
            {
                L.width = 120.0f;
                L.elevation = 10.0f;
            }
            juce::ValueTree st("STOP");
            st.setProperty("id", kStops[(size_t)ls].id, nullptr);
            st.setProperty("layout", L.type, nullptr);
            st.setProperty("width", L.width, nullptr);
            st.setProperty("center", L.center, nullptr);
            st.setProperty("elevation", L.elevation, nullptr);
            st.setProperty("spread", L.spread, nullptr);
            st.setProperty("distance", L.distance, nullptr);
            extra.appendChild(st, nullptr);
        }
        if (!scenesIn.empty())
        {
            juce::ValueTree scs("SCENES");
            for (size_t slot = 0; slot < scenesIn.size() && slot < (size_t)kNumScenes; ++slot)
            {
                juce::StringArray kv;
                for (auto *prm : prmList)
                {
                    const auto id = prm->getParameterID();
                    if (!takesPartInScenes(id))
                        continue;
                    float v = prm->convertFrom0to1(prm->getDefaultValue());
                    for (auto &b : values)
                        if (b.first == id)
                            v = (float)b.second;
                    // the routes' amounts belong to every scene - otherwise moving through the
                    // scenes switches them off (including a route that moves the scenes itself)
                    for (size_t r = 0; r < routesIn.size() && r < (size_t)kNumRoutes; ++r)
                        if (id == getModAmountId((int)r))
                            v = routesIn[r].amt;
                    for (auto &c : scenesIn[slot])
                        if (c.first == id)
                            v = (float)c.second;
                    kv.add(id + "=" + juce::String(prm->convertTo0to1(v), 6));
                }
                juce::ValueTree sc("SCENE");
                sc.setProperty("slot", (int)slot, nullptr);
                sc.setProperty("values", kv.joinIntoString(";"), nullptr);
                scs.appendChild(sc, nullptr);
            }
            extra.appendChild(scs, nullptr);
        }
        state.appendChild(extra, nullptr);
        if (auto xml = state.createXml())
            xml->writeTo(folder.getChildFile(name + ".rysum"));
    };
    auto make = [&](const juce::String &name, Values values, std::vector<Route> routesIn)
    { makeFull(name, std::move(values), std::move(routesIn), {}, false); };
    const auto pipe = [](int t) { return pipeTargetId(t); };

    make("01 Init", {}, {});
    make("02 Plenum", {{"stop0", 1}, {"stop2", 1}, {"stop3", 1}, {"stop5", 1}}, {});
    make(u8("03 Welle durch die Pfeifen"),
         {{"stop0", 1},
          {"stop2", 1},
          {"lfo2Rate", 0.35f},
          {"lfo2Spread", 1.0f},
          {"lfo3Rate", 0.21f},
          {"lfo3Spread", 1.0f}},
         {{srcLfo2, pipe(tgtAz), 0.5f}, {srcLfo3, pipe(tgtEl), 0.25f}});
    make("04 Atmende Wolke",
         {{"stop1", 1},
          {"stop3", 1},
          {"env1Attack", 3.0f},
          {"env1Decay", 4.0f},
          {"env1Sustain", 0.4f},
          {"env1Release", 4.0f},
          {"lfo4Spread", 1.0f},
          {"lfo4SpreadMode", 1},
          {"lfo4Rate", 0.6f},
          {"tremOn", 1},
          {"tremRate", 3.2f},
          {"tremDepth", 0.35f}},
         {{srcEnv1, pipe(tgtDist), 0.4f},
          {srcLfo4, pipe(tgtBright), -0.35f},
          {srcLfo4, pipe(tgtLevel), -0.15f}});
    make("05 Verfall",
         {{"stop0", 1},
          {"stop1", 1},
          {"degAmount", 0.6f},
          {"degDropouts", 0.2f},
          {"motion0", 3},
          {"motion1", 3},
          {"lfo1Shape", (float)shSmooth},
          {"lfo1Rate", 0.15f}},
         {{srcLfo1, "degAmount", 0.35f}});
    make(u8("06 Gl\xc3\xa4serne Spr\xc3\xbcnge"),
         {{"stop3", 1},
          {"stop4", 1},
          {"motion3", 5},
          {"motion4", 5},
          {"rate3", 1.5f},
          {"rate4", 2.5f},
          {"depth3", 90},
          {"depth4", 120},
          {"freezeSmear", 0.8f}},
         {{srcRandom, pipe(tgtPitch), 0.01f}});
    // ---- set 2
    currentSet = 2;
    make(u8("07 Praestant 16\xe2\x80\xb2 Grund"), {{"stop7", 1}, {"stop0", 1}, {"stop1", 1}}, {});
    make(u8("08 Plenum mit 16\xe2\x80\xb2"),
         {{"stop7", 1},
          {"stop0", 1},
          {"stop2", 1},
          {"stop3", 1},
          {"stop5", 1},
          {"stop6", 1},
          {"click", 0.35f}},
         {});
    make(u8("09 Tremulant-Fl\xc3\xb6te"),
         {{"stop1", 1}, {"stop2", 1}, {"tremOn", 1}, {"tremRate", 5.2f}, {"tremDepth", 0.45f}}, {});
    make("10 Kreisende Mixtur",
         {{"stop3", 1},
          {"stop5", 1},
          {"motion3", 1},
          {"motion5", 1},
          {"rate3", 0.3f},
          {"rate5", -0.22f},
          {"ellipse3", 0.5f},
          {"ellipse5", 0.5f},
          {"doppler", 1.0f}},
         {});
    make("11 Werckmeister-Plenum",
         {{"temperament", 2}, {"stop0", 1}, {"stop2", 1}, {"stop3", 1}, {"stop5", 1}}, {});
    make("12 Schwebung",
         {{"stop0", 1},
          {"stop2", 1},
          {"detune", 12.0f},
          {"lfo1Rate", 0.13f},
          {"lfo1Spread", 1.0f},
          {"lfo1SpreadMode", 1}},
         {{srcLfo1, pipe(tgtPitch), 0.006f}});
    make("13 Sterbende Orgel",
         {{"stop0", 1},
          {"stop1", 1},
          {"stop2", 1},
          {"wind", 0.55f},
          {"degAmount", 0.35f},
          {"degWobble", 0.5f},
          {"lfo4Shape", (float)shSmooth},
          {"lfo4Rate", 0.09f},
          {"lfo3Shape", (float)shSmooth},
          {"lfo3Rate", 0.05f}},
         {{srcLfo4, "swind0", -0.45f, true},
          {srcLfo3, "swind2", -0.5f, true},
          {srcLfo4, "degAmount", 0.3f, true}});
    make("14 Eingefroren",
         {{"stop7", 1},
          {"stop1", 1},
          {"freeze", 1},
          {"freezeSmear", 0.8f},
          {"freezeRelease", 6.0f},
          {"lfo2Rate", 0.04f},
          {"lfo2Spread", 1.0f}},
         {{srcLfo2, pipe(tgtAz), 0.5f}});
    make(u8("15 Trompeten-Spr\xc3\xbcnge im Takt"),
         {{"stop6", 1},
          {"motion6", 5},
          {"rate6", 2.0f},
          {"depth6", 100.0f},
          {"motionSync", 1},
          {"lfo2Shape", (float)shSquare},
          {"lfo2Sync", 1},
          {"lfo2Div", 5}},
         {{srcLfo2, pipe(tgtLevel), -0.25f, true}});
    make("16 Glockenausklang",
         {{"stop3", 1},
          {"stop4", 1},
          {"env1Attack", 0.005f},
          {"env1Decay", 2.5f},
          {"env1Sustain", 0.0f},
          {"env1Release", 1.0f}},
         {{srcEnv1, pipe(tgtLevel), 0.3f}, {srcEnv1, pipe(tgtBright), 0.3f}});
    make("17 Atem im Tempo",
         {{"stop0", 1},
          {"stop1", 1},
          {"lfo1Sync", 1},
          {"lfo1Div", 2},
          {"lfo1Shape", (float)shTriangle}},
         {{srcLfo1, "swind0", -0.35f, true}, {srcLfo1, "swind1", -0.35f, true}});
    make("18 Tiefe Wolke",
         {{"stop7", 1},
          {"stop1", 1},
          {"lfo3Shape", (float)shSmooth},
          {"lfo3Rate", 0.3f},
          {"lfo3Spread", 1.0f},
          {"lfo3SpreadMode", 1}},
         {{srcRandom, pipe(tgtDist), 0.4f},
          {srcLfo3, pipe(tgtAz), 0.3f},
          {srcRandom, pipe(tgtEl), 0.25f}});

    // ---- set 3: the room that follows the pipes, unipolar modulation, the 16', and some madness
    currentSet = 3;
    // set-2 fix: the bell attack was too hot (+16 dB); rewritten once with +11 dB
    make("16 Glockenausklang",
         {{"stop3", 1},
          {"stop4", 1},
          {"env1Attack", 0.005f},
          {"env1Decay", 2.5f},
          {"env1Sustain", 0.0f},
          {"env1Release", 1.0f}},
         {{srcEnv1, pipe(tgtLevel), 0.3f}, {srcEnv1, pipe(tgtBright), 0.3f}});
    make("19 Raum folgt dem Schwarm",
         {{"stop7", 1},
          {"stop1", 1},
          {"stop2", 1},
          {"motion7", 1},
          {"motion1", 1},
          {"motion2", 1},
          {"rate7", 0.06f},
          {"rate1", -0.11f},
          {"rate2", 0.17f},
          {"ellipse7", 0.3f},
          {"ellipse1", 0.3f},
          {"ellipse2", 0.3f},
          {"irFollow", 1},
          {"irEnvelop", 0.8f},
          {"irDirect", 0.1f},
          {"irWet", 0.0f},
          {"lfo2Rate", 0.2f},
          {"lfo2Spread", 1.0f}},
         {{srcLfo2, pipe(tgtSend), 0.3f}});
    make("20 Kathedrale im Kopf",
         {{"stop7", 1},
          {"stop1", 1},
          {"irFollow", 0},
          {"irEnvelop", 1.0f},
          {"irDirect", 0.0f},
          {"irWet", 6.0f},
          {"irDry", -18.0f},
          {"freeze", 1},
          {"freezeSmear", 0.9f},
          {"freezeRelease", 8.0f}},
         {});
    make("21 Zentrifuge",
         {{"stop0", 1},
          {"stop2", 1},
          {"stop3", 1},
          {"stop6", 1},
          {"motion0", 1},
          {"motion2", 1},
          {"motion3", 1},
          {"motion6", 1},
          {"rate0", 6.0f},
          {"rate2", -7.5f},
          {"rate3", 9.0f},
          {"rate6", -10.0f},
          {"ellipse0", 0.85f},
          {"ellipse2", 0.85f},
          {"ellipse3", 0.85f},
          {"ellipse6", 0.85f},
          {"doppler", 3.0f},
          {"air", 1.0f},
          {"lfo1Shape", (float)shTriangle},
          {"lfo1Rate", 0.05f}},
         {{srcLfo1, "rate0", 0.3f}, {srcLfo1, "rate3", -0.3f}});
    make("22 Zerbrochene Stimmung",
         {{"temperament", 5},
          {"detune", 45.0f},
          {"stop0", 1},
          {"stop1", 1},
          {"stop2", 1},
          {"lfo3Shape", (float)shSmooth},
          {"lfo3Rate", 0.7f},
          {"lfo3Spread", 1.0f},
          {"lfo3SpreadMode", 1}},
         {{srcRandom, pipe(tgtPitch), 0.08f}, {srcLfo3, pipe(tgtPitch), 0.02f}});
    make("23 Taubenschwarm",
         {{"stop3", 1},
          {"stop5", 1},
          {"motion3", 5},
          {"motion5", 5},
          {"rate3", 8.0f},
          {"rate5", 10.0f},
          {"depth3", 180.0f},
          {"depth5", 180.0f},
          {"doppler", 2.0f},
          {"click", 0.6f},
          {"lfo4Shape", (float)shSmooth},
          {"lfo4Rate", 1.3f},
          {"lfo4Spread", 1.0f},
          {"lfo4SpreadMode", 1}},
         {{srcLfo4, pipe(tgtDist), 0.6f}});
    make("24 Absturz",
         {{"stop0", 1},
          {"stop7", 1},
          {"stop6", 1},
          {"env1Delay", 0.4f},
          {"env1Attack", 6.0f},
          {"env1Decay", 1.0f},
          {"env1Sustain", 1.0f},
          {"env1Release", 0.5f}},
         {{srcEnv1, pipe(tgtPitch), -1.0f},
          {srcEnv1, pipe(tgtBright), -0.4f},
          {srcEnv1, "degAmount", 0.6f}});
    make("25 Atem-Maschine",
         {{"stop0", 1},
          {"stop1", 1},
          {"stop2", 1},
          {"stop4", 1},
          {"motionSync", 1},
          {"lfo1Shape", (float)shSquare},
          {"lfo1Sync", 1},
          {"lfo1Div", 5},
          {"lfo2Shape", (float)shSquare},
          {"lfo2Sync", 1},
          {"lfo2Div", 6}},
         {{srcLfo1, "swind0", -1.0f, true},
          {srcLfo1, "swind1", -1.0f, true},
          {srcLfo2, "swind2", -1.0f, true},
          {srcLfo2, "swind4", -1.0f, true}});
    make(u8("26 16\xe2\x80\xb2 Erdbeben"),
         {{"stop7", 1},
          {"stop6", 1},
          {"vol7", 4.0f},
          {"wind", 0.95f},
          {"lfo1Rate", 7.0f},
          {"lfo1Spread", 1.0f},
          {"lfo1SpreadMode", 1}},
         {{srcLfo1, pipe(tgtPitch), 0.004f}, {srcLfo1, pipe(tgtLevel), 0.15f}});
    make("27 Kirche dreht sich",
         {{"stop0", 1},
          {"stop1", 1},
          {"stop2", 1},
          {"stop3", 1},
          {"motion0", 4},
          {"motion1", 4},
          {"motion2", 4},
          {"motion3", 4},
          {"rate0", 0.07f},
          {"rate1", 0.09f},
          {"rate2", 0.11f},
          {"rate3", 0.13f},
          {"irFollow", 1},
          {"irEnvelop", 0.7f},
          {"lfo3Rate", 0.1f},
          {"lfo3Spread", 1.0f}},
         {{srcLfo3, pipe(tgtEl), 0.3f}});
    make("28 Gefrorener Sturm",
         {{"stop1", 1},
          {"stop3", 1},
          {"stop5", 1},
          {"freeze", 1},
          {"freezeSmear", 1.0f},
          {"freezeRelease", 10.0f},
          {"wind", 1.0f},
          {"motion1", 5},
          {"motion3", 5},
          {"motion5", 5},
          {"rate1", 4.0f},
          {"rate3", 4.0f},
          {"rate5", 4.0f},
          {"depth1", 160.0f},
          {"depth3", 160.0f},
          {"depth5", 160.0f},
          {"degAmount", 0.5f},
          {"degDropouts", 0.4f}},
         {});
    make("29 Bitcrush-Liturgie",
         {{"temperament", 2},
          {"stop0", 1},
          {"stop2", 1},
          {"stop6", 1},
          {"degAmount", 0.9f},
          {"degBits", 5.0f},
          {"degRate", 3000.0f},
          {"lfo1Shape", (float)shSteps},
          {"lfo1Sync", 1},
          {"lfo1Div", 6},
          {"lfo2Shape", (float)shSteps},
          {"lfo2Sync", 1},
          {"lfo2Div", 5},
          {"lfo2Spread", 1.0f},
          {"lfo2SpreadMode", 1}},
         {{srcLfo1, "degRate", 0.4f, true},
          {srcLfo1, "degBits", 0.3f, true},
          {srcLfo2, pipe(tgtAz), 0.5f}});
    make("30 Pendel im Takt",
         {{"stop1", 1},
          {"stop2", 1},
          {"motionSync", 1},
          {"motion1", 2},
          {"motion2", 2},
          {"rate1", 0.5f},
          {"rate2", 1.0f},
          {"depth1", 90.0f},
          {"depth2", 90.0f},
          {"lfo1Shape", (float)shTriangle},
          {"lfo1Sync", 1},
          {"lfo1Div", 4},
          {"lfo1Spread", 1.0f}},
         {{srcLfo1, pipe(tgtBright), 0.4f}});
    make("31 Mechanik pur",
         {{"stop1", 1},
          {"vol1", -14.0f},
          {"click", 1.0f},
          {"releaseClick", 1.0f},
          {"clickBlend", 1.0f},
          {"stopPull", 1.0f},
          {"distAmount", 1.0f}},
         {});
    make("32 Glissando-Wolke",
         {{"stop7", 1},
          {"stop1", 1},
          {"stop2", 1},
          {"stop3", 1},
          {"tremOn", 1},
          {"tremRate", 2.0f},
          {"tremDepth", 0.3f},
          {"irEnvelop", 0.9f},
          {"lfo4Shape", (float)shSmooth},
          {"lfo4Rate", 0.15f},
          {"lfo4Spread", 1.0f},
          {"lfo4SpreadMode", 1}},
         {{srcLfo4, pipe(tgtPitch), 0.05f}, {srcLfo4, pipe(tgtAz), 0.4f}});
    make("33 Ferne Prozession",
         {{"stop0", 1},
          {"stop7", 1},
          {"stop1", 1},
          {"air", 1.0f},
          {"irFollow", 1},
          {"irEnvelop", 0.8f},
          {"irDirect", 0.0f},
          {"env2Mode", 1},
          {"env2Attack", 8.0f},
          {"env2Decay", 1.0f},
          {"env2Sustain", 1.0f},
          {"env2Release", 3.0f}},
         {{srcEnv2, pipe(tgtDist), 0.7f}, {srcEnv2, pipe(tgtSend), 0.3f}});
    make("34 Doppler-Karussell",
         {{"stop2", 1},
          {"stop3", 1},
          {"motionSync", 1},
          {"motion2", 1},
          {"motion3", 1},
          {"rate2", 2.0f},
          {"rate3", -4.0f},
          {"ellipse2", 0.9f},
          {"ellipse3", 0.9f},
          {"doppler", 4.0f},
          {"lfo2Shape", (float)shSquare},
          {"lfo2Sync", 1},
          {"lfo2Div", 2}},
         {{srcLfo2, "ellipse2", 0.5f, true}});

    // ---- set 4: the effects page (sounds that are easy to place)
    currentSet = 4;
    make("35 Werkstatt der Orgelbauer",
         {{"stop1", 1},
          {"stop2", 1},
          {"wind", 0.2f},
          {"fxCustLevel", 0.6f},
          {"fxCustPlace", 1},
          {"fxCustScatter", 0.6f},
          {"fxCustUp", 0.5f},
          {"fxPathLevel", 0.5f},
          {"fxLeakLevel", 0.3f},
          {"fxLeakCount", 4}},
         {});
    make("36 Abstrakten-Ballett",
         {{"stop0", 1},
          {"stop2", 1},
          {"stop5", 1},
          {"click", 0.1f},
          {"fxPathLevel", 0.9f},
          {"fxPathTime", 0.45f},
          {"fxPathTicks", 14},
          {"fxPathSpread", 60.0f},
          {"fxPathWait", 1},
          {"motion0", 1},
          {"motion2", 1},
          {"motion5", 1},
          {"rate0", 0.05f},
          {"rate2", -0.07f},
          {"rate5", 0.09f}},
         {});
    make("37 Glut im Holz",
         {{"stop1", 1},
          {"stop7", 1},
          {"fxCrackLevel", 0.7f},
          {"fxCrackRate", 25.0f},
          {"fxCrackLen", 0.4f},
          {"motion1", 3},
          {"motion7", 3},
          {"depth1", 70.0f},
          {"depth7", 50.0f},
          {"air", 0.8f}},
         {});
    make("38 Altes Holz knackt",
         {{"stop0", 1},
          {"stop1", 1},
          {"fxCrackLevel", 1.0f},
          {"fxCrackRate", 1.5f},
          {"fxCrackLen", 6.0f},
          {"degAmount", 0.25f},
          {"degWobble", 0.4f}},
         {});
    make("39 Undichte Lade",
         {{"stop0", 1},
          {"stop1", 1},
          {"swind0", 0.85f},
          {"wind", 0.35f},
          {"fxWindRound", 1},
          {"fxLeakLevel", 0.7f},
          {"fxLeakCount", 6},
          {"fxLeakFreq", 5500.0f},
          {"fxLeakWander", 8.0f}},
         {});
    make("40 Schlangengrube",
         {{"stop4", 1},
          {"stop1", 1},
          {"wind", 0.2f},
          {"fxWindRound", 1},
          {"fxLeakLevel", 1.0f},
          {"fxLeakCount", 8},
          {"fxLeakFreq", 7000.0f},
          {"fxLeakWander", 60.0f},
          {"lfo1Shape", (float)shSmooth},
          {"lfo1Rate", 0.4f}},
         {{srcLfo1, "fxLeakFreq", 0.35f}});
    make("41 Gespaltene Pfeifen",
         {{"stop0", 1},
          {"stop2", 1},
          {"stop3", 1},
          {"stop5", 1},
          {"fxPartSpread", 120.0f},
          {"fxPartSplit", 2.0f}},
         {});
    make(u8("42 Obertonkarussell"),
         {{"stop1", 1},
          {"stop4", 1},
          {"fxPartSpread", 90.0f},
          {"fxPartSplit", 3.0f},
          {"fxPartTurn", 0.5f},
          {"lfo3Shape", (float)shSmooth},
          {"lfo3Rate", 0.08f}},
         {{srcLfo3, "fxPartTurn", 0.6f}});
    make("43 Echo-Kompass",
         {{"stop6", 1},
          {"stop2", 1},
          {"fxEchoLevel", 0.7f},
          {"fxEchoTime", 0.5f},
          {"fxEchoTurn", 90.0f},
          {"fxEchoRepeats", 4},
          {"fxEchoFeedback", 0.7f}},
         {});
    make("44 Spirale nach oben",
         {{"stop3", 1},
          {"stop5", 1},
          {"fxEchoLevel", 0.8f},
          {"fxEchoTime", 0.18f},
          {"fxEchoTurn", 45.0f},
          {"fxEchoTilt", 12.0f},
          {"fxEchoRepeats", 8},
          {"fxEchoFeedback", 0.85f},
          {"fxEchoDamp", 0.15f}},
         {});
    make("45 Tempo-Echos im Kreis",
         {{"stop6", 1},
          {"click", 0.4f},
          {"motionSync", 1},
          {"fxEchoLevel", 0.75f},
          {"fxEchoSync", 1},
          {"fxEchoDiv", 5},
          {"fxEchoRepeats", 8},
          {"fxEchoTurn", 135.0f},
          {"fxEchoFeedback", 0.75f}},
         {});
    make("46 Schrottplatz-Liturgie",
         {{"stop7", 1},
          {"stop6", 1},
          {"fxCustLevel", 1.0f},
          {"fxCustPlace", 3},
          {"fxCustWander", 240.0f},
          {"fxCustScatter", 1.0f},
          {"fxCustPitch", 1.0f},
          {"fxCustUp", 1.0f},
          {"fxCrackLevel", 0.5f},
          {"fxCrackRate", 60.0f},
          {"fxCrackLen", 0.3f},
          {"fxEchoLevel", 0.5f},
          {"fxEchoTurn", -120.0f},
          {"fxEchoTime", 0.27f},
          {"degAmount", 0.4f}},
         {});
    make("47 Geisterorgel",
         {{"stop1", 1},
          {"wind", 0.4f},
          {"fxWindRound", 1},
          {"fxPathLevel", 0.4f},
          {"fxPathTime", 0.6f},
          {"fxPathWait", 1},
          {"fxLeakLevel", 0.4f},
          {"fxEchoLevel", 0.6f},
          {"fxEchoTilt", 30.0f},
          {"fxEchoTurn", 160.0f},
          {"fxEchoTime", 0.7f},
          {"fxEchoDamp", 0.6f},
          {"fxPartSpread", 180.0f},
          {"fxPartTurn", -0.2f},
          {"irEnvelop", 0.9f},
          {"air", 1.0f}},
         {});
    make("48 Uhrwerk",
         {{"stop3", 1},
          {"motionSync", 1},
          {"fxPathLevel", 0.8f},
          {"fxPathTicks", 16},
          {"fxPathSpread", 90.0f},
          {"fxPathTime", 0.5f},
          {"fxCustLevel", 0.6f},
          {"fxCustPlace", 2},
          {"fxCustPitch", 1.0f},
          {"fxCustUp", 1.0f},
          {"fxEchoLevel", 0.6f},
          {"fxEchoSync", 1},
          {"fxEchoDiv", 6},
          {"fxEchoTurn", 30.0f},
          {"fxEchoRepeats", 8},
          {"fxEchoFeedback", 0.9f}},
         {});

    // ---- the simulated, playable room ("Saal"); set 6 = the same patches with levels for the
    // convolution tail
    currentSet = 6;
    make("49 Atmender Saal",
         {{"stop0", 1},
          {"stop1", 1},
          {"stop2", 1},
          {"simOn", 1},
          {"simW", 12.0f},
          {"simL", 28.0f},
          {"simH", 11.0f},
          {"lfo1Rate", 0.06f}},
         {{srcLfo1, "simSize", 0.3f}});
    make(u8("50 Wandernder H\xc3\xb6rer"),
         {{"stop1", 1},
          {"stop3", 1},
          {"simOn", 1},
          {"simW", 20.0f},
          {"simL", 40.0f},
          {"simH", 12.0f},
          {"simAbs", 0.04f},
          {"lfo2Rate", 0.05f},
          {"lfo3Rate", 0.031f},
          {"lfo3Shape", (float)shTriangle}},
         {{srcLfo2, "simLx", 0.45f}, {srcLfo3, "simLy", 0.45f}});
    make("51 Spiegelkabinett",
         {{"stop3", 1},
          {"stop5", 1},
          {"simOn", 1},
          {"simW", 6.0f},
          {"simL", 6.0f},
          {"simH", 6.0f},
          {"simAbs", 0.0f},
          {"simAbsFloor", 0.0f},
          {"simHigh", 0.0f},
          {"simScatter", 0.05f},
          {"simOrder", 2},
          {"simTail", -8.0f},
          {"simEr", -6.0f},
          {"click", 0.5f},
          {"master", -20.0f}},
         {});
    make("52 Kathedrale",
         {{"stop7", 1},
          {"stop0", 1},
          {"stop1", 1},
          {"stop6", 1},
          {"simOn", 1},
          {"simW", 30.0f},
          {"simL", 90.0f},
          {"simH", 32.0f},
          {"simAbs", 0.04f},
          {"simAbsFloor", 0.1f},
          {"simHigh", 0.25f},
          {"simScatter", 0.5f},
          {"simLy", 0.3f}},
         {});
    make("53 Flatterflur",
         {{"stop6", 1},
          {"click", 0.6f},
          {"releaseClick", 0.5f},
          {"simOn", 1},
          {"simW", 2.5f},
          {"simL", 60.0f},
          {"simH", 3.0f},
          {"simAbs", 0.02f},
          {"simAbsFloor", 0.08f},
          {"simScatter", 0.02f},
          {"simLy", 0.2f},
          {"simEr", -3.0f},
          {"master", -19.0f}},
         {});
    make("54 Schrumpfender Raum",
         {{"stop0", 1},
          {"stop1", 1},
          {"simOn", 1},
          {"simSize", 2.5f},
          {"simAbs", 0.03f},
          {"master", -18.0f},
          {"env1Mode", 1},
          {"env1Attack", 8.0f},
          {"env1Decay", 1.0f},
          {"env1Sustain", 1.0f},
          {"env1Release", 4.0f}},
         {{srcEnv1, "simSize", -0.75f}});
    make(u8("55 Glasw\xc3\xa4nde, Samtboden"),
         {{"stop2", 1},
          {"stop4", 1},
          {"motion2", 1},
          {"motion4", 1},
          {"rate2", 0.2f},
          {"rate4", -0.13f},
          {"simOn", 1},
          {"simW", 16.0f},
          {"simL", 22.0f},
          {"simH", 9.0f},
          {"simAbs", 0.0f},
          {"simAbsFloor", 0.9f},
          {"simHigh", 0.05f},
          {"simScatter", 0.03f},
          {"master", -20.0f}},
         {});

    // ---- set 7: wind model "instabil" (starving pipes whistle and stutter)
    currentSet = 7;
    make("56 Sterbender Wind",
         {{"windModel", 1},
          {"stop0", 1},
          {"stop2", 1},
          {"swind0", 0.6f},
          {"swind2", 0.5f},
          {"lfo1Rate", 0.035f},
          {"lfo2Shape", (float)shSmooth},
          {"lfo2Rate", 0.09f}},
         {{srcLfo1, "swind0", 0.45f}, {srcLfo2, "swind2", 0.4f}});
    currentSet = 8; // set 8: steady wind (the edge tones stand still)
    make(u8("57 Pfeifende Gedackte"),
         {{"windModel", 1}, {"stop1", 1}, {"swind1", 0.2f}, {"master", -6.0f}}, {});
    currentSet = 7;
    make(u8("58 Erlahmendes Gebl\xc3\xa4se"),
         {{"windModel", 1},
          {"stop0", 1},
          {"stop1", 1},
          {"stop2", 1},
          {"stop3", 1},
          {"env1Mode", 1},
          {"env1Delay", 1.0f},
          {"env1Attack", 6.0f},
          {"env1Decay", 1.0f},
          {"env1Sustain", 1.0f},
          {"env1Release", 1.5f}},
         {{srcEnv1, "swind0", -0.88f},
          {srcEnv1, "swind1", -0.85f},
          {srcEnv1, "swind2", -0.9f},
          {srcEnv1, "swind3", -0.8f}});

    // ---- set 9: Lucier, expression (MPE / aftertouch), scenes, the audio input
    currentSet = 9;
    const auto pipes = [](int t) { return pipeTargetId(t); };
    make(u8("59 Lucier \xe2\x80\x93 Ich sitze in einem Raum"),
         {{"stop1", 1},
          {"lucOn", 1},
          {"lucGen", 8.0f},
          {"lucMix", 0.6f},
          {"lucTime", 0.4f},
          {"simW", 9.0f},
          {"simL", 12.0f},
          {"simH", 4.0f}},
         {});
    make(u8("60 Lucier \xe2\x80\x93 Wachsende Generationen"),
         {{"stop0", 0},
          {"stop1", 1},
          {"stop2", 1},
          {"lucOn", 1},
          {"lucGen", 1.0f},
          {"lucGrow", 1.5f},
          {"lucMix", 0.75f}},
         {});
    make(u8("61 Lucier \xe2\x80\x93 Kammer aus Glas"),
         {{"stop0", 0},
          {"stop3", 1},
          {"stop5", 1},
          {"lucOn", 1},
          {"lucGen", 22.0f},
          {"lucMix", 0.85f},
          {"lucTime", 0.8f},
          {"simW", 3.2f},
          {"simL", 4.1f},
          {"simH", 2.6f},
          {"simAbs", 0.02f},
          {"simAbsFloor", 0.02f}},
         {});
    make("62 Lucier atmet",
         {{"stop1", 1},
          {"stop7", 1},
          {"lucOn", 1},
          {"lucGen", 10.0f},
          {"lucMix", 0.5f},
          {"lfo1Shape", (float)shSmooth},
          {"lfo1Rate", 0.05f}},
         {{srcLfo1, "lucMix", 0.35f}});
    make("63 Lucier im Dom",
         {{"stop7", 1},
          {"stop1", 1},
          {"simOn", 1},
          {"simW", 30.0f},
          {"simL", 90.0f},
          {"simH", 32.0f},
          {"simAbs", 0.04f},
          {"lucOn", 1},
          {"lucGen", 5.0f},
          {"lucMix", 0.45f}},
         {});

    // expression: MPE pressure / slide - polyphonic and channel aftertouch count as pressure too
    make(u8("64 Druck \xe2\x80\x93 Schwellwerk"),
         {{"stop1", 1}, {"stop2", 1}, {"vol0", -12.0f}, {"vol1", -12.0f}, {"vol2", -12.0f}},
         {{srcPressure, pipes(tgtLevel), 0.33f}, {srcPressure, pipes(tgtBright), 0.3f}});
    make(u8("65 Gleiten \xe2\x80\x93 Pfeifen wandern"), {{"stop0", 0}, {"stop1", 1}, {"stop3", 1}},
         {{srcSlide, pipes(tgtAz), 0.5f}, {srcPressure, pipes(tgtDist), -0.35f}});
    make(u8("66 Druck \xc3\xb6"
            "ffnet den Wind"),
         {{"windModel", 1}, {"stop1", 1}, {"swind0", 0.35f}, {"swind1", 0.35f}},
         {{srcPressure, "swind0", 0.65f}, {srcPressure, "swind1", 0.65f}});
    make("67 Vibrato unter dem Finger",
         {{"stop2", 1}, {"tremOn", 1}, {"tremDepth", 0.0f}, {"tremRate", 5.5f}},
         {{srcPressure, "tremDepth", 0.8f}, {srcSlide, pipes(tgtPitch), 0.02f}});

    // scenes: "Ueberblenden" on the Spieltisch (or a fader / automation) moves from A to D
    makeFull(
        u8("68 Szenen \xe2\x80\x93 Vom Fl\xc3\xbcstern zum Plenum"),
        {{"stop0", 0}, {"stop1", 1}, {"vol1", -6.0f}, {"rate2", 0.08f}}, {},
        {{},
         {{"stop2", 1}, {"motion2", 1}, {"vol1", -3.0f}},
         {{"stop0", 1}, {"stop2", 1}, {"stop3", 1}, {"stop7", 1}, {"motion2", 1}, {"vol1", 0.0f}},
         {{"stop0", 1},
          {"stop2", 1},
          {"stop3", 1},
          {"stop7", 1},
          {"stop5", 1},
          {"stop6", 1},
          {"motion2", 1},
          {"vol1", 0.0f},
          {"click", 0.4f},
          {"irEnvelop", 0.9f}}},
        false);
    makeFull(u8("69 Szenen \xe2\x80\x93 Der Raum \xc3\xb6"
                "ffnet sich"),
             {{"stop1", 1},
              {"stop2", 1},
              {"simOn", 1},
              {"simW", 3.0f},
              {"simL", 4.0f},
              {"simH", 2.5f},
              {"simAbs", 0.3f},
              {"master", -21.0f}},
             {},
             {{},
              {{"simW", 10.0f}, {"simL", 16.0f}, {"simH", 6.0f}, {"simAbs", 0.1f}},
              {{"simW", 22.0f}, {"simL", 45.0f}, {"simH", 15.0f}, {"simAbs", 0.05f}},
              {{"simW", 30.0f},
               {"simL", 90.0f},
               {"simH", 32.0f},
               {"simAbs", 0.03f},
               {"lucOn", 1},
               {"lucGen", 4.0f},
               {"lucMix", 0.4f}}},
             false);
    makeFull(u8("70 Szenen \xe2\x80\x93 Wanderung von selbst"),
             {{"stop1", 1}, {"lfo1Shape", (float)shTriangle}, {"lfo1Rate", 0.02f}},
             {{srcLfo1, "sceneMorph", 1.0f, true}},
             {{},
              {{"motion0", 1},
               {"motion1", 1},
               {"rate0", 0.2f},
               {"rate1", -0.15f},
               {"ellipse0", 0.5f},
               {"ellipse1", 0.5f}},
              {{"motion0", 1},
               {"motion1", 1},
               {"rate0", 0.35f},
               {"rate1", -0.3f},
               {"stop3", 1},
               {"fxEchoLevel", 0.6f},
               {"fxEchoTurn", 120.0f}},
              {{"stop0", 0},
               {"stop7", 1},
               {"stop3", 1},
               {"tremOn", 1},
               {"tremDepth", 0.4f},
               {"degAmount", 0.4f},
               {"degWobble", 0.5f},
               {"motion1", 2},
               {"rate1", 0.1f}}},
             false);
    makeFull(u8("71 Szenen \xe2\x80\x93 Verfall in vier Stufen"), {{"stop1", 1}, {"stop2", 1}}, {},
             {{},
              {{"degAmount", 0.3f}, {"degWobble", 0.4f}},
              {{"degAmount", 0.6f},
               {"degBits", 6.0f},
               {"degRate", 8000.0f},
               {"wind", 0.4f},
               {"degWobble", 0.4f}},
              {{"degAmount", 0.9f},
               {"degDropouts", 0.5f},
               {"degBits", 4.0f},
               {"degRate", 3000.0f},
               {"wind", 0.7f},
               {"degWobble", 0.6f},
               {"windModel", 1}}},
             false);

    // the audio input (switches the input on): voice, instrument, anything - placed and moved with
    // the pipes
    makeFull(u8("72 Eingang \xe2\x80\x93 Stimme kreist"),
             {{"stop0", 0},
              {"stop1", 1},
              {"vol1", -10.0f},
              {"inpMode", 0},
              {"inpSpin", 25.0f},
              {"inpDist", 3.5f},
              {"inpEl", 15.0f},
              {"irEnvelop", 0.6f}},
             {}, {}, true);
    makeFull(u8("73 Eingang \xe2\x80\x93 Auf der Mixtur"),
             {{"stop0", 0},
              {"stop2", 1},
              {"stop5", 1},
              {"motion5", 1},
              {"rate5", 0.12f},
              {"ellipse5", 0.5f},
              {"inpFollow", 6},
              {"inpWidth", 120.0f}},
             {}, {}, true);
    makeFull(u8("74 Eingang \xe2\x80\x93 Ich sitze in einem Raum"),
             {{"stop0", 0},
              {"inpMode", 0},
              {"inpDist", 2.0f},
              {"lucOn", 1},
              {"lucGen", 1.0f},
              {"lucGrow", 2.0f},
              {"lucMix", 0.9f},
              {"simW", 6.0f},
              {"simL", 8.0f},
              {"simH", 3.0f}},
             {}, {}, true);
    makeFull(u8("75 Eingang \xe2\x80\x93 Tasten \xc3\xb6"
                "ffnen die Stimme"),
             {{"vol0", -8.0f},
              {"motion0", 2},
              {"rate0", 0.3f},
              {"depth0", 60.0f},
              {"inpFollow", 1},
              {"inpGate", 1},
              {"inpRelease", 0.6f},
              {"fxEchoLevel", 0.5f},
              {"fxEchoTurn", 90.0f}},
             {}, {}, true);
    makeFull("76 Eingang steuert die Orgel",
             {{"stop1", 1},
              {"stop2", 1},
              {"swind0", 0.4f},
              {"swind1", 0.4f},
              {"swind2", 0.4f},
              {"inpLevel", -6.0f}},
             {{srcInput, "swind0", 0.6f},
              {srcInput, "swind1", 0.6f},
              {srcInput, "swind2", 0.6f},
              {srcInput, pipes(tgtBright), 0.4f}},
             {}, true);
    makeFull("77 Eingang im Spiegelsaal",
             {{"stop0", 0},
              {"stop3", 1},
              {"vol3", -12.0f},
              {"inpSpin", -40.0f},
              {"simOn", 1},
              {"simW", 6.0f},
              {"simL", 6.0f},
              {"simH", 6.0f},
              {"simAbs", 0.0f},
              {"simAbsFloor", 0.0f},
              {"simHigh", 0.0f},
              {"simScatter", 0.05f},
              {"master", -18.0f}},
             {}, {}, true);
    makeFull(u8("78 Eingang \xe2\x80\x93 Echo-Kompass"),
             {{"stop0", 0},
              {"stop1", 1},
              {"vol1", -14.0f},
              {"inpMode", 0},
              {"fxEchoLevel", 0.7f},
              {"fxEchoTurn", 90.0f},
              {"fxEchoRepeats", 6},
              {"fxEchoFeedback", 0.7f},
              {"fxEchoTilt", 15.0f}},
             {}, {}, true);
    makeFull(u8("79 Eingang zerf\xc3\xa4llt"),
             {{"stop0", 0},
              {"stop7", 1},
              {"vol7", -10.0f},
              {"degMode", 1},
              {"degAmount", 0.6f},
              {"degBits", 6.0f},
              {"degRate", 6000.0f},
              {"degDropouts", 0.3f},
              {"inpWidth", 150.0f}},
             {}, {}, true);

    // ---- set 10: Lucier now has two ways - the patches made for the live way say so; 74 is the
    // piece itself
    currentSet = 10;
    make(u8("59 Lucier \xe2\x80\x93 Ich sitze in einem Raum"),
         {{"stop1", 1},
          {"lucOn", 1},
          {"lucMode", 1},
          {"lucGen", 8.0f},
          {"lucMix", 0.6f},
          {"lucTime", 0.4f},
          {"simW", 9.0f},
          {"simL", 12.0f},
          {"simH", 4.0f}},
         {});
    make(u8("60 Lucier \xe2\x80\x93 Wachsende Generationen"),
         {{"stop0", 0},
          {"stop1", 1},
          {"stop2", 1},
          {"lucOn", 1},
          {"lucMode", 1},
          {"lucGen", 1.0f},
          {"lucGrow", 1.5f},
          {"lucMix", 0.75f}},
         {});
    make(u8("61 Lucier \xe2\x80\x93 Kammer aus Glas"),
         {{"stop0", 0},
          {"stop3", 1},
          {"stop5", 1},
          {"lucOn", 1},
          {"lucMode", 1},
          {"lucGen", 22.0f},
          {"lucMix", 0.85f},
          {"lucTime", 0.8f},
          {"simW", 3.2f},
          {"simL", 4.1f},
          {"simH", 2.6f},
          {"simAbs", 0.02f},
          {"simAbsFloor", 0.02f}},
         {});
    make("62 Lucier atmet",
         {{"stop1", 1},
          {"stop7", 1},
          {"lucOn", 1},
          {"lucMode", 1},
          {"lucGen", 10.0f},
          {"lucMix", 0.5f},
          {"lfo1Shape", (float)shSmooth},
          {"lfo1Rate", 0.05f}},
         {{srcLfo1, "lucMix", 0.35f}});
    make("63 Lucier im Dom",
         {{"stop7", 1},
          {"stop1", 1},
          {"simOn", 1},
          {"simW", 30.0f},
          {"simL", 90.0f},
          {"simH", 32.0f},
          {"simAbs", 0.04f},
          {"lucOn", 1},
          {"lucMode", 1},
          {"lucGen", 5.0f},
          {"lucMix", 0.45f}},
         {});
    makeFull(u8("69 Szenen \xe2\x80\x93 Der Raum \xc3\xb6"
                "ffnet sich"),
             {{"stop1", 1},
              {"stop2", 1},
              {"simOn", 1},
              {"simW", 3.0f},
              {"simL", 4.0f},
              {"simH", 2.5f},
              {"simAbs", 0.3f},
              {"master", -21.0f},
              {"lucMode", 1}},
             {},
             {{},
              {{"simW", 10.0f}, {"simL", 16.0f}, {"simH", 6.0f}, {"simAbs", 0.1f}},
              {{"simW", 22.0f}, {"simL", 45.0f}, {"simH", 15.0f}, {"simAbs", 0.05f}},
              {{"simW", 30.0f},
               {"simL", 90.0f},
               {"simH", 32.0f},
               {"simAbs", 0.03f},
               {"lucOn", 1},
               {"lucGen", 4.0f},
               {"lucMix", 0.4f}}},
             false);
    // the piece: press "Aufnehmen" on the Saal page, speak about a minute, press again - 32
    // generations follow by themselves
    makeFull(u8("74 Eingang \xe2\x80\x93 Ich sitze in einem Raum"),
             {{"stop0", 0},
              {"inpMode", 0},
              {"inpDist", 2.0f},
              {"lucOn", 1},
              {"lucMode", 0},
              {"lucGen", 32.0f},
              {"lucGap", 1.5f},
              {"lucMix", 0.85f},
              {"lucRecSrc", 1},
              {"simW", 6.0f},
              {"simL", 8.0f},
              {"simH", 3.0f}},
             {}, {}, true);
    makeFull(u8("80 Lucier \xe2\x80\x93 St\xc3\xbc"
                "ck f\xc3\xbcr Orgel"),
             {{"stop1", 1},
              {"stop2", 1},
              {"lucOn", 1},
              {"lucMode", 0},
              {"lucGen", 24.0f},
              {"lucGap", 2.0f},
              {"lucMix", 0.7f},
              {"simW", 12.0f},
              {"simL", 26.0f},
              {"simH", 10.0f},
              {"simAbs", 0.05f}},
             {}, {}, false);

    // ---- set 11: notes to single pipes, every stop in its own room
    currentSet = 11;
    make("81 Jeder Ton ein Register",
         {{"stop0", 1}, {"stop1", 1}, {"stop2", 1}, {"stop4", 1}, {"stop6", 1}, {"noteSpread", 1}},
         {});
    make(u8("82 Akkord \xc3\xbc"
            "ber die Register"),
         {{"stop7", 1}, {"stop0", 1}, {"stop1", 1}, {"stop2", 1}, {"stop3", 1}, {"noteSpread", 2}},
         {});
    make(u8("83 Vier R\xc3\xa4ume"),
         {{"stop7", 1},
          {"stop1", 1},
          {"stop3", 1},
          {"stop6", 1},
          {"simOn", 1},
          {"master", -16.0f},
          {"simRoom7", 2},
          {"simRoom1", 0},
          {"simRoom3", 3},
          {"simRoom6", 1},
          {"simW", 12.0f},
          {"simL", 26.0f},
          {"simH", 10.0f}},
         {});
    make(u8("84 Verstreute Pfeifen"),
         {{"stop1", 1},
          {"stop2", 1},
          {"stop3", 1},
          {"stop5", 1},
          {"noteSpread", 3},
          {"motion1", 5},
          {"motion2", 5},
          {"motion3", 5},
          {"motion5", 5},
          {"rate1", 0.7f},
          {"rate2", 0.9f},
          {"rate3", 1.1f},
          {"rate5", 1.3f},
          {"depth1", 120.0f},
          {"depth2", 120.0f},
          {"depth3", 120.0f},
          {"depth5", 120.0f}},
         {});
    make(u8("85 Nah und fern"),
         {{"stop1", 1},
          {"stop6", 1},
          {"noteSpread", 1},
          {"simOn", 1},
          {"master", -16.0f},
          {"simRoom1", 3},
          {"simRoom6", 2}},
         {});

    // ---- set 12: floor plans of the Saal, the spatial image wearing away (Verfall · Raum), grain
    // clouds (Raum, Wolke)
    //      (Verfall · Raum needs an Ambisonics output - AmbiX, binaural or loudspeakers; in stereo
    //      only the grid is heard)
    currentSet = 12;
    make(u8("86 Saal \xe2\x80\x93 Um die Ecke"),
         {{"stop1", 1},
          {"stop2", 1},
          {"simOn", 1},
          {"simShape", (int)rysum::shapeL},
          {"simShapeAmt", 0.9f},
          {"simW", 14.0f},
          {"simL", 22.0f},
          {"simH", 9.0f},
          {"simAbs", 0.04f},
          {"simLy", 0.3f},
          {"motion1", 2},
          {"rate1", 0.08f},
          {"depth1", 70.0f}},
         {});
    make(u8("87 Saal \xe2\x80\x93 Rotunde (Fl\xc3\xbcstergalerie)"),
         {{"stop3", 1},
          {"stop5", 1},
          {"simOn", 1},
          {"simShape", (int)rysum::shapeRound},
          {"simShapeAmt", 1.0f},
          {"simW", 20.0f},
          {"simL", 20.0f},
          {"simH", 16.0f},
          {"simAbs", 0.02f},
          {"simAbsFloor", 0.05f},
          {"simScatter", 0.05f},
          {"simLx", 0.2f},
          {"simLy", 0.5f},
          {"motion3", 1},
          {"rate3", 0.05f},
          {"master", -15.0f}},
         {});
    make(u8("88 Saal \xe2\x80\x93 Kreuzkirche"),
         {{"stop7", 1},
          {"stop1", 1},
          {"stop6", 1},
          {"simOn", 1},
          {"simShape", (int)rysum::shapeCross},
          {"simShapeAmt", 0.4f},
          {"simW", 28.0f},
          {"simL", 44.0f},
          {"simH", 18.0f},
          {"simAbs", 0.04f},
          {"simAbsFloor", 0.12f},
          {"simScatter", 0.45f},
          {"simLy", 0.25f},
          {"master", -16.0f}},
         {});
    make(u8("89 Saal \xe2\x80\x93 F\xc3\xa4"
            "cher"),
         {{"stop1", 1},
          {"stop3", 1},
          {"simOn", 1},
          {"simShape", (int)rysum::shapeFan},
          {"simShapeAmt", 0.9f},
          {"simW", 32.0f},
          {"simL", 36.0f},
          {"simH", 12.0f},
          {"simAbs", 0.08f},
          {"simLy", 0.2f}},
         {});
    make(u8("90 Saal \xe2\x80\x93 Der Grundriss atmet"),
         {{"stop1", 1},
          {"stop2", 1},
          {"stop4", 1},
          {"simOn", 1},
          {"simShape", (int)rysum::shapeApse},
          {"simShapeAmt", 0.5f},
          {"simW", 16.0f},
          {"simL", 30.0f},
          {"simH", 12.0f},
          {"simAbs", 0.03f},
          {"lfo1Shape", (float)shSmooth},
          {"lfo1Rate", 0.04f},
          {"lfo2Shape", (float)shSmooth},
          {"lfo2Rate", 0.023f}},
         {{srcLfo1, "simShapeAmt", 0.5f}, {srcLfo2, "simSize", 0.25f}});
    make(u8("91 Raumverfall \xe2\x80\x93 Verwittern"),
         {{"stop1", 1},
          {"stop2", 1},
          {"stop7", 1},
          {"motion1", 1},
          {"rate1", 0.04f},
          {"motion7", 1},
          {"rate7", -0.05f},
          {"spcWeather", 90.0f},
          {"spcDrops", 0.15f},
          {"irEnvelop", 0.5f}},
         {});
    make(u8("92 Raumverfall \xe2\x80\x93 Raster"),
         {{"stop3", 1},
          {"stop6", 1},
          {"motion3", 1},
          {"rate3", 0.15f},
          {"motion6", 3},
          {"rate6", 0.4f},
          {"depth6", 90.0f},
          {"spcGrid", 0.85f}},
         {});
    make(u8("93 Raumverfall \xe2\x80\x93 Kugel aus Sand"),
         {{"stop1", 1},
          {"stop4", 1},
          {"stop5", 1},
          {"motion4", 2},
          {"rate4", 0.2f},
          {"depth4", 120.0f},
          {"spcBits", 5.0f},
          {"spcRate", 1500.0f},
          {"spcDrops", 0.35f},
          {"degAmount", 0.25f},
          {"degGrain", 0.3f}},
         {});
    make(u8("94 Raumverfall \xe2\x80\x93 Geisterrichtungen"),
         {{"stop3", 1}, {"stop2", 1}, {"motion3", 1}, {"rate3", 0.1f}, {"spcAlias", 0.8f}}, {});
    make(u8("95 Raumverfall \xe2\x80\x93 Die Kugel atmet aus"),
         {{"stop1", 1},
          {"stop6", 1},
          {"motion6", 1},
          {"rate6", 0.25f},
          {"spcErode", 0.0f},
          {"lfo1Shape", (float)shSmooth},
          {"lfo1Rate", 0.06f}},
         {{srcLfo1, "spcErode", 1.0f, true}});
    make(u8("96 Wolke \xe2\x80\x93 Flirrende Mixtur"),
         {{"stop1", 1},
          {"stop6", 1},
          {"cloudMix6", 0.75f},
          {"cloudDens6", 90.0f},
          {"cloudSpread6", 30.0f},
          {"cloudSize6", 35.0f},
          {"cloudScatter6", 15.0f}},
         {});
    makeFull(u8("97 Wolke \xe2\x80\x93 Die Orgel l\xc3\xb6st sich auf"),
             {{"stop1", 1},
              {"stop2", 1},
              {"stop3", 1},
              {"cloudMix1", 0.15f},
              {"cloudMix2", 0.15f},
              {"cloudMix3", 0.15f},
              {"cloudSpread1", 180.0f},
              {"cloudSpread2", 180.0f},
              {"cloudSpread3", 180.0f},
              {"cloudSize1", 220.0f},
              {"cloudSize2", 220.0f},
              {"cloudSize3", 220.0f},
              {"cloudScatter1", 120.0f},
              {"cloudScatter2", 120.0f},
              {"cloudScatter3", 120.0f},
              {"cloudDens1", 35.0f},
              {"cloudDens2", 35.0f},
              {"cloudDens3", 35.0f}},
             {},
             {{},
              {{"cloudMix1", 0.4f}, {"cloudMix2", 0.4f}, {"cloudMix3", 0.4f}},
              {{"cloudMix1", 0.8f}, {"cloudMix2", 0.8f}, {"cloudMix3", 0.8f}},
              {{"cloudMix1", 1.0f}, {"cloudMix2", 1.0f}, {"cloudMix3", 1.0f}, {"spcErode", 0.4f}}},
             false);
    make(u8("98 Wolke \xe2\x80\x93 Chor aus allen Richtungen"),
         {{"stop2", 1},
          {"stop7", 1},
          {"vol7", -6.0f},
          {"cloudMix2", 0.6f},
          {"cloudDens2", 20.0f},
          {"cloudSpread2", 120.0f},
          {"cloudSize2", 420.0f},
          {"cloudScatter2", 60.0f},
          {"cloudMix7", 0.35f},
          {"cloudDens7", 15.0f},
          {"cloudSpread7", 90.0f},
          {"cloudSize7", 300.0f}},
         {});
    make(u8("99 Wolke kreist im Saal"),
         {{"stop3", 1},
          {"stop5", 1},
          {"motion3", 1},
          {"rate3", 0.3f},
          {"motion5", 1},
          {"rate5", -0.22f},
          {"cloudMix3", 0.6f},
          {"cloudSpread3", 40.0f},
          {"cloudScatter3", 150.0f},
          {"cloudDens3", 50.0f},
          {"cloudMix5", 0.6f},
          {"cloudSpread5", 40.0f},
          {"cloudScatter5", 150.0f},
          {"cloudDens5", 50.0f},
          {"simOn", 1},
          {"simShape", (int)rysum::shapeOct},
          {"simShapeAmt", 0.6f},
          {"simW", 18.0f},
          {"simL", 24.0f},
          {"simH", 12.0f},
          {"spcWeather", 180.0f}},
         {});

    // ---- set 13: Ansprache, Abriss, Obertoene (Raum · Pfeife), Luft umkehren, Klick-Resonanz,
    // Glitch
    //      (internal stop numbers: 0 Praestant 8', 1 Gedackt, 2 Octave 4', 3 Octave 2', 4
    //      Sesquialtera, 5 Mixtur, 6 Trompete, 7 Praestant 16')
    currentSet = 13;
    make(u8("100 Ansprache \xe2\x80\x93 Weiche Rede"),
         {{"stop0", 1}, {"stop1", 1}, {"pipeAtt0", -0.8f}, {"pipeAtt1", -0.7f}, {"click", 0.1f}},
         {});
    make(u8("101 Ansprache \xe2\x80\x93 Sch\xc3\xa4rfe"),
         {{"stop0", 0},
          {"stop2", 1},
          {"stop3", 1},
          {"pipeAtt2", 0.9f},
          {"pipeSharp2", 0.6f},
          {"pipeAtt3", 0.9f},
          {"pipeSharp3", 0.7f}},
         {});
    make(u8("102 Ansprache \xe2\x80\x93 Nur das Anblasen"),
         {{"stop0", 0},
          {"stop2", 1},
          {"stop5", 1},
          {"pipeAtt2", 0.5f},
          {"pipeIso2", 1.0f},
          {"pipeRep2", 0.4f},
          {"pipeAtt5", 0.5f},
          {"pipeIso5", 1.0f},
          {"pipeRep5", 0.25f},
          {"simOn", 1},
          {"simW", 16.0f},
          {"simL", 30.0f},
          {"simH", 12.0f}},
         {});
    make(u8("103 Ansprache \xe2\x80\x93 Kr\xc3\xa4hende Pfeife"),
         {{"stop0", 1},
          {"stop2", 1},
          {"pipeAtt0", 0.7f},
          {"pipeSharp0", 0.4f},
          {"pipeRep0", 0.85f},
          {"pipeAtt2", 0.5f},
          {"pipeRep2", 0.6f}},
         {});
    make(u8("104 Abriss \xe2\x80\x93 Atem"),
         {{"stop0", 1},
          {"stop1", 1},
          {"pipePuff0", 0.7f},
          {"pipePuff1", 1.0f},
          {"releaseClick", 0.0f},
          {"noteSpread", 1}},
         {});
    make(u8("105 Abriss \xe2\x80\x93 Seufzer im Kirchenschiff"),
         {{"stop7", 1},
          {"stop6", 1},
          {"stop0", 0},
          {"pipePuff7", 1.0f},
          {"pipePuff6", 0.8f},
          {"simOn", 1},
          {"simShape", (int)rysum::shapeCross},
          {"simShapeAmt", 0.3f},
          {"simW", 26.0f},
          {"simL", 42.0f},
          {"simH", 18.0f},
          {"master", -15.0f}},
         {});
    make(u8("106 Obert\xc3\xb6ne \xe2\x80\x93 Glasorgel"),
         {{"stop0", 1},
          {"stop2", 1},
          {"pipeHiFreq0", 3000.0f},
          {"pipeHiGain0", 10.0f},
          {"pipeHiShape0", 0.4f},
          {"pipeHiFreq2", 3500.0f},
          {"pipeHiGain2", 9.0f},
          {"pipeHiShape2", 0.4f}},
         {});
    make(u8("107 Obert\xc3\xb6ne \xe2\x80\x93 Unter der Decke"),
         {{"stop7", 1},
          {"stop5", 1},
          {"stop0", 0},
          {"pipeHiFreq5", 2500.0f},
          {"pipeHiGain5", -20.0f},
          {"pipeHiFreq7", 1200.0f},
          {"pipeHiGain7", -12.0f},
          {"master", -14.0f}},
         {});
    make(u8("108 Obert\xc3\xb6ne \xe2\x80\x93 Nadelanschlag"),
         {{"stop0", 1},
          {"stop3", 1},
          {"pipeHiFreq0", 5000.0f},
          {"pipeHiGain0", 6.0f},
          {"pipeHiShape0", 1.0f},
          {"pipeHiFreq3", 6000.0f},
          {"pipeHiGain3", 8.0f},
          {"pipeHiShape3", 1.0f}},
         {});
    make(u8("109 Luft umkehren \xe2\x80\x93 Ferne Pr\xc3\xa4senz"),
         {{"stop0", 1},
          {"stop2", 1},
          {"stop3", 1},
          {"airInvert", 0.85f},
          {"simOn", 1},
          {"simW", 24.0f},
          {"simL", 60.0f},
          {"simH", 16.0f},
          {"simLy", 0.1f},
          {"master", -17.0f}},
         {});
    make(u8("110 Klick-Resonanz \xe2\x80\x93 Die Traktur singt"),
         {{"stop0", 0},
          {"stop1", 1},
          {"vol1", -16.0f},
          {"click", 0.8f},
          {"releaseClick", 0.6f},
          {"clickBlend", 0.7f},
          {"clickRes", 0.8f},
          {"clickResDecay", 0.9f},
          {"clickResBright", 0.6f}},
         {});
    make(u8("111 Klick-Resonanz \xe2\x80\x93 Tasten wie Glocken"),
         {{"stop0", 0},
          {"stop1", 1},
          {"vol1", -22.0f},
          {"click", 1.0f},
          {"releaseClick", 1.0f},
          {"clickBlend", 1.0f},
          {"clickRes", 1.0f},
          {"clickResDecay", 2.2f},
          {"clickResBright", 0.9f},
          {"simOn", 1},
          {"simW", 18.0f},
          {"simL", 34.0f},
          {"simH", 14.0f}},
         {});
    make(u8("112 Glitch \xe2\x80\x93 Stottern"), {{"stop0", 1}, {"stop2", 1}, {"degStutter", 0.6f}},
         {});
    make(u8("113 Glitch \xe2\x80\x93 Bandschnitt"),
         {{"stop0", 1}, {"stop1", 1}, {"degSplice", 0.85f}}, {});
    make(u8("114 Glitch \xe2\x80\x93 Harte Kanten"),
         {{"stop0", 0}, {"stop2", 1}, {"stop3", 1}, {"stop5", 1}, {"degZero", 0.9f}}, {});
    make(u8("115 Glitch \xe2\x80\x93 Clipping-Gewitter"),
         {{"stop7", 1},
          {"stop6", 1},
          {"stop0", 0},
          {"degClip", 0.7f},
          {"degStutter", 0.25f},
          {"master", -16.0f}},
         {});
    make(u8("116 Atmende Ansprache"),
         {{"stop0", 1},
          {"stop2", 1},
          {"pipeAtt0", 0.0f},
          {"pipeAtt2", 0.0f},
          {"pipePuff0", 0.5f},
          {"lfo1Shape", (float)shSmooth},
          {"lfo1Rate", 0.05f}},
         {{srcLfo1, "pipeAtt0", 0.9f}, {srcLfo1, "pipeAtt2", -0.8f}});
    make(u8("117 Alles zerf\xc3\xa4llt"),
         {{"stop0", 1},
          {"stop2", 1},
          {"stop5", 1},
          {"pipeAtt0", 0.6f},
          {"pipeRep0", 0.5f},
          {"pipePuff0", 0.6f},
          {"clickRes", 0.6f},
          {"clickResDecay", 0.7f},
          {"degStutter", 0.3f},
          {"degSplice", 0.4f},
          {"degZero", 0.3f},
          {"degAmount", 0.2f},
          {"degGrain", 0.25f},
          {"spcWeather", 120.0f},
          {"spcDrops", 0.2f}},
         {});

    // ---- set 14: playing with the hands - velocity, pitch wheel, mod wheel, pressure; long
    // evolutions; wild ones
    //      (every patch: pitch wheel -> +-2 semitones per pipe, unless said otherwise)
    currentSet = 14;
    {
        const float bend2 = 1.0f / 6.0f;
        // 70 again: its scenes had switched its own LFO route off (route amounts are part of the
        // scenes now)
        makeFull(u8("70 Szenen \xe2\x80\x93 Wanderung von selbst"),
                 {{"stop1", 1}, {"lfo1Shape", (float)shTriangle}, {"lfo1Rate", 0.02f}},
                 {{srcLfo1, "sceneMorph", 1.0f, true}},
                 {{},
                  {{"motion0", 1},
                   {"motion1", 1},
                   {"rate0", 0.2f},
                   {"rate1", -0.15f},
                   {"ellipse0", 0.5f},
                   {"ellipse1", 0.5f}},
                  {{"motion0", 1},
                   {"motion1", 1},
                   {"rate0", 0.35f},
                   {"rate1", -0.3f},
                   {"stop3", 1},
                   {"fxEchoLevel", 0.6f},
                   {"fxEchoTurn", 120.0f}},
                  {{"stop0", 0},
                   {"stop7", 1},
                   {"stop3", 1},
                   {"tremOn", 1},
                   {"tremDepth", 0.4f},
                   {"degAmount", 0.4f},
                   {"degWobble", 0.5f},
                   {"motion1", 2},
                   {"rate1", 0.1f}}},
                 false);

        // velocity shapes the whole pipe: soft = a swelling, dull, distant breath; hard = a sharp,
        // bright, close chiff (envelope 2 is held at 1 while a key is down: it gives the soft end
        // its offset)
        make(u8("118 Anschlag \xe2\x80\x93 Vom Hauch zum Schrei"),
             {{"stop0", 1},
              {"stop2", 1},
              {"pipeAtt0", -0.9f},
              {"pipeAtt2", -0.9f},
              {"pipePuff0", 0.3f},
              {"pipePuff2", 0.3f},
              {"env2Attack", 0.001f},
              {"env2Decay", 1.0f},
              {"env2Sustain", 1.0f},
              {"env2Release", 20.0f}},
             {{srcVelocity, pipe(tgtAttack), 1.0f},
              {srcVelocity, pipe(tgtAttack), 0.9f},
              {srcVelocity, pipe(tgtAttSharp), 0.9f},
              {srcVelocity, pipe(tgtLevel), 0.3f},
              {srcEnv2, pipe(tgtLevel), -0.15f},
              {srcVelocity, pipe(tgtBright), 0.5f},
              {srcEnv2, pipe(tgtBright), -0.4f},
              {srcVelocity, pipe(tgtHiGain), 0.4f},
              {srcVelocity, pipe(tgtDist), -0.25f},
              {srcVelocity, "clickRes", 0.7f},
              {srcPitchWheel, pipe(tgtPitch), bend2},
              {srcModWheel, pipe(tgtAttRep), 0.9f},
              {srcModWheel, pipe(tgtCloudMix), 0.45f},
              {srcPressure, pipe(tgtHiGain), 0.3f}});

        // the mod wheel goes through four scenes: a church -> sharp speech -> tape and grid -> a
        // broken machine
        makeFull(u8("119 Modrad \xe2\x80\x93 Kirche wird Maschine"),
                 {{"stop0", 1},
                  {"stop1", 1},
                  {"simOn", 1},
                  {"simShape", (int)rysum::shapeApse},
                  {"simShapeAmt", 0.6f},
                  {"simW", 16.0f},
                  {"simL", 30.0f},
                  {"simH", 12.0f},
                  {"master", -14.0f}},
                 {{srcModWheel, "sceneMorph", 1.0f},
                  {srcVelocity, pipe(tgtAttack), 0.8f},
                  {srcPitchWheel, pipe(tgtPitch), bend2},
                  {srcVelocity, pipe(tgtClip), 0.0f}},
                 {{},
                  {{"pipeAtt0", 0.6f},
                   {"pipeSharp0", 0.5f},
                   {"pipeRep0", 0.4f},
                   {"pipeHiGain0", 6.0f},
                   {"pipeHiShape0", 0.6f},
                   {"cloudMix1", 0.3f},
                   {"stop3", 1}},
                  {{"stop3", 1},
                   {"stop5", 1},
                   {"pipeRep0", 0.7f},
                   {"degStutter", 0.5f},
                   {"degSplice", 0.6f},
                   {"degAmount", 0.4f},
                   {"degBits", 8.0f},
                   {"cloudMix0", 0.4f},
                   {"cloudMix1", 0.6f},
                   {"spcGrid", 0.6f},
                   {"clickRes", 0.5f},
                   {"mod4Amt", 0.4f}},
                  {{"stop3", 1},
                   {"stop5", 1},
                   {"stop6", 1},
                   {"stop7", 1},
                   {"degStutter", 0.8f},
                   {"degSplice", 0.9f},
                   {"degZero", 0.9f},
                   {"degClip", 0.4f},
                   {"degAmount", 0.8f},
                   {"degBits", 4.0f},
                   {"degRate", 2500.0f},
                   {"spcErode", 0.6f},
                   {"spcAlias", 0.7f},
                   {"airInvert", 1.0f},
                   {"pipeIso0", 0.5f},
                   {"clickRes", 1.0f},
                   {"mod4Amt", 0.7f}}},
                 false);

        // pitch wheel down: the organ runs out of breath (wind model "instabil": edge tones); up:
        // it bends up, rises and brightens
        make(u8("120 Tonh\xc3\xb6henrad \xe2\x80\x93 Atemnot"),
             {{"windModel", 1},
              {"stop0", 1},
              {"stop1", 1},
              {"stop2", 1},
              {"swind0", 0.85f},
              {"swind1", 0.85f},
              {"swind2", 0.85f},
              {"tremOn", 1},
              {"tremDepth", 0.0f},
              {"pipePuff0", 0.5f},
              {"pipePuff1", 0.7f}},
             {{srcPitchWheel, pipe(tgtWind), 0.8f},
              {srcPitchWheel, pipe(tgtPitch), 1.0f / 12.0f},
              {srcPitchWheel, pipe(tgtEl), 0.4f},
              {srcPitchWheel, pipe(tgtHiFreq), 0.4f},
              {srcModWheel, "tremDepth", 0.9f},
              {srcVelocity, pipe(tgtPuff), 0.5f},
              {srcVelocity, pipe(tgtAttack), 0.6f},
              {srcPressure, pipe(tgtWind), 0.3f}});

        // aftertouch / MPE pressure blows the pipe: without pressure it barely speaks
        make(u8("121 Druck \xe2\x80\x93 Die Pfeife unter dem Finger"),
             {{"windModel", 1},
              {"stop0", 1},
              {"stop3", 1},
              {"swind0", 0.38f},
              {"swind3", 0.38f},
              {"pipeAtt0", -0.5f},
              {"pipeAtt3", -0.5f}},
             {{srcPressure, pipe(tgtWind), 0.62f},
              {srcPressure, pipe(tgtHiGain), 0.45f},
              {srcPressure, pipe(tgtCloudMix), 0.5f},
              {srcSlide, pipe(tgtAz), 0.6f},
              {srcSlide, pipe(tgtHiFreq), -0.5f},
              {srcVelocity, pipe(tgtAttack), 1.0f},
              {srcPitchWheel, pipe(tgtPitch), bend2},
              {srcModWheel, pipe(tgtAttRep), 0.8f},
              {srcModWheel, pipe(tgtCloudSpread), 0.6f}});

        // half an hour that never repeats: four slow random LFOs at odd rates - the scenes, and
        // every pipe its own chiff and cloud (mod wheel: the wandering speeds up)
        makeFull(u8("122 Lange Wanderung"),
                 {{"stop0", 0},
                  {"stop1", 1},
                  {"lfo4Shape", (float)shSmooth},
                  {"lfo4Rate", 0.01f},
                  {"lfo3Shape", (float)shSmooth},
                  {"lfo3Rate", 0.0137f},
                  {"lfo3Spread", 1.0f},
                  {"lfo3SpreadMode", 1},
                  {"lfo2Shape", (float)shSmooth},
                  {"lfo2Rate", 0.0171f},
                  {"lfo2Spread", 1.0f},
                  {"lfo2SpreadMode", 1},
                  {"lfo1Shape", (float)shSmooth},
                  {"lfo1Rate", 0.023f},
                  {"lfo1Spread", 0.5f}},
                 {{srcLfo4, "sceneMorph", 1.0f, true},
                  {srcLfo3, pipe(tgtAttack), 0.8f},
                  {srcLfo2, pipe(tgtCloudMix), 0.6f, true},
                  {srcLfo1, pipe(tgtAz), 0.35f},
                  {srcLfo2, pipe(tgtHiGain), 0.3f},
                  {srcModWheel, "lfo4Rate", 0.5f},
                  {srcModWheel, "lfo3Rate", 0.4f},
                  {srcVelocity, pipe(tgtLevel), 0.2f},
                  {srcPitchWheel, pipe(tgtPitch), bend2}},
                 {{},
                  {{"stop2", 1},
                   {"cloudMix2", 0.4f},
                   {"cloudSpread2", 120.0f},
                   {"motion2", 1},
                   {"rate2", 0.03f}},
                  {{"stop2", 1},
                   {"stop4", 1},
                   {"stop5", 1},
                   {"vol5", -10.0f},
                   {"degSplice", 0.5f},
                   {"pipeIso5", 0.6f},
                   {"pipeRep5", 0.5f}},
                  {{"stop7", 1},
                   {"stop6", 1},
                   {"vol6", -8.0f},
                   {"degAmount", 0.5f},
                   {"degWobble", 0.6f},
                   {"spcWeather", 300.0f},
                   {"airInvert", 0.6f}}},
                 false);

        // soft = a glass organ (bright, soft overtones); hard = it shatters (hard edges, clipping,
        // spiky overtones); wheel: it stutters
        make(u8("123 Anschlag \xe2\x80\x93 Glas zerspringt"),
             {{"stop0", 1},
              {"stop2", 1},
              {"stop3", 1},
              {"pipeHiFreq0", 3000.0f},
              {"pipeHiGain0", 8.0f},
              {"pipeHiShape0", -0.6f},
              {"pipeHiFreq2", 3500.0f},
              {"pipeHiGain2", 7.0f},
              {"pipeHiShape2", -0.6f},
              {"pipeHiFreq3", 4500.0f},
              {"pipeHiGain3", 6.0f},
              {"pipeHiShape3", -0.6f}},
             {{srcVelocity, pipe(tgtZero), 0.8f},
              {srcVelocity, pipe(tgtClip), 0.6f},
              {srcVelocity, pipe(tgtHiShape), 1.0f},
              {srcVelocity, pipe(tgtHiShape), 0.6f},
              {srcVelocity, pipe(tgtAttSharp), 1.0f},
              {srcModWheel, pipe(tgtStutter), 0.8f},
              {srcModWheel, pipe(tgtSplice), 0.6f},
              {srcPitchWheel, pipe(tgtPitch), bend2},
              {srcPitchWheel, pipe(tgtHiFreq), 0.5f}});

        // no note like the last: chiff, overtones, direction, a little pitch, cloud, bits, puff -
        // drawn anew for every note (LFO 1 almost still, Versatz random: every pipe also keeps its
        // own leaning)
        make(u8("124 Zufall je Ton \xe2\x80\x93 Kein Ton gleich"),
             {{"stop0", 1},
              {"stop1", 1},
              {"stop3", 1},
              {"degAmount", 0.0f},
              {"degDropouts", 0.0f},
              {"degWobble", 0.0f},
              {"degBits", 16.0f},
              {"lfo1Shape", (float)shSine},
              {"lfo1Rate", 0.01f},
              {"lfo1Spread", 1.0f},
              {"lfo1SpreadMode", 1}},
             {{srcRandom, pipe(tgtAttack), 0.8f},
              {srcLfo1, pipe(tgtHiFreq), 0.5f},
              {srcLfo1, pipe(tgtHiGain), 0.35f},
              {srcRandom, pipe(tgtAz), 0.5f},
              {srcLfo1, pipe(tgtEl), 0.3f},
              {srcRandom, pipe(tgtPitch), 0.006f},
              {srcRandom, pipe(tgtCloudMix), 0.5f},
              {srcLfo1, pipe(tgtDegrade), 0.4f, true},
              {srcRandom, pipe(tgtDegBits), -0.7f},
              {srcRandom, pipe(tgtPuff), 0.8f},
              {srcLfo1, pipe(tgtAttRep), 0.6f},
              {srcModWheel, pipe(tgtStutter), 0.7f},
              {srcVelocity, pipe(tgtLevel), 0.25f},
              {srcPitchWheel, pipe(tgtPitch), bend2}});

        // the mod wheel lets the organ evaporate: the sustain falls away until only the chiffs are
        // left, dissolved into a cloud, far away, the sphere worn down
        make(u8("125 Modrad \xe2\x80\x93 Die Orgel verdunstet"),
             {{"stop0", 1},
              {"stop2", 1},
              {"stop7", 1},
              {"simOn", 1},
              {"simW", 20.0f},
              {"simL", 36.0f},
              {"simH", 14.0f},
              {"master", -14.0f}},
             {{srcModWheel, pipe(tgtAttIso), 1.0f},
              {srcModWheel, pipe(tgtCloudMix), 0.9f},
              {srcModWheel, pipe(tgtCloudSpread), 0.7f},
              {srcModWheel, pipe(tgtCloudSize), 0.5f},
              {srcModWheel, pipe(tgtDist), 0.45f},
              {srcModWheel, "spcErode", 0.85f},
              {srcModWheel, pipe(tgtAttRep), 0.7f},
              {srcVelocity, pipe(tgtAttack), 0.8f},
              {srcPitchWheel, pipe(tgtPitch), bend2},
              {srcPressure, pipe(tgtLevel), 0.2f}});

        // clockwork: every pipe gated by a fast square at its own phase (polyrhythms across the
        // keyboard), the action rings; mod wheel: faster and stuttering; velocity: repeated chiffs
        // and a brighter ring
        make(u8("126 Uhrwerk der Traktur"),
             {{"stop0", 0},
              {"stop1", 1},
              {"stop3", 1},
              {"lfo1Shape", (float)shSquare},
              {"lfo1Rate", 5.0f},
              {"lfo1Spread", 0.37f},
              {"lfo2Shape", (float)shSteps},
              {"lfo2Rate", 3.0f},
              {"lfo2Spread", 1.0f},
              {"lfo2SpreadMode", 1},
              {"click", 0.7f},
              {"releaseClick", 0.6f},
              {"clickRes", 0.7f},
              {"clickResDecay", 0.5f},
              {"pipeAtt1", 0.8f},
              {"pipeAtt3", 0.8f},
              {"pipeIso1", 0.4f}},
             {{srcLfo1, pipe(tgtLevel), -0.5f, true},
              {srcLfo2, pipe(tgtAz), 0.5f},
              {srcLfo2, pipe(tgtHiGain), 0.3f},
              {srcModWheel, "lfo1Rate", 0.3f},
              {srcModWheel, pipe(tgtStutter), 0.7f},
              {srcVelocity, "clickResBright", 0.6f},
              {srcVelocity, pipe(tgtAttRep), 0.7f},
              {srcPitchWheel, pipe(tgtPitch), bend2}});

        // a tipsy organist: every note a little off, the wind of every pipe sways on its own, hard
        // notes stammer; mod wheel: more detuned, more wobble
        make(u8("127 Der Organist hat getrunken"),
             {{"windModel", 1},
              {"stop0", 1},
              {"stop1", 1},
              {"stop2", 1},
              {"degAmount", 0.3f},
              {"degWobble", 0.7f},
              {"degDropouts", 0.0f},
              {"degBits", 16.0f},
              {"degGrain", 0.0f},
              {"lfo1Shape", (float)shSmooth},
              {"lfo1Rate", 0.4f},
              {"lfo1Spread", 1.0f},
              {"lfo1SpreadMode", 1}},
             {{srcRandom, pipe(tgtPitch), 0.012f},
              {srcLfo1, pipe(tgtWind), -0.5f, true},
              {srcLfo1, pipe(tgtPitch), 0.006f},
              {srcVelocity, pipe(tgtAttRep), 0.8f},
              {srcVelocity, pipe(tgtStutter), 0.4f},
              {srcRandom, pipe(tgtAz), 0.4f},
              {srcModWheel, "detune", 0.8f},
              {srcModWheel, pipe(tgtWobble), 0.6f},
              {srcPitchWheel, pipe(tgtPitch), bend2}});

        // every note decays on its own: after a second it starts to crumble (bits, rate, dropouts,
        // cloud, stutter) for 12 s; pressure holds it together, the mod wheel makes it crumble
        // faster
        make(u8("128 H\xc3\xbcllkurve \xe2\x80\x93 Jeder Ton zerf\xc3\xa4llt"),
             {{"stop0", 1},
              {"stop2", 1},
              {"degAmount", 0.0f},
              {"degBits", 4.0f},
              {"degRate", 3000.0f},
              {"degGrain", 0.5f},
              {"degDropouts", 0.3f},
              {"degWobble", 0.5f},
              {"env2Delay", 1.0f},
              {"env2Attack", 12.0f},
              {"env2Decay", 1.0f},
              {"env2Sustain", 1.0f},
              {"env2Release", 6.0f},
              {"env1Attack", 0.002f},
              {"env1Decay", 2.5f},
              {"env1Sustain", 0.2f},
              {"env1Release", 1.0f}},
             {{srcEnv2, pipe(tgtDegrade), 1.0f},
              {srcEnv2, pipe(tgtCloudMix), 0.8f},
              {srcEnv2, pipe(tgtStutter), 0.4f},
              {srcEnv2, pipe(tgtBright), -0.35f},
              {srcEnv1, pipe(tgtHiGain), 0.5f},
              {srcVelocity, pipe(tgtAttack), 1.0f},
              {srcModWheel, "env2Attack", -0.5f},
              {srcPitchWheel, pipe(tgtPitch), bend2},
              {srcPressure, pipe(tgtDegrade), -0.6f}});

        // four instruments on the mod wheel: a flute in a small room - reeds in a cruciform church
        // - only chiffs and ringing keys in a fan-shaped hall - a frozen cloud in a weathering
        // sphere
        makeFull(u8("129 Modrad \xe2\x80\x93 Vier Orgeln"),
                 {{"stop0", 0},
                  {"stop1", 1},
                  {"simOn", 1},
                  {"simW", 5.0f},
                  {"simL", 7.0f},
                  {"simH", 3.5f},
                  {"simAbs", 0.2f},
                  {"master", -14.0f}},
                 {{srcModWheel, "sceneMorph", 1.0f},
                  {srcVelocity, pipe(tgtAttack), 0.8f},
                  {srcPitchWheel, pipe(tgtPitch), bend2},
                  {srcPressure, pipe(tgtHiGain), 0.4f}},
                 {{},
                  {{"stop1", 0},
                   {"stop6", 1},
                   {"stop5", 1},
                   {"stop7", 1},
                   {"simShape", (int)rysum::shapeCross},
                   {"simShapeAmt", 0.5f},
                   {"simW", 28.0f},
                   {"simL", 46.0f},
                   {"simH", 18.0f},
                   {"simAbs", 0.04f},
                   {"simScatter", 0.4f},
                   {"pipePuff6", 0.8f}},
                  {{"stop1", 0},
                   {"stop2", 1},
                   {"stop3", 1},
                   {"pipeIso2", 1.0f},
                   {"pipeIso3", 1.0f},
                   {"pipeRep2", 0.5f},
                   {"pipeRep3", 0.6f},
                   {"pipeAtt2", 0.8f},
                   {"pipeAtt3", 0.8f},
                   {"clickRes", 1.0f},
                   {"clickResDecay", 1.5f},
                   {"click", 0.8f},
                   {"simShape", (int)rysum::shapeFan},
                   {"simShapeAmt", 1.0f},
                   {"simW", 30.0f},
                   {"simL", 34.0f},
                   {"simH", 12.0f}},
                  {{"stop1", 1},
                   {"stop7", 1},
                   {"cloudMix1", 1.0f},
                   {"cloudMix7", 0.8f},
                   {"cloudSize1", 420.0f},
                   {"cloudSize7", 420.0f},
                   {"cloudSpread1", 180.0f},
                   {"cloudSpread7", 180.0f},
                   {"cloudDens1", 25.0f},
                   {"cloudDens7", 25.0f},
                   {"spcAlias", 0.6f},
                   {"spcWeather", 120.0f},
                   {"degAmount", 0.3f},
                   {"simW", 40.0f},
                   {"simL", 80.0f},
                   {"simH", 30.0f},
                   {"simAbs", 0.03f}}},
                 false);

        // the pitch wheel lifts the pipes up into the vault (and a semitone up) or pulls them down
        // to the floor and near; they circle, with Doppler; hard notes come close; the mod wheel
        // dissolves them
        make(u8("130 Tonh\xc3\xb6henrad \xe2\x80\x93 Schwerkraft"),
             {{"stop0", 1},
              {"stop3", 1},
              {"doppler", 2.5f},
              {"motion0", 1},
              {"rate0", 0.15f},
              {"motion3", 1},
              {"rate3", -0.2f}},
             {{srcPitchWheel, pipe(tgtEl), 1.0f},
              {srcPitchWheel, pipe(tgtPitch), 1.0f / 12.0f},
              {srcPitchWheel, pipe(tgtDist), -0.5f},
              {srcModWheel, "doppler", 0.4f},
              {srcVelocity, pipe(tgtDist), -0.5f},
              {srcVelocity, pipe(tgtAttack), 0.7f},
              {srcModWheel, pipe(tgtCloudMix), 0.6f},
              {srcModWheel, pipe(tgtCloudSpread), 0.5f}});

        // for MPE (or aftertouch + CC 74): pressure = loudness and overtones, slide = direction,
        // cloud and colour; wheel: only the chiff
        make(u8("131 Druck & Gleiten \xe2\x80\x93 Kontinuum"),
             {{"stop0", 1},
              {"stop2", 1},
              {"vol0", -10.0f},
              {"vol2", -10.0f},
              {"pipeAtt0", -0.6f},
              {"pipeAtt2", -0.6f}},
             {{srcPressure, pipe(tgtLevel), 0.3f},
              {srcPressure, pipe(tgtHiGain), 0.5f},
              {srcPressure, pipe(tgtHiShape), 0.6f},
              {srcSlide, pipe(tgtAz), 0.8f},
              {srcSlide, pipe(tgtCloudMix), 0.7f},
              {srcSlide, pipe(tgtHiFreq), -0.6f},
              {srcVelocity, pipe(tgtAttack), 1.0f},
              {srcPitchWheel, pipe(tgtPitch), bend2},
              {srcModWheel, pipe(tgtAttIso), 0.8f}});

        // slow erosion: the sphere weathers over ten minutes of playing, LFOs (100 s and more) wear
        // it down and let it grow back, every pipe drifts in its own chiff and cloud; the mod wheel
        // makes it weather faster
        make(u8("132 Langsame Erosion"),
             {{"stop0", 1},
              {"stop1", 1},
              {"stop2", 1},
              {"spcWeather", 600.0f},
              {"degAmount", 0.0f},
              {"degBits", 6.0f},
              {"degDropouts", 0.1f},
              {"lfo4Shape", (float)shSmooth},
              {"lfo4Rate", 0.01f},
              {"lfo3Shape", (float)shSine},
              {"lfo3Rate", 0.01f},
              {"lfo3Spread", 1.0f},
              {"lfo3SpreadMode", 1}},
             {{srcLfo4, "spcErode", 0.5f, true},
              {srcLfo3, pipe(tgtAttack), 0.7f},
              {srcLfo3, pipe(tgtCloudMix), 0.5f, true},
              {srcLfo4, pipe(tgtDegrade), 0.4f, true},
              {srcModWheel, "spcWeather", -0.6f},
              {srcVelocity, pipe(tgtAttack), 0.5f},
              {srcPitchWheel, pipe(tgtPitch), bend2}});

        // velocity = explosion: soft notes are clean, hard ones clip, cut, crush, scatter all
        // around and come close; pitch wheel +-12 semitones; mod wheel: stutter and a broken
        // sphere; pressure: tape splices
        make(u8("133 Anschlag \xe2\x80\x93 Explosion"),
             {{"stop0", 1},
              {"stop2", 1},
              {"stop5", 1},
              {"stop7", 1},
              {"master", -18.0f},
              {"simOn", 1},
              {"simW", 24.0f},
              {"simL", 40.0f},
              {"simH", 16.0f},
              {"degAmount", 0.0f},
              {"degBits", 16.0f}},
             {{srcVelocity, pipe(tgtClip), 1.0f},
              {srcVelocity, pipe(tgtZero), 1.0f},
              {srcVelocity, pipe(tgtHiGain), 0.7f},
              {srcVelocity, pipe(tgtDist), -0.6f},
              {srcVelocity, pipe(tgtCloudMix), 0.8f},
              {srcVelocity, pipe(tgtCloudSpread), 1.0f},
              {srcVelocity, pipe(tgtDegBits), -0.9f},
              {srcVelocity, pipe(tgtDegrade), 0.8f},
              {srcVelocity, pipe(tgtAttack), 1.0f},
              {srcVelocity, pipe(tgtAttRep), 1.0f},
              {srcPitchWheel, pipe(tgtPitch), 1.0f},
              {srcModWheel, pipe(tgtStutter), 1.0f},
              {srcModWheel, "spcAlias", 1.0f},
              {srcModWheel, "spcErode", 0.7f},
              {srcPressure, pipe(tgtSplice), 1.0f}});
    }

    // ---- set 15: the tuning in space (beats move the pipes, difference tones, the Tonnetz), ice
    // walls, double / long exposure
    currentSet = 15;
    {
        const float bend2 = 1.0f / 6.0f;
        const int beat = (int)Motion::Beat, tonnetz = (int)Layout::Tonnetz;
        // play thirds (still), fifths (drifting) and the remote keys (the wolf fifth G#-Eb and the
        // wolf thirds spin)
        make(u8("134 Schwebungsraum \xe2\x80\x93 Die Stimmung tanzt"),
             {{"stop0", 1},
              {"stop2", 1},
              {"motion0", beat},
              {"rate0", 0.08f},
              {"motion2", beat},
              {"rate2", -0.06f},
              {"simOn", 1},
              {"simW", 16.0f},
              {"simL", 24.0f},
              {"simH", 10.0f},
              {"master", -20.0f}},
             {{srcPitchWheel, pipe(tgtPitch), bend2}});
        // beats decide the sound: impure intervals crumble and stutter, the wolf howls; pure ones
        // stay whole
        make(u8("135 Schwebungsraum \xe2\x80\x93 Wolfsgeheul"),
             {{"stop0", 1},
              {"stop3", 1},
              {"motion0", beat},
              {"rate0", 0.15f},
              {"motion3", beat},
              {"rate3", 0.12f},
              {"degAmount", 0.0f},
              {"degBits", 5.0f},
              {"degRate", 4000.0f},
              {"degDropouts", 0.2f},
              {"degWobble", 0.3f}},
             {{srcBeat, pipe(tgtDegrade), 0.9f},
              {srcBeat, pipe(tgtStutter), 0.5f},
              {srcBeat, pipe(tgtHiGain), 0.3f},
              {srcPitchWheel, pipe(tgtPitch), bend2},
              {srcModWheel, pipe(tgtWind), -0.5f}});
        // beats as light: impure intervals dissolve into clouds and brighten, pure ones stand solid
        // in place
        make(u8("136 Schwebung als Licht"),
             {{"stop0", 1},
              {"stop1", 1},
              {"stop2", 1},
              {"cloudSpread0", 120.0f},
              {"cloudSpread2", 120.0f},
              {"cloudSize0", 160.0f},
              {"cloudSize2", 160.0f}},
             {{srcBeat, pipe(tgtCloudMix), 0.9f},
              {srcBeat, pipe(tgtBright), 0.4f},
              {srcBeat, pipe(tgtAttRep), 0.5f},
              {srcPitchWheel, pipe(tgtPitch), bend2},
              {srcVelocity, pipe(tgtAttack), 0.6f}});
        // the resultant 32': held fifths and thirds give their difference tones deep under the
        // floor
        make(u8("137 Differenzt\xc3\xb6ne \xe2\x80\x93 Resultantbass"),
             {{"stop0", 1},
              {"stop2", 1},
              {"fxGhostLevel", 0.0f},
              {"fxGhostPlace", 1.0f},
              {"fxGhostCons", 0.8f}},
             {{srcPitchWheel, pipe(tgtPitch), bend2}, {srcModWheel, "fxGhostLevel", 0.25f}});
        // ghosts between the pipes: every pair sounds its difference, where the two pipes are
        // (spread wide)
        make(u8("138 Differenzt\xc3\xb6ne \xe2\x80\x93 Geister zwischen den Pfeifen"),
             {{"stop0", 0},
              {"stop3", 1},
              {"stop5", 1},
              {"vol5", -6.0f},
              {"fxGhostLevel", 4.0f},
              {"fxGhostPlace", 0.0f},
              {"fxGhostCons", 0.0f},
              {"motion3", 3},
              {"rate3", 0.1f},
              {"depth3", 120.0f},
              {"motion5", 3},
              {"rate5", 0.13f},
              {"depth5", 120.0f}},
             {{srcPitchWheel, pipe(tgtPitch), bend2}});
        // a hall of ice: every early reflection a falling chirp from its own direction
        make(u8("139 Eis-Saal"),
             {{"stop2", 1},
              {"stop3", 1},
              {"click", 0.5f},
              {"simOn", 1},
              {"simIce", 1.0f},
              {"simIceLen", 0.7f},
              {"simW", 20.0f},
              {"simL", 30.0f},
              {"simH", 12.0f},
              {"simAbs", 0.02f},
              {"master", -21.0f}},
             {{srcPitchWheel, pipe(tgtPitch), bend2}, {srcModWheel, "simIceLen", 0.3f}});
        // a frozen rotunda: the pipes circle, the round walls chirp
        make(u8("140 Eis-Saal \xe2\x80\x93 Gefrorene Rotunde"),
             {{"stop0", 1},
              {"stop3", 1},
              {"motion0", 1},
              {"rate0", 0.12f},
              {"motion3", 1},
              {"rate3", -0.17f},
              {"simOn", 1},
              {"simShape", (int)rysum::shapeRound},
              {"simShapeAmt", 1.0f},
              {"simW", 22.0f},
              {"simL", 22.0f},
              {"simH", 16.0f},
              {"simAbs", 0.02f},
              {"simIce", 0.8f},
              {"simIceLen", 1.0f},
              {"master", -20.0f}},
             {{srcPitchWheel, pipe(tgtPitch), bend2}, {srcModWheel, "simIce", 0.2f}});
        // a mirror image a moment later: what sounded on the left comes back on the right
        make(u8("141 Doppelbelichtung \xe2\x80\x93 Spiegelbild"),
             {{"stop0", 1},
              {"stop1", 1},
              {"motion0", 1},
              {"rate0", 0.1f},
              {"fxDexMix", 0.8f},
              {"fxDexTime", 1.5f},
              {"fxDexMirror", 1},
              {"fxDexFade", 0.4f}},
             {{srcPitchWheel, pipe(tgtPitch), bend2}, {srcModWheel, "fxDexTime", 0.3f}});
        // the past, exposed again and again: each image a quarter turn further, fainter, duller
        make(u8("142 Doppelbelichtung \xe2\x80\x93 Echo der Vergangenheit"),
             {{"stop0", 1},
              {"stop2", 1},
              {"fxDexMix", 0.7f},
              {"fxDexTime", 4.0f},
              {"fxDexTurn", 90.0f},
              {"fxDexAgain", 0.7f},
              {"fxDexFade", 0.6f}},
             {{srcPitchWheel, pipe(tgtPitch), bend2}, {srcModWheel, "fxDexTurn", 0.5f}});
        // light trails: two registers circle in opposite directions and paint rings on the sphere
        make(u8("143 Belichtung \xe2\x80\x93 Lichtspuren"),
             {{"stop0", 1},
              {"stop2", 1},
              {"motion0", 1},
              {"rate0", 0.3f},
              {"motion2", 1},
              {"rate2", -0.5f},
              {"fxExpoMix", 0.8f},
              {"fxExpoTime", 6.0f},
              {"fxExpoBright", 0.4f}},
             {{srcPitchWheel, pipe(tgtPitch), bend2}, {srcModWheel, "fxExpoTime", 0.3f}});
        // a long exposure of jumping pipes: every place they jump to keeps glowing for twenty
        // seconds
        make(u8("144 Belichtung \xe2\x80\x93 Zwanzig Sekunden"),
             {{"stop0", 1},
              {"stop3", 1},
              {"motion0", 5},
              {"rate0", 0.7f},
              {"depth0", 150.0f},
              {"motion3", 5},
              {"rate3", 1.1f},
              {"depth3", 150.0f},
              {"fxExpoMix", 1.0f},
              {"fxExpoTime", 20.0f},
              {"fxExpoBright", 0.7f},
              {"master", -15.0f}},
             {{srcPitchWheel, pipe(tgtPitch), bend2}});
        // the tuning as a map: Praestant and Octave arranged in the Tonnetz (change the tuning on
        // the Klang page: the map changes)
        makeFull(u8("145 Tonnetz \xe2\x80\x93 Die Stimmung als Landkarte"),
                 {{"stop0", 1}, {"stop2", 1}}, {{srcPitchWheel, pipe(tgtPitch), bend2}}, {}, false,
                 {{0, tonnetz}, {2, tonnetz}});
        // the Tonnetz moving: every pipe on its lattice place, turning as fast as it beats
        makeFull(u8("146 Tonnetz \xe2\x80\x93 Die Karte schwebt"),
                 {{"stop0", 1},
                  {"stop1", 1},
                  {"motion0", beat},
                  {"rate0", 0.05f},
                  {"motion1", beat},
                  {"rate1", -0.05f},
                  {"fxGhostLevel", -6.0f},
                  {"fxGhostPlace", 0.5f}},
                 {{srcPitchWheel, pipe(tgtPitch), bend2}}, {}, false, {{0, tonnetz}, {1, tonnetz}});
        // everything at once, on the mod wheel: beats move the pipes, ghosts appear, the past is
        // laid over, trails glow
        make(u8("147 Die Stimmung wird Raum"),
             {{"stop0", 1},
              {"stop2", 1},
              {"motion0", beat},
              {"rate0", 0.1f},
              {"motion2", beat},
              {"rate2", -0.08f},
              {"fxGhostLevel", -60.0f},
              {"fxGhostPlace", 0.6f},
              {"fxDexMix", 0.0f},
              {"fxDexTime", 3.0f},
              {"fxDexTurn", 180.0f},
              {"fxExpoMix", 0.0f},
              {"fxExpoTime", 8.0f},
              {"simOn", 1},
              {"simIce", 0.0f},
              {"simW", 18.0f},
              {"simL", 30.0f},
              {"simH", 12.0f},
              {"master", -20.0f}},
             {{srcModWheel, "fxGhostLevel", 0.85f},
              {srcModWheel, "fxDexMix", 0.6f},
              {srcModWheel, "fxExpoMix", 0.7f},
              {srcModWheel, "simIce", 0.8f},
              {srcBeat, pipe(tgtCloudMix), 0.5f},
              {srcPitchWheel, pipe(tgtPitch), bend2},
              {srcVelocity, pipe(tgtAttack), 0.6f}});
    }

    // ---- set 16: the living organ (Effekte, third tab) - shared wind, pallets, the walking
    // listener, wear, sympathetic
    //      pipes, pure tuning, the Tonnetz walker
    currentSet = 16;
    {
        const int beat = (int)Motion::Beat, tonnetz = (int)Layout::Tonnetz;
        // a Kalkant treads every four seconds: hold a chord and the wind runs out before he comes
        // back (a big chord after about a second, a high triad after about three)
        make(u8("148 Kalkant \xe2\x80\x93 Der Atem geht aus"),
             {{"stop7", 1},
              {"stop0", 1},
              {"stop1", 1},
              {"stop2", 1},
              {"lvBellows", 1.0f},
              {"lvBellowsSize", 2.2f},
              {"lvBlower", 1},
              {"lvTreadRate", 15.0f},
              {"lvTreadNoise", 0.5f},
              {"windModel", 1},
              {"wind", 0.1f}},
             {});
        // a blower that is too small for the plenum: every pipe more makes all of them flatter; the
        // starving organ crackles
        make(u8("149 Gebl\xc3\xa4se \xe2\x80\x93 Schwere Akkorde"),
             {{"stop7", 1},
              {"stop0", 1},
              {"stop2", 1},
              {"stop3", 1},
              {"stop5", 1},
              {"lvBellows", 0.6f},
              {"lvBellowsSize", 2.5f},
              {"fxCrackLevel", 0.5f},
              {"fxCrackRate", 14.0f}},
             {{srcBellows, "fxCrackLevel", -0.5f}});
        // play softly: the pipes speak late, breathy, with their edge tone; play hard: at once
        make(u8("150 Z\xc3\xb6gernde Tasten"),
             {{"stop0", 1}, {"stop1", 1}, {"lvPallet", 0.9f}, {"lvPalletSrc", 0}},
             {{srcVelocity, pipe(tgtAttack), 0.5f}});
        // for MPE keyboards / aftertouch: the finger holds the pallet - no pressure, no tone
        make(u8("151 Ventil unter dem Finger (Druck)"),
             {{"stop0", 1}, {"stop6", 1}, {"lvPallet", 1.0f}, {"lvPalletSrc", 1}, {"windModel", 1}},
             {});
        // the listener walks a figure eight through the pipes: they pass close by, with Doppler
        make(u8("152 Gang durch die Orgel"),
             {{"stop0", 1},
              {"stop2", 1},
              {"stop3", 1},
              {"lvWalkShape", 4},
              {"lvWalkSize", 7.0f},
              {"lvWalkRate", 0.03f},
              {"doppler", 1.5f},
              {"simOn", 1},
              {"simW", 18.0f},
              {"simL", 30.0f},
              {"simH", 12.0f},
              {"master", -18.0f}},
             {});
        make(u8("153 Im Kreis um die Pfeifen"),
             {{"stop1", 1},
              {"stop2", 1},
              {"lvWalkShape", 1},
              {"lvWalkSize", 9.0f},
              {"lvWalkRate", 0.06f},
              {"doppler", 1.0f}},
             {});
        // three minutes of sounding and a pipe is gone: the keys you use most crumble first (the
        // wear stays with the project)
        make(u8("154 Das St\xc3\xbc"
                "ck nutzt die Orgel ab"),
             {{"stop0", 1},
              {"stop1", 1},
              {"lvWear", 1.0f},
              {"lvWearTime", 3.0f},
              {"degGrain", 0.6f},
              {"degBits", 7.0f},
              {"degRate", 9000.0f},
              {"degDropouts", 0.25f}},
             {});
        // only the Gedackt is pulled - the pipes of the other stops answer from their places
        make(u8("155 Mitklingende Pfeifen"),
             {{"stop1", 1}, {"lvSympLevel", 0.0f}, {"lvSympDecay", 1.5f}}, {});
        // hold a chord: it beats, the pipes turn - then it pulls itself pure and the room comes to
        // rest
        make(u8("156 Reinheit \xe2\x80\x93 Der Akkord rastet ein"),
             {{"stop0", 1},
              {"stop2", 1},
              {"lvPure", 1.0f},
              {"lvPureTime", 3.0f},
              {"motion0", beat},
              {"rate0", 0.08f},
              {"motion2", beat},
              {"rate2", -0.06f}},
             {});
        // hold one key: a walker goes along the fifths of the Tonnetz - through the room - until he
        // meets the wolf
        makeFull(u8("157 Tonnetz-Wanderer \xe2\x80\x93 Quintenweg"),
                 {{"stop1", 1},
                  {"stop3", 1},
                  {"lvNetOn", 1},
                  {"lvNetPath", 0},
                  {"lvNetRate", 4.0f},
                  {"lvNetGate", 1.5f}},
                 {}, {}, false, {{1, tonnetz}, {3, tonnetz}});
        makeFull(u8("158 Tonnetz-Wanderer \xe2\x80\x93 Heimweh"),
                 {{"stop0", 1},
                  {"lvNetOn", 1},
                  {"lvNetPath", 3},
                  {"lvNetRate", 5.0f},
                  {"lvNetGate", 2.5f},
                  {"lvSympLevel", -3.0f},
                  {"lvPure", 0.6f}},
                 {}, {}, false, {{0, tonnetz}});
        // all of it, gently: a treading Kalkant, hesitant keys, an ageing organ that rings along
        // and tunes itself, heard while walking
        make(u8("159 Lebendige Orgel"),
             {{"stop0", 1},
              {"stop1", 1},
              {"stop2", 1},
              {"lvBellows", 0.5f},
              {"lvBellowsSize", 5.0f},
              {"lvBlower", 1},
              {"lvTreadRate", 10.0f},
              {"lvPallet", 0.5f},
              {"lvWear", 0.6f},
              {"lvWearTime", 10.0f},
              {"lvSympLevel", -6.0f},
              {"lvPure", 0.7f},
              {"lvWalkShape", 4},
              {"lvWalkSize", 4.0f},
              {"lvWalkRate", 0.02f},
              {"windModel", 1}},
             {});
    }

    // ---- set 17: the 3D Saal - pipes as bodies in the room (Flug, Fall, Schwarm, Strömung,
    //      Schweben), knocks, the player and the illusions; some bring one of the built-in rooms
    //      (loaded, the Saal fitted to it) and the light off. Every one is played with the hands:
    //      mod wheel, velocity, pressure, pitch wheel (+-2 semitones)
    currentSet = 17;
    {
        const float bend2 = 1.0f / 6.0f;
        const int flight = (int)Motion::Flight, fall = (int)Motion::Fall, swarm = (int)Motion::Swarm,
                  flow = (int)Motion::Flow, hover = (int)Motion::Hover;
        // a route amount that moves a parameter from 'from' to 'to' (the ranges are skewed: worked
        // out, not guessed)
        auto span = [&](const char *id, float from, float to)
        {
            auto *prm = apvts.getParameter(id);
            return prm == nullptr ? 0.0f : prm->convertTo0to1(to) - prm->convertTo0to1(from);
        };
        const char *kapelle = "rysum_kapelle.obj", *halle = "rysum_hallenkirche.obj",
                   *krypta = "rysum_krypta.obj", *rotunde = "rysum_rotunde.obj",
                   *kreuzgang = "rysum_kreuzgang.obj", *turm = "rysum_treppenturm.obj";
        auto room = [&](const juce::String &name, Values values, std::vector<Route> routesIn,
                        const char *model, int dark)
        { makeFull(name, std::move(values), std::move(routesIn), {}, false, {}, model, dark); };

        // the crypt, dark: the pipes hang free where they are; whoever walks pushes them away -
        // into the columns, where they knock and flash (mod wheel: a harder push)
        room(u8("160 Krypta \xe2\x80\x93 Pfeifen im Dunkeln"),
             {{"stop0", 1},
              {"stop1", 1},
              {"simOn", 1},
              {"master", -23.0f},
              {"motion0", hover},
              {"motion1", hover},
              {"depth0", 150.0f},
              {"depth1", 150.0f},
              {"rate0", 0.04f},
              {"rate1", 0.04f},
              {"lvPlayerMode", 2},
              {"lvPlayerForce", 0.3f},
              {"lvPlayerRange", 3.0f},
              {"lvWalkShape", 4},
              {"lvWalkSize", 2.5f},
              {"lvWalkRate", 0.02f},
              {"lvImpLevel", -8.0f},
              {"lvImpRing", 0.9f}},
             {{srcModWheel, "lvPlayerForce", 0.7f},
              {srcVelocity, pipe(tgtLevel), 0.3f},
              {srcVelocity, pipe(tgtAttack), 0.5f},
              {srcPitchWheel, pipe(tgtPitch), bend2}},
             krypta, 1);
        // the chapel, dark: the 4' and 2' fly as a flock of glow-worms and whisper close by
        // (mod wheel: a swarm in a hurry; pressure: brighter)
        room(u8("161 Kapelle \xe2\x80\x93 Gl\xc3\xbchw\xc3\xbcrmchen"),
             {{"stop2", 1},
              {"stop3", 1},
              {"simOn", 1},
              {"master", -19.0f},
              {"motion2", swarm},
              {"motion3", swarm},
              {"rate2", 0.12f},
              {"rate3", 0.14f},
              {"depth2", 90.0f},
              {"depth3", 110.0f},
              {"lvWhisper", 0.6f},
              {"lvImpLevel", -14.0f}},
             {{srcModWheel, "rate2", span("rate2", 0.12f, 0.6f)},
              {srcModWheel, "rate3", span("rate3", 0.14f, 0.7f)},
              {srcPressure, pipe(tgtBright), 0.4f},
              {srcVelocity, pipe(tgtAttack), 0.6f},
              {srcPitchWheel, pipe(tgtPitch), bend2}},
             kapelle, 1);
        // the hall church: tin pipes rain from the vaults, bounce high and play themselves where
        // they land (mod wheel: more of their own notes)
        room(u8("162 Hallenkirche \xe2\x80\x93 Zinnhagel"),
             {{"stop3", 1},
              {"stop5", 1},
              {"simOn", 1},
              {"master", -24.0f},
              {"motion3", fall},
              {"motion5", fall},
              {"rate3", 0.6f},
              {"rate5", 0.7f},
              {"depth3", 140.0f},
              {"depth5", 150.0f},
              {"lvImpLevel", -4.0f},
              {"lvImpSrc", 1},
              {"lvImpRing", 1.2f},
              {"lvSelfPlay", 0.35f}},
             {{srcModWheel, "lvSelfPlay", 0.6f},
              {srcModWheel, "rate3", span("rate3", 0.6f, 1.2f)},
              {srcVelocity, pipe(tgtLevel), 0.3f},
              {srcPitchWheel, pipe(tgtPitch), bend2}},
             halle, 0);
        // the stair tower: the wooden Gedackt tumbles down a hundred steps, dull and dry
        // (mod wheel / pressure: thrown up higher)
        room(u8("163 Treppenturm \xe2\x80\x93 Holz f\xc3\xa4llt die Stufen hinab"),
             {{"stop1", 1},
              {"simOn", 1},
              {"master", -23.0f},
              {"motion1", fall},
              {"rate1", 0.35f},
              {"depth1", 60.0f},
              {"lvImpLevel", 0.0f},
              {"lvImpSrc", 1},
              {"lvImpRing", 0.5f},
              {"lvSelfPlay", 0.25f}},
             {{srcModWheel, "rate1", span("rate1", 0.35f, 1.0f)},
              {srcPressure, "rate1", span("rate1", 0.35f, 0.7f)},
              {srcVelocity, pipe(tgtAttack), 0.6f},
              {srcPitchWheel, pipe(tgtPitch), bend2}},
             turm, 0);
        // the rotunda: Principal and Octave fly straight, off the ring of columns and the dome,
        // every hard knock its own note (mod wheel: faster)
        room(u8("164 Rotunde \xe2\x80\x93 Billard"),
             {{"stop0", 1},
              {"stop2", 1},
              {"simOn", 1},
              {"master", -25.0f},
              {"motion0", flight},
              {"motion2", flight},
              {"rate0", 0.25f},
              {"rate2", 0.3f},
              {"lvImpLevel", -6.0f},
              {"lvSelfPlay", 0.4f}},
             {{srcModWheel, "rate0", span("rate0", 0.25f, 1.0f)},
              {srcModWheel, "rate2", span("rate2", 0.3f, 1.2f)},
              {srcVelocity, pipe(tgtAttSharp), 0.8f},
              {srcVelocity, pipe(tgtLevel), 0.25f},
              {srcPitchWheel, pipe(tgtPitch), bend2}},
             rotunde, 0);
        // the cloister: a current carries the pipes round the courtyard; the listener walks the
        // arcade and drags them along in a wake (mod wheel: a stronger wake)
        room(u8("165 Kreuzgang \xe2\x80\x93 Str\xc3\xb6mung um den Hof"),
             {{"stop1", 1},
              {"stop2", 1},
              {"stop6", 1},
              {"simOn", 1},
              {"master", -23.0f},
              {"motion1", flow},
              {"motion2", flow},
              {"motion6", flow},
              {"rate1", 0.15f},
              {"rate2", 0.18f},
              {"rate6", 0.1f},
              {"depth1", 50.0f},
              {"depth2", 70.0f},
              {"depth6", 30.0f},
              {"lvPlayerMode", 4},
              {"lvPlayerForce", 0.4f},
              {"lvPlayerRange", 4.0f},
              {"lvWalkShape", 1},
              {"lvWalkSize", 8.0f},
              {"lvWalkRate", 0.015f},
              {"lvImpLevel", -12.0f}},
             {{srcModWheel, "lvPlayerForce", 0.6f},
              {srcPressure, pipe(tgtBright), 0.35f},
              {srcVelocity, pipe(tgtLevel), 0.3f},
              {srcPitchWheel, pipe(tgtPitch), bend2}},
             kreuzgang, 0);
        // the rotunda, dark: Trompete and 16' circle the listener like planets - lamps going
        // round (mod wheel: a tighter, faster orbit)
        room(u8("166 Rotunde im Dunkeln \xe2\x80\x93 Umlaufbahn"),
             {{"stop6", 1},
              {"stop7", 1},
              {"simOn", 1},
              {"master", -27.0f},
              {"motion6", hover},
              {"motion7", hover},
              {"depth6", 180.0f},
              {"depth7", 180.0f},
              {"rate6", 0.03f},
              {"rate7", 0.03f},
              {"lvPlayerMode", 3},
              {"lvPlayerForce", 0.35f},
              {"lvPlayerRange", 6.0f},
              {"lvImpLevel", -10.0f}},
             {{srcModWheel, "lvPlayerForce", 0.6f},
              {srcVelocity, pipe(tgtLevel), 0.35f},
              {srcVelocity, pipe(tgtAttack), 0.4f},
              {srcPitchWheel, pipe(tgtPitch), bend2}},
             rotunde, 1);
        // no room of its own (any Saal): the listener walks to and fro, the trumpets flee and
        // crash into the walls (mod wheel: more fear)
        make(u8("167 Fliehende Trompeten"),
             {{"stop6", 1},
              {"simOn", 1},
              {"master", -18.0f},
              {"simW", 14.0f},
              {"simL", 30.0f},
              {"simH", 12.0f},
              {"motion6", hover},
              {"depth6", 160.0f},
              {"rate6", 0.05f},
              {"lvPlayerMode", 2},
              {"lvPlayerForce", 0.5f},
              {"lvPlayerRange", 5.0f},
              {"lvWalkShape", 3},
              {"lvWalkSize", 7.0f},
              {"lvWalkRate", 0.04f},
              {"lvImpLevel", -6.0f},
              {"lvImpRing", 1.0f}},
             {{srcModWheel, "lvPlayerForce", 0.5f},
              {srcVelocity, pipe(tgtAttack), 0.7f},
              {srcVelocity, pipe(tgtAttSharp), 0.5f},
              {srcPitchWheel, pipe(tgtPitch), bend2}});
        // a small Saal full of flying pipes: the silent ones grow (deeper), a hard knock breaks
        // one in two (an octave up) (mod wheel: faster growing, faster flying)
        make(u8("168 Wachsen und Brechen"),
             {{"stop0", 1},
              {"stop1", 1},
              {"simOn", 1},
              {"master", -23.0f},
              {"simW", 9.0f},
              {"simL", 14.0f},
              {"simH", 7.0f},
              {"motion0", flight},
              {"motion1", flight},
              {"rate0", 0.3f},
              {"rate1", 0.25f},
              {"lvGrow", 0.4f},
              {"lvImpLevel", -10.0f}},
             {{srcModWheel, "lvGrow", 0.6f},
              {srcModWheel, "rate0", span("rate0", 0.3f, 0.9f)},
              {srcVelocity, pipe(tgtLevel), 0.3f},
              {srcPitchWheel, pipe(tgtPitch), bend2}});
        // the pipes come to the listener and whisper into one ear (headphones / stereo)
        // (mod wheel: drawn closer; pressure: louder)
        make(u8("169 Fl\xc3\xbcstern am Ohr"),
             {{"stop1", 1},
              {"stop2", 1},
              {"simOn", 1},
              {"master", -20.0f},
              {"motion1", hover},
              {"motion2", hover},
              {"depth1", 170.0f},
              {"depth2", 170.0f},
              {"rate1", 0.06f},
              {"rate2", 0.06f},
              {"lvPlayerMode", 1},
              {"lvPlayerForce", 0.15f},
              {"lvPlayerRange", 3.0f},
              {"lvWalkShape", 1},
              {"lvWalkSize", 1.5f},
              {"lvWalkRate", 0.03f},
              {"lvWhisper", 1.0f},
              {"lvImpLevel", -18.0f}},
             {{srcModWheel, "lvPlayerForce", 0.5f},
              {srcPressure, pipe(tgtLevel), 0.3f},
              {srcVelocity, pipe(tgtAttack), 0.5f},
              {srcPitchWheel, pipe(tgtPitch), bend2}});
        // the hall church: the far end plays the past - the mod wheel walks you there (forwards:
        // time runs back; let go: it catches up)
        room(u8("170 Hallenkirche \xe2\x80\x93 R\xc3\xbc"
                "ckw\xc3\xa4rts durch die Zeit"),
             {{"stop0", 1},
              {"stop2", 1},
              {"simOn", 1},
              {"master", -16.0f},
              {"motion0", flow},
              {"motion2", flow},
              {"rate0", 0.06f},
              {"rate2", 0.08f},
              {"depth0", 40.0f},
              {"depth2", 40.0f},
              {"lvTimeDepth", 8.0f}},
             {{srcModWheel, "lvListenY", span("lvListenY", 0.0f, 16.0f)},
              {srcVelocity, pipe(tgtAttack), 0.6f},
              {srcPressure, pipe(tgtBright), 0.3f},
              {srcPitchWheel, pipe(tgtPitch), bend2}},
             halle, 0);
        // the chapel seen, a cathedral heard: the mod wheel makes the heard room grow while the
        // walls stay where they are
        room(u8("171 Kapelle sehen, Kathedrale h\xc3\xb6ren"),
             {{"stop7", 1},
              {"stop0", 1},
              {"stop1", 1},
              {"simOn", 1},
              {"master", -21.0f},
              {"motion0", hover},
              {"motion1", hover},
              {"motion7", hover},
              {"depth0", 120.0f},
              {"depth1", 120.0f},
              {"depth7", 120.0f},
              {"lvPlayerMode", 4},
              {"lvPlayerForce", 0.3f},
              {"lvRoomHear", 1.5f}},
             {{srcModWheel, "lvRoomHear", span("lvRoomHear", 1.5f, 6.0f)},
              {srcVelocity, pipe(tgtLevel), 0.3f},
              {srcPitchWheel, pipe(tgtPitch), bend2}},
             kapelle, 0);
        // the cloister: near a wall the reverb opens as if another room lay behind it - the mod
        // wheel walks you to the side wall
        room(u8("172 Kreuzgang \xe2\x80\x93 Die Wand \xc3\xb6"
                "ffnet sich"),
             {{"stop1", 1},
              {"stop3", 1},
              {"simOn", 1},
              {"master", -18.0f},
              {"motion1", flow},
              {"motion3", flow},
              {"rate1", 0.1f},
              {"rate3", 0.14f},
              {"depth1", 60.0f},
              {"depth3", 80.0f},
              {"lvWallPull", 1.0f},
              {"lvImpLevel", -14.0f}},
             {{srcModWheel, "lvListenX", span("lvListenX", 0.0f, 10.0f)},
              {srcVelocity, pipe(tgtAttack), 0.5f},
              {srcPressure, pipe(tgtBright), 0.4f},
              {srcPitchWheel, pipe(tgtPitch), bend2}},
             kreuzgang, 0);
        // the hall church, dark: the plenum swarms through the nave and is its only light - play
        // harder and it shines brighter (mod wheel: a wilder swarm)
        room(u8("173 Hallenkirche im Dunkeln \xe2\x80\x93 Lichtorgel"),
             {{"stop0", 1},
              {"stop2", 1},
              {"stop3", 1},
              {"stop5", 1},
              {"simOn", 1},
              {"master", -23.0f},
              {"motion0", swarm},
              {"motion2", swarm},
              {"motion3", swarm},
              {"motion5", swarm},
              {"rate0", 0.18f},
              {"rate2", 0.2f},
              {"rate3", 0.22f},
              {"rate5", 0.25f},
              {"depth0", 60.0f},
              {"depth2", 70.0f},
              {"depth3", 80.0f},
              {"depth5", 90.0f},
              {"lvImpLevel", -12.0f}},
             {{srcVelocity, pipe(tgtLevel), 0.4f},
              {srcVelocity, pipe(tgtAttack), 0.4f},
              {srcPressure, pipe(tgtBright), 0.4f},
              {srcModWheel, "rate0", span("rate0", 0.18f, 0.7f)},
              {srcModWheel, "rate3", span("rate3", 0.22f, 0.9f)},
              {srcPitchWheel, pipe(tgtPitch), bend2}},
             halle, 1);
        // the mod wheel goes through four ways of being a body in the room: hovering -> carried by
        // a current -> flying -> falling
        makeFull(u8("174 Modrad \xe2\x80\x93 Vom Schweben zum Sturz"),
                 {{"stop0", 1},
                  {"stop1", 1},
                  {"stop2", 1},
                  {"simOn", 1},
              {"master", -20.0f},
                  {"simW", 16.0f},
                  {"simL", 28.0f},
                  {"simH", 12.0f},
                  {"motion0", hover},
                  {"motion1", hover},
                  {"motion2", hover},
                  {"depth0", 120.0f},
                  {"depth1", 120.0f},
                  {"depth2", 120.0f},
                  {"lvImpLevel", -8.0f}},
                 {{srcModWheel, "sceneMorph", 1.0f},
                  {srcVelocity, pipe(tgtAttack), 0.6f},
                  {srcPitchWheel, pipe(tgtPitch), bend2}},
                 {{},
                  {{"motion0", flow},
                   {"motion1", flow},
                   {"motion2", flow},
                   {"rate0", 0.15f},
                   {"rate1", 0.12f},
                   {"rate2", 0.18f},
                   {"depth0", 60.0f},
                   {"depth1", 60.0f},
                   {"depth2", 60.0f}},
                  {{"motion0", flight},
                   {"motion1", flight},
                   {"motion2", flight},
                   {"rate0", 0.45f},
                   {"rate1", 0.4f},
                   {"rate2", 0.5f}},
                  {{"motion0", fall},
                   {"motion1", fall},
                   {"motion2", fall},
                   {"rate0", 0.6f},
                   {"rate1", 0.5f},
                   {"rate2", 0.7f},
                   {"depth0", 150.0f},
                   {"depth1", 150.0f},
                   {"depth2", 150.0f},
                   {"lvSelfPlay", 0.4f}}},
                 false);
        // the stair tower, dark: Mixtur and 2' fall and are thrown up again for ever, and every
        // knock plays the pipe - the room plays itself and lights its own steps (just listen, or
        // play along: the mod wheel throws them higher)
        room(u8("175 Treppenturm im Dunkeln \xe2\x80\x93 Selbstspiel"),
             {{"stop3", 1},
              {"stop5", 1},
              {"simOn", 1},
              {"master", -28.0f},
              {"motion3", fall},
              {"motion5", fall},
              {"rate3", 0.5f},
              {"rate5", 0.55f},
              {"depth3", 120.0f},
              {"depth5", 130.0f},
              {"lvSelfPlay", 0.8f},
              {"lvImpLevel", -10.0f}},
             {{srcModWheel, "rate3", span("rate3", 0.5f, 1.1f)},
              {srcModWheel, "rate5", span("rate5", 0.55f, 1.2f)},
              {srcVelocity, pipe(tgtLevel), 0.3f},
              {srcPitchWheel, pipe(tgtPitch), bend2}},
             turm, 1);
    }

    marker.replaceWithText(juce::String(kFactoryVersion));
}

juce::AudioProcessorEditor *RysumProcessor::createEditor()
{
    return new RysumEditor(*this);
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter()
{
    return new RysumProcessor();
}
