# Afina los candidatos: el RVA debe ser un store x86 (88/89 con SIB, con o sin
# REX) y se anota el inmediato guest 0x82xxxxxx mas cercano hacia atras
# (los ctx.lr del recompilado) para identificar la funcion.
import os as _os
ROOT = _os.path.abspath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))
import struct

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

def is_store(b):
    i = 0
    if b[i] & 0xF0 == 0x40:  # REX
        i += 1
    if b[i] not in (0x88, 0x89):
        return False
    modrm = b[i + 1]
    return (modrm & 0xC0) == 0 and (modrm & 7) == 4  # mod=00, SIB

CODE_LO, CODE_HI = 0x82290000, 0x8312D330
rva = 0x6598
out = []
while rva < 0x5390000:
    fo = rva_to_file(rva)
    if fo and fo + 6 < len(data) and is_store(data[fo:fo + 6]):
        # buscar hacia atras el ultimo dword guest 0x82xxxxxx
        best = None
        for back in range(4, 0x400):
            p = fo - back
            if p < 0:
                break
            w = struct.unpack_from("<I", data, p)[0]
            if CODE_LO <= w <= CODE_HI:
                best = w
                break
        out.append((rva, data[fo:fo + 6].hex(" "), best))
    rva += 0x10000

print("%d stores candidatos:" % len(out))
for rva, bs, lr in out:
    print("  RVA 0x%X bytes %s lr_cercano=%s" % (rva, bs, ("0x%08X" % lr) if lr else "-"))
