# Migas de pan alrededor de un RVA: lista los dwords que parecen direcciones
# de codigo guest (ctx.lr) en un entorno amplio, con su distancia.
import os as _os
ROOT = _os.path.abspath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))
import struct, sys

EXE = _os.path.join(ROOT, 'project', 'out', 'build', 'win-amd64-release', 'lostodyssey.exe')
data = open(EXE, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt_size = struct.unpack_from("<H", data, pe + 20)[0]
sec0 = pe + 24 + opt_size
secs = []
for i in range(nsec):
    off = sec0 + i * 40
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
    secs.append((va, vsize, raw, rsize))

def rva_to_file(rva):
    for va, vsize, raw, rsize in secs:
        if va <= rva < va + max(vsize, rsize):
            return raw + (rva - va)
    return None

rva = int(sys.argv[1], 16)
fo = rva_to_file(rva)
CODE_LO, CODE_HI = 0x82290000, 0x8312D330
hits = []
for delta in range(-0x3000, 0x3000):
    p = fo + delta
    if p < 0 or p + 4 > len(data):
        continue
    w = struct.unpack_from("<I", data, p)[0]
    if CODE_LO <= w <= CODE_HI and (w & 3) == 0:
        hits.append((delta, w))
before = [h for h in hits if h[0] < 0][-6:]
after = [h for h in hits if h[0] >= 0][:6]
for delta, w in before + after:
    print("  %+6d : 0x%08X" % (delta, w))
