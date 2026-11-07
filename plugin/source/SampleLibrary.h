#pragma once

#include "Common.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <vector>

namespace rysum
{
/** one mono sample in memory */
struct Sample
{
    std::vector<float> data;
    double sampleRate = kSampleFileRate;
    int size() const noexcept { return (int) data.size(); }
};

/** one pipe of one stop, as described by rysum_samples.json */
struct Pipe
{
    std::shared_ptr<const Sample> sample;
    int onset = 0, attackEnd = 0, release = 0, loopStart = -1, loopEnd = -1;
    double rate = 1.0;           // != 1 when the pipe is replaced by a pitch-shifted neighbour
    bool valid = false;
    bool substituted = false;
    int substituteFrom = -1;     // key index of the source pipe

    // info for the "Pfeifen" view
    int midi = 0;
    float levelDb = -100.0f, loopNcc = 0.0f;
    double nominalHz = 0, measuredHz = 0, cents = 0;
    bool hasCents = false;
    juce::String file, source, monoMethod;
    double sourceTime = 0;
    juce::StringArray flags;
    int takes = 1;

    bool hasLoop() const noexcept { return loopStart >= 0 && loopEnd > loopStart + 64; }
};

struct NoiseSample
{
    std::shared_ptr<const Sample> sample;
    juce::String kind;
    float peakDb = 0;
    float norm = 1.0f;          // gain that brings the sample to a -6 dBFS peak (effects use it, the console clicks don't)
};

class SampleLibrary
{
public:
    std::array<std::array<Pipe, kNumKeys>, kNumStops> pipes;
    std::vector<NoiseSample> clicksDown, clicksUp, clicksOther, stopPulls;
    std::vector<NoiseSample> custom;          // own sounds from noise/eigene (screws, clanks, ...)
    std::vector<std::shared_ptr<const Sample>> wind;
    juce::File folder;
    juce::StringArray warnings;
    int loadedPipes = 0, substitutedPipes = 0;

    /** Loads rysum_samples.json and all WAVs from 'folder'. Returns nullptr and fills
        'error' when the folder can't be used. Runs on a background thread. */
    static std::unique_ptr<SampleLibrary> load (const juce::File& folder, juce::String& error,
                                                std::function<bool()> shouldExit = {});

    /** typical places where the samples folder may live */
    static juce::Array<juce::File> candidateFolders();
    /** folder for own sounds: <samples>/noise/eigene */
    static juce::File customFolder (const juce::File& sampleFolder) { return sampleFolder.getChildFile ("noise").getChildFile ("eigene"); }

    /** reads all audio files of the own-sounds folder (mono, trimmed, peak-normalised) */
    static std::vector<NoiseSample> loadCustom (const juce::File& sampleFolder, std::function<bool()> shouldExit = {});
    /** the same from any folder (with its subfolders) */
    static std::vector<NoiseSample> loadCustomDir (const juce::File& dir, std::function<bool()> shouldExit = {});

    static bool looksLikeSampleFolder (const juce::File& f) { return f.getChildFile ("rysum_samples.json").existsAsFile(); }
};
} // namespace rysum
