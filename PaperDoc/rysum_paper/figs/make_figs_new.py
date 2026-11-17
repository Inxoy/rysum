"""Figures for the floor plans and the spatial degradation (same style as make_figs.py)."""
import os, numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager as fm
from matplotlib.patches import Polygon
here = os.path.dirname(os.path.abspath(__file__))
for f in ("RysumFell-Roman.ttf", "RysumFell-Italic.ttf"):
    fm.fontManager.addfont(os.path.join(here, "..", "fonts", f))
BG, INK, INK2, MUTED, GRID = "#0d0d0c", "#e9e7e2", "#b9b7b2", "#858380", "#2a2a28"
S1, S2, ACC = "#d9d5c9", "#c4563c", "#d0583b"
plt.rcParams.update({"font.family": ["Rysum Fell", "DejaVu Serif"], "font.size": 10, "figure.facecolor": BG,
    "axes.facecolor": BG, "savefig.facecolor": BG, "text.color": INK, "axes.edgecolor": MUTED, "axes.labelcolor": INK2,
    "xtick.color": MUTED, "ytick.color": MUTED, "axes.spines.top": False, "axes.spines.right": False})

# ---- the eight preset floor plans (vertex rules copied from RoomSim.h, makeFloorPlan), a = 0.5
a = 0.5; pi = np.pi
def box(): return [(0,0),(1,0),(1,1),(0,1)]
def L(): c=.15+.3*a; return [(0,0),(1,0),(1,1-c),(1-c,1-c),(1-c,1),(0,1)]
def fan(): w=1-.75*a; return [(0,0),(1,0),(.5+.5*w,1),(.5-.5*w,1)]
def hexa(): h=.08+.34*a; return [(h,0),(1-h,0),(1,.5),(1-h,1),(h,1),(0,.5)]
def octa(): c=.06+.3*a; return [(c,0),(1-c,0),(1,c),(1,1-c),(1-c,1),(c,1),(0,1-c),(0,c)]
def rnd():
    pe=2+8*(1-a)**2; out=[]
    for i in range(12):
        t=2*pi*(i+.5)/12-.5*pi; c,s=np.cos(t),np.sin(t)
        out.append((.5+.5*np.copysign(abs(c)**(2/pe),c), .5+.5*np.copysign(abs(s)**(2/pe),s)))
    return out
def cross():
    nw=.5*(.75-.4*a); t0,t1=.55,.8; x0,x1=.5-nw,.5+nw
    return [(x0,0),(x1,0),(x1,t0),(1,t0),(1,t1),(x1,t1),(x1,1),(x0,1),(x0,t1),(0,t1),(0,t0),(x0,t0)]
def apse():
    w=.45+.55*a; d=.22; p=[(0,0),(1,0)]
    if w<.995: p.append((1,1-d))
    p.append((.5+.5*w,1-d))
    for k in range(1,6):
        ph=pi*k/6; p.append((.5+.5*w*np.cos(ph),1-d+d*np.sin(ph)))
    p.append((.5-.5*w,1-d))
    if w<.995: p.append((0,1-d))
    return p
plans=[("Quader",box()),("L-Form",L()),("Fächer",fan()),("Sechseck",hexa()),("Achteck",octa()),("Rund",rnd()),("Kreuz",cross()),("Apsis",apse())]
fig,axs=plt.subplots(2,4,figsize=(7.2,3.9))
for ax,(nm,p) in zip(axs.ravel(),plans):
    ax.add_patch(Polygon(p,closed=True,fc="#1c1c1a",ec=INK,lw=1.1))
    ax.plot([0,1,1,0,0],[0,0,1,1,0],ls=(0,(2,3)),lw=.5,c=MUTED)
    ax.set_xlim(-.05,1.05); ax.set_ylim(-.05,1.05); ax.set_aspect("equal"); ax.axis("off")
    ax.set_title(nm,fontsize=10,color=INK2,style="italic",pad=2)
fig.tight_layout(h_pad=.6,w_pad=.4)
fig.savefig(os.path.join(here,"floorplans.pdf")); plt.close(fig)

# ---- spatial erosion: channel weights per degree, bit depth and hold rate per degree
fig,(a1,a2)=plt.subplots(1,2,figsize=(7.2,2.6))
N=3; e=np.linspace(0,1,200)
for l,c in zip(range(1,N+1),[S1,INK2,ACC]):
    a1.plot(e*100,np.clip(N*(1-e)-(l-1),0,1),c=c,lw=1.6,label=f"degree {l}")
a1.set_xlabel("erosion e [%]"); a1.set_ylabel("channel weight"); a1.legend(frameon=False,fontsize=9,labelcolor=INK2)
a1.set_title("(a) order erosion, N = 3",fontsize=10,color=INK2,style="italic")
deg=np.arange(0,6); B=8
a2.plot(deg,16-(16-B)*deg/5,"o-",c=S1,lw=1.4,ms=4,label="bit depth for B = 8")
a2.set_xlabel("degree l  (N = 5)"); a2.set_ylabel("bits",color=S1)
a3=a2.twinx(); a3.spines["right"].set_visible(True); a3.spines["right"].set_color(MUTED)
a3.plot(deg,48000*(4000/48000)**(deg/5),"s-",c=ACC,lw=1.4,ms=4); a3.set_yscale("log"); a3.set_ylabel("hold rate [Hz], f = 4000 Hz",color=ACC)
a2.set_title("(b) coarser with the degree",fontsize=10,color=INK2,style="italic")
fig.tight_layout(); fig.savefig(os.path.join(here,"spacedeg.pdf")); plt.close(fig)
print("ok")
