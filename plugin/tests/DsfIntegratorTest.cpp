#include "DsfIntegrator.h"
#include "DsfMath.h"
#include "DsfOscillator.h"
#include "DsfOversampling.h"

#include <cmath>
#include <iostream>
#include <limits>

int main()
{
    const double pi = std::acos(-1.0);

    rysum::DsfIntegrator integrator;

    if (!integrator.configure(rysum::DsfIntegrationMethod::trapezoidal, pi / 2.0))
    {
        std::cerr << "Configuration failed\n";
        return 1;
    }

    const double inputs[] = {1.0, 0.0, -1.0, 0.0};
    const double expected[] = {1.0, 2.0, 1.0, 0.0};

    for (int i = 0; i < 4; ++i)
    {
        const double actual = integrator.processSample(inputs[i]);

        if (!(std::abs(actual - expected[i]) <= 1.0e-12))
        {
            std::cerr << "Trapezoidal integration failed at sample " << i << ": expected "
                      << expected[i] << ", received " << actual << '\n';
            return 1;
        }
    }

    rysum::DsfIntegrator rectangular;

    if (!rectangular.configure(rysum::DsfIntegrationMethod::rectangular, pi / 2.0))
    {
        std::cerr << "Rectangular configuration failed\n";
        return 1;
    }

    const double rootTwo = std::sqrt(2.0);

    const double rectangularExpected[] = {rootTwo, rootTwo, 0.0, 0.0};

    for (int i = 0; i < 4; ++i)
    {
        const double actual = rectangular.processSample(inputs[i]);

        if (!(std::abs(actual - rectangularExpected[i]) <= 1.0e-12))
        {
            std::cerr << "Rectangular integration failed at sample " << i << '\n';
            return 1;
        }
    }

    // Leave a nonzero input in the trapezoidal integrator's history.
    integrator.processSample(1.0);

    integrator.reset();

    const double afterReset = integrator.processSample(0.0);

    if (!(std::abs(afterReset) <= 1.0e-12))
    {
        std::cerr << "Reset did not clear the integration history\n";
        return 1;
    }

    const double firstAfterReset = integrator.processSample(1.0);

    if (!(std::abs(firstAfterReset - 1.0) <= 1.0e-12))
    {
        std::cerr << "Reset unexpectedly changed the configuration\n";
        return 1;
    }

    const double invalidSteps[] = {0.0, -0.1, pi, std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::quiet_NaN()};

    for (double step : invalidSteps)
    {
        if (integrator.configure(rysum::DsfIntegrationMethod::rectangular, step))
        {
            std::cerr << "Invalid phase step was accepted\n";
            return 1;
        }
    }

    integrator.reset();

    const double afterInvalidConfiguration = integrator.processSample(1.0);

    if (!(std::abs(afterInvalidConfiguration - 1.0) <= 1.0e-12))
    {
        std::cerr << "Invalid configuration changed the integrator\n";
        return 1;
    }

    const double phases[] = {0.0, 1.0e-8, 0.3, pi / 2.0, pi, 2.0 * pi};

    const double balances[] = {0.0, 0.5, 1.0};

    for (double balance : balances)
    {
        for (double phase : phases)
        {
            const double expectedCosine = std::cos(phase) + balance * std::cos(2.0 * phase) +
                                          balance * balance * std::cos(3.0 * phase);

            const double actualCosine = rysum::cosineDsf(phase, balance, 3);

            if (!(std::abs(actualCosine - expectedCosine) <= 1.0e-11))
            {
                std::cerr << "Cosine DSF failed: phase " << phase << ", balance " << balance
                          << ", expected " << expectedCosine << ", received " << actualCosine
                          << '\n';
                return 1;
            }
        }
    }

    // Start at phase zero. The preceding phase was -π/2:
    // sine output = -1; cosine input = 0.
    integrator.reset(-1.0, 0.0);

    const double centredExpected[] = {0.0, 1.0, 0.0, -1.0};

    for (int i = 0; i < 4; ++i)
    {
        const double actual = integrator.processSample(inputs[i]);

        if (!(std::abs(actual - centredExpected[i]) <= 1.0e-12))
        {
            std::cerr << "Centred sine integration failed at sample " << i << '\n';
            return 1;
        }
    }

    const rysum::DsfIntegrationMethod methods[] = {rysum::DsfIntegrationMethod::rectangular,
                                                   rysum::DsfIntegrationMethod::trapezoidal};

    const double sampleRates[] = {44100.0, 48000.0, 96000.0};

    for (auto method : methods)
    {
        for (double rate : sampleRates)
        {
            rysum::DsfOscillator oscillator;

            if (!oscillator.prepare(rate, rate) || !oscillator.startNote(440.0, 0.0, method))
            {
                std::cerr << "Oscillator setup failed\n";
                return 1;
            }

            const double step = 2.0 * pi * 440.0 / rate;

            for (int i = 0; i < 4096; ++i)
            {
                const double expectedSample = std::sin(step * static_cast<double>(i));

                const double actualSample = oscillator.nextSample();

                if (!(std::abs(actualSample - expectedSample) <= 1.0e-9))
                {
                    std::cerr << "Sine oscillator failed at sample " << i << ", sample rate "
                              << rate << ", method " << static_cast<int>(method) << '\n';
                    return 1;
                }
            }
        }
    }

    const double processingRates[] = {48000.0, 192000.0};

    for (auto method : methods)
    {
        for (double rate : processingRates)
        {
            rysum::DsfOscillator oscillator;

            if (!oscillator.prepare(rate, 48000.0) ||
                !oscillator.startNote(6000.0, 0.8, method, 128))
            {
                std::cerr << "Rich oscillator setup failed\n";
                return 1;
            }

            const double step = 2.0 * pi * 6000.0 / rate;

            for (int i = 0; i < 4096; ++i)
            {
                const double phase = step * static_cast<double>(i);

                double expectedSample = 0.0;
                double weight = 1.0;

                for (int n = 1; n <= 3; ++n)
                {
                    const double harmonic = static_cast<double>(n);

                    const double amplitude =
                        method == rysum::DsfIntegrationMethod::rectangular
                            ? std::sin(step / 2.0) / std::sin(harmonic * step / 2.0)
                            : std::tan(step / 2.0) / std::tan(harmonic * step / 2.0);

                    expectedSample += weight * amplitude * std::sin(harmonic * phase);

                    weight *= 0.8;
                }

                const double actualSample = oscillator.nextSample();

                if (!(std::abs(actualSample - expectedSample) <= 1.0e-9))
                {
                    std::cerr << "Rich oscillator failed at sample " << i << ", processing rate "
                              << rate << ", method " << static_cast<int>(method) << '\n';
                    return 1;
                }
            }
        }
    }

    rysum::DsfOscillator lifecycle;

    if (!(std::abs(lifecycle.nextSample()) <= 1.0e-12))
    {
        std::cerr << "Unprepared oscillator was not silent\n";
        return 1;
    }

    if (!lifecycle.prepare(48000.0, 48000.0) ||
        !lifecycle.startNote(440.0, 0.8, rysum::DsfIntegrationMethod::trapezoidal))
    {
        std::cerr << "Lifecycle setup failed\n";
        return 1;
    }

    // Advance beyond the initial zero-valued sample.
    for (int i = 0; i < 100; ++i)
        lifecycle.nextSample();

    lifecycle.stop();

    if (!(std::abs(lifecycle.nextSample()) <= 1.0e-12))
    {
        std::cerr << "Stopped oscillator was not silent\n";
        return 1;
    }

    if (!lifecycle.startNote(440.0, 0.8, rysum::DsfIntegrationMethod::trapezoidal) ||
        !(std::abs(lifecycle.nextSample()) <= 1.0e-12))
    {
        std::cerr << "Restart did not restore the starting state\n";
        return 1;
    }

    const double errorStep = pi / 4.0;

    const double rectangularError =
        rysum::integrationErrorDb(rysum::DsfIntegrationMethod::rectangular, errorStep, 2);

    const double trapezoidalError =
        rysum::integrationErrorDb(rysum::DsfIntegrationMethod::trapezoidal, errorStep, 2);

    const double expectedRectangularError =
        20.0 * std::log10(std::sqrt(4.0 - 2.0 * std::sqrt(2.0)));

    const double expectedTrapezoidalError = 20.0 * std::log10(2.0 * (std::sqrt(2.0) - 1.0));

    if (!(std::abs(rectangularError - expectedRectangularError) <= 1.0e-12) ||
        !(std::abs(trapezoidalError - expectedTrapezoidalError) <= 1.0e-12))
    {
        std::cerr << "Integration error calculation failed\n";
        return 1;
    }

    const auto selected = rysum::chooseDsfOversampling(rysum::DsfIntegrationMethod::trapezoidal,
                                                       6000.0, 48000.0, 3, 0.25);

    if (!selected.has_value() || selected->factor != 4 || !selected->meetsTolerance ||
        !(std::abs(selected->errorDb) <= 0.25))
    {
        std::cerr << "Oversampling selection failed\n";
        return 1;
    }

    const auto fallback = rysum::chooseDsfOversampling(rysum::DsfIntegrationMethod::trapezoidal,
                                                       6000.0, 48000.0, 3, 1.0e-6);

    if (!fallback.has_value() || fallback->factor != 8 || fallback->meetsTolerance ||
        !(std::abs(fallback->errorDb) > 1.0e-6))
    {
        std::cerr << "Oversampling fallback failed\n";
        return 1;
    }

    const auto invalidSpectrum =
        rysum::chooseDsfOversampling(rysum::DsfIntegrationMethod::trapezoidal, 6000.0, 48000.0, 4);

    if (invalidSpectrum.has_value())
    {
        std::cerr << "A partial at output Nyquist was accepted\n";
        return 1;
    }

    std::cout << "DSF integrator tests passed\n";
    return 0;
}
