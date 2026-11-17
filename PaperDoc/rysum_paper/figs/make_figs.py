"""Figures for the Rysum documentation. Run from paper/figs/:  python3 make_figs.py <samples-folder>"""
import json, sys, math, os
import numpy as np
import soundfile as sf
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SAMPLES = sys.argv[1] if len(sys.argv) > 1 else "../../samples"
BG = "#0d0d0c"                          # the plug-in's black
INK, INK2, MUTED, GRID = "#e9e7e2", "#b9b7b2", "#858380", "#2a2a28"
S1, S2 = "#d9d5c9", "#c4563c"          # parchment white and a lighter rubric red
ACC = "#d0583b"                         # rubric red, used for markers / highlights

from matplotlib import font_manager as _fm
for _f in ("RysumFell-Roman.ttf", "RysumFell-Italic.ttf"):
    _fm.fontManager.addfont(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fonts", _f))
plt.rcParams.update({
    "font.family": ["Rysum Fell", "DejaVu Serif"],
    "figure.facecolor": BG, "axes.facecolor": BG, "savefig.facecolor": BG, "text.color": INK,
    "font.size": 10, "axes.titlesize": 10.5, "axes.labelsize": 10,
    "axes.edgecolor": MUTED, "axes.linewidth": 0.6, "axes.labelcolor": INK2,
    "xtick.color": INK2, "ytick.color": INK2, "xtick.major.width": 0.5, "ytick.major.width": 0.5,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.5,
    "axes.spines.top": False, "axes.spines.right": False,
    "legend.frameon": False, "figure.dpi": 150, "savefig.bbox": "tight", "pdf.fonttype": 42,
})

lib = json.load(open(os.path.join(SAMPLES, "rysum_samples.json")))
MEANTONE = [0.0, 76.0, 193.2, 310.3, 386.3, 503.4, 579.5, 696.6, 772.6, 889.7, 1006.8, 1082.9]
NAMES = ["C", "C♯", "D", "E♭", "E", "F", "F♯", "G", "G♯", "A", "B♭", "B"]

# ---------------------------------------------------------------- 1 measured tuning per stop
def fig_tuning():
    stops = [s for s in lib["stops"] if not s.get("compound")]
    fig, axs = plt.subplots(len(stops), 1, figsize=(6.3, 1.25 * len(stops)), sharex=True)
    for ax, st in zip(axs, stops):
        pipes = sorted(st["pipes"], key=lambda p: p["key"])
        x = [p["key"] for p in pipes]
        y = [p["cents_vs_nominal"] for p in pipes]
        ax.axhspan(-5, 5, color=GRID, alpha=0.6, lw=0, zorder=0)
        ax.axhline(0, color=MUTED, lw=0.6)
        bad = [i for i, p in enumerate(pipes) if any(f.startswith(("short", "weak", "unsteady")) for f in p["flags"])]
        ax.plot(x, y, "o", ms=3.2, color=S1, mec=BG, mew=0.4, zorder=3)
        for i in bad:
            ax.plot(x[i], y[i], "o", ms=5.5, mfc="none", mec=ACC, mew=0.9, zorder=4)
        med = np.median(y)
        ax.axhline(med, color=S1, lw=0.8, ls=(0, (3, 2)))
        ax.set_ylim(-28, 32)
        ax.set_yticks([-20, 0, 20])
        ax.text(0.005, 0.93, f"{st['label']}   median {med:+.1f} ct, sd {np.std(y):.1f} ct",
                transform=ax.transAxes, va="top", fontsize=8, color=INK)
    ticks = [36, 41, 48, 53, 60, 65, 69, 72, 77, 81]
    lab = {36: "C", 41: "F", 48: "c", 53: "f", 60: "c′", 65: "f′", 69: "a′", 72: "c″", 77: "f″", 81: "a″"}
    axs[-1].set_xticks(ticks, [lab[t] for t in ticks])
    axs[-1].set_xlabel("key (sounding pitch)")
    fig.supylabel("deviation from ¼-comma meantone, a′ = 444.5 Hz  [cents]", fontsize=8.5, color=INK2)
    fig.savefig("tuning_measured.pdf")

# ---------------------------------------------------------------- 2 temperaments vs equal
def fig_temperaments():
    f6 = 701.955 - 21.506 / 6
    m = lambda v: (v % 1200 + 1200) % 1200
    six = [0, m(7*f6), m(2*f6), m(-3*f6), m(4*f6), m(-f6), m(6*f6), m(f6), m(8*f6), m(3*f6), m(-2*f6), m(5*f6)]
    T = {
        "1/4-comma meantone (recorded)": MEANTONE,
        "1/6-comma meantone": six,
        "Werckmeister III": [0, 90.225, 192.18, 294.135, 390.225, 498.045, 588.27, 696.09, 792.18, 888.27, 996.09, 1092.18],
        "Kirnberger III": [0, 90.225, 193.157, 294.135, 386.314, 498.045, 590.224, 696.578, 792.18, 889.735, 996.09, 1088.269],
        "Vallotti": [0, 94.135, 196.09, 298.045, 392.18, 501.955, 592.18, 698.045, 796.09, 894.135, 1000, 1090.225],
        "Pythagorean": [0, 113.685, 203.91, 294.135, 407.82, 498.045, 611.73, 701.955, 815.64, 905.865, 996.09, 1109.775],
    }
    fig, axs = plt.subplots(2, 3, figsize=(6.3, 3.3), sharey=True)
    for ax, (name, c) in zip(axs.flat, T.items()):
        dev = [(c[i] - c[9]) - (i - 9) * 100 for i in range(12)]   # a' fixed
        cols = [S2 if name.startswith("1/4") else S1] * 12
        ax.bar(range(12), dev, color=cols, width=0.72)
        ax.axhline(0, color=MUTED, lw=0.6)
        ax.set_title(name, loc="left", fontsize=8.5, color=INK)
        ax.set_xticks(range(12), NAMES, fontsize=6.8)
        ax.grid(axis="x", visible=False)
    for ax in axs[:, 0]:
        ax.set_ylabel("vs. equal, a′ fixed [ct]")
    fig.tight_layout()
    fig.savefig("temperaments.pdf")

# ---------------------------------------------------------------- 3 anatomy of one pipe sample
def fig_anatomy():
    st = next(s for s in lib["stops"] if s["id"] == "praestant")
    p = next(p for p in st["pipes"] if p["key"] == 48)
    y, sr = sf.read(os.path.join(SAMPLES, p["file"]))
    if y.ndim > 1: y = y.mean(1)
    t = np.arange(len(y)) / sr
    fig, ax = plt.subplots(figsize=(6.3, 2.1))
    ax.plot(t, y, color=INK2, lw=0.25)
    ax.axvspan(p["loop_start"] / sr, p["loop_end"] / sr, color=S1, alpha=0.12, lw=0)
    marks = [("onset", p["onset"]), ("attack end", p["attack_end"]), ("loop start", p["loop_start"]),
             ("loop end", p["loop_end"]), ("key release", p["release"])]
    top = np.max(np.abs(y)) * 1.15
    for i, (lab, s) in enumerate(marks):
        col = ACC if lab == "key release" else S1
        ax.axvline(s / sr, color=col, lw=0.8)
        ha = "right" if lab in ("loop end",) else "left"
        dx = -0.03 if ha == "right" else 0.03
        yy = top * (1.0 if lab in ("onset", "loop start", "loop end") else 0.80)
        ax.text(s / sr + dx, yy, lab, fontsize=7, color=INK, va="top", ha=ha,
                bbox=dict(fc=BG, ec="none", pad=0.6))
    ax.set_ylim(-top, top * 1.05)
    ax.set_xlim(0, t[-1])
    ax.set_xlabel("time [s]"); ax.set_yticks([])
    ax.set_title(f"Praestant 8′, key c ({p['measured_hz']:.1f} Hz): loop NCC {p['loop_ncc']:.3f}, level {p['level_db']:.1f} dBFS",
                 loc="left", fontsize=8.5, color=INK)
    ax.grid(False)
    fig.savefig("pipe_anatomy.pdf")

# ---------------------------------------------------------------- 4 classic stop-wind curves (OrganEngine.cpp)
def fig_stopwind():
    w = np.linspace(0.0, 1.0, 400)
    lack = 1 - w
    cents = -75 * lack * np.sqrt(lack)
    level = 20 * np.log10(np.maximum(w, 1e-4) ** 1.6)
    fc = 250 + 16000 * w ** 2
    fig, axs = plt.subplots(1, 3, figsize=(6.3, 1.9))
    for ax, yv, lab in zip(axs, (cents, level, fc), ("pitch [ct]", "level [dB]", "low-pass corner [Hz]")):
        ax.plot(w * 100, yv, color=S1, lw=1.6)
        ax.set_xlabel("stop wind [%]"); ax.set_title(lab, loc="left", fontsize=8.5, color=INK)
        ax.set_xlim(0, 100)
    axs[1].set_ylim(-60, 3)
    axs[2].set_yscale("log"); axs[2].set_ylim(200, 20000)
    axs[0].text(55, -55, "+ breath wobble\n35 ct · (1−w)²", fontsize=7, color=INK2)
    fig.tight_layout()
    fig.savefig("stopwind.pdf")

# ---------------------------------------------------------------- 5 edge tone (WindChaos.h), simulated
def fig_edgetone():
    f0 = 262.0
    def run(lock, stopped, fTop=4.0 * 262.0, wOn=0.45):   # since 14 Nov 2026 the tone starts at 2.3 .. 6 x f0 (was 2.2 .. 7 kHz); the paper now draws this with make_edgetone_tikz.pl
        ws = np.linspace(wOn, 0.03, 900)
        k, out, amp = -1, [], []
        res = lambda k: f0 * k * (1 + 0.0025 * (k - 1))
        def nearest(hz):
            k = max(1, round(hz / f0))
            if stopped and k % 2 == 0: k += 1 if hz > res(k) else -1
            return max(1, k)
        f = 0
        for w in ws:
            ff = fTop * math.sqrt(max(0.005, w) / wOn)
            if k < 1: k = nearest(ff)
            else:
                sp = 2.0 if stopped else 1.0
                if abs(ff - res(k)) / (f0 * sp) > 0.5 + 0.3 * lock: k = nearest(ff)
            fT = math.exp(math.log(ff) + lock * (math.log(res(k)) - math.log(ff)))
            f = fT if f == 0 else f + 0.35 * (fT - f)
            out.append(f)
            x = w / wOn
            amp.append(min(1, (1 - x) / 0.12) * x ** 1.3)
        return ws, np.array(out), np.array(amp)
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(6.3, 2.3), gridspec_kw=dict(width_ratios=[1.6, 1]))
    for lock, stopped, col, lab in ((0.05, False, MUTED, "free (lock 0.05)"), (0.85, False, S1, "open pipe, lock 0.85"),
                                     (0.85, True, S2, "stopped pipe, lock 0.85")):
        ws, f, _ = run(lock, stopped)
        a1.plot(ws * 100, f / 1000, color=col, lw=1.4, label=lab)
    a1.invert_xaxis(); a1.set_xlabel("stop wind [%]  (falling →)"); a1.set_ylabel("edge-tone frequency [kHz]")
    a1.legend(fontsize=7, loc="upper right")
    ws, _, a = run(0.5, False)
    a2.plot(ws * 100, 20 * np.log10(np.maximum(a, 1e-4)), color=S1, lw=1.4)
    a2.invert_xaxis(); a2.set_ylim(-40, 2); a2.set_xlabel("stop wind [%]"); a2.set_ylabel("edge-tone level [dB]")
    fig.tight_layout()
    fig.savefig("edgetone.pdf")

# ---------------------------------------------------------------- 6 orbit shapes (Spatial.h applyMotion)
def fig_ellipse():
    fig, axs = plt.subplots(1, 4, figsize=(6.3, 1.9), subplot_kw=dict(projection="polar"))
    a = np.linspace(0, 2 * np.pi, 500)
    for ax, lobes in zip(axs, (1, 2, 3, 4)):
        r = 1 - 0.5 * np.cos(lobes * a)
        ax.plot(a, r, color=S1, lw=1.5)
        ax.plot([0], [0], "o", color=INK, ms=3)
        ax.set_theta_zero_location("N"); ax.set_theta_direction(1)    # +az = left (counter-clockwise)
        ax.set_ylim(0, 1.6); ax.set_yticks([]); ax.set_xticks([])
        ax.set_title(["1 · focus", "2 · centre", "3 · triangle", "4 · square"][lobes - 1], fontsize=8, color=INK)
        ax.spines["polar"].set_color(GRID)
    fig.savefig("orbits.pdf")

# ---------------------------------------------------------------- 7 image sources (RoomSim.h), top view
def fig_images():
    W, L = 14.0, 28.0
    src = np.array([5.0, 19.0]); lis = np.array([7.0, 11.2])
    fig, ax = plt.subplots(figsize=(3.6, 3.6))
    def img(c, D, m):  # positions along one axis after m reflections (all variants)
        return 2 * D * math.ceil(m / 2) * (1 if m % 2 == 0 else 1) if False else None
    pts = []
    for nx in range(-2, 3):
        for ny in range(-2, 3):
            if abs(nx) + abs(ny) > 2: continue
            x = nx * W + (src[0] if nx % 2 == 0 else W - src[0])
            y = ny * L + (src[1] if ny % 2 == 0 else L - src[1])
            pts.append((abs(nx) + abs(ny), x, y))
            if abs(nx) + abs(ny) > 0:
                ax.add_patch(plt.Rectangle((nx * W, ny * L), W, L, fill=False, ec=GRID, lw=0.6))
    ax.add_patch(plt.Rectangle((0, 0), W, L, fill=False, ec=INK, lw=1.0))
    for o, x, y in pts:
        if o == 0: continue
        ax.plot([lis[0], x], [lis[1], y], color=ACC if o == 1 else MUTED, lw=0.5 if o == 2 else 0.7, alpha=0.8)
        ax.plot(x, y, "s", ms=3.2 if o == 1 else 2.4, color=ACC if o == 1 else MUTED)
    ax.plot(*src, "s", ms=5, color=INK); ax.plot(*lis, "o", ms=6, mfc=BG, mec=INK)
    ax.text(src[0] + 0.8, src[1], "pipe", fontsize=7); ax.text(lis[0] + 0.8, lis[1] - 1.5, "listener", fontsize=7)
    ax.set_aspect("equal"); ax.set_xlim(-2 * W, 3 * W); ax.set_ylim(-1.5 * L, 2.5 * L)
    ax.axis("off")
    fig.savefig("image_sources.pdf")

# ---------------------------------------------------------------- 8 level per stop (default balance)
def fig_levels():
    fig, ax = plt.subplots(figsize=(6.3, 2.0))
    for i, st in enumerate(lib["stops"]):
        L = [p["level_db"] for p in st["pipes"]]
        ax.plot(np.full(len(L), i) + np.random.default_rng(i).uniform(-0.18, 0.18, len(L)), L, "o", ms=2.6,
                color=S1, alpha=0.75, mec="none")
        ax.plot([i - 0.3, i + 0.3], [np.median(L)] * 2, color=INK, lw=1.2)
    ax.set_xticks(range(len(lib["stops"])), [s["label"] for s in lib["stops"]], fontsize=7.5)
    ax.set_ylabel("pipe RMS level [dBFS]"); ax.grid(axis="x", visible=False)
    fig.savefig("levels.pdf")

for f in (fig_tuning, fig_temperaments, fig_anatomy, fig_stopwind, fig_edgetone, fig_ellipse, fig_images, fig_levels):
    f(); print("ok", f.__name__)
