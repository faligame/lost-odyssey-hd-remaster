"""Formato .lopack (version 1): pack de texturas HD de Lost Odyssey, cifrado y ligado a los discos.

Todo little-endian.

Cabecera (96 bytes, en claro; la autentica el cifrado del indice):
   0  'LOPK'
   4  u32 version del formato (1)
   8  u32 version del contenido del pack
  12  u32 edicion (1 = USA/Europa v3)
  16  32 bytes sal (aleatoria por pack)
  48  u64 desplazamiento del indice
  56  u64 tamano del indice cifrado (con etiqueta)
  64  12 bytes nonce del indice
  76  u32 numero de entradas
  80  16 bytes reservados (cero)

Datos: por entrada, AES-256-GCM(zstd(imagen)) + etiqueta de 16 bytes.
  nonce de la entrada i = 00 00 00 00 || u64 i ;  AAD = u64 hash || u32 tamano_sin_comprimir || u8 tipo

Indice (cifrado con AES-256-GCM; AAD = los 96 bytes de la cabecera): entradas de 32 bytes
  u64 hash (huella de la textura del juego), u64 desplazamiento, u32 tamano cifrado (con etiqueta),
  u32 tamano sin comprimir, u8 tipo (0 = DDS, 1 = PNG), u8 codec (0 = sin comprimir, 1 = zstd),
  u16 0, u32 0

Clave: HKDF-SHA256(sal = sal del pack, ikm = SHA256("LOPK-v1" || SHA256(default.xex del disco 1) ||
SHA256(LO.fpi del disco 1)), info = "lopack key v1"). No se guarda en ningun sitio: sin los discos de la
edicion correcta no hay forma de abrirlo. Es ofuscacion fuerte, no proteccion legal.
"""
import hashlib
import os
import struct

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

MAGIC = b"LOPK"
FORMATO = 1
HEADER_SIZE = 96
ENTRY_SIZE = 32
EDICION_USA_EUROPA_V3 = 1
TIPO_DDS, TIPO_PNG = 0, 1
CODEC_RAW, CODEC_ZSTD = 0, 1
INDEX_NONCE = b"\xff" * 12


def secreto_de_discos(xex_bytes: bytes, fpi_bytes: bytes) -> bytes:
    """El secreto del disco 1 (el exe calcula exactamente lo mismo con BCrypt)."""
    h = hashlib.sha256()
    h.update(b"LOPK-v1")
    h.update(hashlib.sha256(xex_bytes).digest())
    h.update(hashlib.sha256(fpi_bytes).digest())
    return h.digest()


def secreto_de_carpeta(disco1: str) -> bytes:
    with open(os.path.join(disco1, "default.xex"), "rb") as f:
        xex = f.read()
    with open(os.path.join(disco1, "LO.fpi"), "rb") as f:
        fpi = f.read()
    return secreto_de_discos(xex, fpi)


def derivar_clave(secreto: bytes, sal: bytes) -> bytes:
    return HKDF(algorithm=hashes.SHA256(), length=32, salt=sal, info=b"lopack key v1").derive(secreto)


def nonce_entrada(i: int) -> bytes:
    return b"\x00\x00\x00\x00" + struct.pack("<Q", i)


def aad_entrada(hash64: int, raw_size: int, tipo: int) -> bytes:
    return struct.pack("<QIB", hash64, raw_size, tipo)


def cabecera(version_contenido, edicion, sal, indice_off, indice_size, indice_nonce, n) -> bytes:
    h = MAGIC + struct.pack("<III", FORMATO, version_contenido, edicion) + sal
    h += struct.pack("<QQ", indice_off, indice_size) + indice_nonce + struct.pack("<I", n) + b"\x00" * 16
    assert len(h) == HEADER_SIZE
    return h


class Pack:
    """Lector (verificacion y pruebas)."""

    def __init__(self, ruta, secreto):
        self.f = open(ruta, "rb")
        h = self.f.read(HEADER_SIZE)
        if h[:4] != MAGIC:
            raise ValueError("no es un .lopack")
        self.formato, self.version, self.edicion = struct.unpack("<III", h[4:16])
        self.sal = h[16:48]
        off, size = struct.unpack("<QQ", h[48:64])
        nonce = h[64:76]
        self.n = struct.unpack("<I", h[76:80])[0]
        self.clave = derivar_clave(secreto, self.sal)
        self.aes = AESGCM(self.clave)
        self.f.seek(off)
        cifrado = self.f.read(size)
        plano = self.aes.decrypt(nonce, cifrado, h)  # InvalidTag si la clave no es la de estos discos
        self.entradas = {}
        for i in range(self.n):
            hs, o, cs, rs, tipo, codec, _, _ = struct.unpack("<QQIIBBHI", plano[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE])
            self.entradas[hs] = (i, o, cs, rs, tipo, codec)

    def leer(self, hash64):
        i, o, cs, rs, tipo, codec = self.entradas[hash64]
        self.f.seek(o)
        cifrado = self.f.read(cs)
        datos = self.aes.decrypt(nonce_entrada(i), cifrado, aad_entrada(hash64, rs, tipo))
        if codec == CODEC_ZSTD:
            try:
                from compression import zstd
            except Exception:
                from backports import zstd
            datos = zstd.decompress(datos)
        assert len(datos) == rs
        return datos, tipo
