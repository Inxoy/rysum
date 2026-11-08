# Builds "Rysum Fell" = IM FELL DW Pica + a few symbols from DejaVu Serif it lacks.
# OFL: modified fonts must not use the Reserved Font Name, so the family is renamed.
import sys, copy
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.recordingPen import DecomposingRecordingPen

MISSING = "⅙′″→≈−◀▶♯↔↻"
def build(src, donor, out, style):
    f = TTFont(src); d = TTFont(donor)
    fc = f.getBestCmap(); dc = d.getBestCmap(); dgs = d.getGlyphSet()
    scale = (f['head'].unitsPerEm / d['head'].unitsPerEm) * 0.82     # DejaVu is large-eyed; match Fell
    order = f.getGlyphOrder()
    for ch in MISSING:
        cp = ord(ch)
        if cp in fc or cp not in dc: continue
        gname = dc[cp]; new = "rys_" + gname
        pen = TTGlyphPen(None)
        rec = DecomposingRecordingPen(dgs)
        dgs[gname].draw(rec)
        rec.replay(TransformPen(pen, (scale, 0, 0, scale, 0, 0)))
        f['glyf'][new] = pen.glyph()
        adv, lsb = d['hmtx'][gname]
        f['hmtx'][new] = (int(adv * scale), int(lsb * scale))
        if new not in order: order.append(new)
        for t in f['cmap'].tables:
            if t.isUnicode(): t.cmap[cp] = new
    f.setGlyphOrder(order)
    f['glyf'].glyphOrder = order
    f['maxp'].numGlyphs = len(order)
    for rec in f['name'].names:
        if rec.nameID in (1, 3, 4, 6, 16):
            s = rec.toUnicode()
            s = s.replace("IM FELL DW Pica", "Rysum Fell").replace("IMFELLDWPica", "RysumFell").replace("IM_FELL_DW_Pica", "Rysum_Fell")
            rec.string = s
    f['post'].formatType = 2.0 if f['post'].formatType == 2.0 else f['post'].formatType
    f.save(out)
    print(out, len(order), "glyphs")

build(sys.argv[1], "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf", sys.argv[3], "Roman")
build(sys.argv[2], "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Italic.ttf" , sys.argv[4], "Italic")
