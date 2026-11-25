Rysum - technical documentation (LaTeX), version 17 of 25 November 2026

Build (XeLaTeX is required for the plug-in's fonts):
   latexmk -xelatex main.tex
   or: xelatex main && bibtex main && xelatex main && xelatex main
   or (single program, fetches the LaTeX packages itself): tectonic -X compile main.tex
Needs TeX Live / MiKTeX with fontspec (XeLaTeX), scrartcl, tikz, siunitx, natbib, lettrine, eso-pic.
Versions 12 to 16 were compiled with Tectonic 0.17.0.

main.tex        the document
math*.tex       Appendix A, the mathematical reference (math.tex includes the parts: math_voice, math_conv,
                math_out, math_room, math_ctrl, math_live, math_bodies, math_walk, math_misc)
refs.bib        bibliography
fonts/          the plug-in's typefaces (IM Fell / "Rysum Fell", UnifrakturMaguntia, IBM Plex Mono; SIL OFL
                licences included) and the grain texture
figs/           plots (PDF, dark style) + make_figs.py and make_figs_new.py (floor plans, spatial degradation) and
                make_figs_tuning.py (beats, Tonnetz) and make_figs_live.py, make_figs_dsf.py (bellows; re-implements
                OrganLive.cpp) that regenerate them from rysum-samples/rysum_samples.json:
                python3 make_figs.py <path-to-samples-folder>
                The mouth-tone figure (edgetone.tex, TikZ) follows the wind model of 14 November and is written by
                make_edgetone_tikz.pl:  perl make_edgetone_tikz.pl > edgetone.tex
screens/        UI screenshots rendered with RysumUiSnapshot (1280 x 720; the 3D view 1280 x 760)
tests/          the logs of the test runs (Windows, Ryzen 5 2600X):
                2026-11-15/ (commit 44b5ef0): rendertest.log (375 checks), uisnapshot.log (67 checks + all 175
                factory patches), walk.log (the 3D view, 46 checks); all passed
                2026-11-18/ (commit 8a9cbe5 + the RysumUiSnapshot fix of version 13): rendertest.log (384 checks, all
                passed), uisnapshot.log (85 checks + all 181 patches of the folder, all passed), walk.log (48 checks,
                47 passed: the walking check read this computer's pace x2.5), concert.log (20 checks, 19 passed: an
                intermittent timing check of the input, passed in two reruns)
main.pdf        compiled version (151 pages)

New in version 12: the second recording session (30 October), the unstable wind's new mouth tone, several
cores and the graphics card, built-in impulse responses, MIDI buttons, the dataset sampler, the 3D Saal as a
section of its own (hands, arranging, built-in rooms, light, illusions, audience), factory set 17 (160-175),
and the verification run of 15 November, in which the failures of 1 November no longer occur.

New in version 13: the Verlauf (automation over time: lanes, shapes, clocks and tempo lane, markers and A-B,
variation per pass, gestures that spread through the room, timetables, pieces and the six that come with Rysum),
Konzert (playing for listeners on other computers, the listener app), the Vox humana (a neural stop from
NeuralRysum v2, and the input playing the organ), the output stage Summe (EQ, compressor), effects per stop and
the speed spread, long reverbs that ring out and the dispersion check, light that follows the wind, AmbiX output
turned with the head, walking up to x8, faster reflections, the dataset as rendered, and the run of 18 November.

New in version 14: Appendix A, a mathematical reference that goes through the signal processing function by
function (tuning, encoder, motion and layouts, the voice, degradation, partitioned convolution, the impulse-response
room and its rotation per pipe, MagLS, crosstalk cancellation, AllRAD, the simulated room, Lucier, the output stage,
modulation and the Verlauf, the living organ, spatial degradation, photographic operations, bodies and knocks, beats,
echoes, the grain cloud, the reference tone, the Vox humana's glue and the clocks of Konzert), read off commit bfc83f3.

New in version 17: how the pipes radiate (extent with a velvet-noise diffuse field, pattern and facing for the
direct sound and every early reflection, spin), doors that couple two Saele, and the room keyboard (places pull the
pipes, keys take walls out, the decay shortened by Sabine's law), in the sections on spatialisation and on the room.

New in version 16: the 360-degree frames drawn by the graphics card (PanoGpu, about 15 times faster than the
processor at 4K), and the video made by the plug-in itself: ffmpeg run on its own and the 360-degree and
spatial-audio boxes YouTube reads written into the file (SpatialMeta: spherical V1 and V2, SA3D), in the
section on the 3D view and in Appendix A.

New in version 15: the 360-degree recording of the 3D view (equirectangular video and first-order AmbiX, for
YouTube), in the section on the 3D view and in Appendix A, which also gains the walking camera (projection, light,
fog, walking, jumping) and the 360-degree camera (cube faces, resampling, seams, the audio as the clock).
