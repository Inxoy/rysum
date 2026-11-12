"""Prepare the measurements from 30 October 2026 for the sample library.

    python scripts/process_oct30.py                  # october30_samples/ -> rysum-samples/
    python scripts/process_oct30.py --plots img_dir  # also write analysis plots

Inputs (stereo, 48 kHz):
  wind.wav    blower / wind noise. From about 2.2 s to 15.3 s a technical interference is on the
              recording: a comb of narrow lines on multiples of 100 / 200 Hz (600 Hz ... >8 kHz, like
              the buzz of a DECT / GSM phone near the microphones) that also lifts the broadband level
              above 300 Hz by 3-7 dB. Only the stretches without it are used; they are joined with
              crossfades into one seamless loop, level-matched to the wind loop it replaces.
  IRs.wav     hand claps in the room, one after another. Every clap is measured (dynamic range down
              to the noise floor, straightness of the decay, clipping, leftovers of the previous clap,
              room for the full decay before the next one); the best ones are cut from the clap to
              where the decay meets the noise floor, faded out there and written as stereo IRs.
  clicks.wav  key clicks, pressed and released in a steady rhythm (short gap = key held, long gap =
              pause). Paired into key-down / key-up like the old recording, junk is rejected (clipped,
              overlapping, too quiet, inside the handling noise at the start), level-matched to the
              existing clicks.

Outputs: rysum-samples/noise/wind/wind_oct30.wav, rysum-samples/irs/*.wav,
rysum-samples/noise/clicks_oct30/*.wav, and the entries in rysum-samples/rysum_samples.json
(wind first = the one the plugin plays; 'irs' = built-in impulse responses, the first is the default;
the new clicks are appended to 'clicks'). Running it again replaces its own entries only.
"""
import argparse
import json
import math
import os
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import signal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rysum_preprocess as rp  # noqa: E402  (shared envelope / onset / mono helpers)

REPO_ROOT = Path(__file__).resolve().parents[1]
TAG = 'oct30'


def rms_db(y):
    return float(10 * np.log10(np.mean(np.asarray(y, dtype=np.float64) ** 2) + 1e-20))


# ---------------------------------------------------------------------------
# wind
# ---------------------------------------------------------------------------
def interference_ratio(m, sr, hop=480):
    """per hop: energy on the 200 Hz comb (400 Hz .. 8 kHz) against the bins right next to it, in dB.
    Clean wind ~0 dB, the interference 10-22 dB."""
    f, t, S = signal.stft(m, sr, nperseg=4096, noverlap=4096 - hop, boundary=None)
    P = np.abs(S) ** 2
    df = f[1] - f[0]
    on = [int(round(k * 200 / df)) for k in range(2, 40)]
    nb = [i + o for i in on for o in (-6, -5, 5, 6)]
    r = 10 * np.log10((P[on].sum(0) + 1e-30) / (P[nb].sum(0) / 4 + 1e-30))
    return t, r


def clean_runs(t, r, thr=5.0, margin=0.15, min_len=0.5):
    """stretches where the comb (averaged over 100 ms) stays below thr, shrunk by margin on both
    sides. Single frames of plain noise reach ~7 dB; the interference sits at 10-22 dB."""
    bad = np.convolve(r, np.ones(10) / 10, 'same') > thr
    runs, s = [], None
    for i, b in enumerate(np.r_[bad, True]):
        if not b and s is None:
            s = i
        elif b and s is not None:
            a, e = t[s] + margin, t[i - 1] - margin
            if e - a >= min_len:
                runs.append((a, e))
            s = None
    return runs


def make_wind(raw, out, target_db, plots=None):
    x, sr = sf.read(os.path.join(raw, 'wind.wav'), dtype='float64', always_2d=True)
    m = x.mean(1)
    t, r = interference_ratio(m, sr)
    runs = clean_runs(t, r)
    runs = [(max(a, 0.05), min(e, len(m) / sr - 0.05)) for a, e in runs]
    mono = x.mean(1) * math.sqrt(2)          # as the old wind loops (uncorrelated parts add up)
    xf = int(0.08 * sr)
    w = np.sqrt(np.linspace(0, 1, xf))       # equal power for uncorrelated noise
    y = None
    for a, e in runs:
        piece = mono[int(a * sr):int(e * sr)]
        if y is None:
            y = piece.copy()
        else:
            y = np.concatenate([y[:-xf], y[-xf:] * w[::-1] + piece[:xf] * w, piece[xf:]])
    # seamless loop: the end crossfades into the start
    loop = y[xf:].copy()
    loop[-xf:] = loop[-xf:] * w[::-1] + y[:xf] * w
    # rumble below the blower's range (handling, traffic) out, gently
    sos = signal.butter(2, 25, 'highpass', fs=sr, output='sos')
    loop = signal.sosfiltfilt(sos, np.r_[loop, loop, loop])[len(loop):2 * len(loop)]
    raw_db = rms_db(loop)
    gain_db = (target_db - raw_db) if target_db is not None else 0.0
    loop *= 10 ** (gain_db / 20)
    os.makedirs(os.path.join(out, 'noise', 'wind'), exist_ok=True)
    fn = f'noise/wind/wind_{TAG}.wav'
    sf.write(os.path.join(out, fn), loop.astype(np.float32), sr, subtype='PCM_16')
    # how much of the comb is left (should be ~0 dB)
    _, r2 = interference_ratio(np.r_[loop, loop], sr)
    print(f'[wind] clean stretches {[(round(a, 2), round(b, 2)) for a, b in runs]} -> '
          f'{len(loop) / sr:.2f} s loop, comb {np.median(r[(t > runs[0][1] + 1) & (t < runs[-1][0] - 1)]):.1f} dB '
          f'in the rejected part, {np.median(r2):.1f} dB in the loop, gain {gain_db:+.1f} dB')
    if plots:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(2, 1, figsize=(14, 6))
        ax[0].plot(t, r, lw=.6)
        for a, e in runs:
            ax[0].axvspan(a, e, color='g', alpha=.2)
        ax[0].set_ylabel('comb / neighbours (dB)')
        ax[0].set_title('wind.wav: interference (green = used)')
        for seg, lab in [(m[int(4 * sr):int(14 * sr)], 'interference part'), (loop, 'loop (before gain)')]:
            f, P = signal.welch(seg / (10 ** (gain_db / 20) if lab.startswith('loop') else 1), sr, nperseg=8192)
            ax[1].semilogx(f, 10 * np.log10(P + 1e-20), lw=.6, label=lab)
        ax[1].set_xlim(20, 24000)
        ax[1].legend()
        plt.tight_layout()
        plt.savefig(os.path.join(plots, 'oct30_wind.png'), dpi=70)
        plt.close()
    return dict(file=fn, length=len(loop), loop=True, level_db=round(rms_db(loop), 1),
                source='october30_samples/wind.wav', source_times=[[round(a, 2), round(e, 2)] for a, e in runs],
                note='technical interference (100/200 Hz comb) between the used stretches left out')


# ---------------------------------------------------------------------------
# impulse responses from claps
# ---------------------------------------------------------------------------
FR = 0.005  # envelope frame (s)


def env(m, sr):
    n = int(FR * sr)
    k = len(m) // n
    e = (m[:k * n].reshape(k, n) ** 2).mean(1)
    return 10 * np.log10(e + 1e-20)


def find_claps(m, sr, e):
    """sharp onsets: the level jumps > 25 dB above the preceding 100 ms within 10 ms"""
    floor = np.percentile(e, 10)
    ons = []
    i = 20
    while i < len(e) - 2:
        pre = np.median(e[i - 20:i])
        if e[i] > pre + 25 and e[i] > floor + 35:
            j = i - 1
            while j > i - 5 and e[j] > pre + 6:
                j -= 1
            ons.append(j + 1)
            i += int(0.4 / FR)
        else:
            i += 1
    return floor, ons


def analyse_clap(x, m, sr, e, o, nxt):
    n = int(FR * sr)
    a0, a1 = o * n, (o + 4) * n
    pk_i = a0 + int(np.argmax(np.abs(m[a0:a1])))
    pk = abs(m[pk_i])
    # sample onset: first sample above 20 % of the peak, 1 ms pre-roll
    s = a0
    while s < pk_i and abs(m[s]) < 0.2 * pk:
        s += 1
    start = max(0, s - int(0.001 * sr))
    stop = (nxt * n if nxt is not None else len(m)) - int(0.02 * sr)
    seg_e = e[o:stop // n]
    # noise floor: the quietest 300 ms of the decay (before the next clap)
    win = int(0.3 / FR)
    if len(seg_e) < 2 * win:
        return None
    sm = np.convolve(seg_e, np.ones(win) / win, 'valid')
    floor = float(sm.min())
    peak_db = float(seg_e[:4].max())
    # decay straightness and T60 from peak-5 dB to floor+10 dB (smoothed 100 ms), up to where the
    # smoothed level first gets near the floor
    sm2 = np.convolve(seg_e, np.ones(20) / 20, 'same')
    tt = np.arange(len(seg_e)) * FR
    below = np.where(sm2[4:] < floor + 3)[0]
    first_floor = (below[0] + 4) if len(below) else len(seg_e)
    sel = (sm2 < peak_db - 5) & (sm2 > floor + 10) & (np.arange(len(seg_e)) < first_floor)
    if sel.sum() < 20:
        return None
    k, b = np.polyfit(tt[sel], sm2[sel], 1)
    pred = k * tt[sel] + b
    r2 = 1 - np.sum((sm2[sel] - pred) ** 2) / np.sum((sm2[sel] - sm2[sel].mean()) ** 2)
    # end: where the fitted decay reaches floor + 3 dB (a single dip of the noise must not cut it short)
    end_f = int((floor + 3 - b) / k / FR) if k < 0 else first_floor
    end_f = int(np.clip(end_f, first_floor, len(seg_e)))
    end = o * n + end_f * n
    pre = float(np.median(e[max(0, o - 12):o - 2])) if o > 14 else floor
    clipped = int((np.abs(x[start:end]) > 0.995).any(1).sum())
    return dict(start=start, end=end, t=start / sr, peak_db=round(peak_db, 1), floor_db=round(floor, 1),
                range_db=round(peak_db - floor, 1), t60=round(-60 / k, 2) if k < 0 else None,
                r2=round(float(r2), 4), pre_db=round(pre - floor, 1), clipped=clipped,
                room_after=round((stop - start) / sr, 2), decay_len=round((end - start) / sr, 2))


def make_irs(raw, out, count, plots=None):
    x, sr = sf.read(os.path.join(raw, 'IRs.wav'), dtype='float64', always_2d=True)
    m = x.mean(1)
    e = env(m, sr)
    _, ons = find_claps(m, sr, e)
    claps = []
    for i, o in enumerate(ons):
        c = analyse_clap(x, m, sr, e, o, ons[i + 1] if i + 1 < len(ons) else None)
        if c is None:
            continue
        reasons = []
        if c['clipped']:
            reasons.append(f"{c['clipped']} clipped samples")
        if c['pre_db'] > 6:
            reasons.append(f"previous decay still {c['pre_db']} dB above the floor")
        if c['r2'] < 0.97:
            reasons.append(f"uneven decay (r2 {c['r2']})")
        if c['decay_len'] > c['room_after'] - 0.1:
            reasons.append('next clap before the decay reached the floor')
        if c['range_db'] < 40:
            reasons.append(f"only {c['range_db']} dB above the noise")
        c['rejected'] = reasons
        claps.append(c)
    t60s = [c['t60'] for c in claps if not c['rejected'] and c['t60']]
    t60_med = float(np.median(t60s)) if t60s else None
    for c in claps:
        if not c['rejected'] and t60_med and abs(c['t60'] - t60_med) > 0.25 * t60_med:
            c['rejected'].append(f"T60 {c['t60']} s far from the median {t60_med:.2f} s")
    # best = the room's own decay measured the furthest above the noise (decay length / T60 x 60 dB;
    # the peak alone also counts the direct sound of the clap), with a straight decay
    for c in claps:
        c['decay_db'] = round(60 * c['decay_len'] / c['t60'], 1) if c['t60'] else 0.0
    good = sorted([c for c in claps if not c['rejected']], key=lambda c: -(c['decay_db'] + 50 * c['r2']))
    print(f'[irs] {len(claps)} claps, {len(good)} usable, median T60 {t60_med:.2f} s')
    for c in claps:
        print(f"   {c['t']:6.2f} s  range {c['range_db']:5.1f} dB  decay {c['decay_db']:5.1f} dB  T60 {c['t60']} s  r2 {c['r2']}  "
              f"{'OK' if not c['rejected'] else '; '.join(c['rejected'])}")
    os.makedirs(os.path.join(out, 'irs'), exist_ok=True)
    for f in os.listdir(os.path.join(out, 'irs')):
        if f.startswith(f'clap_{TAG}_'):
            os.remove(os.path.join(out, 'irs', f))
    sos = signal.butter(2, 30, 'highpass', fs=sr, output='sos')
    res = []
    for rank, c in enumerate(good[:count], 1):
        y = x[c['start']:c['end']].copy()
        y = signal.sosfilt(sos, y, axis=0)              # causal: nothing before the clap
        fo = min(int(0.3 * sr), len(y) // 3)            # the last bit sinks into the floor: fade
        y[-fo:] *= (0.5 + 0.5 * np.cos(np.linspace(0, np.pi, fo)))[:, None]
        y *= 0.5 / np.abs(y).max()                      # -6 dBFS peak (the plugin normalises energy)
        fn = f'irs/clap_{TAG}_{rank}.wav'
        sf.write(os.path.join(out, fn), y.astype(np.float32), sr, subtype='PCM_24')
        res.append(dict(file=fn, name=f'Klatschen {rank} (30.10.)', channels=2, length=len(y),
                        seconds=round(len(y) / sr, 2), t60=c['t60'], range_db=c['range_db'], decay_db=c['decay_db'],
                        source='october30_samples/IRs.wav', source_time=round(c['t'], 3)))
    if plots:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(16, 4))
        ax.plot(np.arange(len(e)) * FR, e, lw=.4, color='#888')
        chosen = {c['start'] for c in good[:count]}
        for c in claps:
            col = 'g' if c['start'] in chosen else ('#06c' if not c['rejected'] else 'r')
            ax.axvspan(c['start'] / sr, c['end'] / sr, color=col, alpha=.2)
        ax.set_title('IRs.wav: green = built in, blue = usable, red = rejected')
        plt.tight_layout()
        plt.savefig(os.path.join(plots, 'oct30_irs.png'), dpi=70)
        plt.close()
    return res, claps


# ---------------------------------------------------------------------------
# clicks
# ---------------------------------------------------------------------------
def make_clicks(raw, out, existing, skip_until=16.0):
    x, sr = sf.read(os.path.join(raw, 'clicks.wav'), dtype='float32', always_2d=True)
    m, _ = rp.to_mono(x)
    db = rp.env_db(m)
    floor, ons = rp.detect_events(db, sr)
    hop = rp.HOP
    evs = []
    for i, o in enumerate(ons):
        t = o * hop / sr
        nxt = ons[i + 1] if i + 1 < len(ons) else len(db)
        lim = min(nxt, o + int(2.5 * sr / hop))
        j, quiet = o + 1, 0
        while j < lim - 1:
            quiet = quiet + 1 if db[j] < floor + 3 else 0
            if quiet >= int(0.05 * sr / hop):
                break
            j += 1
        a = max(0, o * hop - int(0.005 * sr))
        b = j * hop
        ev = dict(a=a, b=b, t=a / sr, peak=float(db[o:j].max()) if j > o else float(db[o]))
        reasons = []
        if t < skip_until:
            reasons.append('handling noise at the start')
        if (np.abs(x[a:b]) > 0.995).any():
            reasons.append('clipped')
        # the room tail of the previous click (~1 s) is often still there: only a loud leftover counts
        if np.median(db[max(0, o - 8):max(1, o - 2)]) > floor + 12:
            reasons.append('inside the previous decay')
        if (nxt - o) * hop / sr < 0.3:
            reasons.append('another event right after')
        if ev['peak'] < floor + 18:
            reasons.append('too quiet')
        if (b - a) < int(0.03 * sr):
            reasons.append('too short')
        ev['rejected'] = reasons
        evs.append(ev)
    # pairing, as rysum_preprocess: a click followed 0.5-1.8 s later by another = down + up
    ok = [e for e in evs if not e['rejected']]
    k = 0
    while k < len(ok):
        if k + 1 < len(ok) and 0.5 <= ok[k + 1]['t'] - ok[k]['t'] <= 1.8:
            ok[k]['kind'], ok[k + 1]['kind'] = 'down', 'up'
            k += 2
        else:
            ok[k]['kind'] = 'other'
            k += 1
    # level: median peak of each kind = median peak of the existing clicks of that kind
    gains = {}
    for kind in ('down', 'up', 'other'):
        new = [20 * np.log10(np.abs(m[e['a']:e['b']]).max()) for e in ok if e['kind'] == kind]
        old = existing.get(kind) or existing.get('all')
        gains[kind] = (np.median(old) - np.median(new)) if new and old else 0.0
    sub = os.path.join(out, 'noise', f'clicks_{TAG}')
    os.makedirs(sub, exist_ok=True)
    for f in os.listdir(sub):
        os.remove(os.path.join(sub, f))
    res, counters = [], {}
    for e in ok:
        y = m[e['a']:e['b']].copy()
        fo = min(int(0.02 * sr), len(y) // 4)
        y[-fo:] *= np.linspace(1, 0, fo)
        y *= 10 ** (gains[e['kind']] / 20)
        if np.abs(y).max() > 0.99:
            y *= 0.99 / np.abs(y).max()
        counters[e['kind']] = counters.get(e['kind'], 0) + 1
        fn = f"noise/clicks_{TAG}/{e['kind']}_{counters[e['kind']]:02d}.wav"
        sf.write(os.path.join(out, fn), y, sr, subtype='PCM_16')
        res.append(dict(file=fn, kind=e['kind'], source_time=round(e['t'], 3), length=len(y),
                        peak_db=round(e['peak'] + gains[e['kind']], 1), source='october30_samples/clicks.wav'))
    rej = [e for e in evs if e['rejected']]
    why = {}
    for e in rej:
        why[e['rejected'][0]] = why.get(e['rejected'][0], 0) + 1
    print(f"[clicks] {len(evs)} events, {len(res)} kept ({counters}), rejected: {why}; "
          f"level {', '.join(f'{k} {v:+.1f} dB' for k, v in gains.items())}")
    return res


def existing_click_peaks(out, clicks):
    by = {}
    for c in clicks:
        p = os.path.join(out, c['file'])
        if TAG in c['file'] or not os.path.exists(p):
            continue
        y, _ = sf.read(p, always_2d=True)
        pk = 20 * np.log10(np.abs(y).max() + 1e-12)
        by.setdefault(c['kind'], []).append(pk)
        by.setdefault('all', []).append(pk)
    return by


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--raw', default=str(REPO_ROOT / 'october30_samples'))
    ap.add_argument('--out', default=str(REPO_ROOT / 'rysum-samples'))
    ap.add_argument('--irs', type=int, default=3, help='how many clap IRs to build in (default 3)')
    ap.add_argument('--plots', default=None, help='folder for analysis plots')
    args = ap.parse_args()
    if args.plots:
        os.makedirs(args.plots, exist_ok=True)
    jpath = os.path.join(args.out, 'rysum_samples.json')
    meta = json.load(open(jpath, encoding='utf-8'))
    noise = meta.setdefault('noise', {})

    # wind: level of the wind loop the plugin plays now (the first that exists)
    winds = [w for w in noise.get('wind', []) if TAG not in w['file']]
    cur = next((w for w in winds if os.path.exists(os.path.join(args.out, w['file']))), None)
    target = rms_db(sf.read(os.path.join(args.out, cur['file']))[0]) if cur else None
    noise['wind'] = [make_wind(args.raw, args.out, target, args.plots)] + winds

    irs, _ = make_irs(args.raw, args.out, args.irs, args.plots)
    meta['irs'] = irs + [i for i in meta.get('irs', []) if TAG not in i['file']]

    old = [c for c in noise.get('clicks', []) if TAG not in c['file']]
    noise['clicks'] = old + make_clicks(args.raw, args.out, existing_click_peaks(args.out, old))

    json.dump(meta, open(jpath, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
    print(f'-> {jpath}')


if __name__ == '__main__':
    main()
