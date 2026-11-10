#pragma once

#include <cmath>

namespace rysum
{

enum class DsfIntegrationMethod
{
    rectangular,
    trapezoidal
};

class DsfIntegrator
{
public:
    // phaseStep is the fundamental's phase advance per sample, in radians.
    // Returns false for an invalid method or a frequency outside (0, Nyquist).
    bool configure(DsfIntegrationMethod nextMethod, double phaseStep)
    {
        const double pi = std::acos(-1.0);

        if (!std::isfinite(phaseStep) || phaseStep <= 0.0 || phaseStep >= pi)
            return false;

        double nextGain = 0.0;

        switch (nextMethod)
        {
        case DsfIntegrationMethod::rectangular:
            nextGain = 2.0 * std::sin(phaseStep / 2.0);
            break;

        case DsfIntegrationMethod::trapezoidal:
            nextGain = std::tan(phaseStep / 2.0);
            break;

        default:
            return false;
        }

        method = nextMethod;
        gain = nextGain;

        return true;
    }

    double processSample(double input)
    {
        switch (method)
        {
        case DsfIntegrationMethod::rectangular:
            output += gain * input;
            break;

        case DsfIntegrationMethod::trapezoidal:
            output += gain * (previousInput + input);
            break;
        }

        previousInput = input;

        return output;
    }
    
    void reset(
        double initialOutput = 0.0,
        double initialPreviousInput = 0.0)
    {
        output = initialOutput;
        previousInput = initialPreviousInput;
    }

private:
    DsfIntegrationMethod method = DsfIntegrationMethod::trapezoidal;

    double gain = 0.0;
    double output = 0.0;
    double previousInput = 0.0;
};

} // namespace rysum
