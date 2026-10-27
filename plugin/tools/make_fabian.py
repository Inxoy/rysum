"""Builds Resources/fabian_hrir.bin from FABIAN_HRIR_measured_HATO_0_48k.sofa (FABIAN database, TU Berlin, CC BY 4.0).

    pip install h5py numpy
    python make_fabian.py FABIAN_HRIR_measured_HATO_0_48k.sofa ../Resources/fabian_hrir.bin
"""
import sys, struct, zlib
import h5py, numpy as np

src, dst = sys.argv[1], sys.argv[2]
f = h5py.File(src, 'r')
sp = f['SourcePosition'][:]; ir = f['Data.IR'][:]; sr = float(f['Data.SamplingRate'][0])
az = np.radians(sp[:, 0]); el = np.radians(sp[:, 1])
v = np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], 1)
n = 2000                                            # evenly spread directions (Fibonacci lattice, nearest measured)
i = np.arange(n) + 0.5
phi = np.arccos(1 - 2 * i / n); th = np.pi * (1 + 5 ** 0.5) * i
p = np.stack([np.cos(th) * np.sin(phi), np.sin(th) * np.sin(phi), np.cos(phi)], 1)
idx = np.unique(np.argmax(p @ v.T, 1))
L = 256
sel = ir[idx, :, :L].astype(np.float32)
w = np.ones(L, np.float32); w[-16:] = 0.5 + 0.5 * np.cos(np.pi * np.arange(16) / 16)
sel *= w
scale = np.abs(sel).max() / 32000.0
q = np.round(sel / scale).astype('<i2')
hdr = struct.pack('<4sIIIdf', b'RHRF', 1, len(idx), L, sr, scale)
pos = np.stack([sp[idx, 0], sp[idx, 1]], 1).astype('<f4')
open(dst, 'wb').write(zlib.compress(hdr + pos.tobytes() + q.tobytes(), 9))
print(len(idx), 'directions written to', dst)
