import os as _os
ROOT = _os.path.abspath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', '..'))
import sys, pickle, struct, collections, glob, os
S = sys.argv[1]; sys.argv = sys.argv[:2]
exec(open(S + "/bind_vs.py").read().split("mej = pickle")[0])  # W, bind, learn, fetch_table, disc_fetches
DUMP = _os.path.join(ROOT, 'project', 'out', 'build', 'win-amd64-release', 'dump', 'shaders')
idx = pickle.load(open(S + "/indice_sc.pkl", "rb"))
P = pickle.load(open(S + "/pares.pkl", "rb"))
vsd = {}
for pkg, lst in idx.items():
    for shaders, groups in lst:
        for s in shaders.values():
            if s["t"] == 1: vsd[s["h"]] = s
# aprender declaraciones (por VF) de los VS del juego
decls = collections.defaultdict(dict)   # vf -> {firma: decl}
ok = 0
for f in glob.glob(os.path.join(DUMP, "*.ucode.bin.vert")):
    h = int(os.path.basename(f).split("_")[1].split(".")[0], 16)
    if h not in P["rt2disc"]: continue
    raw = open(f, "rb").read(); n = len(raw) // 4; gw = list(struct.unpack("<%dI" % n, raw))
    for dh in P["rt2disc"][h]:
        s = vsd[dh]; dw = W(s["ucode"]); didx = disc_fetches(dw)
        t = fetch_table(s["hdr"], didx)
        if t is None: continue
        d = learn(dw, gw, didx, t)
        if d is None or bind(dw, didx, t, d) != gw: continue
        ok += 1
        firma = tuple(sorted((k, v[0], v[1], v[2], tuple(sorted(v[3].items()))) for k, v in d.items()))
        for vf in P["vfs_of_vs"][dh]: decls[vf][firma] = d
        break
print("VS del juego reproducidos:", ok)
for vf, ds in decls.items(): print("  ", vf, len(ds), "declaraciones")
# prediccion
pred_vs = set(); pred_pairs = 0; sin_decl = collections.Counter()
for dh, pss in P["pairs"].items():
    s = vsd[dh]; dw = W(s["ucode"]); didx = disc_fetches(dw); t = fetch_table(s["hdr"], didx)
    if t is None: sin_decl["sin tabla"] += 1; continue
    keys = {(x >> 12) & 0xFF for x in t}
    outs = set()
    for vf in P["vfs_of_vs"][dh]:
        for d in decls.get(vf, {}).values():
            if keys <= set(d):
                bw = bind(dw, didx, t, d)
                if bw: outs.add(tuple(bw))
    if not outs: sin_decl["sin declaracion"] += 1; continue
    pred_vs |= outs; pred_pairs += len(outs) * len(pss)
print("VS del disco con pares:", len(P["pairs"]), "->", dict(sin_decl))
print("VS de ejecucion predichos:", len(pred_vs), " pares VS-PS predichos:", pred_pairs)
pickle.dump(decls, open(S + "/decls.pkl", "wb"))
