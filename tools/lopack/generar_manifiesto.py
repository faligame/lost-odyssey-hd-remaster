"""Genera texture-pack.json (el manifiesto que lee el juego).

Fichero entero (lo normal; lo que sube subir_archive.py por defecto):
  python generar_manifiesto.py --fichero textures.lopack --id <identificador de archive.org> [--version N] [--salida texture-pack.json]
Con trozos (subir_archive.py --trozos), a partir del <fichero>.parts.json:
  python generar_manifiesto.py --partes <fichero>.parts.json --id <identificador>
Pruebas con un servidor local: --base-url http://127.0.0.1:8791/ en lugar de --id.

El manifiesto se sube al repositorio de GitHub (su URL va en LO_TEXTURE_MANIFEST_URL de lo_version.h).
"""
import argparse, hashlib, json, os, sys

ap = argparse.ArgumentParser()
ap.add_argument("--fichero", default="", help="el .lopack entero (calcula tamano y SHA-256)")
ap.add_argument("--partes", default="", help="<fichero>.parts.json de subir_archive.py --trozos")
ap.add_argument("--id", default="")
ap.add_argument("--base-url", default="")
ap.add_argument("--version", type=int, default=1, help="version del contenido del pack")
ap.add_argument("--salida", default="texture-pack.json")
a = ap.parse_args()
base = a.base_url or (f"https://archive.org/download/{a.id}/" if a.id else "")
if not base:
    sys.exit("hace falta --id o --base-url")
if not base.endswith("/"):
    base += "/"
out = {"schema": 1, "version": a.version, "file": "textures.lopack"}
if a.partes:
    m = json.load(open(a.partes, encoding="utf-8"))
    out.update(size=m["tamano_total"], sha256=m["sha256_total"], base_urls=[base],
               parts=[{"name": p["nombre"], "size": p["tamano"], "sha256": p["sha256"]} for p in m["partes"]])
elif a.fichero:
    h = hashlib.sha256()
    size = os.path.getsize(a.fichero)
    done = 0
    with open(a.fichero, "rb") as f:
        while True:
            b = f.read(16 << 20)
            if not b:
                break
            h.update(b)
            done += len(b)
            print(f"\r  SHA-256: {100 * done / size:5.1f}%", end="", flush=True)
    print()
    out.update(size=size, sha256=h.hexdigest(), urls=[base + os.path.basename(a.fichero)])
else:
    sys.exit("hace falta --fichero o --partes")
json.dump(out, open(a.salida, "w", encoding="utf-8"), indent=2)
print(f"{a.salida}: {out['size']} bytes")
