# NeuralRysum v2: Causal Audio-to-Organ Resynthesis through a Timbre-Blind Pitch Bottleneck

> This report belongs to [NeuralRysum](https://github.com/Inxoy/NeuralRysum/tree/redesign-pitch-synth) (training, models, Python reference). It is copied here because the plugin's stop *Vox humana* is built from it: the C++ engine is in `plugin/source/vox/`, the glue in `plugin/source/VoxStop.h`, the model in `plugin/resources/vox_humana.nrv2`.


*Technical report, branch `redesign-pitch-synth`, 17 November 2026.*
*Status: final models (pitch estimator 40 k steps, synth 150 k steps). Built into the Rysum plugin as
the stop "Vox humana" (in `main` of rysum_plugin); see Section 8.5.*

---

## Abstract

NeuralRysum turns any monophonic or polyphonic audio input, or MIDI, into the sound of a pipe organ, in
real time and causally. The first version (v1) was a split-rate autoencoder: a fast learned code every
64 samples carried pitch and envelope, and a slow latent carried timbre. Listening tests and a dedicated
leak test showed that v1 did not re-synthesise the input as an organ but passed a filtered version of it
through. A singing voice came out as a noisier singing voice. The fast code was rich enough to carry the
waveform, and no training change we tried (code noise, smaller code, strong timbre augmentation,
adversarial training on foreign input) closed that path.

v2 removes the path by construction. Its only connection from the input waveform to the output is a
small set of *voices*: a fractional MIDI key, a gate and the time since onset or release. These come
from a polyphonic pitch estimator that was trained only on synthetic sounds with random timbre, so it
carries pitch and nothing else. An organ synthesiser renders the voices: a per-voice network predicts
511 harmonic amplitudes and 32 noise-band gains, the harmonics are played from wavetables, and a learned
2-second room impulse response follows. The synth never sees the input waveform. The only timbre input
is v1's slow encoder (8-dim latent plus 3 descriptors, one 42.7 ms frame late), and in Manual mode it is
a fixed vector. MIDI drives the same voices directly.

On the leak test, v2 removes the input's high-frequency content: the level above 5 kHz drops by 36.5 dB
in Manual mode, against 17.5 dB for v1. Output for silent input is digital silence (−240 dB), against
−55 dBFS and a 750 Hz tone for v1. Note detection reacts in 9–33 ms depending on pitch. The whole model
has 0.85 M parameters.

---

## 1 Introduction

### 1.1 Goal

The plugin should make an organ play whatever comes in: a voice, a guitar, a synth line, a full mix, or
MIDI from a keyboard. "Organ" here means the specific instrument of the dataset (Section 4), including
its tuning and room. The constraints:

- **Causal and low latency.** It runs as a VST3 insert. No look-ahead beyond what the plugin reports as
  latency, and a target of a few milliseconds of structural delay.
- **Timbre comes from the organ, not the input.** The input decides *which notes* and *when*. How they
  sound must come from the model.
- **Controllable timbre.** The user can move through the organ's range of registrations (Manual), or
  let the input's brightness steer it (Follow).

### 1.2 What went wrong in v1

v1 (described in [ARCHITECTURE.md](https://github.com/Inxoy/NeuralRysum/blob/redesign-pitch-synth/docs/ARCHITECTURE.md)) encoded the input into a 12-dim fast code `e`
every 64 samples (1.33 ms) and a slow latent `z` every 2048 samples. A FiLM-conditioned decoder turned
`e` plus `z` back into audio. It was trained to reconstruct organ audio from organ audio.

Two problems showed up in use:

1. **A constant high-pitched tone** at 750 Hz = 48 000 / 64 in Manual mode, also on silent input. It was
   the decoder's frame rate leaking through, because `e` for silence lay outside anything seen in
   training.
2. **The input's timbre came through.** Through a microphone, a voice came out as a noisier voice.

The second problem is fundamental. A 12-dim code at 750 frames per second is 9000 numbers per second.
That is enough to describe a waveform's spectral envelope in detail, and a model trained only on organ
audio has no reason not to use it. On organ input, copying the input *is* the right answer. Training
never sees a case where copying is wrong.

We measured this with a leak test (Section 6.1) and tried four fixes on v1: noise on the code, a smaller
code, strong timbre augmentation of the encoder input, and an adversarial fine-tune with synthetic
foreign input. None reduced the leak meaningfully. The foreign-input fine-tune removed the 750 Hz tone
(silence at −71 dBFS) but made the leak worse. Manual "transfer" fell from 56 % to 47 % and Follow from
43 % to 28 % (100 % = organ). The level above 5 kHz in Follow went from −8 dB to +3 dB relative to the
input. The conclusion was that the leak is a property of the architecture, not of the
training.

### 1.3 The v2 idea

Make the only fast path from input to output something that cannot carry timbre:

```
audio --> PitchEstimator --> VoiceTracker --> voices (key, gate, ages) --> OrganSynth --> organ
MIDI  -----------------------------------------------^                        ^
audio --> slow timbre encoder (z + 3 descriptors, Follow only) ---------------/
knobs  (z + 3 descriptors, Manual) -------------------------------------------/
```

- The **pitch estimator** is trained on synthetic sounds whose timbre is random in every example, so its
  output is invariant to timbre by training. It is frozen when the synth is trained.
- Between estimator and synth sits a **hard symbolic bottleneck**: up to 8 voices, each a key number, an
  on/off gate and two timers. No continuous spectral information crosses it. Even if the estimator
  leaked timbre into its salience map, the tracker throws everything away except peak positions.
- The **synth** must produce the organ sound from keys alone. It cannot copy anything, because there is
  nothing to copy.

The price is that the output can only be "notes on an organ". Expressive detail of the input
(dynamics within a note, vibrato depth, articulation) comes through only as far as it changes which
keys are pressed. For an organ, which itself has no velocity, this is the right trade.

---

## 2 Related work

- **DDSP** (Engel et al., 2020) drives a harmonic-plus-noise synthesiser from f0 and loudness. v2
  borrows the harmonic-plus-noise model and the learned reverb. It differs in three ways: it is
  polyphonic, it has no loudness input (organ pipes do not respond to touch), and it plays harmonics
  from wavetables so the cost does not grow with the number of harmonics.
- **RAVE** (Caillon & Esling, 2021) is a VAE with a multi-band decoder for real-time timbre transfer.
  v1 was close to this family and inherited its weakness for our use: a continuous latent at a high
  frame rate carries input timbre.
- **Polyphonic pitch estimation** with harmonic stacking of a log-frequency spectrum follows the
  harmonic CQT idea of Bittner et al. (2017, "Deep Salience") and the lightweight model of Basic Pitch
  (Bittner et al., 2022). Our estimator is far smaller (17 k parameters) and strictly causal.
- **Adversarial training** uses multi-resolution STFT discriminators with hinge loss and feature
  matching, as in MelGAN (Kumar et al., 2019) and later vocoders.

---

## 3 Method

### 3.1 Signal flow and rates

| Stage | Rate | Hop | Frame period |
|---|---|---|---|
| Audio | 48 kHz | 1 | 0.021 ms |
| Pitch estimator, tracker | 750 Hz | 64 | 1.33 ms |
| Synth control (voices, harmonics, noise) | 375 Hz | 128 | 2.67 ms |
| Timbre (z, descriptors) | 23.4 Hz | 2048 | 42.7 ms |

All processing is causal. Every frame uses only samples up to its own last sample; the slow timbre path
is additionally delayed by one slow frame.

### 3.2 Pitch estimator

**Front end (fixed, no parameters).** Four causal STFTs with Hann windows of 512, 1024, 2048 and 4096
samples, all at hop 64, left-padded so that frame *k* ends at sample (*k*+1)·64 − 1. The magnitudes are
resampled onto a log-frequency grid with 3 bins per semitone, by linear interpolation between FFT bins.
Each log bin takes its value from the *shortest* window that still resolves a semitone at that frequency
(window length ≥ 2·sr / (f·(2^(1/12) − 1))). High notes therefore react fast, and low notes wait for
the long window. Magnitudes are scaled so that a full-scale sine reads 1, converted to dB and mapped
from −100…0 dBFS to 0…1.

**Harmonic stacking.** The log spectrum is shifted by 12·3·log2(h) bins for
h ∈ {½, 1, 2, 3, 4, 5, 6, 7} and stacked as 8 channels. At the bin of a candidate pitch, channel *h*
then shows the energy at its *h*-th harmonic, so a 2-D convolution can read off "is this a harmonic
series?" locally. The output grid covers MIDI 21–108 (A0–C8) in 262 bins.

**Network.** A causal 2-D CNN over (frequency, time), causal in time and centred in frequency, with
leaky ReLU (0.2):

| Layer | Kernel (f × t) | Dilation (t) | Channels | Notes |
|---|---|---|---|---|
| l1 | 5 × 3 | 1 | 8 → 24 | |
| l2 | 3 × 3 | 2 | 24 → 24 | residual |
| l3 | 3 × 3 | 4 | 24 → 24 | residual |
| l4 | 13 × 1 (dilation 3 in f) | – | 24 → 12 | spans one octave in semitone steps |
| out | 1 × 1 | – | 36 → 2 | on concat(l3, l4) |

The time receptive field is 15 frames (20 ms) on top of the STFT window. The two outputs are
**salience** logits (is a note sounding at this pitch?) and **onset** logits (does a note start here?),
each 262 bins × 750 frames/s. Parameters: **17 150**.

### 3.3 Synthetic training data for the estimator

The estimator never sees real recordings. Each batch is generated on the GPU with exact labels
([pitch_data.py](https://github.com/Inxoy/NeuralRysum/blob/redesign-pitch-synth/neuralrysum/pitch_data.py)):

- **Notes.** 0–6 voices per example (P = .05, .35, .20, .15, .12, .08, .05), each with 1–2 notes,
  pitch uniform in MIDI 24–96. 70 % of notes are snapped near a semitone (σ = 15 cents) and 30 % lie
  anywhere. A second note repeats the first pitch with P = 0.4, which teaches repeated-note onsets.
  Glides of ±5 semitones (15 %) and vibrato of 0.1–0.8 semitones at 4–7 Hz (25 %).
- **Envelopes.** Attack 1–80 ms, release 10–300 ms, sustained or exponentially decaying
  (τ = 0.1–2 s), level −18…0 dB per voice.
- **Timbre, random per voice.** 64 harmonics with spectral tilt k^(−0.3…−2.5) and log-normal jitter.
  15 % odd harmonics only, 10 % missing fundamental, 25 % three formants (300–3500 Hz) at fixed
  frequencies, 15 % inharmonicity (stiff string, B = 10⁻⁵…10^(−3.3)), and frequency-dependent decay
  for plucked sounds.
- **Environment.** Room reverb (30 %, T60 0.2–2.5 s, mix 10–50 %), random EQ ±12 dB (50 %),
  coloured background noise −70…−30 dB (50 %), noise bursts (10 %), and overall gain −35…−1 dBFS.

**Labels.** At each frame end, the active pitches are drawn as Gaussians (σ = 1 bin) on the salience
map. Onsets are drawn the same way for the first two frames of each note. A note counts as active while
its envelope is above 1 % of its peak and before its release.

**Loss.** Binary cross-entropy with positive weighting: weight 1 + 9·target for salience and
1 + 19·target for onset (onsets are sparse).

### 3.4 Voice tracker

The tracker turns salience and onset frames into up to 8 continuous voices. It is plain code, not
learned, and the plugin runs exactly the same logic. Per frame:

1. **Peaks.** Local maxima of sigmoid(salience) above `off` = 0.3, with parabolic refinement over
   3 bins to a fractional MIDI number.
2. **Continue.** Active voices, oldest first, claim the nearest unused peak within `match` = 0.7
   semitones and follow its pitch. This handles glides, vibrato and detuned pipes.
3. **Restart.** If the onset probability at the voice's pitch exceeds 0.5 and the voice is older than
   6 frames, the voice restarts (`on_age` → 0). This is meant for repeated notes on the same key.
4. **Hold.** A voice that finds no peak keeps its gate for up to `hold` = 24 frames (32 ms) before it is
   released. Without this, a single missed frame cuts the note, which in testing gave 68–82 note starts
   per second on real music with a median note length of 17 ms.
5. **Start.** A remaining peak above `on` = 0.6 starts a voice only after it has been seen in
   `confirm` = 4 consecutive frames (5.3 ms), matched within `match` semitones from frame to frame.
6. **Fold.** Keys outside the organ range [36, 84) (C2 up to, but not including, C6) are moved by whole
   octaves into it.

### 3.5 From tracker frames to synth frames

The tracker runs every 64 samples, the synth every 128. Synth frame *f* starts at sample *f*·128 and may
only use tracker frames that have *ended* by then (the last one is frame 2*f* − 1). A start event in
either of the two covered tracker frames sets `on_age` to 0. Each voice is described at the start of
every synth frame by:

| Feature | Meaning |
|---|---|
| `key` | fractional MIDI number; ≤ 0 = unused voice |
| `gate` | 1 while held |
| `on_age` | seconds since the (re)start |
| `off_age` | seconds since release, −1 while held |

MIDI input produces exactly the same four arrays. Notes are assigned to voices in onset order. A note
takes the first voice that is free (its last note has been released for at least `release_hold` =
0.5 s), and otherwise steals the voice that becomes free first. This is the same assignment used to
prepare the training data.

### 3.6 Organ synth

**Per-voice network (VoiceNet).** For each voice and frame, a 47-dim feature vector:

- a learned 16-dim **key embedding**, interpolated linearly between the two neighbouring integer keys
  (21–108). Every pipe of a real organ is voiced individually, so each key gets its own vector;
- the key position (key − 60)/24 and 8 Fourier features of it (sin/cos at 4 octaves);
- the gate;
- 5 **onset features** exp(−on_age / τ), τ ∈ {10, 30, 100, 300, 1000} ms, which describe the attack
  transient and chiff;
- 4 **release features** exp(−off_age / τ), τ ∈ {10, 30, 100, 300} ms, plus a "released" flag;
- the 11-dim **timbre conditioning** (z plus 3 descriptors), shared by all voices.

An MLP of width 192 (3 layers, residual on layers 2 and 3, leaky ReLU 0.2) maps this to
**511 harmonic log-amplitudes** and **32 noise-band log-gains**. The output layer starts at bias −9, so
the untrained synth is silent rather than noisy. Parameters: 189 535.

**Why harmonics of key/2.** Organ ranks sound at 16′, 8′, 4′, 2⅔′, 2′, 1⅗′ and so on: sub-octave, unison,
octave, twelfth, fifteenth, seventeenth. All of them are harmonics of the 16′ pitch, which is one octave
below the key. One harmonic series on key/2 with 511 partials therefore covers any registration, and the
network learns which partials a stop combination excites. This is what lets one model represent
everything from a single flute to full plenum with mixtures.

**Wavetable oscillator.** Instead of summing 511 sines per voice per sample, each frame's amplitudes
are turned into one 1024-sample wavetable by an inverse FFT, with fixed random phases per harmonic.
Fixed phases avoid the peaky waveform of all-zero phases and keep the table continuous from frame to
frame. Per sample:

- the phase accumulates the instantaneous frequency, which glides linearly from frame *f*−1 to frame *f*
  over the hop;
- the sample reads both tables (frame *f*−1 and frame *f*) with linear interpolation and crossfades
  them linearly over the 128-sample hop. Frame −1 is silent, so a note fades in over one hop.

The cost per sample is constant: two table reads per voice, whatever the number of harmonics. Harmonics
above 0.45·sr are removed, and those between 0.40 and 0.45·sr are faded out against aliasing. A voice
sounds while its gate is on and for `release_hold` = 0.5 s after release.

**Noise.** White noise is split into 32 log-spaced bands from 50 Hz to 22 kHz, each scaled to unit
power. The per-voice band gains are summed over voices and interpolated linearly over each hop. This
models wind noise and the chiff at a pipe's onset. Training uses zero-phase FFT band filters; a causal
filter bank in the plugin gives the same statistics, because the excitation is random.

**Room.** The dry signal is convolved with a learned 2.0 s impulse response (96 000 taps, initialised as
exponentially decaying noise, τ = 0.5 s), and the result is added to the dry signal. The dataset's
reverb tail to −40 dB is 1.5 s median and 2.35 s at most, so 2 s covers almost all of it.

### 3.7 Timbre path

The timbre encoder is v1's: the trunk of the fast encoder (strided causal convolutions 1 → 32 → 64 → 96
channels, total stride 64) followed by the slow encoder (strided convolutions to 128 channels at hop
2048, a GRU, and a linear head). It outputs a Gaussian posterior over an **8-dim latent `z`** and
predicts **3 descriptors**:

| Descriptor | Measure | Scale |
|---|---|---|
| brightness | spectral centroid | 0 = 1 kHz, ±1 = ±3 octaves |
| noisiness | spectral flatness | +1 = white noise, −1 = pure tones |
| spread | spectral bandwidth | 0 = 1 kHz, ±1 = ±3 octaves |

During training the synth receives `z` sampled from the posterior of the *target clip* together with
the target's measured descriptors, both delayed by one slow frame and held over the 16 synth frames of
each slow frame. This is how the synth learns what a registration sounds like from a compact
description of it.

At run time:

- **Manual:** `z` and the descriptors are fixed values: the knobs, or the mean over organ clips as the
  default. Nothing about the input reaches the synth except the voices.
- **Follow:** the encoder runs on the input, so the input's brightness and noisiness steer the
  registration. This is a deliberate, low-rate (23 Hz, 11 numbers) channel. It can make the organ
  brighter when the input is brighter, but it cannot transmit a waveform. Measured leakage in Follow is
  higher than in Manual (Section 6), as intended.

Parameters: trunk 152 k, slow encoder 397 k.

---

## 4 Data

**Source.** 3600 renders of 10 s from the user's "Rysum" organ plugin (48 kHz, stereo, 16-bit), each
with a MIDI note list. There are three kinds:

- **pipe** (1439): single stops;
- **registration** (1441): stop combinations;
- **patch** (720): with effects.

v2 trains on **pipe + registration only**: 2880 clips, of which 2736 are for training (7.6 h) and 144
for validation, following the dataset's split. The patch clips are left out because their effects are
not part of the instrument. Clips are mixed to mono.

**Observed properties** that shaped the design:

- **No dynamics.** Note velocity has no effect on level (correlation −0.03), so the synth has no
  velocity input.
- **Unequal temperament.** A is about +15 cents above A440, and the median offset per pitch class
  ranges from 0 (G♯) to +36.8 cents (E♭). Training on nominal MIDI keys would teach the synth to detune,
  so every note's real pitch is measured from the audio. The FFT is 2¹⁸ points long, the window runs
  from 150 ms after onset up to 2 s, and the measurement uses partials h ∈ {½, 1, 2, 3, 4, 5, 6, 8}
  below 8 kHz. Partials that another sounding note could own are skipped. Each partial's peak is
  refined parabolically, and the note's offset is the amplitude-weighted median over partials. Notes
  without a usable estimate, or beyond ±45 cents, get the median of their pitch class. The synth key is
  the measured fractional MIDI number, so it reproduces the temperament exactly when driven by MIDI, and
  plays any tuning when driven by tracked audio.
- **Polyphony** up to 8, notes between MIDI 36 and 81.

**Training windows.** Random windows of 65 536 samples of *pre-roll* (1.37 s, lets the reverb fill and
the timbre encoder settle; not scored) plus 32 768 scored samples (0.68 s). Windows that are silent in
the scored part are redrawn. The gain is varied between −6 and 0 dB.

---

## 5 Training

### 5.1 Pitch estimator

AdamW with learning rate 10⁻³ and weight decay 10⁻⁴. The schedule is a linear warm-up over 500 steps
followed by cosine decay. Batches are 12 × 65 536 samples, generated fresh every step, for 40 000 steps.
Gradients are clipped at 5. Training took about 3.4 h on an RTX 5070, while sharing the GPU with the
synth.

### 5.2 Synth

The estimator plays no part here. The synth is trained on the ground-truth voices from the MIDI note
lists, so it never learns to compensate for tracker errors. The two models meet only at the voice
interface.

- **Loss before step 40 000:** multi-resolution STFT loss (FFT sizes 2048, 1024, 512, 256, 128; spectral
  convergence plus log-magnitude L1 with floor 10⁻³), plus KL on `z` (weight 10⁻², warmed up over
  20 k steps), plus MSE of the predicted descriptors.
- **From step 40 000:** in addition, a multi-resolution complex-STFT discriminator (FFT sizes 2048,
  1024, 512, 128; 2-D convolutions with weight norm). It uses hinge loss and feature matching with
  weight 10.
- **Optimiser:** AdamW with learning rate 3·10⁻⁴ and β = (0.8, 0.99), batch 8, gradient clipping at 10,
  150 000 steps.
- **Validation:** spectral loss on 32 fixed validation windows every 5000 steps.

Validation spectral loss was 0.97 at 5 k steps and 0.91 at 10 k. Since the adversarial phase began at
40 k it has stayed between 0.84 and 0.89, which is expected: the discriminator trades exact spectral
match for realism.

---

## 6 Evaluation

### 6.1 Leak test

The leak test ([leak_test.py](https://github.com/Inxoy/NeuralRysum/blob/redesign-pitch-synth/scripts/leak_test.py)) sends three synthetic **non-organ** signals
through the model: a sawtooth, sung vowels and a plucked string, four notes of known pitch each. Each is
run once in Manual (dataset-mean timbre) and once in Follow. It reports:

- **transfer:** where the output's spectral shape lies between the input's (0 %) and the organ
  dataset's (100 %). It is measured as RMS distance of third-octave band levels, 50 Hz–16 kHz, with the
  overall level removed.
- **HF:** output level above 5 kHz relative to the input's, in dB. More negative means less of the
  input's top end gets through.
- **pitch:** share of output energy (50 Hz–5 kHz) on harmonics of the played note.
- **env:** correlation of input and output loudness envelopes (10 ms frames).
- **organ:** spectral loss when reconstructing clips of the organ dataset itself.
- **silence:** output level for silent input in Manual mode.

| Model | Manual transfer | Manual HF | Manual pitch | Manual env | Follow transfer | Follow HF | Follow pitch | Follow env | Silence |
|---|---|---|---|---|---|---|---|---|---|
| v1 | 56 % | −17.5 dB | 98 % | 0.51 | 43 % | −8.2 dB | 99 % | 0.83 | −54.8 dBFS (750 Hz tone) |
| v1 + foreign fine-tune | 47 % | −12.3 dB | 99 % | 0.93 | 28 % | +2.7 dB | 99 % | 0.88 | −71.1 dBFS |
| v2, early (pitch 5 k, synth 5 k) | 55 % | −30 dB | 90 % | −0.03 | 62 % | −32 dB | 85 % | 0.07 | −240 dB |
| v2 (pitch 40 k, synth 50.7 k) | 50 % | −36.5 dB | 97 % | 0.23 | 64 % | −29.9 dB | 91 % | 0.25 | −240 dB |
| **v2 final (pitch 40 k, synth 150 k)** | **47 %** | **−36.5 dB** | **94 %** | **0.27** | **58 %** | **−30.3 dB** | **93 %** | **−0.06** | **−240 dB** |

The final v2 results for each input signal:

| Signal | Mode | Transfer | HF | Pitch | Env |
|---|---|---|---|---|---|
| saw | Manual | 47 % | −48.8 dB | 98 % | 0.01 |
| saw | Follow | 46 % | −42.8 dB | 97 % | −0.02 |
| voice | Manual | 42 % | −27.6 dB | 93 % | 0.67 |
| voice | Follow | 72 % | −17.0 dB | 92 % | −0.01 |
| pluck | Manual | 52 % | −33.2 dB | 90 % | 0.11 |
| pluck | Follow | 56 % | −31.2 dB | 90 % | −0.17 |

The organ reconstruction loss of the final model is 2.24 and its validation spectral loss 0.861.

**Reading the numbers.**

- **HF is the clearest leak signal.** A copying model keeps the input's top end, and the v2 synth has
  no way to produce it except by playing organ partials there. Averaged over the three signals, v2 removes
  30–37 dB above 5 kHz, where v1 removes 8–18 dB. For single signals, the range is 18–48 dB for v2 and
  1–29 dB for v1.
- **"Transfer" saturates near 50–65 % for v2.** This is a limitation of the metric, not leakage. It
  compares coarse third-octave shapes, and a sawtooth, a sung vowel and a full organ registration share
  a similar spectral tilt. A model that, by construction, cannot output anything but organ partials
  cannot be "copying" at 50 %.
- **Pitch share** is 97 % in Manual: the notes are right.
- **Envelope correlation** is low by design. The organ has no dynamics, so loudness follows only note
  on/off.
- **The organ reconstruction loss** is not comparable across versions. v1 reconstructs organ audio
  *from the audio itself*, which is exactly the shortcut that caused the leak, so it scores low. v2
  reconstructs it from MIDI keys and a timbre latent. Its value of 2.16, against 1.99 for the early
  snapshot, rose when the adversarial phase began.

### 6.2 Real music

A 99 s bass-heavy full mix (44.1 kHz MP3, resampled to 48 kHz mono) was rendered through every mode:

| Output | Transfer | Share of energy above 5 kHz |
|---|---|---|
| Input | – | −22.1 dB |
| v1 Follow | 27 % | −25.6 dB |
| v2 Follow (early) | 64 % | −36.0 dB |
| v2 Manual (early) | 64 % | −35.8 dB |
| Organ dataset | – | −38.5 dB |

With the final estimator, the mix gives the following:

- **Voices:** 4.6 sound on average, 8 at most, and the output is silent 7 % of the time.
- **Note starts:** 26.3 per second; strict tracking (`confirm` 6, `on` 0.7, `match` 1.0) gives 12.9
  per second and 3.3 voices on average.
- **Tracked pitches:** MIDI 36–84 after folding, median 55.

The estimator's onset head never exceeded 0.5 on this material. Repeated notes on the same key
therefore merge into one held note (Section 7).

Listening verdict from the user: the v2 output no longer sounds like the input material, which is the
property v1 lacked.

### 6.3 Latency

The structural latency of the MIDI path is at most one synth hop to the next frame boundary, plus the
one-hop fade-in: 2.7–5.3 ms.

For the audio path, the latency is measured on harmonic tones (11 partials, 1/k amplitude, onset at
0.5 s). It is the time until the first synth frame with an open gate:

| Note | 36 (C2) | 43 | 48 | 55 | 60 (C4) | 67 | 72 | 84 (C6) |
|---|---|---|---|---|---|---|---|---|
| Gate after (ms) | 33.3 | 20.0 | 22.7 | 17.3 | 17.3 | 12.0 | 12.0 | 9.3 |
| Tracked key | **48.0** | 43.03 | 48.01 | 55.01 | 60.01 | 67.04 | 72.01 | 72.01 (folded) |

Add 2.7 ms of crossfade until full level. The latency falls with pitch because low notes need the long
STFT windows. The 4-frame confirmation adds 5.3 ms at every pitch.

Two findings:

- **Octave error at C2.** The lowest test note was tracked one octave too high, at 48. This is the
  octave-error mode that harmonic stacking is prone to when the fundamental is weak in the short
  windows.
- **C6 folds down.** The fold range is half-open, so C6 itself is moved down an octave.

### 6.4 Model size and compute

| Part | Parameters | Work per second of audio |
|---|---|---|
| Pitch estimator | 17 150 | ≈ 3.35 G MAC (4.47 M MAC per frame × 750 frames) |
| Voice tracker | – | negligible |
| VoiceNet | 189 535 | ≈ 0.56 G MAC (187 k MAC × 8 voices × 375 frames) |
| Wavetables | – | 3000 inverse FFTs (1024 points) + 2 table reads per voice per sample |
| Noise | – | 32-band filter bank |
| Room IR | 96 000 | partitioned FFT convolution, ≈ 0.15 G FLOP |
| Timbre encoder (Follow only) | 549 203 | small: the trunk runs at hop 64, the rest at hop 2048 |
| **Total** | **853 052** | **≈ 4 G MAC** |

The pitch estimator dominates. It runs in 1.9 s for 10 s of audio on one CPU thread in PyTorch, which
means 0.19 × real time. A SIMD C++ implementation should fit comfortably in one core. This has not
been measured yet.

---

## 7 Limitations

- **Bass.** The lowest octave of the organ range is the weakest part, with slow detection (33 ms at C2)
  and octave errors. Bass-heavy mixes are the hardest input.
- **Repeated notes.** The onset head is trained but practically never fires on real material, so the
  same key struck twice in quick succession becomes one note. Likely causes are the synthetic onset
  labels (2 frames, Gaussian in pitch) and the 1 + 19·t weighting, which together leave the logits low
  on real transients.
- **Dense polyphony.** With 8 voices and a symbolic interface, a full mix is reduced to its most salient
  pitches. The tracker has no musical model (key, voice leading). Wrong notes come out as wrong organ
  notes, cleanly, not as smeared timbre.
- **No dynamics, by design.** Input loudness affects only detection. A swell pedal or expression
  control could be added as a plugin parameter.
- **Octave folding** shifts bass lines up by one or two octaves. The fold range is half-open, so C6 is
  folded down.
- **One organ.** The synth knows one instrument and one room. Another organ needs another training run
  of the synth, but the pitch estimator can be reused unchanged.
- **The leak metric.** "Transfer" does not separate "organ-like" from "input-like" well when both have a
  similar tilt. A classifier-based metric (organ vs. not organ) would be more informative.

---

## 8 Integrating v2 into another plugin

v2 is two independent models with a fixed interface between them. Another plugin can use either half on
its own:

- **Organ synth only:** a MIDI-driven neural organ. Feed it voices from your own note handling.
- **Pitch estimator plus tracker only:** a causal polyphonic audio-to-MIDI front end with 9–33 ms
  latency, for driving any instrument.
- **Both:** the full audio-to-organ effect.

### 8.1 Interface

Configuration is fixed per checkpoint and stored in it (`config` dict):

```
sample_rate 48000     synth hop 128    estimator hop 64    slow hop 2048
voices 8              table 1024       harmonics 511       noise bands 32
key range 21..108     fold range [36, 84)    release_hold 0.5 s    IR 96000 taps
```

**Per synth frame (every 128 samples)**, the host side provides for each of the 8 voices
`key`, `gate`, `on_age`, `off_age` (Section 3.5), and once for all voices the 11-dim conditioning
`[z0..z7, brightness, noisiness, spread]`. The synth returns 128 mono samples.

**Per estimator frame (every 64 samples)**, the estimator consumes 64 new input samples and returns
262 salience and 262 onset logits. The tracker turns these into 8 × (midi, gate, started).

### 8.2 Real-time algorithm per block

For each block of 128 samples (two estimator frames, one synth frame):

1. Push the 128 samples into four ring buffers (512–4096). Twice, every 64 samples:
   - compute the four windowed FFT magnitudes at the precomputed log-bin indices;
   - convert to dB, stack harmonics and run the CNN on the newest frame. The time convolutions need
     only a short history of past activations per layer: 2, 4, 8 and 0 frames;
   - step the tracker.
2. Convert the tracker state to voice features, or take them from MIDI. Then advance
   `on_age` / `off_age` by 128 / 48 000 s.
3. **Follow mode only:** run the trunk on the block. Every 2048 samples, run the slow encoder (GRU
   state persists) and update the conditioning *after* the frame, which gives the one-frame delay.
4. Run VoiceNet for each live voice: 47 → 192 → 192 → 192 → 543. Skip dead voices, which saves most of
   the cost at low polyphony.
5. For each voice: build the table by an inverse FFT of `amp · 512 · e^{i(φ_k − π/2)}`, keeping the
   previous table. Render 128 samples with phase accumulation, linear table interpolation and a linear
   crossfade from the old table to the new one.
6. Run the noise filter bank with the summed band gains, interpolated over the block.
7. Convolve with the room IR using uniformly partitioned FFT convolution (block 128, 750 partitions) and
   add the result to the dry signal.

**State to keep:** the ring buffers, the CNN activation history, the tracker state, the per-voice
phase and previous table, the noise filter states, the IR convolution history, and in Follow mode the
encoder's conv buffers and GRU state.

**Fixed arrays to export:** the per-harmonic phases φ_k (511 values, from the checkpoint buffer
`phases`), the log-bin interpolation indices and fractions, and the four Hann windows. All of them can
be recomputed from the config, but exporting them guarantees bit-exact behaviour.

**Latency to report to the host.** Around 15–20 ms is a reasonable compromise for the audio path; there
is no exact value, because detection time depends on pitch (Section 6.3). The MIDI path needs one block
(128 samples).

### 8.3 Controls worth exposing

| Control | Maps to |
|---|---|
| Timbre X/Y or 8 macro knobs | `z` (start from the dataset mean, range about ±2 σ) |
| Brightness, Noisiness, Spread | descriptors |
| Manual / Follow | fixed conditioning or the timbre encoder on the input |
| Sensitivity | tracker `on` (0.5–0.8) |
| Stability | `confirm` (2–8 frames) and `hold` |
| Max voices | 1–8, monophonic for lead lines |
| Octave / range | fold range and transposition before the synth |
| Room mix | scale the wet part of the IR convolution, from 0 (dry) to 1 |
| Release | `release_hold` (the synth has learned 0.5 s; shorter values cut the tail) |

### 8.4 Files

- Checkpoints: `runs/pitch/last.pt` (kind `pitch`) and `runs/synth/last.pt` (kind `synth`). These are
  PyTorch state dicts plus config, loaded by `load_pitch` / `load_synth` in
  [v2.py](https://github.com/Inxoy/NeuralRysum/blob/redesign-pitch-synth/neuralrysum/v2.py). The synth checkpoint also holds the discriminator and optimiser
  states, which are only needed to resume training.
- `python -m neuralrysum.export_v2` writes the C++ file (`.nrv2`, magic `NRV20001`, 3.4 MB): all
  weights, the config, φ_k, the log-bin tables, the Manual timbre and the PCA axes of `z` over the
  organ data. `scripts/make_v2_reference.py` writes reference vectors for testing a port.
- Reference implementation: `V2.transfer` (audio) and `V2.midi` (MIDI) produce the output the C++ port
  has to match.

### 8.5 The Rysum plugin: the stop "Vox humana"

v2 is built into the sample-based Rysum plugin as a ninth stop, *Vox humana 8′*, plus an option
*Eingang spielt die Orgel* (branch `vox-humana`):

- **C++ engine** (`plugin/source/vox/`, header-only): the estimator with an AVX2 + FMA kernel for the
  CNN, the tracker, the synth (voice network batched over all voices, PFFFT inverse FFTs for the
  wavetables, a 4th-order band-pass noise bank, partitioned convolution for the room) and the Follow
  encoder. Against PyTorch (`tests/VoxTest.cpp`): estimator logits within 1.4·10⁻³, tracker decisions
  identical, MIDI rendering within 3·10⁻⁹, tracked rendering within 1 % of peak (phase drift from
  10⁻⁵-semitone pitch differences). One core: 3.5 % for 8 played voices, 7 % for the estimator,
  15 % for everything at once.
- **Glue** (`VoxStop.h`): runs the engine at 48 kHz around any host rate (Hermite resampling), maps
  held keys to fractional keys in the plugin's tuning (all temperaments, Stimmton, Scala, MTS-ESP),
  uses 8 voices for tracked notes and 8 for played ones, and turns tracker voices into MIDI notes on
  channel 16 (folded into the compass, skipping the short octave's gaps), which the sample stops play.
- **Placement**: the engine treats the Vox humana as one more source like the audio input, with its own
  direction, distance, room send and early reflections.
- **Measured in the plugin** (`tests/VoxRender.cpp`, 20 s of a full mix as input): the Vox humana
  alone is about as loud as one sample stop (−28 dBFS RMS against −25.7 dBFS for Gedackt + Octave);
  no clicks; whole plugin 4 % CPU for the Vox humana from the keys, 15 % with the input playing it,
  25 % with input, Follow, Vox humana and sample stops together.

**Where it appears in the plugin:**

| Page | What it shows |
|---|---|
| Spieltisch | the knob *Vox humana* with Lautstärke and Wind, like the other stops; *Vox …* opens the timbre controls (Klangfarbe X/Y, Helligkeit, Hauch, Breite) and the input options (*Eingang spielt die Orgel*, *Klangfarbe folgt dem Eingang*, detection threshold, confirmation time, number of voices) |
| Raum | a row under the audio input with its own panel: Lautstärke, Hall-Anteil, Richtung, Höhe, Abstand, the learned room; a symbol in the top view that is dragged to place it |
| Saal | the symbol in the floor plan, in the room of the audio input; in the 3D view its own model (below) |
| Modulation | all its parameters form the target group *Vox humana*; the engine reads their modulated values, so an LFO on Klangfarbe X glides through the registrations |

**The 3D model.** The stop has no pipes, so it gets a picture of what the network does. Its eight
voices are glass pipes standing in a row at its place, each as long as an open 8′ pipe of the pitch
it sings at that moment (2.44 m · 2^−(key−36)/12), so a tracked voice that glides visibly grows or
shrinks. Around each pipe sit eight collars, one per band of its harmonics on key/2: 16′, 8′, 5⅓′,
4′, 3⅕′–2⅔′, 2²⁄₇′–2′, 1⁷⁄₉′–1′ and higher, from the bottom up. A collar's width and brightness show
that band's strength relative to the strongest, so a flute shows mostly the 8′ collar and a plenum
with mixtures lights the upper ones; turning the timbre controls (or modulating them) changes the
collars. A sounding pipe glows with its level and is a lamp when the light is off. Each pipe shows
the louder of its tracked voice and its played voice. The synth publishes key, level and the eight
band energies per voice and frame for this (`OrganSynth::Display`, lock-free).

---

## 9 Next steps

1. Listening tests of the Vox humana in a host; tune the default level, tracker defaults and room.
2. Re-run Section 6.2 (the full mix) with the final model.
3. Improve the low end: longer analysis windows only for the lowest octave, or a separate bass head;
   make the onset labels and weights fit real transients.
4. Consider training on the patch clips as well, or modelling effects as a separate post stage.

---

## Appendix A: Reproduction

```
# data (once)
python -m neuralrysum.organ_data <rysum_renders> data/rysum_organ_v2 --kinds pipe,registration

# pitch estimator (synthetic data, no dataset needed)
python -m neuralrysum.train_pitch --out runs/pitch --steps 40000 --channels 24

# synth
python -m neuralrysum.train_synth --data data/rysum_organ_v2 --out runs/synth --warmup_adv 40000

# evaluation
python scripts/leak_test.py --data data/rysum_organ_v2 --ckpt runs/synth/last.pt --pitch runs/pitch/last.pt --detail
python scripts/render_compare.py input.wav out_dir --pitch runs/pitch/last.pt --synth runs/synth/last.pt

# render a file
python -m neuralrysum.v2 --pitch runs/pitch/last.pt --synth runs/synth/last.pt --in in.wav --out out.wav [--follow]
```

Tests: `tests/test_v2.py` checks the estimator's causality, the synthetic labels, the synth's causality
and silence, the voice frames, the tracker and octave folding.

## References

- Engel, Hantrakul, Gu, Roberts. *DDSP: Differentiable Digital Signal Processing.* ICLR 2020.
- Caillon, Esling. *RAVE: A variational autoencoder for fast and high-quality neural audio synthesis.*
  arXiv:2111.05011, 2021.
- Bittner, McFee, Salamon, Li, Bello. *Deep Salience Representations for F0 Estimation in Polyphonic
  Music.* ISMIR 2017.
- Bittner, Bosch, Rubinstein, Meseguer-Brocal, Ewert. *A Lightweight Instrument-Agnostic Model for
  Polyphonic Note Transcription and Multipitch Estimation.* ICASSP 2022.
- Kumar et al. *MelGAN: Generative Adversarial Networks for Conditional Waveform Synthesis.*
  NeurIPS 2019.
