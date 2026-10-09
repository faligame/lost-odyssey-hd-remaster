import os as _os
ROOT = _os.path.abspath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', '..'))
import sys, pickle, struct, collections, glob, os
S = sys.argv[1]
DUMP = _os.path.join(ROOT, 'project', 'out', 'build', 'win-amd64-release', 'dump', 'shaders')
idx = pickle.load(open(S + "/indice_sc.pkl", "rb"))
guid2 = {}; pairs = collections.defaultdict(set); vfs_of_vs = collections.defaultdict(set)
for pkg, lst in idx.items():
    for shaders, groups in lst:
        for g, s in shaders.items(): guid2[g] = s
for pkg, lst in idx.items():
    for shaders, groups in lst:
        for mg, vf, m in groups:
            for tname, g in m.items():
                if "VertexShader" not in tname or g not in guid2: continue
                pt = tname.replace("VertexShader", "PixelShader")
                vsh = guid2[g]["h"]; vfs_of_vs[vsh].add(vf)
                if pt in m and m[pt] in guid2:
                    pairs[vsh].add(guid2[m[pt]]["h"])
disc_vs = {s["h"]: s for s in guid2.values() if s["t"] == 1}
disc_ps = {s["h"] for s in guid2.values() if s["t"] == 0}
print("VS", len(disc_vs), "PS", len(disc_ps), "pares", sum(len(v) for v in pairs.values()))
# runtime VS -> disc VS por microcodigo con los campos de vfetch en blanco
def W(b): return list(struct.unpack(">%dI" % (len(b) // 4), b))
def blank(w):
    w = list(w)
    for i in range(len(w) // 3):
        a = w[3*i]
        if (a & 0x1F) == 0 and ((a >> 19) & 1):
            w[3*i] = a & ~(7 << 27) & ~(0x7F << 20) & ~(63 << 12); w[3*i+1] = 0; w[3*i+2] = 0
    return tuple(w)
by_blank = collections.defaultdict(set)
for h, s in disc_vs.items(): by_blank[blank(W(s["ucode"]))].add(h)
rt2disc = {}
for f in glob.glob(os.path.join(DUMP, "*.ucode.bin.vert")):
    raw = open(f, "rb").read(); n = len(raw) // 4
    w = list(struct.unpack("<%dI" % n, raw))
    h = int(os.path.basename(f).split("_")[1].split(".")[0], 16)
    c = by_blank.get(blank(w))
    if c: rt2disc[h] = c
print("VS del juego con VS del disco (en blanco):", len(rt2disc), "de", len(glob.glob(os.path.join(DUMP, "*.ucode.bin.vert"))))
b = open(S + "/4D5307FA.fsi.vk.xpso", "rb").read()
st = collections.Counter()
for p in range(12, len(b) - 67, 68):
    vh, vm, ph, pm = struct.unpack_from("<4Q", b, p + 8)
    if ph not in disc_ps: st["PS no en disco"] += 1; continue
    if vh not in rt2disc: st["VS sin casar"] += 1; continue
    if any(ph in pairs.get(d, ()) for d in rt2disc[vh]): st["PAR PREDICHO"] += 1
    else: st["par no predicho"] += 1
print(st)
pickle.dump(dict(pairs=pairs, vfs_of_vs=vfs_of_vs, rt2disc=rt2disc), open(S + "/pares.pkl", "wb"))
