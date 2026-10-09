"""Casa cada VS volcado por el juego con el VS del disco mas parecido y
caracteriza las diferencias (por instruccion y por campo)."""
import os as _os
ROOT = _os.path.abspath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', '..'))
import pickle, glob, os, struct, collections, sys
S = sys.argv[1]
DUMP = _os.path.join(ROOT, 'project', 'out', 'build', 'win-amd64-release', 'dump', 'shaders')
res = pickle.load(open(S + "/bloques_disc1.pkl", "rb"))
disc_vs = {}
disc_ps = {}
for pkg, bl in res.items():
    for b in bl:
        if b["t"] == 1: disc_vs.setdefault(b["h"], (pkg, b))
        else: disc_ps.setdefault(b["h"], (pkg, b))
print("VS disco", len(disc_vs), "PS disco", len(disc_ps))
def load(f):
    raw = open(f, "rb").read(); n = len(raw) // 4
    return struct.pack(">%dI" % n, *struct.unpack("<%dI" % n, raw))
game = {}
for f in glob.glob(os.path.join(DUMP, "*.ucode.bin.vert")):
    game[os.path.basename(f).split("_")[1].split(".")[0]] = load(f)
print("VS juego", len(game))
def words(b): return struct.unpack(">%dI" % (len(b) // 4), b)
dv = {h: words(b["ucode"]) for h, (p, b) in disc_vs.items()}
# indice por longitud
bylen = collections.defaultdict(list)
for h, w in dv.items(): bylen[len(w)].append(h)
stats = collections.Counter(); ejemplos = []
mejores = {}
for gh, gb in game.items():
    gw = words(gb)
    best = None
    for L in set(bylen) :
        for h in bylen[L]:
            w = dv[h]
            n = min(len(w), len(gw))
            same = sum(1 for i in range(n) if w[i] == gw[i])
            score = same / max(len(w), len(gw))
            if not best or score > best[0]: best = (score, h, len(w))
    mejores[gh] = best
    stats["len igual" if best[2] == len(gw) else "len distinta"] += 1
    stats["score>=0.9" if best[0] >= 0.9 else "score<0.9"] += 1
print(stats)
print(sorted(collections.Counter(round(b[0], 1) for b in mejores.values()).items()))
pickle.dump(mejores, open(S + "/mejores_vs.pkl", "wb"))
