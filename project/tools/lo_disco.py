"""Lectura de los datos del disco de Lost Odyssey desde Python (herramientas).

Misma cadena que src/ue3_package.h (codigo propio, formatos documentados
publicamente): LO.fpi (indice) -> xenon_*.fpd -> bloque "cpx" -> paquete
Unreal Engine 3 big-endian -> exports. Incluye el objeto Font (tabla de
caracteres sobre paginas Texture2D).

    disco = Disco("ruta/al/disco1")
    pkg = disco.paquete("bin\\xenon\\loc\\spa\\menu\\rpfontscommon_spa.xxx")
    for e in pkg.exports:
        if e.clase == "Font":
            f = pkg.fuente(e)
"""

import struct
from dataclasses import dataclass
from pathlib import Path

BS = chr(92)
_ALFABETO = "\0" + "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_." + BS
_SUFIJOS = ["", "_sndw", "_scrw", "_mapw", "_lvdw", "_navw", "_colw", "_camw", "_map", "_cam", "_bx", "_mw",
            "_a", "_d", "_f", "_m", "_p", "_u", "_w", "_0", "_1", "_2", "_00", "_01", "_0mw", "_nav", "_elgt",
            "_000a0", "_010a0", "_020a0", "_030a0", "_040a0"]


def _le16(b, p):
    return struct.unpack_from("<H", b, p)[0]


def _le32(b, p):
    return struct.unpack_from("<I", b, p)[0]


def _be32(b, p):
    return struct.unpack_from(">I", b, p)[0]


def _bes32(b, p):
    return struct.unpack_from(">i", b, p)[0]


# --- LO.fpi -----------------------------------------------------------------
class Fpi:
    def __init__(self, datos):
        self.b = datos
        self.dic = _le32(datos, 40)
        self.ext = _le32(datos, 44)

    def _unpack(self, off):
        b = self.b
        w = _le16(b, off)
        out = _ALFABETO[w // 40 % 40] + _ALFABETO[w // 1600]
        for i in range(w % 40):
            x = _le16(b, off + 2 + 2 * i)
            out += _ALFABETO[x % 40] + _ALFABETO[x // 40 % 40] + _ALFABETO[x // 1600]
        return out.split("\0")[0].lower()

    def _nombre(self, bits):
        if not bits & 0x3FFFF:
            return ""
        n = self._unpack(self.dic + (bits & 0x3FFFF) * 2) + _SUFIJOS[(bits >> 18) & 31]
        e = (bits >> 23) & 31
        if e:
            n += "." + self._unpack(self.dic + _le16(self.b, self.ext + (e - 1) * 2) * 2)
        return n

    def ficheros(self):
        """ruta -> (archivo .fpd, offset, tamano)"""
        b = self.b
        res = {}
        inicio = _le32(b, 32)
        for i in range(_le16(b, 26)):
            ar = inicio + i * 48
            archivo = self._nombre(_le32(b, ar + 24))
            base = ar + _le32(b, ar + 4)
            prefijo = self._nombre(_le32(b, ar + 20))
            pend = [(0, _le16(b, ar + 2), prefijo + BS if prefijo else "")]
            vistos = set()
            while pend:
                idx0, n, ruta = pend.pop()
                for j in range(n):
                    idx = idx0 + j
                    if idx in vistos:
                        continue
                    vistos.add(idx)
                    p = base + idx * 24
                    bits = _le32(b, p)
                    completo = ruta + self._nombre(bits)
                    if bits & 0x10000000:
                        pend.append((_le32(b, p + 20), _le16(b, p + 14), completo + BS))
                    else:
                        res[completo] = (archivo, (_le32(b, p + 8) & 0xFFFFFF) * 2048, _le32(b, p + 16))
        return res


# --- cpx --------------------------------------------------------------------
def _cpx_bloque(blk, ancho, out, base, tam):
    if blk[0] == 255:
        out[base:base + tam] = blk[4:4 + tam]
        return
    lm = blk[0] >> 6
    om = (blk[0] >> 4) & 3
    anchos = [blk[1] & 15, blk[1] >> 4, 0]
    if om < 3:
        anchos[om] = 16
    orden = [(0, 1, 2), (1, 0, 2), (2, 0, 1), (2, 2, 2)][(blk[0] >> 2) & 3]
    d = blk[4:]
    st = [0, 0, 0]  # pos, valor, bits que quedan

    def recargar():
        v = 0
        for _ in range(ancho // 8):
            v <<= 8
            if st[0] < len(d):
                v |= d[st[0]]
            st[0] += 1
        st[1] = v
        st[2] = ancho

    def leer(c):
        r = 0
        while c:
            t = min(c, st[2])
            st[2] -= t
            r = (r << t) | ((st[1] >> st[2]) & ((1 << t) - 1))
            c -= t
            if not st[2]:
                recargar()
        return r

    def directa():
        r = (d[st[0]] << 8) | d[st[0] + 1]
        st[0] += 2
        return r

    recargar()
    pos = 0
    while pos < tam:
        if not leer(1):
            out[base + pos] = leer(8)
            pos += 1
            continue
        if om == 0:
            off = directa() if ancho == 16 else leer(16)
        elif om == 1:
            off = leer(anchos[leer(1)])
        elif om == 2:
            ch = orden[0] if not leer(1) else orden[1 + leer(1)]
            off = leer(anchos[ch])
        else:
            off = 0
        lb = 9
        if lm == 1 and not leer(1):
            lb = 2
        elif lm == 2 and not leer(1):
            lb = 3
        n = leer(lb) + 3
        for _ in range(n):
            out[base + pos] = out[base + pos - off - 1]
            pos += 1


def cpx(datos):
    if datos[:3] != b"cpx":
        return datos
    bloques = _le16(datos, 6)
    guardado = _le32(datos, 8)
    total = _le32(datos, 12)
    ancho = 8 if (datos[4] & 0xF0) == 0x10 else 16
    out = bytearray(total)
    hecho = 0
    for i in range(bloques):
        ini = _le32(datos, 16 + 4 * i)
        fin = _le32(datos, 20 + 4 * i) if i + 1 < bloques else guardado
        n = min(65536, total - hecho)
        _cpx_bloque(datos[ini:fin], ancho, out, hecho, n)
        hecho += n
    return bytes(out)


# --- Paquete UE3 --------------------------------------------------------------
@dataclass
class Export:
    clase: str
    nombre: str
    tam: int
    off: int


@dataclass
class Fuente:
    glifos: list      # (x, y, w, h, pagina)
    paginas: list     # nombres de los exports Texture2D
    kerning: int
    mapa: dict        # codepoint -> indice de glifo


class Paquete:
    def __init__(self, b):
        self.b = b
        p = 12
        p += 4 + _bes32(b, p)  # grupo
        p += 4  # flags
        nc, no, ec, eo, ic, io = struct.unpack_from(">6I", b, p)
        self.nombres = []
        q = no
        for _ in range(nc):
            n = _bes32(b, q)
            q += 4
            if n < 0:
                self.nombres.append(b[q:q - 2 * n].decode("utf-16-be").rstrip("\0"))
                q += -2 * n
            else:
                self.nombres.append(b[q:q + n - 1].decode("latin-1"))
                q += n
            q += 8
        self.imports = []
        q = io
        for _ in range(ic):
            q += 20
            self.imports.append(self._nombre(q))
            q += 8
        self.exports = []
        q = eo
        for _ in range(ec):
            cls = _bes32(b, q)
            nombre = self._nombre(q + 12)
            q += 32
            tam, off = struct.unpack_from(">II", b, q)
            q += 8
            q += 4 + _be32(b, q) * 12  # ComponentMap
            q += 4  # ExportFlags
            q += 4 + _be32(b, q) * 4 + 16  # GenerationNetObjectCount + GUID
            self.exports.append(Export(self.imports[-cls - 1] if cls < 0 else "", nombre, tam, off))

    def _nombre(self, q, b=None):
        b = self.b if b is None else b
        i, n = struct.unpack_from(">II", b, q)
        return self.nombres[i] + (f"_{n - 1}" if n else "")

    def propiedades(self, off):
        """(dict nombre -> valor simple, posicion tras 'None')"""
        b = self.b
        q = off + 4
        out = {}
        while True:
            nombre = self._nombre(q)
            q += 8
            if nombre == "None":
                return out, q
            tipo = self._nombre(q)
            q += 8
            tam = _be32(b, q)
            q += 8
            if tipo == "StructProperty":
                q += 8
            if tipo == "BoolProperty":
                out[nombre] = _be32(b, q)
                q += 4
            v = b[q:q + tam]
            q += tam
            if tipo == "IntProperty":
                out[nombre] = struct.unpack(">i", v)[0]
            elif tipo == "FloatProperty":
                out[nombre] = struct.unpack(">f", v)[0]
            elif tipo == "ByteProperty":
                out[nombre] = v[0] if tam == 1 else self._nombre(0, v)

    def fuente(self, e):
        b = self.b
        _, q = self.propiedades(e.off)
        n = _be32(b, q)
        q += 4
        glifos = []
        for _ in range(n):
            x, y, w, h = struct.unpack_from(">4I", b, q)
            glifos.append((x, y, w, h, b[q + 16]))
            q += 17
        npg = _be32(b, q)
        q += 4
        paginas = [self.exports[p - 1].nombre for p in struct.unpack_from(">%dI" % npg, b, q)]
        q += 4 * npg
        kerning = _bes32(b, q)
        q += 4
        nr = _be32(b, q)
        q += 4
        mapa = {}
        for _ in range(nr):
            cp, ix = struct.unpack_from(">HH", b, q)
            q += 4
            mapa[cp] = ix
        return Fuente(glifos, paginas, kerning, mapa)


class Disco:
    def __init__(self, carpeta):
        self.carpeta = Path(carpeta)
        self.ficheros = Fpi((self.carpeta / "LO.fpi").read_bytes()).ficheros()

    def leer(self, ruta):
        archivo, off, tam = self.ficheros[ruta.replace("/", BS)]
        with open(self.carpeta / archivo, "rb") as f:
            f.seek(off)
            return cpx(f.read(tam))

    def paquete(self, ruta):
        return Paquete(self.leer(ruta))
