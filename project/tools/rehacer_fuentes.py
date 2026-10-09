"""Rehace los atlas de letras de Lost Odyssey a xN con la tipografia vectorial.

Las fuentes del juego son atlas pre-renderizados (UE3 Font): cada caracter es un
rectangulo en una pagina Texture2D, con la cara blanca y un contorno negro de
~1,3 px. Las letras latinas son Trebuchet MS (Maru23, Meiyo18/26, LocTit1/2),
Trebuchet MS Bold (BigNum) y Arial Bold (Arial18, Abc); se identificaron
comparando formas con las fuentes instaladas (ver --identificar).

Por cada fuente se ajustan cuatro valores comparando con el original a 1x:
tamano (em en px), linea base, grosor extra del trazo, radio del contorno y su
opacidad. Por
cada letra, su posicion horizontal (centroide). Despues se dibuja cada letra a
xN en su celda: se renderiza el contorno vectorial (con supermuestreo), se
calcula la distancia al borde y de ahi salen la cara y el contorno con un borde
de 1 px limpio. Las celdas que no son un caracter de la tipografia (el cuadro de
"caracter desconocido", iconos) se quedan con el escalado de escalar_fuentes.py.

Uso:
  python rehacer_fuentes.py <disco1> <dump/disc_textures> <textures del pack> [--idioma spa] [--escala 4]
         [--solo Maru23] [--muestra comparacion.png] [--identificar]

Las tipografias se buscan en C:\\Windows\\Fonts y en las fuentes del usuario;
solo se usan en local para generar el pack (no se copian a ningun sitio).
"""

import argparse
import os
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage, optimize

sys.path.insert(0, str(Path(__file__).parent))
import escalar_fuentes  # noqa: E402
import lo_disco  # noqa: E402

# Fuente del juego -> tipografia (identificadas con --identificar, 29-sep-2026).
TIPOGRAFIAS = {
    "Maru23": "trebuc.ttf",
    "Meiyo18": "trebuc.ttf",
    "Meiyo26": "trebuc.ttf",
    "LocTit1": "trebuc.ttf",
    "LocTit2": "trebuc.ttf",
    "BigNum": "trebucbd.ttf",
    "Arial18": "arialbd.ttf",
    "Abc": "arialbd.ttf",
}

SS = 2  # supermuestreo al renderizar
MARGEN = 4  # px (1x) a cada lado de la celda al renderizar


def carpetas_de_fuentes():
    yield Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts"
    yield Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft/Windows/Fonts"


def buscar_tipografia(nombre):
    for c in carpetas_de_fuentes():
        for p in c.glob("*"):
            if p.name.lower() == nombre.lower():
                return p
    return None


# --- Modelo de una letra ----------------------------------------------------------
class Letra:
    """Renderiza un caracter y lo convierte en (alfa, cara premultiplicada) a k x."""

    def __init__(self, ruta_ttf):
        self.ruta = str(ruta_ttf)
        self._fuentes = {}
        from fontTools.ttLib import TTFont

        tt = TTFont(self.ruta, fontNumber=0)
        self.cmap = tt.getBestCmap()
        os2 = tt["OS/2"]
        self.cap = getattr(os2, "sCapHeight", 0) / tt["head"].unitsPerEm or 0.7

    def tiene(self, cp):
        return cp in self.cmap

    def _fuente(self, tam):
        clave = round(tam * 8) / 8
        f = self._fuentes.get(clave)
        if f is None:
            f = ImageFont.truetype(self.ruta, clave)
            self._fuentes[clave] = f
        return f

    def distancia(self, ch, w, h, k, s, base, pen):
        """Distancia con signo al borde (en px a k x; negativa dentro), lienzo con margen."""
        q = k * SS
        W, H = (w + 2 * MARGEN) * q, h * q
        im = Image.new("L", (int(W), int(H)))
        ImageDraw.Draw(im).text(((MARGEN + pen) * q, base * q), ch, font=self._fuente(s * q), fill=255, anchor="ls")
        dentro = np.asarray(im) > 127
        if not dentro.any():
            return None
        d = np.where(dentro, -ndimage.distance_transform_edt(dentro) + 0.5,
                     ndimage.distance_transform_edt(~dentro) - 0.5)
        # a k x: media de bloques SSxSS, en px de k x
        d = d.reshape(d.shape[0] // SS, SS, d.shape[1] // SS, SS).mean(axis=(1, 3)) / SS
        return d

    @staticmethod
    def capas(d, k, grosor, contorno, opacidad=1.0):
        """Cara blanca y contorno negro; opacidad = cuanto tapa el contorno (algunas
        fuentes, como la de los creditos, lo llevan gris y tenue)."""
        cara = np.clip(0.5 - (d - grosor * k), 0.0, 1.0)
        borde = np.clip(0.5 - (d - (grosor + contorno) * k), 0.0, 1.0)
        alfa = cara + (borde - cara) * opacidad
        return alfa, cara


def reducir(x, k):
    h, w = x.shape
    return x.reshape(h // k, k, w // k, k).mean(axis=(1, 3))


def recortar(x, k, w):
    """Quita el margen del lienzo (a k x)."""
    return x[:, MARGEN * k:(MARGEN + w) * k]


# --- Ajuste -------------------------------------------------------------------------
class Ajuste:
    def __init__(self, letra, fuente, paginas):
        self.letra = letra
        self.f = fuente
        self.paginas = paginas  # float 0..1 RGBA
        self.celdas = {}
        for cp, ix in fuente.mapa.items():
            x, y, w, h, pg = fuente.glifos[ix]
            c = paginas[pg][y:y + h, x:x + w]
            alfa = c[..., 3]
            cara = c[..., :3].mean(axis=2) * alfa
            if cara.sum() < 1 or not letra.tiene(cp):
                continue
            self.celdas[cp] = (ix, alfa, cara)
        self.pen = {}

    def centroide_x(self, m):
        tot = m.sum()
        return (m.sum(axis=0) * np.arange(m.shape[1])).sum() / tot if tot > 0 else 0.0

    def colocar(self, cp, s, base, k=1):
        """Posicion horizontal que alinea el centroide de la cara con el original."""
        ix, alfa_o, cara_o = self.celdas[cp]
        h, w = alfa_o.shape
        d = self.letra.distancia(chr(cp), w, h, 4, s, base, 0.0)
        if d is None:
            return None
        _, cara = Letra.capas(d, 4, 0.0, 0.0)
        cara = reducir(cara, 4)
        cx_r = self.centroide_x(cara) - MARGEN
        return self.centroide_x(cara_o) - cx_r

    def error(self, params, cps):
        s, base, grosor, contorno, opacidad = params
        if s <= 2 or contorno < 0 or not 0.0 <= opacidad <= 1.0:
            return 1e9
        total = 0.0
        for cp in cps:
            ix, alfa_o, cara_o = self.celdas[cp]
            h, w = alfa_o.shape
            pen = self.colocar(cp, s, base)
            if pen is None:
                return 1e9
            d = self.letra.distancia(chr(cp), w, h, 4, s, base, pen)
            alfa, cara = Letra.capas(d, 4, grosor, contorno, opacidad)
            alfa = reducir(recortar(alfa, 4, w), 4)
            cara = reducir(recortar(cara, 4, w), 4)
            total += ((alfa - alfa_o) ** 2).mean() + ((cara - cara_o) ** 2).mean()
        return total / len(cps)

    def ajustar(self):
        # Estimacion inicial con la H: altura de mayusculas y su fila inferior.
        cp_h = ord("H") if ord("H") in self.celdas else next(iter(self.celdas))
        _, alfa_o, cara_o = self.celdas[cp_h]
        filas = np.nonzero(cara_o.max(axis=1) > 0.5)[0]
        alto = filas.max() - filas.min() + 1
        s0 = alto / max(self.letra.cap, 0.5)
        base0 = filas.max() + 1.0
        muestra = [ord(c) for c in "HIOnoaegxkM13&" if ord(c) in self.celdas]
        if not muestra:
            return None, None
        ini = [s0, base0, 0.0, 1.3, 0.95]
        simplex = [ini]
        for i, paso in enumerate([s0 * 0.05, 0.5, 0.3, 0.4, -0.3]):
            v = list(ini)
            v[i] += paso
            simplex.append(v)
        r = optimize.minimize(lambda p: self.error(p, muestra), ini, method="Nelder-Mead",
                              options={"xatol": 0.02, "fatol": 1e-6, "maxiter": 600, "initial_simplex": simplex})
        return r.x, r.fun


# --- Construccion de las paginas -----------------------------------------------------
def rehacer(fuente, letra, params, paginas_u8, k):
    s, base, grosor, contorno, opacidad = params
    ajuste = Ajuste(letra, fuente, [p.astype(np.float32) / 255.0 for p in paginas_u8])
    # fondo: el escalado suave de siempre (para lo que no se redibuja)
    salida = [escalar_fuentes.escalar_pagina(p, k) for p in paginas_u8]
    errores = []
    for cp, (ix, alfa_o, cara_o) in ajuste.celdas.items():
        x, y, w, h, pg = fuente.glifos[ix]
        pen = ajuste.colocar(cp, s, base)
        d = letra.distancia(chr(cp), w, h, k, s, base, pen)
        alfa, cara = Letra.capas(d, k, grosor, contorno, opacidad)
        alfa = recortar(alfa, k, w)
        cara = recortar(cara, k, w)
        lum = np.where(alfa > 1e-4, cara / np.maximum(alfa, 1e-4), 0.0)
        celda = salida[pg][y * k:(y + h) * k, x * k:(x + w) * k]
        g = np.round(np.clip(lum, 0, 1) * 255).astype(np.uint8)
        celda[..., 0] = celda[..., 1] = celda[..., 2] = g
        celda[..., 3] = np.round(alfa * 255).astype(np.uint8)
        err = ((reducir(alfa, k) - alfa_o) ** 2).mean() + ((reducir(cara, k) - cara_o) ** 2).mean()
        errores.append((err, chr(cp)))
    return salida, sorted(errores, reverse=True), len(ajuste.celdas)


# --- Identificacion -----------------------------------------------------------------
def identificar(fuente, paginas_u8, top=5):
    """Compara formas de letras con todas las tipografias instaladas."""
    from escalar_fuentes import np as _np  # noqa: F401

    def ajustado(m):
        ys, xs = np.nonzero(m)
        return None if len(xs) == 0 else m[ys.min():ys.max() + 1, xs.min():xs.max() + 1]

    def normal(m, S=40):
        h, w = m.shape
        e = S / max(h, w)
        im = Image.fromarray((m * 255).astype(np.uint8)).resize((max(1, round(w * e)), max(1, round(h * e))),
                                                                Image.BILINEAR)
        c = np.zeros((S, S))
        a = np.asarray(im) / 255
        c[:a.shape[0], :a.shape[1]] = a
        return c, h / w

    orig = {}
    for ch in "ABCDEGHKMQRSWabdegkmqrsty123457&%$?@":
        ix = fuente.mapa.get(ord(ch))
        if ix is None:
            continue
        x, y, w, h, pg = fuente.glifos[ix]
        c = paginas_u8[pg][y:y + h, x:x + w].astype(float) / 255
        t = ajustado((c[..., 3] > 0.5) & (c[..., :3].mean(2) > 0.5))
        if t is not None:
            orig[ch] = normal(t.astype(float))
    if len(orig) < 8:
        return []  # no son letras (p. ej. la fuente de iconos de botones)
    res = []
    for carpeta in carpetas_de_fuentes():
        for p in carpeta.glob("*"):
            if p.suffix.lower() not in (".ttf", ".otf", ".ttc"):
                continue
            try:
                ft = ImageFont.truetype(str(p), 120)
            except Exception:
                continue
            sc = []
            for ch, (o, ar) in orig.items():
                im = Image.new("L", (260, 220))
                try:
                    ImageDraw.Draw(im).text((40, 30), ch, font=ft, fill=255)
                except Exception:
                    sc.append(0)
                    continue
                t = ajustado(np.asarray(im) > 127)
                if t is None:
                    sc.append(0)
                    continue
                n, ar2 = normal(t.astype(float))
                sc.append(np.minimum(n, o).sum() / (np.maximum(n, o).sum() + 1e-6) * np.exp(-abs(np.log(ar / ar2))))
            res.append((float(np.mean(sc)), " ".join(ft.getname()), p.name))
    res.sort(reverse=True)
    return res[:top]


# --- Principal ----------------------------------------------------------------------
def leer_indice(dump):
    """(paquete en minusculas, export) -> hash"""
    patron = re.compile(r"^([0-9A-F]{16}) \d+x\d+ f\d+ disco\d+ (?:\S+ )?(\S+\.xxx) (\S+)$")
    out = {}
    for linea in (dump / "indice.txt").read_text(encoding="utf-8", errors="replace").splitlines():
        m = patron.match(linea.strip())
        if m:
            out.setdefault((m.group(2).lower(), m.group(3)), m.group(1))
    return out


def png_de_hash(dump, h, cache={}):
    if not cache:
        for p in dump.rglob("tex_*.png"):
            cache.setdefault(p.name[4:20], p)
    return cache.get(h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("disco", type=Path)
    ap.add_argument("dump", type=Path)
    ap.add_argument("textures", type=Path)
    ap.add_argument("--idioma", default="spa")
    ap.add_argument("--escala", type=int, default=4)
    ap.add_argument("--solo")
    ap.add_argument("--muestra", type=Path)
    ap.add_argument("--identificar", action="store_true")
    args = ap.parse_args()

    disco = lo_disco.Disco(args.disco)
    indice = leer_indice(args.dump)
    salida = args.textures / "fuentes"
    salida.mkdir(parents=True, exist_ok=True)
    k = args.escala

    for familia in ("rpfontscommon", "rpfontstitle"):
        ruta = f"bin\\xenon\\loc\\{args.idioma}\\menu\\{familia}_{args.idioma}.xxx".replace("\\", lo_disco.BS)
        if ruta not in disco.ficheros:
            continue
        pkg = disco.paquete(ruta)
        for e in pkg.exports:
            if e.clase != "Font" or (args.solo and e.nombre != args.solo):
                continue
            fuente = pkg.fuente(e)
            hashes = [indice.get((ruta.lower(), n)) for n in fuente.paginas]
            if not all(hashes):
                print(f"{e.nombre}: faltan paginas en el volcado")
                continue
            paginas = [np.asarray(Image.open(png_de_hash(args.dump, h)).convert("RGBA")) for h in hashes]
            if args.identificar or e.nombre not in TIPOGRAFIAS:
                mejores = identificar(fuente, paginas)
                print(f"{e.nombre}: " + "; ".join(f"{n} ({f}) {s:.3f}" for s, n, f in mejores))
                if args.identificar:
                    continue
                if not mejores or mejores[0][0] < 0.65:
                    print(f"{e.nombre}: sin tipografia clara, se deja")
                    continue
                # La silueta sola no siempre decide: se ajusta con las tres
                # mejores y gana la que menos error deja.
                candidatas = [m[2] for m in mejores[:3]]
            else:
                candidatas = [TIPOGRAFIAS[e.nombre]]
            elegida = None
            for nombre_ttf in candidatas:
                ttf = buscar_tipografia(nombre_ttf)
                if ttf is None:
                    print(f"{e.nombre}: no esta instalada {nombre_ttf}")
                    continue
                letra = Letra(ttf)
                ajuste = Ajuste(letra, fuente, [p.astype(np.float32) / 255.0 for p in paginas])
                params, err = ajuste.ajustar()
                if params is None:
                    continue
                if len(candidatas) > 1:
                    print(f"   {e.nombre} con {ttf.name}: error {err:.4f}")
                if elegida is None or err < elegida[3]:
                    elegida = (ttf, letra, params, err)
            if elegida is None:
                print(f"{e.nombre}: sin letras que ajustar, se deja")
                continue
            ttf, letra, params, err = elegida
            nuevas, errores, n = rehacer(fuente, letra, params, paginas, k)
            print(f"{e.nombre}: {ttf.name} tamano {params[0]:.2f} base {params[1]:.2f} trazo {params[2]:+.2f} "
                  f"contorno {params[3]:.2f} opacidad {params[4]:.2f} | error {err:.4f} | {n} letras redibujadas; peores: "
                  + " ".join(f"{c}={x:.3f}" for x, c in errores[:6]))
            for nombre_pg, h, im in zip(fuente.paginas, hashes, nuevas):
                Image.fromarray(im).save(salida / f"tex_{h}_x{k}_{nombre_pg}.png", optimize=True)
            if args.muestra:
                ix = fuente.mapa[ord("A")]
                x, y, w, h, pg = fuente.glifos[ix]
                y0, y1 = y, min(y + h * 2, paginas[pg].shape[0])
                x0, x1 = 0, min(paginas[pg].shape[1], 360 if paginas[pg].shape[1] >= 360 else paginas[pg].shape[1])
                orig = Image.fromarray(paginas[pg][y0:y1, x0:x1]).resize(((x1 - x0) * k, (y1 - y0) * k),
                                                                        Image.BICUBIC)
                nuevo = Image.fromarray(nuevas[pg][y0 * k:y1 * k, x0 * k:x1 * k])
                c = Image.new("RGBA", (orig.width, orig.height * 2), (40, 50, 70, 255))
                c.alpha_composite(orig, (0, 0))
                c.alpha_composite(nuevo, (0, orig.height))
                destino = args.muestra.with_name(f"{args.muestra.stem}_{e.nombre}{args.muestra.suffix}")
                c.save(destino)
                print("   muestra:", destino)


if __name__ == "__main__":
    main()
