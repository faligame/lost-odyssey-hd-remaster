"""Convierte las texturas reescaladas (PNG) al formato que carga el pack sin coste: DDS BC7 con sus mips.

Un PNG de 4096x4096 hay que decodificarlo (~0,4 s), calcularle los mips y subirlo sin comprimir (~85 MB de memoria
de vídeo). El mismo en BC7 con mips son ~22 MB y se sube tal cual del disco. El plugin (xenos1080_texture_replace.cpp)
admite los dos formatos; si una textura está en ambos, manda el DDS.

  python pack_a_dds.py --entrada "G:\\...\\texturas-hd" --salida "G:\\...\\textures\\hd" [--lado-max 2048] [--filtro nbn_0]
  python pack_a_dds.py --lista mapa_actual.txt --raiz-lista <dir color> --entrada texturas-hd --salida ...

Reanudable: lo ya convertido se salta. Las texturas con algún lado menor de 4 px se copian como PNG (BC va por
bloques de 4x4). Usa texconv.exe (DirectXTex, Microsoft), que comprime con la gráfica.
"""
import argparse
import collections
import os
import shutil
import subprocess
import sys
import time

from PIL import Image

AQUI = os.path.dirname(os.path.abspath(__file__))
TEXCONV = os.path.join(AQUI, "texconv.exe")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--entrada", required=True, help="carpeta con los PNG reescalados (se recorre entera)")
    ap.add_argument("--salida", required=True, help="carpeta del pack donde dejar los DDS (misma estructura)")
    ap.add_argument("--lado-max", type=int, default=0, help="reduce las que pasen de este lado (0 = sin tope)")
    ap.add_argument("--filtro", help="solo las rutas que contengan este texto")
    ap.add_argument("--lista", help="fichero con rutas (una por línea) relativas a --raiz-lista")
    ap.add_argument("--raiz-lista", help="carpeta a la que son relativas las rutas de --lista")
    ap.add_argument("--formato", default="BC7_UNORM")
    a = ap.parse_args()

    if a.lista:
        raiz = os.path.normpath(a.raiz_lista)
        with open(a.lista, encoding="utf-8") as fl:
            rel = [os.path.relpath(os.path.normpath(x.strip()), raiz) for x in fl if x.strip()]
        fich = [os.path.join(a.entrada, r) for r in rel if os.path.exists(os.path.join(a.entrada, r))]
    else:
        fich = [os.path.join(d, f) for d, _, fs in os.walk(a.entrada) for f in fs if f.lower().endswith(".png")]
    if a.filtro:
        fich = [f for f in fich if a.filtro in f]
    if not fich:
        sys.exit("sin PNG que convertir")

    # un lote de texconv por carpeta de salida y tamaño final (el tamaño solo se fuerza si hay que reducir)
    lotes = collections.defaultdict(list)
    hechas = copiadas = 0
    for f in sorted(fich):
        dst_dir = os.path.join(a.salida, os.path.dirname(os.path.relpath(f, a.entrada)))
        base = os.path.splitext(os.path.basename(f))[0]
        if os.path.exists(os.path.join(dst_dir, base + ".dds")) or os.path.exists(os.path.join(dst_dir, base + ".png")):
            hechas += 1
            continue
        with Image.open(f) as im:
            w, h = im.size
        if min(w, h) < 4:
            os.makedirs(dst_dir, exist_ok=True)
            shutil.copy2(f, dst_dir)
            copiadas += 1
            continue
        tam = None
        while a.lado_max and max(w, h) > a.lado_max and min(w, h) >= 8:
            w, h = w // 2, h // 2
            tam = (w, h)
        lotes[(dst_dir, tam)].append(f)

    t0 = time.time()
    n = 0
    for (dst_dir, tam), ficheros in lotes.items():
        os.makedirs(dst_dir, exist_ok=True)
        # -sepalpha: los mips no mezclan el color con la alfa; -m 0: cadena completa; -y: sobrescribe
        orden = [TEXCONV, "-nologo", "-y", "-f", a.formato, "-m", "0", "-sepalpha", "-gpu", "0", "-o", dst_dir]
        if tam:
            orden += ["-w", str(tam[0]), "-h", str(tam[1])]
        for i in range(0, len(ficheros), 40):
            r = subprocess.run(orden + ficheros[i:i + 40], capture_output=True, text=True)
            if r.returncode != 0:
                print("texconv falló en", dst_dir, "\n", r.stdout[-600:], r.stderr[-300:])
            n += len(ficheros[i:i + 40])
        print("%d/%d (%.0f s)" % (n, sum(len(v) for v in lotes.values()), time.time() - t0)); sys.stdout.flush()
    print("convertidas %d, copiadas en PNG %d, ya estaban %d, en %.0f s -> %s"
          % (n, copiadas, hechas, time.time() - t0, a.salida))


if __name__ == "__main__":
    main()
