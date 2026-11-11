# Recorded pitch and corrected tuning

Status — in progress. Updated 11 November 2026 against `plainsound/sample-tuning`.
Single-rank correction and Originalstimmung are implemented. Compound-register
measurements and the portable metadata/approval workflow are ongoing.

## Current playback

`rysum-samples/rysum_samples.json` references 287 active recordings across seven
recorded registers. Praestant 16′ derives from Praestant 8′ at half speed.
Historical WAVs can remain alongside the referenced assets.

The manifest now contains `calibrated_hz` for Praestant, Gedackt, Octave 4′,
Octave 2′ and Trompete. Praestant was checked by ear; the other single-register
estimates were accepted for testing. Continued listening checks can refine them.

`SampleLibrary::load` separates native source transposition, sample rate and
calibration. `OrganEngine` applies the calibration ratio per voice when corrected
tuning is active. `slotHz` and the selected source mapping still provide the
nominal reference for the target offset. The pure helper in `SamplePitch.h` tests
native/corrected rate calculations; current engine rendering composes calibration
with the nominal target offset.

Originalstimmung preserves recorded playback and disables Stimmton and
Verstimmung. Other built-in tunings, step scales, valid Scala and active MTS
apply source calibration where available. Held-note compensation handles a
change in preferred source pipe. Optional extended range and chromatic gap
filling apply to retuned playback.

Sesquialtera and Mixtur have no accepted per-rank calibration. Other tunings
transpose each combined recording as a whole. Differences between its ranks
therefore remain part of the sound.

## Measurement and listening tools

The [final-WAV audit](pitch-analysis/final-wav-pitch-analysis.md) stores hashes,
source-frame windows, spectral/autocorrelation observations and automatic review
flags. Its `candidate` and `review` states describe estimator results. The runtime
currently consumes `calibrated_hz` directly from the legacy manifest, independently
of those audit states.

The [verification checklist](pitch-analysis/pitch-checklist.csv) is the working
record for manual comparisons. [DSF Testton](../DSF_TEST_TONE.md) provides measured
single-register references and nominal quarter-comma meantone at A4 = 444.5 Hz.
In Originalstimmung it follows the historical organ compass and missing notes.

Low C and C-sharp Trompete use manually trimmed WAVs. Their loop/release positions
and audit records were adjusted; the original WAVs remain available. Sesquialtera
low D uses a revised shorter loop to reduce the phase and level change at its join.
Further listening checks remain part of this work.

## Remaining work

1. **Compound ranks.** Identify the actual components and breakbacks in
   Sesquialtera and Mixtur. Separate a rank fundamental from its harmonics and
   compare each component against the intended keyboard/rank relationship.
2. **Calibration decisions.** Preserve manual corrections, reviewer notes and
   adopted revisions separately from automatic estimates. Define an acceptance
   region and tolerance for recordings with drift or beating.
3. **Validated metadata.** Implement the [v1 draft](PITCH_METADATA_V1.md) or an agreed
   successor in the loader. Validate hashes, frame regions, source identity,
   approvals and fallback behaviour when loading. The current runtime checks
   calibration values for finiteness and positivity but does not consume this sidecar.
4. **Substituted and derived pipes.** Continue checking the actual source played
   at each key, including Gedackt substitutions and derived 16′. Preserve the raw
   measurement and original source identity when a mapping changes.
5. **Tuning boundaries.** Continue host/controller checks across internal scales,
   Scala, MTS and MPE, including held notes and preset changes. Tuning SysEx timing
   inside a host block remains a separate processor-level improvement.
6. **Portable instruments.** Move Rysum's fixed reference, compass, rank meanings
   and source limits into reusable definitions. Follow the
   [sample-library plan](../to-do/SAMPLE_LIBRARY_IMPLEMENTATION.md).
7. **Release assets.** Apply suitable raw-source reference data to dedicated
   releases as they are added. Follow the [release plan](../to-do/RELEASE_VELOCITY.md).

## Precision boundary

A measured frequency describes a selected region of the recording. Natural
pitch drift, beating, inharmonicity and resampling affect the instantaneous output.
Synthetic tests verify rate calculations and rendered target pitch; listening
checks establish useful tolerances for the real pipes.

The three CTest targets and the offline audio suite currently pass. They cover
known-pitch assets, footage, rate conversion, held-source changes, MTS switching,
missing chromatic notes and the optional extended range. Complete-organ
calibration remains in progress until the compound registers are resolved.
