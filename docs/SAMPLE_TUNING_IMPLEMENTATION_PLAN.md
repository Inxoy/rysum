# Sample tuning and portable registers: implementation plan

Research and code review: 8 November 2026. This document proposes staged implementation; no playback behaviour has been changed. The submitted macOS PR remains the baseline.

## Goals

1. Keep original recorded tuning available, while correcting validated single-rank samples to explicitly requested frequencies in other tuning modes.
2. Offer a player preference to retain the current tuning across patch changes.
3. Import one Norrfjärden register through a portable definition without regenerating or replacing the existing Rysum library.
4. Establish a structure usable by replacement registers and, later, a complete organ definition.

## Sample sources

Use Lars Palo's full GrandOrgue release as the reference source. His [instrument page](https://familjenpalo.se/vpo/about-my-samplesets/norrfjarden-church/) specifies 44.1 kHz, 24-bit stereo audio, multiple loops/releases and CC BY-SA 4.0. The [download listing](https://familjenpalo.se/vpo/download/) identifies version 20230618, approximately 2 GB, with published MD5 `0936959d9901c3726ce106e3eb119c61`. The direct package is [NorrfjardenChurch.orgue](https://familjenpalo.se/sites/default/files/sampleset/packages/NorrfjardenChurch.orgue).

The author describes corrected embedded sample-pitch information, temperament and some release cues in his [June 2023 update](https://github.com/GrandOrgue/grandorgue/discussions/1379). Avoid an unidentified older copy bearing the same filename. Published checksum verification identifies the expected package; also calculate a local SHA-256 for our import provenance.

[Augustine's Hauptwerk adaptation](https://hauptwerk-augustine.info/Norrfjarden.php) is a second distribution of Lars's recordings, not an independent recording session. It provides a [download page](https://kdrive.infomaniak.com/app/share/669203/9e6df9b9-8774-4bca-91f4-4c48cca2f6c6/files/28929). Its publisher describes multiple releases and loops, and simplified D-sharp/E-flat mapping in the Hauptwerk definition. This can be useful for comparison, but the original GrandOrgue definition is preferable for preserving source mapping.

A [2012 message by Lars Palo](https://sourceforge.net/p/ourorgan/mailman/message/29225802/) describes an unfinished multi-attack Norrfjärden demo at `http://www.palohb.com/musik/test/norrfjarden.7z`. This is historical context, not the recommended full set; current availability is not verified.

The full archives have not been downloaded or inspected in this review. The direct GrandOrgue link was located, but its roughly 2 GB contents exceed the web reader's limit. Do not assume the files' actual codecs, embedded metadata, ODF adjustments or release-selection rules until inspected. Multiple releases are not automatically release-velocity layers.

## Current implementation gaps

- `OrganEngine::slotHz` assumes the Rysum keyboard's meantone pitches and A = 444.5 Hz. `centsFor` uses that shared reference for every register; measured pipe deviations survive retuning.
- `Pipe::rate` combines file-rate conversion and source/derived transposition. Measurements copied for substitutions and halved for 16′ require a clearer raw-source model.
- `SampleLibrary` contains a fixed eight-by-41 pipe array. Unknown register IDs and keys outside the historical compass are ignored. Labels, stop ratios and default gains live in code.
- `readMono` can decode audio into float, but averages channels. Original 24-bit stereo material must not be fed through the existing PCM16-export preprocessing path if we intend to retain its source resolution.
- Some sample-region and release calculations use `kSampleFileRate = 48000`; all source-file timing must instead use the actual file rate or explicit source-frame positions.
- `applyState(state, forPatch)` fills missing patch parameters with defaults and calls `apvts.replaceState`. It then loads any embedded Scala definition. Tuning is not among the existing player settings preserved across patch loads.
- Existing pitch analysis is evidence, not approved calibration. Compound recordings cannot necessarily be tuned exactly by one resampling factor.

Related detail: [pitch accuracy review](PITCH_ACCURACY_REVIEW.md), [metadata v1 draft](PITCH_METADATA_V1.md), [final WAV analysis](pitch-analysis/final-wav-pitch-analysis.md).

## Design boundaries

### Immutable source data

An audio asset owns its decoded data, native file rate, frame regions, raw measured reference and provenance. The measured reference is always that of the raw asset, not a value already adjusted for register footage, substitution or derived playback. Validate hashes, finite frequencies and bounds when loading, on a background thread. Parsing, calibration and hashing do not belong in the render callback.

A pipe mapping points to an asset and carries original playback transposition, destination rank ratio, source key identity and optional release mappings. This supports native playback, substituted pipes, derived 16′ and separate release assets without copying an adjusted frequency back into the raw measurement.

### One pitch calculation

Introduce a small pure helper before changing the engine. Its inputs are file/output sample rates, original playback ratio, raw reference frequency, desired keyboard frequency, target rank ratio and tuning policy.

- Native: `fileRate / outputRate * originalPlaybackRatio`.
- Corrected: `fileRate / outputRate * desiredKeyboardHz * rankRatio / rawMeasuredHz`.

Handle invalid/missing calibration explicitly; never divide by zero or silently declare a nominal fallback calibrated. Intentional bend, vibrato, wind and Doppler apply after base tuning. Verify that the engine does not retain and reapply its old nominal-key offset as well.

### Tuning choices and patch policy

Keep original recorded tuning as an explicit choice. Existing other temperaments, step scales, Scala and MTS target a common absolute-frequency representation and receive per-source correction. Preserve host parameter IDs and existing temperament indices. If an ideal corrected version of the original meantone is added, append it as a separate choice.

Add `Keep tuning when changing patches`, initially off to preserve existing patch behaviour. With it on, preserve tuning source, relevant base pitch/reference settings, correction policy, loaded SCL/KBM texts and names, and the held-note tuning-follow setting. Random detuning and sound modulation remain patch effects; MPE already has its own preserved player policy.

Implement preservation by modifying a copied incoming state before replacing it and skipping embedded Scala replacement. Do not apply a patch tuning briefly and restore the old one afterwards: the audio callback could observe the transient. Preserve the lock preference itself across sound patches. Full host project/session restoration must still restore its saved tuning and preference. Cover factory and user patch routes.

### Portable register definitions

Keep eight stable host-facing slots for the first implementation. A slot's sound comes from a `RegisterDefinition`, with an independent ID/name, default gain, source pipe mappings, rank ratios and asset references. Changing a displayed name must not change automation IDs such as `stop0`.

Provide a legacy adapter for `rysum_samples.json`. Imported registers use the same internal model, resolved from relative paths within an external library root. Store source author, licence, package/version and hashes in the import record. Library and register identities belong in saved assignments, with explicit missing-library reporting and a policy for whether patch loads change assignments.

Do not implement a complete GrandOrgue interpreter in the plugin. Use an offline importer for the selected register. Inspect ODF fields, actual audio signatures, embedded pitch/loop/cue information and definition-level tuning corrections. Preserve extracted source metadata separately from the effective playback interpretation, avoiding double pitch corrections.

Start with the overlap of the existing 41-key compass. Preserve extra source note identities, including subsemitones, for later mapping; do not collapse D-sharp/E-flat assets into one ID just because they share a standard MIDI address. Dynamic compass and arbitrary slot counts come later.

### Bound sample range and transposition

Each register should declare its supported sounding range and a conservative maximum transposition from an available source pipe, with per-pipe overrides where needed. Large extensions into higher or lower registers change attack, release, noise and timbre unnaturally; repeatedly reusing an edge sample also cannot represent the locations of additional physical pipes. Select sources by calibrated frequency within those bounds, retain the actual source pipe's spatial identity, and keep any deliberate virtual relocation separate. Outside the supported range, leave that register silent and report the unavailable range rather than silently stretching its end samples. Exact tuning correction does not authorize unlimited compass extension. The current free-tuning path permits shifts of up to two octaves; replace that blanket allowance with register-specific limits chosen by audition and recorded in metadata. Do not impose an arbitrary universal limit before testing the registers, and distinguish supported derived registers, such as the deliberate Praestant 16′, from automatic out-of-range substitution.

### Audio and releases

Keep source rate and 24-bit resolution through decoding; JUCE float storage can represent PCM24 input without first quantizing it to PCM16. Inspect whether an offline lossless decode is needed, preserving useful RIFF metadata in the import sidecar.

Make the initial stereo-to-mono policy explicit because the current spatial pipe engine uses mono emitters. Keep original stereo assets intact; optional stereo playback is a separate engine change. Test native 44.1 kHz source regions and release timing at both 44.1 and 48 kHz output.

The register format should describe an embedded tail and optional separate release assets. Import release selection as actually specified: duration-dependent releases must not be mislabelled as velocity layers. A first test may use one deliberately selected release; retain the other definitions for subsequent release-velocity work. Compare with additional room effects disabled so recorded ambience is not confused with plugin reverb.

## Guided implementation order

| Step | Change | Completion check |
|---|---|---|
| 1 | New feature branch from the submitted PR baseline; agree on policy and minimal types | Existing builds remain available; no changes pushed to the PR branch |
| 2 | Pure pitch-rate helper and synthetic tests | Known sharp/flat sources, 4′/2′/16′ ratios, 44.1/48/96 kHz, invalid inputs and substitution cases |
| 3 | Immutable calibration/reference data and validated loader adapter | Old Rysum library still loads; source and effective pitch are unambiguous |
| 4 | Apply corrected rates per voice and retain native mode | Trompete and other single-rank stops meet a documented reference tolerance; native playback remains unchanged |
| 5 | Patch tuning-lock preference and state tests | User/factory patches preserve tuning when locked, restore it when unlocked; project round-trip works |
| 6 | Obtain and inspect the 20230618 package; choose one single-rank register | Checksums, codec, mappings, pitch metadata, loops, releases and source licence recorded |
| 7 | Register definition and offline subset importer | Load one Norrfjärden register into an existing slot without copying/modifying Rysum WAVs |
| 8 | Instrument tests and saved assignments | Native pitch, corrected 12-EDO/Scala/MTS, held notes, register replacement and missing-library recovery work |

Recommend Hauptwerck Principal 8′ as the first import: it tests another organ's pitch reference while allowing a straightforward comparison with Rysum Praestant. Hauptwerck Trommeten 8′ is an alternative if the immediate priority is comparing reed response. Start with one single rank, not Mixtur or a compound stop.

Synthetic tests can use a strict tolerance against known generated signals. Real samples need an agreed region/tolerance and independent measurement: drift, beating and inharmonicity prevent a promise of perfectly constant instantaneous pitch. Keep unresolved compound ranks visibly uncalibrated.

Keep downloaded packages and extracted recordings outside Git, for example `~/Dev/sample-libraries/norrfjarden/`. Commit importer code, metadata definitions, provenance instructions and small synthetic fixtures rather than a 2 GB package.

## First user step

From a clean repository on the submitted branch:

```sh
git switch -c plainsound/sample-tuning
```

This planning document can follow onto the new branch as an uncommitted file. Then write the pure helper and its tests before wiring any playback behaviour. The first lesson should cover the input structure and the difference between source rate, native transposition and the requested absolute pitch.
