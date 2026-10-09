"""Extrae todos los bloques de sombreador (0x102A1100 PS / 0x102A1101 VS) del
disco 1 con contexto: paquete, export que lo contiene, cabecera y microcodigo."""
import os as _os
ROOT = _os.path.abspath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', '..'))
import sys, re, struct, pickle, time, xxhash
sys.path.insert(0, _os.path.join(ROOT, 'project', 'tools'))
from lo_disco import Disco, Paquete
from multiprocessing import Pool
DISCO = _os.path.join(ROOT, 'disc1')

def trabajo(n):
    dd = Disco(DISCO)
    try:
        raw = dd.leer(n)
    except Exception as ex:
        return n, None, None
    exps = []
    try:
        p = Paquete(raw)
        exps = [(e.off, e.tam, e.clase, e.nombre) for e in p.exports]
    except Exception:
        pass
    out = []
    for m in re.finditer(re.escape(bytes([0x10, 0x2A, 0x11])), raw):
        j = m.start()
        if j + 64 > len(raw) or raw[j + 3] not in (0, 1):
            continue
        h1, h2 = struct.unpack_from(">II", raw, j + 4)
        start, size = j + h1 + 0x40, h2 - 0x40
        if not (0 < h1 < 0x10000 and 0 < size < 0x40000 and size % 12 == 0 and start + size <= len(raw)):
            continue
        ex = next(((c, nm, o) for (o, t, c, nm) in exps if o <= j < o + t), None)
        ucode = raw[start:start + size]
        hdr = raw[j:j + h1 + 0x40]
        out.append(dict(t=raw[j + 3], off=j, ex=ex, hdr=hdr, h=xxhash.xxh3_64_intdigest(ucode),
                        ucode=ucode if raw[j + 3] == 1 else None))
    return n, out, len(exps)

if __name__ == "__main__":
    t0 = time.time()
    d = Disco(DISCO)
    paquetes = [n for n in d.ficheros if n.endswith(".xxx")]
    res = {}
    with Pool(16) as pool:
        for k, (n, out, ne) in enumerate(pool.imap_unordered(trabajo, paquetes, chunksize=8)):
            if out:
                res[n] = out
            if k % 1000 == 0:
                print(k, "%.0f s" % (time.time() - t0), flush=True)
    pickle.dump(res, open(sys.argv[1], "wb"))
    print("hecho", len(res), "paquetes con sombreadores, %.0f s" % (time.time() - t0))
