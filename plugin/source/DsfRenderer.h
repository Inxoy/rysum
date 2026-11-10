#pragma once

#include "DsfOscillator.h"
#include "DsfOversampling.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <utility>

namespace rysum
{

class DsfRenderer
{
public:
    DsfRenderer() = default;

    bool prepare(double sampleRate, int maximumSamplesPerBlock)
    {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || maximumSamplesPerBlock <= 0)
            return false;

        for (std::size_t stage = 0; stage < oversamplers.size(); ++stage)
        {
            auto filter = std::make_unique<Oversampler>(
                1, stage, Oversampler::filterHalfBandPolyphaseIIR, true, false);

            filter->initProcessing(static_cast<std::size_t>(maximumSamplesPerBlock));

            filter->reset();

            oversamplers[stage] = std::move(filter);
        }

        silentInput.setSize(1, maximumSamplesPerBlock);
        silentInput.clear();

        outputRate = sampleRate;
        maximumBlockSize = maximumSamplesPerBlock;

        oscillator.stop();
        previousOscillator.stop();
        transitionRemaining = 0;
        pendingPartials = currentPartials;
        activeStage = 0;

        return true;
    }

    int getOversamplingFactor() const
    {
        if (outputRate <= 0.0)
            return 0;

        return static_cast<int>(oversamplers[activeStage]->getOversamplingFactor());
    }

    bool startNote(double frequency, double spectralBalance, DsfIntegrationMethod method,
                   int requestedHarmonics = 128, int minimumFactor = 1)
    {
        if (outputRate <= 0.0 || !std::isfinite(frequency) || frequency <= 0.0 ||
            frequency >= outputRate * 0.45 || requestedHarmonics < 1 || requestedHarmonics > 4096)
            return false;

        const int retainedHarmonics = static_cast<int>(std::min(
            static_cast<double>(requestedHarmonics), std::floor(outputRate * 0.45 / frequency)));

        const auto choice = chooseDsfOversampling(method, frequency, outputRate, retainedHarmonics);

        if (!choice.has_value() ||
            (minimumFactor != 1 && minimumFactor != 2 && minimumFactor != 4 && minimumFactor != 8))
            return false;

        const int selectedFactor = std::max(choice->factor, minimumFactor);
        std::size_t nextStage = 0;

        for (int factor = selectedFactor; factor > 1; factor /= 2)
            ++nextStage;

        DsfOscillator nextOscillator;

        if (!nextOscillator.prepare(outputRate * static_cast<double>(selectedFactor), outputRate) ||
            !nextOscillator.startNote(frequency, spectralBalance, method, retainedHarmonics))
            return false;

        oversamplers[nextStage]->reset();

        oscillator = nextOscillator;
        activeStage = nextStage;
        currentFrequency = frequency;
        currentBalance = spectralBalance;
        currentPartials = pendingPartials = requestedHarmonics;
        transitionLength = std::max(
            1,
            static_cast<int>(std::ceil(outputRate * static_cast<double>(selectedFactor) * 0.03)));
        transitionRemaining = 0;

        return true;
    }

    // The caller must keep the current factor sufficient for the entire sweep.
    // The continuous reference tone reserves 8x at note start.
    bool updateNote(double frequency, double spectralBalance, int requestedHarmonics = 128)
    {
        if (requestedHarmonics < 1 || requestedHarmonics > 4096)
            return false;
        auto next = oscillator;
        if (!next.updateNote(frequency, spectralBalance, currentPartials))
            return false;
        auto previous = previousOscillator;
        if (transitionRemaining > 0 &&
            !previous.updateNote(frequency, spectralBalance, previousPartials))
            return false;
        oscillator = next;
        previousOscillator = previous;
        currentFrequency = frequency;
        currentBalance = spectralBalance;
        pendingPartials = requestedHarmonics;
        if (transitionRemaining == 0 && pendingPartials != currentPartials)
            beginPartialTransition();
        return true;
    }

    bool render(juce::dsp::AudioBlock<double> output)
    {
        if (outputRate <= 0.0 || output.getNumChannels() != 1 ||
            output.getNumSamples() > static_cast<std::size_t>(maximumBlockSize))
            return false;

        const auto numSamples = output.getNumSamples();

        if (numSamples == 0)
            return true;

        const juce::dsp::AudioBlock<const double> input(silentInput);
        const auto inputBlock = input.getSubBlock(0, numSamples);

        auto &filter = *oversamplers[activeStage];

        auto internalBlock = filter.processSamplesUp(inputBlock);

        auto *samples = internalBlock.getChannelPointer(0);

        for (std::size_t i = 0; i < internalBlock.getNumSamples(); ++i)
        {
            if (transitionRemaining == 0 && pendingPartials != currentPartials)
                beginPartialTransition();
            const double next = oscillator.nextSample();
            if (transitionRemaining > 0)
            {
                const double previous = previousOscillator.nextSample();
                const double mix = static_cast<double>(transitionLength - transitionRemaining + 1) /
                                   static_cast<double>(transitionLength);
                samples[i] = previous + mix * (next - previous);
                --transitionRemaining;
            }
            else
                samples[i] = next;
        }

        filter.processSamplesDown(output);

        return true;
    }

    void stop()
    {
        oscillator.stop();
        previousOscillator.stop();
        transitionRemaining = 0;
        pendingPartials = currentPartials;

        if (oversamplers[activeStage] != nullptr)
            oversamplers[activeStage]->reset();
    }

private:
    // Integer targets, with a continuous fade between phase-aligned spectra.
    // During a fade, keep only the latest requested count and begin the next
    // transition once this one finishes; never discard an audible fade halfway.
    void beginPartialTransition()
    {
        auto next = oscillator;
        if (!next.updateNote(currentFrequency, currentBalance, pendingPartials))
            return;
        previousOscillator = oscillator;
        previousPartials = currentPartials;
        oscillator = next;
        currentPartials = pendingPartials;
        transitionRemaining = transitionLength;
    }

    DsfOscillator oscillator, previousOscillator;
    double currentFrequency = 0.0, currentBalance = 0.0;
    int currentPartials = 128, previousPartials = 128, pendingPartials = 128;
    int transitionRemaining = 0, transitionLength = 1;

    using Oversampler = juce::dsp::Oversampling<double>;

    std::array<std::unique_ptr<Oversampler>, 4> oversamplers;
    std::size_t activeStage = 0;

    double outputRate = 0.0;
    int maximumBlockSize = 0;

    juce::AudioBuffer<double> silentInput;
};

} // namespace rysum
