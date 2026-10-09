"""Prepara una actualizacion firmada: calcula tamano y SHA-256 del zip y escribe latest.json + latest.json.sig.

  python publicar_version.py <zip> --version 0.0.2 --notas-es "..." --notas-en "..." [--tag v0.0.2]
        [--repo faligame/lost-odyssey-hd-remaster] [--url-zip <url completa>] [--salida carpeta]
        [--privada C:\\ruta\\update_private.pem]

Despues:
  1. Crea la release v<version> en GitHub y sube el zip como asset (el nombre del zip debe coincidir con la URL).
  2. Sube latest.json y latest.json.sig a la RAIZ del repositorio (rama main): el juego los lee de raw.githubusercontent.com.
     Hazlo el ULTIMO, cuando el zip ya se descargue: en cuanto estos ficheros cambian, los jugadores empiezan a ver la actualizacion.

El juego comprueba la firma (ECDSA P-256 / SHA-256, clave publica dentro del exe) sobre los bytes EXACTOS de latest.json:
no lo edites a mano despues de firmar. latest.json.sig son 64 bytes (r||s) en hexadecimal.
"""
import argparse
import hashlib
import json
import os
import sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

ap = argparse.ArgumentParser()
ap.add_argument("zip")
ap.add_argument("--version", required=True)
ap.add_argument("--notas-es", default="")
ap.add_argument("--notas-en", default="")
ap.add_argument("--notas-es-archivo", default="", help="fichero UTF-8 con las notas en espanol (mejor que --notas-es con acentos desde PowerShell)")
ap.add_argument("--notas-en-archivo", default="", help="fichero UTF-8 con las notas en ingles")
ap.add_argument("--tag", default="")
ap.add_argument("--repo", default="faligame/lost-odyssey-hd-remaster")
ap.add_argument("--url-zip", default="")
ap.add_argument("--salida", default="")
ap.add_argument("--privada", default=os.path.join(os.path.expanduser("~"), ".lo_signing", "update_private.pem"))
a = ap.parse_args()
if a.notas_es_archivo:
    a.notas_es = open(a.notas_es_archivo, encoding="utf-8").read().strip()
if a.notas_en_archivo:
    a.notas_en = open(a.notas_en_archivo, encoding="utf-8").read().strip()

tag = a.tag or f"v{a.version}"
url = a.url_zip or f"https://github.com/{a.repo}/releases/download/{tag}/{os.path.basename(a.zip)}"
h = hashlib.sha256()
with open(a.zip, "rb") as f:
    for b in iter(lambda: f.read(8 << 20), b""):
        h.update(b)
manifest = {
    "schema": 1,
    "version": a.version,
    "size": os.path.getsize(a.zip),
    "sha256": h.hexdigest(),
    "url": url,
    "notes_en": a.notas_en,
    "notes_es": a.notas_es,
}
data = json.dumps(manifest, indent=2, ensure_ascii=False).encode("utf-8")
with open(a.privada, "rb") as f:
    key = serialization.load_pem_private_key(f.read(), password=None)
r, s = decode_dss_signature(key.sign(data, ec.ECDSA(hashes.SHA256())))
sig = r.to_bytes(32, "big") + s.to_bytes(32, "big")

out = a.salida or os.path.join(os.path.dirname(os.path.abspath(a.zip)), f"actualizacion-{a.version}")
os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "latest.json"), "wb") as f:
    f.write(data)
with open(os.path.join(out, "latest.json.sig"), "w", encoding="ascii", newline="") as f:
    f.write(sig.hex())
print(f"escrito en {out}: latest.json ({len(data)} B) y latest.json.sig; zip {manifest['size']} B sha256 {manifest['sha256'][:16]}...")
print("URL del zip:", url)
