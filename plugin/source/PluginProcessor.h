#pragma once

#include "Audience.h"
#include "Automation.h"
#include "Concert.h"
#include "Binaural.h"
#include "Convolver.h"
#include "DsfReferenceInstrument.h"
#include "Lucier.h"
#include "LucierPiece.h"
#include "MasterBus.h"
#include "OrganEngine.h"
#include "Scala.h"
#include "Speakers.h"
#include "TimeDistance.h"
#include "Transaural.h"
#include "VoxStop.h"
#include "Walk3D.h"
#include "Pano360.h"
#include <map>

// the effect version ("Rysum FX", for audio tracks) is the same code built with IsSynth = 0
#if defined(JucePlugin_IsSynth) && !JucePlugin_IsSynth
#define RYSUM_FX 1
#else
#define RYSUM_FX 0
#endif
#include <deque>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

struct MTSClient;

class RysumProcessor : public juce::AudioProcessor,
                       private juce::Timer,
                       private juce::AsyncUpdater,
                       private rysum::ModHook
{
public:
    RysumProcessor();
    ~RysumProcessor() override;

    double getReferenceToneFrequency() const
    {
        return referenceTone.getFrequency();
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout &layouts) const override;
    void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor *createEditor() override;
    bool hasEditor() const override
    {
        return true;
    }

    const juce::String getName() const override
    {
        return "Rysum";
    }
    bool acceptsMidi() const override
    {
        return true;
    }
    bool producesMidi() const override
    {
        return false;
    }
    bool isMidiEffect() const override
    {
        return false;
    }
    double getTailLengthSeconds() const override
    {
        // (the Saal's late reverb as it is now: a host that stops or bounces after the tail must not
        //  cut a long room off)
        return juce::jmax(2.0, (double)engine.roomTailSeconds.load(std::memory_order_relaxed) + 0.5);
    }

    int getNumPrograms() override
    {
        return 1;
    }
    int getCurrentProgram() override
    {
        return 0;
    }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override
    {
        return "Rysum";
    }
    void changeProgramName(int, const juce::String &) override {}

    void getStateInformation(juce::MemoryBlock &destData) override;
    void setStateInformation(const void *data, int sizeInBytes) override;

    // ------------------------------------------------------------------
    juce::AudioProcessorValueTreeState apvts;
    rysum::OrganEngine engine;
    /** "Summe": EQ and compressor on the output (reductionDb for the meter) */
    rysum::MasterBus masterBus;
    /** the settings of the "Summe" as they act now (audio thread: with the Verlauf and modulation) */
    rysum::MasterSettings masterSettingsNow() const;
    static constexpr std::array<const char *, 14> kMasterIds{
        "mstEqOn", "mstLowHz",  "mstLowDb", "mstMidHz",  "mstMidDb",   "mstMidQ",   "mstHighHz",
        "mstHighDb", "mstCompOn", "mstThresh", "mstRatio", "mstAttack", "mstRelease", "mstMakeup"};
    std::array<int, 14> masterIdx{};
    juce::MidiKeyboardState keyboardState;

    /** current parameters as the engine sees them (also used by the UI for positions) */
    rysum::EngineParams readParams(const float *values = nullptr) const;

    // ---- modulation (routes: message thread writes, audio thread reads)
    int getParamIndex(const juce::String &id) const;
    juce::RangedAudioParameter *getParamAt(int i) const
    {
        return i >= 0 && i < (int)prmList.size() ? prmList[(size_t)i] : nullptr;
    }
    int getNumParams() const
    {
        return (int)prmList.size();
    }
    bool isModTarget(int i) const
    {
        return i >= 0 && i < (int)prmList.size() && isFloatParam[(size_t)i];
    }
    /** normalised value after modulation, NaN if the parameter is not modulated right now */
    float getModulatedNorm(int index) const;

    // ---- Verlauf: automation over time (message thread unless noted)
    rysum::AutoSet &getAuto()
    {
        return autoSet;
    }
    const rysum::AutoSet &getAuto() const
    {
        return autoSet;
    }
    /** after any change of getAuto(): hands a copy to the audio thread */
    void autoChanged();
    /** hands the lanes to the audio thread now (autoChanged does it at the next tick) */
    void autoPublish();
    /** counts changes of getAuto() (also by recording), for the page to redraw */
    int getAutoVersion() const
    {
        return autoVersion.load();
    }
    /** beats since the last key was pressed, on the clock of the lanes "mit jedem Anschlag neu" */
    double getAutoSinceKey() const
    {
        return autoSinceKey.load();
    }
    /** the clock in beats, as the audio thread saw it last */
    double getAutoNow() const
    {
        return autoNow.load();
    }
    /** true = the clock follows the host's song position right now */
    bool isAutoHostClock() const
    {
        return autoHostNow.load();
    }
    /** the clock moves right now (host playing, or the own clock running) */
    bool isAutoMoving() const
    {
        return autoMovingNow.load();
    }
    /** the own clock: run / stop, jump to a time */
    void autoPlay(bool run)
    {
        autoRun.store(run);
    }
    bool isAutoOwnRunning() const
    {
        return autoRun.load();
    }
    void autoSeek(double beats)
    {
        autoSeekReq.store(beats);
    }
    // ---- pieces ("Stücke"): the Verlauf of a piece as a file of its own (message thread). A piece
    // holds the lanes, the clock and its tempo, and the drawn paths its timetables ride on
    static juce::File getPieceFolder(); // Documents/Rysum/Stuecke
    static juce::Array<juce::File> getPieceFiles();
    static constexpr const char *kPieceExtension = ".rysumstueck";
    juce::ValueTree pieceTree() const;
    bool savePiece(const juce::File &f, juce::String &error);
    bool loadPiece(const juce::File &f, juce::String &error);
    void newPiece();
    static juce::StringArray factoryPieceNames();
    void loadFactoryPiece(int index);
    juce::String getPieceName() const
    {
        return pieceName;
    }
    juce::File getPieceFile() const
    {
        return pieceFile;
    }
    /** changed since it was loaded or saved */
    bool isPieceChanged() const
    {
        return autoVersion.load() != pieceSavedVersion;
    }
    /** record: armed lanes write while their control is moved and the clock moves */
    void setAutoRecord(bool on)
    {
        autoRec.store(on);
    }
    bool isAutoRecord() const
    {
        return autoRec.load();
    }
    /** the curve's value (before modulation) of an automated parameter, NaN = not automated now */
    float getAutomatedNorm(int index) const
    {
        return index >= 0 && index < (int)prmList.size() ? autoNorm[(size_t)index].load()
                                                         : std::numeric_limits<float>::quiet_NaN();
    }
    /** a hand holds this parameter's control (its lane gives way meanwhile) */
    bool isAutoTouched(int index) const
    {
        return index >= 0 && index < (int)prmList.size() && autoTouched[(size_t)index].load() != 0;
    }
    /** parameters a lane may drive: every continuous one plus a few switches (stops, motion,
        freeze, tremulant, Saal, tuning, Verfall mode) */
    bool isAutoTarget(int i) const;
    /** number of values of a stepped parameter (switch: 2), 0 = continuous */
    int autoSteps(int i) const;
    int getRouteSrc(int r) const
    {
        return routeSrc[(size_t)r].load();
    }
    int getRouteTgt(int r) const
    {
        return routeTgt[(size_t)r].load();
    }
    void setRoute(int r, int src, int target);
    int addRoute(int src, int target, float amount); // first free slot, -1 if full
    void clearRoute(int r);
    bool getRouteUni(int r) const
    {
        return routeUni[(size_t)r].load();
    }
    void setRouteUni(int r, bool uni);
    juce::String targetId(int target) const;
    int targetFromId(const juce::String &id) const;
    juce::String getModAmountId(int r) const
    {
        return "mod" + juce::String(r + 1) + "Amt";
    }
    std::atomic<int> routeVersion{0};

    // ---- MIDI learn: knobs / faders / buttons of a controller -> any parameter (message thread
    // writes, audio thread reads)
    static constexpr int kNumMidiMaps = 32;
    struct MidiMap
    {
        int cc = -1;                // controller number (128 + n = note n), -1 = not assigned (yet)
        int channel = 0;            // 1..16, 0 = any channel
        int param = -1;             // index into the parameter table
        float lo = 0.0f, hi = 1.0f; // range (normalised): the controller's 0..127 sweeps lo..hi
        bool toggle = false;        // buttons: every press switches between lo and hi
        bool used() const noexcept
        {
            return param >= 0 || cc >= 0;
        }
    };
    MidiMap getMidiMap(int slot) const;
    void setMidiMap(int slot, const MidiMap &m);
    void clearMidiMap(int slot);
    /** waits for the next controller that moves and gives it to this slot (-1 = stop waiting) */
    void learnMidi(int slot);
    int getLearningSlot() const
    {
        return mmLearn.load();
    }
    /** new mapping for a parameter, waiting for a controller; returns the slot (-1 = all slots
     * used) */
    int learnMidiFor(int prmIdx);
    /** slots that drive this parameter */
    juce::Array<int> midiMapsFor(int prmIdx) const;
    /** the last controller that came in: cc | channel << 8 | value << 16, -1 = none yet */
    int getLastController() const
    {
        return lastCc.load();
    }
    std::atomic<int> midiMapVersion{0};
    /** Zeit als Entfernung: how far back is heard right now (s), and how far it can go */
    std::atomic<float> timeBackNow{0.0f}, timeBackMax{rysum::TimeDistance::kMaxSeconds};
    /** the audio thread's load: the share of its time a block takes (smoothed), and the worst block
     *  since the readout last took it (it sets it back to 0) */
    std::atomic<float> cpuLoad{0.0f}, cpuPeak{0.0f};
    std::atomic<float> *audioThreadsParam = nullptr;

    // ---- MIDI buttons -> buttons of the interface: a pad, key or controller button presses a
    // button on screen that has no parameter behind it (the window has to be open for these; the
    // audio thread only notes the press, the editor does the click)
    static constexpr int kNumButtonMaps = 48;
    /** what a MIDI button sends: 0..119 = that controller, kNoteCode + n = note n */
    static constexpr int kNoteCode = 128;
    struct ButtonMap
    {
        int code = -1;       // -1 = not learned (yet)
        int channel = 0;     // 1..16, 0 = any
        juce::String target; // the button on screen (its path in the interface)
        bool used() const noexcept
        {
            return target.isNotEmpty();
        }
    };
    ButtonMap getButtonMap(int slot) const;
    void setButtonMap(int slot, const ButtonMap &m);
    void clearButtonMap(int slot);
    /** a mapping for this button, waiting for the next MIDI button pressed; returns the slot
        (-1 = all slots used) */
    int learnButton(const juce::String &target);
    void stopButtonLearn();
    int getLearningButton() const
    {
        return bmLearn.load();
    }
    juce::Array<int> buttonMapsFor(const juce::String &target) const;
    /** the next press / release that came in for a mapped button (one reader: the editor) */
    bool nextButtonEvent(int &slot, bool &down);
    /** "CC 20", "Note C3" (+ the channel if one is set) */
    static juce::String triggerName(int code, int channel);

    // ---- free tuning: Scala files, MTS-ESP (message thread)
    static constexpr int kScalaTuning = 19, kMtsTuning = 20; // entries of the tuning list
    /** a .scl (scale) or .kbm (keyboard mapping) file; selects the Scala tuning. false + status
     * text on errors */
    bool loadTuningFile(const juce::File &f);
    void clearKbm();
    std::atomic<bool> loadPresetTuning{true};
    std::atomic<bool> extendedRange{false};
    bool usesExtendedRange() const
    {
        return extendedRange.load() && apvts.getRawParameterValue("temperament")->load() > 0.5f;
    }
    juce::String getTuningStatus() const;
    juce::String getScalaReferenceText() const;
    bool hasScala() const
    {
        return scalaReady.load();
    }
    bool hasKbm() const
    {
        return kbmText.isNotEmpty();
    }
    bool mtsHasMaster() const;

    // ---- patches (message thread)
    static juce::File getPatchFolder();
    static juce::Array<juce::File> getPatchFiles();
    static juce::String cleanPatchName(const juce::String &);
    bool savePatch(const juce::String &name);
    bool loadPatch(const juce::File &f);
    juce::String getPatchName() const
    {
        return patchName;
    }
    int patchVersion = 0;
    juce::StringArray factoryWarnings; // filled while writing the factory patches (tests)

    // samples
    void loadSamples(const juce::File &folder);
    juce::File getSamplesFolder() const
    {
        return samplesFolder;
    }
    /** own sounds: <samples>/noise/eigene (empty File when no samples are loaded) */
    juce::File getCustomFolder() const;
    /** own sounds from any folder (empty = the standard one, samples/noise/eigene); saved in the
     * project and in patches */
    void setCustomFolder(const juce::File &dir);
    bool hasOwnCustomFolder() const
    {
        return customOverride != juce::File();
    }
    int getNumCustomSounds() const;
    /** re-reads the own sounds folder in the background, keeps everything else */
    void reloadCustomSounds();
    std::shared_ptr<const rysum::SampleLibrary> getLibrary() const
    {
        return std::atomic_load(&uiLibrary);
    }
    juce::String getLoadStatus() const
    {
        const juce::ScopedLock sl(statusLock);
        return loadStatus;
    }
    bool isLoading() const
    {
        return loading.load();
    }

    // Lucier mode (message thread): what the room does to the sound after n passes
    struct LucierView
    {
        std::vector<float> curve; // dB, LucierResult::kCurvePoints
        std::vector<float> peaks; // Hz
        float generation = 0.0f;
        juce::String what, status;
        int version = 0;
    };
    const LucierView &getLucierView() const
    {
        return lucView;
    }

    // scenes (message thread): up to four snapshots of all sound settings, "Szene" blends between
    // them
    static constexpr int kNumScenes = 4;
    void storeScene(int slot);
    void clearScene(int slot);
    bool isSceneStored(int slot) const
    {
        return slot >= 0 && slot < kNumScenes && !scenes[(size_t)slot].empty();
    }
    void goToScene(int slot);
    int getSceneVersion() const
    {
        return sceneVersion;
    }

    // dice (message thread): random settings within the chosen groups, gently or wildly; one step
    // undo
    enum DiceGroup
    {
        dgRegister,
        dgRaum,
        dgSaal,
        dgKlang,
        dgSpieltisch,
        dgEffekte,
        dgModulation,
        dgLucier,
        dgEingang,
        kNumDiceGroups
    };
    static juce::StringArray diceGroupNames();
    bool isDiceGroupOn(int g) const
    {
        return g >= 0 && g < kNumDiceGroups && diceGroups[(size_t)g];
    }
    void setDiceGroupOn(int g, bool on)
    {
        if (g >= 0 && g < kNumDiceGroups)
            diceGroups[(size_t)g] = on;
    }
    void dice(bool wild);
    bool canUndoDice() const
    {
        return diceUndo.isValid();
    }
    void undoDice();
    float getLucierGeneration() const
    {
        return lucEffective;
    }
    void resetLucierGrowth()
    {
        lucGrown = 0.0f;
    }
    // Lucier as a piece: record, then generation after generation by itself (message thread)
    rysum::LucierPiece &getLucierPiece()
    {
        return lucPiece;
    }
    void lucierRecord();        // starts recording (switches Lucier on, mode "Stueck")
    void lucierStopRecording(); // ... and the piece begins
    void lucierPlay()
    {
        lucPiece.play();
    }
    void lucierStop()
    {
        lucPiece.stop();
    }
    bool loadLucierRecording(const juce::File &f, juce::String &error);
    void exportLucierPiece(const juce::File &f);
    bool isLucierPieceMode() const
    {
        return apvts.getRawParameterValue("lucMode")->load() < 0.5f;
    }

    // room / impulse response (message thread)
    void loadIr(const juce::File &file);
    void clearIr();
    juce::File getIrFile() const
    {
        return irFile;
    }
    juce::String getIrStatus() const
    {
        const juce::ScopedLock sl(statusLock);
        return irStatus;
    }

    // playing helpers
    /** "Halten": a note struck twice quickly keeps sounding as if held, until it is struck again */
    std::atomic<bool> latchOn{false};
    std::atomic<int> latchedCount{0};
    std::atomic<bool> releaseLatched{false}; // UI: let all held notes go
    /** computer keyboard as MIDI keyboard (UI state; kept while the editor is closed) */
    bool computerKeys = false;
    int keysOctave = 4; // the A key plays C of this octave (C4 = MIDI 60)

    // binaural / headphones (message thread)
    static constexpr int kBinauralOutput = 6; // index of "Binaural" in the output choice
    static constexpr int kSpeakerOutput =
        7; // "Lautsprecher": AllRAD decoding to a loudspeaker layout
    static constexpr int kTransauralOutput =
        8; // "Transaural": binaural over two loudspeakers, crosstalk cancelled
    bool isSpeakers() const
    {
        return juce::roundToInt(output->load()) == kSpeakerOutput;
    }
    bool isTransaural() const
    {
        return juce::roundToInt(output->load()) == kTransauralOutput;
    }
    juce::String getTransauralStatus() const
    {
        const juce::ScopedLock sl(statusLock);
        return trStatus;
    }
    std::shared_ptr<const rysum::TransauralFilters> getTransauralFilters() const
    {
        return trUi;
    }
    // ---- loudspeakers (message thread)
    const rysum::SpeakerLayout &getSpeakerLayout() const
    {
        return spkLayout;
    }
    void setSpeakerLayout(const rysum::SpeakerLayout &l, bool rememberAsDefault = true);
    std::shared_ptr<const rysum::AllRadDecoder> getSpeakerDecoder() const
    {
        return spkUi;
    }
    int speakerVersion() const
    {
        return spkVersion;
    }
    /** test noise: -1 = off, else the index of a loudspeaker in the layout */
    std::atomic<int> speakerTest{-1};
    static juce::File speakerDefaultFile();
    /** binaural decoding is running: headphones, or transaural (the same decoder, then crosstalk
     * cancellation) */
    bool isBinaural() const
    {
        const int o = juce::roundToInt(output->load());
        return o == kBinauralOutput || o == kTransauralOutput;
    }
    bool isHeadphones() const
    {
        return juce::roundToInt(output->load()) == kBinauralOutput;
    }
    /** own HRTF set (SOFA); an empty File = the built-in FABIAN */
    void setSofaFile(const juce::File &f);
    juce::File getSofaFile() const
    {
        return sofaFile;
    }
    juce::String getRoomStatus() const
    {
        const juce::ScopedLock sl(statusLock);
        return roomStatus;
    }
    juce::String getBinauralStatus() const
    {
        const juce::ScopedLock sl(statusLock);
        return binStatus;
    }
    bool binauralReady() const
    {
        return binUi != nullptr;
    }

    // layouts (message thread)
    std::array<rysum::LayoutSettings, rysum::kNumStops> layouts;
    int layoutTuning = -1; // the tuning the "Tonnetz" layouts were made for
    /** a stop arranged as a Tonnetz follows a change of the tuning (message thread) */
    void followTuningLayouts();
    void applyLayout(int stop);
    void setPipePosition(int stop, int key, rysum::Pos p);

    static juce::String stopParamId(int s)
    {
        return "stop" + juce::String(s);
    }
    static juce::String motionParamId(int s)
    {
        return "motion" + juce::String(s);
    }
    static juce::String volParamId(int s)
    {
        return "vol" + juce::String(s);
    }
    static juce::String windParamId(int s)
    {
        return "swind" + juce::String(s);
    }
    static juce::String rateParamId(int s)
    {
        return "rate" + juce::String(s);
    }
    static juce::String depthParamId(int s)
    {
        return "depth" + juce::String(s);
    }
    static juce::String ellipseParamId(int s)
    {
        return "ellipse" + juce::String(s);
    }
    static juce::String ellDirParamId(int s)
    {
        return "ellDir" + juce::String(s);
    }
    static juce::String ellTiltParamId(int s)
    {
        return "ellTilt" + juce::String(s);
    }
    static juce::String ellFormParamId(int s)
    {
        return "ellForm" + juce::String(s);
    }
    static juce::String pathSpreadParamId(int s)
    {
        return "pathSpread" + juce::String(s);
    }
    static juce::String jumpHardParamId(int s)
    {
        return "jumpHard" + juce::String(s);
    }
    /** grain cloud per stop: key = Mix Dens Spread Size Scatter */
    static juce::String cloudParamId(const char *key, int s)
    {
        return juce::String("cloud") + key + juce::String(s);
    }
    static constexpr const char *kCloudKeys[5]{"Mix", "Dens", "Spread", "Size", "Scatter"};
    /** the sound of the pipe per stop (Raum page, tab "Pfeife"): key = Att Sharp Iso Rep Puff
     * HiFreq HiGain HiShape */
    static juce::String pipeSndParamId(const char *key, int s)
    {
        return juce::String("pipe") + key + juce::String(s);
    }
    static constexpr const char *kSpace2Ids[15]{
        "fxBeatIdeal", "fxGhostLevel", "fxGhostPlace", "fxGhostCons", "simIce",
        "simIceLen",   "fxDexMix",     "fxDexTime",    "fxDexTurn",   "fxDexMirror",
        "fxDexFade",   "fxDexAgain",   "fxExpoMix",    "fxExpoTime",  "fxExpoBright"};
    /** "Lebendige Orgel" (Effekte, third tab): bellows, pallets, the walking listener, wear,
     * sympathetic pipes, pure tuning, Tonnetz walker */
    enum LiveIndex
    {
        lvBellows,
        lvBellowsSize,
        lvBlower,
        lvTreadRate,
        lvTreadDiv,
        lvTreadNoise,
        lvPallet,
        lvPalletSrc,
        lvListenX,
        lvListenY,
        lvWalkShape,
        lvWalkRate,
        lvWalkSize,
        lvWear,
        lvWearTime,
        lvSympLevel,
        lvSympDecay,
        lvPure,
        lvPureTime,
        lvNetOn,
        lvNetPath,
        lvNetDiv,
        lvNetRate,
        lvNetGate,
        lvImpLevel,
        lvImpSrc,
        lvImpRing,
        lvPlayerMode,
        lvPlayerForce,
        lvPlayerRange,
        lvSelfPlay,
        lvGrow,
        lvWhisper,
        lvTimeDepth,
        lvRoomHear,
        lvWallPull,
        kNumLive
    };
    static constexpr const char *kLiveIds[kNumLive]{
        "lvBellows",  "lvBellowsSize", "lvBlower",   "lvTreadRate", "lvTreadDiv",  "lvTreadNoise",
        "lvPallet",   "lvPalletSrc",   "lvListenX",  "lvListenY",   "lvWalkShape", "lvWalkRate",
        "lvWalkSize", "lvWear",        "lvWearTime", "lvSympLevel", "lvSympDecay", "lvPure",
        "lvPureTime", "lvNetOn",       "lvNetPath",  "lvNetDiv",    "lvNetRate",   "lvNetGate",
        "lvImpLevel", "lvImpSrc",      "lvImpRing",  "lvPlayerMode", "lvPlayerForce", "lvPlayerRange",
        "lvSelfPlay", "lvGrow",        "lvWhisper",  "lvTimeDepth",  "lvRoomHear",    "lvWallPull"};
    /** Verschleiss: all pipes new again (message thread) */
    void resetWear()
    {
        engine.wearReset = true;
    }

    // ---- "Begehen": the 3D view's room model (an OBJ file) and pose. The model is seen, stops
    // the walker and - unless switched off - is heard: its walls hide pipes, the late reverb is
    // ray-traced through it; the Saal's shoebox stays underneath (early reflections, the size)
    std::shared_ptr<const rysum::RoomMesh> getWalkMesh() const
    {
        return std::atomic_load(&walkMesh);
    }
    /** loads the model (any thread); returns an error text, empty on success */
    juce::String loadWalkModel(const juce::File &f, bool zUp, float scale)
    {
        juce::String err;
        auto m = rysum::loadObjRoom(f, zUp, scale, err);
        if (m == nullptr)
            return err;
        walkModelFile = f;
        walkZUp = zUp;
        walkScale = scale;
        std::atomic_store(&walkMesh, std::shared_ptr<const rysum::RoomMesh>(std::move(m)));
        applyWalkAcoustics();
        return {};
    }
    /** "Alles zuruecksetzen": everything as in a new instance - every parameter, the room, the 3D view
        (its model, where one stands, lanterns, layouts), the patch name. Kept: the samples folder (and what is
        stored for this computer, not in the project: MIDI controllers, loudspeakers, window size). */
    void resetEverything();
    void clearWalkModel()
    {
        walkModelFile = juce::File();
        std::atomic_store(&walkMesh, std::shared_ptr<const rysum::RoomMesh>());
        applyWalkAcoustics();
    }
    /** the model as the sound sees it (null: none loaded) */
    std::shared_ptr<const rysum::MeshAcoustics> getWalkAcoustics() const
    {
        return std::atomic_load(&walkAc);
    }
    /** the model is heard (line of sight, reverb), not only seen */
    bool getWalkHear() const noexcept
    {
        return walkHear;
    }
    void setWalkHear(bool on);
    /** the material of one surface of a Saal (kSurfFloor, kSurfCeiling, kSurfWall0 + wall;
        material: an index for surfMaterial, -1 = as the sliders say) */
    void setSurfaceMaterial(int room, int surf, int material);
    /** painting (the 3D view's brush): a cell of 1 m on a surface (u along a wall from its first corner / x on the
        floor and ceiling, v up / y), with a material (an index for surfMaterial; -1 = wipe it off). Message thread. */
    struct PaintCell
    {
        int surf = 0, iu = 0, iv = 0, mat = 0;
    };
    void paintCell(int room, int surf, int iu, int iv, int material);
    void clearPaint(int room);
    const std::vector<PaintCell> &getPaint(int room) const
    {
        return paints[(size_t)juce::jlimit(0, rysum::kNumRooms - 1, room)];
    }
    int paintSerial() const noexcept { return paintChanges; }
    int getSurfaceMaterial(int room, int surf) const
    {
        return drawnPlans[(size_t)juce::jlimit(0, rysum::kNumRooms - 1, room)]
            .surf[(size_t)juce::jlimit(0, rysum::kMaxSurf - 1, surf)];
    }
    std::array<std::vector<PaintCell>, rysum::kNumRooms> paints;
    int paintChanges = 0;
    void refreshPaint(int room);
    juce::File walkModelFile;
    bool walkZUp = false;
    bool walkDark = false; // the 3D view with the light off: the sounding pipes light the room
    float walkScale = 1.0f;
    /** the 3D view's layouts (F1 to F8): where each pipe that was a body in the Saal stood, in its
        room (m). Saved with the project, not with a patch. Message thread. */
    struct WalkLayout
    {
        std::array<uint64_t, rysum::kNumStops> has{}; // which pipes (bit = key)
        std::array<std::array<rysum::Vec3f, rysum::kNumKeys>, rysum::kNumStops> at{};
        bool empty() const noexcept
        {
            for (auto m : has)
                if (m != 0)
                    return false;
            return true;
        }
    };
    std::array<WalkLayout, 8> walkLayouts{};
    /** the lanterns put into the 3D view (K), in the Saal (m); saved with the project */
    std::vector<rysum::Vec3f> walkLanterns;
    /** the 360° recording of the 3D view: the Ambisonics of every block (see Pano360.h) */
    rysum::Pano360Recorder pano360;
    /** counts the times something other than the 3D view changed its room, light or pose (a patch
     *  with a built-in room): the open view takes them over */
    std::atomic<int> walkSerial{0};
    /** "Publikum": the audience's phones as hands (see Audience.h). Message thread; made when
     * first switched on, off with every new instance (not saved). False: no port could be opened */
    bool setAudience(bool on, bool loopbackOnly = false);
    rysum::AudienceHands *getAudience() const noexcept
    {
        return audience != nullptr && audience->running() ? audience.get() : nullptr;
    }
    std::unique_ptr<rysum::AudienceHands> audience;
    /** the Saal (room 1) takes the loaded model's size, as a box; false = no model */
    bool fitSaalToWalkModel();
    /** a patch's room for the 3D view: one of the built-in models (by file name) and the light */
    void applyPatchRoom(const juce::ValueTree &saal3d);
    /** camera: offset from the listener's place (m), yaw (degrees, + = left), pitch (degrees, + =
     * up) */
    std::array<std::atomic<float>, 4> walkPose{};
    /** a stop's own effects (Raum page, tab "Pfeife"): tremulant, crackle, wind leaks, how
        differently fast its pipes move - ids "pipeTrem<s>" ... */
    static constexpr const char *kStopFxKeys[4]{"Trem", "Crack", "Leak", "Spread"};
    /** how a stop's pipes radiate (Raum page, tab "Pfeife"): ids "pipeWidth<s>" ... */
    static constexpr const char *kRadiateKeys[4]{"Width", "DirPat", "Face", "Spin"};
    static constexpr const char *kPipeSndKeys[8]{"Att",  "Sharp",  "Iso",    "Rep",
                                                 "Puff", "HiFreq", "HiGain", "HiShape"};
    static constexpr const char *kSpcIds[7]{"spcErode", "spcWeather", "spcGrid", "spcBits",
                                            "spcRate",  "spcDrops",   "spcAlias"};

    // ---- drawn trajectories (message thread)
    struct DrawnPath
    {
        struct Pt
        {
            float az, el, t;
        }; // degrees, seconds since the start of drawing
        std::vector<Pt> points;
        bool pingPong = false;    // there and back (else a loop)
        bool drawnTiming = false; // the speed as it was drawn (else even)
    };
    const DrawnPath &getDrawnPath(int stop) const
    {
        return drawnPaths[(size_t)stop];
    }
    void setDrawnPath(int stop, std::vector<DrawnPath::Pt> points);
    void setPathOptions(int stop, bool pingPong, bool drawnTiming);

    // ---- drawn floor plans of the simulated rooms (message thread; fractions of width / length)
    const rysum::FloorPlan &getDrawnPlan(int room) const
    {
        return drawnPlans[(size_t)juce::jlimit(0, rysum::kNumRooms - 1, room)];
    }
    void setDrawnPlan(int room, const rysum::FloorPlan &f);
    static juce::StringArray roomShapeNames();

    /** bumps whenever the library, positions or mutes change (UI repaint hint) */
    std::atomic<int> changeCounter{0};

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static BusesProperties makeBuses();
    void timerCallback() override;
    void handleAsyncUpdate() override;
    void applyCcOverrides(rysum::EngineParams &) const;
    void modulate(rysum::EngineParams &p, const float *globalSources) override;
    juce::ValueTree makeState(bool forPatch);
    void applyState(juce::ValueTree state, bool forPatch);
    juce::ValueTree startState; // the state of a new instance (made at the end of the constructor)
    void writeFactoryPatches();
    juce::String patchName;

    struct Indices
    {
        std::array<int, rysum::kNumStops> stop{}, vol{}, swind{}, motion{}, rate{}, depth{},
            ellipse{}, ellDir{}, ellTilt{}, ellForm{}, pathSpread{}, jumpHard{};
        int doppler = 0, air = 0, distAmount = 0, temperament = 0, pitch = 0, detune = 0,
            tremOn = 0, tremRate = 0, tremDepth = 0, freeze = 0, freezeSmear = 0, freezeRelease = 0,
            motionSync = 0, master = 0, output = 0, click = 0, clickBlend = 0, releaseClick = 0,
            stopPull = 0, wind = 0, degAmount = 0, degGrain = 0, degBits = 0, degRate = 0,
            degWobble = 0, degDropouts = 0, degMode = 0, irDry = 0, irWet = 0, binYaw = 0,
            windModel = 0, mtsCont = 0, mpe = 0, mpeBend = 0, lucOn = 0, lucMix = 0, lucMode = 0;
        std::array<int, rysum::kNumLfos> lfoShape{}, lfoRate{}, lfoSync{}, lfoDiv{}, lfoSpread{},
            lfoSpreadMode{};
        std::array<int, rysum::kNumEnvs> envMode{}, envDelay{}, envAttack{}, envDecay{},
            envSustain{}, envRelease{};
        std::array<int, rysum::kNumRoutes> modAmt{};
        std::array<int, 32> fx{};
        std::array<int, 16> sim{};
        std::array<int, 16> inp{};
        std::array<std::array<int, 12>, rysum::kNumRooms> room{};
        std::array<int, rysum::kNumStops> stopRoom{};
        std::array<int, 7> spc{};
        std::array<std::array<int, 5>, rysum::kNumStops> cloud{};
        std::array<std::array<int, 8>, rysum::kNumStops> pipeSnd{};
        std::array<std::array<int, 4>, rysum::kNumStops> stopFx{}; // Trem, Crack, Leak, Spread
        int airInvert = -1, clickRes = -1, clickResDecay = -1, clickResBright = -1;
        std::array<int, 4> glitch{};
        std::array<int, 15> space2{};
        std::array<int, kNumLive> live{}; // (one per LiveIndex: never a fixed number)
        std::array<std::array<int, 4>, rysum::kNumStops> radiate{}; // Width DirPat Face Spin
        std::array<std::array<int, 3>, rysum::kNumRooms> door{};    // DoorTo DoorWall DoorOpen
        int kbGlide = -1;
        int chestLevel = -1, chestTone = -1, leakFollow = -1;
        std::array<int, 4> choreo{};
        std::array<int, 5> dop{};
        std::array<int, 3> arp{};
        std::array<int, 2> harm{};
        std::array<std::array<int, 2>, rysum::kNumRooms> open{};
        int beatInstant = -1, beatMode = -1;
        int inputRoom = 0, noteSpread = 0;
    } ix;
    enum InpIndex
    {
        inpOn,
        inpLevel,
        inpMode,
        inpAz,
        inpEl,
        inpDist,
        inpWidth,
        inpSpin,
        inpFollow,
        inpGate,
        inpRelease,
        inpSend,
        kNumInp
    };
    static constexpr const char *kInpIds[kNumInp]{"inpOn",     "inpLevel", "inpMode",    "inpAz",
                                                  "inpEl",     "inpDist",  "inpWidth",   "inpSpin",
                                                  "inpFollow", "inpGate",  "inpRelease", "inpSend"};
    enum SimIndex
    {
        simOn,
        simW,
        simL,
        simH,
        simSize,
        simLx,
        simLy,
        simAbs,
        simAbsFloor,
        simHigh,
        simScatter,
        simOrder,
        simEr,
        simTail,
        simTailModel,
        kNumSim
    };
    static constexpr const char *kSimIds[kNumSim]{
        "simOn",      "simW",     "simL",   "simH",        "simSize",
        "simLx",      "simLy",    "simAbs", "simAbsFloor", "simHigh",
        "simScatter", "simOrder", "simEr",  "simTail",     "simTailModel"};
    // late reverb of the simulated room: built in the background from the room as heard
    juce::String roomStatus;
    std::array<juce::String, rysum::kNumRooms> builtTailKey;
    std::array<bool, rysum::kNumRooms> tailBuilding{};
    uint32_t tailSerial = 0;
    std::array<std::deque<std::shared_ptr<const rysum::TailIR>>, rysum::kNumRooms>
        tailKeepAlive; // message thread owns the last references
    std::array<std::shared_ptr<const rysum::TailIR>, rysum::kNumRooms>
        pendingTail; // handed to the audio thread
    rysum::RoomParams heardRoom(int room) const;
    /** the room the late reverb is built for ("Unmoeglicher Raum": another size than the one seen) */
    rysum::RoomParams tailRoom(int room) const;
    juce::String roomKey(int room) const;
    void buildTailNow(int room, const juce::String &key);

public:
    /** parameter ids of the simulated rooms: room 0 = the original "simW" ..., rooms 1..3 = "sim2W"
       ... keys: W L H Size Lx Ly Abs AbsFloor High Scatter Shape ShapeAmt */
    static juce::String roomParamId(int room, const char *key)
    {
        return room <= 0 ? juce::String("sim") + key : "sim" + juce::String(room + 1) + key;
    }
    static juce::String stopRoomParamId(int s)
    {
        return "simRoom" + juce::String(s);
    }
    /** pipes on / off (muted) per stop and key (message thread) */
    void setPipeMuted(int stop, int key, bool mutedNow)
    {
        if (stop >= 0 && stop < rysum::kNumStops && key >= 0 && key < rysum::kNumKeys)
        {
            engine.muted[(size_t)stop][(size_t)key] = mutedNow;
            ++changeCounter;
        }
    }
    bool isPipeMuted(int stop, int key) const
    {
        return engine.muted[(size_t)stop][(size_t)key].load();
    }

private:
    void setRoomStatus(const juce::String &s)
    {
        const juce::ScopedLock sl(statusLock);
        roomStatus = s;
    }
    enum FxIndex
    {
        fxCustLevel,
        fxCustDown,
        fxCustUp,
        fxCustPlace,
        fxCustPitch,
        fxCustScatter,
        fxCustWander,
        fxPathLevel,
        fxPathTime,
        fxPathTicks,
        fxPathSpread,
        fxPathWait,
        fxCrackLevel,
        fxCrackRate,
        fxCrackLen,
        fxLeakLevel,
        fxLeakCount,
        fxLeakFreq,
        fxLeakWander,
        fxWindRound,
        fxPartSpread,
        fxPartSplit,
        fxPartTurn,
        fxEchoLevel,
        fxEchoTime,
        fxEchoSync,
        fxEchoDiv,
        fxEchoRepeats,
        fxEchoFeedback,
        fxEchoTurn,
        fxEchoTilt,
        fxEchoDamp,
        kNumFx
    };
    static constexpr const char *kFxIds[kNumFx]{
        "fxCustLevel",   "fxCustDown",   "fxCustUp",      "fxCustPlace",    "fxCustPitch",
        "fxCustScatter", "fxCustWander", "fxPathLevel",   "fxPathTime",     "fxPathTicks",
        "fxPathSpread",  "fxPathWait",   "fxCrackLevel",  "fxCrackRate",    "fxCrackLen",
        "fxLeakLevel",   "fxLeakCount",  "fxLeakFreq",    "fxLeakWander",   "fxWindRound",
        "fxPartSpread",  "fxPartSplit",  "fxPartTurn",    "fxEchoLevel",    "fxEchoTime",
        "fxEchoSync",    "fxEchoDiv",    "fxEchoRepeats", "fxEchoFeedback", "fxEchoTurn",
        "fxEchoTilt",    "fxEchoDamp"};
    static_assert(kNumFx <= 32);
    std::vector<juce::RangedAudioParameter *> prmList;
    std::vector<std::atomic<float> *> rawList;
    std::vector<char> isFloatParam;
    std::map<juce::String, int> paramIndex;
    std::array<std::atomic<int>, rysum::kNumRoutes> routeSrc{}, routeTgt{};
    std::array<std::atomic<bool>, rysum::kNumRoutes> routeUni{};
    std::vector<float> modVals, modOff; // audio thread
    std::vector<char> modHitFlag;
    std::vector<int> hits, prevHits;
    std::unique_ptr<std::atomic<float>[]> modNorm;
    bool haveModVals = false;
    std::vector<juce::RangedAudioParameter *> ccTargets;
    // MIDI learn
    std::array<std::atomic<int>, kNumMidiMaps> mmCc{}, mmChan{}, mmParam{};
    std::array<std::atomic<float>, kNumMidiMaps> mmLo{}, mmHi{}, mmPending{};
    std::array<std::atomic<bool>, kNumMidiMaps> mmToggle{};
    std::array<bool, kNumMidiMaps> mmPressed{}; // audio thread (toggle buttons)
    std::atomic<int> mmLearn{-1}, lastCc{-1};
    // MIDI buttons -> buttons on screen
    std::array<std::atomic<int>, kNumButtonMaps> bmCode{}, bmChan{};
    std::array<std::atomic<bool>, kNumButtonMaps> bmOn{};
    std::array<juce::String, kNumButtonMaps> bmTarget; // message thread
    std::array<bool, kNumButtonMaps> bmPressed{};      // audio thread
    std::atomic<int> bmLearn{-1};
    std::atomic<bool> noteMaps{false}; // some mapping listens to a note: notes are looked at
    juce::AbstractFifo bmFifo{256};
    std::array<int, 256> bmEvents{};
    juce::MidiBuffer noteScratch;
    /** the room keyboard (Saal page): notes on its channel play the room, not the pipes. The held
        keys (velocity, 0 = up) and what they mean for the engine (audio thread). */
    std::array<uint8_t, 128> roomKeyVel{};
    int roomKbChLast = 0;
    rysum::EngineParams::RoomKeys roomKeysNow;
    std::atomic<float> *roomKbChParam = nullptr;
    void takeRoomKeys(juce::MidiBuffer &midi);

public:
    /** the room keyboard's held keys, for the Saal page (bit = MIDI note, 0..127) */
    std::array<std::atomic<uint32_t>, 4> roomKeysHeld{};

private:
    void updateNoteMaps();
    bool handleTrigger(int code, int channel, int value); // audio thread; true = mapped
    int savedMapVersion = 0;
    juce::ValueTree midiMapState() const;
    void applyMidiMapState(const juce::ValueTree &);
    static juce::File midiMapFile();
    void saveMidiMapDefault();
    bool handleLearnedCc(int cc, int channel,
                         int value); // audio thread; true = this controller is mapped
    std::array<std::atomic<float>, 3 + 2 * rysum::kNumStops> ccPending{};
    std::map<std::string, std::atomic<float> *> extraParams;
    void startAutoLoad();
    void setStatus(const juce::String &s)
    {
        const juce::ScopedLock sl(statusLock);
        loadStatus = s;
    }
    void onLoaded(std::shared_ptr<const rysum::SampleLibrary> lib, const juce::String &err,
                  const juce::File &folder);
    void setIrStatus(const juce::String &s)
    {
        const juce::ScopedLock sl(statusLock);
        irStatus = s;
    }

    juce::File irFile;
    // an impulse response was chosen (loaded, removed, or set by a restored project): the built-in
    // default is not applied any more
    bool irChosen = false;
    juce::File customOverride; // chosen folder for own sounds (empty = samples/noise/eigene)
    bool customReloading = false;
    juce::String irStatus{juce::String::fromUTF8("keine \xe2\x80\x93 nur trocken")};
    juce::ThreadPool irPool{1};
    // scenes: per slot (param index, normalised value); empty = not stored
    std::array<std::vector<std::pair<int, float>>, kNumScenes> scenes;
    int sceneVersion = 0, sceneVersionApplied = 0;
    float morphApplied = -1.0f;
    bool takesPartInScenes(const juce::String &id) const;
    void applySceneMorph();
    juce::ValueTree scenesState() const;
    void applyScenesState(const juce::ValueTree &t);
    // ---- Verlauf (automation)
    struct AutoCompiled
    {
        bool on = true;
        int clock = 0;
        double tempo = 60.0;
        bool startOnKey = false;
        bool cycle = false; // the own clock repeats cycleStart .. cycleEnd
        double cycleStart = 0.0, cycleEnd = 16.0;
        bool hasTempo = false; // a tempo lane drives the own clock
        rysum::AutoLane tempoLane;
        struct Lane
        {
            int param = -1, steps = 0, index = 0; // index: in AutoSet::lanes
            bool perPipe = false; // "mit jedem Anschlag neu" on a control with a per-pipe target: the engine plays it
            rysum::AutoLane lane;
        };
        std::vector<Lane> lanes;
        std::vector<rysum::AutoLane> spatial; // per-pipe and path lanes (the view points into it)
        rysum::AutoEngineView view;
    };
    rysum::AutoSet autoSet;                                   // message thread
    std::shared_ptr<const AutoCompiled> autoPending, autoActive; // pending: swapped atomically
    std::deque<std::shared_ptr<const AutoCompiled>> autoKeep; // message thread keeps them alive
    std::atomic<int> autoVersion{0};
    std::atomic<double> autoNow{0.0}, autoSeekReq{-1.0}, autoSinceKey{1.0e9};
    std::atomic<bool> autoHostNow{false}, autoMovingNow{false}, autoRun{false}, autoRec{false};
    double autoOwnBeats = 0.0; // audio thread
    std::unique_ptr<std::atomic<float>[]> autoNorm;
    std::unique_ptr<std::atomic<uint8_t>[]> autoTouched;
    std::vector<char> autoLaneMade; // message thread: this touch has been looked at (record: new lane)
    juce::String pieceName;         // the piece on the Verlauf page (empty: not named yet)
    juce::File pieceFile;           // where it was loaded from / saved to (none: not saved yet)
    int pieceSavedVersion = 0;      // autoVersion when it was loaded or saved
    void applyPieceTree(const juce::ValueTree &t, const juce::String &name, const juce::File &file);
    std::vector<char> autoFlag;           // audio thread: automated in this slice
    std::vector<int> autoHits, prevAutoHits; // audio thread
    std::vector<int> hostToList;          // processor parameter index -> prmList index
    struct AutoRecSample
    {
        int lane;
        double t;
        float v;
    };
    static constexpr int kAutoFifo = 8192;
    juce::AbstractFifo autoFifo{kAutoFifo};
    std::array<AutoRecSample, kAutoFifo> autoFifoData{};
    struct AutoPass // one recorded pass of a lane (message thread)
    {
        bool active = false;
        double t0 = 0.0, tLast = 0.0;
        juce::uint32 lastMs = 0;
        std::vector<rysum::AutoPoint> raw, before;
    };
    std::array<AutoPass, rysum::kMaxAutoLanes> autoPass;
    void applyAutomation(const rysum::EngineParams &p); // audio thread, from modulate()
    void autoTick();                                    // message thread, 40 Hz: recording
    bool autoDirty = false;
    struct GestureWatch : juce::AudioProcessorParameter::Listener
    {
        explicit GestureWatch(RysumProcessor &p) : proc(p) {}
        void parameterValueChanged(int, float) override {}
        void parameterGestureChanged(int i, bool starting) override
        {
            if (i >= 0 && i < (int)proc.hostToList.size() && proc.hostToList[(size_t)i] >= 0)
                proc.autoTouched[(size_t)proc.hostToList[(size_t)i]] = starting ? 1 : 0;
        }
        RysumProcessor &proc;
    } gestureWatch{*this};

    struct MorphTimer : juce::Timer
    {
        explicit MorphTimer(RysumProcessor &p) : proc(p) {}
        void timerCallback() override
        {
            proc.applySceneMorph();
            proc.autoTick();
            proc.followTuningLayouts();
        }
        RysumProcessor &proc;
    } morphTimer{*this};
    // dice
    std::array<bool, kNumDiceGroups> diceGroups{true, true, false, true, true,
                                                true, true, false, false};
    juce::ValueTree diceUndo;
    juce::Random diceRng;
    int diceGroupOf(const juce::String &id) const;
    // Lucier mode: responses built on their own thread
    juce::ThreadPool lucPool{1};
    juce::ThreadPool piecePool{2}; // generations of the piece, export
    rysum::LucierPiece lucPiece;
    std::vector<float> lucRecScratch; // audio thread: the mono signal recorded
    int savedRecordingVersion = 0;
    void saveLucierRecording();
    bool savedFromFile = false;
    LucierView lucView;
    float lucGrown = 0.0f, lucEffective = 1.0f;
    bool lucBuilding = false;
    juce::String builtLucKey, lucBaseKey;
    std::shared_ptr<const rysum::RoomMesh>
        walkMesh; // the 3D view's model (atomic_load / atomic_store)
    std::shared_ptr<const rysum::LucierBase>
        lucBase; // the pool thread reads it, the message thread swaps it
    uint32_t lucSerial = 0;
    std::deque<std::shared_ptr<const rysum::TailIR>> lucKeepAlive;
    std::shared_ptr<const rysum::TailIR> pendingLucier;
    void updateLucier(double dtSeconds);
    std::shared_ptr<rysum::Convolver> irActive;        // audio thread
    std::shared_ptr<rysum::Convolver> pendingIr;       // handed over (atomic_store / exchange)
    std::shared_ptr<rysum::Convolver> irUi, retiredIr; // message thread keeps them alive
    std::atomic<bool> irClearRequest{false};
    std::atomic<double> hostRate{0.0};
    std::array<std::vector<float>, rysum::OrganEngine::kMaxRoomSends> irInputs;
    rysum::Convolver::Settings currentIrSettings() const;
    static juce::String irSettingsKey(const rysum::Convolver::Settings &);
    juce::String builtIrKey, pendingIrKey;
    int irKeyTicks = 0;
    float drySmoothed = 1.0f, wetSmoothed = 0.0f;
    std::atomic<float> *irDry = nullptr, *irWet = nullptr, *irFumaParam = nullptr,
                       *irN3DParam = nullptr, *irFollowParam = nullptr, *irEnvelopParam = nullptr,
                       *irDirectParam = nullptr;
    void applyLatch(juce::MidiBuffer &midi, int numSamples);
    std::array<bool, 128> latched{};
    std::array<int64_t, 128> lastOnAt{};
    int64_t sampleClock = 0;
    bool latchWasOn = false;
    juce::MidiBuffer latchMidi;

    std::vector<rysum::KeyTuning> tunings; // every entry of the tuning list, computed once
    std::array<DrawnPath, rysum::kNumStops> drawnPaths;
    std::array<rysum::FloorPlan, rysum::kNumRooms> drawnPlans;
    // the room model for the sound: the engine uses it on the audio thread, the last few are kept
    // alive here (so the engine never frees one)
    void applyWalkAcoustics();
    std::shared_ptr<const rysum::MeshAcoustics> walkAc;
    std::array<std::shared_ptr<const rysum::MeshAcoustics>, 4> walkAcKeep;
    juce::CriticalSection walkAcLock;
    uint32_t walkAcSerial = 0;
    unsigned walkAcKeepPos = 0;
    bool walkHear = true;
    void sendPath(int stop);
    // free tuning
    rysum::ScalaTuning scala; // message thread
    juce::String sclText, sclName, kbmText, kbmName, tuningError;
    std::atomic<bool> scalaReady{false};
    bool setScala(const juce::String &scl, const juce::String &sName, const juce::String &kbm,
                  const juce::String &kName);
    rysum::NoteTable scalaShared; // written by the message thread under scalaLock
    juce::SpinLock scalaLock;
    std::atomic<int> scalaVersion{0};
    int scalaVersionSeen = -1;
    rysum::NoteTable noteTable; // audio thread: what the engine plays
    MTSClient *mts = nullptr;
    bool mtsActive() const;

    // binaural
    juce::File sofaFile;
    std::shared_ptr<const rysum::HrirSet>
        hrirSet;            // message thread: the loaded set (FABIAN or SOFA)
    juce::File hrirSetFile; // which file hrirSet came from (empty = FABIAN)
    // loudspeakers
    rysum::SpeakerLayout spkLayout;
    std::shared_ptr<const rysum::AllRadDecoder> spkUi, pendingSpk;
    std::deque<std::shared_ptr<const rysum::AllRadDecoder>> spkKeepAlive;
    rysum::SpeakerRenderer spkRenderer; // audio thread
    juce::String spkBuiltKey;
    uint32_t spkSerial = 0;
    int spkVersion = 0;
    std::atomic<int> spkOrderNow{3};
    rysum::XorShift testNoise{17};
    float testLp1 = 0.0f, testLp2 = 0.0f;
    void rebuildSpeakers(bool force);
    juce::ValueTree speakersState() const;
    std::shared_ptr<rysum::BinauralDecoder> binActive;  // audio thread
    rysum::AmbiRotator ambiViewRot;                      // audio thread: AmbiX out turned and tilted by the 3D view's head
    std::shared_ptr<rysum::BinauralDecoder> pendingBin; // handed over (atomic_store / exchange)
    std::shared_ptr<rysum::BinauralDecoder> binUi, retiredBin; // message thread keeps them alive
    // Zeit als Entfernung: the memory (built by the timer once it is wanted, for the channels in use)
    std::shared_ptr<rysum::TimeDistance> timeActive;  // audio thread
    std::shared_ptr<rysum::TimeDistance> pendingTime; // handed over (atomic_store / exchange)
    std::deque<std::shared_ptr<rysum::TimeDistance>> timeKeep; // message thread keeps them alive
    std::atomic<int> timeChannels{0};                 // channels the audio thread needs
    rysum::DsfReferenceInstrument referenceTone;
    juce::AudioBuffer<float> inBuf;
    juce::AudioBuffer<float> voiceBuf; // Konzert, the listener: the player's voice for this block
    bool inputThisBlock = false;     // the audio input of the block (main input + side chain)
    // Vox humana (the neural stop) and "Eingang spielt die Orgel"
    rysum::VoxStop voxStop;
    juce::MidiBuffer voxMidi; // the notes the input plays, added to the block's MIDI
    void runVox(juce::MidiBuffer &midi, int n, rysum::EngineParams &params) noexcept;
    /** the Vox humana's level and place from parameter values (null = the parameters themselves) */
    void fillVoxPlace(rysum::VoxParams &vp, const float *vals) const noexcept;

public:
    /** the Vox humana's model could not be loaded: why (empty = fine) */
    juce::String voxError;
    /** what the input plays right now (for the console): MIDI keys, -1 = none */
    int voxTrackedNote(int v) const noexcept { return voxShown[(size_t)v].load(); }
    /** the Vox humana's voices for the 3D Saal (0..7 tracked, 8..15 played) */
    const vox::OrganSynth::Display &voxVoice(int v) const noexcept { return voxStop.display(v); }
    /** what the input's notes carried so far (tests): note-ons, velocity range, pressure messages */
    const rysum::VoxStop::NoteStats &voxNoteStats() const noexcept { return voxStop.stats; }
    void clearVoxNoteStats() noexcept { voxStop.stats.clear(); }

    /** the Vox humana's level against the sample stops (the network's output is quiet) */
    static constexpr float kVoxGain = 10.0f; // measured: then about as loud as one sample stop
    static constexpr std::array<const char *, 26> kVoxIds{
        "voxOn",   "voxVol",  "voxWind", "voxX",    "voxY",    "voxBright", "voxNoise",  "voxSpread", "voxFollow",
        "voxRoom", "voxAz",   "voxEl",   "voxDist", "voxSend", "inPlays",   "inThresh",  "inConfirm", "inVoices",
        "voxMotion", "voxRate", "voxDepth", "voxWidth", "simRoomVox", "inVelGain", "inVelRange", "voxDyn"};

private:
    std::array<std::atomic<int>, 8> voxShown{};
    std::array<std::atomic<float> *, kVoxIds.size()> voxRaw{}; // kVoxIds, read on the audio thread
    std::array<int, kVoxIds.size()> voxIdx{};                  // their index in prmList (modulated values: modVals)
    juce::AudioBuffer<float> binBuf; // Ambisonics render buffer in binaural mode
    juce::String binStatus{juce::String::fromUTF8("aus \xe2\x80\x93 Ausgabe oben auf \xe2\x80\x9e"
                                                  "Binaural\xe2\x80\x9c stellen")};
    juce::String builtBinKey, pendingBinKey;
    int binKeyTicks = 0;
    std::atomic<float> *binOrderParam = nullptr, *binYawParam = nullptr, *binDfParam = nullptr;
    juce::String binauralKey() const;
    void rebuildBinaural();
    void setBinStatus(const juce::String &s)
    {
        const juce::ScopedLock sl(statusLock);
        binStatus = s;
    }
    // transaural: the crosstalk cancellation after the binaural decoder (filters designed on the
    // pool, loaded on the message thread)
    rysum::TransauralCtc trCtc;
    std::shared_ptr<const rysum::TransauralFilters> trUi;
    juce::String trBuiltKey, trPendingKey;
    int trKeyTicks = 0;
    juce::String trStatus{juce::String::fromUTF8(
        "aus \xe2\x80\x93 Ausgabe auf \xe2\x80\x9eTransaural\xe2\x80\x9c stellen")};
    juce::String transauralKey() const;
    void rebuildTransaural();

    std::shared_ptr<std::atomic<bool>> aliveFlag = std::make_shared<std::atomic<bool>>(true);

    class Loader;
    std::unique_ptr<Loader> loader;
    juce::File samplesFolder;
    std::shared_ptr<const rysum::SampleLibrary> pendingLibrary; // handed to the audio thread
    std::shared_ptr<const rysum::SampleLibrary> uiLibrary;      // used by the editor
    std::shared_ptr<const rysum::SampleLibrary>
        retiredLibrary; // kept alive briefly (freed off the audio thread)
    int retireCountdown = 0;
    juce::CriticalSection statusLock;
    juce::String loadStatus{"Samples werden gesucht ..."};
    std::atomic<bool> loading{false};
    bool autoLoadDone = false;

    std::array<std::atomic<float> *, rysum::kNumStops> stopParams{}, motionParams{}, rateParams{},
        depthParams{}, volParams{}, windParams{}, ellipseParams{};
    std::atomic<float> *master = nullptr, *output = nullptr, *click = nullptr,
                       *clickBlend = nullptr, *releaseClick = nullptr, *stopPull = nullptr,
                       *wind = nullptr, *degAmount = nullptr, *degGrain = nullptr,
                       *degBits = nullptr, *degRate = nullptr, *degWobble = nullptr,
                       *degDropouts = nullptr, *degMode = nullptr;

    friend class rysum::ConcertLink;
    double concertAudioTime = 0.0; // audio thread: seconds of audio processed (the concert's clock)

public:
    /** "Konzert": play for listeners on other computers, or listen to a player (see Concert.h).
        Declared last: it goes first, while everything it uses is still there. */
    rysum::ConcertLink concert{*this};
    /** the listener, audio thread: the player'"'"'s clocks (motion, LFOs, the Verlauf) */
    void concertClocks(const double *motion, const double *lfo, double beats, bool running) noexcept;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RysumProcessor)
};
