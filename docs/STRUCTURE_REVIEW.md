# Rysum structure review

Reviewed 11 November 2026 against `plainsound/sample-tuning`, including upstream's
room-motion and impact-sound changes. Standalone and VST3 builds, the three CTest
targets and all 353 offline audio assertions pass at this revision.

## Current organisation

| Component | Current responsibility |
| --- | --- |
| `plugin/CMakeLists.txt` | Maintained build, plugin formats, dependencies and test targets |
| `SampleLibrary` | Background loading, decoded sample storage, pipe metadata and substitutions |
| `SamplePitch` | Pure native/corrected playback-rate calculation and input validation |
| `OrganEngine` | Pipe voices, MIDI note handling, sample playback, modulation and room/effect rendering |
| `OrganLive.cpp` | Bellows, walking listener, sympathetic pipes, adaptive tuning and Tonnetz walker |
| `OrganBodies.cpp` | Moving pipes in rooms and their impact sounds |
| `PluginProcessor` | Host integration, parameter catalogue, state, tuning tables, resource publication and final output |
| `ProcessorScenes` | Scene storage and morphing |
| `DsfReferenceInstrument`, `DsfReferenceTone`, `DsfRenderer` | MIDI/manual Testton, smoothing, DSF rendering and oversampling |
| `ConsoleView`, `KlangView`, `SpaceView`, `SaalView`, `WalkView` | Console, sound/tuning controls, pipe layout, rooms and 3D view |

The root CMake configuration forwards to `plugin/`. Source, resources and tests
use the lowercase directories `plugin/source/`, `plugin/resources/` and
`plugin/tests/`. Samples remain external in `rysum-samples/`.

## Implemented since the initial review

- Native arm64 macOS Standalone and VST3 builds, microphone usage declaration,
  quoted local-JUCE paths and JUCE 8.0.12 integration.
- Separate native source transposition and file/output sample-rate conversion.
- Manifest `calibrated_hz` values for the five recorded single-rank stops,
  per-voice correction in retuned modes and derived Praestant 16′ playback.
- Explicit Originalstimmung, optional tuning retention across preset loads,
  additional temperaments and an optional extended compass in retuned modes.
- Held-note compensation when a tuning change selects a different source pipe.
- DSF Testton on Klang and Spieltisch, with measured-register and nominal
  quarter-comma references for Originalstimmung.
- Separate live-organ and room-body implementation files, additional spatial
  processing, and automated checks for the new motion modes and impacts.

The sample engine still uses fixed register slots and the historical Rysum
keyboard definition. Detailed calibration and compound-rank work continue in the
[pitch review](in-progress/PITCH_ACCURACY_REVIEW.md).

## Processing scopes

| Scope | Current basis | Further work |
| --- | --- | --- |
| Pipe voice | Reader, playback rate, loop/release state, filters, envelope, degradation, position and freeze | Extract a sample player when release and library work establish its shared interface |
| Logical note | Free-note IDs/tables, key counts, physical-key arrays and processor latch state | Unify channel/key identity, retrigger generations, release velocity and retiring-note expression |
| Register | Stop level, wind, motion, pipe sound and layout settings | Make the assigned sample definition and physical pipe dimensions data-driven |
| Shared organ output | Room sends, early reflections, late reverb, convolution and organ effects | Retain explicit shared controls and define aggregation for note-driven global parameters |
| Reference output | Independent monophonic Testton after organ effects | Continue to use it for listening checks of tuning and samples |

`Modulation.h` distinguishes per-pipe targets from shared parameter targets.
Per-pipe envelopes and key-dependent LFO offsets already exist. A logical-note
layer would let a note's stop voices share note expression and lifecycle while
retaining their own pipe processing.

A route from note pressure to pipe brightness can operate independently for each
note. A route to shared room size needs an explicit aggregation rule. Shared
linear room convolution can process summed sends; note-specific nonlinear
processing needs its own ownership and resource budget.

## Remaining structural work

### Note identity and releases

Historical key mode collapses channels onto physical keys. Free tuning identifies
notes by channel and MIDI key, while latch still stores one flag per MIDI key and
generates channel-1 note-offs. Release clicks use attack velocity, and note-off
velocity is not carried into `keyUp` or `releaseVoice`.

Introduce note occurrences with stable handles and generations. Retain release
velocity through pedal deferral and define how pressure, slide and bend affect
old tails after MPE channel reuse. The staged work and dedicated-release design
are in [the release implementation plan](to-do/RELEASE_VELOCITY.md).

### Sample and instrument definitions

The loader's fixed eight-by-41 pipe storage, named register IDs, Rysum tuning
reference, physical dimensions and sample-frame assumptions still couple the
engine to this organ. A portable asset/register adapter is the next boundary for
replacement samples. The [sample-library plan](to-do/SAMPLE_LIBRARY_IMPLEMENTATION.md)
sets out a first external-register import and subsequent whole-organ support.

### Resource lifetime and audio-thread work

Prepared libraries, impulse responses and decoders are published for adoption by
the audio thread. Message-thread retirement currently uses a countdown of about
five seconds. Replacing it with acknowledged retirement would make resource
ownership explicit when audio is suspended for longer than that interval.

The current budgets are 384 pipe voices, 48 noise voices and 48 early-reflection
slots. A registration can consume eight pipe/reflection slots per note. Source
allocation clears a large reflection-history ring, so onset bursts and draining
tails deserve separate measurements from steady-state rendering.

Fallback buffer growth remains possible in audio processing. Instrument
allocations and destruction under oversized host blocks, retriggers, library
replacement and suspended audio before expanding release or per-note effect
pools. Establish bounded overflow and voice-stealing behaviour for those additions.

### Timing and persistence

The pipe renderer respects MIDI offsets inside host blocks. Tuning SysEx is
currently parsed at processor level before the engine renders the block, so a
later tuning message can affect an earlier note event in that same block.
Timed tuning changes are a further improvement for sequencer handoffs.

Preserve host parameter IDs and the existing order when appending controls.
The latest merge places impact parameters after Testton to preserve this branch's
saved indices. Resource/library revisions and new metadata will also need recall
rules for older projects.

## Implementation order

1. Unify note ownership and carry release velocity through existing playback.
2. Add explicit release regions and audition the transition on a small sample set.
3. Introduce the portable register adapter and import one external single rank.
4. Extend note-scoped modulation and stereo source playback through those interfaces.
5. Split further engine/processor responsibilities when the implemented boundaries
   have tests and measured resource use.

Release and library work can share an asset model. Each milestone should retain
focused render checks, old-project loading and sample-library switching tests.
The current offline regression executable covers many subsystems; extracting
focused suites into CTest would make future failures easier to isolate.

## Related documents

- [macOS build and checks](MAC_BUILD.md)
- [Implemented DSF Testton](DSF_TEST_TONE.md)
- [Pitch accuracy in progress](in-progress/PITCH_ACCURACY_REVIEW.md)
- [Release velocity plan](to-do/RELEASE_VELOCITY.md)
- [Other sample-library plan](to-do/SAMPLE_LIBRARY_IMPLEMENTATION.md)
- [Stereo signal-path plan](to-do/STEREO_SIGNAL_PATH.md)
- [Vox humana: NeuralRysum v2, the neural stop](NeuralRysum_v2_paper.md)
