"""Herramienta propia para las partidas de Lost Odyssey (save.bin).

Formato deducido analizando el cifrador del editor de Killermiles (desempaquetado
con UPX y desensamblado); el codigo de aqui es propio.

    fichero = cabecera de 0x70 bytes EN CLARO + contenido CIFRADO

    cabecera (big-endian):
      0x00  "LOSV"
      0x04  identificador del juego (4D5307FA)
      0x08  version (3)
      0x0C  0x00200050
      0x10  longitud del contenido
      0x14  suma de verificacion del contenido EN CLARO
      0x18  campo variable por partida (tiempo de juego?)

    cifrado, byte a byte, con encadenado del byte cifrado anterior:
      cifrado[i] = ((claro[i] XOR cifrado[i-1]) + i) XOR 0xBB      (i truncado a 8 bits)
      claro[i]   = ((cifrado[i] XOR 0xBB) - i) XOR cifrado[i-1]
      cifrado[-1] = 0

    suma de verificacion = suma en 32 bits de (claro[i] XOR i)

Uso:
    python lo_save.py info      <save.bin>
    python lo_save.py descifrar <save.bin> [salida.bin]
    python lo_save.py cifrar    <claro.bin> [salida.bin]     (recalcula la suma)
    python lo_save.py comprobar <carpeta>                    (ida y vuelta de todas)
"""

import pathlib
import struct
import sys

CABECERA = 0x70
MAGIC = b"LOSV"
CLAVE = 0xBB


def _longitud(datos: bytes) -> int:
    return struct.unpack_from(">I", datos, 0x10)[0]


def comprobar_formato(datos: bytes) -> None:
    if len(datos) < CABECERA or datos[:4] != MAGIC:
        raise ValueError("no parece una partida de Lost Odyssey (falta la marca LOSV)")
    if CABECERA + _longitud(datos) > len(datos):
        raise ValueError("la longitud de la cabecera no cabe en el fichero")


def suma_verificacion(claro: bytes) -> int:
    """Suma de (byte XOR posicion) en 32 bits sobre el contenido en claro."""
    total = 0
    for i, b in enumerate(claro):
        total = (total + (b ^ i)) & 0xFFFFFFFF
    return total


def descifrar(datos: bytes) -> bytes:
    comprobar_formato(datos)
    salida = bytearray(datos)
    anterior = 0
    for i in range(_longitud(datos)):
        cifrado = datos[CABECERA + i]
        salida[CABECERA + i] = (((cifrado ^ CLAVE) - (i & 0xFF)) ^ anterior) & 0xFF
        anterior = cifrado
    return bytes(salida)


def cifrar(datos: bytes, recalcular_suma: bool = True) -> bytes:
    comprobar_formato(datos)
    longitud = _longitud(datos)
    salida = bytearray(datos)
    if recalcular_suma:
        struct.pack_into(">I", salida, 0x14, suma_verificacion(datos[CABECERA:CABECERA + longitud]))
    anterior = 0
    for i in range(longitud):
        cifrado = ((((datos[CABECERA + i] ^ anterior) + (i & 0xFF)) & 0xFF) ^ CLAVE) & 0xFF
        salida[CABECERA + i] = cifrado
        anterior = cifrado
    return bytes(salida)


def info(ruta: pathlib.Path) -> None:
    datos = ruta.read_bytes()
    comprobar_formato(datos)
    longitud = _longitud(datos)
    claro = descifrar(datos)[CABECERA:CABECERA + longitud]
    campos = struct.unpack_from(">IIIIIII", datos, 0x00)
    print(f"{ruta.name}: {len(datos)} bytes")
    print(f"  marca          {datos[:4].decode()}   juego {campos[1]:08X}   version {campos[2]}")
    print(f"  contenido      {longitud} bytes cifrados desde 0x{CABECERA:X}")
    print(f"  suma guardada  {campos[5]:08X}")
    print(f"  suma calculada {suma_verificacion(claro):08X}   {'coincide' if campos[5] == suma_verificacion(claro) else 'NO COINCIDE'}")
    print(f"  campo 0x18     {campos[6]:08X}")
    marcas = [m for m in (b"LOSVMS", b"CONFIG", b"GMINFS", b"PLAYDT") if m in claro]
    print(f"  bloques        {', '.join(m.decode() for m in marcas)}")


def comprobar_carpeta(carpeta: pathlib.Path) -> int:
    fallos = 0
    ficheros = sorted(carpeta.rglob("save.bin"))
    for fichero in ficheros:
        datos = fichero.read_bytes()
        try:
            ida_vuelta = cifrar(descifrar(datos), recalcular_suma=False) == datos
            suma_ok = struct.unpack_from(">I", datos, 0x14)[0] == suma_verificacion(
                descifrar(datos)[CABECERA:CABECERA + _longitud(datos)])
        except ValueError as error:
            print(f"  {fichero.parent.name}: {error}")
            fallos += 1
            continue
        estado = "bien" if ida_vuelta and suma_ok else "FALLA"
        if not (ida_vuelta and suma_ok):
            fallos += 1
        print(f"  {fichero.parent.name:8} ida y vuelta: {ida_vuelta}   suma: {suma_ok}   {estado}")
    print(f"{len(ficheros)} partidas, {fallos} con problemas")
    return fallos


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 1
    orden, ruta = argv[1], pathlib.Path(argv[2])
    if orden == "info":
        info(ruta)
    elif orden == "descifrar":
        destino = pathlib.Path(argv[3]) if len(argv) > 3 else ruta.with_suffix(".claro.bin")
        destino.write_bytes(descifrar(ruta.read_bytes()))
        print(f"descifrado en {destino}")
    elif orden == "cifrar":
        destino = pathlib.Path(argv[3]) if len(argv) > 3 else ruta.with_suffix(".cifrado.bin")
        destino.write_bytes(cifrar(ruta.read_bytes()))
        print(f"cifrado en {destino} (suma recalculada)")
    elif orden == "comprobar":
        return 1 if comprobar_carpeta(ruta) else 0
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
