#pragma once

#include "Common.h"
#include "DsfReferenceTone.h"

#include <atomic>
#include <cstdint>

namespace rysum
{

// A view of the selected tuning, independent of the sample engine.
struct DsfReferenceTuning
{
    KeyTuning tuning;
    int temperament = 0;
    const std::array<double, kNumKeys> *recordedHz = nullptr;
    const float *noteHz = nullptr;
    const std::uint8_t *mapped = nullptr;
    double pitchCents = 0.0;
    float mpeBendRange = 48.0f;
    bool mpe = false;
};

inline constexpr int kDsfLowestMidiNote = 24;
inline constexpr int kDsfHighestMidiNote = 108;

// Recorded mode supplies one measured frequency per key for the selected register.
inline double referenceNoteHz(const DsfReferenceTuning &params, int note, int channel)
{
    if (note < kDsfLowestMidiNote || note > kDsfHighestMidiNote || channel < 1 || channel > 16)
        return 0.0;
    if (params.recordedHz != nullptr)
    {
        const int key = keyIndexForMidi(note);
        if (key < 0)
            return 0.0;
        const double hz = (*params.recordedHz)[static_cast<std::size_t>(key)];
        return std::isfinite(hz) && hz > 0.0 ? hz : 0.0;
    }
    double hz = 0.0;
    if (params.noteHz != nullptr && params.mapped != nullptr)
    {
        const auto index = static_cast<std::size_t>((params.mpe ? 0 : channel - 1) * 128 + note);
        if (params.mapped[index] == 0)
            return 0.0;
        hz = params.noteHz[index];
    }
    else
    {
        const int key = keyIndexForMidi(note);
        if (key < 0)
        {
            const auto targetCents = steppedTuningCents(params.temperament, note);
            hz = targetCents.has_value()
                     ? kA4 * std::exp2(*targetCents / 1200.0)
                     : meantoneHz(note) *
                           std::exp2(temperamentShift(
                                         params.temperament)[static_cast<std::size_t>(note % 12)] /
                                     1200.0);
            return hz * std::exp2(params.pitchCents / 1200.0);
        }
        const auto k = static_cast<std::size_t>(key);
        const int source = params.tuning.source[k];
        if (source < 0 || source >= kNumKeys)
            return 0.0;
        hz = meantoneHz(kPhysKeys[static_cast<std::size_t>(source)]) *
             std::exp2(params.tuning.cents[k] / 1200.0);
    }
    return std::isfinite(hz) && hz > 0.0 ? hz * std::exp2(params.pitchCents / 1200.0) : 0.0;
}

class DsfReferenceInstrument
{
public:
    void prepare(double sampleRate)
    {
        maximumHz = std::min(20000.0, sampleRate * 0.44);
        lastHz = 440.0;
        tone.prepare(sampleRate);
        notes.fill({});
        sustain.fill(false);
        bend.fill(0.0);
        order = 0;
        shownHz.store(0.0);
    }

    double getFrequency() const
    {
        return shownHz.load();
    }

    void process(juce::AudioBuffer<float> &output, const juce::MidiBuffer &midi,
                 const DsfReferenceTuning &params, bool enabled, bool followMidi, double manualHz,
                 double offsetCents, double balance, double levelDb, DsfIntegrationMethod method,
                 bool ambisonic, int partials = 1024)
    {
        auto render = [&](int start, int count)
        {
            int newest = -1;
            std::uint64_t newestOrder = 0;
            if (followMidi)
                for (std::size_t i = 0; i < notes.size(); ++i)
                    if (notes[i].order > newestOrder && (notes[i].held || notes[i].sustained))
                    {
                        newest = static_cast<int>(i);
                        newestOrder = notes[i].order;
                    }
            double hz = manualHz;
            if (followMidi)
            {
                hz = newest < 0 ? 0.0 : referenceNoteHz(params, newest % 128, newest / 128 + 1);
                double cents = offsetCents;
                if (params.mpe && newest >= 128)
                    cents +=
                        bend[static_cast<std::size_t>(newest / 128)] * params.mpeBendRange * 100.0;
                hz *= std::exp2(cents / 1200.0);
            }
            const bool sounding =
                enabled && std::isfinite(hz) && hz >= kDsfMinimumHz && hz <= maximumHz;
            shownHz.store(sounding ? hz : 0.0);
            // A released/unmapped MIDI note fades at its previous frequency.
            tone.process(output, sounding, sounding ? hz : lastHz, balance, levelDb, method,
                         ambisonic, start, count, partials);
            if (sounding)
                lastHz = hz;
        };

        int position = 0;
        for (const auto meta : midi)
        {
            const int at = juce::jlimit(position, output.getNumSamples(), meta.samplePosition);
            if (at > position)
                render(position, at - position);
            position = at;
            const auto message = meta.getMessage();
            const auto channel =
                static_cast<std::size_t>(juce::jlimit(1, 16, message.getChannel()) - 1);
            if (message.isNoteOn())
                notes[channel * 128 + static_cast<std::size_t>(message.getNoteNumber())] = {
                    ++order, true, false};
            else if (message.isNoteOff())
            {
                auto &note =
                    notes[channel * 128 + static_cast<std::size_t>(message.getNoteNumber())];
                note.held = false;
                note.sustained = sustain[channel] || (params.mpe && sustain[0]);
            }
            else if (message.isPitchWheel())
                bend[channel] = juce::jlimit(
                    -1.0, 1.0, static_cast<double>(message.getPitchWheelValue() - 8192) / 8191.0);
            else if (message.isController())
            {
                const int cc = message.getControllerNumber();
                if (cc == 64 || cc == 121)
                {
                    sustain[channel] = cc == 64 && message.getControllerValue() >= 64;
                    if (cc == 121)
                        bend[channel] = 0.0;
                    if (!sustain[channel])
                        for (std::size_t i = 0; i < notes.size(); ++i)
                            if (i / 128 == channel || (params.mpe && channel == 0))
                                if (!sustain[i / 128] && !(params.mpe && sustain[0]))
                                    notes[i].sustained = false;
                }
                if (cc == 120 || cc == 123)
                    for (std::size_t i = 0; i < notes.size(); ++i)
                        if (i / 128 == channel || (params.mpe && channel == 0))
                            notes[i] = {};
            }
        }
        if (position < output.getNumSamples())
            render(position, output.getNumSamples() - position);
    }

private:
    struct Note
    {
        std::uint64_t order = 0;
        bool held = false, sustained = false;
    };
    std::array<Note, 16 * 128> notes{};
    std::array<bool, 16> sustain{};
    std::array<double, 16> bend{};
    std::uint64_t order = 0;
    DsfReferenceTone tone;
    std::atomic<double> shownHz{0.0};
    double lastHz = 440.0;
    double maximumHz = 20000.0;
};

} // namespace rysum
