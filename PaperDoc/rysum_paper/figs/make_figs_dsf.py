"""Reference tone (DsfMath.h, DsfOversampling.h) re-implemented in Python: partial weights and the error of the
integrator. Same style as make_figs.py."""
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
fig, ax = plt.subplots(1, 2, figsize=(8.6, 3.0), gridspec_kw={"wspace": 0.3})
# (a) partial n has amplitude r^(n-1)/n, r = 1-(1-x)^2 for the spectrum slider x
n = np.arange(1, 41)
for x, c, lab in ((0.0, MUTED, "0 %"), (0.2, S1, "20 %"), (0.5, INK2, "50 %"), (0.8, S2, "80 %"), (1.0, ACC, "100 %")):
    r = 1 - (1 - x) ** 2
    a = r ** (n - 1) / n if x > 0 else np.where(n == 1, 1.0, 0.0)
    a = np.maximum(a, 1e-6)
    ax[0].plot(n, 20 * np.log10(a), color=c, lw=1.4, marker="o", ms=2.5, label=lab)
ax[0].set_ylim(-60, 3); ax[0].set_xlabel("partial n"); ax[0].set_ylabel("level (dB)")
ax[0].set_title("Spectrum slider", color=INK2, fontsize=10)
ax[0].legend(frameon=False, ncol=5, loc="upper center", bbox_to_anchor=(0.5, -0.22), fontsize=8, labelcolor=INK2, columnspacing=0.8, handlelength=1.2)
ax[0].grid(color=GRID, lw=0.5)
# (b) error of the highest partial (trapezoidal integrator) for the oversampling factors 1,2,4,8 at fs = 48 kHz
fs = 48000.0
f = np.geomspace(20, 9000, 600)
h = np.minimum(1024, np.floor(0.45 * fs / f))
for F, c, ls in ((1, S2, "-"), (2, S1, "--"), (4, INK2, "-."), (8, ACC, "-")):
    th = 2 * np.pi * f / (fs * F)
    ok = h * th / 2 < np.pi / 2
    err = np.where(ok, 20 * np.log10(h * np.tan(th / 2) / np.tan(h * th / 2)), np.nan)
    ax[1].plot(f, err, color=c, lw=1.4, label="%d×" % F)
ax[1].axhspan(-0.25, 0.25, color=MUTED, alpha=0.35, lw=0)
ax[1].set_xscale("log"); ax[1].set_ylim(-12, 14); ax[1].set_xlim(20, 9000)
ax[1].set_xlabel("fundamental (Hz)"); ax[1].set_ylabel("error of the top partial (dB)")
ax[1].set_title("Integrator error, 48 kHz", color=INK2, fontsize=10)
ax[1].legend(frameon=False, ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.22), fontsize=8, labelcolor=INK2)
ax[1].grid(color=GRID, lw=0.5)
fig.savefig(os.path.join(here, "dsf.pdf"), bbox_inches="tight")
for F in (1, 2, 4, 8):
    th = 2 * np.pi * f / (fs * F); ok = h * th / 2 < np.pi / 2
    err = np.where(ok, 20 * np.log10(h * np.tan(th / 2) / np.tan(h * th / 2)), np.nan)
    print(F, "max |err| over 20 Hz..9 kHz: %.2f dB" % np.nanmax(np.abs(err)), "first f with |err|>0.25 dB: ", f[np.nanargmax(np.abs(np.nan_to_num(err)) > 0.25)] if (np.abs(np.nan_to_num(err)) > 0.25).any() else None)
