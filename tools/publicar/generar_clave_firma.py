"""Genera el par de claves ECDSA P-256 con el que se firman las actualizaciones (latest.json).

  python generar_clave_firma.py [--privada C:\\ruta\\update_private.pem] [--cabecera project\\src\\lo_update_key.h]

- La clave PRIVADA se guarda fuera del proyecto (por defecto %USERPROFILE%\\.lo_signing\\update_private.pem). Quien la tenga puede
  publicar actualizaciones que todos los jugadores instalaran: copiala a un sitio seguro y NO la subas a ningun repositorio.
  Si la pierdes no se pueden publicar mas actualizaciones firmadas con esta clave (habria que sacar una version nueva a mano
  con otra clave publica).
- La clave PUBLICA se escribe en lo_update_key.h (X e Y de 32 bytes) y va dentro del ejecutable.
Si ya existe la privada, NO se sobrescribe: solo se vuelve a escribir la cabecera a partir de ella.
"""
import argparse
import os

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

ap = argparse.ArgumentParser()
ap.add_argument("--privada", default=os.path.join(os.path.expanduser("~"), ".lo_signing", "update_private.pem"))
ap.add_argument("--cabecera", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "project", "src", "lo_update_key.h"))
a = ap.parse_args()

if os.path.exists(a.privada):
    with open(a.privada, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    print("clave privada existente:", a.privada)
else:
    os.makedirs(os.path.dirname(a.privada), exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    with open(a.privada, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    print("clave privada creada:", a.privada, "(COPIA DE SEGURIDAD, y fuera de cualquier repositorio)")

nums = key.public_key().public_numbers()
x = nums.x.to_bytes(32, "big")
y = nums.y.to_bytes(32, "big")
def arr(b):
    return ", ".join(f"0x{c:02X}" for c in b)
header = f"""// lostodyssey - clave PUBLICA para comprobar las actualizaciones firmadas (ECDSA P-256).
// Generada con tools/publicar/generar_clave_firma.py. La privada NO esta en el proyecto.
#pragma once
#include <cstdint>
namespace lo {{
inline constexpr uint8_t kUpdateKeyX[32] = {{{arr(x)}}};
inline constexpr uint8_t kUpdateKeyY[32] = {{{arr(y)}}};
}}  // namespace lo
"""
with open(a.cabecera, "w", encoding="utf-8") as f:
    f.write(header)
print("cabecera escrita:", os.path.abspath(a.cabecera))
