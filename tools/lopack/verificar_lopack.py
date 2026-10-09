"""Verifica un .lopack: abre con los discos, descifra todas las entradas y las compara con la carpeta suelta.

  python verificar_lopack.py --pack textures.lopack --disco1 <carpeta> [--texturas <carpeta>] [--muestra N]
"""
import argparse
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lopack_formato import *  # noqa


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", required=True)
    ap.add_argument("--disco1", required=True)
    ap.add_argument("--texturas", default="")
    ap.add_argument("--muestra", type=int, default=0, help="comprobar solo N entradas al azar (0 = todas)")
    a = ap.parse_args()
    p = Pack(a.pack, secreto_de_carpeta(a.disco1))
    print(f"formato {p.formato}, contenido {p.version}, edicion {p.edicion}, {p.n} entradas: indice abierto")
    claves = list(p.entradas)
    if a.muestra:
        random.shuffle(claves)
        claves = claves[: a.muestra]
    sueltos = {}
    if a.texturas:
        for raiz, _, nombres in os.walk(a.texturas):
            for n in nombres:
                if n.startswith("tex_") and os.path.splitext(n)[1].lower() in (".dds", ".png"):
                    h = int(n[4:20], 16)
                    if h not in sueltos or n.lower().endswith(".dds"):
                        sueltos[h] = os.path.join(raiz, n)
    mal = 0
    for i, h in enumerate(claves):
        datos, tipo = p.leer(h)
        if sueltos:
            with open(sueltos[h], "rb") as f:
                if f.read() != datos:
                    mal += 1
                    print("DISTINTO:", f"{h:016X}")
        if (i + 1) % 1000 == 0:
            print(f"  {i + 1}/{len(claves)}", flush=True)
    print(f"verificadas {len(claves)} entradas, {mal} distintas")
    try:
        Pack(a.pack, b"\x00" * 32)
        print("ERROR: abre con una clave falsa")
    except Exception as e:
        print("con una clave falsa no abre:", type(e).__name__)
    sys.exit(1 if mal else 0)


if __name__ == "__main__":
    main()
