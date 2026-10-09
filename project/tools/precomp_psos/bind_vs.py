"""Reproduce el 'binding' de VS del runtime D3D de la 360: VS del disco +
declaracion de vertices -> VS que ve la GPU. Valida contra los VS del juego."""
import os as _os
ROOT = _os.path.abspath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', '..'))
import pickle, struct, sys, os, glob, collections, xxhash
S = sys.argv[1]
DUMP = _os.path.join(ROOT, 'project', 'out', 'build', 'win-amd64-release', 'dump', 'shaders')
res = pickle.load(open(S + "/bloques_disc1.pkl", "rb"))
dv = {}
for pkg, bl in res.items():
    for b in bl:
        if b["t"] == 1: dv.setdefault(b["h"], b)
def W(b): return list(struct.unpack(">%dI" % (len(b) // 4), b))
def game(h):
    raw = open(os.path.join(DUMP, "shader_%s.ucode.bin.vert" % h), "rb").read(); n = len(raw) // 4
    return list(struct.unpack("<%dI" % n, raw))
def is_vfetch(a): return (a & 0x1F) == 0 and ((a >> 19) & 1)
# tamano en dwords por formato de datos de vertice
FMT_DW = {6: 1, 7: 1, 16: 1, 17: 1, 25: 1, 26: 2, 31: 1, 32: 2, 33: 1, 34: 2, 35: 4, 36: 1, 37: 2, 38: 4, 57: 3}
def fetch_table(hdr, idxs):
    """Entradas (u32) de la cabecera cuyo byte bajo son los indices de los vfetch."""
    n = len(hdr) // 4; v = struct.unpack(">%dI" % n, hdr[:4 * n])
    for p in range(n - len(idxs) + 1):
        if all((v[p + k] & 0xFF) == idxs[k] for k in range(len(idxs))):
            return [v[p + k] for k in range(len(idxs))]
    return None
def disc_fetches(w):
    return [i for i in range(len(w) // 3) if is_vfetch(w[3*i]) and w[3*i+2] == 0 and ((w[3*i+1] >> 16) & 63) == 0]
def swz(v): return [(v >> (3 * c)) & 7 for c in range(4)]
def learn(dw, gw, didx, table):
    """declaracion: clave de uso -> (dword0, dword1 sin swizzle, dword2, remapeo de componentes)."""
    decl = {}
    gidx = [i for i in range(len(gw) // 3) if is_vfetch(gw[3*i])]
    usados = set()
    for k, i in enumerate(didx):
        key = (table[k] >> 12) & 0xFF
        a, b = dw[3*i], dw[3*i+1]
        ds = swz(b)
        cand = [j for j in gidx if j not in usados and ((gw[3*j] >> 12) & 63) == ((a >> 12) & 63)
                and [x == 7 for x in swz(gw[3*j+1])] == [x == 7 for x in ds]]
        # entre varias, la que este en la misma posicion relativa
        if not cand: return None
        if len(cand) > 1:
            cand.sort(key=lambda j: abs(j - i))
        j = cand[0]; usados.add(j)
        gs = swz(gw[3*j+1]); remap = {}
        for c in range(4):
            if ds[c] < 4:
                if remap.get(ds[c], gs[c]) != gs[c]: return None
                remap[ds[c]] = gs[c]
        decl[key] = (gw[3*j] & ~(7 << 27) & ~0x0007F000, gw[3*j+1] & ~0xFFF & ~(1 << 30), gw[3*j+2], remap)
    return decl
def bind(dw, didx, table, decl):
    w = list(dw)
    # grupos de vfetch consecutivos
    groups, cur = [], []
    for i in didx:
        if cur and i != cur[-1] + 1: groups.append(cur); cur = []
        cur.append(i)
    if cur: groups.append(cur)
    keyof = {i: (table[k] >> 12) & 0xFF for k, i in enumerate(didx)}
    for g in groups:
        ins = []
        for i in g:
            if keyof[i] not in decl: return None
            a0, b0, c0, remap = decl[keyof[i]]
            a = (dw[3*i] & 0x0007F000) | a0
            ds = swz(dw[3*i+1]); ns = 0
            for c in range(4):
                v = ds[c]
                if v < 4:
                    if v not in remap: return None
                    v = remap[v]
                ns |= v << (3 * c)
            b = ns | b0
            ins.append([a, b, c0])
        ins.sort(key=lambda t: (-((t[0] >> 20) & 127), (t[2] >> 8) & 0x7FFFFF))
        start = None; first = None
        for t in ins:
            off = (t[2] >> 8) & 0x7FFFFF; size = FMT_DW.get((t[1] >> 16) & 63, 1)
            if first is not None and (t[0] >> 20) == (first[0] >> 20) and off + size - start <= 8:
                t[1] |= 1 << 30
                end = max(end, off + size); count += 1
            else:
                if first is not None and count > 1: first[0] |= ((end - start - 1) & 7) << 27
                first, start, end, count = t, off, off + size, 1
        if count > 1: first[0] |= ((end - start - 1) & 7) << 27
        for i, t in zip(g, ins): w[3*i:3*i+3] = t
    return w
mej = pickle.load(open(S + "/mejores_vs.pkl", "rb"))
st = collections.Counter(); malos = []
for gh, (sc, dh, L) in mej.items():
    if sc < 0.5: st["sin pareja"] += 1; continue
    dw, gw = W(dv[dh]["ucode"]), game(gh)
    if len(dw) != len(gw): st["long distinta"] += 1; continue
    didx = disc_fetches(dw)
    table = fetch_table(dv[dh]["hdr"], [i + 0 for i in didx])
    if table is None: st["sin tabla"] += 1; continue
    decl = learn(dw, gw, didx, table)
    if decl is None: st["no aprende"] += 1; continue
    bw = bind(dw, didx, table, decl)
    if bw == gw: st["EXACTO"] += 1
    else:
        st["difiere"] += 1
        malos.append((gh, dh, [i // 3 for i in range(len(gw)) if bw[i] != gw[i]][:10]))
print(st)
for m in malos[:8]: print(m)
