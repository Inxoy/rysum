# Sample tuning progress and remaining work

Status — in progress. Updated 11 November 2026 against `plainsound/sample-tuning`.
The original implementation plan has produced the current native/corrected pitch
path, tuning retention and DSF reference instrument. Portable imports have a
separate [sample-library implementation plan](../to-do/SAMPLE_LIBRARY_IMPLEMENTATION.md).

## Implemented

| Area | Current state |
| --- | --- |
| Playback-rate helper | `SamplePitch.h` validates native/corrected rate requests; synthetic tests cover pitch, footage and file/output rates |
| Source representation | `Pipe` separates `nativePitchRatio`, raw reference and calibration ratio |
| Single-register correction | Five recorded single ranks use manifest calibration in retuned modes; derived Praestant 16′ follows its 8′ source |
| Originalstimmung | Recorded pitch and historical compass, with Stimmton and random Verstimmung disabled |
| Tuning selection | Built-in temperaments, step scales, Scala and MTS resolve targets with the current source mapping |
| Held notes | Compensation preserves the target when a tuning change selects another source pipe |
| Preset policy | `loadPresetTuning` defaults to true; switching it off retains temperament, Stimmton, held-note follow setting and loaded Scala/KBM data across sound presets |
| Compass | Retuned modes fill missing chromatic notes; optional extended range reaches MIDI 24–108 |
| Testton | Measured-register or nominal quarter-comma reference in Originalstimmung, with historical note limits |
| Evidence | Final-WAV audit, hashes, regional estimates, listening checklist and initial targeted sample edits |

Host projects save the preset-tuning preference and extended-range preference.
Full project restoration loads their saved state. Random detuning remains part of
the sound preset and has an effect in retuned modes.

## Next calibration steps

1. Continue manual checks in `pitch-analysis/pitch-checklist.csv` and preserve
   corrections and notes.
2. Identify Sesquialtera and Mixtur ranks and breakbacks before choosing a
   calibration policy for their combined recordings.
3. Audit attacks, loops and releases where listening exposes problems. Update
   frame positions and evidence when an edited asset is adopted, retaining provenance.
4. Agree on the validation, review and recall semantics of
   [pitch metadata v1](PITCH_METADATA_V1.md), then implement its loader path.
5. Test the actual played source for substitutions and derived registers and
   expand coverage where those checks expose a gap.

## Next library steps

The [sample-library plan](../to-do/SAMPLE_LIBRARY_IMPLEMENTATION.md) covers
immutable asset/register definitions, a legacy adapter, source-frame timing,
Norrfjärden inspection, one-register replacement and whole-organ support.
The [stereo plan](../to-do/STEREO_SIGNAL_PATH.md) and
[release plan](../to-do/RELEASE_VELOCITY.md) share those definitions.

Keep source sample rate, raw reference pitch, native transposition and destination
rank separate. The current runtime is still organised around fixed Rysum slots,
its historic keyboard and 48 kHz region assumptions. A portable importer needs
those assumptions made explicit before adopting another organ's metadata.

## Checks

Run the CTest targets and the offline audio suite described in the
[macOS guide](../MAC_BUILD.md#run-the-checks). Run estimator and metadata checks
with the Python dependencies from `scripts/pitch-analysis-requirements.txt`.

```sh
PYTHONDONTWRITEBYTECODE=1 python -m unittest discover -s scripts -p 'test_*pitch*.py'
```

Real-sample checks should record the tuning mode, source key/register, reference
frequency, enabled pitch effects and region being compared. The audit flags and
runtime calibration decisions serve different purposes and should both remain
visible during this work.
