"""Crea el .lopack a partir de la carpeta de texturas (hd/*.dds + PNG de interfaz) y el disco 1.

  python crear_lopack.py --texturas <carpeta textures> --disco1 <carpeta con default.xex y LO.fpi>
         --salida textures.lopack [--nivel 19] [--hilos N] [--contenido 1] [--limite N]

Las imagenes se identifican por el prefijo tex_<16 hex> del nombre (la huella de la textura del juego).
Si una textura esta en DDS y en PNG, manda el DDS (igual que al cargar la carpeta suelta).
"""
import argparse
import multiprocessing as mp
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lopack_formato import *  # noqa

try:
    from compression import zstd
except Exception:
    from backports import zstd


def hash_de_nombre(nombre):
    if len(nombre) < 20 or not nombre.startswith("tex_"):
        return None
    try:
        return int(nombre[4:20], 16)
    except ValueError:
        return None


def trabajo(args):
    ruta, hash64, tipo, nivel = args
    with open(ruta, "rb") as f:
        crudo = f.read()
    comp = zstd.compress(crudo, level=nivel)
    if len(comp) < len(crudo):
        return hash64, tipo, len(crudo), CODEC_ZSTD, comp
    return hash64, tipo, len(crudo), CODEC_RAW, crudo


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--texturas", required=True)
    ap.add_argument("--disco1", required=True)
    ap.add_argument("--salida", required=True)
    ap.add_argument("--nivel", type=int, default=19)
    ap.add_argument("--hilos", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument("--contenido", type=int, default=1, help="version del contenido del pack")
    ap.add_argument("--limite", type=int, default=0, help="solo las N primeras (pruebas)")
    a = ap.parse_args()

    elegidas = {}
    for raiz, _, nombres in os.walk(a.texturas):
        for n in sorted(nombres):
            ext = os.path.splitext(n)[1].lower()
            if ext not in (".dds", ".png"):
                continue
            h = hash_de_nombre(n)
            if h is None:
                continue
            tipo = TIPO_DDS if ext == ".dds" else TIPO_PNG
            if h not in elegidas or tipo == TIPO_DDS:
                elegidas[h] = (os.path.join(raiz, n), tipo)
    trabajos = [(r, h, t, a.nivel) for h, (r, t) in sorted(elegidas.items())]
    if a.limite:
        trabajos = trabajos[: a.limite]
    print(f"{len(trabajos)} imagenes; nivel zstd {a.nivel}; {a.hilos} hilos")

    secreto = secreto_de_carpeta(a.disco1)
    sal = os.urandom(32)
    clave = derivar_clave(secreto, sal)
    aes = AESGCM(clave)

    entradas = []
    t0 = time.time()
    crudo_total = cifrado_total = 0
    with open(a.salida, "wb") as out:
        out.write(b"\x00" * HEADER_SIZE)  # se reescribe al final
        with mp.Pool(a.hilos) as pool:
            for i, (hash64, tipo, raw_size, codec, datos) in enumerate(pool.imap(trabajo, trabajos, chunksize=4)):
                cifrado = aes.encrypt(nonce_entrada(i), datos, aad_entrada(hash64, raw_size, tipo))
                off = out.tell()
                out.write(cifrado)
                entradas.append(struct.pack("<QQIIBBHI", hash64, off, len(cifrado), raw_size, tipo, codec, 0, 0))
                crudo_total += raw_size
                cifrado_total += len(cifrado)
                if (i + 1) % 250 == 0 or i + 1 == len(trabajos):
                    dt = time.time() - t0
                    print(f"  {i + 1}/{len(trabajos)}  {crudo_total / 1e9:.2f} GB -> {cifrado_total / 1e9:.2f} GB  "
                          f"({crudo_total / 1e6 / dt:.0f} MB/s)", flush=True)
        indice_off = out.tell()
        indice_plano = b"".join(entradas)
        indice_size = len(indice_plano) + 16
        h = cabecera(a.contenido, EDICION_USA_EUROPA_V3, sal, indice_off, indice_size, INDEX_NONCE, len(entradas))
        out.write(aes.encrypt(INDEX_NONCE, indice_plano, h))
        out.seek(0)
        out.write(h)
    print(f"listo: {a.salida} ({os.path.getsize(a.salida) / 1e9:.2f} GB, {len(entradas)} entradas) en "
          f"{time.time() - t0:.0f} s")


if __name__ == "__main__":
    mp.freeze_support()
    main()
