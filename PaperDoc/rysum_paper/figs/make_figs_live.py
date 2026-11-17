"""Bellows (OrganLive.cpp, updateLive) and the pull towards pure intervals (updatePure), re-implemented in Python.
Same style as make_figs.py. Prints the numbers quoted in the text."""
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

dt = 1/60.0     # one slice of 64 samples is 1.3 ms; a coarser step gives the same curves
def blower(T, demand_fn, bellows, size):
    P = 1.0; out = []
    t = np.arange(0, T, dt)
    for ti in t:
        D = demand_fn(ti)
        tau = max(0.03, 0.1 * size)
        pull = 3 * bellows**2 * D / 24.0
        P += ((1 - P) - pull * P) * min(1.0, dt / tau)
        out.append(P)
    return t, np.array(out)
def kalkant(T, D, bellows, size, tread_rate):
    P = 1.0; V = 1.0; rising = False; phase = 0.0; Ps, Vs, tr = [], [], []
    t = np.arange(0, T, dt)
    for ti in t:
        phase += tread_rate / 60.0 * dt
        if phase >= 1.0:
            phase -= 1.0; rising = True; tr.append(ti)
        if rising:
            V += dt / 0.3
            if V >= 1.0: V = 1.0; rising = False
        V = max(0.0, V - bellows * (D / 12.0) / max(0.2, size) * dt)
        x = min(1.0, max(0.0, V / 0.3)); target = x * x * (3 - 2 * x)
        P += (1 - np.exp(-dt / 0.04)) * (target - P)
        Ps.append(P); Vs.append(V)
    return t, np.array(Ps), np.array(Vs), tr

fig, ax = plt.subplots(1, 2, figsize=(8.6, 2.9), gridspec_kw={"wspace": 0.28})
# (a) blower: nothing, then a big chord (demand ~ 20) from 1 s to 6 s; for comparison one pipe (demand ~ 1)
big = lambda t: 20.0 if 1.0 <= t < 6.0 else 0.0
one = lambda t: 1.0 if 1.0 <= t < 6.0 else 0.0
for (fn, lab, c, ls) in ((big, "big chord, 5 stops", S2, "-"), (one, "one pipe", S1, "--")):
    t, P = blower(8, fn, 1.0, 4.0)
    ax[0].plot(t, P, color=c, lw=1.6, ls=ls, label=lab)
ax[0].set_ylim(0, 1.08); ax[0].set_xlabel("time (s)"); ax[0].set_ylabel("wind pressure")
ax[0].set_title("Blower, “Windverbrauch” 100 %, size 4 s", color=INK2, fontsize=10)
ax[0].legend(frameon=False, loc="upper center", bbox_to_anchor=(0.5, -0.28), ncol=2, labelcolor=INK2, fontsize=9)
ax[0].grid(color=GRID, lw=0.5)
# (b) Kalkant: twelve-pipe chord held, a tread every 4 s
t, P, V, tr = kalkant(14, 12.0, 0.8, 3.0, 15.0)
ax[1].plot(t, V, color=S1, lw=1.2, ls="--", label="how full the bellows is")
ax[1].plot(t, P, color=S2, lw=1.6, label="wind pressure")
for x in tr: ax[1].axvline(x, color=MUTED, lw=0.6, ls=":")
ax[1].set_ylim(0, 1.08); ax[1].set_xlabel("time (s)")
ax[1].set_title("Kalkant, 15 treads/min, size 3 s, 80 %", color=INK2, fontsize=10)
ax[1].legend(frameon=False, loc="upper center", bbox_to_anchor=(0.5, -0.28), ncol=2, labelcolor=INK2, fontsize=9)
ax[1].grid(color=GRID, lw=0.5)
fig.savefig(os.path.join(here, "bellows.pdf"), bbox_inches="tight")
print("kalkant: min P %.3f, time below 0.5: %.2f s of %.0f" % (P.min(), (P < 0.5).sum() * dt, 14))
print("blower steady state big chord 1/(1+pull): %.3f" % (1 / (1 + 3 * 20 / 24.0)))

# ---- Reinheit: the springs of updatePure for a triad and a chord with a wolf fifth
JUST = [(0.0, 1.0, 30.0), (111.73, 0.12, 25.0), (182.40, 0.15, 20.0), (203.91, 0.3, 25.0), (266.87, 0.12, 18.0),
        (315.64, 0.6, 40.0), (386.31, 0.8, 45.0), (498.04, 0.9, 45.0), (582.51, 0.1, 18.0), (590.22, 0.12, 20.0),
        (701.96, 1.0, 45.0), (813.69, 0.6, 45.0), (884.36, 0.7, 40.0), (968.83, 0.2, 20.0), (996.09, 0.25, 22.0),
        (1017.60, 0.25, 22.0), (1088.27, 0.2, 25.0), (1200.0, 1.0, 30.0)]
def pure(cents):
    n = len(cents); idx = sorted(range(n), key=lambda i: cents[i]); springs = []
    for a in range(n):
        for b in range(a + 1, n):
            lo, hi = (a, b) if cents[a] <= cents[b] else (b, a)
            d = (cents[hi] - cents[lo]) % 1200.0
            best, e, w = 0.0, 0.0, 0.0
            for (jc, jw, reach) in JUST:
                dev = jc - d; sc = jw * (1 - abs(dev) / reach)
                if sc > best: best, e, w = sc, dev, jw
            if best > 0: springs.append((lo, hi, e, w))
    dd = [0.0] * n
    for it in range(24):
        for k in range(n):
            num, den = 0.0, 0.06
            for (lo, hi, e, w) in springs:
                if hi == k: num += w * (dd[lo] + e); den += w
                elif lo == k: num += w * (dd[hi] - e); den += w
            dd[k] = num / den
    m = sum(dd) / n
    return [x - m for x in dd]
fifth = 1200 * np.log2(1.5) - 21.506 / 4
pc = {"C": 0, "E": 4 * fifth % 1200, "G": fifth, "G♯": 8 * fifth % 1200, "E♭": (-3 * fifth) % 1200, "B": 5 * fifth % 1200, "D": 2 * fifth % 1200}
def show(name, notes):
    c = [pc[x] for x in notes]
    print(name, {k: round(v, 2) for k, v in zip(notes, pure(c))})
show("C-E-G", ["C", "E", "G"])
show("G#-B-E♭ (wolf)", ["G♯", "B", "E♭"])
show("C-E-G-D", ["C", "E", "G", "D"])
