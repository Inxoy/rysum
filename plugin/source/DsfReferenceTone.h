#pragma once

#include "DsfRenderer.h"

namespace rysum
{

// A continuous, dry reference for comparing sample pitch by ear. All state is
// audio-thread owned; prepare is the only method that allocates.
class DsfReferenceTone
{
public:
    void prepare(double sampleRate)
    {
        rate = sampleRate;
        renderer.prepare(sampleRate, chunkSize);
        scratch.setSize(1, chunkSize);
        gain.reset(sampleRate, 0.02);
        gain.setCurrentAndTargetValue(0.0);
        frequency.reset(sampleRate, 0.03);
        spectrum.reset(sampleRate, 0.03);
        running = false;
    }

    void process(juce::AudioBuffer<float> &output, bool enabled, double hz, double brightness,
                 double levelDb, DsfIntegrationMethod method, bool ambisonic, int startSample = 0,
                 int numSamples = -1, int partials = 1024)
    {
        if (rate <= 0.0 || output.getNumChannels() == 0)
            return;

        if (numSamples < 0)
            numSamples = output.getNumSamples() - startSample;
        if (startSample < 0 || numSamples < 0 || startSample > output.getNumSamples() - numSamples)
            return;

        partials = juce::jlimit(1, 1024, partials);
        hz = juce::jlimit(10.0, std::min(20000.0, rate * 0.44), hz);
        brightness = juce::jlimit(0.0, 1.0, brightness);
        frequency.setTargetValue(hz);
        spectrum.setTargetValue(brightness);
        // Fade out before replacing the integration method/filter state.
        const bool changingMethod = running && method != currentMethod;
        gain.setTargetValue(enabled && !changingMethod
                                ? juce::Decibels::decibelsToGain(juce::jlimit(-60.0, -6.0, levelDb))
                                : 0.0);

        for (int start = 0; start < numSamples; start += chunkSize)
        {
            const int count = std::min(chunkSize, numSamples - start);
            if (running && !gain.isSmoothing() && gain.getCurrentValue() <= 0.0)
            {
                renderer.stop();
                running = false;
            }
            if (!running)
            {
                if (!enabled)
                    continue;
                frequency.setCurrentAndTargetValue(hz);
                spectrum.setCurrentAndTargetValue(brightness);
                currentMethod = method;
                // Reserve the maximum factor for live frequency/spectrum sweeps.
                running =
                    renderer.startNote(hz, dsfBrightnessToBalance(brightness), method, partials, 8);
                if (!running)
                    continue;
                gain.setTargetValue(
                    juce::Decibels::decibelsToGain(juce::jlimit(-60.0, -6.0, levelDb)));
            }

            // Rebuilding the integration constant costs O(partials). Only do it
            // during control ramps; a steady tone keeps the closed-form path.
            const bool retarget =
                frequency.isSmoothing() || spectrum.isSmoothing() || partials != currentPartials;
            const double nextHz = frequency.skip(count);
            const double nextBalance = dsfBrightnessToBalance(spectrum.skip(count));
            if (retarget)
                renderer.updateNote(nextHz, nextBalance, partials);
            currentPartials = partials;
            auto block = juce::dsp::AudioBlock<double>(scratch).getSubBlock(
                0, static_cast<std::size_t>(count));
            if (!renderer.render(block))
                continue;
            for (int i = 0; i < count; ++i)
            {
                const float sample =
                    static_cast<float>(scratch.getSample(0, i) * gain.getNextValue());
                if (ambisonic)
                    output.addSample(0, startSample + start + i, sample); // W: omnidirectional
                else
                    for (int channel = 0; channel < std::min(2, output.getNumChannels()); ++channel)
                        output.addSample(channel, startSample + start + i, sample);
            }
        }
    }

private:
    static constexpr int chunkSize = 32;
    int currentPartials = 1024;
    DsfRenderer renderer;
    juce::AudioBuffer<double> scratch;
    juce::SmoothedValue<double> gain, frequency, spectrum;
    double rate = 0.0;
    bool running = false;
    DsfIntegrationMethod currentMethod = DsfIntegrationMethod::trapezoidal;
};

} // namespace rysum
