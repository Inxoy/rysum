#pragma once

#include <cmath>
#include <complex>

namespace rysum
{

// finite integrated-DSF (Discrete Summation Formula) reference sum
// phase: radians
// balance: 0 = sine, 1 = harmonics weighted by 1/n
// harmonics: number of harmonic partials, including fundamental
//
// this function does not apply output normalisation or a Nyquist limit
// the oscillator will handle those separately

inline double integratedDsf(double phase, double balance, int harmonics)
{
    if (!std::isfinite(phase) || !std::isfinite(balance) || balance < 0.0 || balance > 1.0 ||
        harmonics < 1)
        return 0.0;

    double sum = 0.0;
    double harmonicWeight = 1.0;

    for (int n = 1; n <= harmonics && harmonicWeight > 0.0; ++n)
    {
        sum += (harmonicWeight / static_cast<double>(n)) * std::sin(static_cast<double>(n) * phase);

        harmonicWeight *= balance;
    }

    return sum;
}

inline double cosineDsf(double phase, double balance, int harmonics)
{
    if (!std::isfinite(phase) || !std::isfinite(balance) || balance < 0.0 || balance > 1.0 ||
        harmonics < 1)
        return 0.0;

    const std::complex<double> phasePoint(std::cos(phase), std::sin(phase));

    const auto ratio = balance * phasePoint;
    const auto denominator = 1.0 - ratio;

    // Near the geometric formula's singularity, use the finite sum to avoid
    // division by zero and loss of numerical accuracy.

    if (std::abs(denominator) < 1.0e-6)
    {
        double sum = 0.0;
        double weight = 1.0;

        for (int n = 1; n <= harmonics; ++n)
        {
            sum += weight * std::cos(static_cast<double>(n) * phase);

            weight *= balance;
        }

        return sum;
    }

    const auto numerator = 1.0 - std::pow(ratio, harmonics);

    return (phasePoint * numerator / denominator).real();
}

} // namespace rysum
