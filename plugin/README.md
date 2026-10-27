# Rysum – organ plugin (VST3 + Standalone)

Sample-based plugin of the Rysum organ (c. 1457, ¼-comma meantone, a′ ≈ 444.5 Hz),
7 stops, short octave C/E – a″ (41 keys), with a key-click/noise layer, per-pipe
placement in stereo or AmbiX up to 5th order, moving pipes and a degradation
("Verfall") section.

## 1. Build (Windows, once)

Needs: Visual Studio 2022 (workload "Desktop development with C++") and CMake.
Git is **not** needed – JUCE is downloaded as a zip on the first configure.

Open **PowerShell** and run:

```powershell
cd D:\RYSUM_plugin\rysum_plugin\plugin
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The first `cmake -B build …` downloads JUCE (~100 MB) and takes a few minutes.
The build itself takes a few minutes too. When it is done you have:

| what | where |
|---|---|
| VST3 plugin | `build\Rysum_artefacts\Release\VST3\Rysum.vst3` |
| Standalone app | `build\Rysum_artefacts\Release\Standalone\Rysum.exe` |

After changing code, only the second command (`cmake --build build --config Release`) is needed.

### Trying a new version next to the stable one

Build a second copy called **Rysum Dev** into its own folder. It has its own plugin ID, so
Reaper lists both and your old projects keep using the stable "Rysum":

```powershell
cd D:\RYSUM_plugin\rysum_plugin\plugin
cmake -B build-dev -G "Visual Studio 17 2022" -A x64 -DRYSUM_DEV=ON
cmake --build build-dev --config Release
```

Result: `build-dev\Rysum_artefacts\Release\VST3\Rysum Dev.vst3`. Add
`D:\RYSUM_plugin\rysum_plugin\plugin\build-dev\Rysum_artefacts\Release\VST3` to Reaper's VST
paths once and re-scan. When you're happy with a new version, rebuild the stable one
(`cmake --build build --config Release`, with Reaper closed).

## 2. Use it in Reaper

Either copy the whole `Rysum.vst3` folder to `C:\Program Files\Common Files\VST3`
(needs admin rights), **or** add the build folder to Reaper:
*Options → Preferences → Plug-ins → VST → Edit path list → add*
`D:\RYSUM_plugin\rysum_plugin\plugin\build\Rysum_artefacts\Release\VST3`, then *Re-scan*.

The plugin finds the samples by itself in `D:\RYSUM_plugin\rysum_plugin\samples`
(the output of `rysum_preprocess.py`). If they are somewhere else, click
**Samples-Ordner …** on the Spieltisch page and choose the folder that contains
`rysum_samples.json`.

### Stereo
Output "Stereo" (dropdown at the top right). The plugin writes to channels 1/2.

### Ambisonics (AmbiX, ACN/SN3D)
1. Set the track to enough channels: click the track's **Route** button →
   *Track channels*: 4 (1st order), 9, 16, 25 or **36 (5th order)**.
2. In the plugin choose the output format in the dropdown at the top right (Stereo or
   AmbiX 1.–5. Ordnung). Next to it you see how many channels that needs; it turns white
   with a warning if the track has fewer.
3. Decode after the plugin, e.g. IEM *BinauralDecoder* (headphones) or *AllRADecoder*
   (speakers) – the format is the same as the IEM suite (ACN order, SN3D).

The plugin's output bus is declared as 5th-order Ambisonics (36 ch). In stereo
mode only channels 1/2 carry sound.

## 3. The four pages

**Every slider:** click its value to type a number (in the units shown: `50` on a % slider
= 50 %, `300 ms` or `1.5 s`, `440` on the Stimmton = a′ in Hz, `22` on Abtastrate = 22 kHz),
Return applies. Double-click the slider = default. Right-click = modulation.

**Stops:** eight – the seven recorded ones plus a *Praestant 16′*, made from the Praestant 8′
samples played an octave lower (it comes first in the lists; its parameters are added after the
others, so older projects and patches still load). Default volumes are set so all stops start
about equally loud (measured K-weighted loudness of all pipes, including their default distance).

**Spieltisch** – click the stop knobs (photos) to pull/push stops. Under every stop:
*Lautst.* (volume, −48…+12 dB) and *Wind* – how far the stop is drawn: below 100 % the
pipes get quieter, flatter, unsteady and duller, at 0 % they are silent (move it while
holding chords). *Traktur* is the noise layer (key click, how much of the click goes into
the pipe's attack position, release click, stop-pull sound, *Wind (global)*: blower noise,
and above ~35 % an increasingly unstable wind supply that shakes the pitch and level of
all pipes together – wild at 100 %). *Verfall* is the degradation: amount, grain,
bit depth, sample rate, unstable wind (pitch/level wobble), dropouts – applied to
each pipe before it is placed in space ("pro Pfeife") or to the whole output
("Summe"); *Windschwankung* also gets disproportionately wilder towards 100 %. The
keyboard strip can be clicked to play.

**Raum** – every pipe of every stop as a dot (top view: centre = zenith, edge = horizon,
outside the circle = below the horizon). Select a stop on the right, then
- drag a dot to move one pipe,
- drag on empty space to turn the whole stop around you,
- *Anordnung*: layout generators (organ case C/C♯ side, pitch, scatter, ring,
  spiral, zone) with width / centre / height / spread / *Abstand* (distance, 1–40 m;
  bigger dots = nearer),
- *Bewegung*: Statisch, Rotation (horizontal), Pendel, Drift, Vertikal (turns over your
  head: front → up → back → down), Sprünge (every pipe suddenly jumps to a new random place
  around its position, each at its own moment; *Tiefe* = how far). *Geschwindigkeit* runs
  from −10 to +10 Hz: negative = the other direction. Changing the speed never resets the
  position, so you can do smooth speed glissandi (also by automation). *Ellipse* squashes
  the path so pipes come closer in front and move away behind (gives Doppler, see Klang).
- *Hall*: load an impulse response (*IR laden …*: WAV/AIFF/FLAC, up to 10 s). 4/9/16/25/36
  channels are read as AmbiX (ACN/SN3D); switch on *N3D* for N3D-normalised files, *FuMa* for
  first-order B-format in FuMa order (W X Y Z). 2 channels = stereo, 1 = mono. Files with other
  channel counts (e.g. 32 raw Eigenmike capsules) have to be encoded to Ambisonics first.
  - *folgt Pfeifen*: the IR is turned so that its direct sound and early reflections (first
    ~80 ms) come from each pipe's own direction – every pipe gets "its" room. Off = the room
    exactly as measured (early reflections from where the measurement loudspeaker stood).
  - *Umhüllung*: blends the late tail with an even, decorrelated field from all sides
    (energy per order like a real diffuse field). This also turns mono / stereo / first-order
    IRs into an enveloping room up to the *Ord.* chosen.
  - *Direktschall der IR*: how much of the IR's own direct sound is kept (the dry pipes already
    are the direct sound; default 20 %).
  - *Ord.*: highest Ambisonics order of the room (default 3; the pipes stay at full order).
    Higher orders cost more CPU: a 5th-order room with a long IR is heavy.
  - *Direkt* and *Hall* set the dry and room levels, *x* removes the IR. The IR is resampled to
    the project rate and normalised automatically; changing these settings or the output format
    rebuilds the room in the background (~½ s).

**Klang**
- *Stimmung*: temperament (¼-comma meantone = the original recording, ⅙-comma,
  Werckmeister III, Kirnberger III, Vallotti, Pythagorean, equal). a′ stays put, the other
  pitch classes are shifted. *Stimmton* moves everything (±200 ct, shown as a′ in Hz).
  *Verstimmung* gives every pipe its own fixed random detune (0–60 ct).
- *Tremulant*: on/off, speed, depth – pitch + level wave shared by all pipes, plus a bit
  of wind noise.
- *Freeze*: while on, released notes do not stop but turn into a grain cloud from the
  pipe's sustain. *Verwischen* = longer, more detuned grains. Switching it off lets the
  frozen notes fade over *Ausklang*.
- *Entfernung · Doppler*: *Doppler* (pitch shift when a pipe's distance changes – with
  Ellipse or Drift; Sprünge never glide), *Entfernung → Lautstärke* (far pipes quieter),
  *Luft* (far pipes duller). Far pipes also send more into the Hall.
- *MIDI*: *MIDI-CC an* switches the CC mapping below; *Bewegung im Tempo* locks all
  motion to the host tempo and song position (speeds count as written at 120 BPM; for
  free speed glissandi switch it off).

**Modulation**
- *LFO 1–4*: shape (sine, triangle, saw, square, random steps, smooth random), *Tempo*
  0.01–20 Hz or *Sync* to the project tempo (note values), *Versatz je Pfeife* (spread):
  0 % = all pipes move together, 100 % = one full cycle spread across the pipes – along the
  keyboard (a wave running from low to high) or randomly. The dots in the display show
  where each key's pipe is on the wave. Spread only acts on the *Pfeife:* targets.
- *Hüllkurve 1–2* (envelopes): delay, attack, decay, sustain, release. *Pro Pfeife* = every
  note runs its own envelope; *Global* = one envelope, every key restarts it and it
  releases when all keys are up. Global targets (any parameter) always follow the global
  envelope.
- Further sources: *Tastenlage* (low key −1 … high key +1), *Modrad* (CC 1), *Zufall je Ton*
  (a new random value per key press).
- *Zuordnungen* (24 slots, scroll the list): source → target, amount ±100 % (click the
  amount to type it), and **± / +**: ± = bipolar (an LFO swings around the slider value),
  + = unipolar (it only pushes one way, 0 … amount; envelopes and the mod wheel are 0 … 1 anyway).
  Targets:
  - *Pfeife: Richtung / Höhe / Abstand / Lautstärke / Tonhöhe / Helligkeit / Hall-Anteil* –
    act on every pipe separately (at 100 %: ±180°, ±90°, ×8 distance, ±36 dB, ±12 semitones,
    ±6 octaves of brightness, ±24 dB room send);
  - every continuous parameter of the plugin (stop volume, motion speed, wind, Verfall,
    Doppler, Hall, even the LFO speeds themselves …), amount = share of the slider's range.
- **At the slider itself:** a modulated slider shows each routing's range as a red bar with a
  small red lozenge at the full-amount end – drag the lozenge to set the amount right there
  (Shift = 5 % steps); the white tick shows where the modulation is right now.
- **Right-click any slider** → *Modulieren mit …* picks a free slot directly (or removes an
  existing one). A modulated slider shows a white bar and arrow where the modulation takes it
  right now. In the Raum view the pipes move with *Pfeife: Richtung / Höhe / Abstand*
  modulation (silent pipes show the LFO part; sounding ones also their envelopes).
- Everything is updated every 64 samples (≈1.3 ms), so even fast LFOs stay smooth. Amounts
  are automatable parameters; sources and targets of the slots are saved with the project.

Pipes the preprocessing flagged as unusable (Gedackt f and f♯ in the current
samples) are replaced by the neighbouring pipe, pitch-shifted to the exact
meantone pitch. Noise samples you delete from `samples\noise` are simply skipped.

### Patches

The bar at the top left: ◀ / ▶ step through the patches, the name opens the list,
*Speichern* saves (same name = replace). A patch holds everything that makes the sound:
stops, all sliders, pipe layouts and positions, muted pipes, modulation routings and the
loaded impulse response (by file path). It does **not** change the output format
(stereo / AmbiX order belongs to the track) or the samples folder.

Patches are plain files in `Documents\Rysum\Patches\*.rysum` – copy, rename, back up or
share them. On first start six example patches are written there (Init, Plenum, Welle durch
die Pfeifen, Atmende Wolke, Verfall, Gläserne Sprünge); deleted ones are not re-created.
The project in Reaper still remembers everything on its own, patches are extra.

## 4. MIDI

MIDI note = sounding pitch (C2 = 36 … a″ = 81). Velocity shapes the key clicks: a light touch
gives a quieter and duller click, also when the key comes up. C♯2, D♯2, F♯2, G♯2 and g♯″ do not
exist on the organ and stay silent (short octave). Sustain pedal (CC64) works.

With *MIDI-CC an* (Klang page):

| CC | function |
|----|----------|
| 11 | Wind (global) / swell |
| 20–26 | stops on/off (Praestant, Gedackt, Octave 4′, Octave 2′, Sesquialtera, Mixtur, Trompete) |
| 19 | Praestant 16′ on/off |
| 27–33 | stop wind (same order) |
| 34 | Praestant 16′ wind |
| 66 | Freeze (sostenuto pedal) |
| 1 | Tremulant depth (mod wheel) |
| 64 | Sustain |

All stops and settings are plugin parameters, so they can be automated or
MIDI-learned in Reaper.

## 5. Files

```
plugin/
  CMakeLists.txt        build description
  Source/               C++ code
    OrganEngine.*       voices, loops, releases, noise layer, encoder, degradation
    SampleLibrary.*     reads rysum_samples.json + WAVs, replaces unusable pipes
    Spatial.h           AmbiX encoder (ACN/SN3D), stereo panning, layouts, motion
    Degrade.h           "Verfall"
    PluginProcessor.*   parameters, state, sample loading
    Convolver.*         impulse-response reverb (stereo / AmbiX)
    Modulation.h        LFOs, envelopes, routing definitions
    PluginEditor.*, ConsoleView.*, SpaceView.*, KlangView.*, ModView.*, Theme.h   interface
  Resources/            photos, fonts (OFL), grain texture
  Tests/                offline render test + UI snapshot tool (optional)
```

Optional tests (not needed for normal use):
`cmake -B build -DRYSUM_BUILD_TESTS=ON` builds `RysumRenderTest` (renders MIDI scenes
to WAV and checks the encoder maths) and `RysumUiSnapshot`.

Fonts: "Rysum Fell" = IM FELL DW Pica by Igino Marini with a few symbols (arrows, primes, ♯ …)
added from DejaVu Serif and renamed as the OFL requires (built by `tools/make_fell.py`);
UnifrakturMaguntia for the title; IBM Plex (no longer used in the interface). All SIL Open
Font License (see `Resources/OFL_*.txt`).
