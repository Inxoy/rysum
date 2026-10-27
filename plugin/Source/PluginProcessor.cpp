#include "PluginProcessor.h"
#include "PluginEditor.h"

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
                                                   StringArray { "Stereo", "AmbiX 1", "AmbiX 2", "AmbiX 3", "AmbiX 4", "AmbiX 5", "Binaural" }, 0));
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
                                                      NormalisableRange<float> (-10.0f, 10.0f, 0.001f, 0.3f, true), 0.08f,
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
    return l;
}

RysumProcessor::RysumProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::ambisonic (kMaxOrder), true)),
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
    }
    ix.doppler = I ("doppler"); ix.air = I ("air"); ix.distAmount = I ("distAmount"); ix.temperament = I ("temperament");
    ix.pitch = I ("pitch"); ix.detune = I ("detune"); ix.tremOn = I ("tremOn"); ix.tremRate = I ("tremRate"); ix.tremDepth = I ("tremDepth");
    ix.freeze = I ("freeze"); ix.freezeSmear = I ("freezeSmear"); ix.freezeRelease = I ("freezeRelease"); ix.motionSync = I ("motionSync");
    ix.master = I ("master"); ix.output = I ("output"); ix.click = I ("click"); ix.clickBlend = I ("clickBlend");
    ix.releaseClick = I ("releaseClick"); ix.stopPull = I ("stopPull"); ix.wind = I ("wind");
    ix.degAmount = I ("degAmount"); ix.degGrain = I ("degGrain"); ix.degBits = I ("degBits"); ix.degRate = I ("degRate");
    ix.degWobble = I ("degWobble"); ix.degDropouts = I ("degDropouts"); ix.degMode = I ("degMode");
    ix.irDry = I ("irDry"); ix.irWet = I ("irWet"); ix.binYaw = I ("binYaw"); ix.windModel = I ("windModel");
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
    engine.setModHook (this);

    // MIDI controller assignments: every new instance starts with the ones saved last (Documents/Rysum/MIDI-Regler.xml);
    // a saved project / session brings its own
    if (auto xml = juce::XmlDocument::parse (midiMapFile()))
        applyMidiMapState (juce::ValueTree::fromXml (*xml));
    savedMapVersion = midiMapVersion.load();

    startTimer (250);   // first tick: find + load the samples (after a possible setStateInformation)
}

RysumProcessor::~RysumProcessor()
{
    aliveFlag->store (false);
    stopTimer();
    loader.reset();
    irPool.removeAllJobs (true, 10000);
}

bool RysumProcessor::isBusesLayoutSupported (const BusesLayout& busLayout) const
{
    if (busLayout.getMainInputChannels() > 0) return false;
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
    }
    p.doppler = V (ix.doppler);
    p.air = V (ix.air);
    p.distAmount = V (ix.distAmount);
    p.tuning = tunings[(size_t) juce::jlimit (0, (int) tunings.size() - 1, juce::roundToInt (V (ix.temperament)))];   // precomputed (UI + audio thread)
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

    auto S = [&] (int k) { return V (ix.sim[(size_t) k]); };
    auto& r = p.room;
    r.on = S (simOn) > 0.5f;
    r.width = S (simW); r.length = S (simL); r.height = S (simH); r.size = S (simSize);
    r.listenerX = S (simLx); r.listenerY = S (simLy);
    r.absWalls = S (simAbs); r.absFloor = S (simAbsFloor); r.highDamp = S (simHigh); r.scatter = S (simScatter);
    r.order = 1 + juce::roundToInt (S (simOrder));
    r.erGain = S (simEr) <= -59.9f ? 0.0f : dbToGain (S (simEr));
    r.tailGain = S (simTail) <= -59.9f ? 0.0f : dbToGain (S (simTail));
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
    // (setSize with avoidReallocating: no allocation as long as the block fits what prepareToPlay reserved)
    if (binaural) binBuf.setSize (binActive->getNumChannels(), n, false, false, true);
    juce::AudioBuffer<float>& work = binaural ? binBuf : buffer;

    engine.process (work, midi, params);
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
    return samplesFolder != juce::File() ? SampleLibrary::customFolder (samplesFolder) : juce::File();
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
    irPool.addJob ([this, old, folder, alive = aliveFlag]
    {
        auto lib = std::make_shared<SampleLibrary> (*old);       // pipes / clicks are shared, only the own sounds are new
        lib->custom = SampleLibrary::loadCustom (folder);
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
    }
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
    if (! forPatch) extra.setProperty ("sofaFile", sofaFile.getFullPathName(), nullptr);
    extra.setProperty ("patch", patchName, nullptr);
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
    const float keepBin[] { binOrderParam->load(), binYawParam->load(), binDfParam->load() };
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
        // so do the headphone settings
        int bi = 0;
        for (auto* id : { "binOrder", "binYaw", "binDfEq" })
            if (auto* o = apvts.getParameter (id)) o->setValueNotifyingHost (o->convertTo0to1 (keepBin[bi++]));
        // stops the patch does not describe get their default layout
        for (int s = 0; s < kNumStops; ++s)
        {
            bool found = false;
            for (auto st : extra) found = found || (st.hasType ("STOP") && stopIndexForId (st["id"].toString()) == s);
            if (! found)
            {
                layouts[(size_t) s] = defaultLayout (s);
                applyLayout (s);
                for (auto& m : engine.muted[(size_t) s]) m = false;
            }
        }
    }
    patchName = extra["patch"].toString();

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
        if (! forPatch)
        {
            const auto mm = extra.getChildWithName ("MIDIMAP");
            if (mm.isValid()) applyMidiMapState (mm);
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

int RysumProcessor::learnMidiFor (int paramIndex)
{
    if (getParamAt (paramIndex) == nullptr) return -1;
    for (int i = 0; i < kNumMidiMaps; ++i)
        if (! getMidiMap (i).used())
        {
            MidiMap m;
            m.param = paramIndex;
            // switches: a button press flips them
            m.toggle = dynamic_cast<juce::AudioParameterBool*> (getParamAt (paramIndex)) != nullptr;
            setMidiMap (i, m);
            learnMidi (i);
            return i;
        }
    return -1;
}

juce::Array<int> RysumProcessor::midiMapsFor (int paramIndex) const
{
    juce::Array<int> r;
    for (int i = 0; i < kNumMidiMaps; ++i) if (mmParam[(size_t) i].load() == paramIndex && paramIndex >= 0) r.add (i);
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
    constexpr int kFactoryVersion = 8;
    const auto marker = folder.getChildFile (".factory_written");
    const int written = marker.existsAsFile() ? marker.loadFileAsString().trim().getIntValue() : 0;
    if (written >= kFactoryVersion) return;
    int currentSet = 1;
    if (! folder.createDirectory()) return;

    struct Route { int src; juce::String tgt; float amt; bool uni = false; };
    auto make = [&] (const juce::String& name, std::vector<std::pair<juce::String, double>> values, std::vector<Route> routesIn)
    {
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
        state.appendChild (extra, nullptr);
        if (auto xml = state.createXml()) xml->writeTo (folder.getChildFile (name + ".rysum"));
    };
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

    marker.replaceWithText (juce::String (kFactoryVersion));
}

juce::AudioProcessorEditor* RysumProcessor::createEditor() { return new RysumEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RysumProcessor(); }
