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

## 3. The three pages

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
- *Hall*: load an impulse response (WAV/AIFF/FLAC, up to 10 s). 4/9/16/25/36 channels are
  read as AmbiX (ACN/SN3D); for a first-order B-format recording in FuMa order (W X Y Z,
  e.g. from some older tools) switch on *FuMa*. 2 channels = stereo, 1 = mono. The omni part
  of the organ is fed into the IR; *Direkt* and *Hall* set the dry and room levels, *x*
  removes the IR. The IR is resampled to the project rate and normalised automatically.

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

Pipes the preprocessing flagged as unusable (Gedackt f and f♯ in the current
samples) are replaced by the neighbouring pipe, pitch-shifted to the exact
meantone pitch. Noise samples you delete from `samples\noise` are simply skipped.

## 4. MIDI

MIDI note = sounding pitch (C2 = 36 … a″ = 81). C♯2, D♯2, F♯2, G♯2 and g♯″ do not
exist on the organ and stay silent (short octave). Sustain pedal (CC64) works.

With *MIDI-CC an* (Klang page):

| CC | function |
|----|----------|
| 11 | Wind (global) / swell |
| 20–26 | stops on/off (Praestant, Gedackt, Octave 4′, Octave 2′, Sesquialtera, Mixtur, Trompete) |
| 27–33 | stop wind (same order) |
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
    PluginEditor.*, ConsoleView.*, SpaceView.*, KlangView.*, Theme.h   interface
  Resources/            photos, fonts (OFL), grain texture
  Tests/                offline render test + UI snapshot tool (optional)
```

Optional tests (not needed for normal use):
`cmake -B build -DRYSUM_BUILD_TESTS=ON` builds `RysumRenderTest` (renders MIDI scenes
to WAV and checks the encoder maths) and `RysumUiSnapshot`.

Fonts: IBM Plex Sans Condensed / IBM Plex Mono and UnifrakturMaguntia, SIL Open Font
License (see `Resources/OFL_*.txt`).
