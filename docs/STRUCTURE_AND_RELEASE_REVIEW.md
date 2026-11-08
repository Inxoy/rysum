# Rysum: structure, release samples and per-note processing

Review date: 2026-11-07. Branch: `plainsound/macos`; source revision inspected: `7ae77f9`.

Scope: source and sample-metadata review, not a completed build, listening evaluation or thread-safety certification. No implementation changes accompany this review.

## Assessment

The engine already has useful separation: immutable loaded samples, a fixed pipe-voice pool, MIDI events rendered at their offsets, per-voice DSP, spatial sends, and separate convolution/binaural components. Preserve these foundations. The most useful architectural addition is a logical note between incoming MIDI and the individual stop/pipe voices. Do not start by replacing the entire engine or introducing a general-purpose plugin graph.

A single note can activate eight stop voices. A pipe voice, a logical note and the shared organ output are therefore three different processing scopes. Treat them explicitly.

## Current release behaviour

- `Pipe` stores one full recording plus attack, release and loop markers (`plugin/source/SampleLibrary.h:19`).
- `releaseVoice` (`OrganEngine.cpp:611`) crossfades from the current reader to about 40 ms before the stored release marker, then plays the existing recording to its end. No separately identified release sample is loaded.
- `keyUp` (`OrganEngine.cpp:814`) plays a mechanical release click selected from a noise pool. Its gain and brightness use `keyVelocity[key]`, the attack velocity. That comment explicitly assumes the key comes up as hard as it went down.
- `handleMidi` and `noteOffFree` receive a complete MIDI message, but do not pass note-off velocity into `keyUp` or `releaseVoice`.
- Sustain defers note release but does not retain release velocity.
- `applyLatch` (`PluginProcessor.cpp:1202`) tracks only MIDI note number. Its generated note-offs use channel 1. That is not sufficient for independent same-pitch notes on different MPE channels.

These are confirmed code paths, not an inference about how a particular controller behaves.

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

## Recommended release design

### 1. Carry release velocity correctly first

Introduce a logical `NoteState` with a stable ID and generation, MIDI channel/key, selected physical key, attack velocity, optional release velocity, held/pedal state, and expression state. Individual pipe voices reference its handle rather than treating channel/key as a lifetime-unique identity.

Propagate release velocity from explicit Note Off through normal tuning, free tuning, MPE, pedal and latch paths. Keep the value from physical note release when sustain defers audible release. Distinguish a Note On with zero velocity from a true Note Off carrying a valid zero release velocity. Use an explicit missing-value policy for computer keys, UI notes, panic, stop withdrawal and automatic preview endings; do not guess hardware capability from a numeric zero.

A release reason is useful: key-up, pedal-up, stop-off, retrigger, steal, panic or preview end. A stop being withdrawn should not suddenly produce the same mechanical key-release event as a finger lifting.

### 2. Give each pipe an explicit release region

Extend `Pipe` with optional release-region metadata: source sample/file, overlap start, valve-release point, end, reference level and extraction provenance. Initially use a view into the existing immutable sample storage; a dedicated region does not require another copy of the waveform in RAM. An optional separate release WAV can later use the same runtime representation.

Keep a compatibility fallback: older manifests derive this region from the existing release marker and use the current behaviour.

At audible release, transition from the sustain reader into the release reader belonging to that same stop/pipe. Preserve current tuning, level and spatial identity. Share voice-local DSP state where continuity is intended. Avoid playing the old full tail and a new release tail simultaneously. Preserve the note owner until all its layers and local processing tails have ended.

Phase and level matching matter: a periodic sustain and a release preroll can cancel or produce a level bump when crossfaded arbitrarily. Compare short correlations/phase candidates at preparation time, audit transition levels, and test across transpositions. Keep runtime work bounded. A fixed 40 ms source preroll and a 40 ms output crossfade are not the same length when the sample is transposed; derive the overlap from playback rate. Handle release during the attack and very short repeated notes explicitly.

### 3. Prepare release assets reproducibly

`scripts/rysum_preprocess.py:246` estimates key release as the last frame within 3 dB of the sustain plateau. This is a starting estimate, not a precisely measured valve event. Review offsets around the release before treating them as edit points.

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

## Processing scopes

| Scope | Existing basis | Recommended ownership |
| --- | --- | --- |
| Pipe voice | Reader, pitch, filters, envelopes, degradation, position, freeze | One stop layer; keep local DSP state here |
| Logical note | Partly represented by `noteId`, free-note tables and key arrays | Independent expression, velocity, release velocity, note envelopes/LFOs and optional note effect state |
| Stop / group | Stop wind, level, shared motion configuration | Intentional registration/group controls |
| Global output | Echo buffer, late room, convolution, Lucier, final decode | Intentional ensemble processing and final presentation |

`Modulation.h` already distinguishes pipe targets (position, level, pitch, brightness, send) from global parameter targets. Per-pipe envelopes exist. LFOs have shared phase with pipe-dependent offsets. Degradation can be per pipe or on the sum. Thus per-note capability is an extension, not a rewrite from zero.

Give routes explicit source and destination scope. For example, note pressure → note brightness should resolve independently; note pressure → global reverb size needs a documented aggregation policy. A global parameter cannot hold a different value for each note. Do not silently let the latest note win unless the user has chosen that behaviour.

A logical-note envelope should be shared coherently by its stop layers, while independent pipe envelopes remain available. Per-note LFO phase/retrigger is a separate option from the present phase offsets by pipe key.

For a first useful implementation, make level, brightness, distortion/degradation and modulation timing independently controllable per note. Where an effect is applied once to a sum of that note's stops, define how that preserves their different spatial positions. Nonlinear processing before and after summing those layers does not sound equivalent.

Keep common room/IR processing shared initially, with per-note send controls. A shared linear convolution with the same fixed kernel can process the summed sends; nonlinear or differently parameterised effects cannot generally be moved across the sum unchanged. Binaural/speaker decoding can remain a final stage. Independent per-note delays or convolutions need explicit resource budgets, not a full global effect copied into every pipe voice.

## Targeted structural recommendations

### Highest priority: identity and lifecycle

1. Extract `NoteRegistry` / `NoteState` from engine key arrays, free-note ownership and processor latch code. Preserve MIDI identity in historical and free-tuning modes. Store attack velocity per note instead of relying on one value per physical key.
2. Add a generation to handles. `(channel, MIDI key)` is an address, not a unique note occurrence: rapid retriggers and overlapping release tails reuse it.
3. Decide release-expression behaviour explicitly. `voicePressure` / `voiceSlide` read current channel/note arrays, and rendering applies channel bend to active tails. A newly allocated MPE note can otherwise influence a preceding note's tail on the same channel. Snapshot or independently retain the retiring note's expression.
4. Make pedal/latch handling use this same registry. In the non-free key path, held state also collapses different input channels onto one physical key.

### Separate the engine by responsibility

`OrganEngine.cpp` is 2,496 lines and `PluginProcessor.cpp` is 2,732 lines. Their size reflects combined responsibilities, rather than merely long DSP loops.

Extract incrementally:

- `PipeVoice` / `SamplePlayer`: sample reading, loops, release transition and voice-local filters.
- `NoteRegistry`: note identity, pedal/latch lifecycle and note expression.
- `ModulationRuntime`: source scope, routing, envelopes and LFO state.
- `SpatialRenderer` / room-send stage: spatial placement and resource allocation.
- `ResourceExchange`: prepared sample/IR/decoder publication and retirement.
- `ProcessorState` / parameter catalogue: persistence, stable IDs and migration, extending the already extracted `ProcessorScenes`.

Keep `PluginProcessor` as the JUCE/host adapter. Keep offline sample analysis out of the audio callback. Do not make a general allocation-heavy polymorphic graph merely to split the source files.

### Audio-thread predictability

- Audio callbacks have fallback buffer `assign`/`setSize` paths. Reserve the required sub-block storage and define a safe oversized-host-block strategy; instrument allocations under stress.
- `allocSlot` clears a 65,536-float early-reflection ring when allocating a source. Repeated large registrations can do substantial burst work at note onset. Consider lazy clearing with generation markers or a bounded prepared-slot strategy, then measure before changing it.
- The 384 pipe-voice slots and 48 early-reflection slots are different budgets. When all reflection slots are occupied, new voices retain the late tail but receive no individual early reflections. Expose this in diagnostics. Eight stop voices per note can consume the reflection budget at six simultaneous notes, even before other sources and draining tails.
- Resource retirement uses shared pointers and a timer-held reference of approximately five seconds. This is not a proof that destruction stays off the audio thread: an audio thread paused beyond that interval can later drop the final old engine reference during adoption. Use explicit acknowledgement/retirement queues or a deferred-deletion scheme.
- Bound new release and note-effect pools and publish high-water/overflow counters. Define behaviour for exhaustion and stealing rather than silently losing note-off or tail events.

### Timing and tuning

The pipe renderer splits at MIDI sample offsets, which is valuable. However, `PluginProcessor::processBlock` parses all tuning SysEx in a buffer before the engine renders its note events. A later tuning message can therefore affect an earlier note-on in the same host buffer. Treat tuning changes as timed control events when testing sequencer handoffs.

Modulation and spatial updates also have a 64-sample control stage. Distinguish sample-accurate note/release timing from this control rate; verify note envelope attack/release timing when notes arrive partway through a slice.

### Build and persistence

- Make `plugin/CMakeLists.txt` the single source of build configuration; the stale root build currently references missing inputs. Forward from the root or clearly retire the obsolete entry point.
- Preserve existing host parameter IDs, route target IDs and factory patches. Append new source/target identities rather than shifting the existing `pipe:N` indices.
- Add versioned release metadata and processing-scope fields with defaults reproducing existing patches.
- The render test has 2,349 lines and already covers much more than a simple smoke test. Split it into release/lifecycle, tuning/expression, spatial/effects and performance suites. Register focused automated cases with CTest; keep listening/render exports available separately.

## Proposed implementation sequence

1. Establish a native Mac build and baseline renders/CPU measurements; add no new audio behaviour yet.
2. Add channel-correct logical-note ownership and carry release velocity through note-off, pedals and latch. Use current release playback initially.
3. Extract `PipeVoice` with compatibility tests for loops, pitch and the existing release.
4. Add release-region metadata and derive a small audition set: representative low/middle/high pipes and several stop types.
5. Implement the dedicated sustain-to-release transition and audition neutral settings against the original.
6. Expose release-velocity response and missing-value policy; retain the existing sound by default.
7. Add explicit note/pipe/global modulation scopes and a small selection of per-note processing controls.
8. Expand to remaining samples and effects only after measuring polyphony, tails, CPU and memory.

## Acceptance tests

- Same attack velocity, varied true Note Off velocity: sustain stays identical, release response changes as configured.
- Note On velocity zero uses the declared fallback; a true Note Off velocity zero remains valid.
- Two same-pitch notes on different channels, and two pitches mapped to one physical pipe: releasing one does not release or change the other.
- MPE channel reuse during an old release: new pressure/slide/bend does not seize the old tail.
- Rapid retrigger, stop withdrawal, preview, sustain, latch, freeze, panic and voice stealing: no duplicate mechanical click, missing release or stuck note.
- Release during attack and at loop boundaries; shifted/substituted pipes and octave-derived stops; several output sample rates and host block sizes.
- New dedicated release and old embedded tail never both play unintentionally.
- One note's nonlinear local effect does not change another note; global effects remain intentionally shared.
- Old presets and sample manifests retain their previous sound.
- Note and effect capacity stress includes tail draining, library replacement while audio is paused, and audio-thread allocation/deallocation checks.

## Recommendation

Implement note identity and release-velocity plumbing first, then explicit release regions and a small per-note DSP path. This provides the requested musical capability while improving the exact boundaries that currently couple notes, pipes and the global output. Broader cleanup should follow these tested boundaries rather than precede them as a large rewrite.
