#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
subir_archive.py - sube un fichero grande a un ítem de archive.org (reanudable, con hilos).

MODO POR DEFECTO: FICHERO ENTERO.
  Un solo PUT en streaming (bloques de 8 MB, sin cargar el fichero en memoria) a
  https://s3.us.archive.org/<id>/<fichero>. Si la subida se corta, se reintenta con backoff y se vuelve
  a subir DESDE CERO (un PUT único no se puede reanudar). Si el fichero ya está en el ítem con el mismo
  tamaño y MD5, no se hace nada.

MODO ALTERNATIVO (--trozos): reanudable.
  El fichero se parte en trozos de ~1,9 GB (<nombre>.part001, .part002, ...) que se suben en paralelo
  como ficheros del mismo ítem. Cada trozo se reintenta solo y los que ya estén en el ítem (mismo tamaño
  y MD5) se saltan. El instalador debe descargar todos los trozos y concatenarlos en orden. El manifiesto
  <nombre>.parts.json (offsets, tamaños, MD5 y SHA-256 de cada trozo y del total) permite verificarlo.

  Multiparte S3 (CreateMultipartUpload/UploadPart/Complete) NO está implementado: la documentación de
  archive.org solo lo menciona de pasada y la biblioteca oficial `ia` no lo usa.

AUTENTICACIÓN
  Claves S3 de archive.org, en este orden:
    1. Variables de entorno IA_ACCESS_KEY / IA_SECRET_KEY (también IA_ACCESS_KEY_ID / IA_SECRET_ACCESS_KEY).
    2. Fichero de configuración: `ia.ini` junto a este script, IA_CONFIG_FILE, %XDG_CONFIG_HOME%\\internetarchive\\ia.ini,
       ~\\.config\\internetarchive\\ia.ini, ~\\.config\\ia.ini o ~\\.ia (sección [s3], claves access/secret).
  Las claves NUNCA se imprimen ni se guardan en ningún fichero.

MODOS
  Sin --confirmar: SIMULACIÓN. No sube nada ni escribe nada en disco. Valida las claves con una
  petición de solo lectura, comprueba el fichero y muestra el plan y una estimación de tiempo.
  Con --confirmar: SUBIDA REAL (requiere --id).

EJEMPLOS
  python subir_archive.py "G:\\...\\textures.lopack" --id mi-ítem-prueba
  python subir_archive.py "G:\\...\\textures.lopack" --id mi-item-prueba --confirmar --hilos 4

REANUDAR
  Modo entero: vuelve a ejecutar el mismo comando con --confirmar; si el fichero ya está completo en el
  ítem no hace nada, y si no, sube de nuevo desde cero.
  Modo --trozos: el mismo comando; se suben solo los trozos que falten.
"""

import argparse
import configparser
import datetime as dt
import hashlib
import json
import math
import os
import queue
import random
import re
import sys
import threading
import time
from collections import deque
from urllib.parse import quote

try:
    import requests
    import requests.adapters
except ImportError:
    print("Falta la biblioteca 'requests'. Instálala con: python -m pip install requests", file=sys.stderr)
    sys.exit(2)


# ----------------------------------------------------------------------------------------------
# Constantes
# ----------------------------------------------------------------------------------------------
S3_URL_DEF = "https://s3.us.archive.org"
META_URL_DEF = "https://archive.org/metadata"
DESCARGA_URL = "https://archive.org/download"

COLECCION_DEF = "opensource_media"      # "Community Data": datos donados por particulares
MEDIATYPE_DEF = "data"
ETIQUETAS_DEF = "lost odyssey, lopack, recompilacion"
DESCRIPCION_DEF = ("Fichero cifrado para el proyecto de recompilación de Lost Odyssey. "
                   "Requiere los discos originales del propio jugador para su uso.")

TAM_TROZO_DEF_MB = 1900                 # ~1,9 GB: por debajo del límite de 2 GB de algunos sistemas
HILOS_DEF = 4
HILOS_MAX = 8
REINTENTOS_DEF = 6
VELOCIDAD_DEF_MBS = 5.0                 # solo para la estimación de la simulación

BLOQUE = 1024 * 1024                    # 1 MiB por bloque de lectura/envío (modo trozos)
BLOQUE_ENTERO = 8 * BLOQUE              # 8 MiB por bloque de envío (modo fichero entero)
TIMEOUT_CONEXION = 30                   # segundos
TIMEOUT_LECTURA = 1800                  # espera máxima de la respuesta tras enviar un trozo
ESTADOS_REINTENTABLES = {408, 429, 500, 502, 503, 504}
REGEX_IDENTIFICADOR = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.\-]{2,99}$")


# ----------------------------------------------------------------------------------------------
# Errores propios
# ----------------------------------------------------------------------------------------------
class ErrorFatal(Exception):
    """Error que para todas las subidas (credenciales, permisos, datos inconsistentes)."""


class ErrorParte(Exception):
    """Un trozo ha agotado sus reintentos. Los demás trozos siguen; se reintenta al volver a ejecutar."""


class Interrumpido(Exception):
    """Parada pedida por el usuario (Ctrl+C). No es un error."""


# ----------------------------------------------------------------------------------------------
# Utilidades de salida
# ----------------------------------------------------------------------------------------------
def hora():
    return dt.datetime.now().strftime("%H:%M:%S")


def log(msg=""):
    print(msg, flush=True)


def aviso(msg):
    print(f"\n[{hora()}] AVISO: {msg}", file=sys.stderr, flush=True)


def formato_tam(n):
    """Unidades decimales (1 GB = 1.000.000.000 bytes), como las que muestra archive.org."""
    if n >= 1e9:
        return f"{n / 1e9:.2f} GB"
    if n >= 1e6:
        return f"{n / 1e6:.1f} MB"
    return f"{n / 1e3:.1f} KB"


def formato_tiempo(seg):
    if seg is None or seg != seg or seg == float("inf"):
        return "?"
    seg = int(seg)
    return f"{seg // 3600}:{(seg % 3600) // 60:02d}:{seg % 60:02d}"


def escribir_json(ruta, datos):
    """Escritura atómica: primero un .tmp y luego se sustituye (un corte no deja JSON roto)."""
    tmp = ruta + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(datos, f, indent=2, ensure_ascii=False)
    os.replace(tmp, ruta)


# ----------------------------------------------------------------------------------------------
# Credenciales (nunca se imprimen)
# ----------------------------------------------------------------------------------------------
def rutas_config():
    """Ubicaciones que mira la librería `internetarchive` (`ia configure`)."""
    rutas = []
    # Junto al propio script (ia.ini con la seccion [s3]): lo mas comodo; no lo publiques ni lo compartas.
    rutas.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ia.ini"))
    if os.environ.get("IA_CONFIG_FILE"):
        rutas.append(os.environ["IA_CONFIG_FILE"])
    if os.environ.get("XDG_CONFIG_HOME"):
        rutas.append(os.path.join(os.environ["XDG_CONFIG_HOME"], "internetarchive", "ia.ini"))
    home = os.path.expanduser("~")
    rutas.append(os.path.join(home, ".config", "internetarchive", "ia.ini"))
    rutas.append(os.path.join(home, ".config", "ia.ini"))
    rutas.append(os.path.join(home, ".ia"))
    return rutas


def leer_credenciales():
    """Devuelve (access, secret, origen). Si no hay claves, devuelve (None, None, None)."""
    env_ak = os.environ.get("IA_ACCESS_KEY") or os.environ.get("IA_ACCESS_KEY_ID") or ""
    env_sk = os.environ.get("IA_SECRET_KEY") or os.environ.get("IA_SECRET_ACCESS_KEY") or ""
    if env_ak.strip() and env_sk.strip():
        return env_ak.strip(), env_sk.strip(), "variables de entorno IA_ACCESS_KEY / IA_SECRET_KEY"
    if bool(env_ak.strip()) != bool(env_sk.strip()):
        aviso("Solo una de las dos variables de entorno de claves está definida; se ignoran.")

    for ruta in rutas_config():
        if not os.path.isfile(ruta):
            continue
        cp = configparser.ConfigParser(interpolation=None)
        try:
            cp.read(ruta, encoding="utf-8")
        except (configparser.Error, OSError, UnicodeDecodeError):
            continue
        if cp.has_option("s3", "access") and cp.has_option("s3", "secret"):
            ak = cp.get("s3", "access").strip()
            sk = cp.get("s3", "secret").strip()
            if ak and sk and ak.lower() != "none" and sk.lower() != "none":
                return ak, sk, f"fichero de configuración {ruta}"
    return None, None, None


def cabecera_auth(ak, sk):
    """Formato de autenticación S3 de archive.org: 'LOW access:secret'."""
    return f"LOW {ak}:{sk}"


# ----------------------------------------------------------------------------------------------
# Red
# ----------------------------------------------------------------------------------------------
def crear_sesion(hilos):
    s = requests.Session()
    s.headers["User-Agent"] = "subir_archive.py/1.0 (uso personal)"
    # max_retries=0: los reintentos los gestiona este script, con su propio retroceso.
    adaptador = requests.adapters.HTTPAdapter(pool_connections=4, pool_maxsize=hilos + 4, max_retries=0)
    s.mount("https://", adaptador)
    s.mount("http://", adaptador)
    return s


def con_reintentos_lectura(fn, descripcion, n=5):
    """Reintenta una petición de solo lectura (metadatos) con retroceso exponencial."""
    for i in range(n + 1):
        try:
            return fn()
        except (requests.RequestException, ValueError) as e:
            if i == n:
                raise ErrorFatal(f"No se pudo {descripcion} ({type(e).__name__}).")
            espera = min(60, 3 * 2 ** i)
            aviso(f"Fallo al {descripcion}; reintento en {espera} s.")
            time.sleep(espera)


def pedir_metadatos(sesion, meta_url, ident):
    """Metadatos públicos del ítem. Un ítem inexistente devuelve {}."""
    def _pedir():
        r = sesion.get(f"{meta_url}/{ident}", timeout=(TIMEOUT_CONEXION, 120))
        r.raise_for_status()
        d = r.json()
        return d if isinstance(d, dict) else {}
    return con_reintentos_lectura(_pedir, "consultar los metadatos del ítem")


def ficheros_remotos(d):
    """Diccionario nombre -> {size, md5} a partir de la respuesta de la API de metadatos."""
    salida = {}
    for f in d.get("files", []) or []:
        nombre = f.get("name")
        if not nombre:
            continue
        try:
            tam = int(f.get("size", -1))
        except (TypeError, ValueError):
            tam = -1
        salida[nombre] = {"size": tam, "md5": (f.get("md5") or "").lower()}
    return salida


def comprobar_credenciales(sesion, s3_url, ak, sk, ident):
    """Petición de SOLO LECTURA: lista el ítem con una sola entrada. Devuelve un texto de resultado.
    No es una prueba definitiva: archive.org no documenta un endpoint de validación de claves."""
    try:
        r = sesion.get(f"{s3_url}/{ident}", params={"max-keys": 1},
                       headers={"Authorization": cabecera_auth(ak, sk)},
                       timeout=(TIMEOUT_CONEXION, 60))
    except requests.RequestException as e:
        return f"no concluyente (error de red: {type(e).__name__})"
    cuerpo = r.text[:400]
    if r.status_code == 200:
        return "aceptadas (el ítem ya existe y se puede listar)"
    if "SignatureDoesNotMatch" in cuerpo or "InvalidAccessKeyId" in cuerpo:
        return "NO VÁLIDAS (archive.org rechaza la firma o la clave de acceso)"
    if "NoSuchBucket" in cuerpo:
        return "parecen aceptadas (el ítem todavía no existe; se creará al subir)"
    return f"no concluyente (HTTP {r.status_code})"


# ----------------------------------------------------------------------------------------------
# Plan de trozos y manifiesto
# ----------------------------------------------------------------------------------------------
def planificar(ruta, tam_trozo_mb, trozos):
    """Devuelve (modo, partes). Cada parte: nombre, offset y tamaño dentro del fichero original.
    Modo 'entero' (por defecto): una sola parte = el fichero completo."""
    base = os.path.basename(ruta)
    total = os.path.getsize(ruta)
    tam_trozo = tam_trozo_mb * 1_000_000      # MB decimales: 1900 MB = 1,9 GB
    if not trozos or total <= tam_trozo:
        return "entero", [{"nombre": base, "offset": 0, "tamano": total}]
    n = math.ceil(total / tam_trozo)
    partes = []
    for i in range(n):
        off = i * tam_trozo
        partes.append({"nombre": f"{base}.part{i + 1:03d}", "offset": off,
                       "tamano": min(tam_trozo, total - off)})
    return "trozos", partes


def ruta_manifiesto(ruta):
    return ruta + ".parts.json"


def ruta_estado(ruta):
    return ruta + ".subida.json"


def leer_estado(ruta, ident):
    """Estado de una ejecución anterior, solo si es del mismo ítem. Si no es válido, se ignora."""
    try:
        with open(ruta_estado(ruta), encoding="utf-8") as f:
            d = json.load(f)
        return d if isinstance(d, dict) and d.get("identificador") == ident else {}
    except (OSError, ValueError):
        return {}


def calcular_manifiesto(ruta, modo, partes, tam_trozo_mb):
    """Calcula MD5 y SHA-256 de cada trozo y el SHA-256 total en UNA sola lectura del fichero.
    Si ya existe un manifiesto válido para este mismo fichero (tamaño, fecha y trozos), lo reutiliza."""
    mf = ruta_manifiesto(ruta)
    total = os.path.getsize(ruta)
    mtime = os.path.getmtime(ruta)
    base = os.path.basename(ruta)

    if os.path.isfile(mf):
        try:
            with open(mf, encoding="utf-8") as f:
                m = json.load(f)
            if (m.get("tamano_total") == total and m.get("fuente_mtime") == mtime
                    and m.get("tam_trozo_mb") == tam_trozo_mb and m.get("fichero") == base
                    and len(m.get("partes", [])) == len(partes)
                    and all(p.get("md5") and p.get("sha256") for p in m["partes"])):
                log("Se reutilizan los hashes del manifiesto existente (el fichero no ha cambiado).")
                return m
        except (OSError, ValueError):
            pass

    log(f"Calculando MD5 y SHA-256 de {len(partes)} parte(s): lectura completa del fichero "
        f"({formato_tam(total)}). Puede tardar varios minutos...")
    sha_total = hashlib.sha256()
    hecho = 0
    ultimo = 0.0
    partes_out = []
    with open(ruta, "rb") as f:
        for p in partes:
            md5 = hashlib.md5()
            sha = hashlib.sha256()
            restante = p["tamano"]
            while restante > 0:
                bloque = f.read(min(8 * BLOQUE, restante))
                if not bloque:
                    raise ErrorFatal("El fichero se ha acortado mientras se calculaban los hashes.")
                md5.update(bloque)
                sha.update(bloque)
                sha_total.update(bloque)
                restante -= len(bloque)
                hecho += len(bloque)
                if time.time() - ultimo >= 2:
                    ultimo = time.time()
                    print(f"\r  hashes: {100 * hecho / total:5.1f}%  ({formato_tam(hecho)} de "
                          f"{formato_tam(total)})", end="", flush=True)
            partes_out.append({"nombre": p["nombre"], "offset": p["offset"], "tamano": p["tamano"],
                               "md5": md5.hexdigest(), "sha256": sha.hexdigest()})
    print("\r  hashes: 100.0%" + " " * 40, flush=True)

    m = {
        "version": 1,
        "fichero": base,
        "modo": modo,
        "tam_trozo_mb": tam_trozo_mb,
        "tamano_total": total,
        "sha256_total": sha_total.hexdigest(),
        "fuente_mtime": mtime,
        "creado": dt.datetime.now().isoformat(timespec="seconds"),
        "partes": partes_out,
        "reconstruccion": ("Concatenar las partes en orden binario (en Windows: "
                           "copy /b %s+%s+... %s). Comprobar el SHA-256 total al final."
                           % (base + ".part001", base + ".part002", base)) if modo == "trozos"
                          else "Fichero único, sin trozos.",
    }
    escribir_json(mf, m)
    log(f"Manifiesto guardado: {mf}")
    return m


# ----------------------------------------------------------------------------------------------
# Cabeceras de metadatos (mismo formato que la librería oficial `ia`)
# ----------------------------------------------------------------------------------------------
def valor_cabecera(valor):
    """Valores ASCII imprimibles tal cual; el resto como uri(percent-encoding), como hace `ia`."""
    s = str(valor)
    if all(32 <= ord(c) < 127 for c in s):
        return s
    return "uri(" + quote(s, safe="") + ")"


def cabeceras_meta(args, etiquetas):
    """x-archive-meta00-<campo>, x-archive-meta01-<campo>... (índice desde 00, como `ia`)."""
    h = {}

    def add(campo, valores):
        for i, v in enumerate(valores):
            if v:
                h[f"x-archive-meta{i:02d}-{campo}"] = valor_cabecera(v)

    add("title", [args.titulo])
    add("description", [args.descripcion])
    add("mediatype", [args.mediatype])
    add("collection", [args.coleccion])
    add("subject", etiquetas)
    if not args.indexar:
        add("noindex", ["true"])        # el ítem no aparece en las búsquedas de archive.org
    return h


# ----------------------------------------------------------------------------------------------
# Subida de un trozo
# ----------------------------------------------------------------------------------------------
class CuerpoRango:
    """Cuerpo de subida: lee [offset, offset+tamano) del fichero en bloques de 1 MiB.
    Tiene __len__ para que requests envíe Content-Length (sin codificación chunked)."""

    def __init__(self, ruta, offset, tamano, ctx):
        self.ruta = ruta
        self.offset = offset
        self.tamano = tamano
        self.ctx = ctx
        self.enviado = 0

    def __len__(self):
        return self.tamano

    def __iter__(self):
        with open(self.ruta, "rb") as f:
            f.seek(self.offset)
            restante = self.tamano
            while restante > 0:
                if self.ctx.parar.is_set():
                    raise Interrumpido()
                bloque = f.read(min(self.ctx.bloque, restante))
                if not bloque:
                    raise ErrorFatal("Fin de fichero inesperado al leer un trozo.")
                restante -= len(bloque)
                self.enviado += len(bloque)
                self.ctx.sumar_bytes(len(bloque))
                yield bloque


def subir_fichero(ctx, nombre, ruta, offset, tamano, extra_cab=None):
    """Sube un fichero (o trozo) con PUT. Reintenta con retroceso exponencial.
    Lanza ErrorFatal (para todo) o ErrorParte (solo este trozo) o Interrumpido."""
    url = f"{ctx.s3_url}/{ctx.ident}/{quote(nombre)}"
    intento = 0
    while True:
        cuerpo = CuerpoRango(ruta, offset, tamano, ctx)
        cab = {
            "Authorization": ctx.auth,
            "x-archive-auto-make-bucket": "1",
            "x-archive-queue-derive": "0",          # sin tareas de derivados (no hacen falta para datos)
            "x-archive-size-hint": str(tamano),
            "Content-Type": "application/octet-stream",
        }
        if extra_cab:
            cab.update(extra_cab)
        motivo = ""
        try:
            r = ctx.sesion.put(url, data=cuerpo, headers=cab,
                               timeout=(TIMEOUT_CONEXION, TIMEOUT_LECTURA))
            _ = r.content                           # consumir la respuesta (libera la conexión)
            if r.status_code in (200, 201):
                return
            if r.status_code not in ESTADOS_REINTENTABLES:
                # Errores 4xx (403 firma/permisos, 400...) no se arreglan reintentando.
                detalle = " ".join(r.text[:200].split())
                raise ErrorFatal(f"{nombre}: HTTP {r.status_code} de archive.org. {detalle}")
            motivo = f"HTTP {r.status_code}"
        except (requests.ConnectionError, requests.Timeout) as e:
            motivo = type(e).__name__
        except (Interrumpido, ErrorFatal):
            ctx.sumar_bytes(-cuerpo.enviado)
            raise
        # Ha fallado: se descuentan los bytes contados en este intento.
        ctx.sumar_bytes(-cuerpo.enviado)

        intento += 1
        if intento > ctx.reintentos:
            raise ErrorParte(f"{nombre}: agotados {ctx.reintentos} reintentos ({motivo}).")
        espera = min(300, 5 * 2 ** (intento - 1)) + random.uniform(0, 2)
        extra = " (PUT único: se vuelve a subir desde cero)" if ctx.modo == "entero" else ""
        aviso(f"{nombre}: {motivo}. Reintento {intento}/{ctx.reintentos} en {espera:.0f} s.{extra}")
        if ctx.parar.wait(espera):
            raise Interrumpido()


# ----------------------------------------------------------------------------------------------
# Contexto compartido entre hilos
# ----------------------------------------------------------------------------------------------
class Contexto:
    def __init__(self, args, sesion, auth, ruta, total, bytes_base, partes_base, modo):
        self.modo = modo
        self.bloque = BLOQUE_ENTERO if modo == "entero" else BLOQUE
        self.s3_url = args.url_s3
        self.ident = args.ident
        self.sesion = sesion
        self.auth = auth
        self.ruta = ruta
        self.reintentos = args.reintentos
        self.parar = threading.Event()
        self.lock = threading.Lock()
        self.bytes_hechos = 0           # bytes netos enviados en esta ejecución
        self.total_archivo = total
        self.bytes_base = bytes_base    # bytes ya presentes en el ítem al empezar
        self.partes_base = partes_base  # nº de trozos ya presentes al empezar
        self.partes_ok = 0              # trozos subidos en esta ejecución
        self.muestras = deque()         # (tiempo, bytes) para calcular la velocidad
        self.estado = {}                # estado que se guarda en <fichero>.subida.json
        self.ruta_estado = ruta_estado(ruta)

    def sumar_bytes(self, n):
        with self.lock:
            self.bytes_hechos += n

    def velocidad(self):
        """Bytes/s de los últimos ~20 s."""
        with self.lock:
            ahora = time.time()
            self.muestras.append((ahora, self.bytes_hechos))
            while len(self.muestras) > 2 and ahora - self.muestras[0][0] > 20:
                self.muestras.popleft()
            if len(self.muestras) < 2:
                return 0.0
            t0, b0 = self.muestras[0]
            t1, b1 = self.muestras[-1]
            return (b1 - b0) / (t1 - t0) if t1 > t0 else 0.0

    def marcar_ok(self, nombre, tamano, md5):
        with self.lock:
            self.partes_ok += 1
            self.estado.setdefault("partes", {})[nombre] = {
                "tamano": tamano, "md5": md5,
                "subida": dt.datetime.now().isoformat(timespec="seconds")}
            self.estado["identificador"] = self.ident
            self.estado["actualizado"] = dt.datetime.now().isoformat(timespec="seconds")
            escribir_json(self.ruta_estado, self.estado)


def trabajador(ctx, cola, errores):
    """Hilo de subida: toma trozos de la cola hasta que no quedan o hay parada."""
    while not ctx.parar.is_set():
        try:
            p = cola.get_nowait()
        except queue.Empty:
            return
        try:
            subir_fichero(ctx, p["nombre"], ctx.ruta, p["offset"], p["tamano"])
            ctx.marcar_ok(p["nombre"], p["tamano"], p["md5"])
        except Interrumpido:
            return
        except ErrorParte as e:
            errores.append(str(e))              # este trozo queda pendiente; los demás siguen
        except ErrorFatal as e:
            errores.append(str(e))
            ctx.parar.set()
            return
        except Exception as e:                  # fallo inesperado: se aborta todo para no perder datos
            errores.append(f"{p['nombre']}: error inesperado {type(e).__name__}: {e}")
            ctx.parar.set()
            return


# ----------------------------------------------------------------------------------------------
# Progreso
# ----------------------------------------------------------------------------------------------
def esperar_hilos(ctx, hilos, partes_total, base_partes, bytes_pend):
    """Espera a los hilos de subida mostrando el progreso. Ctrl+C: para los hilos, guarda el estado y sale con 130."""
    try:
        while any(h.is_alive() for h in hilos):
            imprimir_progreso(ctx, partes_total, base_partes + ctx.partes_ok, bytes_pend)
            time.sleep(1.0)
    except KeyboardInterrupt:
        ctx.parar.set()
        print()
        log("Parando... (los hilos terminan en unos segundos o al acabar el bloque en curso)")
        for h in hilos:
            h.join(10)
        log("Subida interrumpida. El estado queda guardado; vuelve a ejecutar el mismo comando para reanudar.")
        raise SystemExit(130)
    print()
    for h in hilos:
        h.join()


def imprimir_progreso(ctx, partes_total, partes_hechas_total, bytes_pendientes):
    hecho = ctx.bytes_base + ctx.bytes_hechos
    pct = 100.0 * hecho / ctx.total_archivo if ctx.total_archivo else 100.0
    v = ctx.velocidad()
    restante = max(0, bytes_pendientes - ctx.bytes_hechos)
    eta = restante / v if v > 0 else None
    ancho = 24
    llenos = int(ancho * pct / 100)
    barra = "#" * llenos + "-" * (ancho - llenos)
    linea = (f"\r[{barra}] {pct:5.1f}%  {formato_tam(hecho)} / {formato_tam(ctx.total_archivo)}  "
             f"{v / 1e6:6.1f} MB/s  ETA {formato_tiempo(eta)}  "
             f"partes {partes_hechas_total}/{partes_total}")
    print(linea.ljust(130), end="", flush=True)


# ----------------------------------------------------------------------------------------------
# Verificación final
# ----------------------------------------------------------------------------------------------
def verificar(sesion, meta_url, ident, esperado, espera_max=900):
    """Compara tamaño y MD5 de cada fichero remoto con el local. esperado: nombre -> (tamano, md5).
    archive.org tarda a veces en reflejar las subidas en la API de metadatos: se reintenta un rato."""
    inicio = time.time()
    while True:
        remotos = ficheros_remotos(pedir_metadatos(sesion, meta_url, ident))
        faltan, malos, sin_md5 = [], [], []
        for nombre, (tam, md5) in esperado.items():
            r = remotos.get(nombre)
            if r is None:
                faltan.append(nombre)
            elif r["size"] != tam:
                malos.append(f"{nombre} (remoto {r['size']} bytes, local {tam})")
            elif r["md5"] and r["md5"] != md5:
                malos.append(f"{nombre} (MD5 remoto {r['md5']} distinto del local {md5})")
            elif not r["md5"]:
                sin_md5.append(nombre)
        if malos:
            return False, malos, faltan, sin_md5
        if not faltan:
            return True, [], [], sin_md5
        if time.time() - inicio > espera_max:
            return False, [], faltan, sin_md5
        print(f"\r[{hora()}] Esperando a que archive.org muestre {len(faltan)} fichero(s) en los "
              f"metadatos...", end="", flush=True)
        time.sleep(30)


# ----------------------------------------------------------------------------------------------
# Simulación
# ----------------------------------------------------------------------------------------------
def mostrar_plan(args, ruta, modo, partes, origen_claves, resultado_credenciales, remotos, item_existe):
    total = os.path.getsize(ruta)
    log("=" * 78)
    log("SIMULACIÓN: no se sube nada y no se escribe nada en disco." if not args.confirmar
        else "SUBIDA REAL")
    log("=" * 78)
    log(f"Fichero        : {ruta}")
    log(f"Tamaño         : {formato_tam(total)} ({total:,} bytes)".replace(",", "."))
    if modo == "trozos":
        log("Modo           : trozos (~%d MB cada uno, .partNNN; reanudable)" % args.tam_trozo)
    else:
        log("Modo           : fichero entero (un PUT en streaming; si se corta, se vuelve a subir desde cero)")
    log(f"Partes         : {len(partes)}")
    for p in partes[:20]:
        marca = "ya en el ítem" if p["nombre"] in remotos and remotos[p["nombre"]]["size"] == p["tamano"] else "pendiente"
        log(f"   {p['nombre']:<32} offset {p['offset']:>15,}  tamaño {formato_tam(p['tamano']):>10}  [{marca}]"
            .replace(",", "."))
    if len(partes) > 20:
        log(f"   ... y {len(partes) - 20} más")
    log("-" * 78)
    log(f"Identificador  : {args.ident or '(sin --id: no se comprueba ningún ítem)'}")
    log(f"Ítem existente : {'sí' if item_existe else 'no' if args.ident else '-'}")
    log(f"Colección      : {args.coleccion}")
    log(f"mediatype      : {args.mediatype}")
    log(f"Etiquetas      : {args.etiquetas}")
    log(f"Visible en búsquedas: {'sí' if args.indexar else 'no (noindex)'}")
    if modo == "trozos":
        log(f"Hilos          : {args.hilos}   Reintentos por parte: {args.reintentos}")
    else:
        log(f"Reintentos     : {args.reintentos} (con espera exponencial)")
    log(f"Claves S3      : {'encontradas en ' + origen_claves if origen_claves else 'NO ENCONTRADAS'}")
    if resultado_credenciales is not None:
        log(f"Credenciales   : {resultado_credenciales}")
    pendientes = sum(p["tamano"] for p in partes if not (
        p["nombre"] in remotos and remotos[p["nombre"]]["size"] == p["tamano"]))
    seg = pendientes / (args.velocidad * 1e6)
    log(f"Estimación     : {formato_tam(pendientes)} por subir a {args.velocidad:.1f} MB/s "
        f"~ {formato_tiempo(seg)} (orientativo; depende de tu subida real)")
    log("-" * 78)


# ----------------------------------------------------------------------------------------------
# Principal
# ----------------------------------------------------------------------------------------------
def construir_parser():
    p = argparse.ArgumentParser(
        description="Sube un fichero grande a archive.org en trozos paralelos y reanudable. "
                    "Sin --confirmar solo simula.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Sin --confirmar no se sube nada. Con --confirmar se sube de verdad (requiere --id).")
    p.add_argument("fichero", help="ruta del fichero a subir (p. ej. textures.lopack)")
    p.add_argument("--id", dest="ident", default=None,
                   help="identificador del ítem en archive.org (letras, números, '.', '_', '-'; 3-100 caracteres)")
    p.add_argument("--titulo", default=None, help="título del ítem (por defecto, el nombre del fichero)")
    p.add_argument("--descripcion", default=DESCRIPCION_DEF)
    p.add_argument("--mediatype", default=MEDIATYPE_DEF, help="por defecto: data")
    p.add_argument("--coleccion", default=COLECCION_DEF,
                   help="colección (por defecto opensource_media = 'Community Data')")
    p.add_argument("--etiquetas", default=ETIQUETAS_DEF, help="etiquetas separadas por comas (subject)")
    p.add_argument("--indexar", action="store_true",
                   help="hace el ítem visible en las búsquedas (por defecto NO: lleva noindex)")
    p.add_argument("--confirmar", action="store_true", help="sube de verdad (sin esto solo simula)")
    p.add_argument("--trozos", action="store_true",
                   help="modo alternativo reanudable: parte el fichero en trozos (.partNNN) subidos en paralelo")
    p.add_argument("--hilos", type=int, default=HILOS_DEF,
                   help=f"[solo --trozos] partes en paralelo, 1-{HILOS_MAX} (defecto {HILOS_DEF})")
    p.add_argument("--tam-trozo", type=int, default=TAM_TROZO_DEF_MB,
                   help=f"[solo --trozos] tamaño de cada trozo en MB (defecto {TAM_TROZO_DEF_MB})")
    p.add_argument("--reintentos", type=int, default=REINTENTOS_DEF,
                   help=f"reintentos por trozo ante errores de red o 5xx (defecto {REINTENTOS_DEF})")
    p.add_argument("--velocidad", type=float, default=VELOCIDAD_DEF_MBS,
                   help="MB/s de subida supuestos para la estimación de la simulación")
    p.add_argument("--url-s3", default=S3_URL_DEF, help=argparse.SUPPRESS)
    p.add_argument("--url-meta", default=META_URL_DEF, help=argparse.SUPPRESS)
    return p


def validar_args(args):
    if not (1 <= args.hilos <= HILOS_MAX):
        raise SystemExit(f"--hilos debe estar entre 1 y {HILOS_MAX}.")
    if not (1 <= args.tam_trozo <= 2000):
        raise SystemExit("--tam-trozo debe estar entre 1 y 2000 MB.")
    if args.reintentos < 0 or args.reintentos > 50:
        raise SystemExit("--reintentos debe estar entre 0 y 50.")
    if args.velocidad <= 0:
        raise SystemExit("--velocidad debe ser positiva.")
    if args.ident is not None and not REGEX_IDENTIFICADOR.match(args.ident):
        raise SystemExit("Identificador no válido: usa 3-100 caracteres (letras, números, '.', '_', '-'), "
                         "empezando por letra o número.")
    if args.confirmar and not args.ident:
        raise SystemExit("Para subir de verdad hace falta --id (identificador del ítem).")


def subir_real(args, ruta, ak, sk, origen, modo, partes):
    """Flujo de subida real: hashes -> comprobación remota -> trozos en paralelo -> manifiesto -> verificación."""
    sesion = crear_sesion(args.hilos)
    auth = cabecera_auth(ak, sk)
    base = os.path.basename(ruta)
    total = os.path.getsize(ruta)

    # 1) Comprobar las claves con una petición de solo lectura (antes de leer 28 GB).
    res = comprobar_credenciales(sesion, args.url_s3, ak, sk, args.ident)
    log(f"Credenciales: {res}")
    if res.startswith("NO VÁLIDAS"):
        raise ErrorFatal("Las claves de archive.org no son válidas. No se ha subido nada.")

    # 2) Manifiesto con hashes (una sola lectura del fichero, reutilizable).
    m = calcular_manifiesto(ruta, modo, partes, args.tam_trozo)
    partes = m["partes"]

    # 3) Estado remoto del ítem.
    d = pedir_metadatos(sesion, args.url_meta, args.ident)
    remotos = ficheros_remotos(d)
    item_existe = bool(d)
    log(f"Ítem '{args.ident}': {'ya existe' if item_existe else 'no existe; se creará'}.")

    presentes, pendientes = [], []
    for p in partes:
        r = remotos.get(p["nombre"])
        if r and r["size"] == p["tamano"] and r["md5"] == p["md5"]:
            presentes.append(p)
        else:
            pendientes.append(p)
    for p in presentes:
        log(f"  ya en el ítem, se salta: {p['nombre']}")

    ctx = Contexto(args, sesion, auth, ruta, total,
                   bytes_base=sum(p["tamano"] for p in presentes),
                   partes_base=len(presentes), modo=modo)
    # Estado local: se conserva lo registrado en ejecuciones anteriores del mismo ítem.
    previo = leer_estado(ruta, args.ident)
    ctx.estado = {"identificador": args.ident, "fichero": base, "modo": modo,
                  "creado": previo.get("creado", dt.datetime.now().isoformat(timespec="seconds")),
                  "partes": dict(previo.get("partes", {}))}
    for p in presentes:
        ctx.estado["partes"][p["nombre"]] = {"tamano": p["tamano"], "md5": p["md5"],
                                             "verificada_en_remoto": True}
    etiquetas = [e.strip() for e in args.etiquetas.split(",") if e.strip()]
    errores = []

    # 4) Todas las subidas van en hilos de trabajo. En Windows, Ctrl+C no interrumpe un send() bloqueado
    #    en el hilo principal, así que el hilo principal solo espera (con sleep) y así Ctrl+C responde.
    bytes_pend = sum(p["tamano"] for p in pendientes)
    if pendientes and not item_existe:
        # Si el ítem no existe, el primer trozo va solo y lleva los metadatos del ítem.
        primero = pendientes.pop(0)
        log(f"Creando el ítem con {primero['nombre']} ...")
        resultado_primero = []

        def correr_primero():
            try:
                subir_fichero(ctx, primero["nombre"], ruta, primero["offset"], primero["tamano"],
                              extra_cab=cabeceras_meta(args, etiquetas))
                ctx.marcar_ok(primero["nombre"], primero["tamano"], primero["md5"])
            except BaseException as e:
                resultado_primero.append(e)

        hilo_primero = threading.Thread(target=correr_primero, daemon=True)
        hilo_primero.start()
        esperar_hilos(ctx, [hilo_primero], len(partes), len(presentes), bytes_pend)
        if resultado_primero:
            e = resultado_primero[0]
            if isinstance(e, ErrorParte):
                raise ErrorFatal(str(e) + " Vuelve a ejecutar el mismo comando para reintentar.")
            raise e

    if pendientes:
        cola = queue.Queue()
        for p in pendientes:
            cola.put(p)
        n_hilos = 1 if modo == "entero" else min(args.hilos, len(pendientes))
        hilos = [threading.Thread(target=trabajador, args=(ctx, cola, errores), daemon=True)
                 for _ in range(n_hilos)]
        log(f"Subiendo {len(pendientes)} {'fichero' if modo == 'entero' else 'trozo(s)'} con {n_hilos} hilo(s). "
            f"Ctrl+C para parar (se puede reanudar con el mismo comando).")
        for h in hilos:
            h.start()
        esperar_hilos(ctx, hilos, len(partes), len(presentes), bytes_pend)

    if errores:
        for e in errores:
            aviso(e)
        raise ErrorFatal(f"{len(errores)} error(es) de subida. Vuelve a ejecutar el mismo comando para reintentar "
                         "solo lo que falta.")

    # 5) Modo trozos: subir el manifiesto (pequeño) para que el instalador pueda verificar la reconstrucción.
    mf = ruta_manifiesto(ruta)
    esperado = {p["nombre"]: (p["tamano"], p["md5"]) for p in partes}
    if modo == "trozos":
        tam_mf = os.path.getsize(mf)
        with open(mf, "rb") as f:
            md5_mf = hashlib.md5(f.read()).hexdigest()
        log(f"Subiendo el manifiesto {os.path.basename(mf)} ...")
        subir_fichero(ctx, os.path.basename(mf), mf, 0, tam_mf)
        esperado[os.path.basename(mf)] = (tam_mf, md5_mf)

    # 6) Verificación final contra la API de metadatos (tamaño y MD5).
    ok, malos, faltan, sin_md5 = verificar(sesion, args.url_meta, args.ident, esperado)
    print()
    if malos:
        for x in malos:
            aviso(f"No coincide: {x}")
        raise ErrorFatal("La verificación ha fallado: revisa los ficheros remotos antes de borrar nada local.")
    if not ok:
        raise ErrorFatal("Tras 15 minutos archive.org aún no muestra: " + ", ".join(faltan)
                         + ". Vuelve a ejecutar el mismo comando más tarde para verificar.")
    if sin_md5:
        aviso("archive.org no ofrece MD5 para: " + ", ".join(sin_md5) + " (solo se ha comprobado el tamaño).")

    log("")
    log("VERIFICACIÓN CORRECTA: tamaño y MD5 de todos los ficheros coinciden con los locales.")
    log(f"Ítem: https://archive.org/details/{args.ident}")
    for p in partes:
        log(f"  Descarga: {DESCARGA_URL}/{args.ident}/{quote(p['nombre'])}")
    if modo == "trozos":
        log(f"  Descarga: {DESCARGA_URL}/{args.ident}/{quote(os.path.basename(mf))}")
    log(f"SHA-256 total (para comprobar la reconstrucción): {m['sha256_total']}")
    return 0


def main(argv=None):
    # Consola de Windows: evitar errores al imprimir caracteres no ASCII.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

    args = construir_parser().parse_args(argv)
    validar_args(args)

    ruta = os.path.abspath(args.fichero)
    if not os.path.isfile(ruta):
        print(f"No existe el fichero: {ruta}", file=sys.stderr)
        return 1
    total = os.path.getsize(ruta)
    if total == 0:
        print("El fichero está vacío; no se sube.", file=sys.stderr)
        return 1
    if args.titulo is None:
        args.titulo = os.path.basename(ruta)

    ak, sk, origen = leer_credenciales()
    modo, partes = planificar(ruta, args.tam_trozo, args.trozos)

    try:
        if not args.confirmar:
            # ----- SIMULACIÓN: solo lectura, no escribe nada -----
            sesion = crear_sesion(args.hilos)
            remotos, item_existe, res = {}, False, None
            if args.ident:
                try:
                    d = pedir_metadatos(sesion, args.url_meta, args.ident)
                    remotos = ficheros_remotos(d)
                    item_existe = bool(d)
                except ErrorFatal as e:
                    aviso(str(e))
                if ak and sk:
                    res = comprobar_credenciales(sesion, args.url_s3, ak, sk, args.ident)
            mostrar_plan(args, ruta, modo, partes, origen, res, remotos, item_existe)
            if not ak:
                aviso("No se han encontrado claves S3. Define IA_ACCESS_KEY / IA_SECRET_KEY o ejecuta `ia configure`.")
            log("Para subir de verdad (con reanudación):")
            log(f'  python "{os.path.abspath(__file__)}" "{ruta}" --id {args.ident or "<identificador>"} --confirmar'
                + (" --trozos" if args.trozos else "")
                + ("" if not args.trozos or args.hilos == HILOS_DEF else f" --hilos {args.hilos}"))
            return 0

        # ----- SUBIDA REAL -----
        if not ak or not sk:
            print("No hay claves S3 (IA_ACCESS_KEY / IA_SECRET_KEY o `ia configure`). No se sube nada.",
                  file=sys.stderr)
            return 1
        log(f"Claves S3 leídas de: {origen} (no se muestran).")
        return subir_real(args, ruta, ak, sk, origen, modo, partes)

    except KeyboardInterrupt:
        print("\nInterrumpido por el usuario. Vuelve a ejecutar el mismo comando para reanudar.", file=sys.stderr)
        return 130
    except ErrorFatal as e:
        print(f"\nERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
