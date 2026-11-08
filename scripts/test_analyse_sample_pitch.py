"""Synthetic checks for pitch accuracy, ambiguity and frame selection."""
import unittest

import numpy as np

from analyse_sample_pitch import cents, periodic_pitch, regions, single_window


class PitchEstimatorTests(unittest.TestCase):
    def test_detuned_harmonic_tones_at_multiple_rates(self):
        for rate in (44100, 48000, 96000):
            for nominal in (66.47, 444.5, 1778.0, 3556.0):
                for deviation in (-26.1, 21.5):
                    with self.subTest(rate=rate, nominal=nominal, deviation=deviation):
                        hz = nominal * 2 ** (deviation / 1200)
                        t = np.arange(int(0.7 * rate)) / rate
                        signal = sum(amplitude * np.sin(2 * np.pi * hz * h * t + h * 0.2)
                                     for h, amplitude in ((1, 0.3), (2, 0.8), (3, 0.4), (4, 0.2)))
                        signal += np.random.default_rng(7).normal(0, 0.001, len(t))
                        result = single_window(signal, rate, nominal)
                        self.assertLess(abs(cents(result['spectral_hz'], hz)), 0.2)
                        self.assertLess(abs(cents(result['autocorrelation_hz'], hz)), 3.0)
                        self.assertGreater(result['periodicity'], 0.90)
                        self.assertGreaterEqual(result['harmonics'], 3)

    def test_missing_fundamental_with_harmonic_evidence(self):
        rate, hz = 48000, 220.7
        t = np.arange(33600) / rate
        signal = sum(np.sin(2 * np.pi * hz * h * t) / h for h in (2, 3, 4, 5))
        result = single_window(signal, rate, 220.0)
        self.assertLess(abs(cents(result['spectral_hz'], hz)), 0.2)
        self.assertLess(abs(cents(result['autocorrelation_hz'], hz)), 3.0)

    def test_noise_is_not_confident_periodic_pitch(self):
        signal = np.random.default_rng(9).normal(size=33600)
        _, periodicity = periodic_pitch(signal, 48000, 220)
        self.assertTrue(periodicity is None or periodicity < 0.1)

    def test_regions_remain_within_loop_and_sustain(self):
        pipe = {'attack_end': 1000, 'release': 150000, 'loop_start': 24000, 'loop_end': 120000}
        kind, spans = regions(pipe, 160000, 48000)
        self.assertEqual(kind, 'loop')
        self.assertEqual(len(spans), 5)
        for a, b in spans:
            self.assertGreaterEqual(a, 24000)
            self.assertLessEqual(b, 120000)
            self.assertEqual(b - a, 33600)

    def test_short_sample_does_not_invent_extra_windows(self):
        kind, spans = regions({'attack_end': 4560, 'release': 19680}, 39600, 48000)
        self.assertEqual(kind, 'short_sustain_fallback')
        self.assertEqual(spans, [(4560, 19680)])


if __name__ == '__main__':
    unittest.main()
