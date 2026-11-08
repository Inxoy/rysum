# Recorded pitch and corrected tuning: implementation review

This is a design review of the current `plainsound/macos` source, prepared on 7 November 2026. It proposes future changes; the audit does not retune samples or change playback.

## Intended behaviour

Original recorded tuning preserves the actual recorded pitches and their differences between registers. Selecting another temperament, a step scale, Scala, MTS-ESP or MIDI Tuning Standard should instead produce the requested frequencies, subject to validated measurements and the inherent stability of each recording. Pitch bend and explicitly enabled expressive effects are applied afterwards.

No intermediate conversion of WAVs to 12-EDO is needed. Preserve the recordings and compute their playback rates from measured source pitch and the requested destination pitch.

## Current data and playback

`samples/rysum_samples.json` contains 287 recordings: seven stops with 41 pipes each. Every entry has `measured_hz`, but compound-stop measurements are not rank identifications. Praestant 16′ is generated from Praestant 8′ at half speed and has no additional WAVs.

`scripts/rysum_preprocess.py::measure_f0` uses a median YIN estimate between 45 and 4800 Hz, taken from a portion of the source recording before final cleaning and WAV export. It stores no estimator confidence or temporal pitch spread. Compound registers deliberately receive no `cents_vs_nominal`.

`SampleLibrary::load` reads the measurements, but normal playback does not use them for pitch correction. `OrganEngine::slotHz` is initialized from fixed quarter-comma meantone frequencies with A = 444.5 Hz. `centsFor` compares the requested frequency with that nominal reference, shared across registers. Playback therefore retains the source recordings' deviations.

The legacy Trompete metadata has a median deviation of +13.4 cents, ranging from −2.9 to +26.1 cents. The final-WAV audit independently checks these observations; see [the analysis](pitch-analysis/final-wav-pitch-analysis.md).

## Gaps to fix

1. **Final-asset measurement.** Measure final WAV sustain and loop regions. Store the WAV hash, sample rate, frame ranges, estimator and version, pitch stability, agreement between methods and review disposition. Detect octave/partial ambiguity instead of silently accepting a number. Preserve manual decisions and original measurements.
2. **Register meaning.** Separate keyboard reference pitch from rank pitch. An Octave 4′ must remain an octave above the keyboard reference; an actual pipe fundamental must not be mistaken for an 8′ reference. Compound ranks need explicit identities and ratios, potentially varying with key and breakback.
3. **Per-voice correction.** Replace shared nominal-key offsets with a calculation using the selected source recording, its measured reference and the destination register ratio. This must apply separately to every sounding register voice.
4. **Explicit tuning policy.** Distinguish original recorded playback from corrected target tuning. The existing original menu label describes an ideal meantone tuning, while playback actually includes recorded deviations. Keep saved parameter indices stable; append any new corrected-original-temperament choice rather than shifting existing indices.
5. **One target-frequency path.** Bring built-in temperaments, step scales, Scala and MTS under the same target-resolution model. Preserve MPE behaviour, intended channel-specific tuning semantics, held-note follow/freeze policy and the behaviour of stops enabled while notes are held. Keep intentional pitch effects after base correction. A request for exact base tuning is not a promise of constant instantaneous pitch when effects are enabled.
6. **Substitutions.** The loader copies neighbouring-pipe metadata and embeds a nominal-key transposition in `Pipe::rate`. Raw file frequency, source identity and effective pitch must be separate so correction is neither omitted nor applied twice. Gedackt keys 53 and 54 are currently rejected by the loader's quality rules and substituted; auditing their files does not establish what playback sounds like at those keys.
7. **Derived registers.** Praestant 16′ currently halves both `rate` and stored frequencies. Use explicit source playback and destination rank ratios, preserving raw measurements of the shared WAV. Do not duplicate or overwrite its source measurement.
8. **Compound stops.** A single F0 estimate cannot calibrate all ranks in Sesquialtera or Mixtur. Identify observed components, expected rank ratios and per-key breakbacks. If the ranks have different tuning errors, resampling the combined recording cannot correct them independently. Separate rank assets or a separately evaluated decomposition are needed for that claim.
9. **Metadata validation and fallback.** Require finite positive frequencies and valid frame regions. Match hashes and actual WAV properties. Reject incompatible schema major versions. Missing, stale, ambiguous or unapproved measurements must be visible; keep old libraries playable with an explicitly labelled nominal fallback rather than silently presenting it as calibrated playback.
10. **Regression coverage.** Current free-tuning tests exercise Praestant and tolerate its recording error. Add synthetic known-pitch assets and tests for sharp/flat pipes, single-rank stops, footage, sample-rate conversion, substitution, derived 16′, MPE, built-in and loaded tunings, held-note updates, repeated library changes and original-mode rate preservation. Validate actual rendered pitches with effects disabled.
11. **Library portability.** `Common.h` still defines the compass, A = 444.5, meantone pitch classes and stop identities. The new organ definition must eventually supply those values. Patch/project state needs stable library identities and tuning-policy versioning so updated calibration cannot silently change recalled results.

## Proposed order

1. Keep the existing WAVs and manifest immutable during the audit.
2. Review the final-WAV analysis, especially measurement disagreements and weak/short samples.
3. Agree on [pitch metadata v1](PITCH_METADATA_V1.md), approve single-rank measurements and add loader validation.
4. Implement one per-voice target/reference calculation, preserving an explicit original mode.
5. Cover derived and substituted sources and all tuning-entry paths with tests.
6. Resolve compound-rank data before claiming complete-organ calibration.
7. Integrate the same definitions into the replaceable-register/organ library format and planned release-velocity assets.

## Precision boundary

A scalar frequency is a calibrated reference over a specified region, not an assertion that an organ sample has a perfectly constant fundamental. Drift, vibrato, beating, inharmonicity, interpolation and measurement uncertainty remain. Define an acceptance tolerance and representative playback interval; do not advertise mathematical exactness of the waveform.
