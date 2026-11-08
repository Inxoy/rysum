#include "SamplePitch.h"

#include <cmath>
#include <iostream>
#include <limits>

int main()
{
    rysum::SamplePitchRequest request;
    request.sourceSampleRate = 44100.0;
    request.outputSampleRate = 48000.0;

    const auto native = rysum::samplePlaybackRate(
        request, rysum::SampleTuningMode::native);

    if (!native.has_value()
        || std::abs(*native - 0.91875) > 1.0e-12)
    {
        std::cerr << "Native sample-rate conversion failed\n";
        return 1;
    }

    request.sourceSampleRate = 48000.0;
    request.measuredReferenceHz = 442.0;
    request.targetKeyboardHz = 440.0;

    const auto corrected = rysum::samplePlaybackRate(
        request, rysum::SampleTuningMode::corrected);

    if (!corrected.has_value()
        || std::abs(*corrected - 220.0 / 221.0) > 1.0e-12)
    {
        std::cerr << "Corrected pitch calculation failed\n";
        return 1;
    }

    // Corrected playback requires a measured reference frequency.
    request.measuredReferenceHz.reset();

    const auto missing = rysum::samplePlaybackRate(
        request, rysum::SampleTuningMode::corrected);

    if (missing.has_value())
    {
        std::cerr << "Missing calibration was accepted\n";
        return 1;
    }

    // Native playback still works without calibration.
    request.nativePitchRatio = 0.5;

    const auto native16 = rysum::samplePlaybackRate(
        request, rysum::SampleTuningMode::native);

    if (!native16.has_value()
        || std::abs(*native16 - 0.5) > 1.0e-12)
    {
        std::cerr << "Native octave transposition failed\n";
        return 1;
    }

    // A 4-foot rank must sound one octave above the keyboard target.
    request.measuredReferenceHz = 880.0;
    request.targetKeyboardHz = 440.0;
    request.targetRankRatio = 2.0;

    const auto corrected4 = rysum::samplePlaybackRate(
        request, rysum::SampleTuningMode::corrected);

    if (!corrected4.has_value()
        || std::abs(*corrected4 - 1.0) > 1.0e-12)
    {
        std::cerr << "Corrected register ratio failed\n";
        return 1;
    }

    // A zero output rate would cause division by zero.
    request.outputSampleRate = 0.0;

    const auto zeroRate = rysum::samplePlaybackRate(
        request, rysum::SampleTuningMode::native);

    if (zeroRate.has_value())
    {
        std::cerr << "Zero output sample rate was accepted\n";
        return 1;
    }

    request.outputSampleRate = 48000.0;

    // NaN means "not a number": present metadata can still be invalid.
    request.measuredReferenceHz =
        std::numeric_limits<double>::quiet_NaN();

    const auto invalidPitch = rysum::samplePlaybackRate(
        request, rysum::SampleTuningMode::corrected);

    if (invalidPitch.has_value())
    {
        std::cerr << "Invalid measured frequency was accepted\n";
        return 1;
    }

    // Invalid calibration must not prevent native playback.
    const auto nativeWithoutCalibration = rysum::samplePlaybackRate(
        request, rysum::SampleTuningMode::native);

    if (!nativeWithoutCalibration.has_value())
    {
        std::cerr << "Native playback incorrectly requires calibration\n";
        return 1;
    }

    std::cout << "Sample pitch tests passed\n";
    return 0;
}
