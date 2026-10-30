"""Rysum: crops the stop / manual photos, makes them black & white and adds film grain.
usage: python3 -I tools/make_photos.py <newpics folder> Resources [<folder with rysum_photos_*.jpg>]   (needs Pillow + numpy)

Stop tiles on the Spieltisch are about 140 x 121 px (aspect 1.16) and drawn 'cover', so every stop crop has
that aspect: the label fits completely, the knob sits below it. Where the next stop's label would show at the
bottom, the photo fades into the panel tone instead."""
import sys, numpy as np
from PIL import Image, ImageOps, ImageFilter
src, dst = sys.argv[1], sys.argv[2]
older = sys.argv[3] if len(sys.argv) > 3 else None
TILE_ASPECT = 1.16

def load(n):
    return ImageOps.exif_transpose(Image.open(f"{src}/{n}.jpg")).convert("RGB")

def grain(shape, amount, seed):
    rng = np.random.default_rng(seed)
    g = rng.normal(0, 1, shape).astype(np.float32)
    g = np.asarray(Image.fromarray(np.clip(g * 40 + 128, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.6))).astype(np.float32) - 128
    return g * (amount / max(1e-3, g.std()))

def tone(im, lo, hi, gamma, pct=(1.0, 99.3)):
    a = np.asarray(im).astype(np.float32)
    l = 0.36 * a[..., 0] + 0.52 * a[..., 1] + 0.12 * a[..., 2]
    p0, p1 = np.percentile(l, pct)
    t = np.clip((l - p0) / max(1.0, p1 - p0), 0, 1) ** gamma
    return lo + t * (hi - lo)

def finish(out, amount, seed):
    w = 0.55 + 0.45 * np.sin(np.pi * np.clip(out / 255, 0, 1))      # silver grain: strongest in the mid tones
    return Image.fromarray(np.clip(out + grain(out.shape, amount, seed) * w, 0, 255).astype(np.uint8), "L")

def stop(name, photo, label, top, bottom_limit=None, top_fade=0.0, width=420, margin=60, seed=1, lo=20, hi=205, gamma=1.15):
    """label = (x0, x1) of the label paper; top = crop top; bottom_limit = where the next label starts (faded out)"""
    x0, x1 = label
    w = (x1 - x0) + 2 * margin
    h = round(w / TILE_ASPECT)
    cx = (x0 + x1) / 2
    box = (round(cx - w / 2), top, round(cx + w / 2), top + h)
    im = load(photo).crop(box)
    im = im.resize((width, round(width * im.height / im.width)), Image.LANCZOS)
    out = tone(im, lo, hi, gamma)
    if bottom_limit is not None and bottom_limit < top + h:
        s = width / w
        y1 = (bottom_limit - top) * s                       # first row of the next label
        y0 = y1 - 0.09 * out.shape[0]                       # fade starts a little above it
        panel = np.median(np.concatenate([out[:, :int(0.08 * width)].ravel(), out[:, -int(0.08 * width):].ravel()]))
        ys = np.arange(out.shape[0], dtype=np.float32)[:, None]
        k = np.clip((ys - y0) / max(1.0, y1 - y0), 0, 1)
        k = k * k * (3 - 2 * k)
        out = out * (1 - k) + panel * k
    if top_fade > 0.0:                                      # something of the stop above peeks in at the top
        panel = np.median(np.concatenate([out[:, :int(0.08 * width)].ravel(), out[:, -int(0.08 * width):].ravel()]))
        ys = np.arange(out.shape[0], dtype=np.float32)[:, None]
        k = np.clip(1 - ys / (top_fade * out.shape[0]), 0, 1)
        k = k * k * (3 - 2 * k)
        out = out * (1 - k) + panel * k
    r = finish(out, 7.0, seed)
    r.save(f"{dst}/{name}.jpg", quality=88, optimize=True)
    return r

def free(name, photo, box, width, seed, lo=18, hi=210, gamma=1.1, amount=7.0, pct=(1.0, 99.3)):
    im = load(photo).crop(box)
    im = im.resize((width, round(width * im.height / im.width)), Image.LANCZOS)
    r = finish(tone(im, lo, hi, gamma, pct), amount, seed)
    r.save(f"{dst}/{name}.jpg", quality=88, optimize=True)
    return r

if __name__ == "__main__":
    # coordinates on the upright 3000 x 4000 photos
    stop("k_gedackt",      "gedackt",      (880, 2155), 1195, bottom_limit=2285, seed=11)
    stop("k_mixtur",       "mixtur",       (980, 1955), 1230, bottom_limit=2238, seed=12)
    stop("k_oktave4",      "mixtur",       (1000, 2055), 2215, top_fade=0.045, seed=13, margin=220)
    stop("k_oktave2",      "oktave",       (990, 2055), 1240, bottom_limit=2200, seed=14)
    stop("k_sesquialtera", "sesquialtera", (790, 2195), 1150, bottom_limit=2300, seed=15)
    stop("k_trompete",     "trompete",     (770, 2145), 1260, seed=16)
    # Praestant: a wooden lever, no label - the handle with its collar, at the tile aspect
    free("k_praestant",    "praestant1",   (760, 2120, 2560, 2120 + round(1800 / TILE_ASPECT)), 420, seed=17, gamma=1.35, pct=(0.5, 99.0))
    free("k_praestant16",  "praestant2",   (250, 2280, 2050, 2280 + round(1800 / TILE_ASPECT)), 420, seed=18, gamma=1.35, pct=(0.5, 99.0))
    # the manual, frontal, at the aspect of the photo field next to the key strip (250 x 115)
    free("keys",           "klav2",        (120, 720, 3930, 720 + round(3810 / 2.17)), 900, seed=19, gamma=1.05)

    # the page about the organ: facade pipes and the two stop jambs (older photos)
    if older is not None:
        src = older
        free("org_pipes",  "rysum_photos_0028", (300, 300, 3356, 5168), 420, seed=21, gamma=1.2)
        free("org_jamb_l", "rysum_photos_0019", (1100, 1300, 2750, 4000), 300, seed=22, gamma=1.15)
        free("org_jamb_r", "rysum_photos_0020", (1050, 1200, 2700, 3900), 300, seed=23, gamma=1.15)
