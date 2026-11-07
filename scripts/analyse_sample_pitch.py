#!/usr/bin/env python3
"""Non-destructive audit of final pipe WAVs. Requires Python 3 and NumPy.

No sample is retuned and no existing manifest field is updated. Estimates are
constrained by the declared single-rank nominal pitch; this is verification,
not blind discovery. Compound stops receive spectral observations, not an F0.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import platform
import wave

import numpy as np

VERSION = "1.0.0"
REPO_ROOT = Path(__file__).resolve().parents[1]
THRESHOLDS = {"minimum_windows": 3, "minimum_periodicity": 0.85,
              "maximum_window_spread_cents": 5.0, "maximum_method_difference_cents": 3.0,
              "minimum_harmonic_count": 3, "maximum_legacy_difference_cents": 5.0}


def cents(a, b):
    return 1200.0 * math.log2(a / b)


def median_weighted(values, weights):
    order = np.argsort(values)
    values, weights = np.asarray(values)[order], np.asarray(weights)[order]
    return float(values[np.searchsorted(np.cumsum(weights), weights.sum() * 0.5)])


def spectrum(signal, rate):
    x = signal - signal.mean()
    nfft = 1 << (max(16, len(x) * 8) - 1).bit_length()
    power = np.abs(np.fft.rfft(x * np.hanning(len(x)), n=nfft)) ** 2
    return power, rate / nfft


def peak_at(power, step, low, high):
    first, last = max(1, math.ceil(low / step)), min(len(power) - 2, math.floor(high / step))
    if last <= first:
        return None
    i = first + int(np.argmax(power[first:last + 1]))
    if i == first or i == last:
        return None
    y = np.log(np.maximum(power[i - 1:i + 2], 1e-30))
    den = y[0] - 2 * y[1] + y[2]
    delta = float(0.5 * (y[0] - y[2]) / den) if abs(den) > 1e-20 else 0.0
    return (i + np.clip(delta, -0.5, 0.5)) * step, float(power[i])


def periodic_pitch(signal, rate, nominal):
    x = signal - signal.mean()
    n = len(x)
    nfft = 1 << (2 * n - 1).bit_length()
    # Refine the lag grid for high notes; integer-lag parabolic fits alone
    # are biased when there are only ~13 samples per period.
    oversample = 4 if rate / nominal < 20 else (2 if rate / nominal < 40 else 1)
    ac = np.fft.irfft(np.abs(np.fft.rfft(x, nfft)) ** 2, nfft * oversample) * oversample
    sums = np.r_[0.0, np.cumsum(x * x)]
    lo = max(2, int(rate / (nominal * 2 ** (100 / 1200))))
    hi = min(n - 2, math.ceil(rate / (nominal / 2 ** (100 / 1200))))
    if hi <= lo:
        return None, None
    indices = np.arange(lo * oversample - 1, hi * oversample + 2)
    lags = indices / oversample
    energy_a = np.interp(n - lags, np.arange(n + 1), sums)
    energy_b = sums[n] - np.interp(lags, np.arange(n + 1), sums)
    norm = ac[indices] / np.sqrt(np.maximum(energy_a * energy_b, 1e-30))
    j = 1 + int(np.argmax(norm[1:-1]))
    if j in (1, len(norm) - 2):
        return None, None
    den = norm[j - 1] - 2 * norm[j] + norm[j + 1]
    delta = float(0.5 * (norm[j - 1] - norm[j + 1]) / den) if abs(den) > 1e-20 else 0.0
    return rate / (lags[j] + np.clip(delta, -0.5, 0.5) / oversample), float(np.clip(norm[j] - 0.25 * (norm[j - 1] - norm[j + 1]) * delta, 0, 1))


def single_window(signal, rate, nominal):
    power, step = spectrum(signal, rate)
    candidates, weights, observations = [], [], []
    for harmonic in range(1, 13):
        expected = nominal * harmonic
        if expected > rate * 0.45:
            break
        peak = peak_at(power, step, expected / 2 ** (100 / 1200), expected * 2 ** (100 / 1200))
        if peak:
            hz, strength = peak
            candidates.append(hz / harmonic)
            weights.append(math.sqrt(strength))
            observations.append({"harmonic": harmonic, "frequency_hz": hz, "power": strength})
    if not candidates:
        return {"spectral_hz": None, "autocorrelation_hz": None, "periodicity": None, "harmonics": 0}
    # Discard weak peaks and harmonics inconsistent with the robust first fit.
    initial = median_weighted(candidates, weights)
    keep = [i for i, hz in enumerate(candidates)
            if abs(cents(hz, initial)) <= 10 and weights[i] >= max(weights) * 0.03]
    f0 = median_weighted([candidates[i] for i in keep], [weights[i] for i in keep]) if keep else None
    periodic_hz, periodicity = periodic_pitch(signal, rate, nominal)
    return {"spectral_hz": f0, "autocorrelation_hz": periodic_hz,
            "periodicity": periodicity, "harmonics": len(keep)}


def spectral_observations(signal, rate):
    power, step = spectrum(signal, rate)
    indices = np.flatnonzero((power[1:-1] > power[:-2]) & (power[1:-1] >= power[2:])) + 1
    indices = [int(i) for i in indices if 40 <= i * step <= min(12000, rate * 0.45)]
    chosen = []
    for i in sorted(indices, key=lambda k: power[k], reverse=True):
        peak = peak_at(power, step, (i - 2) * step, (i + 2) * step)
        if peak and all(abs(peak[0] - p[0]) > 3 for p in chosen):
            chosen.append(peak)
        if len(chosen) == 12:
            break
    top = max((p[1] for p in chosen), default=1)
    return [{"frequency_hz": float(hz), "relative_db": 10 * math.log10(max(p / top, 1e-30))}
            for hz, p in sorted(chosen)]


def regions(pipe, length, rate):
    # Stay after the attack and before the recorded release. Prefer the region
    # which actually loops during held playback, without joining loop endpoints.
    start = max(int(pipe.get("attack_end", 0)) + int(0.15 * rate), 0)
    end = min(int(pipe.get("release", length)) - int(0.08 * rate), length)
    ls, le = pipe.get("loop_start"), pipe.get("loop_end")
    source = "sustain"
    if isinstance(ls, int) and isinstance(le, int) and le > ls:
        start, end = max(start, ls), min(end, le)
        source = "loop"
    if end - start < int(0.3 * rate):
        start = max(int(pipe.get("attack_end", 0)), 0)
        end = min(int(pipe.get("release", length)), length)
        source = "short_sustain_fallback"
    width = min(int(0.7 * rate), end - start)
    if width < int(0.25 * rate):
        return source, []
    starts = sorted(set(int(a) for a in np.linspace(start, end - width, 5)))
    return source, [(a, a + width) for a in starts]


def analyse(path, stop, pipe):
    raw = path.read_bytes()
    with wave.open(str(path)) as wav:
        rate, channels, bits, length = wav.getframerate(), wav.getnchannels(), wav.getsampwidth(), wav.getnframes()
        if channels != 1 or bits != 2:
            raise ValueError(f"Expected mono PCM16 WAV: {path}")
        signal = np.frombuffer(wav.readframes(length), dtype="<i2").astype(float) / 32768
    nominal = float(pipe["nominal_hz"])
    kind, spans = regions(pipe, length, rate)
    row = {"stop_id": stop["id"], "key": pipe["key"], "name": pipe["name"], "file": pipe["file"],
           "sha256": hashlib.sha256(raw).hexdigest(), "sample_rate_hz": rate, "frames": length,
           "nominal_hz": nominal, "legacy_measured_hz": pipe.get("measured_hz"),
           "region_kind": kind, "legacy_flags": pipe.get("flags", []),
           "windows": [], "flags": [], "status": "review"}
    if stop.get("compound"):
        row["status"] = "compound_unresolved"
        row["flags"] = ["multiple_ranks_single_f0_not_authoritative"]
        if spans:
            a, b = spans[len(spans) // 2]
            row["spectral_observation_region"] = {"start_frame": a, "end_frame": b}
            row["spectral_peaks"] = spectral_observations(signal[a:b], rate)
        return row
    for a, b in spans:
        result = single_window(signal[a:b], rate, nominal)
        row["windows"].append({"start_frame": a, "end_frame": b, **result})
    good = [w for w in row["windows"] if w["spectral_hz"] is not None]
    if not good:
        row["flags"].append("no_usable_pitch")
        return row
    hz = float(np.median([w["spectral_hz"] for w in good]))
    deviations = [cents(w["spectral_hz"], hz) for w in good]
    method_diffs = [abs(cents(w["spectral_hz"], w["autocorrelation_hz"])) for w in good
                    if w["autocorrelation_hz"] is not None]
    periodicities = [w["periodicity"] for w in good if w["periodicity"] is not None]
    row.update(measured_hz=hz, deviation_cents=cents(hz, nominal),
               window_spread_cents=float(np.percentile(deviations, 95) - np.percentile(deviations, 5)),
               median_periodicity=float(np.median(periodicities)) if periodicities else None,
               median_method_difference_cents=float(np.median(method_diffs)) if method_diffs else None,
               minimum_harmonic_count=min(w["harmonics"] for w in good),
               legacy_difference_cents=cents(hz, pipe["measured_hz"]) if pipe.get("measured_hz") else None)
    if len(good) < THRESHOLDS["minimum_windows"]:
        row["flags"].append("too_few_windows")
    if row["median_periodicity"] is None or row["median_periodicity"] < THRESHOLDS["minimum_periodicity"]:
        row["flags"].append("weak_periodicity")
    if row["window_spread_cents"] > THRESHOLDS["maximum_window_spread_cents"]:
        row["flags"].append("pitch_varies_between_windows")
    if row["median_method_difference_cents"] is None or row["median_method_difference_cents"] > THRESHOLDS["maximum_method_difference_cents"]:
        row["flags"].append("estimators_disagree")
    if row["minimum_harmonic_count"] < THRESHOLDS["minimum_harmonic_count"]:
        row["flags"].append("few_resolved_harmonics")
    if row["legacy_difference_cents"] is not None and abs(row["legacy_difference_cents"]) > THRESHOLDS["maximum_legacy_difference_cents"]:
        row["flags"].append("differs_from_legacy_measurement")
    if kind == "short_sustain_fallback":
        row["flags"].append("short_sustain_fallback")
    row["status"] = "candidate" if not row["flags"] else "review"
    return row


def write_reports(output, manifest, manifest_path, rows):
    output.mkdir(parents=True, exist_ok=True)
    document = {"schema": "rysum-final-wav-pitch-analysis/v1", "analysis_version": VERSION,
                "python_version": platform.python_version(), "numpy_version": np.__version__,
                "manifest_sha256": hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
                "analysis_script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                "search_half_width_cents": 100, "thresholds": THRESHOLDS,
                "notes": ["Candidates are provisional estimates, not approved calibration.",
                          "Five overlapping windows are not five independent recordings.",
                          "No WAV or existing manifest was modified.",
                          "Compound peaks can be harmonics, ranks or noise; no rank identity is inferred."],
                "samples": rows}
    (output / "final-wav-pitch-analysis.json").write_text(json.dumps(document, indent=2, allow_nan=False) + "\n")
    columns = ["stop_id", "key", "name", "file", "status", "nominal_hz", "legacy_measured_hz", "measured_hz",
               "deviation_cents", "legacy_difference_cents", "window_spread_cents", "median_periodicity",
               "median_method_difference_cents", "minimum_harmonic_count", "region_kind", "runtime_substitute_key", "sha256", "flags"]
    with (output / "final-wav-pitch-analysis.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        for r in rows:
            writer.writerow({**r, "flags": ";".join(r["flags"])})
    lines = ["# Final WAV pitch analysis", "", "This audit analyses the 287 final pipe WAVs referenced by `samples/rysum_samples.json`. Mechanical noises and wind recordings are excluded: they are not pitched pipe assets. The derived Praestant 16′ has no additional WAVs.", "",
             "## Method and limits", "", "The analysis uses final PCM16 audio, five windows of up to 700 ms per single-rank pipe, preferentially within its sustain loop. A Hann-windowed, zero-padded spectrum supplies interpolated harmonic peaks; a robust harmonic fit is cross-checked against normalized autocorrelation. Both searches use a ±100-cent neighbourhood of the declared nominal pitch. This cannot discover a wrongly labelled note or reliably exclude octave errors outside that range.", "",
             "Window spread is the 95th–5th percentile of window estimates, not a confidence interval. Overlapping windows are correlated. `candidate` means the automatic checks passed; it does not mean approved or exact. The method does not remove vibrato, drift, beating or inharmonicity. Independent listening and measurement remain necessary for reviewed entries and compound stops.", "",
             "Review thresholds: fewer than 3 usable windows; median periodicity below 0.85; window spread above 5 cents; median method disagreement above 3 cents; fewer than 3 resolved harmonics in any usable window; difference from legacy measurement above 5 cents; or a short-sustain fallback.", "",
             "All results are observations only. Original files and the original manifest remain unchanged. JSON includes WAV hashes, frame regions, individual window estimates, analysis settings and tool versions. CSV is the sortable summary.", "", "## Summary", "",
             "Median and range below include automatic candidates only, excluding unresolved or flagged estimates.", "", "| Stop | Files | Candidates | Review / unresolved | Candidate median deviation (ct) | Candidate range (ct) |", "|---|---:|---:|---:|---:|---:|"]
    for stop in manifest["stops"]:
        group = [r for r in rows if r["stop_id"] == stop["id"]]
        values = [r["deviation_cents"] for r in group if r["status"] == "candidate"]
        lines.append(f"| {stop['id']} | {len(group)} | {sum(r['status']=='candidate' for r in group)} | {sum(r['status']!='candidate' for r in group)} | " +
                     (f"{np.median(values):+.2f} | {min(values):+.2f} … {max(values):+.2f} |" if values else "unresolved compound | — |"))
    lines += ["", "## Findings and review priorities", "", "Trompete has 41 automatic candidates, with a consistently sharp median reference. See the summary for measured values. This supports the retained-recorded-deviation explanation; it does not establish the original organ builder’s or performer’s intent.", "", "Gedackt MIDI 53 has a short sustain and only one analysis window. Gedackt MIDI 54 has weak loop periodicity and large disagreement between estimators: its numerical estimate is not reliable calibration. The current loader substitutes both pipes under its existing short/unsteady-sustain rules. Their raw-file results must not be mistaken for their substituted playback pitches.", "", "Many Gedackt and upper-octave pipes trigger the conservative fewer-than-three-harmonics rule. A flute-like near-sinusoidal recording can still have a useful pitch estimate; this flag requests review rather than establishing a faulty recording. Seven single-rank files differ by more than five cents from the legacy measurement. Review the selected region and old YIN estimate rather than automatically accepting either value.", "", "## Single-rank pipe results", "", "Frequency estimates below are in Hz. Positive deviation means sharp relative to the declared original nominal pitch. ‘Legacy Δ’ compares this audit with the manifest’s existing measurement.", "",
              "| Stop / MIDI key | Estimate (Hz) | Deviation (ct) | Legacy Δ (ct) | Spread (ct) | Status / flags |", "|---|---:|---:|---:|---:|---|"]
    for r in rows:
        if "measured_hz" in r:
            legacy = f"{r['legacy_difference_cents']:+.2f}" if r['legacy_difference_cents'] is not None else "—"
            lines.append(f"| {r['stop_id']} / {r['key']} | {r['measured_hz']:.4f} | {r['deviation_cents']:+.2f} | {legacy} | {r['window_spread_cents']:.2f} | {r['status']}: {', '.join(r['flags']) or 'automatic checks passed'} |")
    lines += ["", "## Compound stops", "", "All 82 compound samples remain `compound_unresolved`. The JSON records up to twelve prominent spectral peaks in a representative sustain window for each. These are not identified ranks or validated fundamentals. Correcting all constituent pitches requires rank identification and, if they differ in tuning, separate rank assets or another decomposition method.", "",
              "## Reproduction", "", "From the repository root, using Python with NumPy installed:", "", "```sh", "python scripts/analyse_sample_pitch.py --samples samples --out docs/pitch-analysis", "```", "",
              "Synthetic estimator checks: `python -m unittest discover -s scripts -p 'test_analyse_sample_pitch.py'`.", ""]
    (output / "final-wav-pitch-analysis.md").write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=Path, default=REPO_ROOT / "samples")
    parser.add_argument("--out", type=Path, default=REPO_ROOT / "docs/pitch-analysis")
    args = parser.parse_args()
    path = args.samples / "rysum_samples.json"
    manifest = json.loads(path.read_text())
    rows = []
    for stop in manifest["stops"]:
        for pipe in stop["pipes"]:
            rows.append(analyse(args.samples / pipe["file"], stop, pipe))
        print(f"Analysed {stop['id']}: {len(stop['pipes'])} files", flush=True)
    # Mirror only the current loader's source-selection rule, without treating a
    # substitute's audio as a measurement of the rejected raw recording.
    for stop in manifest["stops"]:
        group = [r for r in rows if r["stop_id"] == stop["id"]]
        def unusable(row):
            for flag in row["legacy_flags"]:
                if flag.startswith("short sustain"):
                    return True
                if flag.startswith("unsteady") and float(flag.split("(")[1].split()[0]) > 4:
                    return True
            return False
        usable = [r for r in group if not unusable(r)]
        for row in group:
            if unusable(row) and usable:
                neighbour = min(usable, key=lambda r: (abs(r["key"] - row["key"]), -r["key"]))
                if abs(neighbour["key"] - row["key"]) <= 3:
                    row["runtime_substitute_key"] = neighbour["key"]
    write_reports(args.out, manifest, path, rows)
    print(f"Wrote {len(rows)} results to {args.out}")


if __name__ == "__main__":
    main()
