# Final WAV pitch analysis

This audit analyses the 287 final pipe WAVs referenced by `rysum-samples/rysum_samples.json`. Mechanical noises and wind recordings are excluded: they are not pitched pipe assets. The derived Praestant 16′ has no additional WAVs.

## Method and limits

The analysis uses final PCM16 audio, five windows of up to 700 ms per single-rank pipe, preferentially within its sustain loop. A Hann-windowed, zero-padded spectrum supplies interpolated harmonic peaks; a robust harmonic fit is cross-checked against normalized autocorrelation. Both searches use a ±100-cent neighbourhood of the declared nominal pitch. This cannot discover a wrongly labelled note or reliably exclude octave errors outside that range.

Window spread is the 95th–5th percentile of window estimates, not a confidence interval. Overlapping windows are correlated. `candidate` means the automatic checks passed; it does not mean approved or exact. The method does not remove vibrato, drift, beating or inharmonicity. Independent listening and measurement remain necessary for reviewed entries and compound stops.

Review thresholds: fewer than 3 usable windows; median periodicity below 0.85; window spread above 5 cents; median method disagreement above 3 cents; fewer than 3 resolved harmonics in any usable window; difference from legacy measurement above 5 cents; or a short-sustain fallback.

All results are observations only. Original files and the original manifest remain unchanged. JSON includes WAV hashes, frame regions, individual window estimates, analysis settings and tool versions. CSV is the sortable summary.

Runtime single-rank calibration choices are stored separately as `calibrated_hz` in the manifest. Compound-register review is ongoing.

## Summary

Median and range below include automatic candidates only, excluding unresolved or flagged estimates.

| Stop | Files | Candidates | Review / unresolved | Candidate median deviation (ct) | Candidate range (ct) |
|---|---:|---:|---:|---:|---:|
| praestant | 41 | 37 | 4 | -1.99 | -4.95 … +1.57 |
| gedackt | 41 | 11 | 30 | -2.34 | -4.86 … +0.24 |
| oktave4 | 41 | 28 | 13 | -0.93 | -3.86 … +0.81 |
| oktave2 | 41 | 21 | 20 | -2.28 | -9.37 … +2.22 |
| sesquialtera | 41 | 0 | 41 | unresolved compound | — |
| mixtur | 41 | 0 | 41 | unresolved compound | — |
| trompete | 41 | 41 | 0 | +12.76 | -2.97 … +25.32 |

## Findings and review priorities

Trompete has 41 automatic candidates, with a consistently sharp median reference. See the summary for measured values. This supports the retained-recorded-deviation explanation; it does not establish the original organ builder’s or performer’s intent.

Gedackt MIDI 53 has a short sustain and only one analysis window. Gedackt MIDI 54 has weak loop periodicity and large disagreement between estimators: its numerical estimate is not reliable calibration. The current loader substitutes both pipes under its existing short/unsteady-sustain rules. Their raw-file results must not be mistaken for their substituted playback pitches.

Many Gedackt and upper-octave pipes trigger the conservative fewer-than-three-harmonics rule. A flute-like near-sinusoidal recording can still have a useful pitch estimate; this flag requests review rather than establishing a faulty recording. Seven single-rank files differ by more than five cents from the legacy measurement. Review the selected region and old YIN estimate rather than automatically accepting either value.

## Single-rank pipe results

Frequency estimates below are in Hz. Positive deviation means sharp relative to the declared original nominal pitch. ‘Legacy Δ’ compares this audit with the manifest’s existing measurement.

| Stop / MIDI key | Estimate (Hz) | Deviation (ct) | Legacy Δ (ct) | Spread (ct) | Status / flags |
|---|---:|---:|---:|---:|---|
| praestant / 36 | 66.4200 | -1.30 | -3.67 | 0.56 | candidate: automatic checks passed |
| praestant / 41 | 88.6918 | -4.08 | -1.84 | 0.62 | candidate: automatic checks passed |
| praestant / 38 | 74.2421 | -1.75 | -7.26 | 0.73 | review: differs_from_legacy_measurement |
| praestant / 43 | 99.2818 | -1.99 | -3.64 | 0.72 | candidate: automatic checks passed |
| praestant / 40 | 82.9945 | -1.91 | -1.91 | 0.35 | candidate: automatic checks passed |
| praestant / 45 | 111.0764 | -0.76 | -3.31 | 0.28 | candidate: automatic checks passed |
| praestant / 46 | 118.9317 | +0.45 | -5.14 | 0.46 | review: differs_from_legacy_measurement |
| praestant / 47 | 123.9819 | -3.67 | -3.20 | 0.86 | candidate: automatic checks passed |
| praestant / 48 | 132.8648 | -0.97 | -4.18 | 0.74 | candidate: automatic checks passed |
| praestant / 49 | 138.8749 | -0.38 | -3.18 | 0.35 | candidate: automatic checks passed |
| praestant / 50 | 148.5943 | -0.46 | -1.02 | 0.10 | candidate: automatic checks passed |
| praestant / 51 | 158.9118 | -1.35 | -3.00 | 0.61 | candidate: automatic checks passed |
| praestant / 52 | 166.1812 | +0.08 | -3.04 | 0.51 | candidate: automatic checks passed |
| praestant / 53 | 177.9619 | +1.57 | -0.70 | 0.52 | candidate: automatic checks passed |
| praestant / 54 | 185.2864 | -4.71 | -2.32 | 0.11 | candidate: automatic checks passed |
| praestant / 55 | 198.5980 | -1.70 | -1.09 | 0.16 | candidate: automatic checks passed |
| praestant / 56 | 207.6076 | -0.89 | -2.15 | 0.24 | candidate: automatic checks passed |
| praestant / 57 | 221.8871 | -2.83 | -2.61 | 0.22 | candidate: automatic checks passed |
| praestant / 58 | 237.6648 | -1.01 | -1.57 | 0.34 | candidate: automatic checks passed |
| praestant / 59 | 248.0048 | -3.38 | -2.11 | 0.22 | candidate: automatic checks passed |
| praestant / 60 | 265.5045 | -2.43 | -1.43 | 0.13 | candidate: automatic checks passed |
| praestant / 61 | 277.3397 | -2.93 | -1.65 | 0.23 | candidate: automatic checks passed |
| praestant / 62 | 296.7926 | -2.78 | -0.99 | 0.23 | candidate: automatic checks passed |
| praestant / 63 | 317.4700 | -3.27 | -1.55 | 0.38 | candidate: automatic checks passed |
| praestant / 64 | 332.0771 | -1.40 | -1.71 | 0.20 | candidate: automatic checks passed |
| praestant / 65 | 355.1292 | -2.31 | -1.54 | 0.38 | candidate: automatic checks passed |
| praestant / 66 | 371.3359 | -1.15 | -1.42 | 0.37 | candidate: automatic checks passed |
| praestant / 67 | 397.4499 | -0.59 | -1.09 | 0.18 | candidate: automatic checks passed |
| praestant / 68 | 414.4962 | -3.89 | -1.53 | 0.33 | candidate: automatic checks passed |
| praestant / 69 | 443.6542 | -3.30 | -1.38 | 0.26 | review: few_resolved_harmonics |
| praestant / 70 | 475.5334 | -0.26 | -1.16 | 0.17 | candidate: automatic checks passed |
| praestant / 71 | 496.8072 | -0.60 | -1.02 | 0.30 | candidate: automatic checks passed |
| praestant / 72 | 531.0287 | -2.37 | -0.56 | 0.23 | candidate: automatic checks passed |
| praestant / 73 | 554.4945 | -3.51 | -1.03 | 0.14 | candidate: automatic checks passed |
| praestant / 74 | 592.8404 | -4.95 | -0.75 | 0.23 | candidate: automatic checks passed |
| praestant / 75 | 635.3792 | -2.08 | -0.95 | 0.21 | review: few_resolved_harmonics |
| praestant / 76 | 663.5540 | -2.96 | -0.50 | 0.69 | candidate: automatic checks passed |
| praestant / 77 | 710.8560 | -0.85 | -0.53 | 0.41 | candidate: automatic checks passed |
| praestant / 78 | 742.0944 | -2.50 | -0.89 | 0.25 | candidate: automatic checks passed |
| praestant / 79 | 793.8619 | -2.85 | -0.48 | 0.44 | candidate: automatic checks passed |
| praestant / 81 | 887.4841 | -2.95 | -1.25 | 1.33 | candidate: automatic checks passed |
| gedackt / 36 | 66.2675 | -5.28 | -8.79 | 1.38 | review: few_resolved_harmonics, differs_from_legacy_measurement |
| gedackt / 41 | 88.8618 | -0.76 | -5.51 | 0.52 | review: few_resolved_harmonics, differs_from_legacy_measurement |
| gedackt / 38 | 74.1295 | -4.37 | -5.05 | 1.08 | review: few_resolved_harmonics, differs_from_legacy_measurement |
| gedackt / 43 | 99.2944 | -1.77 | -1.80 | 1.34 | candidate: automatic checks passed |
| gedackt / 40 | 82.9180 | -3.51 | -1.75 | 1.13 | candidate: automatic checks passed |
| gedackt / 45 | 111.0684 | -0.88 | -3.62 | 0.83 | review: few_resolved_harmonics |
| gedackt / 46 | 118.7406 | -2.34 | -4.32 | 1.39 | candidate: automatic checks passed |
| gedackt / 47 | 124.1853 | -0.83 | -4.23 | 0.54 | review: few_resolved_harmonics |
| gedackt / 48 | 132.6380 | -3.92 | -1.71 | 0.46 | review: few_resolved_harmonics |
| gedackt / 49 | 138.6658 | -2.98 | -3.71 | 1.28 | review: few_resolved_harmonics |
| gedackt / 50 | 148.6546 | +0.24 | -1.52 | 0.63 | candidate: automatic checks passed |
| gedackt / 51 | 158.8146 | -2.41 | -2.80 | 0.56 | review: few_resolved_harmonics |
| gedackt / 52 | 165.8671 | -3.19 | -3.17 | 1.30 | review: few_resolved_harmonics |
| gedackt / 53 | 177.1710 | -6.15 | -9.76 | 0.00 | review: too_few_windows, estimators_disagree, few_resolved_harmonics, differs_from_legacy_measurement, short_sustain_fallback |
| gedackt / 54 | 196.3170 | +95.41 | +106.65 | 0.00 | review: too_few_windows, weak_periodicity, estimators_disagree, few_resolved_harmonics, differs_from_legacy_measurement |
| gedackt / 55 | 198.3625 | -3.75 | -1.47 | 0.33 | candidate: automatic checks passed |
| gedackt / 56 | 207.8202 | +0.89 | -2.02 | 0.53 | review: few_resolved_harmonics |
| gedackt / 57 | 222.0513 | -1.55 | -1.14 | 0.18 | candidate: automatic checks passed |
| gedackt / 58 | 237.3446 | -3.34 | -2.18 | 0.40 | review: few_resolved_harmonics |
| gedackt / 59 | 248.2057 | -1.97 | -2.30 | 1.13 | candidate: automatic checks passed |
| gedackt / 60 | 265.7131 | -1.07 | -3.75 | 0.49 | review: few_resolved_harmonics |
| gedackt / 61 | 277.3726 | -2.73 | -2.07 | 0.62 | candidate: automatic checks passed |
| gedackt / 62 | 296.9562 | -1.82 | -1.43 | 0.33 | candidate: automatic checks passed |
| gedackt / 63 | 318.1878 | +0.64 | -1.73 | 0.17 | review: few_resolved_harmonics |
| gedackt / 64 | 331.8288 | -2.69 | -0.83 | 0.59 | candidate: automatic checks passed |
| gedackt / 65 | 355.5054 | -0.48 | -1.07 | 0.28 | review: few_resolved_harmonics |
| gedackt / 66 | 371.6675 | +0.39 | -1.78 | 0.33 | review: few_resolved_harmonics |
| gedackt / 67 | 397.3933 | -0.84 | -1.63 | 0.35 | review: few_resolved_harmonics |
| gedackt / 68 | 415.4737 | +0.19 | -2.56 | 0.27 | review: few_resolved_harmonics |
| gedackt / 69 | 443.9713 | -2.06 | -1.36 | 0.69 | review: few_resolved_harmonics |
| gedackt / 70 | 475.0759 | -1.93 | -1.46 | 0.21 | review: few_resolved_harmonics |
| gedackt / 71 | 497.4238 | +1.55 | -0.71 | 0.37 | review: few_resolved_harmonics |
| gedackt / 72 | 530.5501 | -3.93 | -2.59 | 0.53 | review: few_resolved_harmonics |
| gedackt / 73 | 555.3641 | -0.80 | -0.98 | 0.45 | review: few_resolved_harmonics |
| gedackt / 74 | 593.6901 | -2.47 | -1.24 | 0.16 | review: few_resolved_harmonics |
| gedackt / 75 | 636.0925 | -0.14 | -0.92 | 0.51 | review: few_resolved_harmonics |
| gedackt / 76 | 662.8293 | -4.86 | -0.61 | 0.47 | candidate: automatic checks passed |
| gedackt / 77 | 709.9757 | -3.00 | -1.08 | 0.81 | review: few_resolved_harmonics |
| gedackt / 78 | 743.1219 | -0.10 | -1.06 | 0.47 | review: few_resolved_harmonics |
| gedackt / 79 | 794.9085 | -0.57 | -0.86 | 0.13 | review: few_resolved_harmonics |
| gedackt / 81 | 888.0596 | -1.83 | -1.25 | 0.58 | review: few_resolved_harmonics |
| oktave4 / 36 | 133.0736 | +1.75 | -4.46 | 1.21 | review: few_resolved_harmonics |
| oktave4 / 41 | 177.4894 | -3.04 | -1.70 | 0.73 | candidate: automatic checks passed |
| oktave4 / 38 | 148.4931 | -1.64 | -1.23 | 0.24 | candidate: automatic checks passed |
| oktave4 / 43 | 198.6871 | -0.92 | -1.06 | 0.48 | candidate: automatic checks passed |
| oktave4 / 40 | 166.2510 | +0.81 | -3.06 | 0.64 | candidate: automatic checks passed |
| oktave4 / 45 | 222.1567 | -0.73 | -2.44 | 0.51 | review: few_resolved_harmonics |
| oktave4 / 46 | 237.8105 | +0.05 | -1.07 | 0.20 | candidate: automatic checks passed |
| oktave4 / 47 | 247.9352 | -3.86 | -0.77 | 0.19 | candidate: automatic checks passed |
| oktave4 / 48 | 265.8017 | -0.50 | -1.09 | 0.08 | candidate: automatic checks passed |
| oktave4 / 49 | 277.5304 | -1.74 | -0.69 | 0.15 | candidate: automatic checks passed |
| oktave4 / 50 | 297.3873 | +0.69 | -0.73 | 0.22 | candidate: automatic checks passed |
| oktave4 / 51 | 317.8225 | -1.35 | -1.70 | 0.09 | candidate: automatic checks passed |
| oktave4 / 52 | 332.3222 | -0.12 | -0.44 | 0.23 | candidate: automatic checks passed |
| oktave4 / 53 | 355.2353 | -1.79 | -1.69 | 0.47 | candidate: automatic checks passed |
| oktave4 / 54 | 371.2727 | -1.45 | -0.80 | 0.36 | candidate: automatic checks passed |
| oktave4 / 55 | 397.3929 | -0.84 | -1.57 | 0.33 | review: few_resolved_harmonics |
| oktave4 / 56 | 415.4709 | +0.18 | -1.25 | 0.16 | candidate: automatic checks passed |
| oktave4 / 57 | 443.9866 | -2.00 | -1.24 | 0.20 | candidate: automatic checks passed |
| oktave4 / 58 | 475.5953 | -0.04 | -1.33 | 0.39 | candidate: automatic checks passed |
| oktave4 / 59 | 496.5757 | -1.40 | -1.18 | 0.06 | review: few_resolved_harmonics |
| oktave4 / 60 | 531.9189 | +0.53 | -0.60 | 0.05 | candidate: automatic checks passed |
| oktave4 / 61 | 555.5022 | -0.37 | -1.12 | 0.25 | candidate: automatic checks passed |
| oktave4 / 62 | 593.5126 | -2.99 | -0.98 | 0.46 | review: few_resolved_harmonics |
| oktave4 / 63 | 636.3765 | +0.64 | -0.98 | 0.20 | candidate: automatic checks passed |
| oktave4 / 64 | 664.5136 | -0.46 | -0.81 | 0.18 | review: few_resolved_harmonics |
| oktave4 / 65 | 710.8421 | -0.89 | -1.02 | 0.32 | candidate: automatic checks passed |
| oktave4 / 66 | 743.3565 | +0.45 | -0.83 | 0.11 | candidate: automatic checks passed |
| oktave4 / 67 | 794.8300 | -0.74 | -0.96 | 0.19 | candidate: automatic checks passed |
| oktave4 / 68 | 830.1204 | -1.54 | -1.20 | 0.67 | candidate: automatic checks passed |
| oktave4 / 69 | 887.6225 | -2.68 | -0.92 | 0.14 | candidate: automatic checks passed |
| oktave4 / 70 | 951.4271 | +0.39 | -0.77 | 0.08 | review: few_resolved_harmonics |
| oktave4 / 71 | 993.2891 | -1.16 | -0.72 | 0.25 | review: few_resolved_harmonics |
| oktave4 / 72 | 1062.0919 | -2.32 | -0.82 | 0.09 | review: few_resolved_harmonics |
| oktave4 / 73 | 1109.5015 | -2.71 | -0.89 | 0.10 | review: few_resolved_harmonics |
| oktave4 / 74 | 1186.9075 | -3.16 | -0.75 | 0.08 | candidate: automatic checks passed |
| oktave4 / 75 | 1271.2516 | -1.41 | -1.22 | 0.15 | candidate: automatic checks passed |
| oktave4 / 76 | 1327.6033 | -2.32 | -1.01 | 0.32 | review: few_resolved_harmonics |
| oktave4 / 77 | 1421.3234 | -1.32 | -1.02 | 0.11 | review: few_resolved_harmonics |
| oktave4 / 78 | 1484.5453 | -2.08 | -1.27 | 0.34 | review: few_resolved_harmonics |
| oktave4 / 79 | 1587.6516 | -2.93 | -1.42 | 0.25 | candidate: automatic checks passed |
| oktave4 / 81 | 1777.0314 | -0.94 | -1.80 | 0.13 | candidate: automatic checks passed |
| oktave2 / 36 | 265.2638 | -4.00 | -0.95 | 0.16 | candidate: automatic checks passed |
| oktave2 / 41 | 354.5899 | -4.94 | -0.28 | 0.16 | candidate: automatic checks passed |
| oktave2 / 38 | 295.6645 | -9.37 | -1.76 | 1.03 | candidate: automatic checks passed |
| oktave2 / 43 | 397.6962 | +0.48 | -1.20 | 0.28 | candidate: automatic checks passed |
| oktave2 / 40 | 332.6903 | +1.80 | -0.72 | 0.24 | candidate: automatic checks passed |
| oktave2 / 45 | 444.0081 | -1.92 | -1.29 | 0.21 | candidate: automatic checks passed |
| oktave2 / 46 | 477.0194 | +5.14 | -1.22 | 0.27 | review: few_resolved_harmonics |
| oktave2 / 47 | 494.6452 | -8.15 | -1.22 | 0.09 | review: few_resolved_harmonics |
| oktave2 / 48 | 532.4388 | +2.22 | -1.46 | 0.07 | candidate: automatic checks passed |
| oktave2 / 49 | 554.1364 | -4.63 | -0.69 | 0.09 | candidate: automatic checks passed |
| oktave2 / 50 | 593.8885 | -1.89 | -1.07 | 0.29 | candidate: automatic checks passed |
| oktave2 / 51 | 634.2442 | -5.18 | -0.98 | 0.49 | candidate: automatic checks passed |
| oktave2 / 52 | 666.0296 | +3.48 | -1.02 | 0.13 | review: few_resolved_harmonics |
| oktave2 / 53 | 709.0858 | -5.17 | -0.83 | 0.07 | candidate: automatic checks passed |
| oktave2 / 54 | 742.6175 | -1.28 | -0.65 | 0.18 | review: few_resolved_harmonics |
| oktave2 / 55 | 795.0028 | -0.37 | -0.88 | 0.21 | candidate: automatic checks passed |
| oktave2 / 56 | 829.8847 | -2.03 | -0.87 | 0.30 | candidate: automatic checks passed |
| oktave2 / 57 | 887.8084 | -2.32 | -0.89 | 0.19 | candidate: automatic checks passed |
| oktave2 / 58 | 950.5639 | -1.18 | -0.65 | 0.14 | review: few_resolved_harmonics |
| oktave2 / 59 | 992.6487 | -2.28 | -0.80 | 0.24 | candidate: automatic checks passed |
| oktave2 / 60 | 1062.6561 | -1.40 | -0.92 | 0.11 | review: few_resolved_harmonics |
| oktave2 / 61 | 1108.4939 | -4.29 | -0.84 | 0.22 | candidate: automatic checks passed |
| oktave2 / 62 | 1187.4525 | -2.36 | -0.67 | 0.26 | candidate: automatic checks passed |
| oktave2 / 63 | 1272.2545 | -0.04 | -0.78 | 0.14 | review: few_resolved_harmonics |
| oktave2 / 64 | 1330.2131 | +1.08 | -1.09 | 0.10 | candidate: automatic checks passed |
| oktave2 / 65 | 1420.6919 | -2.09 | -1.29 | 0.44 | review: few_resolved_harmonics |
| oktave2 / 66 | 1485.8237 | -0.59 | -1.12 | 0.12 | review: few_resolved_harmonics |
| oktave2 / 67 | 1588.6577 | -1.84 | -1.42 | 0.08 | candidate: automatic checks passed |
| oktave2 / 68 | 1660.2001 | -1.58 | -1.36 | 0.04 | candidate: automatic checks passed |
| oktave2 / 69 | 1772.7770 | -5.09 | -1.54 | 0.09 | candidate: automatic checks passed |
| oktave2 / 70 | 1901.8148 | -0.55 | -1.52 | 0.30 | review: few_resolved_harmonics |
| oktave2 / 71 | 1989.6714 | +1.53 | -1.77 | 0.11 | review: few_resolved_harmonics |
| oktave2 / 72 | 2126.6019 | -0.35 | -1.01 | 0.09 | review: few_resolved_harmonics |
| oktave2 / 73 | 2217.0737 | -4.22 | -1.22 | 0.21 | review: few_resolved_harmonics |
| oktave2 / 74 | 2366.8713 | -8.23 | -2.73 | 0.19 | review: few_resolved_harmonics |
| oktave2 / 75 | 2535.8299 | -5.96 | -2.50 | 0.30 | review: few_resolved_harmonics |
| oktave2 / 76 | 2649.4111 | -6.10 | -2.65 | 0.35 | review: few_resolved_harmonics |
| oktave2 / 77 | 2837.9579 | -4.18 | -2.88 | 0.06 | review: few_resolved_harmonics |
| oktave2 / 78 | 2965.0515 | -4.44 | -3.40 | 0.12 | review: few_resolved_harmonics |
| oktave2 / 79 | 3190.3387 | +5.24 | -3.92 | 0.50 | review: few_resolved_harmonics |
| oktave2 / 81 | 3548.1930 | -3.81 | -1.56 | 0.21 | review: few_resolved_harmonics |
| trompete / 36 | 67.2882 | +21.18 | -0.30 | 0.28 | candidate: automatic checks passed |
| trompete / 41 | 89.2730 | +7.23 | -0.21 | 0.36 | candidate: automatic checks passed |
| trompete / 38 | 75.1792 | +19.97 | -0.04 | 0.08 | candidate: automatic checks passed |
| trompete / 43 | 99.8473 | +7.84 | -0.15 | 0.09 | candidate: automatic checks passed |
| trompete / 40 | 83.9490 | +17.89 | -0.37 | 1.66 | candidate: automatic checks passed |
| trompete / 45 | 111.7047 | +9.01 | -0.11 | 0.12 | candidate: automatic checks passed |
| trompete / 46 | 118.8505 | -0.74 | -0.02 | 0.17 | candidate: automatic checks passed |
| trompete / 47 | 124.6512 | +5.65 | -0.22 | 0.62 | candidate: automatic checks passed |
| trompete / 48 | 134.1087 | +15.17 | -0.66 | 0.69 | candidate: automatic checks passed |
| trompete / 49 | 140.8690 | +24.31 | -0.37 | 0.36 | candidate: automatic checks passed |
| trompete / 50 | 149.9809 | +15.62 | -0.10 | 0.07 | candidate: automatic checks passed |
| trompete / 51 | 160.2725 | +13.41 | +0.03 | 0.27 | candidate: automatic checks passed |
| trompete / 52 | 167.7146 | +15.99 | -0.18 | 0.19 | candidate: automatic checks passed |
| trompete / 53 | 179.1739 | +13.32 | -0.14 | 0.16 | candidate: automatic checks passed |
| trompete / 54 | 185.4726 | -2.97 | -0.08 | 0.10 | candidate: automatic checks passed |
| trompete / 55 | 200.0848 | +11.21 | -0.07 | 0.20 | candidate: automatic checks passed |
| trompete / 56 | 209.4007 | +14.00 | -0.07 | 0.13 | candidate: automatic checks passed |
| trompete / 57 | 223.9563 | +13.24 | -0.24 | 0.09 | candidate: automatic checks passed |
| trompete / 58 | 239.3799 | +11.44 | -0.13 | 0.05 | candidate: automatic checks passed |
| trompete / 59 | 249.3345 | +5.88 | -0.03 | 0.14 | candidate: automatic checks passed |
| trompete / 60 | 268.7899 | +18.86 | -0.16 | 0.28 | candidate: automatic checks passed |
| trompete / 61 | 280.2868 | +15.37 | -0.19 | 0.03 | candidate: automatic checks passed |
| trompete / 62 | 297.9983 | +4.24 | -0.20 | 0.24 | candidate: automatic checks passed |
| trompete / 63 | 320.4940 | +13.14 | -0.58 | 0.07 | candidate: automatic checks passed |
| trompete / 64 | 333.7492 | +7.30 | -0.26 | 0.03 | candidate: automatic checks passed |
| trompete / 65 | 358.4915 | +14.01 | +0.26 | 0.07 | candidate: automatic checks passed |
| trompete / 66 | 375.2323 | +16.92 | -0.07 | 0.11 | candidate: automatic checks passed |
| trompete / 67 | 399.8691 | +9.91 | -0.06 | 0.22 | candidate: automatic checks passed |
| trompete / 68 | 418.4870 | +12.70 | -0.96 | 0.08 | candidate: automatic checks passed |
| trompete / 69 | 444.3624 | -0.54 | -0.29 | 0.06 | candidate: automatic checks passed |
| trompete / 70 | 479.1235 | +12.76 | -0.44 | 0.11 | candidate: automatic checks passed |
| trompete / 71 | 504.2997 | +25.32 | -0.75 | 0.10 | candidate: automatic checks passed |
| trompete / 72 | 534.9262 | +10.29 | +0.18 | 0.11 | candidate: automatic checks passed |
| trompete / 73 | 556.4068 | +2.45 | -0.36 | 0.05 | candidate: automatic checks passed |
| trompete / 74 | 598.2724 | +10.84 | -0.37 | 0.25 | candidate: automatic checks passed |
| trompete / 75 | 640.3581 | +11.43 | -0.17 | 0.26 | candidate: automatic checks passed |
| trompete / 76 | 668.0322 | +8.68 | -0.27 | 0.13 | candidate: automatic checks passed |
| trompete / 77 | 715.6149 | +10.70 | -0.15 | 0.37 | candidate: automatic checks passed |
| trompete / 78 | 750.0728 | +16.02 | -0.43 | 0.43 | candidate: automatic checks passed |
| trompete / 79 | 802.0593 | +14.93 | +0.05 | 0.08 | candidate: automatic checks passed |
| trompete / 81 | 897.4096 | +16.30 | +0.09 | 0.33 | candidate: automatic checks passed |

## Compound stops

All 82 compound samples remain `compound_unresolved`. The JSON records up to twelve prominent spectral peaks in a representative sustain window for each. These are not identified ranks or validated fundamentals. Correcting all constituent pitches requires rank identification and, if they differ in tuning, separate rank assets or another decomposition method.

## Reproduction

From the repository root, using Python with NumPy installed:

```sh
python scripts/analyse_sample_pitch.py --samples rysum-samples --out docs/in-progress/pitch-analysis
```

Synthetic estimator checks: `python -m unittest discover -s scripts -p 'test_analyse_sample_pitch.py'`.
