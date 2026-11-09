#include "DsfIntegrator.h"

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

    std::cout << "DSF integrator tests passed\n";
    return 0;
}
