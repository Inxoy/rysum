"""Beats between pipe pairs and the Tonnetz layout, computed with the plug-in's own rules
(OrganEngine::updateBeats, Spatial.h tonnetzPlaces). Same style as make_figs.py."""
import os, numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager as fm
here = os.path.dirname(os.path.abspath(__file__))
for f in ("RysumFell-Roman.ttf", "RysumFell-Italic.ttf"):
    fm.fontManager.addfont(os.path.join(here, "..", "fonts", f))
BG, INK, INK2, MUTED, GRID = "#0d0d0c", "#e9e7e2", "#b9b7b2", "#858380", "#2a2a28"
S1, S2, ACC = "#d9d5c9", "#c4563c", "#d0583b"
plt.rcParams.update({"font.family": ["Rysum Fell", "DejaVu Serif"], "font.size": 10, "figure.facecolor": BG,
    "axes.facecolor": BG, "savefig.facecolor": BG, "text.color": INK, "axes.edgecolor": MUTED, "axes.labelcolor": INK2,
    "xtick.color": MUTED, "ytick.color": MUTED, "axes.spines.top": False, "axes.spines.right": False})

NAMES = ["C","C♯","D","E♭","E","F","F♯","G","G♯","A","B♭","B"]
# position of each pitch class in the chain of fifths E♭ .. G♯ (relative to C)
CHAIN = {0:0, 7:1, 2:2, 9:3, 4:4, 11:5, 6:6, 1:7, 8:8, 5:-1, 10:-2, 3:-3}
def pcs(fifth):            # cents of the twelve pitch classes for a regular tuning with this fifth
    return {pc: (k * fifth) % 1200.0 for pc, k in CHAIN.items()}
MT = pcs(1200 * np.log2(1.5) - 21.506 / 4)      # 1/4-comma meantone (696.58 ct)
PY = pcs(1200 * np.log2(1.5))
ET = {pc: 100.0 * pc for pc in range(12)}

def beat(f1, f2):          # OrganEngine::updateBeats
    best, b = 1000, None
    for m in range(1, 7):
        nn = int(round(m * f2 / f1))
        if nn < 1 or nn > 8 or nn * m >= best: continue
        x = abs(nn * f1 - m * f2)
        if x < 35.0: best, b = nn * m, x
    return b

# ---- beats of intervals above c' (MIDI 60) in meantone and equal temperament, a' = 444.5 Hz in meantone
def hz(tab, pc, octave=4, a4=444.5):
    c = tab[pc] - tab[9] + 1200 * (octave - 4)
    return a4 * 2 ** (c / 1200)
ivs = [("c′–g′ fifth", 0, 7, 4), ("c′–e′ maj. third", 0, 4, 4), ("c′–e♭′ min. third", 0, 3, 4),
       ("c′–f′ fourth", 0, 5, 4), ("e′–g♯′ maj. third", 4, 8, 4), ("g♯–e♭′ wolf fifth", 8, 3, 3),
       ("b–e♭′ wolf third", 11, 3, 3)]
fig, ax = plt.subplots(figsize=(7.2, 2.8))
y = np.arange(len(ivs))
for off, tab, a4, col, lab in ((-0.18, MT, 444.5, S2, "¼-comma meantone (a′ = 444.5 Hz)"), (0.18, ET, 444.5, S1, "equal temperament")):
    vals = []
    for name, p1, p2, o1 in ivs:
        f1 = hz(tab, p1, o1, a4); f2 = hz(tab, p2, 4, a4)
        if f2 < f1: f2 *= 2
        b = beat(f1, f2); vals.append(b if b is not None else np.nan)
    ax.barh(y + off, vals, height=0.34, color=col, label=lab)
    for yy, v in zip(y + off, vals):
        if np.isfinite(v): ax.text(v + 0.3, yy, f"{v:.1f}", va="center", fontsize=8, color=INK2)
ax.set_yticks(y); ax.set_yticklabels([n for n, *_ in ivs]); ax.invert_yaxis()
ax.set_xlabel("beat rate [Hz]  (nearest coinciding partials, below 35 Hz)")
ax.legend(frameon=False, fontsize=9, labelcolor=INK2, loc="lower right")
fig.tight_layout(); fig.savefig(os.path.join(here, "beats.pdf")); plt.close(fig)

# ---- Tonnetz: Spatial.h tonnetzPlaces for one octave of pitch classes
def tonnetz(tab):
    def pcof(c): return c % 1200.0
    fs = [pcof(tab[(c + 7) % 12] - tab[c] + 1200) for c in range(12)]
    ts = [pcof(tab[(c + 4) % 12] - tab[c] + 1200) for c in range(12)]
    F = sorted(fs)[6]; T = sorted(ts)[6]
    wF = 0.5 + abs(F - 701.955) / 4; wT = 0.5 + abs(T - 386.314) / 4
    sF = 0.7 + abs(F - 701.955) / 10; sT = 0.7 + abs(T - 386.314) / 10
    out = {}
    for pc in range(12):
        p = pcof(tab[pc]); best = (1e9, 0, 0)
        for b in range(-2, 3):
            for a in range(-8, 9):
                e = abs(pcof(a * F + b * T) - p); e = min(e, 1200 - e)
                cost = abs(a) * wF + abs(b) * wT + e / 3
                if cost < best[0] - 1e-9: best = (cost, a, b)
        out[pc] = (sF * (best[1] + 0.5 * best[2]), sT * best[2])
    return out
fig = plt.figure(figsize=(7.2, 4.2))
gs = fig.add_gridspec(2, 2, height_ratios=[2.2, 1])
axs = [fig.add_subplot(gs[0, 0]), fig.add_subplot(gs[1, :]), fig.add_subplot(gs[0, 1])]
for ax, (ttl, tab) in zip(axs, (("¼-comma meantone", MT), ("Pythagorean", PY), ("equal temperament", ET))):
    pl = tonnetz(tab)
    for pc, (x, yv) in pl.items():
        for q, st in ((7, ":"), (4, "-")):           # fifth and major third neighbours
            if (pc + q) % 12 in pl:
                x2, y2 = pl[(pc + q) % 12]
                if abs(x2 - x) + abs(y2 - yv) < 6: ax.plot([x, x2], [yv, y2], ls=st, lw=0.6, c=MUTED)
    for pc, (x, yv) in pl.items():
        col = ACC if pc in (8, 3) else S1
        ax.plot(x, yv, "o", ms=13, mfc=BG, mec=col, mew=1.0)
        ax.text(x, yv, NAMES[pc], ha="center", va="center", fontsize=8.5, color=col)
    ax.set_title(ttl, fontsize=10, color=INK2, style="italic"); ax.set_aspect("equal"); ax.axis("off")
    ax.margins(0.12)
fig.tight_layout(h_pad=0.4); fig.savefig(os.path.join(here, "tonnetz.pdf")); plt.close(fig)
print("ok")
