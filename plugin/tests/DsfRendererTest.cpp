#include "DsfRenderer.h"
#include "DsfReferenceInstrument.h"

#include <cmath>
#include <iostream>
#include <limits>

int main()
{
    rysum::DsfRenderer renderer;

    if (!renderer.prepare(48000.0, 512))
    {
        std::cerr << "Valid preparation failed\n";
        return 1;
    }

    if (renderer.prepare(0.0, 512) || renderer.prepare(48000.0, 0) ||
        renderer.prepare(std::numeric_limits<double>::quiet_NaN(), 512))
    {
        std::cerr << "Invalid preparation accepted\n";
        return 1;
    }

    // Preparing again must also be supported.
    if (!renderer.prepare(44100.0, 256))
    {
        std::cerr << "Repeated preparation failed\n";
        return 1;
    }

    const auto method = rysum::DsfIntegrationMethod::trapezoidal;

    rysum::DsfRenderer unprepared;

    if (unprepared.startNote(440.0, 0.5, method))
    {
        std::cerr << "Unprepared renderer accepted a note\n";
        return 1;
    }

    if (!renderer.startNote(440.0, 0.5, method))
    {
        std::cerr << "Valid note start failed\n";
        return 1;
    }

    if (renderer.startNote(0.0, 0.5, method) || renderer.startNote(440.0, 1.5, method) ||
        renderer.startNote(440.0, 0.5, method, 0))
    {
        std::cerr << "Invalid note accepted\n";
        return 1;
    }

    rysum::DsfRenderer silentRenderer;

    if (!silentRenderer.prepare(48000.0, 512))
        return 1;

    juce::AudioBuffer<double> silence(1, 512);
    juce::dsp::AudioBlock<double> silenceBlock(silence);

    // Fill with a nonzero value to confirm rendering overwrites it.
    for (int i = 0; i < silence.getNumSamples(); ++i)
        silence.setSample(0, i, 1.0);

    if (!silentRenderer.render(silenceBlock))
    {
        std::cerr << "Silent rendering failed\n";
        return 1;
    }

    for (int i = 0; i < silence.getNumSamples(); ++i)
    {
        if (!(std::abs(silence.getSample(0, i)) <= 1.0e-12))
        {
            std::cerr << "Inactive renderer produced sound\n";
            return 1;
        }
    }

    rysum::DsfRenderer wholeRenderer;
    rysum::DsfRenderer splitRenderer;

    if (!wholeRenderer.prepare(48000.0, 512) || !splitRenderer.prepare(48000.0, 512) ||
        !wholeRenderer.startNote(6000.0, 0.8, method) ||
        !splitRenderer.startNote(6000.0, 0.8, method))
    {
        std::cerr << "Rendering test setup failed\n";
        return 1;
    }

    if (wholeRenderer.getOversamplingFactor() != 4)
    {
        std::cerr << "Expected 4x oversampling\n";
        return 1;
    }

    juce::AudioBuffer<double> whole(1, 512);
    juce::AudioBuffer<double> split(1, 512);

    juce::dsp::AudioBlock<double> wholeBlock(whole);
    juce::dsp::AudioBlock<double> splitBlock(split);

    if (!wholeRenderer.render(wholeBlock) ||
        !splitRenderer.render(splitBlock.getSubBlock(0, 137)) ||
        !splitRenderer.render(splitBlock.getSubBlock(137, 375)))
    {
        std::cerr << "Block rendering failed\n";
        return 1;
    }

    double energy = 0.0;

    for (int i = 0; i < whole.getNumSamples(); ++i)
    {
        const double sample = whole.getSample(0, i);
        const double difference = sample - split.getSample(0, i);

        if (!std::isfinite(sample) || !(std::abs(difference) <= 1.0e-10))
        {
            std::cerr << "Rendering depends on block boundaries\n";
            return 1;
        }

        energy += sample * sample;
    }

    if (!(energy > 1.0))
    {
        std::cerr << "Active renderer produced insufficient signal\n";
        return 1;
    }

    wholeRenderer.stop();

    if (!wholeRenderer.render(wholeBlock))
    {
        std::cerr << "Rendering after stop failed\n";
        return 1;
    }

    for (int i = 0; i < whole.getNumSamples(); ++i)
    {
        if (!(std::abs(whole.getSample(0, i)) <= 1.0e-12))
        {
            std::cerr << "Stopped renderer produced sound\n";
            return 1;
        }
    }

    struct RenderingCase
    {
        double frequency;
        int harmonics;
        int expectedFactor;
    };

    const RenderingCase cases[] = {{440.0, 1, 1}, {2000.0, 3, 2}, {6000.0, 3, 4}, {7000.0, 3, 8}};

    for (const auto &test : cases)
    {
        rysum::DsfRenderer wholePath;
        rysum::DsfRenderer splitPath;

        if (!wholePath.prepare(48000.0, 512) || !splitPath.prepare(48000.0, 512) ||
            !wholePath.startNote(test.frequency, 0.8, method, test.harmonics) ||
            !splitPath.startNote(test.frequency, 0.8, method, test.harmonics))
        {
            std::cerr << "Oversampling case setup failed\n";
            return 1;
        }

        if (wholePath.getOversamplingFactor() != test.expectedFactor ||
            splitPath.getOversamplingFactor() != test.expectedFactor)
        {
            std::cerr << "Wrong oversampling factor for " << test.frequency << " Hz\n";
            return 1;
        }

        if (!wholePath.render(wholeBlock) || !splitPath.render(splitBlock.getSubBlock(0, 137)) ||
            !splitPath.render(splitBlock.getSubBlock(137, 375)))
        {
            std::cerr << "Oversampling case rendering failed\n";
            return 1;
        }

        double renderedEnergy = 0.0;

        for (int i = 0; i < whole.getNumSamples(); ++i)
        {
            const double sample = whole.getSample(0, i);
            const double difference = sample - split.getSample(0, i);

            if (!std::isfinite(sample) || !(std::abs(difference) <= 1.0e-10))
            {
                std::cerr << "Invalid or discontinuous output at " << test.expectedFactor << "x\n";
                return 1;
            }

            renderedEnergy += sample * sample;
        }

        if (!(renderedEnergy > 1.0))
        {
            std::cerr << "Insufficient signal at " << test.expectedFactor << "x\n";
            return 1;
        }
    }

    rysum::DsfReferenceTuning tuning;
    tuning.tuning = rysum::makeKeyTuning(0);
    if (!(std::abs(rysum::referenceNoteHz(tuning, 69, 1) - rysum::kA4) <= 1.0e-8))
    {
        std::cerr << "Original reference is not nominal meantone\n";
        return 1;
    }
    for (const int note : {80, 82, 84, 96, 108})
        if (!(std::abs(rysum::referenceNoteHz(tuning, note, 1) - rysum::meantoneHz(note)) <=
              1.0e-6))
        {
            std::cerr << "Nominal reference rejected a note outside the sample compass\n";
            return 1;
        }
    tuning.pitchCents = 1200.0f;
    if (!(std::abs(rysum::referenceNoteHz(tuning, 69, 1) - 2.0 * rysum::kA4) <= 1.0e-8))
        return 1;
    tuning.pitchCents = 0.0f;
    struct TestNoteTable
    {
        std::array<float, 16 * 128> hz{};
        std::array<std::uint8_t, 16 * 128> mapped{};
    } custom;
    custom.hz[128 + 69] = 432.0f;
    custom.mapped[128 + 69] = 1;
    tuning.noteHz = custom.hz.data();
    tuning.mapped = custom.mapped.data();
    if (!(std::abs(rysum::referenceNoteHz(tuning, 69, 2) - 432.0) <= 1.0e-8) ||
        !(std::abs(rysum::referenceNoteHz(tuning, 69, 1)) <= 1.0e-8))
        return 1;
    tuning.mpe = true;
    custom.hz[69] = 441.0f;
    custom.mapped[69] = 1;
    if (!(std::abs(rysum::referenceNoteHz(tuning, 69, 2) - 441.0) <= 1.0e-8))
        return 1;
    tuning.noteHz = nullptr;
    tuning.mapped = nullptr;
    tuning.mpe = false;

    rysum::DsfReferenceInstrument instrument;
    instrument.prepare(48000.0);
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer events;
    auto play = [&](bool enabled, bool midiMode, double hz, double cents, double brightness,
                    rysum::DsfIntegrationMethod integration)
    {
        audio.clear();
        instrument.process(audio, events, tuning, enabled, midiMode, hz, cents, brightness, -24.0,
                           integration, false);
        events.clear();
        for (int c = 0; c < audio.getNumChannels(); ++c)
            for (int i = 0; i < audio.getNumSamples(); ++i)
                if (!std::isfinite(audio.getSample(c, i)))
                    return false;
        return true;
    };
    events.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 256);
    if (!play(true, true, 1000.0, 0.0, 0.0, method))
        return 1;
    for (int i = 0; i < 256; ++i)
        if (!(std::abs(audio.getSample(0, i)) <= 1.0e-12f))
        {
            std::cerr << "Reference sounded before MIDI note-on\n";
            return 1;
        }
    if (!(std::abs(instrument.getFrequency() - rysum::kA4) <= 1.0e-6))
        return 1;
    events.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    events.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
    if (!play(true, true, 1000.0, 1200.0, 0.8, method) ||
        !(std::abs(instrument.getFrequency() - 2.0 * rysum::kA4) <= 1.0e-6))
        return 1;
    events.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
    for (int i = 0; i < 4; ++i)
        if (!play(true, true, 1000.0, 0.0, 0.8, method))
            return 1;
    if (!(audio.getMagnitude(0, audio.getNumSamples()) <= 1.0e-12f))
    {
        std::cerr << "Reference hung after sustain release\n";
        return 1;
    }
    int upwardCrossings = 0;
    float previous = 0.0f;
    // Settle the manual reference, then count cycles over exactly one second.
    for (int block = 0; block < 98; ++block)
    {
        if (!play(true, false, 1000.0, 0.0, 0.0, method))
            return 1;
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            const int sampleIndex = block * 512 + i;
            const float value = audio.getSample(0, i);
            if (sampleIndex >= 2048 && sampleIndex < 50048 && previous < 0.0f && value >= 0.0f)
                ++upwardCrossings;
            previous = value;
        }
    }
    if (std::abs(upwardCrossings - 1000) > 1)
    {
        std::cerr << "Manual reference frequency is incorrect\n";
        return 1;
    }
    for (int i = 0; i < 8; ++i)
        if (!play(true, false, 500.0 + 50.0 * i, 0.0, 0.1 * i,
                  rysum::DsfIntegrationMethod::rectangular))
            return 1;
    for (int i = 0; i < 4; ++i)
        if (!play(false, false, 1000.0, 0.0, 0.8, method))
            return 1;
    if (!(audio.getMagnitude(0, audio.getNumSamples()) <= 1.0e-12f))
    {
        std::cerr << "Reference did not stop\n";
        return 1;
    }

    events.addEvent(juce::MidiMessage::noteOn(1, 96, static_cast<juce::uint8>(100)), 0);
    const double partialOffset = 1200.0 * std::log2(3.0) + 0.01;
    for (int block = 0; block < 4; ++block)
        if (!play(true, true, 1000.0, partialOffset, 0.0, method))
            return 1;
    const double expectedPartial = rysum::meantoneHz(96) * 3.0 * std::exp2(0.01 / 1200.0);
    if (!(std::abs(instrument.getFrequency() - expectedPartial) < 1.0e-6) ||
        !(audio.getMagnitude(0, audio.getNumSamples()) > 0.001f))
    {
        std::cerr << "High MIDI note or harmonic reference is silent/incorrect\n";
        return 1;
    }

    // A 20 Hz reference must retain audible harmonics above the old 128 limit.
    // Measure harmonic 200 (4000 Hz) over one second after the gain/filter settle.
    rysum::DsfReferenceTone lowTone;
    lowTone.prepare(48000.0);
    juce::AudioBuffer<float> lowAudio(1, 500);
    double real200 = 0.0, imag200 = 0.0;
    const double twoPi = 2.0 * std::acos(-1.0);
    for (int block = 0; block < 106; ++block)
    {
        lowAudio.clear();
        lowTone.process(lowAudio, true, 20.0, 1.0, -24.0, method, false);
        if (block < 10)
            continue;
        for (int i = 0; i < 500; ++i)
        {
            const double sample = lowAudio.getSample(0, i);
            if (!std::isfinite(sample))
                return 1;
            const int sampleIndex = (block - 10) * 500 + i;
            const double angle = twoPi * 4000.0 * static_cast<double>(sampleIndex) / 48000.0;
            real200 += sample * std::cos(angle);
            imag200 += sample * std::sin(angle);
        }
    }
    const double harmonic200 = 2.0 * std::hypot(real200, imag200) / 48000.0;
    if (!(harmonic200 > 0.0002 && harmonic200 < 0.0004))
    {
        std::cerr << "Low reference lost its upper harmonics: " << harmonic200 << "\n";
        return 1;
    }

    real200 = imag200 = 0.0;
    for (int block = 0; block < 106; ++block)
    {
        lowAudio.clear();
        lowTone.process(lowAudio, true, 20.0, 1.0, -24.0, method, false, 0, -1, 1);
        if (block < 10)
            continue;
        for (int i = 0; i < 500; ++i)
        {
            const double sample = lowAudio.getSample(0, i);
            const int sampleIndex = (block - 10) * 500 + i;
            const double angle = twoPi * 4000.0 * static_cast<double>(sampleIndex) / 48000.0;
            real200 += sample * std::cos(angle);
            imag200 += sample * std::sin(angle);
        }
    }
    if (!(2.0 * std::hypot(real200, imag200) / 48000.0 < 1.0e-7))
    {
        std::cerr << "Partial limit 1 did not remove upper harmonics\n";
        return 1;
    }

    // At 1x there is no decimator delay: verify the entire crossfade against
    // independent, phase-aligned oscillators, including a queued count change.
    rysum::DsfRenderer fading;
    rysum::DsfOscillator onePartial, fourPartials, twoPartials;
    if (!fading.prepare(48000.0, 512) || !fading.startNote(600.0, 0.8, method, 1) ||
        fading.getOversamplingFactor() != 1)
        return 1;
    for (auto *oscillator : {&onePartial, &fourPartials, &twoPartials})
        if (!oscillator->prepare(48000.0, 48000.0))
            return 1;
    if (!onePartial.startNote(600.0, 0.8, method, 1) ||
        !fourPartials.startNote(600.0, 0.8, method, 4) ||
        !twoPartials.startNote(600.0, 0.8, method, 2) || !fading.render(wholeBlock))
        return 1;
    for (int i = 0; i < 512; ++i)
    {
        onePartial.nextSample();
        fourPartials.nextSample();
        twoPartials.nextSample();
    }
    if (!fading.updateNote(600.0, 0.8, 4))
        return 1;
    for (int block = 0; block < 6; ++block)
    {
        if (block == 1 && !fading.updateNote(600.0, 0.8, 2))
            return 1;
        if (!fading.render(wholeBlock))
            return 1;
        for (int i = 0; i < 512; ++i)
        {
            const int elapsed = block * 512 + i;
            const double one = onePartial.nextSample();
            const double four = fourPartials.nextSample();
            const double two = twoPartials.nextSample();
            double expectedSample;
            if (elapsed < 1440)
                expectedSample = one + (four - one) * static_cast<double>(elapsed + 1) / 1440.0;
            else
                expectedSample =
                    four +
                    (two - four) * std::min(1.0, static_cast<double>(elapsed - 1440 + 1) / 1440.0);
            if (!(std::abs(whole.getSample(0, i) - expectedSample) <= 1.0e-9))
            {
                std::cerr << "Partial count transition was discontinuous or changed phase\n";
                return 1;
            }
        }
    }

    return 0;
}
