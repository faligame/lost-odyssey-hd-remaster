"""Quita de los binarios la ruta absoluta del equipo de compilacion (cadenas __FILE__, ruta del PDB...).

  python limpiar_rutas_binarios.py <fichero|carpeta> [...]

Sustituye cada aparicion de la carpeta raiz del proyecto y de la carpeta personal del usuario (C:\\Users\\<usuario>)
por una cadena de la misma longitud hecha de barras, asi no se mueve nada dentro del fichero. Se usa sobre las
COPIAS que van al paquete; la raiz del proyecto se detecta a partir de la ubicacion de este script.
"""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
USER = os.path.expanduser('~')
BARRA_INVERSA = chr(92)


def variantes(ruta):
    ruta = ruta.rstrip(BARRA_INVERSA + '/')
    salida = set()
    for sep in (BARRA_INVERSA, '/'):
        r = ruta.replace(BARRA_INVERSA, sep).replace('/', sep)
        salida.add(r)
        salida.add(r.replace(sep, sep * 2))
        salida.add(r[0].lower() + r[1:])
        salida.add(r[0].upper() + r[1:])
    return sorted(salida, key=len, reverse=True)


def limpiar(datos):
    n = 0
    for ruta in (ROOT, USER):
        for v in variantes(ruta):
            b = v.encode('utf-8')
            if b in datos:
                n += datos.count(b)
                datos = datos.replace(b, b'/' * len(b))
            w = v.encode('utf-16-le')
            if w in datos:
                n += datos.count(w)
                datos = datos.replace(w, ('/' * len(v)).encode('utf-16-le'))
    return datos, n


def procesar(p):
    with open(p, 'rb') as f:
        datos = f.read()
    nuevo, n = limpiar(datos)
    if n:
        with open(p, 'wb') as f:
            f.write(nuevo)
    print(f'{os.path.basename(p)}: {n} rutas quitadas')


if __name__ == '__main__':
    for arg in sys.argv[1:]:
        if os.path.isdir(arg):
            for nombre in sorted(os.listdir(arg)):
                if nombre.lower().endswith(('.exe', '.dll', '.bin', '.xpso', '.lopack')):
                    procesar(os.path.join(arg, nombre))
        else:
            procesar(arg)
