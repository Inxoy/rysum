#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "libMTSClient.h"

using namespace rysum;

// ---------------------------------------------------------------------------
// background sample loading
// ---------------------------------------------------------------------------
class RysumProcessor::Loader : public juce::Thread
{
public:
    Loader (RysumProcessor& p, juce::File f) : juce::Thread ("Rysum sample loader"), owner (p), folder (std::move (f)) {}
    ~Loader() override { stopThread (10000); }

    void run() override
    {
        juce::String err;
        auto lib = SampleLibrary::load (folder, err, [this] { return threadShouldExit(); });
        if (threadShouldExit()) return;
        std::shared_ptr<const SampleLibrary> shared (lib.release());
        juce::MessageManager::callAsync ([proc = &owner, shared, err, folder = folder, alive = owner.aliveFlag]
        {
            if (! alive->load()) return;
            proc->onLoaded (shared, err, folder);
        });
    }

private:
    RysumProcessor& owner;
    juce::File folder;
};

// ---------------------------------------------------------------------------
juce::AudioProcessorValueTreeState::ParameterLayout RysumProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    auto pct = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (juce::roundToInt (v * 100)) + " %"; });

    for (int s = 0; s < kNumStops; ++s)
        l.add (std::make_unique<AudioParameterBool> (ParameterID { stopParamId (s), 1 }, prettyLabel (kStops[(size_t) s].label), s == 0));
    for (int s = 0; s < kNumStops; ++s)
    {
        const String name = prettyLabel (kStops[(size_t) s].label);
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { volParamId (s), 1 }, name + u8 (" Lautst\xc3\xa4rke"),
                                                      NormalisableRange<float> (-48.0f, 12.0f, 0.1f), kDefaultStopVolume[(size_t) s],
                                                      AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v > 0 ? "+" : "") + String (v, 1) + " dB"; })));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { windParamId (s), 1 }, name + " Wind", NormalisableRange<float> (0, 1), 1.0f, pct));
    }

    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "master", 1 }, u8 ("Lautst\xc3\xa4rke"), NormalisableRange<float> (-60.0f, 12.0f, 0.1f), -12.0f,
                                                  AudioParameterFloatAttributes().withLabel ("dB")));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "output", 1 }, "Ausgabe",
                                                   StringArray { "Stereo", "AmbiX 1", "AmbiX 2", "AmbiX 3", "AmbiX 4", "AmbiX 5", "Binaural", "Lautsprecher" }, 0));
    // loudspeakers: AllRAD decoding of this order to the layout set up on the page "Lautsprecher"
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "spkOrder", 1 }, "Lautsprecher-Ordnung",
                                                   StringArray { "1. Ordnung", "2. Ordnung", "3. Ordnung", "4. Ordnung", "5. Ordnung" }, 2));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "spkDistComp", 1 }, "Lautsprecher-Abstand ausgleichen", true));
    // binaural: Ambisonics of this order, decoded with HRTFs (FABIAN or an own SOFA file)
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "binOrder", 1 }, "Binaural-Ordnung",
                                                   StringArray { "1. Ordnung", "2. Ordnung", "3. Ordnung", "4. Ordnung", "5. Ordnung" }, 2));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "binYaw", 1 }, "Kopfdrehung", NormalisableRange<float> (-180.0f, 180.0f, 0.1f), 0.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                                                  { return std::abs (v) < 0.05f ? String ("geradeaus") : String (roundToInt (std::abs (v))) + juce::String::fromUTF8 ("\xc2\xb0 ") + (v > 0 ? "links" : "rechts"); })));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "binDfEq", 1 }, "Diffusfeld-Entzerrung", true));

    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "click", 1 },        "Tastenklick",        NormalisableRange<float> (0, 1), 0.2f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "clickBlend", 1 },   "Klick ins Anschlagen", NormalisableRange<float> (0, 1), 0.45f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "releaseClick", 1 }, "Loslassklick",       NormalisableRange<float> (0, 1), 0.2f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "stopPull", 1 },     "Registerzug",        NormalisableRange<float> (0, 1), 0.70f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "wind", 1 },         "Wind",               NormalisableRange<float> (0, 1), 0.0f, pct));

    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "degAmount", 1 },   "Verfall Menge",     NormalisableRange<float> (0, 1), 0.0f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "degGrain", 1 },    u8 ("K\xc3\xb6rnung"),          NormalisableRange<float> (0, 1), 0.0f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "degBits", 1 },     "Bittiefe",          NormalisableRange<float> (3.0f, 16.0f, 0.1f), 16.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " bit"; })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "degRate", 1 },     "Abtastrate",        NormalisableRange<float> (600.0f, 48000.0f, 1.0f, 0.35f), 48000.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v >= 1000 ? String (v / 1000.0f, 2) + " kHz" : String (roundToInt (v)) + " Hz"; })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "degWobble", 1 },   "Windschwankung",    NormalisableRange<float> (0, 1), 0.2f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "degDropouts", 1 }, "Aussetzer",         NormalisableRange<float> (0, 1), 0.08f, pct));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "degMode", 1 },    "Verfall auf",       StringArray { "pro Pfeife", "Summe" }, 0));

    auto dbAttr = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v <= -59.9f ? String ("aus") : (v > 0 ? "+" : "") + String (v, 1) + " dB"; });
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "irDry", 1 }, "Direkt", NormalisableRange<float> (-60.0f, 6.0f, 0.1f, 2.0f), 0.0f, dbAttr));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "irWet", 1 }, "Hall",   NormalisableRange<float> (-60.0f, 12.0f, 0.1f, 2.0f), -6.0f, dbAttr));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "irFuma", 1 }, "IR FuMa", false));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "irN3D", 1 }, "IR N3D", false));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "irOrder", 1 }, "Hall-Ordnung",
                                                   StringArray { "1. Ordnung", "2. Ordnung", "3. Ordnung", "4. Ordnung", "5. Ordnung" }, 2));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "irFollow", 1 }, u8 ("Hall folgt den Pfeifen"), true));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "irEnvelop", 1 }, u8 ("Umh\xc3\xbcllung"), NormalisableRange<float> (0, 1), 0.6f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "irDirect", 1 }, "Direktschall der IR", NormalisableRange<float> (0, 1), 0.2f, pct));

    // distance
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "doppler", 1 }, "Doppler", NormalisableRange<float> (0.0f, 4.0f, 0.01f), 1.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v < 0.005f ? String ("aus") : String (v, 2) + juce::String::fromUTF8 (" \xc3\x97"); })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "air", 1 }, "Luft", NormalisableRange<float> (0, 1), 0.5f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "distAmount", 1 }, "Entfernung", NormalisableRange<float> (0, 1), 0.7f, pct));

    // tuning
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "temperament", 1 }, "Stimmung", temperamentNames(), 0));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "pitch", 1 }, "Stimmton", NormalisableRange<float> (-200.0f, 200.0f, 0.1f), 0.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                                                  { return juce::String::fromUTF8 ("a\xe2\x80\xb2 = ") + String (kA4 * std::pow (2.0, v / 1200.0), 1) + " Hz"; })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "detune", 1 }, "Verstimmung", NormalisableRange<float> (0.0f, 60.0f, 0.1f, 0.6f), 0.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return juce::String::fromUTF8 ("\xc2\xb1") + String (v, 1) + " ct"; })));

    // tremulant
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "tremOn", 1 }, "Tremulant", false));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "tremRate", 1 }, "Tremulant Tempo", NormalisableRange<float> (1.5f, 12.0f, 0.01f, 0.7f), 5.5f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 2) + " Hz"; })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "tremDepth", 1 }, "Tremulant Tiefe", NormalisableRange<float> (0, 1), 0.5f, pct));

    // freeze
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "freeze", 1 }, "Freeze", false));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "freezeSmear", 1 }, "Verwischen", NormalisableRange<float> (0, 1), 0.5f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "freezeRelease", 1 }, "Freeze Ausklang", NormalisableRange<float> (0.1f, 12.0f, 0.01f, 0.5f), 2.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " s"; })));

    // MIDI / tempo
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "motionSync", 1 }, "Bewegung im Tempo", false));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "midiCC", 1 }, "MIDI-CC", true));

    for (int s = 0; s < kNumStops; ++s)
    {
        const String name = prettyLabel (kStops[(size_t) s].label);
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { motionParamId (s), 1 }, name + " Bewegung", motionNames(), 0));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { rateParamId (s), 1 }, name + " Geschwindigkeit",
                                                      NormalisableRange<float> (-500.0f, 500.0f, 0.0001f, 0.2f, true), 0.08f,
                                                      AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v > 0 ? "+" : "") + String (v, std::abs (v) < 1.0f ? 3 : 2) + " Hz"; })));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { ellipseParamId (s), 1 }, name + " Ellipse",
                                                      NormalisableRange<float> (0.0f, 0.9f, 0.001f), 0.0f, pct));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { depthParamId (s), 1 }, name + " Tiefe",
                                                      NormalisableRange<float> (0.0f, 180.0f, 0.1f), 40.0f,
                                                      AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0")); })));
    }

    // ---- modulation
    auto hz = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, v < 1.0f ? 3 : 2) + " Hz"; });
    auto secs = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                { return v < 1.0f ? String (roundToInt (v * 1000.0f)) + " ms" : String (v, 2) + " s"; });
    for (int i = 0; i < kNumLfos; ++i)
    {
        const String n = "LFO " + String (i + 1);
        const String id = "lfo" + String (i + 1);
        const float rates[] { 0.25f, 0.5f, 2.0f, 0.1f };
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { id + "Shape", 1 }, n + " Form", lfoShapeNames(), i == 3 ? (int) shSmooth : (int) shSine));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Rate", 1 }, n + " Tempo", NormalisableRange<float> (0.01f, 20.0f, 0.001f, 0.3f), rates[i], hz));
        l.add (std::make_unique<AudioParameterBool> (ParameterID { id + "Sync", 1 }, n + " Sync", false));
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { id + "Div", 1 }, n + " Notenwert", lfoDivNames(), 2));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Spread", 1 }, n + " Versatz", NormalisableRange<float> (0, 1), 0.0f, pct));
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { id + "SpreadMode", 1 }, n + " Versatz-Art", StringArray { "Tastatur", "Zufall" }, 0));
    }
    for (int j = 0; j < kNumEnvs; ++j)
    {
        const String n = String::fromUTF8 ("H\xc3\xbcllkurve ") + String (j + 1);
        const String id = "env" + String (j + 1);
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { id + "Mode", 1 }, n + " Modus", StringArray { "Pro Pfeife", "Global" }, 0));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Delay", 1 },   n + String::fromUTF8 (" Verz\xc3\xb6gerung"), NormalisableRange<float> (0.0f, 10.0f, 0.001f, 0.35f), 0.0f, secs));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Attack", 1 },  n + " Anstieg",  NormalisableRange<float> (0.001f, 20.0f, 0.001f, 0.3f), j == 0 ? 0.3f : 3.0f, secs));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Decay", 1 },   n + " Abfall",   NormalisableRange<float> (0.001f, 20.0f, 0.001f, 0.3f), j == 0 ? 1.0f : 4.0f, secs));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Sustain", 1 }, n + " Halten",   NormalisableRange<float> (0, 1), j == 0 ? 0.6f : 0.0f, pct));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id + "Release", 1 }, n + " Ausklang", NormalisableRange<float> (0.001f, 20.0f, 0.001f, 0.3f), j == 0 ? 1.5f : 3.0f, secs));
    }
    for (int r = 0; r < kNumRoutes; ++r)
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "mod" + String (r + 1) + "Amt", 1 }, "Modulation " + String (r + 1) + " Menge",
                                                      NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f,
                                                      AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                                                      { return (v > 0.0005f ? "+" : "") + String (roundToInt (v * 100.0f)) + " %"; })));

    // ---- effects (page "Effekte"); all off by default, so older patches sound the same
    auto deg = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + String::fromUTF8 ("\xc2\xb0"); });
    auto degS = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + String::fromUTF8 ("\xc2\xb0/s"); });
    auto sdeg = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v > 0.5f ? "+" : "") + String (roundToInt (v)) + String::fromUTF8 ("\xc2\xb0"); });
    auto cnt = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)); });
    auto ms = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, v < 1.0f ? 2 : 1) + " ms"; });
    auto times = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + String::fromUTF8 (" \xc3\x97"); });
    auto shz = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return std::abs (v) < 0.0005f ? String ("steht") : (v > 0 ? "+" : "") + String (v, 2) + " Hz"; });
    auto fhz = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v >= 1000 ? String (v / 1000.0f, 1) + " kHz" : String (roundToInt (v)) + " Hz"; });
    auto F = [&] (const char* id, const String& name, NormalisableRange<float> r, float def, AudioParameterFloatAttributes a)
    { l.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def, a)); };
    const String ae = String::fromUTF8 ("\xc3\xa4"), oe = String::fromUTF8 ("\xc3\xb6"), ue = String::fromUTF8 ("\xc3\xbc");

    F ("fxCustLevel",   "Eigene Ger" + ae + "usche",           NormalisableRange<float> (0, 1), 0.0f, pct);
    F ("fxCustDown",    "Eigene: beim Anschlag",               NormalisableRange<float> (0, 1), 1.0f, pct);
    F ("fxCustUp",      "Eigene: beim Loslassen",              NormalisableRange<float> (0, 1), 0.0f, pct);
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "fxCustPlace", 1 }, "Eigene: Ort",
                                                   StringArray { "an der Pfeife", "irgendwo im Raum", "am Spieltisch", "kreisend" }, 0));
    F ("fxCustPitch",   "Eigene: Tonh" + oe + "he folgt",      NormalisableRange<float> (0, 1), 0.0f, pct);
    F ("fxCustScatter", "Eigene: Streuung",                    NormalisableRange<float> (0, 1), 0.0f, pct);
    F ("fxCustWander",  "Eigene: Wandern",                     NormalisableRange<float> (0, 360, 0.1f, 0.5f), 0.0f, degS);

    F ("fxPathLevel",   "Trakturweg",                          NormalisableRange<float> (0, 1), 0.0f, pct);
    F ("fxPathTime",    "Trakturweg Laufzeit",                 NormalisableRange<float> (0.01f, 0.6f, 0.001f, 0.5f), 0.12f, secs);
    F ("fxPathTicks",   "Trakturweg Schritte",                 NormalisableRange<float> (2, 16, 1), 6, cnt);
    F ("fxPathSpread",  "Trakturweg Streuung",                 NormalisableRange<float> (0, 90, 0.1f), 12.0f, deg);
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "fxPathWait", 1 }, "Pfeifen warten auf die Traktur", false));

    F ("fxCrackLevel",  "Knistern",                            NormalisableRange<float> (0, 1), 0.0f, pct);
    F ("fxCrackRate",   "Knistern Dichte",                     NormalisableRange<float> (0.2f, 80.0f, 0.01f, 0.4f), 6.0f, hz);
    F ("fxCrackLen",    "Knistern L" + ae + "nge",             NormalisableRange<float> (0.1f, 8.0f, 0.01f, 0.5f), 1.2f, ms);

    F ("fxLeakLevel",   "Windlecks",                           NormalisableRange<float> (0, 1), 0.0f, pct);
    F ("fxLeakCount",   "Windlecks Anzahl",                    NormalisableRange<float> (1, 8, 1), 3, cnt);
    F ("fxLeakFreq",    "Windlecks Tonh" + oe + "he",          NormalisableRange<float> (800.0f, 12000.0f, 1.0f, 0.4f), 4000.0f, fhz);
    F ("fxLeakWander",  "Windlecks Wandern",                   NormalisableRange<float> (0, 90, 0.1f, 0.6f), 0.0f, degS);
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "fxWindRound", 1 }, "Geb" + ae + "se ringsum", false));

    F ("fxPartSpread",  "Obert" + oe + "ne verteilen",         NormalisableRange<float> (0, 180, 0.1f), 0.0f, deg);
    F ("fxPartSplit",   "Obert" + oe + "ne ab",                NormalisableRange<float> (1.5f, 12.0f, 0.01f, 0.6f), 3.0f, times);
    F ("fxPartTurn",    "Obert" + oe + "ne kreisen",           NormalisableRange<float> (-2.0f, 2.0f, 0.001f, 0.4f, true), 0.0f, shz);

    F ("fxEchoLevel",   "Echos",                               NormalisableRange<float> (0, 1), 0.0f, pct);
    F ("fxEchoTime",    "Echo Zeit",                           NormalisableRange<float> (0.03f, 2.0f, 0.001f, 0.5f), 0.35f, secs);
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "fxEchoSync", 1 }, "Echo im Tempo", false));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "fxEchoDiv", 1 }, "Echo Notenwert", lfoDivNames(), 4));
    F ("fxEchoRepeats", "Echo Wiederholungen",                 NormalisableRange<float> (1, 8, 1), 4, cnt);
    F ("fxEchoFeedback","Echo Abnahme",                        NormalisableRange<float> (0.0f, 0.95f), 0.6f, pct);
    F ("fxEchoTurn",    "Echo Drehung",                        NormalisableRange<float> (-180, 180, 0.1f), 90.0f, sdeg);
    F ("fxEchoTilt",    "Echo Neigung",                        NormalisableRange<float> (-45, 45, 0.1f), 0.0f, sdeg);
    F ("fxEchoDamp",    "Echo D" + ae + "mpfung",              NormalisableRange<float> (0, 1), 0.3f, pct);

    // ---- simulated room ("Saal"): off by default
    auto metres = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, v < 10.0f ? 1 : 0) + " m"; });
    auto dbOff = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v <= -59.9f ? String ("aus") : (v > 0 ? "+" : "") + String (v, 1) + " dB"; });
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "simOn", 1 }, "Saal-Simulation", false));
    F ("simW",       "Saal Breite",                 NormalisableRange<float> (2.0f, 40.0f, 0.01f, 0.6f), 14.0f, metres);
    F ("simL",       ("Saal L" + ae + "nge"),        NormalisableRange<float> (2.0f, 100.0f, 0.01f, 0.5f), 28.0f, metres);
    F ("simH",       ("Saal H" + oe + "he"),         NormalisableRange<float> (2.0f, 40.0f, 0.01f, 0.6f), 11.0f, metres);
    F ("simSize",    ("Saal Gr" + oe + String::fromUTF8 ("\xc3\x9f") + "e"), NormalisableRange<float> (0.25f, 3.0f, 0.001f, 0.5f), 1.0f, times);
    F ("simLx",      ("Saal H" + oe + "rer quer"),   NormalisableRange<float> (0, 1), 0.5f, pct);
    F ("simLy",      ("Saal H" + oe + "rer l" + ae + "ngs"), NormalisableRange<float> (0, 1), 0.4f, pct);
    F ("simAbs",     ("Saal Absorption W" + ae + "nde"), NormalisableRange<float> (0, 1, 0.001f, 0.5f), 0.06f, pct);
    F ("simAbsFloor", "Saal Absorption Boden/Decke", NormalisableRange<float> (0, 1, 0.001f, 0.5f), 0.2f, pct);
    F ("simHigh",    ("Saal H" + oe + "hend" + ae + "mpfung"), NormalisableRange<float> (0, 1), 0.15f, pct);
    F ("simScatter", "Saal Streuung",               NormalisableRange<float> (0, 1), 0.3f, pct);
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "simOrder", 1 }, "Saal Reflexionen",
                                                   StringArray { "bis 1. Ordnung", "bis 2. Ordnung", "bis 3. Ordnung" }, 1));
    F ("simEr",      ("Saal fr" + ue + "he Reflexionen"), NormalisableRange<float> (-60.0f, 12.0f, 0.1f, 2.0f), 0.0f, dbOff);
    F ("simTail",    "Saal Nachhall",               NormalisableRange<float> (-60.0f, 12.0f, 0.1f, 2.0f), 0.0f, dbOff);

    // ---- wind model of the stops (page "Klang"); classic by default, so older patches sound the same
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "windModel", 1 }, "Windmodell", StringArray { "klassisch", "instabil" }, 0));
    // late reverb of the simulated room: convolution (default) or the feedback delay network
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "simTailModel", 1 }, "Saal Nachhall-Modell", StringArray { "Faltung", "FDN" }, 0));
    // MTS-ESP: held notes follow a changing tuning (or keep the pitch they started with)
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "mtsContinuous", 1 }, u8 ("MTS: gehaltene T\xc3\xb6ne folgen"), true));
    // Lucier mode (off by default): the output passes through the room again and again
    // scenes: where between the stored scenes A..D the sound is (0 = A .. 3 = D)
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "sceneMorph", 1 }, "Szene", NormalisableRange<float> (0.0f, 3.0f, 0.001f), 0.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                                                  {
                                                      const char* n[] { "A", "B", "C", "D" };
                                                      const int i = juce::jlimit (0, 3, (int) std::floor (v + 1.0e-4f));
                                                      const float t = v - (float) i;
                                                      if (t < 0.005f || i >= 3) return String (n[i]);
                                                      if (t > 0.995f) return String (n[i + 1]);
                                                      return String (n[i]) + String::fromUTF8 (" \xe2\x86\x92 ") + n[i + 1] + " " + String (roundToInt (t * 100.0f)) + " %";
                                                  })
                                                  .withValueFromStringFunction ([] (const String& t)
                                                  {
                                                      const auto c = t.trim().toUpperCase();
                                                      if (c.length() == 1 && c[0] >= 'A' && c[0] <= 'D') return (float) (c[0] - 'A');
                                                      return t.getFloatValue();
                                                  })));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "lucOn", 1 }, "Lucier an", false));
    // "Stueck": record, then the recording goes through the room again and again by itself (Lucier's piece);
    // "Live": everything that sounds goes through n passes at once
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "lucMode", 1 }, "Lucier Art", StringArray { String::fromUTF8 ("St\xc3\xbc" "ck (Aufnahme)"), "Live" }, 0));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "lucGap", 1 }, "Lucier Pause", NormalisableRange<float> (0.0f, 10.0f, 0.01f, 0.6f), 1.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " s"; })
                                                                                 .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); })));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "lucRecSrc", 1 }, "Lucier Aufnahme von", StringArray { "alles (Orgel + Eingang)", "nur Eingang" }, 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "lucSource", 1 }, "Lucier Raum", StringArray { "Saal (simuliert)", "Impulsantwort (Seite Raum)" }, 0));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "lucGen", 1 }, "Lucier Generation", NormalisableRange<float> (1.0f, 40.0f, 0.01f, 0.6f), 6.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, v < 10.0f ? 2 : 1); })
                                                                                 .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "lucGrow", 1 }, "Lucier Wachsen", NormalisableRange<float> (0.0f, 20.0f, 0.01f, 0.5f), 0.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v < 0.005f ? String ("steht") : String (v, 2) + " / min"; })
                                                                                 .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "lucTime", 1 }, "Lucier Nachklingen", NormalisableRange<float> (0.03f, 1.5f, 0.001f, 0.5f), 0.3f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 2) + " s"; })
                                                                                 .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "lucMix", 1 }, "Lucier Anteil", NormalisableRange<float> (0.0f, 1.0f), 0.5f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + " %"; })
                                                                                 .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue() / 100.0f; })));
    // MPE (off by default): note channels bend, press and slide their own notes
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "mpe", 1 }, "MPE", false));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "mpeBend", 1 }, "MPE Bend-Bereich", NormalisableRange<float> (1.0f, 96.0f, 1.0f), 48.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " HT"; })
                                                                                 .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); })));

    // ---- audio input ("Eingang", page "Raum"): live sound placed and moved like a pipe. In the instrument off by default
    // (older projects / patches sound the same); in the effect version "Rysum FX" the input is the point, so on.
#if RYSUM_FX
    const bool inputDefault = true;
#else
    const bool inputDefault = false;
#endif
    {
        auto dbIn = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v <= -59.9f ? String ("aus") : (v > 0 ? "+" : "") + String (v, 1) + " dB"; })
                                                   .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); });
        auto degIn = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v > 0.5f ? "+" : "") + String (roundToInt (v)) + String::fromUTF8 ("\xc2\xb0"); })
                                                    .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); });
        auto spinIn = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return std::abs (v) < 0.05f ? String ("steht") : (v > 0 ? "+" : "") + String (v, std::abs (v) < 10.0f ? 1 : 0) + String::fromUTF8 ("\xc2\xb0/s"); })
                                                     .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); });
        l.add (std::make_unique<AudioParameterBool> (ParameterID { "inpOn", 1 }, "Eingang an", inputDefault));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "inpLevel", 1 }, "Eingang Pegel", NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f, dbIn));
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { "inpMode", 1 }, String::fromUTF8 ("Eingang Kan\xc3\xa4le"), StringArray { "Mono", "Stereo" }, 1));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "inpAz", 1 }, "Eingang Richtung", NormalisableRange<float> (-180.0f, 180.0f, 0.1f), 0.0f, degIn));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "inpEl", 1 }, String::fromUTF8 ("Eingang H\xc3\xb6he"), NormalisableRange<float> (-15.0f, 90.0f, 0.1f), 10.0f, degIn));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "inpDist", 1 }, "Eingang Abstand", NormalisableRange<float> (0.5f, 40.0f, 0.01f, 0.4f), 4.0f,
                                                      AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, v < 10.0f ? 1 : 0) + " m"; })
                                                                                     .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); })));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "inpWidth", 1 }, "Eingang Breite", NormalisableRange<float> (0.0f, 180.0f, 0.1f), 60.0f, degIn));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "inpSpin", 1 }, "Eingang Kreisen", NormalisableRange<float> (-360.0f, 360.0f, 0.01f, 0.4f, true), 0.0f, spinIn));
        StringArray follow { "eigener Ort" };
        for (int s = 0; s < kNumStops; ++s) follow.add (String::fromUTF8 ("auf ") + prettyLabel (kStops[(size_t) s].label));
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { "inpFollow", 1 }, "Eingang Ort", follow, 0));
        l.add (std::make_unique<AudioParameterBool> (ParameterID { "inpGate", 1 }, String::fromUTF8 ("Eingang: Tasten \xc3\xb6" "ffnen"), false));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "inpRelease", 1 }, "Eingang Ausklang", NormalisableRange<float> (0.01f, 4.0f, 0.001f, 0.4f), 0.3f,
                                                      AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return v < 1.0f ? String (roundToInt (v * 1000.0f)) + " ms" : String (v, 2) + " s"; })
                                                                                     .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); })));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "inpSend", 1 }, "Eingang Hall-Anteil", NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f, dbIn));
    }

    // ---- shape of the motion per stop (page "Raum"): ellipse direction / tilt / form, drawn path, hard jumps
    for (int s = 0; s < kNumStops; ++s)
    {
        const String name = prettyLabel (kStops[(size_t) s].label);
        auto sdeg2 = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v > 0.5f ? "+" : "") + String (roundToInt (v)) + String::fromUTF8 ("\xc2\xb0"); });
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { ellDirParamId (s), 1 }, name + " Ellipse Richtung",
                                                      NormalisableRange<float> (-180.0f, 180.0f, 0.1f), 0.0f, sdeg2));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { ellTiltParamId (s), 1 }, name + " Ellipse Neigung",
                                                      NormalisableRange<float> (-60.0f, 60.0f, 0.1f), 0.0f, sdeg2));
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { ellFormParamId (s), 1 }, name + " Ellipse Form", ellipseFormNames(), 0));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { pathSpreadParamId (s), 1 }, name + " Verteilung auf der Bahn",
                                                      NormalisableRange<float> (0.0f, 1.0f), 1.0f, pct));
        l.add (std::make_unique<AudioParameterBool> (ParameterID { jumpHardParamId (s), 1 }, name + u8 (" harte Spr\xc3\xbcnge"), false));
    }
    return l;
}

// the audio input: the instrument has a main input (Reaper: the track's own audio; the standalone app: the audio interface)
// and a side chain (Ableton Live and others route other tracks there); the effect "Rysum FX" sits on an audio track
juce::AudioProcessor::BusesProperties RysumProcessor::makeBuses()
{
    auto b = BusesProperties();
#if RYSUM_FX
    b = b.withInput ("Input", juce::AudioChannelSet::stereo(), true);
#else
    b = b.withInput (juce::String::fromUTF8 ("Eingang"), juce::AudioChannelSet::stereo(), true)
         .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true);
#endif
    return b.withOutput ("Output", juce::AudioChannelSet::ambisonic (kMaxOrder), true);
}

RysumProcessor::RysumProcessor()
    : AudioProcessor (makeBuses()),
      apvts (*this, nullptr, "RYSUM", createLayout())
{
    for (int s = 0; s < kNumStops; ++s)
    {
        stopParams[(size_t) s]   = apvts.getRawParameterValue (stopParamId (s));
        volParams[(size_t) s]    = apvts.getRawParameterValue (volParamId (s));
        windParams[(size_t) s]   = apvts.getRawParameterValue (windParamId (s));
        motionParams[(size_t) s] = apvts.getRawParameterValue (motionParamId (s));
        rateParams[(size_t) s]   = apvts.getRawParameterValue (rateParamId (s));
        depthParams[(size_t) s]  = apvts.getRawParameterValue (depthParamId (s));
        ellipseParams[(size_t) s] = apvts.getRawParameterValue (ellipseParamId (s));
        layouts[(size_t) s] = defaultLayout (s);
    }
    master = apvts.getRawParameterValue ("master");
    output = apvts.getRawParameterValue ("output");
    click = apvts.getRawParameterValue ("click");
    clickBlend = apvts.getRawParameterValue ("clickBlend");
    releaseClick = apvts.getRawParameterValue ("releaseClick");
    stopPull = apvts.getRawParameterValue ("stopPull");
    wind = apvts.getRawParameterValue ("wind");
    degAmount = apvts.getRawParameterValue ("degAmount");
    degGrain = apvts.getRawParameterValue ("degGrain");
    degBits = apvts.getRawParameterValue ("degBits");
    degRate = apvts.getRawParameterValue ("degRate");
    degWobble = apvts.getRawParameterValue ("degWobble");
    degDropouts = apvts.getRawParameterValue ("degDropouts");
    degMode = apvts.getRawParameterValue ("degMode");
    irDry = apvts.getRawParameterValue ("irDry");
    irWet = apvts.getRawParameterValue ("irWet");
    irFumaParam = apvts.getRawParameterValue ("irFuma");
    irN3DParam = apvts.getRawParameterValue ("irN3D");
    irFollowParam = apvts.getRawParameterValue ("irFollow");
    irEnvelopParam = apvts.getRawParameterValue ("irEnvelop");
    irDirectParam = apvts.getRawParameterValue ("irDirect");
    for (int t = 0; t < temperamentNames().size(); ++t) tunings.push_back (makeKeyTuning (t));
    binOrderParam = apvts.getRawParameterValue ("binOrder");
    binYawParam = apvts.getRawParameterValue ("binYaw");
    binDfParam = apvts.getRawParameterValue ("binDfEq");
    // the standalone app is mostly used with headphones: start in binaural mode
    if (juce::PluginHostType::getPluginLoadedAs() == wrapperType_Standalone)
        if (auto* o = apvts.getParameter ("output")) o->setValueNotifyingHost (o->convertTo0to1 ((float) kBinauralOutput));
    for (auto* id : { "doppler", "air", "distAmount", "temperament", "pitch", "detune", "tremOn", "tremRate", "tremDepth",
                      "freeze", "freezeSmear", "freezeRelease", "motionSync", "midiCC" })
        extraParams[id] = apvts.getRawParameterValue (id);

    // MIDI CC targets (see handleCc)
    ccTargets.push_back (apvts.getParameter ("wind"));
    for (int s = 0; s < kNumStops; ++s) ccTargets.push_back (apvts.getParameter (stopParamId (s)));
    for (int s = 0; s < kNumStops; ++s) ccTargets.push_back (apvts.getParameter (windParamId (s)));
    ccTargets.push_back (apvts.getParameter ("freeze"));
    ccTargets.push_back (apvts.getParameter ("tremDepth"));
    for (auto& c : ccPending) c = std::numeric_limits<float>::quiet_NaN();
    for (int i = 0; i < kNumMidiMaps; ++i)
    {
        mmCc[(size_t) i] = -1; mmChan[(size_t) i] = 0; mmParam[(size_t) i] = -1;
        mmLo[(size_t) i] = 0.0f; mmHi[(size_t) i] = 1.0f; mmToggle[(size_t) i] = false;
        mmPending[(size_t) i] = std::numeric_limits<float>::quiet_NaN();
    }

    // ---- parameter table (modulation works on indices into it)
    for (auto* prm : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
        {
            paramIndex[rp->getParameterID()] = (int) prmList.size();
            prmList.push_back (rp);
            rawList.push_back (apvts.getRawParameterValue (rp->getParameterID()));
            isFloatParam.push_back (dynamic_cast<juce::AudioParameterFloat*> (rp) != nullptr && rp->getParameterID() != "output" ? 1 : 0);
        }
    const int N = (int) prmList.size();
    modVals.assign ((size_t) N, 0.0f);
    modOff.assign ((size_t) N, 0.0f);
    modHitFlag.assign ((size_t) N, 0);
    modNorm.reset (new std::atomic<float>[(size_t) N]);
    for (int i = 0; i < N; ++i) modNorm[i] = std::numeric_limits<float>::quiet_NaN();
    hits.reserve (kNumRoutes); prevHits.reserve (kNumRoutes);
    auto I = [this] (const juce::String& id) { jassert (paramIndex.count (id) > 0); return paramIndex.at (id); };
    for (int s = 0; s < kNumStops; ++s)
    {
        ix.stop[(size_t) s] = I (stopParamId (s)); ix.vol[(size_t) s] = I (volParamId (s)); ix.swind[(size_t) s] = I (windParamId (s));
        ix.motion[(size_t) s] = I (motionParamId (s)); ix.rate[(size_t) s] = I (rateParamId (s));
        ix.depth[(size_t) s] = I (depthParamId (s)); ix.ellipse[(size_t) s] = I (ellipseParamId (s));
        ix.ellDir[(size_t) s] = I (ellDirParamId (s)); ix.ellTilt[(size_t) s] = I (ellTiltParamId (s));
        ix.ellForm[(size_t) s] = I (ellFormParamId (s)); ix.pathSpread[(size_t) s] = I (pathSpreadParamId (s));
        ix.jumpHard[(size_t) s] = I (jumpHardParamId (s));
    }
    ix.doppler = I ("doppler"); ix.air = I ("air"); ix.distAmount = I ("distAmount"); ix.temperament = I ("temperament");
    ix.pitch = I ("pitch"); ix.detune = I ("detune"); ix.tremOn = I ("tremOn"); ix.tremRate = I ("tremRate"); ix.tremDepth = I ("tremDepth");
    ix.freeze = I ("freeze"); ix.freezeSmear = I ("freezeSmear"); ix.freezeRelease = I ("freezeRelease"); ix.motionSync = I ("motionSync");
    ix.master = I ("master"); ix.output = I ("output"); ix.click = I ("click"); ix.clickBlend = I ("clickBlend");
    ix.releaseClick = I ("releaseClick"); ix.stopPull = I ("stopPull"); ix.wind = I ("wind");
    ix.degAmount = I ("degAmount"); ix.degGrain = I ("degGrain"); ix.degBits = I ("degBits"); ix.degRate = I ("degRate");
    ix.degWobble = I ("degWobble"); ix.degDropouts = I ("degDropouts"); ix.degMode = I ("degMode");
    ix.irDry = I ("irDry"); ix.irWet = I ("irWet"); ix.binYaw = I ("binYaw"); ix.windModel = I ("windModel"); ix.mtsCont = I ("mtsContinuous");
    ix.mpe = I ("mpe"); ix.mpeBend = I ("mpeBend"); ix.lucOn = I ("lucOn"); ix.lucMix = I ("lucMix"); ix.lucMode = I ("lucMode");
    for (int i = 0; i < kNumLfos; ++i)
    {
        const juce::String id = "lfo" + juce::String (i + 1);
        ix.lfoShape[(size_t) i] = I (id + "Shape"); ix.lfoRate[(size_t) i] = I (id + "Rate"); ix.lfoSync[(size_t) i] = I (id + "Sync");
        ix.lfoDiv[(size_t) i] = I (id + "Div"); ix.lfoSpread[(size_t) i] = I (id + "Spread"); ix.lfoSpreadMode[(size_t) i] = I (id + "SpreadMode");
    }
    for (int j = 0; j < kNumEnvs; ++j)
    {
        const juce::String id = "env" + juce::String (j + 1);
        ix.envMode[(size_t) j] = I (id + "Mode"); ix.envDelay[(size_t) j] = I (id + "Delay"); ix.envAttack[(size_t) j] = I (id + "Attack");
        ix.envDecay[(size_t) j] = I (id + "Decay"); ix.envSustain[(size_t) j] = I (id + "Sustain"); ix.envRelease[(size_t) j] = I (id + "Release");
    }
    for (int r = 0; r < kNumRoutes; ++r) { ix.modAmt[(size_t) r] = I ("mod" + juce::String (r + 1) + "Amt"); routeSrc[(size_t) r] = -1; routeTgt[(size_t) r] = -1; }
    for (int k = 0; k < kNumFx; ++k) ix.fx[(size_t) k] = I (kFxIds[k]);
    for (int k = 0; k < kNumSim; ++k) ix.sim[(size_t) k] = I (kSimIds[k]);
    for (int k = 0; k < kNumInp; ++k) ix.inp[(size_t) k] = I (kInpIds[k]);
    engine.setModHook (this);

    // MIDI controller assignments: every new instance starts with the ones saved last (Documents/Rysum/MIDI-Regler.xml);
    // a saved project / session brings its own
    if (auto xml = juce::XmlDocument::parse (midiMapFile()))
        applyMidiMapState (juce::ValueTree::fromXml (*xml));
    savedMapVersion = midiMapVersion.load();

    {
        // loudspeakers: the layout used last (Documents/Rysum/Lautsprecher.json), otherwise an octagon
        rysum::SpeakerLayout l; juce::String err;
        if (! (speakerDefaultFile().existsAsFile() && rysum::speakerLayoutFromJson (speakerDefaultFile().loadFileAsString(), l, err)))
            l = rysum::speakerPresets()[4];
        setSpeakerLayout (l, false);
    }

    mts = MTS_RegisterClient();          // microtuning from an MTS-ESP master (if the MTS-ESP library is installed)

    startTimer (250);   // first tick: find + load the samples (after a possible setStateInformation)
    morphTimer.startTimerHz (40);
}

RysumProcessor::~RysumProcessor()
{
    if (mts != nullptr) { MTS_DeregisterClient (mts); mts = nullptr; }
    aliveFlag->store (false);
    stopTimer();
    morphTimer.stopTimer();
    loader.reset();
    irPool.removeAllJobs (true, 10000);
    lucPiece.cancelAll();
    piecePool.removeAllJobs (true, 10000);
    lucPool.removeAllJobs (true, 10000);
}

bool RysumProcessor::isBusesLayoutSupported (const BusesLayout& busLayout) const
{
    // inputs: off, mono or stereo
    for (const auto& in : busLayout.inputBuses)
        if (in.size() > 2) return false;
    const int n = busLayout.getMainOutputChannels();
    return n >= 1 && n <= kMaxOutChannels;
}

void RysumProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    keyboardState.reset();
    for (auto& b : irInputs) b.assign ((size_t) juce::jmax (512, samplesPerBlock), 0.0f);
    latchMidi.ensureSize (8192);
    lastOnAt.fill (std::numeric_limits<int64_t>::min() / 2);
    const bool rateChanged = std::abs (sampleRate - hostRate.load()) > 0.5;
    hostRate = sampleRate;
    if (irActive != nullptr) irActive->reset();
    binBuf.setSize (kMaxAmbiChannels, juce::jmax (512, samplesPerBlock), false, true, false);   // reserved once (see processBlock)
    spkRenderer.prepare (sampleRate, juce::jmax (512, samplesPerBlock));
    lucPiece.prepare (sampleRate);
    lucRecScratch.assign ((size_t) juce::jmax (512, samplesPerBlock), 0.0f);
    inBuf.setSize (2, juce::jmax (512, samplesPerBlock), false, true, false);
    if (binActive != nullptr) binActive->reset();
    if (rateChanged && irFile.existsAsFile())
        juce::MessageManager::callAsync ([this, alive = aliveFlag] { if (alive->load()) loadIr (irFile); });   // resample IR
}

EngineParams RysumProcessor::readParams (const float* vals) const
{
    auto V = [this, vals] (int i) { return vals != nullptr ? vals[i] : rawList[(size_t) i]->load(); };
    EngineParams p;
    for (int s = 0; s < kNumStops; ++s)
    {
        p.stopOn[(size_t) s] = V (ix.stop[(size_t) s]) > 0.5f;
        p.stopGain[(size_t) s] = dbToGain (V (ix.vol[(size_t) s]));
        p.stopWind[(size_t) s] = V (ix.swind[(size_t) s]);
        p.motionMode[(size_t) s] = (int) V (ix.motion[(size_t) s]);
        p.motionRate[(size_t) s] = V (ix.rate[(size_t) s]);
        p.motionDepth[(size_t) s] = V (ix.depth[(size_t) s]);
        p.ellipse[(size_t) s] = V (ix.ellipse[(size_t) s]);
        p.ellipseDir[(size_t) s] = V (ix.ellDir[(size_t) s]);
        p.ellipseTilt[(size_t) s] = V (ix.ellTilt[(size_t) s]);
        p.ellipseLobes[(size_t) s] = 1 + juce::roundToInt (V (ix.ellForm[(size_t) s]));
        p.pathSpread[(size_t) s] = V (ix.pathSpread[(size_t) s]);
        p.jumpHard[(size_t) s] = V (ix.jumpHard[(size_t) s]) > 0.5f;
    }
    p.doppler = V (ix.doppler);
    p.air = V (ix.air);
    p.distAmount = V (ix.distAmount);
    p.tuning = tunings[(size_t) juce::jlimit (0, (int) tunings.size() - 1, juce::roundToInt (V (ix.temperament)))];   // precomputed (UI + audio thread)
    {
        // free tuning: every MIDI note its own pitch (the table is filled on the audio thread, see processBlock)
        const int t = juce::roundToInt (V (ix.temperament));
        if ((t == kScalaTuning && scalaReady.load()) || (t == kMtsTuning && mtsActive())) p.notes = &noteTable;
        p.notesContinuous = V (ix.mtsCont) > 0.5f;
        p.mpe = V (ix.mpe) > 0.5f;
        p.mpeBendRange = V (ix.mpeBend);
        p.lucierOn = V (ix.lucOn) > 0.5f && V (ix.lucMode) > 0.5f;          // the piece is played by the processor (lucPiece)
        p.lucierMix = V (ix.lucMix);
    }
    p.pitchCents = V (ix.pitch);
    p.detune = V (ix.detune);
    p.tremOn = V (ix.tremOn) > 0.5f;
    p.tremRate = V (ix.tremRate);
    p.tremDepth = V (ix.tremDepth);
    p.freeze = V (ix.freeze) > 0.5f;
    p.freezeSmear = V (ix.freezeSmear);
    p.freezeRelease = V (ix.freezeRelease);
    p.sync = V (ix.motionSync) > 0.5f;
    p.masterGain = dbToGain (V (ix.master));
    p.outputMode = juce::roundToInt (V (ix.output));
    if (p.outputMode == kBinauralOutput) p.outputMode = 1 + juce::roundToInt (binOrderParam->load());   // processBlock uses the decoder's order
    if (p.outputMode == kSpeakerOutput) p.outputMode = spkOrderNow.load();       // processBlock uses the decoder's order
    p.clickLevel = V (ix.click);
    p.clickBlend = V (ix.clickBlend);
    p.releaseClick = V (ix.releaseClick);
    p.stopPull = V (ix.stopPull);
    p.wind = V (ix.wind);
    p.deg.amount = V (ix.degAmount);
    p.deg.grain = V (ix.degGrain);
    p.deg.bits = V (ix.degBits);
    p.deg.rateHz = V (ix.degRate);
    p.deg.wobble = V (ix.degWobble);
    p.deg.dropouts = V (ix.degDropouts);
    p.deg.perPipe = V (ix.degMode) < 0.5f;
    p.windModel = juce::roundToInt (V (ix.windModel));

    for (int i = 0; i < kNumLfos; ++i)
    {
        auto& L = p.mod.lfo[(size_t) i];
        L.shape = (int) V (ix.lfoShape[(size_t) i]);
        L.rate = V (ix.lfoRate[(size_t) i]);
        L.sync = V (ix.lfoSync[(size_t) i]) > 0.5f;
        L.div = (int) V (ix.lfoDiv[(size_t) i]);
        L.spread = V (ix.lfoSpread[(size_t) i]);
        L.spreadMode = (int) V (ix.lfoSpreadMode[(size_t) i]);
    }
    for (int j = 0; j < kNumEnvs; ++j)
    {
        auto& E = p.mod.env[(size_t) j];
        E.perPipe = V (ix.envMode[(size_t) j]) < 0.5f;
        E.delay = V (ix.envDelay[(size_t) j]);
        E.attack = V (ix.envAttack[(size_t) j]);
        E.decay = V (ix.envDecay[(size_t) j]);
        E.sustain = V (ix.envSustain[(size_t) j]);
        E.release = V (ix.envRelease[(size_t) j]);
    }
    for (int r = 0; r < kNumRoutes; ++r)
        p.mod.routes[(size_t) r] = { routeSrc[(size_t) r].load(), routeTgt[(size_t) r].load(), V (ix.modAmt[(size_t) r]), routeUni[(size_t) r].load() };

    auto X = [&] (int k) { return V (ix.fx[(size_t) k]); };
    auto& f = p.fx;
    f.custLevel = X (fxCustLevel); f.custDown = X (fxCustDown); f.custUp = X (fxCustUp); f.custPlace = juce::roundToInt (X (fxCustPlace));
    f.custPitch = X (fxCustPitch); f.custScatter = X (fxCustScatter); f.custWander = X (fxCustWander);
    f.pathLevel = X (fxPathLevel); f.pathTime = X (fxPathTime); f.pathTicks = juce::roundToInt (X (fxPathTicks));
    f.pathSpread = X (fxPathSpread); f.pathWait = X (fxPathWait) > 0.5f;
    f.crackLevel = X (fxCrackLevel); f.crackRate = X (fxCrackRate); f.crackLen = X (fxCrackLen);
    f.leakLevel = X (fxLeakLevel); f.leakCount = juce::roundToInt (X (fxLeakCount)); f.leakFreq = X (fxLeakFreq);
    f.leakWander = X (fxLeakWander); f.windRound = X (fxWindRound) > 0.5f;
    f.partSpread = X (fxPartSpread); f.partSplit = X (fxPartSplit); f.partTurn = X (fxPartTurn);
    f.echoLevel = X (fxEchoLevel); f.echoTime = X (fxEchoTime); f.echoSync = X (fxEchoSync) > 0.5f;
    f.echoDiv = juce::roundToInt (X (fxEchoDiv)); f.echoRepeats = juce::roundToInt (X (fxEchoRepeats));
    f.echoFeedback = X (fxEchoFeedback); f.echoTurn = X (fxEchoTurn); f.echoTilt = X (fxEchoTilt); f.echoDamp = X (fxEchoDamp);

    {
        auto In = [&] (int k) { return V (ix.inp[(size_t) k]); };
        auto& ip = p.input;
        ip.on = In (inpOn) > 0.5f;
        ip.level = In (inpLevel) <= -59.9f ? 0.0f : dbToGain (In (inpLevel));
        ip.stereo = In (inpMode) > 0.5f;
        ip.az = In (inpAz); ip.el = In (inpEl); ip.dist = In (inpDist); ip.width = In (inpWidth); ip.spin = In (inpSpin);
        ip.follow = juce::roundToInt (In (inpFollow)) - 1;
        ip.gate = In (inpGate) > 0.5f;
        ip.release = In (inpRelease);
        ip.send = In (inpSend) <= -59.9f ? 0.0f : dbToGain (In (inpSend));
    }

    auto S = [&] (int k) { return V (ix.sim[(size_t) k]); };
    auto& r = p.room;
    r.on = S (simOn) > 0.5f;
    r.width = S (simW); r.length = S (simL); r.height = S (simH); r.size = S (simSize);
    r.listenerX = S (simLx); r.listenerY = S (simLy);
    r.absWalls = S (simAbs); r.absFloor = S (simAbsFloor); r.highDamp = S (simHigh); r.scatter = S (simScatter);
    r.order = 1 + juce::roundToInt (S (simOrder));
    r.erGain = S (simEr) <= -59.9f ? 0.0f : dbToGain (S (simEr));
    r.tailGain = S (simTail) <= -59.9f ? 0.0f : dbToGain (S (simTail));
    r.tailModel = juce::roundToInt (S (simTailModel));
    return p;
}

// ---------------------------------------------------------------------------
// modulation of plugin parameters (audio thread, every 64 samples)
// ---------------------------------------------------------------------------
void RysumProcessor::modulate (EngineParams& p, const float* src)
{
    const int N = (int) rawList.size();
    for (int i = 0; i < N; ++i) modVals[(size_t) i] = rawList[(size_t) i]->load();
    hits.clear();
    for (int r = 0; r < kNumRoutes; ++r)
    {
        const int sIdx = routeSrc[(size_t) r].load(), t = routeTgt[(size_t) r].load() - kGlobalTargetBase;
        if (sIdx < 0 || sIdx >= kNumSources || t < 0 || t >= N || ! isFloatParam[(size_t) t]) continue;
        const float a = modVals[(size_t) ix.modAmt[(size_t) r]];
        if (a == 0.0f) continue;
        if (! modHitFlag[(size_t) t]) { modHitFlag[(size_t) t] = 1; modOff[(size_t) t] = 0.0f; hits.push_back (t); }
        modOff[(size_t) t] += a * shapeSource (sIdx, src[sIdx], routeUni[(size_t) r].load());
    }
    for (int t : prevHits) modNorm[t] = std::numeric_limits<float>::quiet_NaN();
    for (int t : hits)
    {
        auto* prm = prmList[(size_t) t];
        const float n = juce::jlimit (0.0f, 1.0f, prm->convertTo0to1 (modVals[(size_t) t]) + modOff[(size_t) t]);
        modVals[(size_t) t] = prm->convertFrom0to1 (n);
        modNorm[t] = n;
        modHitFlag[(size_t) t] = 0;
    }
    std::swap (hits, prevHits);
    haveModVals = true;
    if (prevHits.empty()) return;            // nothing global is modulated: p already holds these values

    EngineParams q = readParams (modVals.data());
    applyCcOverrides (q);
    q.bpm = p.bpm; q.ppq = p.ppq; q.playing = p.playing;
    q.roomHarmonics = p.roomHarmonics; q.roomFollow = p.roomFollow; q.roomBaseAz = p.roomBaseAz;
    q.outputMode = p.outputMode;
    p = q;
}

int RysumProcessor::getParamIndex (const juce::String& id) const
{
    auto it = paramIndex.find (id);
    return it == paramIndex.end() ? -1 : it->second;
}

float RysumProcessor::getModulatedNorm (int index) const
{
    return index >= 0 && index < (int) prmList.size() ? modNorm[index].load() : std::numeric_limits<float>::quiet_NaN();
}

juce::String RysumProcessor::targetId (int target) const
{
    if (target < 0) return {};
    if (target < kNumPipeTargets) return pipeTargetId (target);
    const int i = target - kGlobalTargetBase;
    return i >= 0 && i < (int) prmList.size() ? prmList[(size_t) i]->getParameterID() : juce::String();
}

int RysumProcessor::targetFromId (const juce::String& id) const
{
    if (id.startsWith ("pipe:")) { const int t = id.fromFirstOccurrenceOf (":", false, false).getIntValue(); return t >= 0 && t < kNumPipeTargets ? t : -1; }
    const int i = getParamIndex (id);
    return i >= 0 && isFloatParam[(size_t) i] ? kGlobalTargetBase + i : -1;
}

void RysumProcessor::setRoute (int r, int src, int target)
{
    if (r < 0 || r >= kNumRoutes) return;
    routeSrc[(size_t) r] = src;
    routeTgt[(size_t) r] = target;
    ++routeVersion;
}

int RysumProcessor::addRoute (int src, int target, float amount)
{
    for (int r = 0; r < kNumRoutes; ++r)
        if (routeSrc[(size_t) r].load() < 0 && routeTgt[(size_t) r].load() < 0)
        {
            setRoute (r, src, target);
            if (auto* a = prmList[(size_t) ix.modAmt[(size_t) r]])
            {
                a->beginChangeGesture(); a->setValueNotifyingHost (a->convertTo0to1 (amount)); a->endChangeGesture();
            }
            return r;
        }
    return -1;
}

void RysumProcessor::setRouteUni (int r, bool uni)
{
    if (r < 0 || r >= kNumRoutes) return;
    routeUni[(size_t) r] = uni;
    ++routeVersion;
}

void RysumProcessor::clearRoute (int r)
{
    if (r < 0 || r >= kNumRoutes) return;
    routeUni[(size_t) r] = false;
    setRoute (r, -1, -1);
    if (auto* a = prmList[(size_t) ix.modAmt[(size_t) r]])
    {
        a->beginChangeGesture(); a->setValueNotifyingHost (a->convertTo0to1 (0.0f)); a->endChangeGesture();
    }
}

void RysumProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    if (auto pending = std::atomic_exchange (&pendingLibrary, std::shared_ptr<const SampleLibrary>()))
        engine.setLibrary (std::move (pending));      // the old one is still referenced by retiredLibrary

    keyboardState.processNextMidiBuffer (midi, 0, buffer.getNumSamples(), true);
    applyLatch (midi, buffer.getNumSamples());

    // MIDI controllers -> parameters: own assignments (MIDI learn) first, then the fixed map
    // (the fixed map is applied right away, the parameters follow on the message thread)
    {
        const bool fixedMap = extraParams.at ("midiCC")->load() > 0.5f;
        bool any = false;
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            if (! m.isController()) continue;
            const int cc = m.getControllerNumber(), v = m.getControllerValue();
            if (handleLearnedCc (cc, m.getChannel(), v)) { any = true; continue; }
            if (! fixedMap) continue;
            int target = -1; float value = 0.0f;
            const int ns = kNumStops;
            if (cc == 11)                  { target = 0; value = v / 127.0f; }                            // expression -> global wind
            else if (cc >= 20 && cc <= 26) { target = 1 + (cc - 20); value = v >= 64 ? 1.0f : 0.0f; }      // stops on / off
            else if (cc == 19)             { target = 1 + kPraestant16; value = v >= 64 ? 1.0f : 0.0f; }   // Praestant 16'
            else if (cc >= 27 && cc <= 33) { target = 1 + ns + (cc - 27); value = v / 127.0f; }           // stop wind
            else if (cc == 34)             { target = 1 + ns + kPraestant16; value = v / 127.0f; }        // Praestant 16' wind
            else if (cc == 66)             { target = 1 + 2 * ns; value = v >= 64 ? 1.0f : 0.0f; }        // sostenuto -> freeze
            else if (cc == 1)              { target = 2 + 2 * ns; value = v / 127.0f; }                   // mod wheel -> tremulant depth
            if (target >= 0) { ccPending[(size_t) target] = value; any = true; }
        }
        if (any) triggerAsyncUpdate();
    }

    // free tuning: MIDI Tuning Standard SysEx (e.g. from Hexatone) goes to the MTS-ESP client (used when no master
    // is connected); then the note table the engine plays from
    if (mts != nullptr)
        for (const auto meta : midi)
            if (meta.getMessage().isSysEx())
                MTS_ParseMIDIDataU (mts, meta.data, meta.numBytes);
    {
        const int t = juce::roundToInt (extraParams.at ("temperament")->load());
        if (t == kMtsTuning && mts != nullptr)
        {
            for (int ch = 0; ch < 16; ++ch)
                for (int n = 0; n < 128; ++n)
                {
                    const size_t i = (size_t) (ch * 128 + n);
                    noteTable.hz[i] = (float) MTS_NoteToFrequency (mts, (char) n, (signed char) ch);
                    noteTable.mapped[i] = MTS_ShouldFilterNote (mts, (char) n, (signed char) ch) ? 0 : 1;
                }
        }
        else if (t == kScalaTuning && scalaVersion.load() != scalaVersionSeen)
        {
            const juce::SpinLock::ScopedTryLockType tl (scalaLock);          // (the message thread writes it rarely)
            if (tl.isLocked()) { noteTable = scalaShared; scalaVersionSeen = scalaVersion.load(); }
        }
    }

    auto params = readParams();
    applyCcOverrides (params);

    // host tempo / position for tempo-synced movement
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm()) params.bpm = *bpm;
            if (auto ppq = pos->getPpqPosition()) params.ppq = *ppq;
            params.playing = pos->getIsPlaying() && pos->getPpqPosition().hasValue();
        }

    // room: take a new impulse response before the engine runs (the sends depend on it)
    if (irClearRequest.exchange (false)) irActive.reset();          // still referenced by retiredIr
    if (auto t = std::atomic_exchange (&pendingTail, std::shared_ptr<const rysum::TailIR>()))
        engine.setTailIR (std::move (t));                         // still referenced by tailKeepAlive
    if (auto t = std::atomic_exchange (&pendingLucier, std::shared_ptr<const rysum::TailIR>()))
        engine.setLucierIR (std::move (t));                       // still referenced by lucKeepAlive
    if (auto pending = std::atomic_exchange (&pendingIr, std::shared_ptr<rysum::Convolver>()))
        irActive = std::move (pending);
    if (irActive != nullptr && ! params.room.on)
    {
        params.roomHarmonics = irActive->getInfo().harmonics;
        params.roomFollow = irFollowParam->load() > 0.5f;
        params.roomBaseAz = irActive->getInfo().baseAzDeg;
    }

    // binaural: the engine renders Ambisonics into an own buffer (the host usually has only 2 channels),
    // the decoder turns it into left / right at the end. Until the first decoder is ready: stereo.
    if (auto pending = std::atomic_exchange (&pendingBin, std::shared_ptr<rysum::BinauralDecoder>()))
        binActive = std::move (pending);                 // the old one is still referenced by retiredBin
    const int n = buffer.getNumSamples();
    const bool binaural = juce::roundToInt (output->load()) == kBinauralOutput && binActive != nullptr && buffer.getNumChannels() > 0;
    if (juce::roundToInt (output->load()) == kBinauralOutput)
        params.outputMode = binaural ? binActive->getOrder() : 0;
    // loudspeakers: the same Ambisonics buffer, decoded with AllRAD to the layout's channels
    if (auto pending = std::atomic_exchange (&pendingSpk, std::shared_ptr<const rysum::AllRadDecoder>()))
        spkRenderer.setDecoder (std::move (pending));                 // still referenced by spkKeepAlive
    const bool speakers = juce::roundToInt (output->load()) == kSpeakerOutput && spkRenderer.next() != nullptr
                          && spkRenderer.next()->error.isEmpty() && buffer.getNumChannels() > 0;
    if (juce::roundToInt (output->load()) == kSpeakerOutput)
        params.outputMode = speakers ? spkRenderer.next()->order : 0;
    // (setSize with avoidReallocating: no allocation as long as the block fits what prepareToPlay reserved)
    if (binaural) binBuf.setSize (binActive->getNumChannels(), n, false, false, true);
    if (speakers) binBuf.setSize ((params.outputMode + 1) * (params.outputMode + 1), n, false, false, true);
    juce::AudioBuffer<float>& work = binaural || speakers ? binBuf : buffer;

    // the audio input (main input + side chain, summed) is copied first: the engine overwrites the buffer
    {
        bool any = false;
        if (inBuf.getNumSamples() < n) inBuf.setSize (2, n, false, false, true);    // (only if the host sends a longer block than announced)
        for (int b = 0; b < getBusCount (true); ++b)
        {
            const auto* bus = getBus (true, b);
            if (bus == nullptr || ! bus->isEnabled()) continue;
            const int nc = bus->getNumberOfChannels();
            for (int c = 0; c < juce::jmin (2, nc); ++c)
            {
                const int idx = bus->getChannelIndexInProcessBlockBuffer (c);
                if (idx < 0 || idx >= buffer.getNumChannels()) continue;
                for (int dst = (nc == 1 ? 0 : c); dst <= (nc == 1 ? 1 : c); ++dst)
                {
                    if (! any) inBuf.clear (0, n);
                    any = true;
                    juce::FloatVectorOperations::add (inBuf.getWritePointer (dst), buffer.getReadPointer (idx), n);
                }
            }
        }
        inputThisBlock = any;
        if (! any) engine.setInput (nullptr, nullptr, 0);
        else engine.setInput (inBuf.getReadPointer (0), inBuf.getReadPointer (1), n);
    }

    engine.process (work, midi, params);
    engine.setInput (nullptr, nullptr, 0);
    midi.clear();

    // ---- room: dry level + convolution with the loaded impulse response

    const bool stereo = params.outputMode == 0;
    const int used = juce::jmin (work.getNumChannels(), stereo ? 2 : (params.outputMode + 1) * (params.outputMode + 1));
    const float dryDb = haveModVals ? modVals[(size_t) ix.irDry] : irDry->load();
    const float wetDb = haveModVals ? modVals[(size_t) ix.irWet] : irWet->load();
    const float dryTarget = dryDb <= -59.9f ? 0.0f : dbToGain (dryDb);
    const float wetTarget = wetDb <= -59.9f ? 0.0f : dbToGain (wetDb);

    if (irActive != nullptr && used > 0 && ! params.room.on)       // a simulated room replaces the impulse response
    {
        // sends to the room (sum + direction harmonics, far pipes relatively louder); wet level ramped here
        const int numIn = irActive->getInfo().numInputs;
        const int chunk = (int) irInputs[0].size();
        for (int start = 0; start < n; start += chunk)
        {
            const int m = juce::jmin (chunk, n - start);
            const float* ins[rysum::OrganEngine::kMaxRoomSends] {};
            for (int k = 0; k < numIn; ++k)
            {
                const float* a = engine.getRoomSend (k) + start;
                float* dst = irInputs[(size_t) k].data();
                for (int i = 0; i < m; ++i)
                    dst[i] = a[i] * (wetSmoothed + (wetTarget - wetSmoothed) * (float) (start + i) / (float) n);
                ins[k] = dst;
            }
            // apply the dry level to this chunk before the wet signal is added
            const float d0 = drySmoothed + (dryTarget - drySmoothed) * (float) start / (float) n;
            const float d1 = drySmoothed + (dryTarget - drySmoothed) * (float) (start + m) / (float) n;
            for (int c = 0; c < used; ++c) work.applyGainRamp (c, start, m, d0, d1);
            float* outs[rysum::kMaxOutChannels] {};
            for (int c = 0; c < used; ++c) outs[c] = work.getWritePointer (c, start);
            irActive->process (ins, m, outs, used, 1.0f);
        }
    }
    else if (std::abs (dryTarget - 1.0f) > 1.0e-6f || std::abs (drySmoothed - 1.0f) > 1.0e-6f)
    {
        for (int c = 0; c < used; ++c) work.applyGainRamp (c, 0, n, drySmoothed, dryTarget);
    }
    drySmoothed = dryTarget;
    wetSmoothed = wetTarget;

    // ---- Lucier as a piece: recording, and the generations played back (mono: W / the middle of L+R)
    if (used > 0 && n <= (int) lucRecScratch.size())
    {
        const bool fromInput = juce::roundToInt (apvts.getRawParameterValue ("lucRecSrc")->load()) == 1;
        float* rec = lucRecScratch.data();
        if (fromInput)
        {
            if (inputThisBlock) for (int i = 0; i < n; ++i) rec[i] = 0.5f * (inBuf.getSample (0, i) + inBuf.getSample (1, i));
            else std::fill_n (rec, n, 0.0f);
        }
        else if (stereo && used > 1) for (int i = 0; i < n; ++i) rec[i] = 0.5f * (work.getSample (0, i) + work.getSample (1, i));
        else std::copy_n (work.getReadPointer (0), n, rec);
        float gains[rysum::kMaxOutChannels] {};
        if (stereo) { gains[0] = used > 1 ? 0.7071f : 1.0f; if (used > 1) gains[1] = 0.7071f; }
        else gains[0] = 1.0f;
        lucPiece.process (rec, work.getArrayOfWritePointers(), gains, used, n, apvts.getRawParameterValue ("lucMix")->load());
    }

    // ---- binaural decoding (head rotation + HRTF filters)
    if (binaural)
    {
        const float yaw = haveModVals ? modVals[(size_t) ix.binYaw] : binYawParam->load();
        float* l = buffer.getWritePointer (0);
        if (buffer.getNumChannels() > 1)
            binActive->process (binBuf.getArrayOfWritePointers(), n, l, buffer.getWritePointer (1), yaw);
        else
        {
            // mono host: decode in place pairwise (rare); the right ear goes through the last Ambisonics channel's
            // already consumed samples, see BinauralDecoder::process (reads a sample before writing it)
            float* r = binBuf.getWritePointer (binBuf.getNumChannels() - 1);
            binActive->process (binBuf.getArrayOfWritePointers(), n, l, r, yaw);
            for (int i = 0; i < n; ++i) l[i] = 0.5f * (l[i] + r[i]);
        }
        for (int c = 2; c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);
    }

    // ---- loudspeakers (AllRAD)
    if (speakers)
        spkRenderer.process (binBuf.getArrayOfReadPointers(), binBuf.getNumChannels(), buffer.getArrayOfWritePointers(), buffer.getNumChannels(), n);

    // ---- test noise on one loudspeaker (setting up a layout); straight to its channel, about -20 dBFS, pink-ish
    {
        const int t = speakerTest.load();
        const auto* d = speakers ? spkRenderer.current() : nullptr;
        if (t >= 0 && d != nullptr && t < (int) d->all.size())
        {
            const int ch = d->all[(size_t) t].channel - 1;
            if (! d->all[(size_t) t].imaginary && ch >= 0 && ch < buffer.getNumChannels())
            {
                float* o = buffer.getWritePointer (ch);
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
    return getPatchFolder().getParentDirectory().getChildFile ("Lautsprecher.json");
}

void RysumProcessor::setSpeakerLayout (const rysum::SpeakerLayout& l, bool rememberAsDefault)
{
    spkLayout = l;
    if (rememberAsDefault)
    {
        speakerDefaultFile().getParentDirectory().createDirectory();
        speakerDefaultFile().replaceWithText (rysum::speakerLayoutToJson (l));
    }
    rebuildSpeakers (true);
}

void RysumProcessor::rebuildSpeakers (bool force)
{
    const int order = 1 + juce::roundToInt (apvts.getRawParameterValue ("spkOrder")->load());
    const bool comp = apvts.getRawParameterValue ("spkDistComp")->load() > 0.5f;
    const juce::String key = juce::String (order) + (comp ? "c" : "-");
    if (! force && key == spkBuiltKey) return;
    spkBuiltKey = key;
    auto d = rysum::buildAllRad (spkLayout, order, comp, ++spkSerial);       // a few ms: on the message thread
    spkUi = d;
    spkOrderNow = d->error.isEmpty() ? d->order : 0;
    spkKeepAlive.push_back (d);
    std::atomic_store (&pendingSpk, std::shared_ptr<const rysum::AllRadDecoder> (d));
    ++spkVersion;
    ++changeCounter;
}

juce::ValueTree RysumProcessor::speakersState() const
{
    juce::ValueTree t ("SPEAKERS");
    t.setProperty ("json", rysum::speakerLayoutToJson (spkLayout), nullptr);
    return t;
}

// ---------------------------------------------------------------------------
// impulse response
// ---------------------------------------------------------------------------
void RysumProcessor::applyCcOverrides (EngineParams& p) const
{
    auto get = [this] (int i) { return ccPending[(size_t) i].load(); };
    float v;
    if (! std::isnan (v = get (0))) p.wind = v;
    for (int s = 0; s < kNumStops; ++s)
    {
        if (! std::isnan (v = get (1 + s))) p.stopOn[(size_t) s] = v > 0.5f;
        if (! std::isnan (v = get (1 + kNumStops + s))) p.stopWind[(size_t) s] = v;
    }
    if (! std::isnan (v = get (1 + 2 * kNumStops))) p.freeze = v > 0.5f;
    if (! std::isnan (v = get (2 + 2 * kNumStops))) p.tremDepth = v;
}

bool RysumProcessor::handleLearnedCc (int cc, int channel, int value)
{
    if (cc >= 120) return false;                                       // channel mode messages
    lastCc = cc | (channel << 8) | (value << 16);
    int learning = mmLearn.load();
    if (learning >= 0 && learning < kNumMidiMaps && mmLearn.compare_exchange_strong (learning, -1))
    {
        mmCc[(size_t) learning] = cc;
        mmChan[(size_t) learning] = channel;
        mmPressed[(size_t) learning] = value >= 64;
        ++midiMapVersion;
    }
    bool mapped = false;
    for (int i = 0; i < kNumMidiMaps; ++i)
    {
        const int pi = mmParam[(size_t) i].load();
        if (mmCc[(size_t) i].load() != cc || pi < 0) continue;
        const int ch = mmChan[(size_t) i].load();
        if (ch != 0 && ch != channel) continue;
        mapped = true;
        const float lo = mmLo[(size_t) i].load(), hi = mmHi[(size_t) i].load();
        if (mmToggle[(size_t) i].load())
        {
            // a button: each press switches to the other end of the range (from where the parameter is now)
            const bool down = value >= 64, wasDown = mmPressed[(size_t) i];
            mmPressed[(size_t) i] = down;
            if (! down || wasDown) continue;
            const float cur = prmList[(size_t) pi]->convertTo0to1 (rawList[(size_t) pi]->load());
            const bool atHi = std::abs (cur - hi) < std::abs (cur - lo);
            mmPending[(size_t) i] = atHi ? lo : hi;
        }
        else mmPending[(size_t) i] = juce::jlimit (0.0f, 1.0f, lo + (hi - lo) * (float) value / 127.0f);
    }
    return mapped;
}

void RysumProcessor::handleAsyncUpdate()
{
    for (int i = 0; i < kNumMidiMaps; ++i)
    {
        const float n = mmPending[(size_t) i].exchange (std::numeric_limits<float>::quiet_NaN());
        if (std::isnan (n)) continue;
        if (auto* prm = getParamAt (mmParam[(size_t) i].load()))
            if (std::abs (prm->getValue() - n) > 1.0e-6f)
            {
                prm->beginChangeGesture();
                prm->setValueNotifyingHost (n);
                prm->endChangeGesture();
            }
    }
    for (size_t i = 0; i < ccTargets.size(); ++i)
    {
        const float v = ccPending[i].exchange (std::numeric_limits<float>::quiet_NaN());
        if (std::isnan (v) || ccTargets[i] == nullptr) continue;
        ccTargets[i]->beginChangeGesture();
        ccTargets[i]->setValueNotifyingHost (ccTargets[i]->convertTo0to1 (v));
        ccTargets[i]->endChangeGesture();
    }
}

void RysumProcessor::loadIr (const juce::File& file)
{
    irFile = file;
    const auto settings = currentIrSettings();
    builtIrKey = irSettingsKey (settings);
    setIrStatus (u8 ("lade ") + file.getFileName() + u8 (" \xe2\x80\xa6"));
    irPool.addJob ([this, file, settings, alive = aliveFlag]
    {
        juce::String err;
        std::shared_ptr<rysum::Convolver> cv (rysum::Convolver::create (file, settings, err).release());
        juce::MessageManager::callAsync ([this, cv, err, file, alive]
        {
            if (! alive->load() || file != irFile) return;
            if (cv == nullptr) { setIrStatus (err); return; }
            retiredIr = irUi;
            retireCountdown = 20;
            irUi = cv;
            std::atomic_store (&pendingIr, cv);
            const auto& in = cv->getInfo();
            setIrStatus (in.fileName + u8 (" \xc2\xb7 ") + in.description);
        });
    });
}

// ---------------------------------------------------------------------------
// "Halten": strike a note twice quickly -> it stays down until it is struck again
// ---------------------------------------------------------------------------
void RysumProcessor::applyLatch (juce::MidiBuffer& midi, int numSamples)
{
    const bool on = latchOn.load();
    const bool letGo = releaseLatched.exchange (false) || (latchWasOn && ! on);
    latchWasOn = on;
    if (! on && ! letGo) { sampleClock += numSamples; return; }

    latchMidi.clear();
    if (letGo)
        for (int n = 0; n < 128; ++n)
            if (latched[(size_t) n]) { latched[(size_t) n] = false; latchMidi.addEvent (juce::MidiMessage::noteOff (1, n), 0); }
    const int64_t window = (int64_t) (0.4 * (getSampleRate() > 0 ? getSampleRate() : 48000.0));
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (on && m.isNoteOn())
        {
            const int n = m.getNoteNumber();
            const int64_t t = sampleClock + meta.samplePosition;
            if (latched[(size_t) n])
            {
                // struck again: no new attack, the note ends when this key comes up
                latched[(size_t) n] = false;
                lastOnAt[(size_t) n] = std::numeric_limits<int64_t>::min() / 2;
                continue;
            }
            if (t - lastOnAt[(size_t) n] < window) latched[(size_t) n] = true;
            lastOnAt[(size_t) n] = t;
        }
        else if (on && m.isNoteOff() && latched[(size_t) m.getNoteNumber()])
            continue;                                   // held: the key coming up is ignored
        latchMidi.addEvent (m, meta.samplePosition);
    }
    midi.swapWith (latchMidi);
    int count = 0;
    for (auto b : latched) count += b ? 1 : 0;
    latchedCount = count;
    sampleClock += numSamples;
}

// ---------------------------------------------------------------------------
// simulated room: ray tracing (message thread starts it, the pool thread runs it)
// ---------------------------------------------------------------------------
rysum::RoomParams RysumProcessor::heardRoom() const
{
    rysum::RoomParams r;
    r.on = engine.roomOn.load();
    r.width = engine.roomW.load(); r.length = engine.roomL.load(); r.height = engine.roomH.load(); r.size = 1.0f;
    r.absWalls = engine.roomAbsW.load(); r.absFloor = engine.roomAbsF.load(); r.highDamp = engine.roomHighD.load(); r.scatter = engine.roomScat.load();
    return r;
}

juce::String RysumProcessor::roomKey() const
{
    // rounded so that tiny modulation steps do not rebuild all the time (1.5 % in size, 0.004 in absorption)
    const auto r = heardRoom();
    auto q = [] (float v, float step) { return juce::String (juce::roundToInt (v / step)); };
    auto ql = [] (float v) { return juce::String (juce::roundToInt (std::log (juce::jmax (0.1f, v)) / 0.015f)); };
    return ql (r.W()) + "|" + ql (r.L()) + "|" + ql (r.H()) + "|" + q (r.absWalls, 0.004f) + "|" + q (r.absFloor, 0.004f) + "|"
         + q (r.highDamp, 0.01f) + "|" + q (r.scatter, 0.02f) + "|" + q (engine.roomAir.load(), 0.02f) + "|"
         + juce::String (juce::jmin (2, engine.roomOutOrder.load())) + "|" + juce::String (hostRate.load(), 0);
}

void RysumProcessor::buildTailNow (const juce::String& key)
{
    builtTailKey = key;
    tailBuilding = true;
    const auto r = heardRoom();
    const float air = engine.roomAir.load();
    const int ord = engine.roomOutOrder.load();
    const double rate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
    const uint32_t serial = ++tailSerial;
    irPool.addJob ([this, r, air, ord, rate, serial, alive = aliveFlag]
    {
        auto ir = rysum::TailIR::build (r, air, ord, rate, serial);
        juce::MessageManager::callAsync ([this, ir, alive]
        {
            if (! alive->load()) return;
            tailBuilding = false;
            tailKeepAlive.push_back (ir);                 // the engine's copies are never the last ones
            std::atomic_store (&pendingTail, ir);
            setRoomStatus ("Nachhall: " + juce::String (ir->rays) + juce::String::fromUTF8 (" Strahlen \xc2\xb7 T30 ")
                           + juce::String (ir->rtLow, 2) + " s tief / " + juce::String (ir->rtHigh, 2) + juce::String::fromUTF8 (" s hoch \xc2\xb7 Antwort ")
                           + juce::String ((double) ir->length / ir->sampleRate, 1) + " s, " + (ir->order == 0 ? juce::String ("Stereo") : juce::String (ir->order) + ". Ordnung")
                           + ", " + juce::String (ir->channels) + " dekorrelierte Kan\xc3\xa4le");
        });
    });
}

// ---------------------------------------------------------------------------
// Lucier mode: the response of n passes through the room, rebuilt in the background whenever the
// generation (growing by itself, or modulated), the room or the damping changes
// ---------------------------------------------------------------------------
void RysumProcessor::lucierRecord()
{
    if (auto* p = apvts.getParameter ("lucMode")) p->setValueNotifyingHost (0.0f);
    if (auto* p = apvts.getParameter ("lucOn")) p->setValueNotifyingHost (1.0f);
    lucPiece.startRecording (300.0);
}

void RysumProcessor::lucierStopRecording()
{
    lucPiece.stopRecording (true);
    saveLucierRecording();
}

void RysumProcessor::saveLucierRecording()
{
    // every new recording is kept as a file (Documents/Rysum/Lucier), so the project can play it again later
    const auto& r = lucPiece.getRecording();
    if (lucPiece.recordingVersion == savedRecordingVersion) return;
    savedRecordingVersion = lucPiece.recordingVersion;
    if (r.empty() || savedFromFile) { savedFromFile = false; return; }
    auto dir = getPatchFolder().getParentDirectory().getChildFile ("Lucier");
    dir.createDirectory();
    auto f = dir.getChildFile ("Aufnahme " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + ".wav");
    juce::WavAudioFormat wav;
    if (auto* os = new juce::FileOutputStream (f); os->openedOk())
    {
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os, lucPiece.recordingRate(), 1, 24, {}, 0));
        if (w != nullptr)
        {
            const float* ch[] { r.data() };
            w->writeFromFloatArrays (ch, 1, (int) r.size());
            lucPiece.recordingFile = f.getFullPathName();
        }
    }
    else delete os;
}

bool RysumProcessor::loadLucierRecording (const juce::File& f, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
    if (rd == nullptr) { error = juce::String::fromUTF8 ("keine lesbare Audiodatei"); return false; }
    if (rd->lengthInSamples > (juce::int64) (rd->sampleRate * 1200.0)) { error = juce::String::fromUTF8 ("l\xc3\xa4nger als 20 Minuten"); return false; }
    juce::AudioBuffer<float> b ((int) rd->numChannels, (int) rd->lengthInSamples);
    rd->read (&b, 0, b.getNumSamples(), 0, true, true);
    std::vector<float> mono ((size_t) b.getNumSamples(), 0.0f);
    for (int c = 0; c < b.getNumChannels(); ++c)
        juce::FloatVectorOperations::addWithMultiply (mono.data(), b.getReadPointer (c), 1.0f / (float) b.getNumChannels(), b.getNumSamples());
    const double rate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
    if (std::abs (rd->sampleRate - rate) > 0.5)
    {
        // to the host's rate
        std::vector<float> out ((size_t) ((double) mono.size() * rate / rd->sampleRate), 0.0f);
        juce::LagrangeInterpolator li;
        li.process (rd->sampleRate / rate, mono.data(), out.data(), (int) out.size());
        mono = std::move (out);
    }
    lucPiece.stop();
    lucPiece.setRecording (std::move (mono), rate);
    lucPiece.recordingFile = f.getFullPathName();
    savedFromFile = true;
    return true;
}

void RysumProcessor::exportLucierPiece (const juce::File& f)
{
    lucPiece.exportPiece (f, lucBase, juce::roundToInt (apvts.getRawParameterValue ("lucGen")->load()), apvts.getRawParameterValue ("lucGap")->load(), piecePool);
}

void RysumProcessor::updateLucier (double dt)
{
    auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    const bool on = raw ("lucOn") > 0.5f;
    // the generation as modulated right now, plus what it has grown by
    float gen = raw ("lucGen");
    {
        const int idx = getParamIndex ("lucGen");
        const float mn = getModulatedNorm (idx);
        if (! std::isnan (mn)) gen = prmList[(size_t) idx]->convertFrom0to1 (mn);
    }
    const bool piece = raw ("lucMode") < 0.5f;
    if (piece) { gen = (float) juce::jmax (1, lucPiece.playingGen.load()); lucGrown = 0.0f; }     // the display follows the piece
    else if (on) lucGrown += (float) (raw ("lucGrow") / 60.0 * dt);
    lucEffective = juce::jlimit (1.0f, 40.0f, gen + lucGrown);
    if (gen + lucGrown > 40.0f) lucGrown = juce::jmax (0.0f, 40.0f - gen);
    if (! on || lucBuilding) return;

    const int source = juce::roundToInt (raw ("lucSource"));
    const double rate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
    rysum::RoomParams r;
    r.width = raw ("simW"); r.length = raw ("simL"); r.height = raw ("simH"); r.size = raw ("simSize");
    r.listenerX = raw ("simLx"); r.listenerY = raw ("simLy");
    r.absWalls = raw ("simAbs"); r.absFloor = raw ("simAbsFloor"); r.highDamp = raw ("simHigh"); r.scatter = raw ("simScatter");
    const float air = raw ("air");
    auto q = [] (float v, float step) { return juce::String (juce::roundToInt (v / step)); };
    const juce::String baseKey = source == 1 ? "ir|" + irFile.getFullPathName() + "|" + juce::String (rate, 0)
                                             : "saal|" + q (r.W(), 0.05f) + "|" + q (r.L(), 0.05f) + "|" + q (r.H(), 0.05f) + "|" + q (r.listenerX, 0.01f) + "|"
                                               + q (r.listenerY, 0.01f) + "|" + q (r.absWalls, 0.004f) + "|" + q (r.absFloor, 0.004f) + "|" + q (r.highDamp, 0.01f)
                                               + "|" + q (air, 0.02f) + "|" + juce::String (rate, 0);
    const float tau = raw ("lucTime");
    const juce::String key = baseKey + "#" + q (lucEffective, 0.02f) + "|" + q (tau, 0.005f);
    if (key == builtLucKey) return;
    if (source == 1 && ! irFile.existsAsFile())
    {
        if (lucView.status != "keine Impulsantwort geladen (Seite Raum)") { lucView.status = "keine Impulsantwort geladen (Seite Raum)"; ++lucView.version; }
        return;
    }
    builtLucKey = key;
    lucBuilding = true;
    const uint32_t serial = ++lucSerial;
    auto base = baseKey == lucBaseKey ? lucBase : nullptr;
    const float g = lucEffective;
    const juce::File file = irFile;
    lucPool.addJob ([this, base, baseKey, source, r, air, rate, file, g, tau, serial, alive = aliveFlag]
    {
        auto b = base;
        juce::String err;
        if (b == nullptr) b = source == 1 ? rysum::lucierFromFile (file, rate, err) : rysum::lucierFromRoom (r, air, rate);
        std::shared_ptr<rysum::LucierResult> res;
        if (b != nullptr) res = std::make_shared<rysum::LucierResult> (rysum::buildLucier (*b, g, tau, serial));
        juce::MessageManager::callAsync ([this, b, baseKey, res, err, alive]
        {
            if (! alive->load()) return;
            lucBuilding = false;
            if (b == nullptr || res == nullptr) { lucView.status = err; builtLucKey.clear(); ++lucView.version; return; }
            lucBase = b; lucBaseKey = baseKey;
            lucKeepAlive.push_back (res->ir);
            std::atomic_store (&pendingLucier, res->ir);
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
    return sofaFile.getFullPathName() + "|" + juce::String (1 + juce::roundToInt (binOrderParam->load())) + "|"
         + juce::String ((int) (binDfParam->load() > 0.5f)) + "|" + juce::String (hostRate.load() > 0 ? hostRate.load() : 48000.0, 0);
}

void RysumProcessor::setSofaFile (const juce::File& f)
{
    sofaFile = f;
    builtBinKey.clear();           // the timer designs it (only while binaural is selected)
    if (! isBinaural()) setBinStatus (juce::String::fromUTF8 ("gew\xc3\xa4hlt: ") + (f == juce::File() ? juce::String ("FABIAN") : f.getFileName())
                                      + juce::String::fromUTF8 (" \xe2\x80\x93 aktiv, sobald die Ausgabe auf Binaural steht"));
}

void RysumProcessor::rebuildBinaural()
{
    const auto key = binauralKey();
    builtBinKey = key;
    const int order = 1 + juce::roundToInt (binOrderParam->load());
    const bool df = binDfParam->load() > 0.5f;
    const double rate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
    const auto file = sofaFile;
    auto set = (hrirSet != nullptr && hrirSetFile == file) ? hrirSet : std::shared_ptr<const rysum::HrirSet>();
    setBinStatus (juce::String::fromUTF8 ("berechne Filter \xe2\x80\xa6"));
    irPool.addJob ([this, key, order, df, rate, file, set, alive = aliveFlag]
    {
        juce::String err;
        auto useSet = set;
        if (useSet == nullptr)
        {
            std::unique_ptr<rysum::HrirSet> loaded (file == juce::File() ? rysum::HrirSet::fabian (err) : rysum::HrirSet::fromSofa (file, err));
            useSet.reset (loaded.release());
        }
        std::shared_ptr<rysum::BinauralDecoder> dec;
        if (useSet != nullptr)
            if (auto f = rysum::BinauralFilters::design (*useSet, order, rate, df, [alive] { return ! alive->load(); }))
                dec = std::make_shared<rysum::BinauralDecoder> (*f);
        if (useSet != nullptr && dec == nullptr && err.isEmpty()) err = "zu wenige Richtungen in der HRTF";
        juce::MessageManager::callAsync ([this, key, file, useSet, dec, err, alive]
        {
            if (! alive->load()) return;
            if (useSet != nullptr) { hrirSet = useSet; hrirSetFile = file; }
            if (key != builtBinKey) return;                               // settings changed meanwhile
            if (dec == nullptr) { setBinStatus ((file == juce::File() ? juce::String ("FABIAN") : file.getFileName()) + ": " + err); return; }
            retiredBin = binUi;
            retireCountdown = 20;
            binUi = dec;
            std::atomic_store (&pendingBin, dec);
            setBinStatus (dec->getDescription() + juce::String::fromUTF8 (" \xc2\xb7 ") + useSet->info);
        });
    });
}

void RysumProcessor::clearIr()
{
    irFile = juce::File();
    retiredIr = irUi;
    retireCountdown = 20;
    irUi.reset();
    std::atomic_store (&pendingIr, std::shared_ptr<rysum::Convolver>());
    irClearRequest = true;
    setIrStatus (u8 ("keine \xe2\x80\x93 nur trocken"));
}

// ---------------------------------------------------------------------------
// samples
// ---------------------------------------------------------------------------
void RysumProcessor::loadSamples (const juce::File& folder)
{
    loader.reset();
    samplesFolder = folder;
    loading = true;
    setStatus ("Lade Samples aus " + folder.getFullPathName() + " ...");
    loader = std::make_unique<Loader> (*this, folder);
    loader->startThread();
}

juce::File RysumProcessor::getCustomFolder() const
{
    if (customOverride != juce::File()) return customOverride;
    return samplesFolder != juce::File() ? SampleLibrary::customFolder (samplesFolder) : juce::File();
}

void RysumProcessor::setCustomFolder (const juce::File& dir)
{
    if (dir == customOverride) { reloadCustomSounds(); return; }
    customOverride = dir.isDirectory() ? dir : juce::File();
    reloadCustomSounds();
    ++changeCounter;
}

int RysumProcessor::getNumCustomSounds() const
{
    auto lib = std::atomic_load (&uiLibrary);
    return lib != nullptr ? (int) lib->custom.size() : 0;
}

void RysumProcessor::reloadCustomSounds()
{
    auto old = std::atomic_load (&uiLibrary);
    if (old == nullptr || samplesFolder == juce::File()) return;
    const auto folder = samplesFolder;
    const auto dir = getCustomFolder();
    customReloading = true;
    irPool.addJob ([this, old, folder, dir, alive = aliveFlag]
    {
        auto lib = std::make_shared<SampleLibrary> (*old);       // pipes / clicks are shared, only the own sounds are new
        lib->custom = SampleLibrary::loadCustomDir (dir);
        std::shared_ptr<const SampleLibrary> shared (lib);
        juce::MessageManager::callAsync ([this, shared, folder, alive]
        {
            if (alive->load()) onLoaded (shared, {}, folder);
        });
    });
}

void RysumProcessor::onLoaded (std::shared_ptr<const SampleLibrary> lib, const juce::String& err, const juce::File& folder)
{
    loading = false;
    const bool wasCustomReload = std::exchange (customReloading, false);
    if (lib == nullptr)
    {
        setStatus ("Keine Samples: " + err);
        ++changeCounter;
        return;
    }
    retiredLibrary = std::atomic_load (&uiLibrary);
    retireCountdown = 20;                                 // ~5 s, then freed on the message thread
    std::atomic_store (&uiLibrary, lib);
    std::atomic_store (&pendingLibrary, lib);
    samplesFolder = folder;
    juce::String s;
    s << lib->loadedPipes << " Pfeifen geladen";
    if (lib->substitutedPipes > 0) s << ", " << lib->substitutedPipes << " ersetzt";
    if (! lib->custom.empty()) s << ", " << (int) lib->custom.size() << juce::String::fromUTF8 (" eigene Ger\xc3\xa4usche");
    setStatus (s);
    ++changeCounter;
    // a full load reads the standard folder: a chosen one is read right after
    if (! wasCustomReload && customOverride != juce::File()) reloadCustomSounds();
}

void RysumProcessor::startAutoLoad()
{
    if (samplesFolder != juce::File() && SampleLibrary::looksLikeSampleFolder (samplesFolder))
    {
        loadSamples (samplesFolder);
        return;
    }
    for (auto& f : SampleLibrary::candidateFolders())
        if (SampleLibrary::looksLikeSampleFolder (f)) { loadSamples (f); return; }
    setStatus (u8 ("Samples-Ordner nicht gefunden \xe2\x80\x93 bitte w\xc3\xa4hlen"));
    ++changeCounter;
}

void RysumProcessor::timerCallback()
{
    if (! autoLoadDone)
    {
        autoLoadDone = true;
        writeFactoryPatches();
        startAutoLoad();
        if (irFile.existsAsFile()) loadIr (irFile);
    }
    if (retireCountdown > 0 && --retireCountdown == 0) { retiredLibrary.reset(); retiredIr.reset(); retiredBin.reset(); }
    rebuildSpeakers (false);
    {
        // decoders the audio thread no longer uses (the latest one always stays)
        const uint32_t inUse = spkRenderer.serialInUse.load();
        while (spkKeepAlive.size() > 1 && spkKeepAlive.front()->serial < inUse && std::atomic_load (&pendingSpk) == nullptr) spkKeepAlive.pop_front();
        while (spkKeepAlive.size() > 8) spkKeepAlive.pop_front();
    }
    // MIDI controller assignments changed (edited, learned, or a project brought its own): the next new instance starts with them
    if (midiMapVersion.load() != savedMapVersion && mmLearn.load() < 0)
    {
        savedMapVersion = midiMapVersion.load();
        saveMidiMapDefault();
    }
    // simulated room: (re)build the late reverb whenever the room as heard has changed (also while it is
    // modulated: then about four times a second, each new response crossfaded in by the engine)
    if (autoLoadDone && engine.roomOn.load())
    {
        const auto key = roomKey();
        if (key != builtTailKey && ! tailBuilding) buildTailNow (key);
    }
    {
        // responses the engine no longer uses are freed here, not on the audio thread
        const uint32_t inUse = engine.tailSerialInUse.load();
        while (tailKeepAlive.size() > 1 && tailKeepAlive.front()->serial < inUse) tailKeepAlive.pop_front();
        const uint32_t lucInUse = engine.lucierSerialInUse.load();
        while (lucKeepAlive.size() > 1 && lucKeepAlive.front()->serial < lucInUse) lucKeepAlive.pop_front();
    }
    if (autoLoadDone) updateLucier (getTimerInterval() * 0.001);
    lucPiece.tick (lucBase, juce::roundToInt (apvts.getRawParameterValue ("lucGen")->load()), apvts.getRawParameterValue ("lucGap")->load(),
                   piecePool, aliveFlag);
    if (lucPiece.state.load() != rysum::LucierPiece::stRecording && lucPiece.recordingVersion != savedRecordingVersion)
        saveLucierRecording();
    // binaural: (re)design the decoder when the mode is on and its settings changed (~0.5 s debounce)
    if (autoLoadDone && isBinaural())
    {
        const auto key = binauralKey();
        if (key != builtBinKey)
        {
            if (binUi == nullptr && builtBinKey.isEmpty()) rebuildBinaural();         // first time: right away
            else if (key != pendingBinKey) { pendingBinKey = key; binKeyTicks = 0; }
            else if (++binKeyTicks >= 2) { rebuildBinaural(); pendingBinKey.clear(); }
        }
    }
    // IR settings / output format changed: rebuild the room once the values have settled (~0.5 s)
    if (irFile.existsAsFile() && autoLoadDone)
    {
        const auto key = irSettingsKey (currentIrSettings());
        if (key != builtIrKey)
        {
            if (key != pendingIrKey) { pendingIrKey = key; irKeyTicks = 0; }
            else if (++irKeyTicks >= 2) { loadIr (irFile); pendingIrKey.clear(); }
        }
    }
}

rysum::Convolver::Settings RysumProcessor::currentIrSettings() const
{
    rysum::Convolver::Settings s;
    s.format = irFumaParam->load() > 0.5f ? rysum::Convolver::Format::FuMa
             : (irN3DParam->load() > 0.5f ? rysum::Convolver::Format::N3D : rysum::Convolver::Format::AmbiX);
    const int out = (int) output->load();
    s.stereoOut = out == 0;
    s.outOrder = juce::jlimit (1, kMaxOrder, out == 0 ? 1 : juce::jmin (out, 1 + (int) apvts.getRawParameterValue ("irOrder")->load()));
    s.envelop = irEnvelopParam->load();
    s.direct = irDirectParam->load();
    s.hostRate = hostRate.load() > 0 ? hostRate.load() : 48000.0;
    return s;
}

juce::String RysumProcessor::irSettingsKey (const rysum::Convolver::Settings& s)
{
    return juce::String ((int) s.format) + "|" + juce::String (s.outOrder) + "|" + juce::String ((int) s.stereoOut) + "|"
         + juce::String (s.envelop, 2) + "|" + juce::String (s.direct, 2) + "|" + juce::String (s.hostRate, 0);
}

// ---------------------------------------------------------------------------
// layouts / positions
// ---------------------------------------------------------------------------
void RysumProcessor::applyLayout (int stop)
{
    std::array<Pos, kNumKeys> pos {};
    generateLayout (layouts[(size_t) stop], stop, pos);
    for (int k = 0; k < kNumKeys; ++k) setPipePosition (stop, k, pos[(size_t) k]);
}

void RysumProcessor::setPipePosition (int stop, int key, Pos p)
{
    engine.baseAz[(size_t) stop][(size_t) key] = wrapAz (p.az);
    engine.baseEl[(size_t) stop][(size_t) key] = juce::jlimit (-90.0f, 90.0f, p.el);
    engine.baseDist[(size_t) stop][(size_t) key] = juce::jlimit (kMinDist, kMaxDist, p.dist);
    ++changeCounter;
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
juce::ValueTree RysumProcessor::makeState (bool forPatch)
{
    auto state = apvts.copyState();
    juce::ValueTree extra ("EXTRA");
    if (! forPatch) extra.setProperty ("samplesFolder", samplesFolder.getFullPathName(), nullptr);
    extra.setProperty ("irFile", irFile.getFullPathName(), nullptr);
    extra.setProperty ("customFolder", customOverride.getFullPathName(), nullptr);
    if (! forPatch) extra.setProperty ("sofaFile", sofaFile.getFullPathName(), nullptr);
    extra.setProperty ("patch", patchName, nullptr);
    if (forPatch && apvts.getRawParameterValue ("inpOn")->load() > 0.5f) extra.setProperty ("input", true, nullptr);   // a patch made for the input
    if (sclText.isNotEmpty())
    {
        // the tuning files themselves (not only their paths): the project / patch works on any computer
        extra.setProperty ("scl", sclText, nullptr);
        extra.setProperty ("sclName", sclName, nullptr);
        if (kbmText.isNotEmpty()) { extra.setProperty ("kbm", kbmText, nullptr); extra.setProperty ("kbmName", kbmName, nullptr); }
    }
    for (int s = 0; s < kNumStops; ++s)
    {
        juce::ValueTree st ("STOP");
        st.setProperty ("id", kStops[(size_t) s].id, nullptr);
        const auto& L = layouts[(size_t) s];
        st.setProperty ("layout", L.type, nullptr);
        st.setProperty ("width", L.width, nullptr);
        st.setProperty ("center", L.center, nullptr);
        st.setProperty ("elevation", L.elevation, nullptr);
        st.setProperty ("spread", L.spread, nullptr);
        juce::StringArray az, el, mu, di;
        for (int k = 0; k < kNumKeys; ++k)
        {
            di.add (juce::String (engine.baseDist[(size_t) s][(size_t) k].load(), 2));
            az.add (juce::String (engine.baseAz[(size_t) s][(size_t) k].load(), 2));
            el.add (juce::String (engine.baseEl[(size_t) s][(size_t) k].load(), 2));
            mu.add (engine.muted[(size_t) s][(size_t) k].load() ? "1" : "0");
        }
        st.setProperty ("az", az.joinIntoString (","), nullptr);
        st.setProperty ("el", el.joinIntoString (","), nullptr);
        st.setProperty ("mute", mu.joinIntoString (","), nullptr);
        st.setProperty ("dist", di.joinIntoString (","), nullptr);
        st.setProperty ("distance", L.distance, nullptr);
        const auto& dp = drawnPaths[(size_t) s];
        if (! dp.points.empty())
        {
            juce::StringArray pts;
            for (auto& q : dp.points) pts.add (juce::String (q.az, 2) + " " + juce::String (q.el, 2) + " " + juce::String (q.t, 3));
            st.setProperty ("path", pts.joinIntoString (","), nullptr);
            st.setProperty ("pathPingPong", dp.pingPong, nullptr);
            st.setProperty ("pathTiming", dp.drawnTiming, nullptr);
        }
        extra.appendChild (st, nullptr);
    }
    {
        juce::ValueTree mod ("MOD");
        for (int r = 0; r < kNumRoutes; ++r)
        {
            mod.setProperty ("src" + juce::String (r), routeSrc[(size_t) r].load(), nullptr);
            mod.setProperty ("tgt" + juce::String (r), targetId (routeTgt[(size_t) r].load()), nullptr);
            if (routeUni[(size_t) r].load()) mod.setProperty ("uni" + juce::String (r), true, nullptr);
        }
        extra.appendChild (mod, nullptr);
    }
    if (! forPatch) extra.appendChild (midiMapState(), nullptr);       // controller assignments belong to the setup, not to the sound
    if (! forPatch) extra.appendChild (speakersState(), nullptr);      // so does the loudspeaker layout
    if (! forPatch && lucPiece.recordingFile.isNotEmpty()) extra.setProperty ("lucRecording", lucPiece.recordingFile, nullptr);
    extra.appendChild (scenesState(), nullptr);
    if (! forPatch)
    {
        juce::String dg;
        for (int g = 0; g < kNumDiceGroups; ++g) dg << (diceGroups[(size_t) g] ? "1" : "0");
        extra.setProperty ("diceGroups", dg, nullptr);
    }
    state.removeChild (state.getChildWithName ("EXTRA"), nullptr);
    state.appendChild (extra, nullptr);
    return state;
}

void RysumProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = makeState (false).createXml()) copyXmlToBinary (*xml, destData);
}

void RysumProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr) return;
    applyState (juce::ValueTree::fromXml (*xml), false);
}

void RysumProcessor::applyState (juce::ValueTree state, bool forPatch)
{
    if (! state.hasType (apvts.state.getType())) return;
    auto extra = state.getChildWithName ("EXTRA");
    const float keepOutput = output->load();
    // kept when a patch loads: headphone settings and the controller setup (MPE) belong to the player, not the sound
    static const char* const keepIds[] { "binOrder", "binYaw", "binDfEq", "mpe", "mpeBend", "inpOn", "spkOrder", "spkDistComp" };
    float keepVals[std::size (keepIds)] {};
    for (size_t i = 0; i < std::size (keepIds); ++i) keepVals[i] = apvts.getRawParameterValue (keepIds[i])->load();
    if (forPatch)
    {
        // a patch sets every parameter: ones missing in the file (older patches) go to their defaults
        state = state.createCopy();
        for (auto* prm : prmList)
            if (! state.getChildWithProperty ("id", prm->getParameterID()).isValid())
            {
                juce::ValueTree t ("PARAM");
                t.setProperty ("id", prm->getParameterID(), nullptr);
                t.setProperty ("value", prm->convertFrom0to1 (prm->getDefaultValue()), nullptr);
                state.appendChild (t, nullptr);
            }
    }
    apvts.replaceState (state);
    if (forPatch)
    {
        // the output format belongs to the track, not to the sound
        if (auto* o = apvts.getParameter ("output")) o->setValueNotifyingHost (o->convertTo0to1 (keepOutput));
        // so do the headphone settings and MPE
        for (size_t i = 0; i < std::size (keepIds); ++i)
            if (auto* o = apvts.getParameter (keepIds[i])) o->setValueNotifyingHost (o->convertTo0to1 (keepVals[i]));
        // the input stays as it is (it is part of the setup) - unless the patch is made for it: then it comes on
        if ((bool) extra.getProperty ("input", false))
            if (auto* o = apvts.getParameter ("inpOn")) o->setValueNotifyingHost (1.0f);
        // stops the patch does not describe get their default layout
        for (int s = 0; s < kNumStops; ++s)
        {
            bool found = false;
            for (auto st : extra) found = found || (st.hasType ("STOP") && stopIndexForId (st["id"].toString()) == s);
            if (! found)
            {
                layouts[(size_t) s] = defaultLayout (s);
                applyLayout (s);
                drawnPaths[(size_t) s] = {};
                sendPath (s);
                for (auto& m : engine.muted[(size_t) s]) m = false;
            }
        }
    }
    patchName = extra["patch"].toString();
    if (extra.hasProperty ("scl"))
        setScala (extra["scl"].toString(), extra["sclName"].toString(), extra["kbm"].toString(), extra["kbmName"].toString());
    else if (! forPatch && sclText.isNotEmpty())
        setScala ({}, {}, {}, {});                   // a project without a Scala file (a patch without one keeps the current file)

    {
        const auto mod = extra.getChildWithName ("MOD");
        for (int r = 0; r < kNumRoutes; ++r)
        {
            const int src = mod.isValid() ? (int) mod.getProperty ("src" + juce::String (r), -1) : -1;
            const int tgt = mod.isValid() ? targetFromId (mod.getProperty ("tgt" + juce::String (r)).toString()) : -1;
            setRoute (r, src >= 0 && src < kNumSources ? src : -1, tgt);
            routeUni[(size_t) r] = mod.isValid() && (bool) mod.getProperty ("uni" + juce::String (r), false);
        }
    }
    if (extra.isValid())
    {
        for (auto st : extra)
        {
            if (! st.hasType ("STOP")) continue;
            const int s = stopIndexForId (st["id"].toString());
            if (s < 0) continue;
            auto& L = layouts[(size_t) s];
            L.type = (int) st.getProperty ("layout", L.type);
            L.width = (float) st.getProperty ("width", L.width);
            L.center = (float) st.getProperty ("center", L.center);
            L.elevation = (float) st.getProperty ("elevation", L.elevation);
            L.spread = (float) st.getProperty ("spread", L.spread);
            L.distance = (float) st.getProperty ("distance", L.distance);
            {
                auto& dp = drawnPaths[(size_t) s];
                dp.points.clear();
                juce::StringArray pts;
                pts.addTokens (st["path"].toString(), ",", {});
                for (auto& t : pts)
                {
                    juce::StringArray v; v.addTokens (t, " ", {});
                    if (v.size() >= 3) dp.points.push_back ({ v[0].getFloatValue(), v[1].getFloatValue(), v[2].getFloatValue() });
                }
                dp.pingPong = (bool) st.getProperty ("pathPingPong", false);
                dp.drawnTiming = (bool) st.getProperty ("pathTiming", false);
                sendPath (s);
            }
            juce::StringArray di;
            di.addTokens (st["dist"].toString(), ",", {});
            for (int k = 0; k < juce::jmin (kNumKeys, di.size()); ++k)
                engine.baseDist[(size_t) s][(size_t) k] = juce::jlimit (kMinDist, kMaxDist, di[k].getFloatValue());
            juce::StringArray az, el, mu;
            az.addTokens (st["az"].toString(), ",", {});
            el.addTokens (st["el"].toString(), ",", {});
            mu.addTokens (st["mute"].toString(), ",", {});
            for (int k = 0; k < kNumKeys; ++k)
            {
                if (k < az.size()) engine.baseAz[(size_t) s][(size_t) k] = az[k].getFloatValue();
                if (k < el.size()) engine.baseEl[(size_t) s][(size_t) k] = el[k].getFloatValue();
                if (k < mu.size()) engine.muted[(size_t) s][(size_t) k] = mu[k] == "1";
            }
        }
        const juce::File irf (extra["irFile"].toString());
        if (irf != juce::File() && irf != irFile && irf.existsAsFile())
        {
            irFile = irf;
            if (autoLoadDone) loadIr (irf);           // otherwise the first timer tick loads it
        }
        else if (irf == juce::File() && irFile != juce::File() && autoLoadDone && ! forPatch) clearIr();   // patches without IR keep the room
        {
            // own sounds: a project always says which folder; a patch only if it was saved with a chosen one
            const juce::File cf (extra["customFolder"].toString());
            if (cf != juce::File() && cf.isDirectory()) { if (cf != customOverride) { customOverride = cf; if (autoLoadDone) reloadCustomSounds(); } }
            else if (! forPatch && customOverride != juce::File()) { customOverride = juce::File(); if (autoLoadDone) reloadCustomSounds(); }
        }
        applyScenesState (extra.getChildWithName ("SCENES"));
        if (! forPatch && extra.hasProperty ("diceGroups"))
        {
            const auto dg = extra["diceGroups"].toString();
            for (int g = 0; g < juce::jmin ((int) kNumDiceGroups, dg.length()); ++g) diceGroups[(size_t) g] = dg[g] == '1';
        }
        if (! forPatch)
        {
            const auto mm = extra.getChildWithName ("MIDIMAP");
            if (mm.isValid()) applyMidiMapState (mm);
            {
                const juce::File lr (extra["lucRecording"].toString());
                juce::String err;
                if (lr.existsAsFile() && lr.getFullPathName() != lucPiece.recordingFile) loadLucierRecording (lr, err);
            }
            const auto sp = extra.getChildWithName ("SPEAKERS");
            rysum::SpeakerLayout sl; juce::String err;
            if (sp.isValid() && rysum::speakerLayoutFromJson (sp["json"].toString(), sl, err)) setSpeakerLayout (sl, false);
            const juce::File sf (extra["sofaFile"].toString());
            if (sf != sofaFile) setSofaFile (sf.existsAsFile() ? sf : juce::File());
        }
        const juce::File f (forPatch ? juce::File() : juce::File (extra["samplesFolder"].toString()));
        if (f != juce::File() && f != samplesFolder && SampleLibrary::looksLikeSampleFolder (f))
        {
            samplesFolder = f;
            if (autoLoadDone) loadSamples (f);       // otherwise the auto-load picks it up
        }
    }
    ++changeCounter;
}

// ---------------------------------------------------------------------------
// drawn trajectories
// ---------------------------------------------------------------------------
void RysumProcessor::setDrawnPath (int stop, std::vector<DrawnPath::Pt> points)
{
    if (stop < 0 || stop >= kNumStops) return;
    drawnPaths[(size_t) stop].points = std::move (points);
    sendPath (stop);
    ++changeCounter;
}

void RysumProcessor::setPathOptions (int stop, bool pingPong, bool drawnTiming)
{
    if (stop < 0 || stop >= kNumStops) return;
    drawnPaths[(size_t) stop].pingPong = pingPong;
    drawnPaths[(size_t) stop].drawnTiming = drawnTiming;
    sendPath (stop);
    ++changeCounter;
}

void RysumProcessor::sendPath (int stop)
{
    // the drawn points -> the engine's path: unit vectors, at most kMax points, the time (0..1) every point is reached
    const auto& dp = drawnPaths[(size_t) stop];
    MotionPath mp;
    mp.pingPong = dp.pingPong;
    const int count = (int) dp.points.size();
    if (count >= 2)
    {
        const int maxPts = MotionPath::kMax - 1;                    // room for closing the loop
        const int stepN = juce::jmax (1, (count + maxPts - 1) / maxPts);
        std::vector<DrawnPath::Pt> pts;
        for (int i = 0; i < count; i += stepN) pts.push_back (dp.points[(size_t) i]);
        if ((count - 1) % stepN != 0) pts.push_back (dp.points.back());
        pts.resize ((size_t) juce::jmin ((int) pts.size(), maxPts));
        auto vec = [] (const DrawnPath::Pt& q, float& x, float& y, float& z)
        {
            const float a = juce::degreesToRadians (q.az), e = juce::degreesToRadians (q.el);
            x = std::cos (e) * std::cos (a); y = std::cos (e) * std::sin (a); z = std::sin (e);
        };
        const int n = (int) pts.size();
        for (int i = 0; i < n; ++i) vec (pts[(size_t) i], mp.x[(size_t) i], mp.y[(size_t) i], mp.z[(size_t) i]);
        const bool loop = ! dp.pingPong;
        int m = n;
        if (loop) { mp.x[(size_t) n] = mp.x[0]; mp.y[(size_t) n] = mp.y[0]; mp.z[(size_t) n] = mp.z[0]; m = n + 1; }
        // the weight of every segment: its length (even speed) or the time it took to draw
        std::vector<double> w ((size_t) (m - 1), 0.0);
        double totalLen = 0.0, totalT = 0.0;
        for (int i = 0; i + 1 < m; ++i)
        {
            const double dx = mp.x[(size_t) i + 1] - mp.x[(size_t) i], dy = mp.y[(size_t) i + 1] - mp.y[(size_t) i], dz = mp.z[(size_t) i + 1] - mp.z[(size_t) i];
            w[(size_t) i] = std::sqrt (dx * dx + dy * dy + dz * dz);
            totalLen += w[(size_t) i];
            if (i + 1 < n) totalT += juce::jmax (0.0f, pts[(size_t) i + 1].t - pts[(size_t) i].t);
        }
        if (dp.drawnTiming && totalT > 1.0e-3)
        {
            // drawing time per segment; the closing segment at the average drawing speed
            const double speed = totalLen / juce::jmax (1.0e-6, totalLen - (loop ? w.back() : 0.0)) * totalT;
            for (int i = 0; i + 1 < m; ++i)
                w[(size_t) i] = (i + 1 < n) ? juce::jmax (0.0, (double) (pts[(size_t) i + 1].t - pts[(size_t) i].t)) + 1.0e-6
                                            : w[(size_t) i] / juce::jmax (1.0e-6, totalLen) * speed;
        }
        double acc = 0.0, sum = 0.0;
        for (double x : w) sum += x;
        mp.u[0] = 0.0f;
        for (int i = 0; i + 1 < m; ++i) { acc += w[(size_t) i]; mp.u[(size_t) i + 1] = (float) (acc / juce::jmax (1.0e-9, sum)); }
        mp.n = m;
    }
    engine.setPath (stop, mp);
}

// ---------------------------------------------------------------------------
// free tuning: Scala files, MTS-ESP
// ---------------------------------------------------------------------------
bool RysumProcessor::mtsActive() const
{
    return mts != nullptr && (MTS_HasMaster (mts) || MTS_HasReceivedMTSSysEx (mts));
}

bool RysumProcessor::mtsHasMaster() const { return mts != nullptr && MTS_HasMaster (mts); }

bool RysumProcessor::setScala (const juce::String& scl, const juce::String& sName, const juce::String& kbm, const juce::String& kName)
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
    if (! ScalaTuning::parseScl (scl, t, err)) { tuningError = sName + ": " + err; return false; }
    if (kbm.isNotEmpty() && ! ScalaTuning::parseKbm (kbm, t, err)) { tuningError = kName + ": " + err; return false; }
    scala = std::move (t);
    sclText = scl; sclName = sName; kbmText = kbm.isNotEmpty() ? kbm : juce::String(); kbmName = kbm.isNotEmpty() ? kName : juce::String();
    tuningError.clear();
    const auto f = scala.frequencies();
    {
        const juce::SpinLock::ScopedLockType sl (scalaLock);
        for (int ch = 0; ch < 16; ++ch)
            for (int n = 0; n < 128; ++n)
            {
                scalaShared.hz[(size_t) (ch * 128 + n)] = (float) f[(size_t) n];
                scalaShared.mapped[(size_t) (ch * 128 + n)] = f[(size_t) n] > 0.0 ? 1 : 0;
            }
        ++scalaVersion;
    }
    scalaReady = true;
    ++changeCounter;
    return true;
}

bool RysumProcessor::loadTuningFile (const juce::File& f)
{
    const auto text = f.loadFileAsString();
    bool ok;
    if (f.hasFileExtension ("kbm"))
    {
        if (sclText.isEmpty()) { tuningError = u8 ("Erst eine Skala (.scl) laden, dann die Tastenzuordnung (.kbm)"); return false; }
        ok = setScala (sclText, sclName, text, f.getFileName());
    }
    else ok = setScala (text, f.getFileName(), kbmText, kbmName);
    if (ok)
        if (auto* t = apvts.getParameter ("temperament"))
        {
            t->beginChangeGesture();
            t->setValueNotifyingHost (t->convertTo0to1 ((float) kScalaTuning));
            t->endChangeGesture();
        }
    return ok;
}

void RysumProcessor::clearKbm()
{
    if (sclText.isNotEmpty()) setScala (sclText, sclName, {}, {});
}

juce::String RysumProcessor::getTuningStatus() const
{
    const int t = juce::roundToInt (extraParams.at ("temperament")->load());
    const auto dot = u8 (" \xc2\xb7 ");
    juce::String s;
    if (tuningError.isNotEmpty() && t == kScalaTuning) return u8 ("Fehler: ") + tuningError;
    if (t == kScalaTuning)
    {
        if (! scalaReady.load()) return u8 ("Noch keine Skala geladen \xe2\x80\x93 \xe2\x80\x9eScala laden\xe2\x80\xa6\xe2\x80\x9c (.scl, dazu optional .kbm)");
        s = sclName + dot + juce::String (scala.size()) + " Stufen" + dot + "Periode " + juce::String (scala.cents.back(), 1) + " ct";
        if (scala.description.isNotEmpty()) s << "\n" << scala.description;
        s << "\n" << (kbmText.isNotEmpty() ? "KBM: " + kbmName
                                          : u8 ("Standard-Zuordnung: Stufe 0 auf Taste 60, Taste 69 = 444,5 Hz"));
        return s;
    }
    if (t == kMtsTuning)
    {
        if (mts == nullptr) return u8 ("MTS-ESP nicht verf\xc3\xbcgbar");
        const juce::String name (juce::CharPointer_UTF8 (MTS_GetScaleName (mts)));
        if (MTS_HasMaster (mts)) return u8 ("MTS-ESP: Master verbunden") + (name.isNotEmpty() ? dot + name : juce::String());
        if (MTS_HasReceivedMTSSysEx (mts)) return u8 ("MIDI-Stimmung (MTS-SysEx) empfangen") + (name.isNotEmpty() ? dot + name : juce::String());
        return u8 ("Kein MTS-ESP-Master verbunden, noch keine MTS-SysEx empfangen \xe2\x80\x93 bis dahin die gew\xc3\xa4hlte Originalstimmung (\xc2\xbc-Komma)");
    }
    if (mtsHasMaster()) return u8 ("Ein MTS-ESP-Master ist verbunden \xe2\x80\x93 \xe2\x80\x9eMTS-ESP\xe2\x80\x9c w\xc3\xa4hlen, um ihm zu folgen");
    return {};
}

// ---------------------------------------------------------------------------
// MIDI learn
// ---------------------------------------------------------------------------
RysumProcessor::MidiMap RysumProcessor::getMidiMap (int i) const
{
    MidiMap m;
    if (i < 0 || i >= kNumMidiMaps) return m;
    m.cc = mmCc[(size_t) i].load(); m.channel = mmChan[(size_t) i].load(); m.param = mmParam[(size_t) i].load();
    m.lo = mmLo[(size_t) i].load(); m.hi = mmHi[(size_t) i].load(); m.toggle = mmToggle[(size_t) i].load();
    return m;
}

void RysumProcessor::setMidiMap (int i, const MidiMap& m)
{
    if (i < 0 || i >= kNumMidiMaps) return;
    mmParam[(size_t) i] = -1;                 // the audio thread skips the slot while it changes
    mmCc[(size_t) i] = juce::jlimit (-1, 119, m.cc);
    mmChan[(size_t) i] = juce::jlimit (0, 16, m.channel);
    mmLo[(size_t) i] = juce::jlimit (0.0f, 1.0f, m.lo);
    mmHi[(size_t) i] = juce::jlimit (0.0f, 1.0f, m.hi);
    mmToggle[(size_t) i] = m.toggle;
    mmParam[(size_t) i] = m.param >= 0 && m.param < getNumParams() ? m.param : -1;
    ++midiMapVersion;
}

void RysumProcessor::clearMidiMap (int i)
{
    if (i < 0 || i >= kNumMidiMaps) return;
    int l = i;
    mmLearn.compare_exchange_strong (l, -1);
    setMidiMap (i, MidiMap {});
}

void RysumProcessor::learnMidi (int slot)
{
    mmLearn = slot >= 0 && slot < kNumMidiMaps ? slot : -1;
    ++midiMapVersion;
}

int RysumProcessor::learnMidiFor (int prmIdx)
{
    if (getParamAt (prmIdx) == nullptr) return -1;
    for (int i = 0; i < kNumMidiMaps; ++i)
        if (! getMidiMap (i).used())
        {
            MidiMap m;
            m.param = prmIdx;
            // switches: a button press flips them
            m.toggle = dynamic_cast<juce::AudioParameterBool*> (getParamAt (prmIdx)) != nullptr;
            setMidiMap (i, m);
            learnMidi (i);
            return i;
        }
    return -1;
}

juce::Array<int> RysumProcessor::midiMapsFor (int prmIdx) const
{
    juce::Array<int> r;
    for (int i = 0; i < kNumMidiMaps; ++i) if (mmParam[(size_t) i].load() == prmIdx && prmIdx >= 0) r.add (i);
    return r;
}

juce::ValueTree RysumProcessor::midiMapState() const
{
    juce::ValueTree t ("MIDIMAP");
    for (int i = 0; i < kNumMidiMaps; ++i)
    {
        const auto m = getMidiMap (i);
        if (! m.used()) continue;
        juce::ValueTree e ("MAP");
        e.setProperty ("slot", i, nullptr);
        e.setProperty ("cc", m.cc, nullptr);
        e.setProperty ("channel", m.channel, nullptr);
        e.setProperty ("param", m.param >= 0 ? prmList[(size_t) m.param]->getParameterID() : juce::String(), nullptr);
        e.setProperty ("lo", m.lo, nullptr);
        e.setProperty ("hi", m.hi, nullptr);
        e.setProperty ("toggle", m.toggle, nullptr);
        t.appendChild (e, nullptr);
    }
    return t;
}

void RysumProcessor::applyMidiMapState (const juce::ValueTree& t)
{
    if (! t.hasType ("MIDIMAP")) return;
    for (int i = 0; i < kNumMidiMaps; ++i) setMidiMap (i, MidiMap {});
    for (auto e : t)
    {
        const int slot = (int) e.getProperty ("slot", -1);
        if (slot < 0 || slot >= kNumMidiMaps) continue;
        MidiMap m;
        m.cc = (int) e.getProperty ("cc", -1);
        m.channel = (int) e.getProperty ("channel", 0);
        m.param = getParamIndex (e["param"].toString());
        m.lo = (float) e.getProperty ("lo", 0.0f);
        m.hi = (float) e.getProperty ("hi", 1.0f);
        m.toggle = (bool) e.getProperty ("toggle", false);
        if (m.used()) setMidiMap (slot, m);
    }
    mmLearn = -1;
}

juce::File RysumProcessor::midiMapFile()
{
    return getPatchFolder().getParentDirectory().getChildFile ("MIDI-Regler.xml");
}

void RysumProcessor::saveMidiMapDefault()
{
    if (auto xml = midiMapState().createXml())
    {
        midiMapFile().getParentDirectory().createDirectory();
        xml->writeTo (midiMapFile());
    }
}

// ---------------------------------------------------------------------------
// patches: XML files in Documents/Rysum/Patches
// ---------------------------------------------------------------------------
juce::File RysumProcessor::getPatchFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Rysum").getChildFile ("Patches");
}

juce::Array<juce::File> RysumProcessor::getPatchFiles()
{
    auto files = getPatchFolder().findChildFiles (juce::File::findFiles, false, "*.rysum");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
               { return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension()) < 0; });
    return files;
}

juce::String RysumProcessor::cleanPatchName (const juce::String& name)
{
    return juce::File::createLegalFileName (name.trim()).trim();
}

bool RysumProcessor::savePatch (const juce::String& name)
{
    const auto clean = cleanPatchName (name);
    if (clean.isEmpty()) return false;
    auto folder = getPatchFolder();
    if (! folder.createDirectory()) return false;
    patchName = clean;
    auto xml = makeState (true).createXml();
    if (xml == nullptr) return false;
    const bool ok = xml->writeTo (folder.getChildFile (clean + ".rysum"));
    ++patchVersion;
    return ok;
}

bool RysumProcessor::loadPatch (const juce::File& f)
{
    auto xml = juce::XmlDocument::parse (f);
    if (xml == nullptr) return false;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.hasType (apvts.state.getType())) return false;
    applyState (state, true);
    patchName = f.getFileNameWithoutExtension();
    ++patchVersion;
    return true;
}

void RysumProcessor::writeFactoryPatches()
{
    auto folder = getPatchFolder();
    // written once per factory-set version, so deleted patches stay deleted until a new set comes
    // each patch belongs to a set; only sets newer than the last one written are added
    // (deleted patches stay deleted, edited ones are not overwritten)
    constexpr int kFactoryVersion = 10;
    const auto marker = folder.getChildFile (".factory_written");
    const int written = marker.existsAsFile() ? marker.loadFileAsString().trim().getIntValue() : 0;
    if (written >= kFactoryVersion) return;
    int currentSet = 1;
    if (! folder.createDirectory()) return;

    struct Route { int src; juce::String tgt; float amt; bool uni = false; };
    using Values = std::vector<std::pair<juce::String, double>>;
    // scenes: each one = the patch's values with these changes on top; 'input' = made for the audio input (switches it on)
    auto makeFull = [&] (const juce::String& name, Values values, std::vector<Route> routesIn, std::vector<Values> scenesIn, bool input)
    {
        for (auto& sc : scenesIn)
            for (auto& kv : sc)
                if (paramIndex.count (kv.first) == 0) factoryWarnings.add (name + ": unknown scene parameter " + kv.first);
        for (auto& kv : values)
            if (paramIndex.count (kv.first) == 0) factoryWarnings.add (name + ": unknown parameter " + kv.first);
        for (auto& rt : routesIn)
            if (targetFromId (rt.tgt) < 0) factoryWarnings.add (name + ": unknown target " + rt.tgt);
        if (currentSet <= written) return;
        juce::ValueTree state (apvts.state.getType());
        for (auto* prm : prmList)
        {
            float v = prm->convertFrom0to1 (prm->getDefaultValue());
            for (auto& kv : values) if (kv.first == prm->getParameterID()) v = (float) kv.second;
            juce::ValueTree t ("PARAM");
            t.setProperty ("id", prm->getParameterID(), nullptr);
            t.setProperty ("value", v, nullptr);
            state.appendChild (t, nullptr);
        }
        juce::ValueTree extra ("EXTRA"), mod ("MOD");
        extra.setProperty ("patch", name, nullptr);           // no IR: a factory patch keeps the room you loaded
        for (int r = 0; r < kNumRoutes; ++r)
        {
            const bool used = r < (int) routesIn.size();
            mod.setProperty ("src" + juce::String (r), used ? routesIn[(size_t) r].src : -1, nullptr);
            mod.setProperty ("tgt" + juce::String (r), used ? routesIn[(size_t) r].tgt : juce::String(), nullptr);
            if (used && routesIn[(size_t) r].uni) mod.setProperty ("uni" + juce::String (r), true, nullptr);
            if (used)
                for (int i = 0; i < state.getNumChildren(); ++i)
                    if (state.getChild (i)["id"].toString() == getModAmountId (r))
                        state.getChild (i).setProperty ("value", routesIn[(size_t) r].amt, nullptr);
        }
        extra.appendChild (mod, nullptr);
        if (input) extra.setProperty ("input", true, nullptr);
        if (! scenesIn.empty())
        {
            juce::ValueTree scs ("SCENES");
            for (size_t slot = 0; slot < scenesIn.size() && slot < (size_t) kNumScenes; ++slot)
            {
                juce::StringArray kv;
                for (auto* prm : prmList)
                {
                    const auto id = prm->getParameterID();
                    if (! takesPartInScenes (id)) continue;
                    float v = prm->convertFrom0to1 (prm->getDefaultValue());
                    for (auto& b : values) if (b.first == id) v = (float) b.second;
                    for (auto& c : scenesIn[slot]) if (c.first == id) v = (float) c.second;
                    kv.add (id + "=" + juce::String (prm->convertTo0to1 (v), 6));
                }
                juce::ValueTree sc ("SCENE");
                sc.setProperty ("slot", (int) slot, nullptr);
                sc.setProperty ("values", kv.joinIntoString (";"), nullptr);
                scs.appendChild (sc, nullptr);
            }
            extra.appendChild (scs, nullptr);
        }
        state.appendChild (extra, nullptr);
        if (auto xml = state.createXml()) xml->writeTo (folder.getChildFile (name + ".rysum"));
    };
    auto make = [&] (const juce::String& name, Values values, std::vector<Route> routesIn) { makeFull (name, std::move (values), std::move (routesIn), {}, false); };
    const auto pipe = [] (int t) { return pipeTargetId (t); };

    make ("01 Init", {}, {});
    make ("02 Plenum", { { "stop0", 1 }, { "stop2", 1 }, { "stop3", 1 }, { "stop5", 1 } }, {});
    make (u8 ("03 Welle durch die Pfeifen"),
          { { "stop0", 1 }, { "stop2", 1 }, { "lfo2Rate", 0.35f }, { "lfo2Spread", 1.0f }, { "lfo3Rate", 0.21f }, { "lfo3Spread", 1.0f } },
          { { srcLfo2, pipe (tgtAz), 0.5f }, { srcLfo3, pipe (tgtEl), 0.25f } });
    make ("04 Atmende Wolke",
          { { "stop1", 1 }, { "stop3", 1 }, { "env1Attack", 3.0f }, { "env1Decay", 4.0f }, { "env1Sustain", 0.4f }, { "env1Release", 4.0f },
            { "lfo4Spread", 1.0f }, { "lfo4SpreadMode", 1 }, { "lfo4Rate", 0.6f }, { "tremOn", 1 }, { "tremRate", 3.2f }, { "tremDepth", 0.35f } },
          { { srcEnv1, pipe (tgtDist), 0.4f }, { srcLfo4, pipe (tgtBright), -0.35f }, { srcLfo4, pipe (tgtLevel), -0.15f } });
    make ("05 Verfall",
          { { "stop0", 1 }, { "stop1", 1 }, { "degAmount", 0.6f }, { "degDropouts", 0.2f }, { "motion0", 3 }, { "motion1", 3 },
            { "lfo1Shape", (float) shSmooth }, { "lfo1Rate", 0.15f } },
          { { srcLfo1, "degAmount", 0.35f } });
    make (u8 ("06 Gl\xc3\xa4serne Spr\xc3\xbcnge"),
          { { "stop3", 1 }, { "stop4", 1 }, { "motion3", 5 }, { "motion4", 5 }, { "rate3", 1.5f }, { "rate4", 2.5f }, { "depth3", 90 }, { "depth4", 120 },
            { "freezeSmear", 0.8f } },
          { { srcRandom, pipe (tgtPitch), 0.01f } });
    // ---- set 2
    currentSet = 2;
    make (u8 ("07 Praestant 16\xe2\x80\xb2 Grund"), { { "stop7", 1 }, { "stop0", 1 }, { "stop1", 1 } }, {});
    make (u8 ("08 Plenum mit 16\xe2\x80\xb2"),
          { { "stop7", 1 }, { "stop0", 1 }, { "stop2", 1 }, { "stop3", 1 }, { "stop5", 1 }, { "stop6", 1 }, { "click", 0.35f } }, {});
    make (u8 ("09 Tremulant-Fl\xc3\xb6te"),
          { { "stop1", 1 }, { "stop2", 1 }, { "tremOn", 1 }, { "tremRate", 5.2f }, { "tremDepth", 0.45f } }, {});
    make ("10 Kreisende Mixtur",
          { { "stop3", 1 }, { "stop5", 1 }, { "motion3", 1 }, { "motion5", 1 }, { "rate3", 0.3f }, { "rate5", -0.22f },
            { "ellipse3", 0.5f }, { "ellipse5", 0.5f }, { "doppler", 1.0f } }, {});
    make ("11 Werckmeister-Plenum",
          { { "temperament", 2 }, { "stop0", 1 }, { "stop2", 1 }, { "stop3", 1 }, { "stop5", 1 } }, {});
    make ("12 Schwebung",
          { { "stop0", 1 }, { "stop2", 1 }, { "detune", 12.0f }, { "lfo1Rate", 0.13f }, { "lfo1Spread", 1.0f }, { "lfo1SpreadMode", 1 } },
          { { srcLfo1, pipe (tgtPitch), 0.006f } });
    make ("13 Sterbende Orgel",
          { { "stop0", 1 }, { "stop1", 1 }, { "stop2", 1 }, { "wind", 0.55f }, { "degAmount", 0.35f }, { "degWobble", 0.5f },
            { "lfo4Shape", (float) shSmooth }, { "lfo4Rate", 0.09f }, { "lfo3Shape", (float) shSmooth }, { "lfo3Rate", 0.05f } },
          { { srcLfo4, "swind0", -0.45f, true }, { srcLfo3, "swind2", -0.5f, true }, { srcLfo4, "degAmount", 0.3f, true } });
    make ("14 Eingefroren",
          { { "stop7", 1 }, { "stop1", 1 }, { "freeze", 1 }, { "freezeSmear", 0.8f }, { "freezeRelease", 6.0f },
            { "lfo2Rate", 0.04f }, { "lfo2Spread", 1.0f } },
          { { srcLfo2, pipe (tgtAz), 0.5f } });
    make (u8 ("15 Trompeten-Spr\xc3\xbcnge im Takt"),
          { { "stop6", 1 }, { "motion6", 5 }, { "rate6", 2.0f }, { "depth6", 100.0f }, { "motionSync", 1 },
            { "lfo2Shape", (float) shSquare }, { "lfo2Sync", 1 }, { "lfo2Div", 5 } },
          { { srcLfo2, pipe (tgtLevel), -0.25f, true } });
    make ("16 Glockenausklang",
          { { "stop3", 1 }, { "stop4", 1 }, { "env1Attack", 0.005f }, { "env1Decay", 2.5f }, { "env1Sustain", 0.0f }, { "env1Release", 1.0f } },
          { { srcEnv1, pipe (tgtLevel), 0.3f }, { srcEnv1, pipe (tgtBright), 0.3f } });
    make ("17 Atem im Tempo",
          { { "stop0", 1 }, { "stop1", 1 }, { "lfo1Sync", 1 }, { "lfo1Div", 2 }, { "lfo1Shape", (float) shTriangle } },
          { { srcLfo1, "swind0", -0.35f, true }, { srcLfo1, "swind1", -0.35f, true } });
    make ("18 Tiefe Wolke",
          { { "stop7", 1 }, { "stop1", 1 }, { "lfo3Shape", (float) shSmooth }, { "lfo3Rate", 0.3f }, { "lfo3Spread", 1.0f }, { "lfo3SpreadMode", 1 } },
          { { srcRandom, pipe (tgtDist), 0.4f }, { srcLfo3, pipe (tgtAz), 0.3f }, { srcRandom, pipe (tgtEl), 0.25f } });

    // ---- set 3: the room that follows the pipes, unipolar modulation, the 16', and some madness
    currentSet = 3;
    // set-2 fix: the bell attack was too hot (+16 dB); rewritten once with +11 dB
    make ("16 Glockenausklang",
          { { "stop3", 1 }, { "stop4", 1 }, { "env1Attack", 0.005f }, { "env1Decay", 2.5f }, { "env1Sustain", 0.0f }, { "env1Release", 1.0f } },
          { { srcEnv1, pipe (tgtLevel), 0.3f }, { srcEnv1, pipe (tgtBright), 0.3f } });
    make ("19 Raum folgt dem Schwarm",
          { { "stop7", 1 }, { "stop1", 1 }, { "stop2", 1 }, { "motion7", 1 }, { "motion1", 1 }, { "motion2", 1 },
            { "rate7", 0.06f }, { "rate1", -0.11f }, { "rate2", 0.17f }, { "ellipse7", 0.3f }, { "ellipse1", 0.3f }, { "ellipse2", 0.3f },
            { "irFollow", 1 }, { "irEnvelop", 0.8f }, { "irDirect", 0.1f }, { "irWet", 0.0f },
            { "lfo2Rate", 0.2f }, { "lfo2Spread", 1.0f } },
          { { srcLfo2, pipe (tgtSend), 0.3f } });
    make ("20 Kathedrale im Kopf",
          { { "stop7", 1 }, { "stop1", 1 }, { "irFollow", 0 }, { "irEnvelop", 1.0f }, { "irDirect", 0.0f }, { "irWet", 6.0f }, { "irDry", -18.0f },
            { "freeze", 1 }, { "freezeSmear", 0.9f }, { "freezeRelease", 8.0f } }, {});
    make ("21 Zentrifuge",
          { { "stop0", 1 }, { "stop2", 1 }, { "stop3", 1 }, { "stop6", 1 }, { "motion0", 1 }, { "motion2", 1 }, { "motion3", 1 }, { "motion6", 1 },
            { "rate0", 6.0f }, { "rate2", -7.5f }, { "rate3", 9.0f }, { "rate6", -10.0f },
            { "ellipse0", 0.85f }, { "ellipse2", 0.85f }, { "ellipse3", 0.85f }, { "ellipse6", 0.85f }, { "doppler", 3.0f }, { "air", 1.0f },
            { "lfo1Shape", (float) shTriangle }, { "lfo1Rate", 0.05f } },
          { { srcLfo1, "rate0", 0.3f }, { srcLfo1, "rate3", -0.3f } });
    make ("22 Zerbrochene Stimmung",
          { { "temperament", 5 }, { "detune", 45.0f }, { "stop0", 1 }, { "stop1", 1 }, { "stop2", 1 },
            { "lfo3Shape", (float) shSmooth }, { "lfo3Rate", 0.7f }, { "lfo3Spread", 1.0f }, { "lfo3SpreadMode", 1 } },
          { { srcRandom, pipe (tgtPitch), 0.08f }, { srcLfo3, pipe (tgtPitch), 0.02f } });
    make ("23 Taubenschwarm",
          { { "stop3", 1 }, { "stop5", 1 }, { "motion3", 5 }, { "motion5", 5 }, { "rate3", 8.0f }, { "rate5", 10.0f },
            { "depth3", 180.0f }, { "depth5", 180.0f }, { "doppler", 2.0f }, { "click", 0.6f },
            { "lfo4Shape", (float) shSmooth }, { "lfo4Rate", 1.3f }, { "lfo4Spread", 1.0f }, { "lfo4SpreadMode", 1 } },
          { { srcLfo4, pipe (tgtDist), 0.6f } });
    make ("24 Absturz",
          { { "stop0", 1 }, { "stop7", 1 }, { "stop6", 1 },
            { "env1Delay", 0.4f }, { "env1Attack", 6.0f }, { "env1Decay", 1.0f }, { "env1Sustain", 1.0f }, { "env1Release", 0.5f } },
          { { srcEnv1, pipe (tgtPitch), -1.0f }, { srcEnv1, pipe (tgtBright), -0.4f }, { srcEnv1, "degAmount", 0.6f } });
    make ("25 Atem-Maschine",
          { { "stop0", 1 }, { "stop1", 1 }, { "stop2", 1 }, { "stop4", 1 }, { "motionSync", 1 },
            { "lfo1Shape", (float) shSquare }, { "lfo1Sync", 1 }, { "lfo1Div", 5 },
            { "lfo2Shape", (float) shSquare }, { "lfo2Sync", 1 }, { "lfo2Div", 6 } },
          { { srcLfo1, "swind0", -1.0f, true }, { srcLfo1, "swind1", -1.0f, true },
            { srcLfo2, "swind2", -1.0f, true }, { srcLfo2, "swind4", -1.0f, true } });
    make (u8 ("26 16\xe2\x80\xb2 Erdbeben"),
          { { "stop7", 1 }, { "stop6", 1 }, { "vol7", 4.0f }, { "wind", 0.95f },
            { "lfo1Rate", 7.0f }, { "lfo1Spread", 1.0f }, { "lfo1SpreadMode", 1 } },
          { { srcLfo1, pipe (tgtPitch), 0.004f }, { srcLfo1, pipe (tgtLevel), 0.15f } });
    make ("27 Kirche dreht sich",
          { { "stop0", 1 }, { "stop1", 1 }, { "stop2", 1 }, { "stop3", 1 },
            { "motion0", 4 }, { "motion1", 4 }, { "motion2", 4 }, { "motion3", 4 },
            { "rate0", 0.07f }, { "rate1", 0.09f }, { "rate2", 0.11f }, { "rate3", 0.13f },
            { "irFollow", 1 }, { "irEnvelop", 0.7f }, { "lfo3Rate", 0.1f }, { "lfo3Spread", 1.0f } },
          { { srcLfo3, pipe (tgtEl), 0.3f } });
    make ("28 Gefrorener Sturm",
          { { "stop1", 1 }, { "stop3", 1 }, { "stop5", 1 }, { "freeze", 1 }, { "freezeSmear", 1.0f }, { "freezeRelease", 10.0f },
            { "wind", 1.0f }, { "motion1", 5 }, { "motion3", 5 }, { "motion5", 5 }, { "rate1", 4.0f }, { "rate3", 4.0f }, { "rate5", 4.0f },
            { "depth1", 160.0f }, { "depth3", 160.0f }, { "depth5", 160.0f }, { "degAmount", 0.5f }, { "degDropouts", 0.4f } }, {});
    make ("29 Bitcrush-Liturgie",
          { { "temperament", 2 }, { "stop0", 1 }, { "stop2", 1 }, { "stop6", 1 },
            { "degAmount", 0.9f }, { "degBits", 5.0f }, { "degRate", 3000.0f },
            { "lfo1Shape", (float) shSteps }, { "lfo1Sync", 1 }, { "lfo1Div", 6 },
            { "lfo2Shape", (float) shSteps }, { "lfo2Sync", 1 }, { "lfo2Div", 5 }, { "lfo2Spread", 1.0f }, { "lfo2SpreadMode", 1 } },
          { { srcLfo1, "degRate", 0.4f, true }, { srcLfo1, "degBits", 0.3f, true }, { srcLfo2, pipe (tgtAz), 0.5f } });
    make ("30 Pendel im Takt",
          { { "stop1", 1 }, { "stop2", 1 }, { "motionSync", 1 }, { "motion1", 2 }, { "motion2", 2 },
            { "rate1", 0.5f }, { "rate2", 1.0f }, { "depth1", 90.0f }, { "depth2", 90.0f },
            { "lfo1Shape", (float) shTriangle }, { "lfo1Sync", 1 }, { "lfo1Div", 4 }, { "lfo1Spread", 1.0f } },
          { { srcLfo1, pipe (tgtBright), 0.4f } });
    make ("31 Mechanik pur",
          { { "stop1", 1 }, { "vol1", -14.0f }, { "click", 1.0f }, { "releaseClick", 1.0f }, { "clickBlend", 1.0f },
            { "stopPull", 1.0f }, { "distAmount", 1.0f } }, {});
    make ("32 Glissando-Wolke",
          { { "stop7", 1 }, { "stop1", 1 }, { "stop2", 1 }, { "stop3", 1 }, { "tremOn", 1 }, { "tremRate", 2.0f }, { "tremDepth", 0.3f },
            { "irEnvelop", 0.9f }, { "lfo4Shape", (float) shSmooth }, { "lfo4Rate", 0.15f }, { "lfo4Spread", 1.0f }, { "lfo4SpreadMode", 1 } },
          { { srcLfo4, pipe (tgtPitch), 0.05f }, { srcLfo4, pipe (tgtAz), 0.4f } });
    make ("33 Ferne Prozession",
          { { "stop0", 1 }, { "stop7", 1 }, { "stop1", 1 }, { "air", 1.0f }, { "irFollow", 1 }, { "irEnvelop", 0.8f }, { "irDirect", 0.0f },
            { "env2Mode", 1 }, { "env2Attack", 8.0f }, { "env2Decay", 1.0f }, { "env2Sustain", 1.0f }, { "env2Release", 3.0f } },
          { { srcEnv2, pipe (tgtDist), 0.7f }, { srcEnv2, pipe (tgtSend), 0.3f } });
    make ("34 Doppler-Karussell",
          { { "stop2", 1 }, { "stop3", 1 }, { "motionSync", 1 }, { "motion2", 1 }, { "motion3", 1 }, { "rate2", 2.0f }, { "rate3", -4.0f },
            { "ellipse2", 0.9f }, { "ellipse3", 0.9f }, { "doppler", 4.0f },
            { "lfo2Shape", (float) shSquare }, { "lfo2Sync", 1 }, { "lfo2Div", 2 } },
          { { srcLfo2, "ellipse2", 0.5f, true } });

    // ---- set 4: the effects page (sounds that are easy to place)
    currentSet = 4;
    make ("35 Werkstatt der Orgelbauer",
          { { "stop1", 1 }, { "stop2", 1 }, { "wind", 0.2f }, { "fxCustLevel", 0.6f }, { "fxCustPlace", 1 }, { "fxCustScatter", 0.6f },
            { "fxCustUp", 0.5f }, { "fxPathLevel", 0.5f }, { "fxLeakLevel", 0.3f }, { "fxLeakCount", 4 } }, {});
    make ("36 Abstrakten-Ballett",
          { { "stop0", 1 }, { "stop2", 1 }, { "stop5", 1 }, { "click", 0.1f }, { "fxPathLevel", 0.9f }, { "fxPathTime", 0.45f },
            { "fxPathTicks", 14 }, { "fxPathSpread", 60.0f }, { "fxPathWait", 1 },
            { "motion0", 1 }, { "motion2", 1 }, { "motion5", 1 }, { "rate0", 0.05f }, { "rate2", -0.07f }, { "rate5", 0.09f } }, {});
    make ("37 Glut im Holz",
          { { "stop1", 1 }, { "stop7", 1 }, { "fxCrackLevel", 0.7f }, { "fxCrackRate", 25.0f }, { "fxCrackLen", 0.4f },
            { "motion1", 3 }, { "motion7", 3 }, { "depth1", 70.0f }, { "depth7", 50.0f }, { "air", 0.8f } }, {});
    make ("38 Altes Holz knackt",
          { { "stop0", 1 }, { "stop1", 1 }, { "fxCrackLevel", 1.0f }, { "fxCrackRate", 1.5f }, { "fxCrackLen", 6.0f },
            { "degAmount", 0.25f }, { "degWobble", 0.4f } }, {});
    make ("39 Undichte Lade",
          { { "stop0", 1 }, { "stop1", 1 }, { "swind0", 0.85f }, { "wind", 0.35f }, { "fxWindRound", 1 },
            { "fxLeakLevel", 0.7f }, { "fxLeakCount", 6 }, { "fxLeakFreq", 5500.0f }, { "fxLeakWander", 8.0f } }, {});
    make ("40 Schlangengrube",
          { { "stop4", 1 }, { "stop1", 1 }, { "wind", 0.2f }, { "fxWindRound", 1 },
            { "fxLeakLevel", 1.0f }, { "fxLeakCount", 8 }, { "fxLeakFreq", 7000.0f }, { "fxLeakWander", 60.0f },
            { "lfo1Shape", (float) shSmooth }, { "lfo1Rate", 0.4f } },
          { { srcLfo1, "fxLeakFreq", 0.35f } });
    make ("41 Gespaltene Pfeifen",
          { { "stop0", 1 }, { "stop2", 1 }, { "stop3", 1 }, { "stop5", 1 }, { "fxPartSpread", 120.0f }, { "fxPartSplit", 2.0f } }, {});
    make (u8 ("42 Obertonkarussell"),
          { { "stop1", 1 }, { "stop4", 1 }, { "fxPartSpread", 90.0f }, { "fxPartSplit", 3.0f }, { "fxPartTurn", 0.5f },
            { "lfo3Shape", (float) shSmooth }, { "lfo3Rate", 0.08f } },
          { { srcLfo3, "fxPartTurn", 0.6f } });
    make ("43 Echo-Kompass",
          { { "stop6", 1 }, { "stop2", 1 }, { "fxEchoLevel", 0.7f }, { "fxEchoTime", 0.5f }, { "fxEchoTurn", 90.0f },
            { "fxEchoRepeats", 4 }, { "fxEchoFeedback", 0.7f } }, {});
    make ("44 Spirale nach oben",
          { { "stop3", 1 }, { "stop5", 1 }, { "fxEchoLevel", 0.8f }, { "fxEchoTime", 0.18f }, { "fxEchoTurn", 45.0f }, { "fxEchoTilt", 12.0f },
            { "fxEchoRepeats", 8 }, { "fxEchoFeedback", 0.85f }, { "fxEchoDamp", 0.15f } }, {});
    make ("45 Tempo-Echos im Kreis",
          { { "stop6", 1 }, { "click", 0.4f }, { "motionSync", 1 }, { "fxEchoLevel", 0.75f }, { "fxEchoSync", 1 }, { "fxEchoDiv", 5 },
            { "fxEchoRepeats", 8 }, { "fxEchoTurn", 135.0f }, { "fxEchoFeedback", 0.75f } }, {});
    make ("46 Schrottplatz-Liturgie",
          { { "stop7", 1 }, { "stop6", 1 }, { "fxCustLevel", 1.0f }, { "fxCustPlace", 3 }, { "fxCustWander", 240.0f }, { "fxCustScatter", 1.0f },
            { "fxCustPitch", 1.0f }, { "fxCustUp", 1.0f }, { "fxCrackLevel", 0.5f }, { "fxCrackRate", 60.0f }, { "fxCrackLen", 0.3f },
            { "fxEchoLevel", 0.5f }, { "fxEchoTurn", -120.0f }, { "fxEchoTime", 0.27f }, { "degAmount", 0.4f } }, {});
    make ("47 Geisterorgel",
          { { "stop1", 1 }, { "wind", 0.4f }, { "fxWindRound", 1 }, { "fxPathLevel", 0.4f }, { "fxPathTime", 0.6f }, { "fxPathWait", 1 },
            { "fxLeakLevel", 0.4f }, { "fxEchoLevel", 0.6f }, { "fxEchoTilt", 30.0f }, { "fxEchoTurn", 160.0f }, { "fxEchoTime", 0.7f },
            { "fxEchoDamp", 0.6f }, { "fxPartSpread", 180.0f }, { "fxPartTurn", -0.2f }, { "irEnvelop", 0.9f }, { "air", 1.0f } }, {});
    make ("48 Uhrwerk",
          { { "stop3", 1 }, { "motionSync", 1 }, { "fxPathLevel", 0.8f }, { "fxPathTicks", 16 }, { "fxPathSpread", 90.0f }, { "fxPathTime", 0.5f },
            { "fxCustLevel", 0.6f }, { "fxCustPlace", 2 }, { "fxCustPitch", 1.0f }, { "fxCustUp", 1.0f },
            { "fxEchoLevel", 0.6f }, { "fxEchoSync", 1 }, { "fxEchoDiv", 6 }, { "fxEchoTurn", 30.0f }, { "fxEchoRepeats", 8 }, { "fxEchoFeedback", 0.9f } }, {});

    // ---- the simulated, playable room ("Saal"); set 6 = the same patches with levels for the convolution tail
    currentSet = 6;
    make ("49 Atmender Saal",
          { { "stop0", 1 }, { "stop1", 1 }, { "stop2", 1 }, { "simOn", 1 }, { "simW", 12.0f }, { "simL", 28.0f }, { "simH", 11.0f },
            { "lfo1Rate", 0.06f } },
          { { srcLfo1, "simSize", 0.3f } });
    make (u8 ("50 Wandernder H\xc3\xb6rer"),
          { { "stop1", 1 }, { "stop3", 1 }, { "simOn", 1 }, { "simW", 20.0f }, { "simL", 40.0f }, { "simH", 12.0f }, { "simAbs", 0.04f },
            { "lfo2Rate", 0.05f }, { "lfo3Rate", 0.031f }, { "lfo3Shape", (float) shTriangle } },
          { { srcLfo2, "simLx", 0.45f }, { srcLfo3, "simLy", 0.45f } });
    make ("51 Spiegelkabinett",
          { { "stop3", 1 }, { "stop5", 1 }, { "simOn", 1 }, { "simW", 6.0f }, { "simL", 6.0f }, { "simH", 6.0f }, { "simAbs", 0.0f },
            { "simAbsFloor", 0.0f }, { "simHigh", 0.0f }, { "simScatter", 0.05f }, { "simOrder", 2 }, { "simTail", -8.0f }, { "simEr", -6.0f }, { "click", 0.5f }, { "master", -20.0f } }, {});
    make ("52 Kathedrale",
          { { "stop7", 1 }, { "stop0", 1 }, { "stop1", 1 }, { "stop6", 1 }, { "simOn", 1 }, { "simW", 30.0f }, { "simL", 90.0f }, { "simH", 32.0f },
            { "simAbs", 0.04f }, { "simAbsFloor", 0.1f }, { "simHigh", 0.25f }, { "simScatter", 0.5f }, { "simLy", 0.3f } }, {});
    make ("53 Flatterflur",
          { { "stop6", 1 }, { "click", 0.6f }, { "releaseClick", 0.5f }, { "simOn", 1 }, { "simW", 2.5f }, { "simL", 60.0f }, { "simH", 3.0f },
            { "simAbs", 0.02f }, { "simAbsFloor", 0.08f }, { "simScatter", 0.02f }, { "simLy", 0.2f }, { "simEr", -3.0f }, { "master", -19.0f } }, {});
    make ("54 Schrumpfender Raum",
          { { "stop0", 1 }, { "stop1", 1 }, { "simOn", 1 }, { "simSize", 2.5f }, { "simAbs", 0.03f }, { "master", -18.0f },
            { "env1Mode", 1 }, { "env1Attack", 8.0f }, { "env1Decay", 1.0f }, { "env1Sustain", 1.0f }, { "env1Release", 4.0f } },
          { { srcEnv1, "simSize", -0.75f } });
    make (u8 ("55 Glasw\xc3\xa4nde, Samtboden"),
          { { "stop2", 1 }, { "stop4", 1 }, { "motion2", 1 }, { "motion4", 1 }, { "rate2", 0.2f }, { "rate4", -0.13f },
            { "simOn", 1 }, { "simW", 16.0f }, { "simL", 22.0f }, { "simH", 9.0f }, { "simAbs", 0.0f }, { "simAbsFloor", 0.9f },
            { "simHigh", 0.05f }, { "simScatter", 0.03f }, { "master", -20.0f } }, {});

    // ---- set 7: wind model "instabil" (starving pipes whistle and stutter)
    currentSet = 7;
    make ("56 Sterbender Wind",
          { { "windModel", 1 }, { "stop0", 1 }, { "stop2", 1 }, { "swind0", 0.6f }, { "swind2", 0.5f },
            { "lfo1Rate", 0.035f }, { "lfo2Shape", (float) shSmooth }, { "lfo2Rate", 0.09f } },
          { { srcLfo1, "swind0", 0.45f }, { srcLfo2, "swind2", 0.4f } });
    currentSet = 8;          // set 8: steady wind (the edge tones stand still)
    make (u8 ("57 Pfeifende Gedackte"),
          { { "windModel", 1 }, { "stop1", 1 }, { "swind1", 0.2f }, { "master", -6.0f } }, {});
    currentSet = 7;
    make (u8 ("58 Erlahmendes Gebl\xc3\xa4se"),
          { { "windModel", 1 }, { "stop0", 1 }, { "stop1", 1 }, { "stop2", 1 }, { "stop3", 1 },
            { "env1Mode", 1 }, { "env1Delay", 1.0f }, { "env1Attack", 6.0f }, { "env1Decay", 1.0f }, { "env1Sustain", 1.0f }, { "env1Release", 1.5f } },
          { { srcEnv1, "swind0", -0.88f }, { srcEnv1, "swind1", -0.85f }, { srcEnv1, "swind2", -0.9f }, { srcEnv1, "swind3", -0.8f } });

    // ---- set 9: Lucier, expression (MPE / aftertouch), scenes, the audio input
    currentSet = 9;
    const auto pipes = [] (int t) { return pipeTargetId (t); };
    make (u8 ("59 Lucier \xe2\x80\x93 Ich sitze in einem Raum"),
          { { "stop1", 1 }, { "lucOn", 1 }, { "lucGen", 8.0f }, { "lucMix", 0.6f }, { "lucTime", 0.4f },
            { "simW", 9.0f }, { "simL", 12.0f }, { "simH", 4.0f } }, {});
    make (u8 ("60 Lucier \xe2\x80\x93 Wachsende Generationen"),
          { { "stop0", 0 }, { "stop1", 1 }, { "stop2", 1 }, { "lucOn", 1 }, { "lucGen", 1.0f }, { "lucGrow", 1.5f }, { "lucMix", 0.75f } }, {});
    make (u8 ("61 Lucier \xe2\x80\x93 Kammer aus Glas"),
          { { "stop0", 0 }, { "stop3", 1 }, { "stop5", 1 }, { "lucOn", 1 }, { "lucGen", 22.0f }, { "lucMix", 0.85f }, { "lucTime", 0.8f },
            { "simW", 3.2f }, { "simL", 4.1f }, { "simH", 2.6f }, { "simAbs", 0.02f }, { "simAbsFloor", 0.02f } }, {});
    make ("62 Lucier atmet",
          { { "stop1", 1 }, { "stop7", 1 }, { "lucOn", 1 }, { "lucGen", 10.0f }, { "lucMix", 0.5f },
            { "lfo1Shape", (float) shSmooth }, { "lfo1Rate", 0.05f } },
          { { srcLfo1, "lucMix", 0.35f } });
    make ("63 Lucier im Dom",
          { { "stop7", 1 }, { "stop1", 1 }, { "simOn", 1 }, { "simW", 30.0f }, { "simL", 90.0f }, { "simH", 32.0f }, { "simAbs", 0.04f },
            { "lucOn", 1 }, { "lucGen", 5.0f }, { "lucMix", 0.45f } }, {});

    // expression: MPE pressure / slide - polyphonic and channel aftertouch count as pressure too
    make (u8 ("64 Druck \xe2\x80\x93 Schwellwerk"),
          { { "stop1", 1 }, { "stop2", 1 }, { "vol0", -12.0f }, { "vol1", -12.0f }, { "vol2", -12.0f } },
          { { srcPressure, pipes (tgtLevel), 0.33f }, { srcPressure, pipes (tgtBright), 0.3f } });
    make (u8 ("65 Gleiten \xe2\x80\x93 Pfeifen wandern"),
          { { "stop0", 0 }, { "stop1", 1 }, { "stop3", 1 } },
          { { srcSlide, pipes (tgtAz), 0.5f }, { srcPressure, pipes (tgtDist), -0.35f } });
    make (u8 ("66 Druck \xc3\xb6" "ffnet den Wind"),
          { { "windModel", 1 }, { "stop1", 1 }, { "swind0", 0.35f }, { "swind1", 0.35f } },
          { { srcPressure, "swind0", 0.65f }, { srcPressure, "swind1", 0.65f } });
    make ("67 Vibrato unter dem Finger",
          { { "stop2", 1 }, { "tremOn", 1 }, { "tremDepth", 0.0f }, { "tremRate", 5.5f } },
          { { srcPressure, "tremDepth", 0.8f }, { srcSlide, pipes (tgtPitch), 0.02f } });

    // scenes: "Ueberblenden" on the Spieltisch (or a fader / automation) moves from A to D
    makeFull (u8 ("68 Szenen \xe2\x80\x93 Vom Fl\xc3\xbcstern zum Plenum"),
              { { "stop0", 0 }, { "stop1", 1 }, { "vol1", -6.0f }, { "rate2", 0.08f } }, {},
              { {},
                { { "stop2", 1 }, { "motion2", 1 }, { "vol1", -3.0f } },
                { { "stop0", 1 }, { "stop2", 1 }, { "stop3", 1 }, { "stop7", 1 }, { "motion2", 1 }, { "vol1", 0.0f } },
                { { "stop0", 1 }, { "stop2", 1 }, { "stop3", 1 }, { "stop7", 1 }, { "stop5", 1 }, { "stop6", 1 }, { "motion2", 1 }, { "vol1", 0.0f },
                  { "click", 0.4f }, { "irEnvelop", 0.9f } } }, false);
    makeFull (u8 ("69 Szenen \xe2\x80\x93 Der Raum \xc3\xb6" "ffnet sich"),
              { { "stop1", 1 }, { "stop2", 1 }, { "simOn", 1 }, { "simW", 3.0f }, { "simL", 4.0f }, { "simH", 2.5f }, { "simAbs", 0.3f }, { "master", -21.0f } }, {},
              { {},
                { { "simW", 10.0f }, { "simL", 16.0f }, { "simH", 6.0f }, { "simAbs", 0.1f } },
                { { "simW", 22.0f }, { "simL", 45.0f }, { "simH", 15.0f }, { "simAbs", 0.05f } },
                { { "simW", 30.0f }, { "simL", 90.0f }, { "simH", 32.0f }, { "simAbs", 0.03f }, { "lucOn", 1 }, { "lucGen", 4.0f }, { "lucMix", 0.4f } } }, false);
    makeFull (u8 ("70 Szenen \xe2\x80\x93 Wanderung von selbst"),
              { { "stop1", 1 }, { "lfo1Shape", (float) shTriangle }, { "lfo1Rate", 0.02f } },
              { { srcLfo1, "sceneMorph", 1.0f, true } },
              { {},
                { { "motion0", 1 }, { "motion1", 1 }, { "rate0", 0.2f }, { "rate1", -0.15f }, { "ellipse0", 0.5f }, { "ellipse1", 0.5f } },
                { { "motion0", 1 }, { "motion1", 1 }, { "rate0", 0.35f }, { "rate1", -0.3f }, { "stop3", 1 },
                  { "fxEchoLevel", 0.6f }, { "fxEchoTurn", 120.0f } },
                { { "stop0", 0 }, { "stop7", 1 }, { "stop3", 1 }, { "tremOn", 1 }, { "tremDepth", 0.4f }, { "degAmount", 0.4f }, { "degWobble", 0.5f },
                  { "motion1", 2 }, { "rate1", 0.1f } } }, false);
    makeFull (u8 ("71 Szenen \xe2\x80\x93 Verfall in vier Stufen"),
              { { "stop1", 1 }, { "stop2", 1 } }, {},
              { {},
                { { "degAmount", 0.3f }, { "degWobble", 0.4f } },
                { { "degAmount", 0.6f }, { "degBits", 6.0f }, { "degRate", 8000.0f }, { "wind", 0.4f }, { "degWobble", 0.4f } },
                { { "degAmount", 0.9f }, { "degDropouts", 0.5f }, { "degBits", 4.0f }, { "degRate", 3000.0f }, { "wind", 0.7f }, { "degWobble", 0.6f },
                  { "windModel", 1 } } }, false);

    // the audio input (switches the input on): voice, instrument, anything - placed and moved with the pipes
    makeFull (u8 ("72 Eingang \xe2\x80\x93 Stimme kreist"),
              { { "stop0", 0 }, { "stop1", 1 }, { "vol1", -10.0f }, { "inpMode", 0 }, { "inpSpin", 25.0f }, { "inpDist", 3.5f }, { "inpEl", 15.0f },
                { "irEnvelop", 0.6f } }, {}, {}, true);
    makeFull (u8 ("73 Eingang \xe2\x80\x93 Auf der Mixtur"),
              { { "stop0", 0 }, { "stop2", 1 }, { "stop5", 1 }, { "motion5", 1 }, { "rate5", 0.12f }, { "ellipse5", 0.5f },
                { "inpFollow", 6 }, { "inpWidth", 120.0f } }, {}, {}, true);
    makeFull (u8 ("74 Eingang \xe2\x80\x93 Ich sitze in einem Raum"),
              { { "stop0", 0 }, { "inpMode", 0 }, { "inpDist", 2.0f }, { "lucOn", 1 }, { "lucGen", 1.0f }, { "lucGrow", 2.0f }, { "lucMix", 0.9f },
                { "simW", 6.0f }, { "simL", 8.0f }, { "simH", 3.0f } }, {}, {}, true);
    makeFull (u8 ("75 Eingang \xe2\x80\x93 Tasten \xc3\xb6" "ffnen die Stimme"),
              { { "vol0", -8.0f }, { "motion0", 2 }, { "rate0", 0.3f }, { "depth0", 60.0f }, { "inpFollow", 1 }, { "inpGate", 1 }, { "inpRelease", 0.6f },
                { "fxEchoLevel", 0.5f }, { "fxEchoTurn", 90.0f } }, {}, {}, true);
    makeFull ("76 Eingang steuert die Orgel",
              { { "stop1", 1 }, { "stop2", 1 }, { "swind0", 0.4f }, { "swind1", 0.4f }, { "swind2", 0.4f }, { "inpLevel", -6.0f } },
              { { srcInput, "swind0", 0.6f }, { srcInput, "swind1", 0.6f }, { srcInput, "swind2", 0.6f }, { srcInput, pipes (tgtBright), 0.4f } }, {}, true);
    makeFull ("77 Eingang im Spiegelsaal",
              { { "stop0", 0 }, { "stop3", 1 }, { "vol3", -12.0f }, { "inpSpin", -40.0f }, { "simOn", 1 }, { "simW", 6.0f }, { "simL", 6.0f }, { "simH", 6.0f },
                { "simAbs", 0.0f }, { "simAbsFloor", 0.0f }, { "simHigh", 0.0f }, { "simScatter", 0.05f }, { "master", -18.0f } }, {}, {}, true);
    makeFull (u8 ("78 Eingang \xe2\x80\x93 Echo-Kompass"),
              { { "stop0", 0 }, { "stop1", 1 }, { "vol1", -14.0f }, { "inpMode", 0 }, { "fxEchoLevel", 0.7f }, { "fxEchoTurn", 90.0f },
                { "fxEchoRepeats", 6 }, { "fxEchoFeedback", 0.7f }, { "fxEchoTilt", 15.0f } }, {}, {}, true);
    makeFull (u8 ("79 Eingang zerf\xc3\xa4llt"),
              { { "stop0", 0 }, { "stop7", 1 }, { "vol7", -10.0f }, { "degMode", 1 }, { "degAmount", 0.6f }, { "degBits", 6.0f }, { "degRate", 6000.0f },
                { "degDropouts", 0.3f }, { "inpWidth", 150.0f } }, {}, {}, true);

    // ---- set 10: Lucier now has two ways - the patches made for the live way say so; 74 is the piece itself
    currentSet = 10;
    make (u8 ("59 Lucier \xe2\x80\x93 Ich sitze in einem Raum"),
          { { "stop1", 1 }, { "lucOn", 1 }, { "lucMode", 1 }, { "lucGen", 8.0f }, { "lucMix", 0.6f }, { "lucTime", 0.4f },
            { "simW", 9.0f }, { "simL", 12.0f }, { "simH", 4.0f } }, {});
    make (u8 ("60 Lucier \xe2\x80\x93 Wachsende Generationen"),
          { { "stop0", 0 }, { "stop1", 1 }, { "stop2", 1 }, { "lucOn", 1 }, { "lucMode", 1 }, { "lucGen", 1.0f }, { "lucGrow", 1.5f }, { "lucMix", 0.75f } }, {});
    make (u8 ("61 Lucier \xe2\x80\x93 Kammer aus Glas"),
          { { "stop0", 0 }, { "stop3", 1 }, { "stop5", 1 }, { "lucOn", 1 }, { "lucMode", 1 }, { "lucGen", 22.0f }, { "lucMix", 0.85f }, { "lucTime", 0.8f },
            { "simW", 3.2f }, { "simL", 4.1f }, { "simH", 2.6f }, { "simAbs", 0.02f }, { "simAbsFloor", 0.02f } }, {});
    make ("62 Lucier atmet",
          { { "stop1", 1 }, { "stop7", 1 }, { "lucOn", 1 }, { "lucMode", 1 }, { "lucGen", 10.0f }, { "lucMix", 0.5f },
            { "lfo1Shape", (float) shSmooth }, { "lfo1Rate", 0.05f } },
          { { srcLfo1, "lucMix", 0.35f } });
    make ("63 Lucier im Dom",
          { { "stop7", 1 }, { "stop1", 1 }, { "simOn", 1 }, { "simW", 30.0f }, { "simL", 90.0f }, { "simH", 32.0f }, { "simAbs", 0.04f },
            { "lucOn", 1 }, { "lucMode", 1 }, { "lucGen", 5.0f }, { "lucMix", 0.45f } }, {});
    makeFull (u8 ("69 Szenen \xe2\x80\x93 Der Raum \xc3\xb6" "ffnet sich"),
              { { "stop1", 1 }, { "stop2", 1 }, { "simOn", 1 }, { "simW", 3.0f }, { "simL", 4.0f }, { "simH", 2.5f }, { "simAbs", 0.3f }, { "master", -21.0f },
                { "lucMode", 1 } }, {},
              { {},
                { { "simW", 10.0f }, { "simL", 16.0f }, { "simH", 6.0f }, { "simAbs", 0.1f } },
                { { "simW", 22.0f }, { "simL", 45.0f }, { "simH", 15.0f }, { "simAbs", 0.05f } },
                { { "simW", 30.0f }, { "simL", 90.0f }, { "simH", 32.0f }, { "simAbs", 0.03f }, { "lucOn", 1 }, { "lucGen", 4.0f }, { "lucMix", 0.4f } } }, false);
    // the piece: press "Aufnehmen" on the Saal page, speak about a minute, press again - 32 generations follow by themselves
    makeFull (u8 ("74 Eingang \xe2\x80\x93 Ich sitze in einem Raum"),
              { { "stop0", 0 }, { "inpMode", 0 }, { "inpDist", 2.0f }, { "lucOn", 1 }, { "lucMode", 0 }, { "lucGen", 32.0f }, { "lucGap", 1.5f },
                { "lucMix", 0.85f }, { "lucRecSrc", 1 }, { "simW", 6.0f }, { "simL", 8.0f }, { "simH", 3.0f } }, {}, {}, true);
    makeFull (u8 ("80 Lucier \xe2\x80\x93 St\xc3\xbc" "ck f\xc3\xbcr Orgel"),
              { { "stop1", 1 }, { "stop2", 1 }, { "lucOn", 1 }, { "lucMode", 0 }, { "lucGen", 24.0f }, { "lucGap", 2.0f }, { "lucMix", 0.7f },
                { "simW", 12.0f }, { "simL", 26.0f }, { "simH", 10.0f }, { "simAbs", 0.05f } }, {}, {}, false);

    marker.replaceWithText (juce::String (kFactoryVersion));
}

juce::AudioProcessorEditor* RysumProcessor::createEditor() { return new RysumEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RysumProcessor(); }
