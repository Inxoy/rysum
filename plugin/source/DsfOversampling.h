#pragma once

#include "DsfIntegrator.h"

#include <cmath>
#include <limits>
#include <optional>

namespace rysum
{

// Signed amplitude error relative to ideal 1/n weighting.
// Invalid inputs return positive infinity.
inline double integrationErrorDb(DsfIntegrationMethod method, double phaseStep, int highestHarmonic)
{
    const double pi = std::acos(-1.0);
    const double invalid = std::numeric_limits<double>::infinity();

    if (!std::isfinite(phaseStep) || phaseStep <= 0.0 || highestHarmonic < 1)
        return invalid;

    const double fundamentalAngle = phaseStep / 2.0;
    const double highestAngle = fundamentalAngle * static_cast<double>(highestHarmonic);

    if (highestAngle >= pi / 2.0)
        return invalid;

    double amplitude = 0.0;

    switch (method)
    {
    case DsfIntegrationMethod::rectangular:
        amplitude = std::sin(fundamentalAngle) / std::sin(highestAngle);
        break;

    case DsfIntegrationMethod::trapezoidal:
        amplitude = std::tan(fundamentalAngle) / std::tan(highestAngle);
        break;

    default:
        return invalid;
    }

    const double relativeAmplitude = amplitude * static_cast<double>(highestHarmonic);

    if (!std::isfinite(relativeAmplitude) || relativeAmplitude <= 0.0)
        return invalid;

    return 20.0 * std::log10(relativeAmplitude);
}

struct DsfOversamplingChoice
{
    int factor = 1;
    double errorDb = 0.0;
    bool meetsTolerance = false;
};

inline std::optional<DsfOversamplingChoice>
chooseDsfOversampling(DsfIntegrationMethod method, double frequency, double outputSampleRate,
                      int highestHarmonic, double maximumErrorDb = 0.25)
{
    if (!std::isfinite(frequency) || !std::isfinite(outputSampleRate) ||
        !std::isfinite(maximumErrorDb) || frequency <= 0.0 || outputSampleRate <= 0.0 ||
        highestHarmonic < 1 || maximumErrorDb < 0.0)
        return std::nullopt;

    // Oversampling must not permit partials above output Nyquist.
    if (frequency >= outputSampleRate * 0.5 / static_cast<double>(highestHarmonic))
        return std::nullopt;

    const double twoPi = 2.0 * std::acos(-1.0);
    const int factors[] = {1, 2, 4, 8};

    DsfOversamplingChoice choice;

    for (int factor : factors)
    {
        const double internalRate = outputSampleRate * static_cast<double>(factor);

        const double phaseStep = twoPi * (frequency / internalRate);

        const double error = integrationErrorDb(method, phaseStep, highestHarmonic);

        if (!std::isfinite(error))
            return std::nullopt;

        choice = {factor, error, std::abs(error) <= maximumErrorDb};

        if (choice.meetsTolerance)
            return choice;
    }

    return choice;
}

} // namespace rysum
