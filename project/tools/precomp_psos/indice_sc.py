"""Indice completo de los ShaderCache del disco: sombreadores por GUID y grupos
(paquete, material, vertex factory) -> {tipo: GUID}."""
import os as _os
ROOT = _os.path.abspath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', '..'))
import sys, struct, pickle, time, collections, xxhash
sys.path.insert(0, _os.path.join(ROOT, 'project', 'tools'))
from lo_disco import Disco, Paquete
from multiprocessing import Pool
DISCO = _os.path.join(ROOT, 'disc1')
MAGIC = bytes([0x10, 0x2A, 0x11])

def parse(raw, p, e):
    N = p.nombres
    u = lambda q: struct.unpack_from(">I", raw, q)[0]
    nm = lambda q: N[u(q)] + ("" if not u(q + 4) else "_%d" % (u(q + 4) - 1))
    end = e.off + e.tam
    q = raw.find(MAGIC, e.off, end)
    if q < 0: return None
    q -= 34
    shaders = {}
    while q + 38 <= end and u(q) < len(N) and raw[q + 34:q + 37] == MAGIC:
        guid = raw[q + 8:q + 24]; skip = u(q + 24); ln = u(q + 30)
        blob = q + 34; t = raw[blob + 3]
        h1, h2 = struct.unpack_from(">II", raw, blob + 4)
        uc = raw[blob + h1 + 0x40: blob + h1 + h2]
        shaders[guid] = dict(type=N[u(q)], t=t, h=xxhash.xxh3_64_intdigest(uc), hdr=raw[blob:blob + h1 + 0x40],
                             ucode=uc)
        if skip <= q: break
        q = skip
    groups = []
    nmat = u(q); q += 4
    for _ in range(nmat):
        mguid = raw[q:q + 16]; q += 16
        q += 8; skip = u(q); q += 4; q += 4; nvf = u(q); q += 4
        def tmap(q):
            n = u(q); q += 4; m = {}
            for _ in range(n):
                m[nm(q)] = raw[q + 8:q + 24]; q += 32
            return m, q
        m0, q = tmap(q)
        for _ in range(nvf - 1):
            vf = nm(q); q += 8
            m, q = tmap(q)
            if m: groups.append((mguid, vf, m))
        groups.append((mguid, nm(q), m0))
        q += 8
        if raw[q:q + 16] != mguid: raise ValueError("cola de material inesperada")
        if q > skip: raise ValueError("desalineado %d" % (q - skip))
        q = skip
    return shaders, groups

def trabajo(n):
    dd = Disco(DISCO)
    try:
        raw = dd.leer(n); p = Paquete(raw)
    except Exception:
        return n, None, "lectura"
    out = []
    for e in p.exports:
        if e.clase != "ShaderCache": continue
        try:
            r = parse(raw, p, e)
            if r: out.append(r)
        except Exception as ex:
            return n, out, "parse: %r" % ex
    return n, out, None

if __name__ == "__main__":
    t0 = time.time()
    d = Disco(DISCO)
    paquetes = [n for n in d.ficheros if n.endswith(".xxx")]
    res = {}; errs = collections.Counter()
    with Pool(16) as pool:
        for k, (n, out, err) in enumerate(pool.imap_unordered(trabajo, paquetes, chunksize=8)):
            if err: errs[err.split(":")[0]] += 1; 
            if err and errs[err.split(":")[0]] < 4: print(n, err, flush=True)
            if out: res[n] = out
            if k % 1000 == 0: print(k, "%.0f s" % (time.time() - t0), flush=True)
    pickle.dump(res, open(sys.argv[1], "wb"))
    print("hecho", len(res), "paquetes con ShaderCache, errores", dict(errs), "%.0f s" % (time.time() - t0))
