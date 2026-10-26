"""
Rysum organ – sample preprocessing
==================================

Turns the long raw recordings (one per stop, keys held with ~1 s of near-silence
between them) into one file per pipe plus a JSON description the plugin reads.

What it does differently from note_isolation.py
-----------------------------------------------
* No normalisation – every pipe keeps its real level (voicing).
* Full release tail – each pipe runs until the tail has sunk into the noise
  floor (or the next note starts), then a short fade.
* Fragments are merged / dropped – sustained notes are found with a
  hysteresis threshold relative to the file's noise floor; gaps < 0.35 s are
  bridged so quiet pipes don't break into pieces.
* Pitch is not "detected and believed" – every note is assigned to a physical
  key with a Viterbi alignment over the expected key order (short octave
  C/E – a'', no g#''). For compound stops (Sesquialtera, Mixtur) only order and
  pitch-class plausibility are used; the detected f0 is stored but ignored.
* Retakes: if a key was recorded several times the best take is chosen
  (long steady sustain, good loop, good pitch fit). All takes are listed in
  the report.
* Loop points are searched in the sustain (normalised cross-correlation),
  the key-up position is stored, so the plugin can loop and then play the
  real release.
* Mono first: each recording is turned into one mono track before anything
  else (per note, the two mics are time-aligned and summed; if they still
  cancel, the louder mic is used). All timing – note detection, onsets,
  release, loop points – is then computed on that mono track.
* Noise layer: key clicks (paired into key-down / key-up), stop-pull sounds
  and a wind/blower loop taken from the gaps between notes.
* Clean-up (per pipe, after cutting): a zero-phase high-pass at about half
  the pipe's fundamental removes blower rumble / sub information, and a
  spectral noise reduction with a noise print from the silent gaps of the same
  recording lowers the constant wind noise (moderately, so the pipe's own
  breath and chiff stay). The tail then ends where the cleaned release reaches
  the remaining noise and is faded out softly instead of being cut.
  --no-clean writes the samples as before.

Usage:  python rysum_preprocess.py            (run inside rysum_plugin/)
        python rysum_preprocess.py --raw raw_unprocessed_files --out samples
Requires: numpy, scipy, soundfile, librosa, matplotlib (for the report plot)
"""
import argparse, json, os, math
import numpy as np
import soundfile as sf
import librosa
from scipy.ndimage import uniform_filter1d, binary_closing
from scipy.signal import fftconvolve, butter, sosfiltfilt, stft, istft

# --------------------------------------------------------------------------
# Instrument
# --------------------------------------------------------------------------
A4 = 444.5                      # measured from the recordings
# quarter-comma meantone, cents above C (C, C#, D, Eb, E, F, F#, G, G#, A, Bb, B)
MEANTONE = [0.0, 76.0, 193.2, 310.3, 386.3, 503.4, 579.5, 696.6, 772.6, 889.7, 1006.8, 1082.9]
NAMES = ['C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'G#', 'A', 'Bb', 'B']

# physical keys left -> right, as MIDI note of the 8' pitch they sound.
# short octave: keys E F F# G G# A Bb B sound C F D G E A Bb B; no g#'' (MIDI 80)
PHYS_KEYS = [36, 41, 38, 43, 40, 45, 46, 47] + list(range(48, 80)) + [81]

def mt_freq(midi, a4=A4):
    return a4 * 2 ** ((midi // 12 - 5) + (MEANTONE[midi % 12] - MEANTONE[9]) / 1200)

def helmholtz(midi):
    o = midi // 12 - 1; n = NAMES[midi % 12]
    if o == 2: return n                       # C
    if o == 3: return n.lower()               # c
    return n.lower() + "'" * (o - 3)          # c', c''

# stops: sources = list of (file, t_start, t_end); mult = footage factor vs 8'
# compound = only pitch-class plausibility (intervals in cents above the key)
STOPS = {
    'praestant':    dict(label="Praestant 8'",  sources=[('preastant_full.wav', 0, None)], mult=1),
    'gedackt':      dict(label="Gedackt 8'",    sources=[('gedackt_8_1.wav', 0, None),
                                                         ('register1_mehr.wav', 0, None),
                                                         ('register1.wav', 215, None)], mult=1),
    'oktave4':      dict(label="Octave 4'",     sources=[('oktav_four.wav', 0, None)], mult=2),
    'oktave2':      dict(label="Octave 2'",     sources=[('oktav2.wav', 0, None)], mult=4),
    'sesquialtera': dict(label='Sesquialtera',  sources=[('sesquialtera.wav', 0, None)], compound=[0.0, 696.6, 386.3]),
    'mixtur':       dict(label='Mixtur',        sources=[('mixtr.wav', 0, None)], compound=[0.0, 696.6]),
    'trompete':     dict(label="Trompete 8'",   sources=[('trompete.wav', 0, None)], mult=1),
}
NOISE = dict(clicks='klickgerausche.wav', stops='register_ziehen.wav',
             wind=['preastant_full.wav', 'oktav_four.wav', 'trompete.wav'])

# --------------------------------------------------------------------------
# Analysis parameters
# --------------------------------------------------------------------------
HOP = 240                 # 5 ms @ 48 kHz
ON_DB = 8.0               # note threshold above noise floor
GAP_BRIDGE = 0.35         # s, gaps shorter than this inside a note are bridged
MIN_NOTE = 0.25           # s, shorter segments are discarded
PREROLL = 0.020           # s before the detected onset
TAIL_DB = 2.0             # tail ends when level is within this of the floor
MAX_TAIL = 5.0            # s
FADE_OUT = 0.040          # s
LOOP_WIN = 2048           # samples compared at the loop seam
JUNK = 4.0                # max emission cost
MONO = True               # write mono files (time-aligned sum of both mics); --stereo keeps L/R
MONO_MAX_LAG = 200        # samples (~4 ms) searched to line the two mics up
MONO_MIN_EFF = 0.6        # if the aligned sum still cancels more than this, use the stronger mic alone
STEADY_DB = 3.0           # std of the sustain level above which a pipe is flagged unsteady
SKIP = 2.5                # cost of declaring a segment junk (not a key)
CLEAN = True              # high-pass + noise reduction + soft tails (--no-clean to switch off)
HP_RATIO = 0.55           # high-pass corner = this x the lowest fundamental of the pipe
HP_MIN = 25.0             # Hz
NR_DB = 15.0              # maximum noise reduction (dB)
NR_OVER = 1.6             # over-subtraction factor of the noise print
NR_FFT = 2048
TAIL_FADE = 0.35          # s, soft fade at the very end of each pipe


def env_db(mono):
    n = len(mono) // HOP
    e = (mono[:n * HOP].reshape(n, HOP) ** 2).mean(1)
    e = uniform_filter1d(e, 6)
    return 10 * np.log10(e + 1e-14)


def _runs(act, sr, bridge, min_len, offset=0):
    if bridge > 0:
        act = binary_closing(np.r_[False, act, False], structure=np.ones(max(1, int(bridge * sr / HOP))))[1:-1]
    d = np.diff(np.r_[0, act.astype(int), 0])
    st, en = np.where(d == 1)[0], np.where(d == -1)[0]
    return [(s + offset, e + offset) for s, e in zip(st, en) if (e - s) * HOP / sr >= min_len]


def find_notes(db, sr):
    """two passes: loud notes with a high threshold (keeps short gaps between
    notes intact), then quiet notes searched only inside the gaps."""
    floor = float(np.percentile(db, 5))
    segs = _runs(db > floor + 12, sr, 0.15, MIN_NOTE)
    extra = []
    bounds = [(0, 0)] + segs + [(len(db), len(db))]
    for (s0, e0), (s1, e1) in zip(bounds[:-1], bounds[1:]):
        g0 = e0 + int(0.4 * sr / HOP)          # skip the previous note's tail
        g1 = s1
        if g1 - g0 < int(0.5 * sr / HOP): continue
        extra += _runs(db[g0:g1] > floor + ON_DB, sr, GAP_BRIDGE, 0.4, offset=g0)
    segs = sorted(segs + extra)
    # split segments that swallowed two notes (dip between them stayed above threshold)
    med = np.median([e - s for s, e in segs]) if segs else 0
    out = []
    stack = list(segs)
    while stack:
        s, e = stack.pop(0)
        if e - s > 1.7 * med:
            sm = uniform_filter1d(db[s:e], 10)
            edge = int(1.0 * sr / HOP)
            inner = sm[edge:-edge]
            if len(inner):
                j = int(np.argmin(inner)) + edge
                plateau = np.percentile(sm, 75)
                if plateau - sm[j] > 6:
                    stack[:0] = [(s, s + j), (s + j, e)]
                    continue
        out.append((s, e))
    return floor, out


def folded_cents(f, ref):
    c = 1200 * math.log2(f / ref)
    return abs((c + 600) % 1200 - 600), c


def emission(f0, key, cfg):
    if not f0 or f0 <= 0 or f0 > 4700:          # no usable pitch
        return 1.5 if 'compound' in cfg else JUNK
    if 'compound' in cfg:
        d = min(folded_cents(f0, mt_freq(key) * 2 ** (iv / 1200))[0] / 12 + (0.0 if iv == 0 else 0.3 + 0.2 * j)
                for j, iv in enumerate(cfg['compound']))
        return min(d, JUNK)
    d, c = folded_cents(f0, mt_freq(key) * cfg['mult'])
    octs = abs(round(c / 1200))
    return min(d / 12 + 1.2 * octs, JUNK)


def viterbi(f0s, cfg):
    """assign each segment (in recording order) to a physical key index"""
    K, N = len(PHYS_KEYS), len(f0s)
    E = np.array([[emission(f, PHYS_KEYS[k], cfg) for k in range(K)] for f in f0s])
    best = None
    for direction in (+1, -1):
        T = np.empty((K, K))
        for p in range(K):
            for q in range(K):
                step = q - p
                if step == direction: T[p, q] = 0.0
                elif step == 0: T[p, q] = 0.6
                elif abs(step - direction) <= 2: T[p, q] = 1.2
                else: T[p, q] = 2.5 + 0.02 * abs(step)
        # a segment may also be skipped as junk: the key position stays, cost SKIP
        D = np.minimum(E[0], SKIP); B = np.zeros((N, K), int); J = np.zeros((N, K), bool)
        J[0] = E[0] > SKIP
        for i in range(1, N):
            M = D[:, None] + T
            take = M.min(0) + E[i]; skip = D + SKIP
            J[i] = skip < take
            B[i] = np.where(J[i], np.arange(K), M.argmin(0))
            D = np.minimum(take, skip)
        k = int(D.argmin()); path = []
        for i in range(N - 1, -1, -1):
            path.append((k, bool(J[i, k])))
            k = int(B[i, k])
        path = path[::-1]; cost = float(D.min())
        if best is None or cost < best[0]:
            best = (cost, path, direction)
    cost, path, direction = best
    return [(k, JUNK if junk else float(E[i, k])) for i, (k, junk) in enumerate(path)], direction


def measure_f0(mono, a, b, sr):
    y = mono[a:b]
    if len(y) < 4096: return None
    f = librosa.yin(y, fmin=45, fmax=4800, sr=sr, frame_length=4096)
    return float(np.median(f))


def find_loop(stereo, a, b, sr):
    """loop between a..b (sample indices); returns (start, end, ncc) or None"""
    y = stereo.mean(1)
    N = LOOP_WIN
    if b - a < int(0.6 * sr):
        return None
    best = None
    for e in np.linspace(b - min(int(0.8 * sr), (b - a) // 3), b, 16).astype(int):
        T = y[e - N:e]
        lo, hi = a + N, e - int(0.5 * sr)
        if hi - lo < N: continue
        R = y[lo - N:hi]
        num = fftconvolve(R, T[::-1], mode='valid')                       # sum R[s-N+i]*T[i]
        en = np.convolve(R ** 2, np.ones(N), mode='valid')
        ncc = num / np.sqrt(en * np.sum(T ** 2) + 1e-20)
        starts = np.arange(lo, lo + len(ncc))
        score = ncc + 0.03 * (e - starts) / sr                              # prefer longer loops
        j = int(score.argmax())
        cand = (float(score[j]), int(starts[j]), int(e), float(ncc[j]))
        if best is None or cand[0] > best[0]:
            best = cand
    if best is None: return None
    return best[1], best[2], best[3]


def extract_note(x, db, floor, s, e, next_on, prev_end, sr):
    """returns dict with sample-accurate positions in the source file"""
    sus = db[s:e]
    plateau = np.median(sus[len(sus) // 4: 3 * len(sus) // 4]) if len(sus) > 8 else sus.max()
    # onset: walk back to where level leaves the floor
    i = s
    while i > 0 and db[i - 1] > floor + 4 and (prev_end is None or i - 1 > prev_end): i -= 1
    onset = i
    # attack end: first frame within 3 dB of plateau
    att = s + int(np.argmax(sus >= plateau - 3)) if np.any(sus >= plateau - 3) else s
    # key release: last frame within 3 dB of plateau
    above = np.where(sus >= plateau - 3)[0]
    rel = s + int(above[-1]) if len(above) else e
    # tail end
    j = rel; lim = len(db) if next_on is None else next_on
    lim = min(lim, rel + int(MAX_TAIL * sr / HOP))
    quiet = 0
    while j < lim - 1:
        quiet = quiet + 1 if db[j] < floor + TAIL_DB else 0
        if quiet >= int(0.08 * sr / HOP): break
        j += 1
    tail_end = j
    a = max(0, onset * HOP - int(PREROLL * sr))
    b = min(len(x), tail_end * HOP)
    return dict(a=a, b=b, onset=onset * HOP, attack_end=att * HOP, release=rel * HOP,
                plateau_db=float(plateau), steadiness=float(np.std(uniform_filter1d(db[att:rel], 20))) if rel > att + 4 else 99.0)


def _shift(r, lag):
    """delay (lag > 0) or advance r by lag samples, zero padded"""
    if lag > 0: return np.r_[np.zeros(lag, r.dtype), r[:-lag]]
    if lag < 0: return np.r_[r[-lag:], np.zeros(-lag, r.dtype)]
    return r


def to_mono(y, a=None, b=None):
    """Two spaced mics summed naively cancel many partials (up to -13 dB here).
    The right channel is shifted by the lag that maximises its correlation with
    the left inside y[a:b] (the steady part), then both are summed and the loss
    of summing is compensated so the pipe keeps its level. If the channels
    still cancel badly, the louder channel is used alone."""
    if y.ndim == 1 or y.shape[1] == 1:
        return (y if y.ndim == 1 else y[:, 0]).copy(), dict(method='mono', lag=0, efficiency=1.0)
    L, R = y[:, 0].astype(np.float64), y[:, 1].astype(np.float64)
    a = 0 if a is None else max(0, a); b = len(y) if b is None or b - a < 4096 else b
    l, r = L[a:b], R[a:b]
    M = MONO_MAX_LAG
    c = fftconvolve(l, r[::-1], mode='full'); mid = len(r) - 1
    lag = int(np.argmax(c[mid - M:mid + M + 1])) - M          # R shifted by lag matches L best
    R2 = _shift(R, lag)
    l2, r2 = L[a + M:b - M], R2[a + M:b - M]
    p_ch = 0.5 * (np.mean(l2 ** 2) + np.mean(r2 ** 2)) + 1e-20
    eff = float(np.mean((l2 + r2) ** 2) / (4 * p_ch))           # 1 = coherent, 0.5 = uncorrelated
    if eff >= MONO_MIN_EFF:
        mono = 0.5 * (L + R2) / math.sqrt(eff)                  # keep the average channel level
        info = dict(method='aligned_sum', lag=lag, efficiency=round(eff, 3))
    else:
        louder = 0 if np.mean(l ** 2) >= np.mean(r ** 2) else 1
        mono = y[:, louder].astype(np.float64)
        info = dict(method='left' if louder == 0 else 'right', lag=0, efficiency=round(eff, 3))
    peak = np.max(np.abs(mono))
    if peak > 0.999:                                           # never clip (rare): scale down, note it
        mono *= 0.999 / peak; info['gain_reduced_db'] = round(20 * math.log10(0.999 / peak), 2)
    return mono.astype(np.float32), info


def mono_track(x, sr):
    """Step 1: turn a stereo recording into ONE mono track before any timing
    analysis. Every pipe stands somewhere else relative to the two mics, so the
    alignment lag is found per note: the file is cut into regions (one note each,
    borders in the middle of the gaps) and each region is aligned + summed with
    to_mono(). Returns the mono track and the list of regions with their info."""
    if x.shape[1] == 1:
        return x[:, 0].copy(), []
    db = env_db(x.mean(1))
    _, segs = find_notes(db, sr)
    M = np.zeros(len(x), np.float32)
    regions = []
    bounds = [0] + [((e0 + s1) // 2) * HOP for (_, e0), (s1, _) in zip(segs[:-1], segs[1:])] + [len(x)]
    for (s, e), r0, r1 in zip(segs, bounds[:-1], bounds[1:]):
        q = (e - s) * HOP // 4                               # steady middle of the note
        y = x[r0:r1]
        m, info = to_mono(y, s * HOP - r0 + q, e * HOP - r0 - q)
        M[r0:r1] = m
        info['lr_correlation'] = round(float(np.corrcoef(y[:, 0], y[:, 1])[0, 1]), 3)
        regions.append((r0, r1, info))
    if not segs:
        M[:], info = to_mono(x)
    return M, regions


# --------------------------------------------------------------------------
# Clean-up: rumble, wind noise, soft tails
# --------------------------------------------------------------------------
def noise_print(mono, db, floor, sr):
    """average power spectrum of the quiet gaps between notes (blower + room noise)"""
    quiet = db < floor + 3
    idx = np.repeat(quiet, HOP)[:len(mono)]
    q = mono[:len(idx)][idx]
    if len(q) < NR_FFT * 8:                                # not enough silence: use the quietest 5 %
        q = mono[np.argsort(np.abs(mono))[:max(NR_FFT * 8, len(mono) // 20)]]
    _, _, Z = stft(q, fs=sr, nperseg=NR_FFT, noverlap=NR_FFT * 3 // 4)
    return np.mean(np.abs(Z) ** 2, axis=1)


def clean_pipe(y, sr, f_low, npsd):
    """y: mono float array of one pipe. Returns the cleaned pipe."""
    x = y.astype(np.float64)
    # 1) zero-phase high-pass below the fundamental (rumble / sub)
    fc = max(HP_MIN, HP_RATIO * f_low)
    sos = butter(2, fc / (sr / 2), btype='highpass', output='sos')
    pad = min(len(x) - 1, int(sr * 0.5))
    x = sosfiltfilt(sos, x, padlen=pad)
    # 2) spectral noise reduction (Wiener-like gain with a floor, smoothed in time and frequency)
    if npsd is not None and NR_DB > 0:
        f, t, Z = stft(x, fs=sr, nperseg=NR_FFT, noverlap=NR_FFT * 3 // 4)
        # the noise print went through no high-pass: apply the same high-pass to it
        hp = 1.0 / (1.0 + (fc / np.maximum(f, 1.0)) ** 8)
        N = (npsd * hp)[:, None]
        P = np.abs(Z) ** 2
        g = np.maximum(1.0 - NR_OVER * N / np.maximum(P, 1e-20), 0.0)
        gmin = 10 ** (-NR_DB / 20)
        g = np.maximum(np.sqrt(g), gmin)
        g = uniform_filter1d(g, 3, axis=0)                        # across frequency
        # across time: open fast, close slowly (no pumping, no 'musical noise')
        gs = np.empty_like(g); gs[:, 0] = g[:, 0]
        for j in range(1, g.shape[1]):
            gs[:, j] = np.where(g[:, j] > gs[:, j - 1], g[:, j], 0.7 * gs[:, j - 1] + 0.3 * g[:, j])
        _, xr = istft(Z * gs, fs=sr, nperseg=NR_FFT, noverlap=NR_FFT * 3 // 4)
        x = xr[:len(x)] if len(xr) >= len(x) else np.r_[xr, np.zeros(len(x) - len(xr))]
    return x.astype(np.float32)


def soft_tail(y, sr, release):
    """end the pipe where the release tail has reached the remaining noise, with a long soft fade"""
    db = env_db(y)
    if len(db) < 20: return y
    floor = float(np.percentile(db[min(len(db) - 1, release // HOP):], 10)) if release // HOP < len(db) - 10 else float(np.min(db))
    r = release // HOP
    above = np.where(db[r:] > floor + 6)[0]
    end_f = r + (int(above[-1]) if len(above) else 0)
    end = min(len(y), end_f * HOP + int(0.25 * sr))
    end = max(end, min(len(y), release + int(0.2 * sr)))
    y = y[:end].copy()
    fo = min(int(TAIL_FADE * sr), max(1, (end - release) // 2) if end > release else len(y) // 4)
    if fo > 1: y[-fo:] *= (0.5 + 0.5 * np.cos(np.linspace(0, np.pi, fo)))
    return y


def process_stop(sid, cfg, raw, out, report):
    cands = {}
    for fname, t0, t1 in cfg['sources']:
        x, sr = sf.read(os.path.join(raw, fname), dtype='float32', always_2d=True)
        a0 = int(t0 * sr); a1 = len(x) if t1 is None else int(t1 * sr)
        x = x[a0:a1]
        regions = []
        if MONO:                                   # step 1: mono, then all timing on the mono track
            m, regions = mono_track(x, sr)
            x = m[:, None]
        mono = x.mean(1); db = env_db(mono)
        floor, segs = find_notes(db, sr)
        npsd = noise_print(mono, db, floor, sr) if CLEAN else None
        f0s = []
        info = []
        for idx, (s, e) in enumerate(segs):
            nxt = segs[idx + 1][0] if idx + 1 < len(segs) else None
            prv = segs[idx - 1][1] if idx > 0 else None
            n = extract_note(x, db, floor, s, e, nxt, prv, sr)
            ma = n['attack_end'] + int(0.15 * sr); mb = min(n['release'] - int(0.1 * sr), ma + int(1.5 * sr))
            if mb - ma < 4096: ma, mb = n['onset'], n['release']
            f0 = measure_f0(mono, ma, mb, sr)
            f0s.append(f0); info.append(n)
        assign, direction = viterbi(f0s, cfg)
        for n, f0, (k, cost) in zip(info, f0s, assign):
            key = PHYS_KEYS[k]
            reg = next((r[2] for r in regions if r[0] <= n['onset'] < r[1]), None)
            n.update(src=fname, src_offset=a0, f0=f0, key=key, fit=cost, x=x, sr=sr, floor=floor, mono=reg, npsd=npsd,
                     t=round((a0 + n['onset']) / sr, 2), sus_len=(n['release'] - n['attack_end']) / sr)
            if cost >= 2.5:
                report['junk'].append(dict(stop=sid, src=fname, t=n['t'], f0=f0)); continue
            lp = find_loop(x, n['attack_end'] + int(0.2 * sr), n['release'] - int(0.08 * sr), sr)
            n['loop'] = lp
            n['score'] = min(n['sus_len'], 3.0) - 0.5 * cost - (0 if lp and lp[2] > 0.9 else 1.0) \
                - 0.1 * min(n['steadiness'], 10)
            cands.setdefault(key, []).append(n)
        report['files'].append(dict(stop=sid, src=fname, notes=len(segs), direction='up' if direction > 0 else 'down',
                                    floor_db=round(floor, 1)))

    os.makedirs(os.path.join(out, sid), exist_ok=True)
    pipes = []
    for key in PHYS_KEYS:
        if key not in cands:
            report['missing'].append(dict(stop=sid, key=key, name=helmholtz(key)))
            continue
        takes = sorted(cands[key], key=lambda n: -n['score'])
        n = takes[0]
        x, sr = n['x'], n['sr']
        y = x[n['a']:n['b']].copy()
        fo = min(int(FADE_OUT * sr), len(y) // 4)
        y[-fo:] *= (0.5 + 0.5 * np.cos(np.linspace(0, np.pi, fo)))[:, None]
        fi = int(0.002 * sr); y[:fi] *= np.linspace(0, 1, fi)[:, None]
        fname = f"{key:03d}_{helmholtz(key).replace(chr(39), 'p')}.wav"
        rel = lambda p: int(p - n['a'])
        mono_info = n.get('mono')
        lr_corr = mono_info.get('lr_correlation') if mono_info else \
            (round(float(np.corrcoef(y[:, 0], y[:, 1])[0, 1]), 3) if y.shape[1] > 1 else None)
        if y.shape[1] == 1: y = y[:, 0]
        if CLEAN and y.ndim == 1:
            f_low = mt_freq(key) * (cfg.get('mult', 1) if 'compound' not in cfg else 1)
            y = clean_pipe(y, sr, f_low, n.get('npsd'))
            y = soft_tail(y, sr, rel(n['release']))
            fi = int(0.002 * sr); y[:fi] *= np.linspace(0, 1, fi)
        sf.write(os.path.join(out, sid, fname), y, sr, subtype='PCM_16')
        nominal = mt_freq(key) * cfg.get('mult', 1)
        meas = n['f0']
        cents = None if 'compound' in cfg or not meas else round(1200 * math.log2(meas / nominal), 1)
        octave_shift = None if cents is None else int(round(cents / 1200))
        flags = []
        if len(takes) > 1: flags.append(f'{len(takes)} takes')
        if n['sus_len'] < 1.0: flags.append('short sustain')
        if not n['loop'] or n['loop'][2] < 0.9: flags.append('weak loop')
        if n['steadiness'] > STEADY_DB and n['sus_len'] >= 1.0: flags.append(f"unsteady ({n['steadiness']:.1f} dB)")
        if octave_shift: flags.append(f'sounds {octave_shift:+d} oct vs nominal')
        if mono_info and mono_info['method'] != 'aligned_sum': flags.append(f"mono: {mono_info['method']} mic only")
        pipes.append(dict(
            key=key, name=helmholtz(key), phys_index=PHYS_KEYS.index(key), file=f"{sid}/{fname}",
            length=len(y), onset=rel(n['onset']), attack_end=rel(n['attack_end']), release=rel(n['release']),
            loop_start=rel(n['loop'][0]) if n['loop'] else None, loop_end=rel(n['loop'][1]) if n['loop'] else None,
            loop_ncc=round(n['loop'][2], 4) if n['loop'] else None,
            level_db=round(n['plateau_db'], 2), nominal_hz=round(nominal, 3),
            measured_hz=round(meas, 3) if meas else None, cents_vs_nominal=cents,
            lr_correlation=lr_corr, channels=1 if y.ndim == 1 else y.shape[1], mono=mono_info,
            source=n['src'], source_time=n['t'], fit=round(n['fit'], 3), flags=flags,
            other_takes=[dict(source=t['src'], time=t['t'], sustain=round(t['sus_len'], 2)) for t in takes[1:]]))
    devs = [p['cents_vs_nominal'] - 1200 * round(p['cents_vs_nominal'] / 1200) for p in pipes if p['cents_vs_nominal'] is not None]
    med = float(np.median(devs)) if devs else 0.0
    for p in pipes:
        if p['cents_vs_nominal'] is not None:
            d = p['cents_vs_nominal'] - 1200 * round(p['cents_vs_nominal'] / 1200)
            if abs(d - med) > 12: p['flags'].append(f'pitch {d - med:+.0f} ct vs stop median')
    return dict(id=sid, label=cfg['label'], footage_mult=cfg.get('mult'), compound='compound' in cfg,
                median_cents=round(med, 1), pipes=pipes)


def detect_events(db, sr, rel_db=15, min_gap=0.3, max_len=2.5):
    floor = float(np.percentile(db, 5))
    thr = floor + rel_db
    ons = []
    i = 1
    while i < len(db):
        if db[i] > thr and db[i - 1] <= thr and (not ons or np.min(db[ons[-1]:i]) < thr - 6):
            j = i
            while j > 0 and db[j - 1] > floor + 4 and db[j - 1] < db[j] + 0.5: j -= 1
            ons.append(j); i += int(min_gap * sr / HOP)
        else:
            i += 1
    return floor, ons


def cut_events(fname, raw, out, sub, sr_out_list, pair=False):
    x, sr = sf.read(os.path.join(raw, fname), dtype='float32', always_2d=True)
    if MONO and x.shape[1] > 1:
        m, _ = to_mono(x); x = m[:, None]
    db = env_db(x.mean(1))
    floor, ons = detect_events(db, sr)
    os.makedirs(os.path.join(out, 'noise', sub), exist_ok=True)
    evs = []
    for i, o in enumerate(ons):
        lim = ons[i + 1] if i + 1 < len(ons) else len(db)
        lim = min(lim, o + int(2.5 * sr / HOP))
        j = o + 1; quiet = 0
        while j < lim - 1:
            quiet = quiet + 1 if db[j] < floor + 3 else 0
            if quiet >= int(0.05 * sr / HOP): break
            j += 1
        a = max(0, o * HOP - int(0.005 * sr)); b = j * HOP
        evs.append(dict(a=a, b=b, t=a / sr, peak=float(db[o:j].max()) if j > o else float(db[o])))
    # pair key-down / key-up: a strong event followed 0.5-1.8 s later by another
    if pair:
        k = 0
        while k < len(evs):
            if k + 1 < len(evs) and 0.5 <= evs[k + 1]['t'] - evs[k]['t'] <= 1.8 and evs[k].get('kind') is None:
                evs[k]['kind'] = 'down'; evs[k + 1]['kind'] = 'up'; k += 2
            else:
                evs[k].setdefault('kind', 'other'); k += 1
    res = []
    counters = {}
    for ev in evs:
        y = x[ev['a']:ev['b']].copy()
        if len(y) < int(0.03 * sr): continue
        fo = min(int(0.02 * sr), len(y) // 4); y[-fo:] *= np.linspace(1, 0, fo)[:, None]
        if y.shape[1] == 1: y = y[:, 0]
        kind = ev.get('kind', 'pull')
        counters[kind] = counters.get(kind, 0) + 1
        fn = f"{kind}_{counters[kind]:02d}.wav"
        sf.write(os.path.join(out, 'noise', sub, fn), y, sr, subtype='PCM_16')
        res.append(dict(file=f"noise/{sub}/{fn}", kind=kind, source_time=round(ev['t'], 3),
                        length=len(y), peak_db=round(ev['peak'], 1)))
    return res


def wind_loops(raw, out, files, target=6.0):
    """the blower/wind noise between notes: all quiet stretches (>= 0.35 s, away
    from note tails) are joined with short crossfades into one ~6 s loop per file"""
    os.makedirs(os.path.join(out, 'noise', 'wind'), exist_ok=True)
    res = []
    for fname in files:
        x, sr = sf.read(os.path.join(raw, fname), dtype='float32', always_2d=True)
        if MONO and x.shape[1] > 1:
            x = (x.mean(1) * math.sqrt(2))[:, None]
        db = env_db(x.mean(1)); floor, segs = find_notes(db, sr)
        quiet = db < floor + 3
        for s, e in segs:                                   # exclude notes + 0.5 s tail
            quiet[max(0, s - 10):min(len(quiet), e + int(0.5 * sr / HOP))] = False
        runs = _runs(quiet, sr, 0, 0.35)
        xf = int(0.05 * sr); w = np.sqrt(np.linspace(0, 1, xf))[:, None]
        y = None; used = []
        for s, e in runs:
            piece = x[s * HOP + xf: e * HOP - xf]           # trim edges
            if len(piece) < 3 * xf: continue
            if y is None: y = piece.copy()
            else:
                y = np.concatenate([y[:-xf], y[-xf:] * w[::-1] + piece[:xf] * w, piece[xf:]])
            used.append(round(s * HOP / sr, 2))
            if len(y) >= target * sr: break
        if y is None or len(y) < sr: continue
        y_loop = y[xf:].copy(); y_loop[-xf:] = y_loop[-xf:] * w[::-1] + y[:xf] * w   # seamless loop
        fn = f"wind_{os.path.splitext(fname)[0]}.wav"
        if y_loop.shape[1] == 1: y_loop = y_loop[:, 0]
        sf.write(os.path.join(out, 'noise', 'wind', fn), y_loop, sr, subtype='PCM_16')
        res.append(dict(file=f"noise/wind/{fn}", length=len(y_loop), loop=True,
                        level_db=round(float(10 * np.log10(np.mean(y_loop ** 2) + 1e-14)), 1),
                        source=fname, source_times=used))
    return res


def plot_report(stops, path):
    try:
        import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
    except ImportError:
        return
    fig, axs = plt.subplots(len(stops), 1, figsize=(16, 2.3 * len(stops)), sharex=True)
    xs = np.arange(len(PHYS_KEYS))
    for ax, st in zip(axs, stops):
        by = {p['key']: p for p in st['pipes']}
        lv = [by[k]['level_db'] if k in by else np.nan for k in PHYS_KEYS]
        ax.bar(xs, np.nan_to_num(np.array(lv) + 70, nan=0), color=['#c60' if k in by and any(not f.endswith('takes') for f in by[k]['flags']) else '#444' if k in by else '#fff' for k in PHYS_KEYS], edgecolor='#888')
        for i, k in enumerate(PHYS_KEYS):
            if k not in by: ax.text(i, 2, 'x', ha='center', color='red')
        ax.set_ylabel('dB+70'); ax.set_title(st['label'], loc='left', fontsize=10)
        if not st['compound']:
            ax2 = ax.twinx()
            c = [by[k]['cents_vs_nominal'] - 1200 * round(by[k]['cents_vs_nominal'] / 1200) if k in by and by[k]['cents_vs_nominal'] is not None else np.nan for k in PHYS_KEYS]
            ax2.plot(xs, c, 'o', ms=3, color='#06c'); ax2.set_ylim(-30, 30); ax2.axhline(0, color='#06c', lw=.3)
            ax2.set_ylabel('cents', color='#06c')
    axs[-1].set_xticks(xs); axs[-1].set_xticklabels([helmholtz(k) for k in PHYS_KEYS], rotation=90, fontsize=7)
    plt.tight_layout(); plt.savefig(path, dpi=80)


def main():
    global MONO, CLEAN, NR_DB
    ap = argparse.ArgumentParser()
    ap.add_argument('--raw', default='raw_unprocessed_files')
    ap.add_argument('--out', default='samples')
    ap.add_argument('--stops', nargs='*', default=list(STOPS))
    ap.add_argument('--stereo', action='store_true', help='keep both microphone channels instead of mono')
    ap.add_argument('--no-clean', action='store_true', help='no high-pass / noise reduction / soft tails')
    ap.add_argument('--nr-db', type=float, default=15.0, help='maximum noise reduction in dB (default 15)')
    args = ap.parse_args()
    MONO = not args.stereo
    CLEAN = not args.no_clean
    NR_DB = args.nr_db
    os.makedirs(args.out, exist_ok=True)
    report = dict(files=[], missing=[], junk=[])
    stops = []
    for sid in args.stops:
        print(f'[{sid}] ...', flush=True)
        st = process_stop(sid, STOPS[sid], args.raw, args.out, report)
        flagged = sum(1 for p in st['pipes'] if p['flags'])
        print(f'   {len(st["pipes"])}/{len(PHYS_KEYS)} pipes, {flagged} flagged', flush=True)
        stops.append(st)
    print('[noise] ...', flush=True)
    noise = dict(clicks=cut_events(NOISE['clicks'], args.raw, args.out, 'clicks', None, pair=True),
                 stop_pulls=cut_events(NOISE['stops'], args.raw, args.out, 'stop_pulls', None),
                 wind=wind_loops(args.raw, args.out, NOISE['wind']))
    print(f"   {len(noise['clicks'])} clicks, {len(noise['stop_pulls'])} stop pulls, {len(noise['wind'])} wind loops")
    meta = dict(instrument='Rysum, um 1457', temperament='1/4-comma meantone', a4_hz=A4,
                keys=[dict(midi=k, name=helmholtz(k), phys_index=i) for i, k in enumerate(PHYS_KEYS)],
                sample_rate=48000, channels=1 if MONO else 2, stops=stops, noise=noise)
    json.dump(meta, open(os.path.join(args.out, 'rysum_samples.json'), 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
    json.dump(report, open(os.path.join(args.out, 'preprocess_report.json'), 'w'), indent=1)
    plot_report(stops, os.path.join(args.out, 'preprocess_overview.png'))
    print('missing keys:', ', '.join(f"{m['stop']}:{m['name']}" for m in report['missing']) or 'none')
    print('done ->', args.out)


if __name__ == '__main__':
    main()
