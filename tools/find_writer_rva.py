# La instruccion culpable vive en un RVA congruente con 0x6598 (mod 0x10000).
# Busca candidatos cuyo entorno tenga el guard de escritura guest del
# recompilado (shl r?d,0x0C = bytes C1 E? 0C o 41 C1 E? 0C) justo antes.
import os as _os
ROOT = _os.path.abspath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))
import struct

EXE = _os.path.join(ROOT, 'project', 'out', 'build', 'win-amd64-release', 'lostodyssey.exe')
data = open(EXE, "rb").read()

# Tabla de secciones PE: RVA -> offset de fichero.
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt_size = struct.unpack_from("<H", data, pe + 20)[0]
sec0 = pe + 24 + opt_size
secs = []
for i in range(nsec):
    off = sec0 + i * 40
    name = data[off:off + 8].rstrip(b"\0").decode(errors="replace")
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
    secs.append((name, va, vsize, raw, rsize))

def rva_to_file(rva):
    for name, va, vsize, raw, rsize in secs:
        if va <= rva < va + max(vsize, rsize):
            return raw + (rva - va)
    return None

image_size = 0x5390000
cands = []
rva = 0x6598
while rva < image_size:
    fo = rva_to_file(rva)
    if fo and fo + 4 < len(data):
        window = data[fo - 12:fo]
        # shl r/m32,0x0C: C1 Ex 0C (o con prefijo REX 41/45)
        found = False
        for j in range(len(window) - 2):
            if window[j] == 0xC1 and (window[j + 1] & 0xF8) == 0xE0 and window[j + 2] == 0x0C:
                found = True
                break
        if found:
            cands.append((rva, data[fo:fo + 6].hex(" ")))
    rva += 0x10000

print("%d candidatos con guard shl-12 delante:" % len(cands))
for rva, bs in cands:
    print("  RVA 0x%X : bytes %s" % (rva, bs))
