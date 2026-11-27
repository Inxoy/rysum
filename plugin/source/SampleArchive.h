#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <functional>
#include <memory>

namespace rysum
{
/** The sample library packed into one file for handing out ("rysum-samples.rysum" next to the
    folder name it stands for): every file of the folder, the audio as FLAC (lossless), the whole
    scrambled with a key compiled into the program - so it opens in no audio program and no tool
    that looks for audio inside files finds any. It is not copy protection: the program itself
    can read it, so anyone determined could too.

    Mounted, its files appear under the folder's name - in memory only, nothing is written to disk -
    and the sample library and the impulse responses read them from there. */
namespace SampleArchive
{
/** the archive that stands for a samples folder: "<folder>.rysum" */
juce::File archiveFor(const juce::File &folder);

/** packs a samples folder (every file but pictures; WAVs as FLAC) into 'out' */
bool pack(const juce::File &folder, const juce::File &out, juce::String &error,
          const std::function<void(const juce::String &)> &log = {});

/** mounts the archive for 'folder' (once; false = none, or not ours) */
bool mount(const juce::File &folder, juce::String &error);

/** a mounted file's bytes (nullptr = not in a mounted archive) */
std::shared_ptr<const juce::MemoryBlock> find(const juce::File &f);

/** whether the file exists on disk or in a mounted archive */
bool exists(const juce::File &f);

/** an audio reader for a file on disk or in a mounted archive */
std::unique_ptr<juce::AudioFormatReader> openAudio(juce::AudioFormatManager &fm, const juce::File &f);
} // namespace SampleArchive
} // namespace rysum
