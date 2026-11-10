#pragma once

#include "DsfOscillator.h"

#include <juce_dsp/juce_dsp.h>

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

        return true;
    }

private:
    DsfOscillator oscillator;

    using Oversampler = juce::dsp::Oversampling<double>;

    std::array<std::unique_ptr<Oversampler>, 4> oversamplers;

    double outputRate = 0.0;
    int maximumBlockSize = 0;

    juce::AudioBuffer<double> silentInput;
};

} // namespace rysum
