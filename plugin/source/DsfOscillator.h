#pragma once

#include "DsfIntegrator.h"
#include "DsfMath.h"

#include <algorithm>
#include <cmath>

namespace rysum
{

class DsfOscillator
{
public:
    bool prepare(double internalSampleRate, double outputSampleRate)
    {
        if (!std::isfinite(internalSampleRate) || !std::isfinite(outputSampleRate) ||
            outputSampleRate <= 0.0 || internalSampleRate < outputSampleRate)
            return false;

        processingRate = internalSampleRate;
        outputRate = outputSampleRate;

        phase = 0.0;
        phaseStep = 0.0;
        sourcePhaseOffset = 0.0;
        active = false;
        integrator.reset();

        return true;
    }

    bool startNote(double frequency, double spectralBalance, DsfIntegrationMethod method,
                   int requestedHarmonics = 128)
    {
        const double cutoff = outputRate * 0.45;

        if (processingRate <= 0.0 || !std::isfinite(frequency) || frequency <= 0.0 ||
            frequency >= cutoff || !std::isfinite(spectralBalance) || spectralBalance < 0.0 ||
            spectralBalance > 1.0 || requestedHarmonics < 1 || requestedHarmonics > 4096)
            return false;

        const double pi = std::acos(-1.0);
        const double nextStep = 2.0 * pi * frequency / processingRate;

        const int nextHarmonics = static_cast<int>(
            std::min(static_cast<double>(requestedHarmonics), std::floor(cutoff / frequency)));

        DsfIntegrator nextIntegrator;

        if (!nextIntegrator.configure(method, nextStep))
            return false;

        const bool rectangular = method == DsfIntegrationMethod::rectangular;

        const double nextOffset = rectangular ? -nextStep / 2.0 : 0.0;

        const double firstInput = cosineDsf(nextOffset, spectralBalance, nextHarmonics);

        const double previousInput =
            cosineDsf(nextOffset - nextStep, spectralBalance, nextHarmonics);

        const double gain = rectangular ? 2.0 * std::sin(nextStep / 2.0) : std::tan(nextStep / 2.0);

        const double initialOutput =
            rectangular ? -gain * firstInput : -gain * (previousInput + firstInput);

        nextIntegrator.reset(initialOutput, previousInput);

        phase = 0.0;
        phaseStep = nextStep;
        sourcePhaseOffset = nextOffset;
        balance = spectralBalance;
        harmonics = nextHarmonics;
        integrator = nextIntegrator;
        active = true;

        return true;
    }

    double nextSample()
    {
        if (!active)
            return 0.0;

        const double input = cosineDsf(phase + sourcePhaseOffset, balance, harmonics);

        const double sample = integrator.processSample(input);

        const double twoPi = 2.0 * std::acos(-1.0);

        phase += phaseStep;

        if (phase >= twoPi)
            phase -= twoPi;

        return sample;
    }

    void stop()
    {
        active = false;
    }

private:
    double processingRate = 0.0;
    double outputRate = 0.0;

    double phase = 0.0;
    double phaseStep = 0.0;
    double sourcePhaseOffset = 0.0;

    double balance = 0.0;
    int harmonics = 1;

    bool active = false;

    DsfIntegrator integrator;
};

} // namespace rysum
