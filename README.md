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
(the output of `scripts/rysum_preprocess.py`, run from the repository root). If they are somewhere else, click
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
and binaural mode only channels 1/2 carry sound.

### Binaural (headphones) – also in Ableton and the standalone app
Output "Binaural (Kopfhörer)". The plugin renders Ambisonics internally and decodes it to
two ears with head-related transfer functions, so a normal stereo track is enough (Ableton,
Reaper stereo track, the standalone app – which starts in this mode). Settings are on the
**Klang** page (*Kopfhörer*):
- *Ordnung* 1–5 (default 3): higher = sharper directions, more CPU.
- *SOFA laden …*: your own HRTF set in the SOFA format (convention SimpleFreeFieldHRIR, e.g.
  an individually measured one or one from the SOFA databases); *FABIAN* goes back to the
  built-in set. The file path is saved with the project (not in patches).
- *Kopfdrehung*: turns the whole scene as if you turned your head (also a modulation target –
  an LFO on it is a quick way to check what is where).
- *Diffusfeld-Entzerrung* (on): removes the average colouration of the HRTF set, for
  diffuse-field-tuned headphones. Off = the dummy head as measured.

How it works – the same approach as the IEM *BinauralDecoder*: from the HRTF set the plugin
computes a magnitude-least-squares (MagLS) decoder for the chosen order (Schörkhuber,
Zaunschirm & Höldrich 2018): below ~620 Hz × order the HRTFs are fitted exactly (correct
interaural time differences), above only their magnitude (no dull treble at low orders), then
the diffuse-field energy is matched. This takes about half a second in the background whenever
the order, the HRTF or the sample rate changes. The built-in HRTF is the **FABIAN** head and
torso simulator of the TU Berlin (2000 of the 11 950 measured directions). Latency of the
binaural decoder: 128 samples.

## 3. The six pages

**Top bar, next to the patches:**
- *Tastatur*: play with the computer keyboard. `A W S E D F T G Z H U J K O L P` = C up to
  d♯ of the next octave (German layout: Z is the black key between G and H), `Y` / `X` = octave
  down / up (the button shows the octave), `C` / `V` = softer / harder touch (key clicks). In a
  host click into the plugin window first; in Ableton switch off Live's own computer MIDI keyboard
  (the keyboard button at the top right of Live) so the keys reach the plugin.
- *Halten*: strike a key twice quickly and the note stays down, as if you kept holding it – the
  pipes sound normally (no Freeze), so you can pull stops, change the wind and modulate with free
  hands. Strike it once more to let it go; switching *Halten* off lets all held notes go. The
  button shows how many notes are held. Works with MIDI, the computer keyboard and the keyboard
  on the Spieltisch page.

- **Full screen** (the square at the far right of the top bar, or `F11`): the whole interface fills
  the screen the plugin window is on, scaled up as far as the screen allows. `Esc`, `F11` or the button
  again brings it back into the plugin window. While it is full screen, the plugin window just shows
  where it went; clicking it also brings the interface back. This works the same in Reaper, Ableton
  and the standalone app. The interface is 16:9 (1280 x 720), so on a full-HD screen it fills the
  whole screen at 1.5x.
- **Window size**: the first time, the plugin window opens about as large as the screen allows
  (on a 1920 x 1080 screen roughly 1.3x). Drag the corner to change it (half to twice the size); the
  size you leave it at is remembered for every new instance (`Documents/Rysum/Fenster.xml`;
  delete that file to get the automatic size again).

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

**Raum** – every pipe of every stop as a small dot (top view: centre = zenith, edge = horizon,
outside the circle = below the horizon). Select a stop on the right, then
- drag a dot to move one pipe,
- drag on empty space to turn the whole stop around you,
- *Anordnung*: layout generators (organ case C/C♯ side, pitch, scatter, ring,
  spiral, zone) with width / centre / height / spread / *Abstand* (distance, 1–40 m;
  bigger dots = nearer),
- The lower part has two tabs, *Bewegung* and *Hall* (buttons in its title line).
- *Bewegung*: Statisch, Rotation (horizontal), Pendel, Drift, Vertikal (turns over your
  head: front → up → back → down), Sprünge (every pipe suddenly jumps to a new random place
  around its position, each at its own moment; *Tiefe* = how far), Gezeichnet (see below).
  *Geschwindigkeit* runs from −500 to +500 Hz (fine at slow speeds, the knob gets coarser
  towards the top): negative = the other direction. Changing the speed never resets the
  position, so you can do smooth speed glissandi (also by automation). Positions are
  computed every 64 samples, so above ~300 Hz the motion itself starts to alias – a sound of its own.
  - *harte Sprünge* (Sprünge only): no smoothing at all. The place changes at the exact sample,
    so jumps click; from ~50 Hz on it buzzes and rattles.
- *Gezeichnet* – draw your own trajectory: press *✎ Bahn zeichnen*, then draw a line in the top
  view with the mouse held down (centre = overhead, edge = horizon, outside = below). When you
  let go, the stop switches to *Gezeichnet* and its pipes travel along the line like a train
  (the line stays visible in red). *Geschwindigkeit* = passes per second.
  - *Verteilung auf der Bahn*: 0 % = all pipes of the stop together at one point, 100 % = spread
    over the whole path (along the keyboard). It is modulatable.
  - *Schleife*: at the end back to the start (the path closes). *Hin & zurück*: forwards, then
    backwards.
  - *gleichmäßig*: even speed along the path. *Zeichentempo*: as fast as you drew, so fast
    where you drew fast and slow where you drew slowly.
  - The distance stays the pipe's own (*Abstand*, layout). The ellipse below still applies.
  - Paths are saved with the project and in patches; drawing again replaces the path.
- *Ellipse* – the shape of the orbit, for every motion. *Ellipse* = how much nearer the near
  side is (and the far side farther); this gives Doppler (see Klang).
  - *Richtung*: where the near side is (0° = front); modulate it and the ellipse turns.
  - *Form*: *1* = one near side (you are at a focus, as before), *2* = near on two opposite sides
    (you in the centre of the ellipse), *3* / *4* = triangle / square-like orbits.
  - *Neigung*: the orbit tilts – higher towards the near side, lower on the far side (negative:
    the other way).
  - The small picture shows the orbit from above (front up). Defaults are as before, so older
    projects sound the same.
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

**Saal** – a simulated room instead of the impulse response, made to be played. *Saal an*
switches it on (the IR on the Raum page pauses meanwhile and comes back when it is off).
- How it works:
  - *Early reflections*, per sound: every sounding pipe (and every click / own sound) gets its own
    mirror-image sources in the shoebox room, up to the 1st, 2nd or 3rd order (6, 24 or 62
    reflections), each from its own direction, with its own delay, wall and air damping. Pipes
    that move get their reflections moving with them, with their own Doppler shift. The first
    reflections are encoded at up to 3rd order, the later ones at 1st order (cheaper, and they
    are diffuse anyway).
  - *Late reverb*: the way room-acoustics software (e.g. RAVEN) does it. **Ray tracing**
    (4000 rays with specular and diffuse reflections and air absorption) gives the room's decay
    in two bands; from it a dense noise response is synthesised – one independent noise per
    Ambisonics channel with the energy of a diffuse field (up to 2nd order; stereo: two) – and
    convolved with the organ. A noise response has no modes. Ray tracing matters when
    the absorption is uneven or the room is not diffuse: a corridor with hard walls rings far
    longer than Eyring's formula says. The tail's level follows the room's critical distance
    (big, damped rooms are drier, small hard ones wetter).
  - *Nachhall: Faltung / FDN* (buttons in the *Hörer · Pegel* title): the late reverb can also
    be a **feedback delay network**, the earlier model: 16 delay lines around the room's mean
    free path, a random orthogonal mixing matrix, slowly modulated delays, decay per band from
    the same ray tracing, the lines spread over 16 directions. It sounds a little more alive
    and grainy and needs less CPU. Both are set to the same level. When you switch while
    playing, the old tail rings out underneath, so there is no gap. (The ringing heard in
    earlier versions was a precision bug in the delay lines of the early reflections and of
    the FDN, which made both alias after a few minutes of running. It is fixed in both.)
  - When the room changes – also while it is modulated – a new response is built in the
    background (about four times a second while it keeps changing) and crossfaded in. The
    convolution keeps the input history apart from the response, so the reverb that is already
    sounding simply turns into the new room's, without a gap; the noise is the same for every
    rebuild, so held tones do not jump. The tail follows the room ~¼–½ s late, the early
    reflections immediately. Responses are at most 6 s long.
- The plan shows the room from above, the pipes of the pulled stops inside it (pipes outside
  the walls are pushed in), the listener (drag it), the four mirrored rooms and, in red, the
  reflections of the last pipe you played: its mirror images and the paths over the walls.
- *Maße*: width, length, height and *Größe* (scales all three – the easiest knob to modulate).
  Room buttons: Kapelle, Dorfkirche, Kathedrale, Flur (flutter echo), Würfel (a small, almost
  lossless box that rings like an instrument), Halle.
- *Oberflächen*: absorption of the walls and of floor/ceiling, extra damping of the highs,
  *Streuung* (how much is reflected diffusely: less in the reflections, more in the tail).
  Material buttons set them: Stein, Putz, Holz, Glas, Vorhang, Spiegel (nothing is absorbed –
  an impossible room).
- *Hörer · Pegel*: where you stand (also by dragging in the plan), level of the early
  reflections and of the tail.
- Everything is a modulation target ("Saal" in the list): an LFO on *Größe* makes the room
  breathe, envelopes can shrink it while a chord is held, LFOs on the listener let you walk
  through it. Patches 49–55 show some of this.
- CPU: reflections up to 2nd order are the sensible default; 3rd order with many pipes is heavy.
  Up to 48 sounds get their own reflections at a time (all of them still reach the tail).

**Klang**
- *Stimmung*: besides the historical temperaments below there are exotic tunings. Twelve notes
  per octave: just intonation (5-limit on C, septimal 7-limit), the harmonic series on C
  (partials 16–30: the 7th, 11th and 13th partials sound very foreign), 12 of 31-EDO, 12 of 19-EDO,
  12 of 53-EDO (almost just), Maqam Rast (E and B a quarter tone low). Step tunings – every MIDI
  note is one step, so the keyboard becomes microtonal: 31-EDO (38.7 ct), 19-EDO (63.2 ct),
  quarter tones (50 ct), Bohlen–Pierce (13 steps per twelfth, 146.3 ct, no octaves at all) and
  Wendy Carlos's Alpha (78 ct). For those every key takes the recorded pipe closest to its new
  pitch, so the pipes hardly need to be shifted (31-EDO: at most 40 ct). a′ stays where it is.
- **Free tuning – Scala and MTS-ESP** (the last two entries of the list). Here every MIDI note,
  on every MIDI channel and over the whole range 0–127, gets its own pitch. The recorded pipe
  nearest to that pitch plays, shifted by the rest. Its sound comes from that pipe's place.
  Notes that land on the same pipe (31-EDO steps, clusters) each get their own voice. Notes
  beyond the organ's compass are shifted from the outermost pipes, up to two octaves (further
  out they stay silent). Velocity, modulation, Halten and Freeze work as usual.
  - *Scala-Datei (.scl / .kbm)*: *Scala laden …* takes a scale (`.scl`, cents or ratios) and,
    loaded afterwards, a keyboard mapping (`.kbm`: which key plays which degree, unmapped keys,
    reference key and frequency). *KBM entfernen* goes back to the default mapping: degree 0 on
    note 60, note 69 = 444.5 Hz (the organ's a′), so twelve-note scales sit where the organ
    is. The files themselves are saved in the project and in patches, so they also work on
    another computer. Loading a file selects this entry.
  - *MTS-ESP / MIDI-Stimmung (SysEx)*: follows an **MTS-ESP master** (ODDSound; e.g. the free
    *MTS-ESP Mini*, or a master in another plugin), including multi-channel tables (keyboards with
    more than 128 keys, e.g. Lumatone layouts) and keys the master leaves unmapped. Without a
    master it takes **MIDI Tuning Standard SysEx** from the MIDI input: that is what Hexatone
    and similar isomorphic-keyboard tools send. *Töne folgen*: held notes follow a tuning change
    at once (off: each note keeps the pitch it was struck with). Until a master is connected or
    SysEx has arrived, it plays the original tuning. The line below the buttons shows the state.
    MTS-ESP needs its library installed (it comes with every MTS-ESP master); without it the
    plugin works as before.
  - **Hexatone via MIDI loopback** (loopMIDI on Windows, IAC on macOS): in the **standalone
    app** choose the loopback port as MIDI input (*Options → Audio/MIDI Settings*), select
    *MTS-ESP / MIDI-Stimmung*, and let Hexatone send MTS SysEx on that port. In **Reaper** the
    track's MIDI input passes SysEx on. **Ableton Live does not pass SysEx to plugins.** There,
    use an MTS-ESP master (e.g. MTS-ESP Mini with the same scale) and send Hexatone only notes.
- Historical temperaments (¼-comma meantone = the original recording, ⅙-comma,
  Werckmeister III, Kirnberger III, Vallotti, Pythagorean, equal). a′ stays put, the other
  pitch classes are shifted. *Stimmton* moves everything (±200 ct, shown as a′ in Hz).
  *Verstimmung* gives every pipe its own fixed random detune (0–60 ct).
- *Tremulant*: on/off, speed, depth – pitch + level wave shared by all pipes, plus a bit
  of wind noise.
- *Registerwind* – the wind model of the stops, i.e. what the *Wind* slider of each stop
  (Spieltisch) does when it is turned down:
  - *klassisch* (default, as before): a half-drawn stop gets quieter, flatter, duller and unsteady.
  - *instabil*: the same, and below roughly half wind each pipe also gets an edge tone. That is
    the slow jet oscillating at the upper lip: a thin whistle (roughly 1.5–8 kHz) next to the pipe tone.
    Like on a real organ it is
    - steady as long as the wind is steady;
    - lower and quieter as the wind falls (its pitch follows the jet speed, about the square root of
      the pressure);
    - pulled towards the pipe's own resonances, so on some pipes it moves in small hops instead of
      gliding (a stopped Gedackt only has the odd ones);
    - different from pipe to pipe (where it appears, how high, some pipes have a second, weaker
      one), but always the same for the same pipe.
    The mouth also hisses, and at very low wind (below ~15 %) the speech sputters. The Trompete
    chatters instead. At full wind nothing changes, so patches sound the same in both models.
    Try a slow LFO or an envelope on a stop's wind, or a MIDI fader (see section 4). Patches 56–58 show it.
- *Freeze*: while on, released notes do not stop but turn into a grain cloud from the
  pipe's sustain. *Verwischen* = longer, more detuned grains. Switching it off lets the
  frozen notes fade over *Ausklang*.
- *Entfernung · Doppler*: *Doppler* (pitch shift when a pipe's distance changes – with
  Ellipse or Drift; Sprünge never glide), *Entfernung → Lautstärke* (far pipes quieter),
  *Luft* (far pipes duller). Far pipes also send more into the Hall.
- *MIDI*: *MIDI-CC an* switches the fixed CC mapping (section 4); *Eigene Zuordnungen …*
  opens your own controller assignments (MIDI learn); *Bewegung im Tempo* locks all
  motion to the host tempo and song position (speeds count as written at 120 BPM; for
  free speed glissandi switch it off).
- *Kopfhörer (binaural)*: *Binaural an* switches the output to binaural (and back to
  stereo), order, own SOFA file, head rotation, diffuse-field EQ – see section 2.

**Effekte** – sounds that are easy to locate (short, bright, full of transients) and that
follow the pipes. Everything is off in older patches.
- *Eigene Geräusche*: your own recordings, played by the keys. Put WAV/AIFF/FLAC files
  (up to 20 s, up to 64 files) into `samples\noise\eigene` – *Ordner öffnen* creates the folder
  and opens it, *Neu einlesen* reads it again without reloading the pipes. Each file is trimmed
  to its first transient and set to the same peak level. With an empty folder the recorded key
  clicks are used. *Ort*: at a pipe of a pulled stop, anywhere in the room, at the
  console, or circling. *Beim Anschlag / Loslassen* = probability per key, *Tonhöhe folgt der
  Taste* = pitch follows the key (100 % = semitones), *Streuung* adds delayed copies at other
  times and places, *Wandern* lets each sound travel around you (°/s). Velocity sets level and
  brightness, like the key clicks.
- *Trakturweg*: small ticks travel from the console to the pipes of the pulled stops (the
  trackers), *Laufzeit* = travel time, *Schritte* = number of ticks, *Streuung* = how far the path
  bends. *Pfeifen warten*: the pipe only speaks when the path arrives.
- *Knistern*: short crackles at every sounding pipe – they move with the pipe, so the ear finds
  it. *Dichte* per pipe (Hz), *Länge* of a crackle (ms).
- *Windlecks*: up to eight hissing leaks at fixed places (they can wander). *Gebläse ringsum*:
  the blower noise (*Wind* on the Spieltisch) comes from all sides instead of one signal.
- *Obertöne verteilen*: above *Trennung* (× the pipe's fundamental) each pipe's sound is moved
  sideways (randomly left or right per pipe) – the fundamental stays at the pipe. *Kreisen*
  turns the upper part around you.
- *Wandernde Echos*: up to eight repeats of the whole organ, each from the next direction
  (*Drehung*, *Neigung* per repeat), quieter by *Abnahme* and darker by *Dämpfung*. *Im Tempo*
  uses a note value instead of *Zeit*. The little circle shows where the repeats come from
  (top view, front up). Echoes also go into the Hall from their own direction.

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
  (a new random value per key press), *Anschlag* (key velocity 0 … 1, per note – each pipe keeps
  the velocity its key was struck with; global targets follow the last note), *Tonhöhenrad*
  (pitch wheel −1 … +1; it bends nothing by itself – route it to *Pfeife: Tonhöhe*, e.g. 17 %
  = ±2 semitones, or anywhere else).
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
- **MIDI-Regler** (the second tab of *Zuordnungen*): knobs, faders and buttons of your MIDI
  keyboard on any control of the plugin – see section 4.

Pipes the preprocessing flagged as unusable (Gedackt f and f♯ in the current
samples) are replaced by the neighbouring pipe, pitch-shifted to the exact
meantone pitch. Noise samples you delete from `samples\noise` are simply skipped. Your own sounds live in
`samples\noise\eigene` (see Effekte); the preprocessing never touches that folder.

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

With a free tuning (Scala or MTS-ESP, Klang page) every MIDI note 0–127 on every channel can sound,
each at the pitch the tuning gives it (see *Stimmung*). MIDI Tuning Standard SysEx is read there.
The pitch wheel and velocity are also modulation sources (*Tonhöhenrad*, *Anschlag*).

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

### Your own knobs and faders (MIDI learn)

The knobs, faders and pads of a MIDI keyboard send *control change* messages (CC): a
controller number (0–119) and a value 0–127, on a MIDI channel. A host like Ableton maps
them to plugin parameters itself (MIDI Map mode). Rysum can also do this on its own, and that
works the same way in Ableton, Reaper and the standalone app:

- **Right-click any slider → *MIDI lernen***, then move the knob. The slider shows
  *MIDI: Regler bewegen …* while it waits, and afterwards a small *CC 21* tag next to its value.
  The same menu removes the assignment.
- **Modulation page → Zuordnungen → *MIDI-Regler*** shows all 32 assignments:
  - *lernen*: click it, move a controller (click again to cancel). It shows the CC number and
    channel (*K2*) it learned. Only that channel counts.
  - *Ziel*: any parameter, including switches like stops, Freeze and Tremulant.
    Picking a target in an empty row starts learning right away.
  - Range (the bar with two lozenges): the controller sweeps from the left lozenge (knob down)
    to the right one (knob up). Swap them to reverse the direction. Double-click = full range,
    Shift = 5 % steps. The values are shown at both ends.
  - *Taster*: for buttons and pads. Every press switches between the two ends of the range.
    It is on by default for switches.
  - ×: remove.
  - Top right shows the last controller that came in (number, channel, value). Use it to find
    out what a knob sends.
- A learned CC takes precedence over the fixed mapping above. For example, if you learn CC 11
  onto something else, it no longer moves the global wind. The fixed mapping can be switched off
  completely (*MIDI-CC an*). Learned assignments always work.
- The assignments belong to your setup, not to the sound. They are saved in the project / session
  and in `Documents\Rysum\MIDI-Regler.xml`, and every new instance starts with the last ones
  used. Patches do not change them.
- The parameter moves exactly as if you turned it with the mouse. When Ableton or Reaper record
  automation, the knob movement is recorded too.

## 5. Files

The maintained application is in `plugin/`. Repository-wide utilities are in
`scripts/`; sample preparation and analysis are documented in `scripts/README.md`.
`docs/` contains design and analysis reports. Historical source copies, extracted
notes and original photos are retained in `_archive/` and are not built.

All paths in the following tree are relative to the repository root.

```
plugin/
  CMakeLists.txt        build description
  source/               C++ code
    OrganEngine.*       voices, loops, releases, noise layer, encoder, degradation
    SampleLibrary.*     reads rysum_samples.json + WAVs, replaces unusable pipes
    Spatial.h           AmbiX encoder (ACN/SN3D), stereo panning, layouts, motion
    Degrade.h           "Verfall"
    PluginProcessor.*   parameters, state, sample loading
    Convolver.*         impulse-response reverb (stereo / AmbiX)
    Binaural.*          HRTF sets (built-in FABIAN, SOFA via libmysofa), MagLS design, binaural decoder
    SofaGunzip.cpp      zlib inflate for libmysofa (JUCE's zlib)
    Modulation.h        LFOs, envelopes, routing definitions
    RoomSim.h           simulated room: image sources, Eyring, ray tracer
    RoomTail.*          late reverb of the simulated room: synthesised responses, partitioned convolution
    Scala.h             Scala files (.scl scales, .kbm keyboard mappings)
    WindChaos.h         wind model "instabil": resonances, intermittent speech, hiss of starving pipes
    PluginEditor.*, ConsoleView.*, SpaceView.*, SaalView.*, KlangView.*, EffectsView.*, ModView.*,
    MidiMapView.*, Theme.h   interface
  resources/            photos, fonts (OFL), grain texture, fabian_hrir.bin (FABIAN HRIRs)
  third-party/libmysofa/ SOFA reader (BSD-3, see its LICENSE and README_RYSUM.txt)
  third-party/MTS-ESP/   MTS-ESP client by ODDSound (0BSD, see its LICENSE and README_RYSUM.txt)
  tests/                offline render test + UI snapshot tool (optional)
```

Optional tests (not needed for normal use):
`cmake -B build -DRYSUM_BUILD_TESTS=ON` builds `RysumRenderTest` (renders MIDI scenes
to WAV and checks the encoder maths) and `RysumUiSnapshot`.

Fonts: "Rysum Fell" = IM FELL DW Pica by Igino Marini with a few symbols (arrows, primes, ♯ …)
added from DejaVu Serif and renamed as the OFL requires (built by `scripts/make_fell.py`);
UnifrakturMaguntia for the title; IBM Plex (no longer used in the interface). All SIL Open
Font License (see `plugin/resources/OFL_*.txt`).

HRTF: FABIAN head-related transfer function database, Audio Communication Group, TU Berlin –
F. Brinkmann et al., "A High Resolution and Full-Spherical Head-Related Transfer Function
Database for Different Head-Above-Torso Orientations", JAES 65(10), 2017, data DOI
10.14279/depositonce-5718, licence CC BY 4.0 (see `plugin/resources/FABIAN_LICENSE.txt`).
SOFA files are read with libmysofa by Christian Hoene / Symonics (BSD 3-clause).
Microtuning via the MTS-ESP client library by ODDSound Ltd. (0BSD).
