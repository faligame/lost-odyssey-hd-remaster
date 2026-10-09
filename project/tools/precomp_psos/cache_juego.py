"""Lee la cache del plugin: .xsh (microcodigo visto) y .xpso (pipelines vistos)."""
import os as _os
ROOT = _os.path.abspath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', '..'))
import struct, collections
R = _os.path.join(ROOT, 'project', 'out', 'build', 'win-amd64-release', 'cache', 'shaders', 'shareable')
def xsh():
    b = open(R + r"\4D5307FA.xsh", "rb").read(); p = 8; out = {}
    while p + 12 <= len(b):
        h, w = struct.unpack_from("<QI", b, p); n = w & 0x7FFFFFFF; t = w >> 31; p += 12
        u = b[p:p + 4 * n]; p += 4 * n
        be = struct.pack(">%dI" % n, *struct.unpack("<%dI" % n, u))
        out[h] = ("frag" if t == 1 else "vert", be)
    return out
def xpso(name="4D5307FA.rov.d3d12.xpso"):
    b = open(R + "\\" + name, "rb").read(); out = []
    for p in range(12, len(b) - 71, 72):
        vh, vm, ph, pm = struct.unpack_from("<4Q", b, p + 8)
        rest = b[p + 40:p + 72]
        out.append((vh, vm, ph, pm, rest))
    return out
if __name__ == "__main__":
    s = xsh(); print(collections.Counter(t for t, _ in s.values()))
    ps = xpso(); print(len(ps), "pipelines")
    vs = collections.Counter(v for v, *_ in ps); pp = collections.Counter(p for _, _, p, *_ in ps)
    print("VS distintos", len(vs), "PS distintos", len(pp))
    print("VS mods distintos", len(set(m for _, m, *_ in ps)), "PS mods distintos", len(set(m for *_, m, _ in ps)))
    print("PS mods:", collections.Counter("%X" % m for _, _, _, m, _ in ps).most_common(12))
    print("estados distintos", len(set(r for *_, r in ps)))
    pairs = collections.defaultdict(set)
    for v, vm, p, pm, r in ps: pairs[p].add(v)
    print("PS con n VS:", collections.Counter(len(x) for x in pairs.values()))
