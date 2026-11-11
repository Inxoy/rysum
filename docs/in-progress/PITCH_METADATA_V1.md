# Pitch metadata v1 — draft contract

Status — draft; loader integration is pending. The machine-readable definition is [pitch-metadata-v1.schema.json](pitch-metadata-v1.schema.json). Measurement evidence is separate from approval. The [final-WAV audit](pitch-analysis/final-wav-pitch-analysis.md) supplies automatic observations.
The current runtime reads adopted `calibrated_hz` values from `rysum_samples.json`;
it does not yet consume this draft sidecar or its approval records.

## Packaging and versioning

Introduce an optional `rysum_pitch.json` sidecar next to `rysum_samples.json`, with `schema: "rysum-pitch-metadata/v1"` and `schema_version: 1`. This permits existing libraries to keep loading before the new loader exists. A future general organ manifest can reference the same definitions.

`library_id` identifies the organ/library; `revision` identifies this calibration release. `base_manifest_sha256` binds it to the exact legacy manifest. Schema v1 is frozen when implemented; incompatible field semantics require v2. Unknown major versions must not be interpreted as v1. A changed WAV or changed calibration requires a new revision and explicit recall policy, not silent replacement.

The sidecar has:

- `assets`: unique source audio files and their raw pitch evidence.
- `pipes`: stop/key mappings, source transpositions and destination pitch relationships.

Audio files remain unchanged. Relative paths are resolved against the sample-library root and must not escape it. IDs are unique within their collection. Every pipe references an existing asset.

## Asset fields

| Field | Meaning |
|---|---|
| `id`, `file`, `sha256` | Stable local ID, relative WAV path and hash of the complete file bytes |
| `sample_rate_hz`, `frames`, `channels` | Properties verified against the actual file |
| `pitch.kind` | `single_rank`, `compound` or `unpitched` |
| `pitch.measurement` | Required reference measurement for `single_rank` |
| `pitch.ranks` | For `compound`, zero or more explicitly identified rank measurements; an empty list means unresolved |

A measurement contains `frequency_hz`, `status` (`candidate`, `review`, `approved` or `rejected`), `method`, `method_version`, nonempty `regions`, and `flags`. Frame ranges are half-open `[start_frame, end_frame)` in the raw file, before playback transposition. Frequency uses Hz at the file's declared sample rate, not a frequency already adjusted for key, footage or derived playback. Preserve full numeric precision; cents fields are derived diagnostics, not the authoritative source.

Optional `metrics` stores pitch-window spread, periodicity and disagreement between estimators. These are diagnostics with specified units, not a statistical confidence interval. Optional `approval` stores reviewer, decision date and notes. `approved` requires this record. A manual correction must identify its method and rationale; do not relabel a raw automatic estimate as independently verified.

For a compound asset, each rank has a stable `id` and its own measurement. Rank identification must distinguish a separate rank from a harmonic. The loader may offer corrected combined playback only when all approved ranks support the same resampling multiplier within a documented tolerance. Otherwise exact corrected playback is unsupported for that combined asset. One approved rank must not silently authorize every rank.

Unpitched assets have no reference frequency and are never pitch-calibrated.

## Pipe fields and units

| Field | Meaning |
|---|---|
| `stop_id`, `key_midi` | Existing stop identity and keyboard address; not the recording's dominant spectral peak |
| `asset_id` | Actual source file, including when substituting another key |
| `source_playback_ratio` | Original-mode pitch/speed multiplier excluding sample-rate conversion; 1 for an own recording, 0.5 for a derived 16′ using the same 8′ WAV |
| `target_reference_ratio` | Rational rank frequency relative to the requested keyboard frequency: 1/1 for 8′, 2/1 for 4′, 4/1 for 2′, 1/2 for 16′ |
| `compound_targets` | Optional per-rank target ratios for a compound asset; supports breakbacks by being defined per pipe/key |
| `substituted_from` | Optional source stop/key with reason; raw asset measurements remain those of that source |

`target_reference_ratio` is null for unresolved compound or unpitched pipes. A compound pipe uses `compound_targets` when ranks have been identified. Positive rational integers avoid confusion between frequency ratios and cents. A separate future organ definition supplies the keyboard compass and tuning reference; do not infer these from a file's name or assume A = 444.5 for every library.

## Playback equations

Let `Fs` be file sample rate, `Fo` output sample rate, `R0` original source playback ratio, `Fm` the raw file's approved measured reference, `Ft` requested keyboard frequency, and `Q` the register's target reference ratio.

```text
Original mode:
    source frames per output frame = (Fs / Fo) × R0

Corrected single-rank mode:
    effective original reference = Fm × R0
    additional correction        = (Ft × Q) / (Fm × R0)
    source frames per output frame = (Fs / Fo) × R0 × additional correction
                                  = (Fs / Fo) × (Ft × Q) / Fm
```

`R0` cancels only in corrected mode because the destination frequency is absolute. Do not apply both a pre-corrected frequency and the same source ratio again. Example: derived 16′ references an unchanged Praestant 8′ asset, with `R0 = 0.5` and `Q = 1/2`; the raw asset measurement stays at its 8′ recorded frequency.

For a substituted pipe, use the replacement WAV's measured reference and the destination register's ratio. For compounds, compute `(Ft × Q_rank) / Fm_rank` for every identified rank; a combined WAV has only one playback speed, so incompatible multipliers cannot all be satisfied.

Pitch bend, explicit tuning offsets, modulation, wind and Doppler act afterwards. The setting for held notes following tuning changes determines whether their target is recomputed; it does not alter the source measurement.

## Validation beyond JSON Schema

The loader must also check:

1. IDs and `(stop_id, key_midi)` pairs are unique, and all references resolve.
2. Hashes, rates, channel counts, lengths and frame ranges match the WAV. `end_frame` must exceed `start_frame` and be at most `frames`.
3. Frequencies and ratios are finite and positive; ranks are identified, not just spectral peaks. Plausibility checks detect octave/partial mistakes against declared register/key relationships.
4. Only `approved` measurements are eligible for production correction. Candidate/review/rejected or missing values trigger a visible fallback or refusal according to policy.
5. A substituted pipe's source provenance and transposition are consistent. Derived samples are not double-transposed.
6. Sample-rate correction is included exactly once. Region indices always refer to the source file.
7. An old host project can retain the prior library revision/policy or ask explicitly to adopt new calibration. Keep existing parameter IDs and temperament indices stable.

## Example (illustrative, not approved calibration)

See [pitch-metadata-v1.example.json](pitch-metadata-v1.example.json). It contains a candidate Trompete measurement from the audit and own/derived Praestant mappings. None of these entries is approved. The example is an illustrative partial snapshot from the initial review; its manifest hash
refers to that snapshot. Runtime library revisions need their own validated metadata.

## Acceptance tests for implementation

- Original playback rates remain unchanged, including derived and substituted pipes.
- Known synthetic single-rank signals render at their requested target within a specified tolerance across 44.1/48/96 kHz.
- 4′, 2′ and 16′ destinations retain their intended register ratios.
- MTS, Scala, built-in and step tunings share the calibrated path; MPE and held-note policies remain correct.
- Compound assets with incompatible rank multipliers cannot be declared fully calibrated.
- Hash mismatches, invalid regions and unavailable approvals produce clear diagnostics.
- Released samples use their own raw measured reference when release-velocity support introduces separate assets; they must not blindly inherit a sustain asset's calibration.

## Reproducing validation

Use a Python environment with `scripts/pitch-analysis-requirements.txt` installed. Run from the repository root:

```sh
PYTHONDONTWRITEBYTECODE=1 python -m unittest discover -s scripts -p 'test_*pitch*.py'
```

The tests validate the JSON Schema and example, reject invalid approval/frequency/version data, verify coverage and file hashes for every audit entry, and check estimators against synthetic sharp/flat harmonic signals at several sample rates. These checks do not approve measurements or implement playback correction.
