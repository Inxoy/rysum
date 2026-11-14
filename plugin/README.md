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
| effect version (for audio tracks) | `build\RysumFX_artefacts\Release\VST3\Rysum FX.vst3` |

The effect version **Rysum FX** is the same plugin as an audio effect (see *Eingang* below).
To build only the instrument (faster), configure once with `-DRYSUM_BUILD_FX=OFF`.

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

The plugin finds the samples by itself in `D:\RYSUM_plugin\rysum_plugin\rysum-samples`
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

### Two loudspeakers (transaural) – the binaural image over a pair of speakers

Output *Transaural (2 LS)*, or *Transaural an* on the Klang page (box *Zwei Lautsprecher*, below
Freeze). It is the binaural mode – same order, HRTF (FABIAN or an own SOFA file), Kopfdrehung,
Diffusfeld-Entzerrung – followed by a crosstalk cancellation: each speaker also reaches the far
ear, a little later (the ITD) and quieter (the head shadow); two filters (on (L+R)/2 and (L−R)/2)
cancel that path, so each ear gets (nearly) only its own binaural channel.
- *Lautsprecher bei* ±10…150° (default ±90°: the two speakers on opposite sides of the room,
  facing each other, you in the middle between them – a good layout for this, because the head
  already shades each ear from the far speaker);
- *Übersprechen aus* 0–100 %: 0 % = the binaural signal plainly over the speakers, 100 % = full
  cancellation (more spatial, more sensitive to where you sit);
- *höchste Anhebung* 3–20 dB: cancellation needs a bass lift in the side signal (the head is
  small against long waves); this limits it (regularised inverse). Below ~80 Hz and above ~12 kHz
  the filters fade to plain;
- *Seitenklang ausgleichen*: also takes out the colour of the speakers' own direction (the
  near-ear HRTF at the angle, 1/3 octave smoothed, at most ±10 dB).

The filters come from the HRTF in use at the set angle: the measured far-ear/near-ear ratio with
the ITD taken out, smoothed over 1/12 octave, regularised; designed in the background (a few
hundred ms) and crossfaded in. Extra delay: 5.3 ms. Against FABIAN itself the far ear drops from
−13 dB to −28 dB at ±90° (250 Hz–6 kHz; −13 → −26 dB at ±60°, −7 → −20 dB at ±30°) – with a real
head and a real room expect less, and the effect is best on the line between the speakers,
equally far from both.

### Loudspeakers (AllRAD) – a speaker array straight from the plugin or the standalone app
Output "Lautsprecher". The plugin decodes its Ambisonics to your loudspeaker layout, the
approach of the IEM *AllRADecoder* (Zotter & Frank 2012): a dense virtual sphere (5000
directions) is decoded with max-rE weighting and every virtual loudspeaker is panned to the real
ones with VBAP (triangulated convex hull). Where there are no loudspeakers (below a dome, behind
a frontal setup) imaginary loudspeakers fill the gap; their signal is dropped. The decoder output
is normalised to the same loudness as stereo / binaural.

Set it up on the page **Lautsprecher** – click the channel display next to the output menu
(top right), or *Lautsprecher (AllRAD) …* on the Klang page:
- *Vorlage*: stereo, quad, 5.1, 7.1.4, octagon, cube, octagon + 4, a 19-speaker dome;
  *IEM-Datei laden / Speichern*: layouts in the JSON format of the IEM Plug-in Suite (files
  from the AllRADecoder work directly, and the other way round).
- The list: output channel, azimuth (0° = front, +90° = left), elevation, distance, gain,
  imaginary – double-click a value to type a new one. *+ Lautsprecher*, *Entfernen*. In the map
  loudspeakers can be dragged.
- *Ordnung* 1–5, *Abstand ausgleichen* (closer loudspeakers quieter and delayed, as if all stood
  as far away as the farthest).
- *Test* puts noise (about −20 dBFS) on the selected loudspeaker's channel; *Rundgang* moves
  it to the next one every second – to check the wiring.
- The map shows the triangulation and the decoder's energy (red = quieter, white = louder than
  the mean); below it the energy spread and the localisation (rE) error.

The layout belongs to the project (not to patches) and the last one is remembered for new
instances (`Documents\Rysum\Lautsprecher.json`). In Reaper give the track as many channels as
the highest loudspeaker channel.

### ASIO (standalone app, Windows)
The standalone app is built with ASIO (JUCE 8 brings Steinberg's ASIO headers): *Options →
Audio/MIDI settings → Audio device type: ASIO*, choose the driver and the output channels –
with *Lautsprecher* this plays a speaker array directly. (Build without it: configure once with
`-DRYSUM_ASIO=OFF`.)

## 3. The pages

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
- *Zufall* (dice): random settings. *Sanft* takes small steps away from the current sound,
  *Wild* sets everything anew – but playable: at least one stop pulled, stop volumes between
  −12 and +6 dB, stop wind at least 30 %, slow movement, effects either off or moderate.
  Below in the menu: which areas are diced (Register, Raum, Saal, Klang, Traktur und Verfall,
  Effekte, Modulation, Lucier – Saal and Lucier are off at first). Never touched: master volume,
  output, tuning, MIDI, Freeze, the pipe layouts. *Zurück zum Klang davor* undoes the last dice.
- **The organ itself**: click the *Rysum* in the top left. A page about the organ in Rysum –
  its history, the stops and where their pipes come from, photos. Click *Rysum* again to go back.

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
= 50 %, `300 ms` or `1.5 s`, `440` on the Stimmton = A4 in Hz, `22` on Abtastrate = 22 kHz),
Return applies. Double-click the slider = default. Right-click = modulation.

**Stops:** eight – the seven recorded ones plus a *Praestant 16′*, made from the Praestant 8′
samples played an octave lower (it comes first in the lists; its parameters are added after the
others, so older projects and patches still load). Default volumes are set so all stops start
about equally loud (measured K-weighted loudness of all pipes, including their default distance).

**Scenes** (Spieltisch, under *Ausgang*): four snapshots *A–D* of the whole sound – every
slider and switch except master volume, output, tuning and MIDI (not the pipe layouts, drawn
paths and routings: those stay). Click an empty letter = store the current sound there (and go
there); click a stored one = go there; right-click = store again / clear. *Überblenden* blends
between the stored scenes (empty ones are skipped): sliders glide, switches change half way.
It is a normal parameter – automate it, learn a MIDI fader for it, or let an LFO wander through
the scenes. Only moving it blends: between two moves you can change the sound freely and store
it. Scenes are saved in the project and in patches.

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

*Verfall · Raum* (tab *Raum* in the Verfall title; everything off by default): the same idea for
the spatial image – the Ambisonics sphere wears away. It works on the whole output after the
impulse response / Saal, before the binaural or loudspeaker decoder, so it needs an Ambisonics
output (AmbiX, binaural, loudspeakers); in stereo only the direction grid is heard.
- *Ordnung abtragen*: the higher orders fade out from the top, continuously (50 % of 3rd order =
  1st order full, 2nd at half, 3rd gone); 100 % = only W, everything from everywhere.
- *Verwittern*: the sphere wears away by itself while something sounds – after this time it is
  completely eroded; in silence it grows back four times as fast (the title shows how far).
- *Richtungsraster*: every direction snaps to the nearest of a few grid points (64 down to 4,
  evenly over the sphere) – moving pipes jump from point to point. Also in stereo.
- *Raum-Bittiefe* / *Raum-Abtastrate*: bit crushing and sample-and-hold of the directional
  channels; the higher the order, the coarser (W stays clean) – fine directions decay first.
- *Raum-Aussetzer*: single channels or whole orders fall out for 50–400 ms (5 ms ramps) – the
  sound tips to one side or goes flat for a moment.
- *Raum-Aliasing*: the field is sampled at too few points and encoded again (fewer points the
  higher the amount): ghost directions, slowly wandering as the sampling grid turns.
All of them are modulation targets (group "Verfall") and part of the dice group Traktur und Verfall.

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
  around its position, each at its own moment; *Tiefe* = how far), Gezeichnet (see below),
  Schwebung, and four motions in the Saal: Flug, Fall, Schwarm, Strömung (see *Im Saal* below).
  *Geschwindigkeit* runs from −500 to +500 Hz (fine at slow speeds, the knob gets coarser
  towards the top): negative = the other direction. Changing the speed never resets the
  position, so you can do smooth speed glissandi (also by automation). Positions are
  computed every 64 samples, so above ~300 Hz the motion itself starts to alias – a sound of its own.
  - *harte Sprünge* (Sprünge only): no smoothing at all. The place changes at the exact sample,
    so jumps click; from ~50 Hz on it buzzes and rattles.
- **Im Saal** – four motions that leave the sphere around the listener: every pipe becomes a body
  with a place and a speed in the Saal (its floor plan, floor and ceiling; the Saal of the stop's
  room, also when *Saal an* is off), and is heard, reflected and hidden behind corners from where
  it really is (Doppler included). *Geschwindigkeit*: 1 Hz = 10 m/s (at most 40 m/s; with *Sync*
  faster at a faster tempo, not locked to the bar). The pipes start where they were.
  - *Flug*: straight lines, bouncing off the walls. *Tiefe* = how roughly the walls throw them off.
  - *Fall*: gravity. The pipes fall, bounce on the floor with their whole length (a long one
    stands tall), slide to a halt and are thrown up again after a moment (*Geschwindigkeit* = how
    hard). *Tiefe* = how bouncy.
  - *Schwarm*: the pipes of the stop flock – together, in one direction, never too close.
    *Tiefe* = how much each one wants its own way.
  - *Strömung*: a slow current through the room carries them. *Tiefe* = turbulence.
  - The motion runs in fixed steps (1/480 s): the same paths and impacts at any buffer size.
  - Where a pipe hits a wall, the floor or the ceiling it knocks – *Aufprall* on the Effekte page,
    tab *Lebendige Orgel*: *Lautstärke* (off by default), *von* klingenden / allen Pfeifen,
    *Nachklang des Rohrs*. The faster the hit, the louder and the brighter (a shorter contact
    excites the higher modes of the tube); an absorbing wall, floor or curtain makes it quieter
    and duller (the Saal's absorption and high damping). The struck pipe rings with the bending
    modes of its tube (long pipes deep, short ones high) and faintly its own note; the wall adds a
    thump. Each knock comes from where it happened and goes into the room. In the 3D view a
    knock flashes.
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

**Wolke** (Raum page, third tab next to *Bewegung* / *Hall*, per stop, off by default): every
sounding pipe of the stop scatters grains of its own sound over the sphere around it.
*Menge* dissolves the pipe into the cloud (equal power: 100 % = only grains), *Dichte* grains per
second and pipe, *Streuung* how far around the pipe (a few degrees = a shimmering spot, 180° =
the whole sphere), *Korngröße* (Hann-windowed, ±30 %), *Zeitstreuung* how far back in the pipe's
sound a grain reaches (up to 150 ms – smears attacks and movement). The cloud follows the pipe's
motion. The grains are not sent to the room separately (the pipe's room send stays as it is).
*für alle Register übernehmen* copies the five values to every stop. Up to 64 pipes at once
have a cloud (256 grains in all); more pipes stay whole.

**Lucier** (Saal page, middle column) – after Alvin Lucier's *I am sitting in a room* (1969):
a recording is played into a room and recorded again, over and over, until only the room's
resonances are left. Two ways (buttons under *Lucier an*):

**Stück (Aufnahme)** – the piece as it is performed. *Aufnehmen*, speak / sing / play (up to
5 minutes), *Aufnahme fertig*: the recording is played back, then generation 1 – the recording
played into the room and recorded again – then generation 2, 3 … each one as long as the recording,
all by itself, until *Generationen* is reached (1 minute × 32 generations ≈ 33 minutes).
- Every generation is a real pass: the whole previous generation convolved with the room
  (undamped, the full reverberation, with loudspeaker and microphone), brought back to the level
  of the recording like Lucier re-recording at full level. The next one is computed in the
  background while the current one plays (about a second per minute of recording).
- *Aufnahme von*: everything Rysum plays (organ + input) or only the audio input.
- *Pause zwischen den Generationen*, *Anteil* (live sound vs. the playback while the piece runs).
- *Abspielen* starts the piece again from the recording, ■ stops. Every recording is saved in
  `Documents\Rysum\Lucier` and the project remembers it; *Aufnahme laden …* takes any audio file
  (e.g. a recording of your voice). *Stück als WAV …* computes the whole piece into one file in
  the background, faster than real time.
- The playback comes out of the middle (W in Ambisonics); the display shows the room's response
  for the generation playing and how long the piece still runs.

**Live** – everything that sounds goes through the room *n* times at once (*Generation*): every
pass multiplies the spectrum by the room's response once more, so its strongest resonances rise
above everything else; it rings out when you stop. (Factory patches 59–63 use this.)
- *Raum*: the simulated hall (its eigenmodes, computed from the dimensions up to about twice the
  Schroeder frequency, with a diffuse field above) or the impulse response loaded on the Raum page –
  e.g. a recording from the church in Rysum itself. Loudspeaker and microphone are part of every
  pass, as in Lucier's setup (about 60 Hz – 10 kHz).
- *Generation* 1–40, fractions blend two passes; modulatable. *Wachsen*: the generation rises by
  itself while Lucier is on (generations per minute) – over a whole piece. *Wachsen neu beginnen*
  goes back to the set generation.
- *Nachklingen je Durchgang*: a real pass lasts the whole recording; here each pass is damped so
  the resonances swell up within seconds. Short = fast and broader, long = slow and very narrow.
- *Anteil*: 0 = only the organ, 100 % = only the room's resonances. The resonances are kept about
  as loud as the organ (like Lucier bringing every generation back up to level); a chord that hits
  a resonance is caught at once, so nothing bursts out.
- The display shows the response after *n* passes, its strongest resonances (Hz and the nearest
  note, from a′ = 444.5 Hz) and how loud the room sings right now. The response is rebuilt in the
  background whenever something changes and crossfaded in. Switched off, the room rings out.

**Saal** – a simulated room instead of the impulse response, made to be played. *Saal an*
switches it on (the IR on the Raum page pauses meanwhile and comes back when it is off).
- **Four rooms** (*Räume*, above the plan): every stop and the audio input sound in one of
  *Saal 1–4* – row *im Saal*: click a stop's number for the next room, right-click to choose.
  Each room has its own size, surfaces and listener position (the tabs choose which one the plan
  and the sliders show); reflection order and the two levels are shared. Every room in use gets
  its own early reflections and its own late reverb (built in the background like room 1), so
  the cost grows with the number of rooms used. Clicks, own sounds and effects stay in room 1.
  Everything starts in room 1, so older projects sound as before.
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
- **Grundriss** (floor plan, the row of small pictures above the plan, per room): *Quader* (the
  shoebox, exactly as before and the default), *L-Form* (a corner missing front right: the
  reflections go round the corner, some paths are hidden), *Fächer* (narrow at the organ, wide at
  the back), *Sechseck*, *Achteck* (a central building), *Rund* (twelve walls around an ellipse:
  concave, it gathers the sound), *Kreuz* (nave with a transept), *Apsis* (nave ending in a round
  apse) and *Gezeichnet* (your own plan). *Form (Grundriss)* below the pictures sets how far the
  plan departs from the rectangle (modulatable: the room changes shape while you play).
  Width and length are the plan's bounding box; the walls are vertical, floor and ceiling flat.
  - *Gezeichnet*: 3–12 corners. Drag the corners in the plan, double-click on a wall for a new
    corner, right-click a corner to remove it; a plan that would cross itself is refused. Right-
    click into the plan: *Diesen Grundriss zum Zeichnen übernehmen* (start from the shape you see)
    or reset. The drawn plan is saved in the project and in patches.
  - How it works: the mirror images of a polygon are found by reflecting over every sequence of
    walls (up to the chosen order) and keeping only the paths that really hit the walls – in
    non-convex rooms (L, Kreuz, Apsis, drawn ones) also only those not blocked by another wall.
    Floor and ceiling images combine with them as in the shoebox. At most the 62 nearest are
    played. This search is expensive, so it runs only when a pipe has moved ~12 cm, the room
    changed or a sound starts – and spread so each sound searches at most every 6th block;
    in between the known paths simply move with the pipe. Paths that appear or vanish (a
    reflection sliding past the end of a wall) fade in / out over 25 ms, so nothing clicks.
    The ray tracer follows the same walls, so the reverb's decay belongs to the plan. Pipes and
    the listener are kept inside the plan (a pipe stops 0.3 m before the wall along its line to
    the listener, so its direct sound always reaches you). Lucier's room modes use a shoebox of
    the same floor area (the exact modes of an arbitrary plan would need a wave solver).
  - CPU: about the same as the shoebox (worst case 48 moving pipes at 3rd order: ~10–20 % more
    in non-convex plans).
- The plan shows the room from above, the pipes of the pulled stops inside it (pipes outside
  the walls are pushed in), the listener (drag it), the four mirrored rooms and, in red, the
  reflections of the last pipe you played: its mirror images and the paths over the walls
  (other floor plans: the plan mirrored at every wall, and the real paths up to 2nd order).
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
  - *Scala-Datei (.scl / .kbm)*: without a loaded file, this plays 12edo at A4 = 440 Hz
    and displays Stimmton C4 (MIDI 60), 261.625565 Hz. *Scala laden …* takes a scale (`.scl`, cents or ratios) and,
    loaded afterwards, a keyboard mapping (`.kbm`: which key plays which degree, unmapped keys,
    reference key and frequency). *KBM entfernen* goes back to the default mapping: degree 0 on
    C4 (MIDI 60) at 261.625565 Hz, the 12edo C at A4 = 440 Hz. The Stimmton row shows
    the reference MIDI note and frequency supplied by the KBM, or this default reference.
    Scala tuning uses these absolute frequencies independently of the normal Stimmton setting.
    The files themselves are saved in the project and in patches, so they also work on
    another computer. Loading a file selects this entry.
  - *MIDI Tuning (MTS-ESP)*: follows an **MTS-ESP master** (ODDSound; e.g. the free
    *MTS-ESP Mini*, or a master in another plugin), including multi-channel tables (keyboards with
    more than 128 keys, e.g. Lumatone layouts) and keys the master leaves unmapped. Without a
    master it takes **MIDI Tuning Standard SysEx** from the MIDI input: that is what Hexatone
    and similar isomorphic-keyboard tools send. *Töne folgen*: held notes follow a tuning change
    at once (off: each note keeps the pitch it was struck with). Until a master is connected or
    SysEx has arrived, it plays calibrated 12edo at A4 = 440 Hz, independently of the stored
    Stimmton setting. This mode hides Stimmton and puts *Töne folgen* in its place. The status
    identifies the fallback; received tuning is shown as *MIDI Tuning (MTS-ESP) empfangen*.
    MTS-ESP needs its library installed (it comes with every MTS-ESP master); without it the
    plugin works as before.
  - **Hexatone via MIDI loopback** (loopMIDI on Windows, IAC on macOS): in the **standalone
    app** choose the loopback port as MIDI input (*Options → Audio/MIDI Settings*), select
    *MIDI Tuning (MTS-ESP)*, and let Hexatone send MTS SysEx on that port. In **Reaper** the
    track's MIDI input passes SysEx on. **Ableton Live does not pass SysEx to plugins.** There,
    use an MTS-ESP master (e.g. MTS-ESP Mini with the same scale) and send Hexatone only notes.
- Historical temperaments (¼-comma meantone = the original recording, ⅙-comma,
  Werckmeister III, Kirnberger III, Vallotti, Pythagorean, equal). a′ stays put, the other
  pitch classes are shifted. *Stimmton* moves everything (±200 ct, shown as A4 in Hz).
  *Verstimmung* gives every pipe its own fixed random detune (0–60 ct).
- *Tremulant*: on/off, speed, depth – pitch + level wave shared by all pipes, plus a bit
  of wind noise.
- *Registerwind* – the wind model of the stops, i.e. what the *Wind* slider of each stop
  (Spieltisch) does when it is turned down:
  - *klassisch* (default, as before): a half-drawn stop gets quieter, flatter, duller and unsteady.
  - *instabil*: the same, and below roughly half wind each pipe also gets a mouth tone. That is
    the slow jet oscillating at the upper lip. It belongs to the pipe: it starts a few partials
    above the pipe's note and comes down towards it as the wind falls. It is no clean whistle - a
    slow jet is turbulent, so most of it is breath (noise in a band around its pitch) with only a
    core of tone, and both waver in pitch and loudness all the time, the more the weaker the wind.
    Like on a real organ it is
    - steady as long as the wind is steady;
    - lower and quieter as the wind falls (its pitch follows the jet speed, about the square root of
      the pressure);
    - pulled towards the pipe's own resonances, so on some pipes it moves in small hops instead of
      gliding (a stopped Gedackt only has the odd ones);
    - different from pipe to pipe (where it appears, how high, some pipes have a second, weaker
      one), but always the same for the same pipe.
    The pipe also breathes (noise coloured by the pipe, a few partials above its note, swelling
    and sinking a little), and at very low wind (below ~15 %) the speech sputters. The Trompete
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
  free speed glissandi switch it off); *MPE* and *MPE-Beugung*: see section 4.
- *Kopfhörer (binaural)*: *Binaural an* switches the output to binaural (and back to
  stereo), order, own SOFA file, head rotation, diffuse-field EQ – see section 2.

**Effekte** – sounds that are easy to locate (short, bright, full of transients) and that
follow the pipes. Everything is off in older patches.
- *Eigene Geräusche*: your own recordings, played by the keys. Put WAV/AIFF/FLAC files
  (up to 20 s, up to 64 files, subfolders too) into `samples\noise\eigene`, **or** pick any
  folder with *Ordner wählen …* (saved in the project and in patches; *x* goes back to the
  standard folder). *Öffnen* shows the folder in Explorer, *Neu einlesen* reads it again
  without reloading the pipes. Each file is trimmed
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

*Effekte · Stimmung & Belichtung* (second tab, everything off by default):
- **Schwebungsraum** – the tuning moves the room. For every two sounding pipes the engine finds
  the lowest partials that nearly coincide (n × f1 ≈ m × f2) and their beat rate. With the
  motion *Schwebung* (Raum page, Bewegung) every pipe turns as fast as it beats with the others
  (*Geschwindigkeit* = turns per beat): in ¼-comma meantone pure major thirds stand still, the
  fifths drift (c′–g′ about 2.5 Hz), minor thirds go faster (about 6 Hz), the wolf fifth G♯–E♭
  spins (about 13 Hz) and the wolf thirds even faster. A pipe stops where it is when its
  interval becomes pure. *Schwebungen aus*: the tuning as it is meant (ideal) or the pipes as
  recorded (they are only accurate to about ±5 ct, so even pure thirds beat a little). The
  modulation source *Schwebung* (per pipe, 0 = still … 1 = 32 Hz, logarithmic) lets the beats
  drive anything else (e.g. *Schwebung* → *Pfeife: Verfall*: impure intervals crumble).
- **Differenztöne** (resultant ghosts) – every two held pipes sound their difference tone
  (higher minus lower frequency), like the resultant with which organ builders suggest a 32′:
  a fifth gives the octave below its lower note, a pure third two octaves below the root of the
  triad. Up to 12 at once, fading in over ~0.4 s while the interval is held. *Ort*: between the
  two pipes … deep below the listener; *nur Konsonanzen*: 0 = every pair, 100 % = only simple
  ratios. Below ~80 Hz no Ambisonics system places sound well – the deepest ghosts are felt
  more than located.
- **Tonnetz** – an arrangement (Raum page, *Anordnung*): every key gets its place in the
  lattice of the tuning, fifths to the right, major thirds upwards. The tuning's own fifth and
  third decide: pure intervals are short, impure ones long, and each key takes the lattice point
  that is cheapest to reach. In ¼-comma the triads form tight groups and G♯ and E♭ end up on
  opposite edges (the wolf is the seam); in Pythagorean tuning the keys line up as one chain of
  fifths; in equal temperament the thirds are drawn long. Changing the tuning redraws the map.
- **Doppelbelichtung** (double exposure) – the field of a few seconds ago (*Abstand* 0.05–8 s),
  turned (*Drehung*), mirrored (*Spiegeln*: left/right, front/back, up/down), faded (*Verblassen*,
  a low-pass) and laid over the present; *mehrfach belichten* feeds the copy back (more, fainter
  images further back). On the whole field after Hall and Saal, up to 3rd order (stereo: mirrored
  = sides swapped).
- **Belichtung** (long exposure) – 24 directions of the sphere each keep what passes through
  them and let it go slowly (*Belichtungszeit* = −60 dB, 0.2–60 s; *Helligkeit der Spuren*):
  moving pipes leave light trails, fast ones paint arcs and rings. Up to 3rd order.

*Effekte · Lebendige Orgel* (third tab, everything off by default) – the organ as one physical
thing instead of a set of independent samples:
- **Balg** (bellows) – one finite wind supply for all stops. *Windverbrauch* 0 % = endless wind
  (as before); above that every sounding pipe draws from the same supply, low pipes and many
  stops the most. When wind is short, all pipes get flatter, quieter, duller and unsteady
  together (with the wind model *instabil* also their edge tones). *Windquelle*:
  - *Gebläse*: a blower feeds steadily; a big chord pulls the pressure down for as long as it
    sounds, a single pipe hardly at all. *Größe des Balgs* = how slowly the pressure answers.
  - *Kalkant tritt*: a bellows treader lifts the bellows every few seconds (*Tritte* per minute,
    or *im Tempo*: every 8 … ¼ bars). In between it sinks as the pipes use the wind
    (*Größe* = seconds a twelve-pipe chord lasts at 100 %). Play too much and the organ runs out
    of breath until the next tread. *Trittgeräusch* = the thump at the console.
  The two bars show the supply and the pressure. The modulation source *Balg (Winddruck)*
  (0 = empty … 1 = full) lets the pressure drive anything else.
- **Ventil** (pallet) – the pallet under the pipe no longer opens instantly. *Anschlag*: a slowly
  pressed key (low velocity) lets the wind in hesitantly - the pipe speaks flat and dull (with the
  wind model *instabil* also breathy, with its mouth tone), and only then finds its tone (up to
  1.6 s); a fast key speaks at once.
  *Druck* / *Gleiten*: the finger holds the pallet itself (MPE pressure, aftertouch or CC 74) –
  at 100 % no pressure means no tone. The slider sets how far the pallet can stay closed.
- **Hörer** (listener) – you leave the usual listening place. *links / rechts* and
  *zurück / vor* (metres, both can be automated and modulated) set where you stand; *Hörer
  geht* walks around that place by itself (circle, left / right, forward / back, figure
  eight; *Tempo*, *Weite*). Pipes you pass get louder, brighter and move past you, with
  Doppler; in the Saal the early reflections are heard from where you are (the late reverb
  stays that of the usual place, and the Saal page shows you walking). The little map shows
  the pipes of the pulled stops and you (red). Clicks stay with you at the console. You keep
  looking forward – turn the head with *Kopfdrehung* (Klang, binaural).
- **Mitklingen** (sympathetic pipes) – silent pipes, also those of stops that are not pulled,
  ring quietly when a partial of a sounding pipe meets their pitch (within 25 ct), each at its
  own place: up to 16 at once. Pure intervals wake more pipes than impure ones, another tuning
  wakes others. *Nachklang* = how long they ring on.
- **Verschleiß** (wear) – every pipe remembers how long it has sounded. *Lebensdauer* = minutes
  of sounding until a pipe is worn out; *Wie stark man ihn hört* = how much of that is audible.
  Worn pipes go out of tune (most of them flat), waver, and crumble with the grain / bit depth /
  sample rate / dropouts set under *Verfall* on the Spieltisch – so the keys a piece uses most
  fall apart first. The map shows every pipe (one row per stop, red = worn). The wear is saved
  with the project (not in patches: a patch does not make the instrument young again);
  *Alle Pfeifen neu* resets it. It only accumulates while the slider is above 0.
- **Reinheit** (adaptive pure tuning) – held notes pull each other towards pure intervals
  (springs between every pair of notes, strongest for octaves, fifths and thirds; a weak one
  holds the chord at its tempered pitch). New notes start in the chosen tuning and glide
  (*Zeit bis rein*): the beats calm down and the chord locks in. Even the wolf fifth is tamed.
  With the motion *Schwebung* the pipes stop turning as the chord becomes pure; the difference
  tones settle too. The title shows the largest correction in cents.
- **Tonnetz-Wanderer** – while you hold keys, a walker steps through the lattice of the tuning
  (fifths to the right, major thirds up) and sounds every key it comes to, starting at the key
  struck last. *Weg*: along the fifths (after eleven steps: the wolf), a chain of thirds
  (major, minor, major …), random, random with homesickness, or a spiral around the first key.
  *Schritt*: free (*Tempo*) or a note value of the project tempo. *Tonlänge* above 1 step lets
  the notes overlap. With the arrangement *Tonnetz* (Raum page) the sound takes the same walk
  through the room. Your own keys sound as usual. Not available with free tuning (Scala / MTS)
  or MPE. Patches 148–159 show all of this.

*Saal · Eis* (Saal page, below *Nachhall*): the walls turn to ice – in every early reflection
the low frequencies travel slower than the high ones (an all-pass chain per Ambisonics channel of
the reflections), so each reflection becomes a falling chirp from its own direction; the later
reflections (longer ways) chirp more. *Eis* blends from the plain reflections to only the chirps,
*Eis Länge* sets how long a chirp is (about 5–50 ms).

*Saal · Begehen* (button *3D begehen* on the floor plan): the Saal in 3D, seen through your own
eyes. The pipes float where Ambisonics places them (with their motion and modulation); sounding
pipes glow. **W A S D** walk, **dragging the mouse** looks around, the left and right arrow keys
turn, **Shift** runs, **Space** jumps, the **mouse wheel** changes the field of view, **Esc**
goes back. Walking is smooth: the speed builds up and dies away, the view follows the mouse
within a few hundredths of a second, and the ears go up with a jump. The
camera is the listener: walking moves the listener of the room simulation and the direct sound
(as *Hörer* on the living-organ tab does), the walls stop you. Turning your head is heard with
headphones (binaural, transaural) only; with the Ambisonics or loudspeaker outputs the field
stays still. A small map (bottom right) is turned with your view. While the view has the keyboard
the walking keys do not play notes: play over MIDI, or hold a chord with *Halten*.
*Raummodell laden …* takes an OBJ file (with its MTL colours, up to 80,000 triangles) as the
room you see; *Meter / Zentimeter / …* and *Z oben* (Z instead of Y as the up axis) fix the
import. The model is seen, walked (its walls stop you, steps up to 45 cm are climbed) and –
with *Modell hörbar* – heard: a pipe behind a wall or pillar of the model comes around its edge,
quieter and duller the longer the detour, and the late reverb is ray-traced through the model's
triangles. The Saal's shoebox stays underneath (drawn as a red outline): it gives the early
reflections, closes a model that is open somewhere, and its floor and ceiling are drawn far
beyond the model, so a model without a floor or a roof never shows a black void.
*Maße übernehmen* gives the Saal the model's width, length and height (as a rectangle).
*Zum Platz* goes back to the listener's place. The model's file and where you stand are saved
with the project (not with patches); a missing file is kept by name.
Six rooms come with the plugin, in the menu of *Raummodell laden …*: *Kapelle* (12 × 24 × 9 m,
columns, pews, altar steps), *Hallenkirche* (21 × 40 × 15 m, three aisles, a west gallery
reached by a stair), *Krypta* (13 × 17 × 4 m, sixteen short columns under ribs), *Rotunde*
(18 m round, a ring of eight columns under a dome), *Kreuzgang* (a covered walk around an open
court with a well) and *Treppenturm* (9 × 9 × 23 m, five flights of twenty steps up to a landing
20 m above the floor). Choosing one writes it as an ordinary OBJ (with `rysum_modelle.mtl`) into
`Documents/Rysum/Modelle`, loads it z up in metres, gives the Saal its size and puts the listener
at a free place in it. They are closed (except the court), their walls have a thickness, and
their steps are 18–20 cm, so everything meant to be climbed can be.
The room is drawn in greys like an old print, with moving film grain, and its surfaces follow
the Saal's acoustics: the wall absorption and high damping pick the nearest of the Saal page's
materials (*Stein*: ashlar blocks, *Putz*: plaster, *Holz*: panelling, *Glas*: leaded panes,
*Vorhang*: heavy folds, *Spiegel*: bright and smooth); the more a surface swallows, the darker it
is, *Streuung* makes it rougher, high damping duller. The floor and ceiling follow their own
absorption (stone slabs, boards, carpet).
*Single surfaces*: the floor, the ceiling and every wall can be of its own material. On the Saal
page choose the surface next to *Oberflächen* and press a material (again: back to the sliders);
in the 3D view look at a surface and press **M**. A surface with its own material reflects,
damps the highs and feeds the ray-traced reverb with that material's absorption, and looks like
it; they are saved with the project.
*The player and the pipes* (*Pfeifen: …* in the 3D view, or *Spieler bewegt die Pfeifen* on
Effekte · Lebendige Orgel): the listener pulls the pipes like a mass (*zieht an*), pushes them
away (*stößt ab*), lets them circle (*Umlaufbahn*) or drags them along (*Sog*), with *Kraft* and
*Reichweite*. It acts on stops whose motion takes place in the Saal (Flug, Fall, Schwarm,
Strömung) – and on the new motion *Schweben*, where the pipes rest at their place until the
player moves them (*Tiefe* = how free they are, *Geschwindigkeit* = an unrest of their own).
**G** lets the graphics card or the processor draw the picture (see chapter 6; the card is the default).

*Immersiv* (button in the top row, or the key **I**): nothing but the picture. The 3D view fills
the whole screen (the window goes to full screen for it and comes back afterwards), with no
buttons, no writing and no map – only the small cross in the middle stays. The mouse looks around
without a button held: the pointer is hidden and kept inside the view. A click knocks on the
pipe under the cross. **Esc** or **I** goes back; so does switching to another program, which
also frees the pointer at once.

The hands are also buttons in a row at the bottom (*E Druckwelle*, *Q Greifen*, *F Anhalten*,
*R Zurückrufen*, *C Klatschen*, *M Material*, *Springen*), so each can be put on a MIDI button
(see *MIDI buttons for any button* in chapter 4) – also *Immersiv* itself. *Greifen*, *Anhalten*
and *Springen* act for as long as the MIDI button is held.

*Illusions* (the row of sliders under the buttons; all off at first, all of them parameters – they
can be modulated, automated and put on MIDI knobs with a right-click):

- **Selbstspiel** – the room plays itself. A pipe that knocks against a wall, the floor or the
  ceiling sounds its own note, longer the harder it hits. Needs stops whose motion happens in
  the Saal (*Flug*, *Fall*, *Schwarm*, *Strömung*); nobody has to play. At most 40 such notes
  at once, and never at the cost of what you play.
- **Wachsen & Brechen** – pipes that are not sounding slowly grow and sink in pitch (up to twice
  their length, an octave down: half a minute at 100 %, a quarter of an hour at 10 %). A hard
  knock breaks a pipe in two: it is half as long and an octave higher (down to a quarter), with
  a crack. You see the lengths. Turned to *aus*, every pipe finds back within a few seconds;
  **R** mends them all at once. One pipe stays one pipe: the broken-off half is heard as the
  crack, it does not fly on as a second pipe.
- **Flüstern** – a pipe within arm's reach of your head (1.4 m) is also heard at the two ears
  themselves, and leaves the room sound as it comes: the near ear much louder (more than 10 dB
  at 15 cm), the far one later and duller, the tone warmer. Take a pipe with **Q** and bring it nearer with the **mouse wheel** (down to 20 cm),
  or let the pipes come to you (*Pfeifen: ich ziehe an*). Headphones (binaural, also over two
  loudspeakers) and stereo only; with Ambisonics or loudspeaker output it does nothing.
- **Zeit als Entfernung** – the front of the room plays the past. At your usual place you hear
  the present; at the front wall what sounded so many seconds ago (up to 20 s; with many
  Ambisonics channels the memory is shorter, e.g. about 14 s with third-order binaural at
  48 kHz). Walk forwards quickly and time runs backwards; walk back and it catches up, at up to
  four times the speed. The top line says how far back you are hearing. It also follows the
  listener's automatic walk (*Effekte → Hörer*), without the 3D view.
- **Hörraum** – the late reverb belongs to a room of another size than the one you see and walk
  in (× 0.25 … × 8): see a chapel, hear a cathedral. The early reflections stay those of the
  room you see – that contradiction is the illusion. Needs *Saal an*.
- **Wandnähe** – the nearer you come to a wall (from 3 m), the larger and louder the late
  reverb, as if another room opened behind it. Needs *Saal an*. The reverb is recalculated as
  you walk (a few times a second, crossfaded).

*The player's hands* (keys in the 3D view; with **Shift** stronger):


**E** – a shockwave: a deep burst pushes every pipe away from you, the nearer the harder, and
they knock on the walls. **Q** (held) – takes the pipe under the cross (it gets a ring when a
pipe is within 9 m), carries it ahead of you, and throws it when you let go – with the speed
of your hand and your walk; a hovering pipe stays where it comes to rest. **F** (held) – holds
everything that moves still. **R** – calls every pipe back to its place in the organ.
**C** – a hand clap where you stand, to hear the room from there. A **click** on a pipe knocks
on it: it rings with its own modes and faintly its note, without its key.
Pushing, carrying and throwing need the pipes to be bodies in the Saal: a stop that stands still
is given the motion *Schweben* by E and Q (the 3D view says so); stops with another motion
(Rotation, Pendel …) are left as they are. Pipes the player has just moved knock on walls even
when *Aufprall* is turned down.
The pipes are real 3D pipes – a conical foot, the
round body, the mouth turned towards the listener's place. The view is drawn on several
threads at the screen's full resolution and lowers it by itself when a frame takes too long.
*Out of sight* (floor plans that are not convex: L, cross, apse …, also when walking with
*Hörer*): when a wall stands between you and a pipe, its direct sound comes around the corner –
from the corner's direction, over the longer way, quieter and duller the sharper it bends
(over two corners into the far arm of a cross). The reflections still come from where they
reach you. The 3D view shows how many sounding pipes are heard around a corner.

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
  = ±2 semitones, or anywhere else), *Schwebung* (per pipe: how fast it beats with the other
  sounding pipes, see Effekte · Stimmung & Belichtung), *Balg (Winddruck)* (the pressure of the shared
  bellows, 0 … 1, see Effekte · Lebendige Orgel).
- *Zuordnungen* (24 slots, scroll the list): source → target, amount ±100 % (click the
  amount to type it), and **± / +**: ± = bipolar (an LFO swings around the slider value),
  + = unipolar (it only pushes one way, 0 … amount; envelopes and the mod wheel are 0 … 1 anyway).
  Targets:
  - *Pfeife: Richtung / Höhe / Abstand / Lautstärke / Tonhöhe / Helligkeit / Hall-Anteil* –
    act on every pipe separately (at 100 %: ±180°, ±90°, ×8 distance, ±36 dB, ±12 semitones,
    ±6 octaves of brightness, ±24 dB room send);
  - *Pfeife: Wind / Verfall / Windschwankung / Knistern* – also per pipe: the pipe's own stop wind
    (added to the stop's *Wind* slider, ±100 %; 0 % = the pipe falls silent, low wind = flatter,
    duller, unsteady – with wind model *instabil* also its edge tone), its amount of *Verfall*
    (mode *pro Pfeife*, using the Verfall section's grain / bits / rate / dropouts), its
    *Windschwankung* and its crackle (*Knistern*, Effekte page). Example: *Hüllkurve 1 pro Pfeife*
    → *Pfeife: Wind* −80 % with a slow attack lets every note start full and sag on its own;
    *Tastenlage* → *Pfeife: Verfall* wears out only the high pipes;
  - in submenus, added on top of the pipe's stop setting (Raum · Pfeife / Wolke, Verfall, Glitch):
    *Ansprache · Abriss · Obertöne* (Ansprache ±1, Schärfe / Nur Ansprache / Wiederholen /
    Abriss ±100 %, Obertöne ab ±4 octaves, Pegel ±24 dB, Anschlag ±100 %, Luft umkehren ±100 %),
    *Wolke* (Menge ±100 %, Dichte and Korngröße ×8 / :8, Streuung ±180°, Zeitstreuung ±150 ms),
    *Verfall* (Körnung / Aussetzer ±100 %, Bittiefe ±13 bits, Abtastrate ±6 octaves – only in the
    mode *pro Pfeife*, and only where the pipe's *Verfall* amount is above 0) and *Glitch*
    (Stottern, Bandschnitt, harte Kanten, Clipping ±100 %). *Wiederholen* and the hard start of
    *harte Kanten* are taken when the note starts, *Abriss* and the hard release when the key
    is let go; everything else follows the modulation all the time (Obertöne Pegel and Luft
    umkehren glide over ~4 ms). Example: *Zufall je Ton* → *Pfeife: Ansprache* +60 % gives every
    note its own chiff; *Tastenlage* → *Pfeife: Wolke Menge* dissolves only the high pipes;
  - every continuous parameter of the plugin (stop volume, motion speed, wind, Verfall,
    Doppler, Hall, even the LFO speeds themselves …), amount = share of the slider's range.
    **These are global:** one value for the whole instrument. There the per-pipe sources act
    as one: an envelope *pro Pfeife* behaves like the global one (every key restarts it), the
    LFOs' *Versatz* does nothing, *Tastenlage* / *Anschlag* are those of the last note. For
    something to differ from pipe to pipe, use a *Pfeife:* target (the target menu lists them
    first, the tooltip on a routing's target says which kind it is).
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
share them. Example patches are written there on first start, and new ones with each new
set (deleted ones are not re-created). Set 9 (59–79) shows the newer features:
59–63 Lucier (live), 80 Lucier as a piece (74 too, with the input), 64–67 expression (MPE pressure / slide – polyphonic or channel aftertouch
work as pressure too), 68–71 scenes (move *Überblenden* on the Spieltisch; 70 moves by itself
with an LFO), 72–79 the audio input (they switch it on); set 11 (81–85): notes spread to single
pipes and stops in different rooms; set 12 (86–99): Saal floor plans (86 L-shape round the
corner, 87 rotunda / whispering gallery, 88 cruciform church, 89 fan, 90 a plan that breathes
with an LFO), Verfall · Raum (91 weathering, 92 direction grid, 93 sphere of sand – crushed and
dropping out, 94 ghost directions, 95 an LFO erodes the sphere and lets it grow back) and grain
clouds (96 shimmering Mixtur, 97 the organ dissolves – move *Überblenden* A → D, 98 a choir from
all directions, 99 clouds circling in an octagonal hall while the sphere weathers). 91–95 need an
Ambisonics output (AmbiX, binaural or loudspeakers); in stereo only 92's grid is heard. Set 13 (100–117): Ansprache (100 soft speech, 101 sharp chiff on the high
registers, 102 only the chiff, 103 repeated chiff), Abriss (104 breath, 105 sighs in a cruciform
church), upper partials (106 glass organ, 107 under the ceiling, 108 needle attack), 109 inverted
air (distant presence), key-click resonance (110, 111), glitches (112 stutter, 113 tape splices,
114 hard edges, 115 clipping storm), 116 an LFO breathes the Ansprache, 117 everything decays.
101, 102, 110, 111 and 114 use registers other than the Praestant 8′, so they are quiet or silent
in a build that only has the Praestant samples. Set 14 (118–133) is made for playing with the hands – velocity,
pitch wheel (±2 semitones per pipe unless noted), mod wheel, pressure / slide: 118 from a breath to
a scream (velocity shapes chiff, brightness, level, overtones, distance, the ringing keys), 119 a
church becomes a machine (mod wheel through four scenes), 120 out of breath (pitch wheel down
starves the wind, wind model *instabil*), 121 the pipe under the finger (pressure blows it – needs
aftertouch or MPE), 122 a long walk (four slow random LFOs, never the same), 123 glass shatters
(hard notes cut and clip), 124 no note like the last (*Zufall je Ton*), 125 the organ evaporates
(mod wheel: only the chiffs remain, in a cloud, far away), 126 clockwork (every pipe gated at its
own phase), 127 the organist has been drinking, 128 every note crumbles on its own (pressure holds
it together), 129 four organs on the mod wheel, 130 gravity (pitch wheel lifts the pipes into the
vault), 131 continuum (MPE pressure and slide), 132 slow erosion (ten minutes and more), 133
explosion (velocity, pitch wheel ±12 semitones). Set 15 (134–147): the tuning in space – 134 the tuning dances
(motion *Schwebung*), 135 wolf howl (beats make the impure intervals crumble), 136 beats as light
(impure intervals dissolve into clouds), 137 resultant bass, 138 ghosts between the pipes, 139
ice hall, 140 frozen rotunda, 141 mirror image, 142 echo of the past (double exposure, again and
again), 143 light trails, 144 twenty seconds (long exposure of jumping pipes), 145 the tuning as a
map (Tonnetz), 146 the map floats (Tonnetz + beats), 147 the tuning becomes space (everything on
the mod wheel). Set 14 also re-issues 70: factory scenes used to
store the routes at 0 %, so moving through the scenes switched the LFO off that moves them.

A patch made with the input on switches the input on when it loads; every other patch leaves
the input as it is (on or off belongs to your setup, like MPE).
The project in Reaper still remembers everything on its own, patches are extra.

### Eingang – audio input (page Raum, last row of the list)

Live audio – a voice, an instrument, another track – placed and moved in the same space as the
pipes. Switch it on with the square left of **Eingang (Audio)**; click the row for its panel:
*Mono / Stereo* (two sources, *Breite* apart), *Pegel*, *Richtung*, *Höhe*, *Abstand* (level,
air and travel time like a pipe: moving sources get Doppler), *Kreisen* (turns around you, °/s),
*Hall-Anteil*, and **Ort**: *eigener Ort*, or *auf <Register>* – the input rides on the pipes of
that stop and makes its motion (left on the low, right on the high side). **Tasten öffnen**:
the input only sounds while keys are held (*Ausklang* = how fast it closes). Drag the input
in the top view to place it.

Everything after the pipes works on it too: simulated room (Saal) or impulse response, echoes,
Verfall on the sum, Lucier, the output (stereo / AmbiX / binaural). Its loudness is also the
modulation source **Eingang (Pegel)** – even with the input off, so a voice can open the
organ's wind or brightness without being heard.

How the audio gets in:

- **Reaper:** the instrument has inputs. Audio on Rysum's own track goes in directly
  (record or route it to the track); other tracks: a send to channels 3/4 (side chain).
- **Ableton Live:** put **Rysum FX** on the audio track (for a microphone: monitoring *In*).
  To play the organ as well, set a MIDI track's *MIDI To* to that track / Rysum FX. The
  instrument also has a side-chain input for hosts that offer it.
- **Standalone app:** *Options → Audio/MIDI settings*, choose the input, and untick *Mute audio
  input* (use headphones – speakers feed back).
- **Rysum FX** works the same in every host: its input is the track's audio, MIDI routed to it
  plays the organ. The input is on by default there.

### Pipes on / off, notes to single pipes

- **Pipes** (Raum page, under *Anordnung*): the 41 pipes of the selected stop; click to mute
  one, drag across several, right-click for patterns (all on, all off, every other one, lower /
  upper half, half at random, invert). Muted pipes do not speak; in the top view they are faint
  rings. Saved with the project and in patches.
- **Töne verteilen** (Spieltisch, Traktur): instead of every pulled stop sounding each note,
  every note goes to a single pipe of one stop:
  - *der Reihe nach*: 1st note on the first pulled stop (console order, left to right), 2nd note
    on the next, …;
  - *nach Lage*: by position in the chord at the moment it is played – the lowest held note on
    the first stop, the next one up on the second …;
  - *zufällig*: a random stop, never twice the same in a row.
  Muted pipes are skipped (the note goes to the next stop that has that pipe). Pulling a stop
  while notes are held does not add them to it.

## 4. MIDI

MIDI note = sounding pitch (C2 = 36 … a″ = 81). Velocity shapes the key clicks: a light touch
gives a quieter and duller click, also when the key comes up. C♯2, D♯2, F♯2, G♯2 and g♯″ do not
exist on the organ and stay silent (short octave). Sustain pedal (CC64) works.

With a free tuning (Scala or MTS-ESP, Klang page) every MIDI note 0–127 on every channel can sound,
each at the pitch the tuning gives it (see *Stimmung*). MIDI Tuning Standard SysEx is read there.
The pitch wheel and velocity are also modulation sources (*Tonhöhenrad*, *Anschlag*), and so are
pressure and slide (*Druck (MPE)*: MPE pressure, polyphonic aftertouch or channel pressure;
*Gleiten (MPE, CC 74)*), per note.

### MPE (Lumatone, LinnStrument, Seaboard, Osmose …)

Switch on *MPE* (Klang page, MIDI). Then every note is on its own MIDI channel (2–16) and is
bent, pressed and slid on its own:
- *Pitch bend* on a note channel bends only that note; *MPE-Beugung* is the bend range for a full
  bend (default 48 semitones, as most MPE controllers send; set it to what yours sends). The pipe
  keeps sounding and is shifted, so a long slide sounds like the pipe being pitched, not like
  changing pipes.
- *Pressure* and *slide* (CC 74) are modulation sources per note: route them on the Modulation page
  (e.g. *Druck* → *Pfeife: Lautstärke* or → *Pfeife: Helligkeit*, *Gleiten* → *Pfeife: Richtung*).
- Channel 1 is the whole instrument: its pitch wheel is the *Tonhöhenrad* modulation source.
- With a free tuning (Scala, MTS-ESP) the tuning of channel 1 applies to all channels (MPE uses the
  channels for the notes, not for different tunings).
- MPE and its bend range belong to your controller, not to the sound: patches do not change them.

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

### MIDI buttons for any button

Every button of the interface can be put on a MIDI button: a pad, a controller button or a key.

- **Ctrl+M**, or *Modulation → Zuordnungen → MIDI-Regler → Knöpfe der Oberfläche zuordnen …*,
  lays a veil over the interface in which every button is framed. **Click a button, then press
  the MIDI button.** The button gets a red tag with what it listens to (`CC 30 · K1`,
  `Note C2 · K10`).
- **Right-click** removes the assignment. **Ctrl+click** operates the button as usual (to get to
  another page). **Esc** ends it. While assigning, MIDI buttons operate nothing.
- A MIDI button may send a controller (pressed = 64 or more) or a **note**. A note that is
  assigned no longer plays a pipe. Pick pads or keys outside the range you play.
- Switches that are parameters (the stops, *Saal an*, the on/off buttons of the effects …) get an
  ordinary entry in the *MIDI-Regler* list: each press flips them, and they work while the
  window is closed.
- All other buttons (pages, patches, *Zufall*, *Vollbild*, the buttons of the 3D view …) are
  clicked by the window, so **the plugin window has to be open** for them. The buttons that act
  while held in the 3D view follow the MIDI button: down = held, up = let go.
- Saved like the other assignments: in the project and in `Documents\Rysum\MIDI-Regler.xml`.
  A button is remembered by its place in the interface, so an assignment may need to be made
  again after an update that rearranges a page.

### A dataset for a neural network

`scripts\make_dataset.ps1` samples the plugin into a dataset for training a neural synthesis
network: the plugin plays by itself and every clip is written with its notes and settings.

```powershell
cd D:\RYSUM_plugin\rysum_plugin
.\scripts\make_dataset.ps1
```

That makes ten hours (3600 clips of 10 s, stereo, 48 kHz, 16-bit WAV, about 7 GB, roughly twenty
minutes) in `rysum-dataset` next to the repository:

| file | what |
|---|---|
| `audio\000000.wav` … | the clips |
| `midi\000000.mid` … | the notes of each clip |
| `metadata.jsonl` | one line per clip: file, `train` / `valid`, kind, stops, notes (on / off in seconds, MIDI note, velocity), levels, and the parameters that differ from the opened plugin |
| `dataset.json` | the format, and the table of all parameters with defaults and ranges |

- **What is played:** 40 % single pipes (one stop, one held note), 40 % registrations of one to
  five stops playing a phrase (lines, chords, a line over a held chord, free notes), 20 % factory
  patches playing a phrase. `-Mix "100,0,0"` changes the shares.
- **Options:** `-Hours`, `-Clip` (seconds, at least 4), `-Rate`, `-Mono`, `-Bits 24`, `-Flac`,
  `-Seed`, `-Dry` (no room in any clip), `-Valid` (per cent validation clips), `-Out`.
- Stopped with Ctrl+C and started again with the same options, it carries on where it was. The
  same seed gives the same notes and settings; the sound has the organ's own chance in it, so
  two runs are not identical sample for sample.
- Clips that would clip are turned down (`gain_db` says by how much); a silent draw is repeated.
- The 3D view's illusions are not part of it.

## 5. Files

```
plugin/
  CMakeLists.txt        build description
  Source/               C++ code
    OrganEngine.*       voices, loops, releases, noise layer, encoder, degradation
    SampleLibrary.*     reads rysum_samples.json + WAVs, replaces unusable pipes
    Spatial.h           AmbiX encoder (ACN/SN3D), stereo panning, layouts, motion
    Degrade.h           "Verfall"
    SpatialDegrade.h    "Verfall · Raum" (spatial erosion) and the grain cloud parameters
    PluginProcessor.*   parameters, state, sample loading
    Convolver.*         impulse-response reverb (stereo / AmbiX)
    PartConv.h          partitioned convolution with its work spread over time (used by all convolutions)
    FastFFT.h           PFFFT wrapper (real FFT, spectra products), aligned buffers
    Kernels.*           innermost loops, plain + AVX2/FMA (chosen at start-up)
    Binaural.*          HRTF sets (built-in FABIAN, SOFA via libmysofa), MagLS design, binaural decoder
    SofaGunzip.cpp      zlib inflate for libmysofa (JUCE's zlib)
    Modulation.h        LFOs, envelopes, routing definitions
    RoomSim.h           simulated room: image sources, Eyring, ray tracer
    RoomTail.*          late reverb of the simulated room: synthesised responses, partitioned convolution
    Scala.h             Scala files (.scl scales, .kbm keyboard mappings)
    WindChaos.h         wind model "instabil": resonances, intermittent speech, hiss of starving pipes
    PluginEditor.*, ConsoleView.*, SpaceView.*, SaalView.*, KlangView.*, EffectsView.*, ModView.*,
    MidiMapView.*, Theme.h   interface
  Resources/            photos, fonts (OFL), grain texture, fabian_hrir.bin (FABIAN HRIRs)
  ThirdParty/libmysofa/ SOFA reader (BSD-3, see its LICENSE and README_RYSUM.txt)
  ThirdParty/MTS-ESP/   MTS-ESP client by ODDSound (0BSD, see its LICENSE and README_RYSUM.txt)
  ThirdParty/pffft/     PFFFT by Julien Pommier (FFTPACK licence, BSD-like, see its LICENSE and README_RYSUM.txt)
  tools/make_fabian.py  rebuilds Resources/fabian_hrir.bin from the FABIAN SOFA file
  Tests/                offline render test + UI snapshot tool (optional)
```

Optional tests (not needed for normal use):
`cmake -B build -DRYSUM_BUILD_TESTS=ON` builds `RysumRenderTest` (renders MIDI scenes
to WAV and checks the encoder maths) and `RysumUiSnapshot`.

CPU benchmark (optional): `cmake -B build-bench -G "Visual Studio 17 2022" -A x64 -DRYSUM_BUILD_BENCH=ON`,
then `cmake --build build-bench --config Release --target RysumBench`, and run
`build-bench\RysumBench_artefacts\Release\RysumBench.exe D:\RYSUM_plugin\rysum_plugin\rysum-samples 128 8`
(samples folder, block size, seconds per scenario, optionally a filter such as `saal`). It plays all
eight stops with a ten-note chord and moving pipes through the whole plugin in several set-ups
(stereo, AmbiX 3 / 5 with an impulse response, binaural, Saal) and prints for each the average load
and the worst blocks in % of one core's real-time budget. The worst blocks are what cause dropouts.
A second line says where the time went (before the pipes, the pipes, the reflections with their
geometry, the late reverb, what follows the engine). `walk3d` are two scenes of the 3D view with
everything on; `RYSUM_AUDIO_THREADS=1` (an environment variable) measures without the helper
threads, and `RYSUM_BENCH_WAV=<folder>` writes what was measured as WAV files for comparing two
builds.

## 6. CPU

**The picture**: the interface draws 60 times a second (the 3D view lowers its resolution by itself when a
frame gets slow). **Ctrl+Shift+P** shows a readout: pictures per second, milliseconds per picture, how
much of the window is redrawn, whether the interface thread falls behind, the renderer (Direct2D or
software) and the pixel scale; once a second it is also written to `Documents/Rysum/paint-stats.txt`.
**Ctrl+Shift+R** switches the renderer (for comparing; Direct2D is the fast one).

**The sound** is in the same readout: *Ton* is the share of its time an audio block takes, with the
worst block since the last line (towards 100 % it crackles), and *Kerne* the processor cores the audio
uses. The next line says where the engine's time goes, in % of real time: before the pipes (their
motion in the Saal, their places), the pipes, the reflections, the late reverb. On the 3D page a last
line gives the picture's cost: milliseconds per picture, how much of it is geometry, drawing the
pixels, and the resolution it has stepped down to.

**Several cores.** The Saal's early reflections - by far the largest part of a full scene - are
rendered in twelve groups of sounds, each into its own bus, and spread over helper threads: by
default half the processor's cores, at most four. With 48 reflected sounds and reflections up to 3rd
order the load of the audio thread falls from 88 % to 45 % of its time and the worst block from 150 %
to 68 % (Ryzen 5 2600X, blocks of 128); the 3D view with everything on from 62 % to 42 %. The sound is
the same, bit for bit, whichever thread did what. *Kerne für den Ton* is a parameter (*automatisch*,
1 ... 4); **Ctrl+Shift+K** steps through it. In a host that already fills every core with other
tracks, 1 can be the better choice. The helpers sleep while there are fewer than eight reflected
sounds.

**Pipes flying in a shaped Saal.** A pipe remembers how far the nearest wall was and only asks the
walls again when it may have come near one; the walls and the listener's place are worked out once
per slice, not for every pipe. The result is the same to the last bit; in a drawn room with all
pipes flying the audio thread's load fell from 36 - 42 % to 29 - 35 %.

**The graphics card draws the 3D view** (OpenGL 3.2, which the built-in graphics of laptops of the
last ten years offer too - measured only on a desktop card so far).
The processor still places the room and the pipes; the card fills the pixels - the patterns of the
surfaces, fog, grain and the grey of the print are the same formulas, as a shader - and draws the
glow of the sounding pipes. The picture is as sharp as the screen at any window size (the
processor's picture steps its resolution down to keep up), and the whole program needs about a
quarter of the processor time: with a full scene 0.7 instead of 2.6 to 3.1 cores on a Ryzen 5 2600X
with a Radeon RX 6700 XT, the sound included.
**G** in the 3D view switches between the card and the processor (remembered for this computer in
`Documents/Rysum/Grafik.xml`); the line of hints at the bottom says who is drawing. If the card or
its driver cannot do it, the processor draws, as before, and while buttons are being assigned to
MIDI (Ctrl+M) too. Should a host misbehave with it: **G**, or `grafikkarte="0"` in that file, or
`RYSUM_GPU=0` (an environment variable) - each gives the processor's picture back.
`RYSUM_GPU_TEST=1` writes frames drawn both ways and their difference (the scene, every kind of
surface, a model) to `Documents/Rysum/gpu-test`.

**For the sound the graphics card is not used**, and it would not help: the work is thousands of
small jobs that must each be done within about a millisecond, and the way to the card and back takes
longer than the job.

What costs the most, roughly in this order: the simulated room (*Saal*, every sounding pipe has its
own early reflections - up to 48 sounds × 24 or 62 mirror images), a long impulse response at a high
Ambisonics order, the number of sounding pipes × output channels (8 stops × 10 notes = 80 pipes, each
into 36 channels at 5th order), binaural / loudspeaker decoding. How the plugin keeps it down:

- **Convolutions without spikes.** All real-time convolutions (impulse response, binaural decoder,
  the Saal tail, Lucier) use PFFFT (SIMD) instead of JUCE's FFT, which on Windows is a plain scalar
  one. The impulse response is partitioned non-uniformly: the early part and the first 75 ms of the
  tail in blocks of 512, the rest in blocks of 2048 and 8192 - about 12× fewer operations for a 3 s
  response. The large blocks spread their work evenly over the time they cover instead of computing
  everything in the one audio block where a partition is complete (before, every 512 / 8192 samples
  a single block had to do the whole convolution - the CPU meter's peaks). Latency unchanged.
- **AVX2.** The innermost loops (`Source/Kernels.*`) exist twice; on CPUs with AVX2 + FMA (Intel
  since 2013, AMD since 2015) the faster version runs, otherwise the plain one - decided at start-up.
- **Early reflections** four mirror images at a time; the Ambisonics encoder without per-channel
  trigonometry; the mixing of every pipe into the output channels in one pass per four channels.

If it is still too much: the reflection order of the Saal (1st order = 6 instead of 24 / 62 mirror
images per sound), the Ambisonics / binaural order (3rd instead of 5th: 16 instead of 36 channels),
and fewer stops or shorter impulse responses help the most. A larger audio buffer in the host
(256 or 512 instead of 64 / 128) lowers the overhead per block.

Fonts: "Rysum Fell" = IM FELL DW Pica by Igino Marini with a few symbols (arrows, primes, ♯ …)
added from DejaVu Serif and renamed as the OFL requires (built by `tools/make_fell.py`);
UnifrakturMaguntia for the title; IBM Plex (no longer used in the interface). All SIL Open
Font License (see `Resources/OFL_*.txt`).

HRTF: FABIAN head-related transfer function database, Audio Communication Group, TU Berlin –
F. Brinkmann et al., "A High Resolution and Full-Spherical Head-Related Transfer Function
Database for Different Head-Above-Torso Orientations", JAES 65(10), 2017, data DOI
10.14279/depositonce-5718, licence CC BY 4.0 (see `Resources/FABIAN_LICENSE.txt`).
SOFA files are read with libmysofa by Christian Hoene / Symonics (BSD 3-clause).
Microtuning via the MTS-ESP client library by ODDSound Ltd. (0BSD).
FFTs: PFFFT by Julien Pommier, based on FFTPACKv4 by Paul N. Swarztrauber, NCAR (FFTPACK licence,
see `ThirdParty/pffft/LICENSE` - its copyright notice has to go with binary distributions too).
