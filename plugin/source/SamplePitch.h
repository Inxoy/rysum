#pragma once

#include <optional>
#include <cmath>

namespace rysum
{

enum class SampleTuningMode
{
    native,
    corrected
};

struct SamplePitchRequest
{
    double sourceSampleRate = 0.0;
    double outputSampleRate = 0.0;

    // Pitch transposition in native mode, excluding sample-rate conversion.
    double nativePitchRatio = 1.0;

    // Validated frequency of the raw recording, before any transposition.
    // No value means that usable calibration is unavailable.
    std::optional<double> measuredReferenceHz;

    // Requested keyboard frequency, supplied by the selected tuning.
    double targetKeyboardHz = 0.0;

    // Register relationship: 8′ = 1, 4′ = 2, 2′ = 4, 16′ = 0.5.
    double targetRankRatio = 1.0;
};

inline std::optional<double> samplePlaybackRate(
    const SamplePitchRequest& request,
    SampleTuningMode mode)
{
    const auto positiveFinite = [] (double value)
    {
        return std::isfinite(value) && value > 0.0;
    };

    if (!positiveFinite(request.sourceSampleRate)
        || !positiveFinite(request.outputSampleRate))
        return std::nullopt;

    // defensive safeguard, if pitchRatio is left unassigned 0 value will be rejected
    double pitchRatio = 0.0;

    switch (mode)
    {
        case SampleTuningMode::native:
            if (!positiveFinite(request.nativePitchRatio))
                return std::nullopt;

            pitchRatio = request.nativePitchRatio;
            break;

        case SampleTuningMode::corrected:
            if (!request.measuredReferenceHz.has_value())
                return std::nullopt;

            if (!positiveFinite(*request.measuredReferenceHz)
                || !positiveFinite(request.targetKeyboardHz)
                || !positiveFinite(request.targetRankRatio))
                return std::nullopt;

            pitchRatio =
                (request.targetKeyboardHz / *request.measuredReferenceHz) * request.targetRankRatio;
            break;

        default:
            return std::nullopt;
    }

    const double rate = (request.sourceSampleRate / request.outputSampleRate) * pitchRatio;

    if (!positiveFinite(rate))
        return std::nullopt;

    return rate;
}

} // namespace rysum
