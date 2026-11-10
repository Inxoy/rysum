# DSF test tone

The Klang page contains a dedicated Testton panel. The reference is
monophonic and has its own level, independent of the organ's master level,
detuning, wind, tremulant and room effects. It enters the output before binaural
or loudspeaker decoding. In Ambisonics it occupies the omnidirectional W channel.

The Spieltisch also has a Testton button beside the registers, with Lautstärke
and Klangfarbe controls. These share the same enable, level and spectrum
parameters as the Klang page; tuning and frequency settings remain on Klang.

- **Testton an** enables the reference, initially off.
- **MIDI on:** the most recent held or sustained note determines the pitch.
  The reference follows the selected global tuning and Stimmton, with an
  independent Feinstimmung offset of ±50 cents, in 0.01-cent steps. MPE member-channel pitch bend
  follows the organ's configured range. The heading displays the calculated target
  frequency to six decimal places, including cents, octave and Mixtur adjustments.
  The cents fader displays hundredths and the Hz fader displays tenths.
- **MIDI off:** a continuous reference uses the editable absolute Frequenz value,
  from 20 to 20000 Hz, in 0.1-Hz steps, independent of global tuning and the MIDI offset. Click
  the numerical value for precise entry. Unsupported pitches at a low host sample
  rate are silent rather than reported at an incorrect frequency.
- **Spektrum:** zero is a sine; increasing the value introduces harmonics with
  integrated DSF weighting, up to 1024 partials below the output cutoff (45% of output sample rate).
  Brightness position x maps to geometric weight r = 1 − (1 − x)², spreading the
  upper-harmonic range over more of the fader. Zero remains a pure sine and full
  travel retains 1/n weighting. The quadratic response places r = 0.75 at half travel.
- Playback uses compensated trapezoidal integration. The rectangular selector is
  retired; the legacy parameter remains readable for saved-session compatibility.
- Cents and Hz controls adjust by 0.01 cents and 0.1 Hz respectively per horizontal pixel or wheel event. Alt-drag gives coarse adjustment; click the value to type
  an exact number. Clicking the track jumps to that position; dragging the thumb
  then makes fine adjustments. Double-click the cents fader to restore 0.00 cents.
  Frequency and spectrum changes preserve phase.
- **Oktavlage:** stepped MIDI transposition, 32′ / 16′ / 8′ / 4′ / 2′.
  8′ is untransposed; the other positions shift by −2 / −1 / +1 / +2 octaves.
  Manual Hz remains absolute.
- **Mixtur (x Grundton):** integer multiplier 1–9 after MIDI octave transposition.
  Use brightness zero to compare an individual harmonic of a recorded pipe.
  This moves the whole reference spectrum; it does not isolate a partial from
  the sample. Manual Hz remains absolute.
- **Number of Harmonic Partials:** maximum partial count, from 1 to 1024;
  the output-frequency cutoff can reduce it further. One produces a sine.
  Counts and saved values remain integers. Changes crossfade between phase-aligned
  spectra over 30 ms, before the downsampling filter. Rapid changes retain the
  latest target and finish the current fade before starting another.
- **Pegel:** independent output gain, initially −32 dB.

In Originalstimmung, the register menu to the right of MIDI selects the pipe
whose measured playback frequency Testton follows. It uses the checked
calibration where available and otherwise the manifest's measured frequency.
Notes without a measurement are silent. The menu lists single-rank registers,
including the derived Praestant 16′. Other tunings use their selected target
frequencies from MIDI 24 to 108, inclusive. Testton respects unmapped notes in
Scala and MTS and plays frequencies of 20 Hz or higher.

To check a sample, begin with Spektrum at zero and one organ stop enabled. In
MIDI mode, hold a note and adjust Feinstimmung to minimise beating; the heading
shows the resulting reference frequency. Alternatively, switch MIDI off and enter
or adjust an absolute Hz value. This is an aural comparison tool, not an automatic
frequency analyser; wind, random detuning, tremulant and Doppler can affect the
sample being compared.

The live reference reserves 8× oversampling throughout a sweep. Buffers and filters
are allocated in preparation. Frequency/spectrum controls are smoothed over 30 ms,
with integration-state updates at most every 32 output samples; gain changes use
a 20 ms ramp. Note starts and releases respect MIDI offsets within the host block.
The general renderer retains its per-note automatic factor selector.

Reference controls remain unchanged by sound-patch loading, scene morphing and
randomisation. Host project state can save and restore them. There is no automatic
sample-pitch correction or new sample-tuning metadata in this change.
