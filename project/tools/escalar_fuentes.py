"""Escala x4 (o xN) las paginas de fuente de Lost Odyssey para el pack de texturas.

Las paginas son glifos con cara blanca y contorno negro sobre transparente, con
bordes suavizados. Un escalador de IA emborrona los atlas de letras; aqui se usa
el suavizado como campo de distancia: se amplia con interpolacion (en alfa
premultiplicado, para que el color de los pixeles transparentes no manche) y se
vuelve a afilar cada borde en su sitio (smoothstep alrededor de 0.5). El
contorno conserva su grosor y el borde queda de ~1 pixel a la nueva escala.

Uso:
  python escalar_fuentes.py <carpeta dump/disc_textures> <carpeta textures del pack> [--escala 4] [--idioma spa]
  python escalar_fuentes.py ... --muestra recorte.png   (solo compara un recorte de Maru23)

Lee dump/disc_textures/indice.txt para encontrar las paginas (exports *_PageX de
los paquetes rpfonts*_<idioma>) y escribe tex_<HASH>_x4_<nombre>.png en
<textures>/fuentes/. El pack las reconoce por el prefijo tex_<HASH>.
"""

import argparse
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def ampliar(canal, escala):
    """Canal float 0..1 -> ampliado con Lanczos (PIL en modo F)."""
    h, w = canal.shape
    im = Image.fromarray(canal.astype(np.float32), mode="F")
    return np.asarray(im.resize((w * escala, h * escala), Image.LANCZOS), dtype=np.float32)


def desenfocar(canal, sigma):
    """Gaussiano separable (sigma en pixeles)."""
    if sigma <= 0:
        return canal
    r = int(np.ceil(3 * sigma))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    p = np.pad(canal, r, mode="edge")
    p = np.apply_along_axis(lambda v: np.convolve(v, k, "valid"), 1, p)
    return np.apply_along_axis(lambda v: np.convolve(v, k, "valid"), 0, p).astype(np.float32)


def escalar_pagina(rgba, escala, ancho_borde_px=1.25, suave_alfa=0.75, suave_color=0.3):
    """suave_* en pixeles ORIGINALES: quitan el ruido de la compresion DXT5 (la
    transparencia solo tiene 8 niveles por bloque de 4x4), que al afilar se veria
    como bordes ondulados. Solo se afila el borde exterior (alfa); el paso de la
    cara blanca al contorno oscuro (1 pixel en el original) se amplia suave con
    algo de contraste, o se lo comeria el afilado."""
    a = rgba[..., 3].astype(np.float32) / 255.0
    lum = rgba[..., :3].astype(np.float32).mean(axis=2) / 255.0
    # Alfa premultiplicado: luminancia * alfa, y se divide despues.
    a_up = ampliar(a, escala)
    la_up = ampliar(lum * a, escala)
    a_borde = desenfocar(a_up, suave_alfa * escala)
    a_color = desenfocar(a_up, suave_color * escala)
    la_color = desenfocar(la_up, suave_color * escala)
    lum_up = np.where(a_color > 1e-3, la_color / np.maximum(a_color, 1e-3), 0.0)

    # Un pixel original de transicion = `escala` pixeles nuevos; el borde final
    # mide ~ancho_borde_px pixeles nuevos.
    w = min(0.5, 0.5 * ancho_borde_px / escala)
    a_out = smoothstep(0.5 - w, 0.5 + w, a_borde)
    lum_out = smoothstep(0.45, 0.85, np.clip(lum_up, 0.0, 1.0))

    out = np.empty((a_out.shape[0], a_out.shape[1], 4), dtype=np.uint8)
    g = np.round(lum_out * 255.0).astype(np.uint8)
    out[..., 0] = out[..., 1] = out[..., 2] = g
    out[..., 3] = np.round(a_out * 255.0).astype(np.uint8)
    return out


def paginas(dump, idioma):
    """hash -> (nombre, png) de las paginas de fuente del idioma."""
    indice = dump / "indice.txt"
    # hash WxH fN discoN [tipo] paquete nombre (los indices antiguos no traen el tipo)
    patron = re.compile(r"^([0-9A-F]{16}) \d+x\d+ f\d+ disco\d+ (?:\S+ )?(\S+\.xxx) (\S+)$")
    elegidas = {}
    for linea in indice.read_text(encoding="utf-8", errors="replace").splitlines():
        m = patron.match(linea.strip())
        if not m:
            continue
        h, paquete, nombre = m.groups()
        paquete_min = paquete.lower().replace("\\", "/")
        if not re.search(r"/rpfonts[a-z]*_" + idioma + r"\.xxx$", paquete_min):
            continue
        if not re.search(r"_Page[A-Z]$", nombre):
            continue
        elegidas.setdefault(h, nombre)
    # El PNG esta donde se escribio la primera aparicion (cualquier idioma).
    pngs = {}
    for png in dump.rglob("tex_*.png"):
        h = png.name[4:20]
        if h in elegidas and h not in pngs:
            pngs[h] = png
    return {h: (elegidas[h], pngs[h]) for h in elegidas if h in pngs}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump", type=Path)
    ap.add_argument("textures", type=Path)
    ap.add_argument("--escala", type=int, default=4)
    ap.add_argument("--idioma", default="spa")
    ap.add_argument("--muestra", type=Path)
    args = ap.parse_args()

    encontradas = paginas(args.dump, args.idioma)
    if not encontradas:
        sys.exit("no hay paginas de fuente en el indice (vuelca primero las texturas del disco)")

    if args.muestra:
        h, (nombre, png) = next((k, v) for k, v in encontradas.items() if "Maru23" in v[0])
        rgba = np.asarray(Image.open(png).convert("RGBA"))
        recorte = rgba[0:48, 0:128]
        e = args.escala
        tam = (recorte.shape[1] * e, recorte.shape[0] * e)
        filas = [
            Image.fromarray(recorte).resize(tam, Image.NEAREST),
            Image.fromarray(recorte).resize(tam, Image.BICUBIC),
            Image.fromarray(escalar_pagina(recorte, e)),
        ]
        lienzo = Image.new("RGBA", (tam[0], tam[1] * 3), (70, 90, 120, 255))
        for i, f in enumerate(filas):
            lienzo.alpha_composite(f, (0, tam[1] * i))
        lienzo.save(args.muestra)
        print("muestra:", args.muestra, "(arriba original, medio bicubico, abajo este metodo)")
        return

    salida = args.textures / "fuentes"
    salida.mkdir(parents=True, exist_ok=True)
    for h, (nombre, png) in sorted(encontradas.items(), key=lambda kv: kv[1][0]):
        rgba = np.asarray(Image.open(png).convert("RGBA"))
        out = escalar_pagina(rgba, args.escala)
        destino = salida / f"tex_{h}_x{args.escala}_{nombre}.png"
        Image.fromarray(out).save(destino, optimize=True)
        print(f"{nombre:18s} {rgba.shape[1]}x{rgba.shape[0]} -> {out.shape[1]}x{out.shape[0]}  {destino.name}")
    print(len(encontradas), "paginas en", salida)


if __name__ == "__main__":
    main()
