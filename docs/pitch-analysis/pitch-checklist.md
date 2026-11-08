# Sample pitch verification checklist

Source: `final-wav-pitch-analysis.csv`. Frequencies are estimates from the existing audit, rounded to 0.001 Hz for convenient comparison; this does not imply that level of accuracy. No new analysis or calibration approval has been performed.

Open `pitch-checklist.csv` in a spreadsheet to enter your measurements in `checked_hz` and comments in `review_notes`. Check the raw WAV at its native sample rate, without tuning or pitch effects. The estimates describe the sustain region, preferably inside the loop, rather than the attack.

`candidate` means automatic checks passed; `review` means the estimate needs investigation. Compound stops have no authoritative single fundamental and are listed without a frequency. Runtime substitutions are identified because the app may play a different source file from the one being checked.


## praestant

| Checked | File | Estimated Hz | Status / concerns |
|---|---|---:|---|
| ☐ | `praestant/036_C.wav` | 66.420 | candidate |
| ☐ | `praestant/041_F.wav` | 88.692 | candidate |
| ☐ | `praestant/038_D.wav` | 74.242 | review: differs_from_legacy_measurement |
| ☐ | `praestant/043_G.wav` | 99.282 | candidate |
| ☐ | `praestant/040_E.wav` | 82.994 | candidate |
| ☐ | `praestant/045_A.wav` | 111.076 | candidate |
| ☐ | `praestant/046_Bb.wav` | 118.932 | review: differs_from_legacy_measurement |
| ☐ | `praestant/047_B.wav` | 123.982 | candidate |
| ☐ | `praestant/048_c.wav` | 132.865 | candidate |
| ☐ | `praestant/049_c#.wav` | 138.875 | candidate |
| ☐ | `praestant/050_d.wav` | 148.594 | candidate |
| ☐ | `praestant/051_eb.wav` | 158.912 | candidate |
| ☐ | `praestant/052_e.wav` | 166.181 | candidate |
| ☐ | `praestant/053_f.wav` | 177.962 | candidate |
| ☐ | `praestant/054_f#.wav` | 185.286 | candidate |
| ☐ | `praestant/055_g.wav` | 198.598 | candidate |
| ☐ | `praestant/056_g#.wav` | 207.608 | candidate |
| ☐ | `praestant/057_a.wav` | 221.887 | candidate |
| ☐ | `praestant/058_bb.wav` | 237.665 | candidate |
| ☐ | `praestant/059_b.wav` | 248.005 | candidate |
| ☐ | `praestant/060_cp.wav` | 265.505 | candidate |
| ☐ | `praestant/061_c#p.wav` | 277.340 | candidate |
| ☐ | `praestant/062_dp.wav` | 296.793 | candidate |
| ☐ | `praestant/063_ebp.wav` | 317.470 | candidate |
| ☐ | `praestant/064_ep.wav` | 332.077 | candidate |
| ☐ | `praestant/065_fp.wav` | 355.129 | candidate |
| ☐ | `praestant/066_f#p.wav` | 371.336 | candidate |
| ☐ | `praestant/067_gp.wav` | 397.450 | candidate |
| ☐ | `praestant/068_g#p.wav` | 414.496 | candidate |
| ☐ | `praestant/069_ap.wav` | 443.654 | review: few_resolved_harmonics |
| ☐ | `praestant/070_bbp.wav` | 475.533 | candidate |
| ☐ | `praestant/071_bp.wav` | 496.807 | candidate |
| ☐ | `praestant/072_cpp.wav` | 531.029 | candidate |
| ☐ | `praestant/073_c#pp.wav` | 554.494 | candidate |
| ☐ | `praestant/074_dpp.wav` | 592.840 | candidate |
| ☐ | `praestant/075_ebpp.wav` | 635.379 | review: few_resolved_harmonics |
| ☐ | `praestant/076_epp.wav` | 663.554 | candidate |
| ☐ | `praestant/077_fpp.wav` | 710.856 | candidate |
| ☐ | `praestant/078_f#pp.wav` | 742.094 | candidate |
| ☐ | `praestant/079_gpp.wav` | 793.862 | candidate |
| ☐ | `praestant/081_app.wav` | 887.484 | candidate |
## gedackt

| Checked | File | Estimated Hz | Status / concerns |
|---|---|---:|---|
| ☐ | `gedackt/036_C.wav` | 66.268 | review: few_resolved_harmonics;differs_from_legacy_measurement |
| ☐ | `gedackt/041_F.wav` | 88.862 | review: few_resolved_harmonics;differs_from_legacy_measurement |
| ☐ | `gedackt/038_D.wav` | 74.130 | review: few_resolved_harmonics;differs_from_legacy_measurement |
| ☐ | `gedackt/043_G.wav` | 99.294 | candidate |
| ☐ | `gedackt/040_E.wav` | 82.918 | candidate |
| ☐ | `gedackt/045_A.wav` | 111.068 | review: few_resolved_harmonics |
| ☐ | `gedackt/046_Bb.wav` | 118.741 | candidate |
| ☐ | `gedackt/047_B.wav` | 124.185 | review: few_resolved_harmonics |
| ☐ | `gedackt/048_c.wav` | 132.638 | review: few_resolved_harmonics |
| ☐ | `gedackt/049_c#.wav` | 138.666 | review: few_resolved_harmonics |
| ☐ | `gedackt/050_d.wav` | 148.655 | candidate |
| ☐ | `gedackt/051_eb.wav` | 158.815 | review: few_resolved_harmonics |
| ☐ | `gedackt/052_e.wav` | 165.867 | review: few_resolved_harmonics |
| ☐ | `gedackt/053_f.wav` | 177.171 | review: too_few_windows;estimators_disagree;few_resolved_harmonics;differs_from_legacy_measurement;short_sustain_fallback; runtime substitutes key 52 |
| ☐ | `gedackt/054_f#.wav` | 196.317 | review: too_few_windows;weak_periodicity;estimators_disagree;few_resolved_harmonics;differs_from_legacy_measurement; runtime substitutes key 55 |
| ☐ | `gedackt/055_g.wav` | 198.363 | candidate |
| ☐ | `gedackt/056_g#.wav` | 207.820 | review: few_resolved_harmonics |
| ☐ | `gedackt/057_a.wav` | 222.051 | candidate |
| ☐ | `gedackt/058_bb.wav` | 237.345 | review: few_resolved_harmonics |
| ☐ | `gedackt/059_b.wav` | 248.206 | candidate |
| ☐ | `gedackt/060_cp.wav` | 265.713 | review: few_resolved_harmonics |
| ☐ | `gedackt/061_c#p.wav` | 277.373 | candidate |
| ☐ | `gedackt/062_dp.wav` | 296.956 | candidate |
| ☐ | `gedackt/063_ebp.wav` | 318.188 | review: few_resolved_harmonics |
| ☐ | `gedackt/064_ep.wav` | 331.829 | candidate |
| ☐ | `gedackt/065_fp.wav` | 355.505 | review: few_resolved_harmonics |
| ☐ | `gedackt/066_f#p.wav` | 371.667 | review: few_resolved_harmonics |
| ☐ | `gedackt/067_gp.wav` | 397.393 | review: few_resolved_harmonics |
| ☐ | `gedackt/068_g#p.wav` | 415.474 | review: few_resolved_harmonics |
| ☐ | `gedackt/069_ap.wav` | 443.971 | review: few_resolved_harmonics |
| ☐ | `gedackt/070_bbp.wav` | 475.076 | review: few_resolved_harmonics |
| ☐ | `gedackt/071_bp.wav` | 497.424 | review: few_resolved_harmonics |
| ☐ | `gedackt/072_cpp.wav` | 530.550 | review: few_resolved_harmonics |
| ☐ | `gedackt/073_c#pp.wav` | 555.364 | review: few_resolved_harmonics |
| ☐ | `gedackt/074_dpp.wav` | 593.690 | review: few_resolved_harmonics |
| ☐ | `gedackt/075_ebpp.wav` | 636.093 | review: few_resolved_harmonics |
| ☐ | `gedackt/076_epp.wav` | 662.829 | candidate |
| ☐ | `gedackt/077_fpp.wav` | 709.976 | review: few_resolved_harmonics |
| ☐ | `gedackt/078_f#pp.wav` | 743.122 | review: few_resolved_harmonics |
| ☐ | `gedackt/079_gpp.wav` | 794.908 | review: few_resolved_harmonics |
| ☐ | `gedackt/081_app.wav` | 888.060 | review: few_resolved_harmonics |
## oktave4

| Checked | File | Estimated Hz | Status / concerns |
|---|---|---:|---|
| ☐ | `oktave4/036_C.wav` | 133.074 | review: few_resolved_harmonics |
| ☐ | `oktave4/041_F.wav` | 177.489 | candidate |
| ☐ | `oktave4/038_D.wav` | 148.493 | candidate |
| ☐ | `oktave4/043_G.wav` | 198.687 | candidate |
| ☐ | `oktave4/040_E.wav` | 166.251 | candidate |
| ☐ | `oktave4/045_A.wav` | 222.157 | review: few_resolved_harmonics |
| ☐ | `oktave4/046_Bb.wav` | 237.810 | candidate |
| ☐ | `oktave4/047_B.wav` | 247.935 | candidate |
| ☐ | `oktave4/048_c.wav` | 265.802 | candidate |
| ☐ | `oktave4/049_c#.wav` | 277.530 | candidate |
| ☐ | `oktave4/050_d.wav` | 297.387 | candidate |
| ☐ | `oktave4/051_eb.wav` | 317.822 | candidate |
| ☐ | `oktave4/052_e.wav` | 332.322 | candidate |
| ☐ | `oktave4/053_f.wav` | 355.235 | candidate |
| ☐ | `oktave4/054_f#.wav` | 371.273 | candidate |
| ☐ | `oktave4/055_g.wav` | 397.393 | review: few_resolved_harmonics |
| ☐ | `oktave4/056_g#.wav` | 415.471 | candidate |
| ☐ | `oktave4/057_a.wav` | 443.987 | candidate |
| ☐ | `oktave4/058_bb.wav` | 475.595 | candidate |
| ☐ | `oktave4/059_b.wav` | 496.576 | review: few_resolved_harmonics |
| ☐ | `oktave4/060_cp.wav` | 531.919 | candidate |
| ☐ | `oktave4/061_c#p.wav` | 555.502 | candidate |
| ☐ | `oktave4/062_dp.wav` | 593.513 | review: few_resolved_harmonics |
| ☐ | `oktave4/063_ebp.wav` | 636.377 | candidate |
| ☐ | `oktave4/064_ep.wav` | 664.514 | review: few_resolved_harmonics |
| ☐ | `oktave4/065_fp.wav` | 710.842 | candidate |
| ☐ | `oktave4/066_f#p.wav` | 743.356 | candidate |
| ☐ | `oktave4/067_gp.wav` | 794.830 | candidate |
| ☐ | `oktave4/068_g#p.wav` | 830.120 | candidate |
| ☐ | `oktave4/069_ap.wav` | 887.622 | candidate |
| ☐ | `oktave4/070_bbp.wav` | 951.427 | review: few_resolved_harmonics |
| ☐ | `oktave4/071_bp.wav` | 993.289 | review: few_resolved_harmonics |
| ☐ | `oktave4/072_cpp.wav` | 1062.092 | review: few_resolved_harmonics |
| ☐ | `oktave4/073_c#pp.wav` | 1109.502 | review: few_resolved_harmonics |
| ☐ | `oktave4/074_dpp.wav` | 1186.907 | candidate |
| ☐ | `oktave4/075_ebpp.wav` | 1271.252 | candidate |
| ☐ | `oktave4/076_epp.wav` | 1327.603 | review: few_resolved_harmonics |
| ☐ | `oktave4/077_fpp.wav` | 1421.323 | review: few_resolved_harmonics |
| ☐ | `oktave4/078_f#pp.wav` | 1484.545 | review: few_resolved_harmonics |
| ☐ | `oktave4/079_gpp.wav` | 1587.652 | candidate |
| ☐ | `oktave4/081_app.wav` | 1777.031 | candidate |
## oktave2

| Checked | File | Estimated Hz | Status / concerns |
|---|---|---:|---|
| ☐ | `oktave2/036_C.wav` | 265.264 | candidate |
| ☐ | `oktave2/041_F.wav` | 354.590 | candidate |
| ☐ | `oktave2/038_D.wav` | 295.665 | candidate |
| ☐ | `oktave2/043_G.wav` | 397.696 | candidate |
| ☐ | `oktave2/040_E.wav` | 332.690 | candidate |
| ☐ | `oktave2/045_A.wav` | 444.008 | candidate |
| ☐ | `oktave2/046_Bb.wav` | 477.019 | review: few_resolved_harmonics |
| ☐ | `oktave2/047_B.wav` | 494.645 | review: few_resolved_harmonics |
| ☐ | `oktave2/048_c.wav` | 532.439 | candidate |
| ☐ | `oktave2/049_c#.wav` | 554.136 | candidate |
| ☐ | `oktave2/050_d.wav` | 593.889 | candidate |
| ☐ | `oktave2/051_eb.wav` | 634.244 | candidate |
| ☐ | `oktave2/052_e.wav` | 666.030 | review: few_resolved_harmonics |
| ☐ | `oktave2/053_f.wav` | 709.086 | candidate |
| ☐ | `oktave2/054_f#.wav` | 742.618 | review: few_resolved_harmonics |
| ☐ | `oktave2/055_g.wav` | 795.003 | candidate |
| ☐ | `oktave2/056_g#.wav` | 829.885 | candidate |
| ☐ | `oktave2/057_a.wav` | 887.808 | candidate |
| ☐ | `oktave2/058_bb.wav` | 950.564 | review: few_resolved_harmonics |
| ☐ | `oktave2/059_b.wav` | 992.649 | candidate |
| ☐ | `oktave2/060_cp.wav` | 1062.656 | review: few_resolved_harmonics |
| ☐ | `oktave2/061_c#p.wav` | 1108.494 | candidate |
| ☐ | `oktave2/062_dp.wav` | 1187.453 | candidate |
| ☐ | `oktave2/063_ebp.wav` | 1272.254 | review: few_resolved_harmonics |
| ☐ | `oktave2/064_ep.wav` | 1330.213 | candidate |
| ☐ | `oktave2/065_fp.wav` | 1420.692 | review: few_resolved_harmonics |
| ☐ | `oktave2/066_f#p.wav` | 1485.824 | review: few_resolved_harmonics |
| ☐ | `oktave2/067_gp.wav` | 1588.658 | candidate |
| ☐ | `oktave2/068_g#p.wav` | 1660.200 | candidate |
| ☐ | `oktave2/069_ap.wav` | 1772.777 | candidate |
| ☐ | `oktave2/070_bbp.wav` | 1901.815 | review: few_resolved_harmonics |
| ☐ | `oktave2/071_bp.wav` | 1989.671 | review: few_resolved_harmonics |
| ☐ | `oktave2/072_cpp.wav` | 2126.602 | review: few_resolved_harmonics |
| ☐ | `oktave2/073_c#pp.wav` | 2217.074 | review: few_resolved_harmonics |
| ☐ | `oktave2/074_dpp.wav` | 2366.871 | review: few_resolved_harmonics |
| ☐ | `oktave2/075_ebpp.wav` | 2535.830 | review: few_resolved_harmonics |
| ☐ | `oktave2/076_epp.wav` | 2649.411 | review: few_resolved_harmonics |
| ☐ | `oktave2/077_fpp.wav` | 2837.958 | review: few_resolved_harmonics |
| ☐ | `oktave2/078_f#pp.wav` | 2965.051 | review: few_resolved_harmonics |
| ☐ | `oktave2/079_gpp.wav` | 3190.339 | review: few_resolved_harmonics |
| ☐ | `oktave2/081_app.wav` | 3548.193 | review: few_resolved_harmonics |
## sesquialtera

| Checked | File | Estimated Hz | Status / concerns |
|---|---|---:|---|
| ☐ | `sesquialtera/036_C.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/041_F.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/038_D.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/043_G.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/040_E.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/045_A.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/046_Bb.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/047_B.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/048_c.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/049_c#.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/050_d.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/051_eb.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/052_e.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/053_f.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/054_f#.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/055_g.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/056_g#.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/057_a.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/058_bb.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/059_b.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/060_cp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/061_c#p.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/062_dp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/063_ebp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/064_ep.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/065_fp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/066_f#p.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/067_gp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/068_g#p.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/069_ap.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/070_bbp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/071_bp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/072_cpp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/073_c#pp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/074_dpp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/075_ebpp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/076_epp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/077_fpp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/078_f#pp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/079_gpp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `sesquialtera/081_app.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
## mixtur

| Checked | File | Estimated Hz | Status / concerns |
|---|---|---:|---|
| ☐ | `mixtur/036_C.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/041_F.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/038_D.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/043_G.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/040_E.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/045_A.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/046_Bb.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/047_B.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/048_c.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/049_c#.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/050_d.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/051_eb.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/052_e.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/053_f.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/054_f#.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/055_g.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/056_g#.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/057_a.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/058_bb.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/059_b.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/060_cp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/061_c#p.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/062_dp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/063_ebp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/064_ep.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/065_fp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/066_f#p.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/067_gp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/068_g#p.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/069_ap.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/070_bbp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/071_bp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/072_cpp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/073_c#pp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/074_dpp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/075_ebpp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/076_epp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/077_fpp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/078_f#pp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/079_gpp.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
| ☐ | `mixtur/081_app.wav` | — | compound_unresolved: multiple_ranks_single_f0_not_authoritative |
## trompete

| Checked | File | Estimated Hz | Status / concerns |
|---|---|---:|---|
| ☐ | `trompete/036_C.wav` | 67.288 | candidate |
| ☐ | `trompete/041_F.wav` | 89.273 | candidate |
| ☐ | `trompete/038_D.wav` | 75.179 | candidate |
| ☐ | `trompete/043_G.wav` | 99.847 | candidate |
| ☐ | `trompete/040_E.wav` | 83.949 | candidate |
| ☐ | `trompete/045_A.wav` | 111.705 | candidate |
| ☐ | `trompete/046_Bb.wav` | 118.851 | candidate |
| ☐ | `trompete/047_B.wav` | 124.651 | candidate |
| ☐ | `trompete/048_c.wav` | 134.109 | candidate |
| ☐ | `trompete/049_c#.wav` | 140.869 | candidate |
| ☐ | `trompete/050_d.wav` | 149.981 | candidate |
| ☐ | `trompete/051_eb.wav` | 160.272 | candidate |
| ☐ | `trompete/052_e.wav` | 167.715 | candidate |
| ☐ | `trompete/053_f.wav` | 179.174 | candidate |
| ☐ | `trompete/054_f#.wav` | 185.473 | candidate |
| ☐ | `trompete/055_g.wav` | 200.085 | candidate |
| ☐ | `trompete/056_g#.wav` | 209.401 | candidate |
| ☐ | `trompete/057_a.wav` | 223.956 | candidate |
| ☐ | `trompete/058_bb.wav` | 239.380 | candidate |
| ☐ | `trompete/059_b.wav` | 249.334 | candidate |
| ☐ | `trompete/060_cp.wav` | 268.790 | candidate |
| ☐ | `trompete/061_c#p.wav` | 280.287 | candidate |
| ☐ | `trompete/062_dp.wav` | 297.998 | candidate |
| ☐ | `trompete/063_ebp.wav` | 320.494 | candidate |
| ☐ | `trompete/064_ep.wav` | 333.749 | candidate |
| ☐ | `trompete/065_fp.wav` | 358.491 | candidate |
| ☐ | `trompete/066_f#p.wav` | 375.232 | candidate |
| ☐ | `trompete/067_gp.wav` | 399.869 | candidate |
| ☐ | `trompete/068_g#p.wav` | 418.487 | candidate |
| ☐ | `trompete/069_ap.wav` | 444.362 | candidate |
| ☐ | `trompete/070_bbp.wav` | 479.123 | candidate |
| ☐ | `trompete/071_bp.wav` | 504.300 | candidate |
| ☐ | `trompete/072_cpp.wav` | 534.926 | candidate |
| ☐ | `trompete/073_c#pp.wav` | 556.407 | candidate |
| ☐ | `trompete/074_dpp.wav` | 598.272 | candidate |
| ☐ | `trompete/075_ebpp.wav` | 640.358 | candidate |
| ☐ | `trompete/076_epp.wav` | 668.032 | candidate |
| ☐ | `trompete/077_fpp.wav` | 715.615 | candidate |
| ☐ | `trompete/078_f#pp.wav` | 750.073 | candidate |
| ☐ | `trompete/079_gpp.wav` | 802.059 | candidate |
| ☐ | `trompete/081_app.wav` | 897.410 | candidate |
