# Clasifica los draws del volcado por lane: (vs, prim, vte) con el rango de
# posiciones v0 del slot 95 (x en [0..2000] plausible de pantalla).
import re, sys
from collections import defaultdict

path = sys.argv[1]
rx = re.compile(
    r"xenos1080 draw f(\d+) #\d+ vs=([0-9A-F]{16}) ps=([0-9A-F]{16}) prim=(\d+) n=(\d+) "
    r"vte=0x([0-9A-F]+) woff=\S+ vb=0x([0-9A-F]+) v0=\[([^\]]+)\]")

groups = defaultdict(list)
for ln in open(path, encoding="utf-8", errors="ignore"):
    m = rx.search(ln)
    if not m:
        continue
    vs, prim, n, vte, vb = m.group(2)[:8], m.group(4), m.group(5), m.group(6), m.group(7)
    v = m.group(8).split()
    try:
        x, y = float(v[0]), float(v[1])
    except (ValueError, IndexError):
        continue
    groups[(vs, prim, vte)].append((x, y, vb))

for key, items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
    xs = [i[0] for i in items]
    ys = [i[1] for i in items]
    vbs = sorted({i[2] for i in items})
    print("[%4d draws] vs=%s prim=%s vte=0x%s  x:[%.4g..%.4g] y:[%.4g..%.4g] vbs=%d (%s...)" % (
        len(items), key[0], key[1], key[2], min(xs), max(xs), min(ys), max(ys), len(vbs),
        ",".join(vbs[:3])))
