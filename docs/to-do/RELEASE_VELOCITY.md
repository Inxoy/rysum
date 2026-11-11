# Release velocity and dedicated release playback

Status — planned. Reviewed against the `plainsound/sample-tuning` branch on
11 November 2026. The macOS build and DSF Testton are available for implementing
and auditioning this work.

The goal is to carry real MIDI release velocity to each note and use it for
mechanical key-up noise and the shutdown of its pipe layers. Existing recordings
supply an initial release region for each pipe. The
[structure review](../STRUCTURE_REVIEW.md) describes the current engine and the
[sample-library plan](SAMPLE_LIBRARY_IMPLEMENTATION.md) describes reusable asset
and register definitions.

## Current release behaviour

- `Pipe` stores one full recording plus attack, release and loop markers (`plugin/source/SampleLibrary.h`).
- `releaseVoice` crossfades from the current reader to about 40 ms before the stored release marker, then plays the existing recording to its end. No separately identified release sample is loaded.
- `keyUp` plays a mechanical release click selected from a noise pool. Its gain and brightness use `keyVelocity[key]`, the attack velocity. That comment explicitly assumes the key comes up as hard as it went down.
- `handleMidi` and `noteOffFree` receive a complete MIDI message, but do not pass note-off velocity into `keyUp` or `releaseVoice`.
- Sustain defers note release but does not retain release velocity.
- `applyLatch` tracks only MIDI note number. Its generated note-offs use channel 1. That is not sufficient for independent same-pitch notes on different MPE channels.

These paths were checked again against the current source on 11 November 2026.

### Available audio

`rysum-samples/rysum_samples.json` describes 287 recorded pipe entries: seven recorded stops with 41 keys each. The eighth runtime stop is derived from recorded material. All but one recorded entry have loop markers. The manifest says 48 kHz.

Remaining recording length after the release marker:

| Recorded stop | Median | Range |
| --- | ---: | ---: |
| Praestant | 1.055 s | 0.580–1.855 s |
| Gedackt | 0.530 s | 0.260–1.515 s |
| Oktave 4 | 0.640 s | 0.435–1.035 s |
| Oktave 2 | 0.940 s | 0.420–1.725 s |
| Sesquialtera | 0.775 s | 0.475–1.275 s |
| Mixtur | 0.885 s | 0.400–1.460 s |
| Trompete | 0.700 s | 0.500–0.880 s |

These are metadata durations, not measured durations of useful audible release. The files contain enough post-release material to prototype dedicated release regions, subject to listening and marker checks. Raw recordings are present too.

## Implementation stages

### 1. Carry release velocity through the note lifecycle

Introduce a logical `NoteState` with a stable ID and generation, MIDI channel/key, selected physical key, attack velocity, optional release velocity, held/pedal state, and expression state. Individual pipe voices reference its handle rather than treating channel/key as a lifetime-unique identity.

Propagate release velocity from explicit Note Off through normal tuning, free tuning, MPE, pedal and latch paths. Keep the value from physical note release when sustain defers audible release. Distinguish a Note On with zero velocity from a true Note Off carrying a valid zero release velocity. Use an explicit missing-value policy for computer keys, UI notes, panic, stop withdrawal and automatic preview endings; do not guess hardware capability from a numeric zero.

A release reason is useful: key-up, pedal-up, stop-off, retrigger, steal, panic or preview end. A stop being withdrawn should not suddenly produce the same mechanical key-release event as a finger lifting.

### 2. Give each pipe an explicit release region

Extend `Pipe` with optional release-region metadata: source sample/file, overlap start, valve-release point, end, reference level and extraction provenance. Initially use a view into the existing immutable sample storage; a dedicated region does not require another copy of the waveform in RAM. An optional separate release WAV can later use the same runtime representation.

Keep a compatibility fallback: older manifests derive this region from the existing release marker and use the current behaviour.

At audible release, transition from the sustain reader into the release reader belonging to that same stop/pipe. Preserve current tuning, level and spatial identity. Share voice-local DSP state where continuity is intended. Avoid playing the old full tail and a new release tail simultaneously. Preserve the note owner until all its layers and local processing tails have ended.

Phase and level matching matter: a periodic sustain and a release preroll can cancel or produce a level bump when crossfaded arbitrarily. Compare short correlations/phase candidates at preparation time, audit transition levels, and test across transpositions. Keep runtime work bounded. A fixed 40 ms source preroll and a 40 ms output crossfade are not the same length when the sample is transposed; derive the overlap from playback rate. Handle release during the attack and very short repeated notes explicitly.

### 3. Prepare release assets reproducibly

`scripts/rysum_preprocess.py::extract_note` estimates key release as the last frame within 3 dB of the sustain plateau. This is a starting estimate, not a precisely measured valve event. Review offsets around the release before treating them as edit points.

Add a separate, non-destructive release preparation pass that:

1. Reads the existing manifest and audio without overwriting them.
2. Examines the release transition, includes a short sustain preroll and preserves the useful tail.
3. Applies only the fades/cleanup necessary for a clean transition; retains level relative to the pipe body.
4. Writes optional release WAVs and versioned metadata, plus plots/audition examples and confidence flags.
5. Records source file, sample rate, exact source-frame offsets, analysis version and settings.

Current `source_time` is rounded and is not a complete source-frame extraction record. Re-detect/refine on the raw recording or retain the original crop offsets when regenerating; do not assume a rounded time reconstructs a crop exactly.

Both preprocessing (`soft_tail`) and the sample loader currently fade sample endings. Make the responsibility explicit so a prepared release is not unintentionally faded twice. Preserve old-manifest behaviour through a documented compatibility path.

### 4. Map release velocity musically

Keep two independently adjustable responses:

- Mechanical key-up noise: level and brightness.
- Pipe shutdown: transition/envelope behaviour, with optional tonal change.

Start with a neutral reference release and continuous controls derived from one recorded release per pipe. Do not call these authentic multi-velocity recordings: the available capture does not establish several different physical releases for every pipe. Additional takes may supply alternatives only after their releases are assessed independently of sustain quality.

Do not time-compress the whole recorded tail to simulate a quicker key release: it also changes pitch and room decay. Faster valve closure and the room continuing to ring are different phenomena. Audition a modest release transition/level/brightness mapping before adding more elaborate resynthesis.

Mechanical clicks should normally occur once per logical key release, while pipe release regions occur once per sounding stop voice. Otherwise an eight-stop registration multiplies the mechanical release noise.

## Acceptance tests

- Same attack velocity, varied true Note Off velocity: sustain stays identical, release response changes as configured.
- Note On velocity zero uses the declared fallback; a true Note Off velocity zero remains valid.
- Two same-pitch notes on different channels, and two pitches mapped to one physical pipe: releasing one does not release or change the other.
- MPE channel reuse during an old release: new pressure/slide/bend does not seize the old tail.
- Rapid retrigger, stop withdrawal, preview, sustain, latch, freeze, panic and voice stealing: no duplicate mechanical click, missing release or stuck note.
- Release during attack and at loop boundaries; shifted/substituted pipes and octave-derived stops; several output sample rates and host block sizes.
- New dedicated release and old embedded tail never both play unintentionally.
- Old presets and sample manifests retain their previous sound.
- Note and effect capacity stress includes tail draining, library replacement while audio is paused, and audio-thread allocation/deallocation checks.

## First implementation milestone

Carry optional release velocity through both note-off paths, sustain and latch,
with note identity preserved across channels and retriggers. Verify the received
values with synthetic MIDI before changing release sound. Then audition dedicated
release regions for representative low, middle and high notes of Praestant,
Gedackt and Trompete.

Expose the response only after the neutral transition reproduces the existing
release without doubled tails or repeated mechanical clicks. Additional
release recordings can later share the same asset and region model.
