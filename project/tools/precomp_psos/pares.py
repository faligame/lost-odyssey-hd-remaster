import struct, collections, sys
def xpso(path, rec):
    b = open(path, "rb").read(); out = []
    for p in range(12, len(b) - rec + 1, rec):
        vh, vm, ph, pm = struct.unpack_from("<4Q", b, p + 8)
        out.append((vh, vm, ph, pm, b[p + 40:p + rec]))
    return out
S = sys.argv[1]
for name, rec in (("4D5307FA.fsi.vk.xpso", 68), ("4D5307FA.rtv.d3d12.xpso", 72)):
    ps = xpso(S + "/" + name, rec)
    print("==", name, len(ps), "pipelines")
    vs = collections.Counter(v for v, *_ in ps); pp = collections.Counter(p for _, _, p, *_ in ps)
    print("VS distintos", len(vs), "PS distintos", len(pp), "VS mods", len(set(m for _, m, *_ in ps)), "PS mods", len(set(m for _, _, _, m, _ in ps)))
    print("PS mods:", collections.Counter("%X" % m for _, _, _, m, _ in ps).most_common(10))
    print("VS mods:", collections.Counter("%X" % m for _, m, _, _, _ in ps).most_common(10))
    print("estados distintos", len(set(r for *_, r in ps)))
    pv = collections.defaultdict(set); vp = collections.defaultdict(set)
    for v, vm, p, pm, r in ps: pv[p].add(v); vp[v].add(p)
    print("PS con n VS:", sorted(collections.Counter(len(x) for x in pv.values()).items()))
    print("VS con n PS:", sorted(collections.Counter(len(x) for x in vp.values()).items())[-8:])
