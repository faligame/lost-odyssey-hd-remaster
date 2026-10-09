"""Genera prewarm_seed.bin: lo minimo para que la precreacion de pipelines
funcione desde el primer arranque de un jugador nuevo, sin codigo del juego:

  - declaraciones de vertices por vertex factory (formato, signo, offset,
    stride y remapeo de componentes de cada elemento), aprendidas comparando
    los VS que ha usado el juego (.xsh) con los del disco;
  - estados de pipeline por clase de pasada (vertex factory + tipo de VS) y
    backend, sacados de las descripciones guardadas (.xpso) con las huellas de
    sombreador a cero y sin la mascara de interpoladores.

Uso: python generar_semilla.py <carpeta_trabajo> <cache_shareable> <salida.bin>
  carpeta_trabajo: con indice_sc.pkl (indice_sc.py) y bind_vs.py
  cache_shareable: cache/shaders/shareable del juego (copia si el juego esta abierto)

Formato (little-endian), version 1:
  'LOPS' u32 version
  u32 n_vf;   por vf: u32 vf_id, u32 n_decl; por decl: u32 n_elem;
              por elem: u32 clave, u32 a, u32 b, u32 c, 4 x i8 remap
  u32 n_set;  por backend: u32 etiqueta, u32 tam_descripcion, u32 n_clases;
              por clase: u32 clase, u32 n_estados; por estado: u32 cuenta, bytes
"""
import os, sys, struct, pickle, collections

W_DIR, SHAREABLE, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
sys.argv = [sys.argv[0], W_DIR]
exec(open(os.path.join(W_DIR, "bind_vs.py")).read().split("mej = pickle")[0])  # W, learn, bind, ...


def fnv(s):
    h = 2166136261
    for c in s.encode("latin-1"):
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


idx = pickle.load(open(os.path.join(W_DIR, "indice_sc.pkl"), "rb"))
guid2 = {}
for lst in idx.values():
    for sh, _ in lst:
        guid2.update(sh)
vsd = {s["h"]: s for s in guid2.values() if s["t"] == 1}
classes = collections.defaultdict(set)   # VS del disco -> {(vf, tipo)}
for lst in idx.values():
    for sh, groups in lst:
        for mg, vf, m in groups:
            for t, g in m.items():
                if "VertexShader" in t and g in guid2:
                    classes[guid2[g]["h"]].add((vf, t))


def blank(w):
    w = list(w)
    for i in range(len(w) // 3):
        a = w[3 * i]
        if (a & 0x1F) == 0 and ((a >> 19) & 1):
            w[3 * i] = a & ~(7 << 27) & ~(0x7F << 20) & ~(63 << 12)
            w[3 * i + 1] = 0
            w[3 * i + 2] = 0
    return tuple(w)


by_blank = collections.defaultdict(set)
for h, s in vsd.items():
    by_blank[blank(W(s["ucode"]))].add(h)

# VS usados por el juego (.xsh: microcodigo big-endian tal cual)
b = open(os.path.join(SHAREABLE, "4D5307FA.xsh"), "rb").read()
p, rt_vs = 8, {}
while p + 12 <= len(b):
    h, w = struct.unpack_from("<QI", b, p)
    n, t = w & 0x7FFFFFFF, w >> 31
    p += 12
    if t == 0:
        rt_vs[h] = list(struct.unpack_from(">%dI" % n, b, p))
    p += 4 * n

decls = collections.defaultdict(dict)  # vf_id -> firma -> decl
rt_classes = {}                         # VS del juego -> {clase}
for h, gw in rt_vs.items():
    for dh in by_blank.get(blank(gw), ()):
        rt_classes.setdefault(h, set()).update(fnv(vf + "|" + t) for vf, t in classes[dh])
        s = vsd[dh]
        dw = W(s["ucode"])
        didx = disc_fetches(dw)
        tab = fetch_table(s["hdr"], didx)
        if tab is None:
            continue
        d = learn(dw, gw, didx, tab)
        if d is None or bind(dw, didx, tab, d) != gw:
            continue
        firma = tuple(sorted((k, v[0], v[1], v[2], tuple(sorted(v[3].items()))) for k, v in d.items()))
        for vf in {vf for vf, _ in classes[dh]}:
            decls[fnv(vf)][firma] = d

# Estados por backend. Tamano de la descripcion: registro menos la huella (8).
SETS = [("D12R", "4D5307FA.rov.d3d12.xpso", 72), ("D12T", "4D5307FA.rtv.d3d12.xpso", 72),
        ("VKFS", "4D5307FA.fsi.vk.xpso", 68), ("VKFB", "4D5307FA.fbo.vk.xpso", 68)]
sets = []
for tag, name, rec in SETS:
    path = os.path.join(SHAREABLE, name)
    if not os.path.exists(path):
        continue
    raw = open(path, "rb").read()
    per_class = collections.defaultdict(collections.Counter)
    for q in range(12, len(raw) - rec + 1, rec):
        desc = bytearray(raw[q + 8:q + rec])
        vh, vm, ph, pm = struct.unpack_from("<4Q", desc, 0)
        if not ph or (pm >> 63) & 1:  # sin PS, o pipeline del HUD
            continue
        struct.pack_into("<4Q", desc, 0, 0, vm & ~0xFFFF, 0, pm & ~0xFFFFFFFF)
        for c in rt_classes.get(vh, ()):
            per_class[c][bytes(desc)] += 1
    if per_class:
        sets.append((tag, rec - 8, per_class))

out = bytearray(b"LOPS") + struct.pack("<I", 1)
out += struct.pack("<I", len(decls))
for vf_id, ds in sorted(decls.items()):
    out += struct.pack("<II", vf_id, len(ds))
    for d in ds.values():
        out += struct.pack("<I", len(d))
        for k, (a, bb, c, remap) in sorted(d.items()):
            r = [remap.get(i, -1) for i in range(4)]
            out += struct.pack("<IIII4b", k, a & 0xFFFFFFFF, bb & 0xFFFFFFFF, c & 0xFFFFFFFF, *r)
out += struct.pack("<I", len(sets))
for tag, size, per_class in sets:
    out += struct.pack("<4sII", tag.encode(), size, len(per_class))
    for c, cnt in sorted(per_class.items()):
        out += struct.pack("<II", c, len(cnt))
        for desc, n in cnt.most_common():
            out += struct.pack("<I", n) + desc
open(OUT, "wb").write(out)
print("semilla: %d vertex factories, %d declaraciones; %s; %d bytes" % (
    len(decls), sum(len(v) for v in decls.values()),
    ", ".join("%s %d clases/%d estados" % (t, len(pc), sum(len(c) for c in pc.values())) for t, _, pc in sets),
    len(out)))
