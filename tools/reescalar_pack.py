"""Reescala las texturas de color del volcado de los discos (dump/disc_textures/color) para el pack de texturas.

Usa las funciones del skill reescalar-texturas (reescalar_lote.py) y añade lo que pide este juego:
  - la alfa se conserva SUAVE (Lanczos), no umbralizada: el pelo y las DXT5 traen degradados
  - tope de lado: una textura no pasa de --lado-max (las de 2048 salen a x2 en vez de x4)
  - se saltan los mapas de luz/sombra y las categorías que no se piden
  - se saltan los mapas que no son de color: *_TM, *_T y los *_S en blanco y negro (ver excluida())
  - reanudable (lo ya hecho se salta) y un fichero PARAR en la carpeta de salida lo detiene

  python reescalar_pack.py --salida "G:\\...\\texturas-hd" [--categorias chr field world obj battle] [--filtro pc_000a0]
"""
import argparse
import glob
import os
import re
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.expanduser(r"~\.claude\skills\reescalar-texturas"))
import reescalar_lote as rl  # noqa: E402

AQUI = os.path.dirname(os.path.abspath(__file__))
ENTRADA = os.path.join(AQUI, "..", "project", "out", "build", "win-amd64-release", "dump", "disc_textures", "color")
MODELO = __import__("os").environ.get("LO_MODELO_PBRIFY", "4x-PBRify_UpscalerDAT2_V1.pth")  # ruta al modelo .pth
EXCLUIR = re.compile(r"LightMap|ShadowMap", re.I)


SUFIJO = re.compile(r"_(TM|T|S)$", re.I)


def excluida(ruta):
    """Mapas que no son de color: los _TM y _T siempre; los _S (especular) solo si van en blanco y negro.

    Un _S "en gris" no es gris exacto: la compresión DXT le deja algo de tinte. Se mide la diferencia media entre el
    canal más alto y el más bajo sobre una miniatura; en el volcado los grises quedan por debajo de 4 y los de color
    por encima de 8, con pocos casos en medio."""
    m = SUFIJO.search(os.path.splitext(os.path.basename(ruta))[0])
    if not m:
        return False
    if m.group(1).upper() != "S":
        return True
    a = np.asarray(Image.open(ruta).convert("RGB").resize((64, 64), Image.BOX)).astype(int)
    d = a.max(2) - a.min(2)
    return d.mean() < 4 and np.percentile(d, 95) < 10


def una(m, usa16, ruta, lado_max, tesela, solape):
    a = np.asarray(Image.open(ruta).convert("RGBA"))
    h, w = a.shape[:2]
    escala = 4
    while escala > 1 and max(w, h) * escala > lado_max:
        escala //= 2
    if escala == 1:
        return None
    alfa = a[..., 3]
    opaco = alfa > 0
    if not opaco.any():
        return None
    cur = rl.pasar(m, usa16, rl.rellenar_huecos(a[..., :3].copy(), opaco), tesela, solape)
    if cur.shape[1] != w * escala:
        cur = np.asarray(Image.fromarray(cur).resize((w * escala, h * escala), Image.LANCZOS))
    if alfa.min() == 255:
        al = np.full(cur.shape[:2], 255, np.uint8)
    else:
        al = np.asarray(Image.fromarray(alfa).resize((w * escala, h * escala), Image.LANCZOS))
    return Image.fromarray(np.dstack([cur, al]), "RGBA")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--entrada", default=ENTRADA)
    ap.add_argument("--salida", required=True)
    ap.add_argument("--modelo", default=MODELO)
    ap.add_argument("--categorias", nargs="+", default=["chr", "field", "world", "obj", "battle"])
    ap.add_argument("--filtro", help="solo las rutas que contengan este texto")
    ap.add_argument("--lista", help="fichero con las rutas a reescalar (una por línea, dentro de --entrada)")
    ap.add_argument("--lado-max", type=int, default=4096)
    ap.add_argument("--tesela", type=int, default=192)
    ap.add_argument("--solape", type=int, default=16)
    ap.add_argument("--cada", type=int, default=50)
    a = ap.parse_args()

    fich = []
    if a.lista:
        with open(a.lista, encoding="utf-8") as fl:
            fich = [os.path.normpath(x.strip()) for x in fl if x.strip()]
        # la categoría es la PRIMERA carpeta bajo la entrada (vfx\field no es field)
        fich = [f for f in fich if os.path.relpath(f, a.entrada).split(os.sep)[0] in a.categorias]
    else:
        for c in a.categorias:
            fich += sorted(glob.glob(os.path.join(a.entrada, c, "**", "*.png"), recursive=True))
    fich = [f for f in fich if not EXCLUIR.search(os.path.basename(f)) and (not a.filtro or a.filtro in f)]
    if not fich:
        sys.exit("sin texturas que reescalar")
    os.makedirs(a.salida, exist_ok=True)
    m = usa16 = None
    hechos = saltados = omitidas = fuera = 0
    t0 = time.time()
    for i, f in enumerate(fich):
        if os.path.exists(os.path.join(a.salida, "PARAR")):
            print("PARAR: detenido en %d/%d" % (i, len(fich))); break
        out = os.path.join(a.salida, os.path.relpath(f, a.entrada))
        if os.path.exists(out):
            saltados += 1; continue
        if excluida(f):
            fuera += 1; continue
        if m is None:
            m, usa16 = rl.cargar(a.modelo, False)
        y = una(m, usa16, f, a.lado_max, a.tesela, a.solape)
        if y is None:
            omitidas += 1; continue
        os.makedirs(os.path.dirname(out), exist_ok=True)
        y.save(out + ".tmp", "PNG", compress_level=4)
        os.replace(out + ".tmp", out)
        hechos += 1
        if hechos % a.cada == 0:
            print("%d/%d (%.0f s)" % (i + 1, len(fich), time.time() - t0)); sys.stdout.flush()
    print("hechas %d, ya estaban %d, omitidas %d, fuera por no ser de color %d, total %d en %.0f s -> %s"
          % (hechos, saltados, omitidas, fuera, len(fich), time.time() - t0, a.salida))


if __name__ == "__main__":
    main()
