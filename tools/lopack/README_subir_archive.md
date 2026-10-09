# subir_archive.py

Sube `textures.lopack` (~28 GB) a un ítem de archive.org desde Windows. Solo necesita la biblioteca
estándar y `requests`.

## Modos

**Fichero entero (por defecto).** Un único PUT en streaming a `https://s3.us.archive.org/<id>/<fichero>`,
leyendo el fichero en bloques de 8 MB (no se carga en memoria).

- Si la subida se corta, se reintenta con espera exponencial, y como un PUT único no se puede reanudar,
  **se vuelve a subir desde cero**. El script lo dice en la salida.
- Antes de subir, si el fichero ya está en el ítem con el mismo tamaño y MD5, no hace nada.
- El resultado es un único fichero `textures.lopack`. El instalador lo descarga tal cual.

**Trozos (`--trozos`, alternativa reanudable).** El fichero se parte en trozos de ~1,9 GB
(`textures.lopack.part001`, `.part002`, ...) que se suben en paralelo como ficheros del mismo ítem. Cada
trozo se reintenta solo y los ya presentes (mismo tamaño y MD5) se saltan. Requiere que el instalador
descargue todos los trozos y los concatene en orden. El manifiesto `textures.lopack.parts.json` (que se
sube al ítem) trae offset, tamaño, MD5 y SHA-256 de cada trozo y el SHA-256 total para verificarlo.

**Multiparte S3** (CreateMultipartUpload / UploadPart / CompleteMultipartUpload): no está implementado.
La documentación de archive.org solo lo menciona de pasada ("ias3 has support for multipart uploads" y
una nota sobre `x-archive-keep-old-version`), no describe el protocolo y la biblioteca oficial `ia` no lo
usa. No hay una forma fiable de usarlo sin probarlo en una cuenta real.

## Claves

Se leen, en este orden, de:

1. Variables de entorno `IA_ACCESS_KEY` / `IA_SECRET_KEY` (también `IA_ACCESS_KEY_ID` / `IA_SECRET_ACCESS_KEY`).
2. El fichero de `ia configure`: `%USERPROFILE%\.config\internetarchive\ia.ini` (sección `[s3]`).

Las claves nunca se muestran ni se guardan. Para definirlas en PowerShell solo para la sesión actual:

```powershell
$env:IA_ACCESS_KEY = "..."
$env:IA_SECRET_KEY = "..."
```

## Uso

Simulación (por defecto: no sube ni escribe nada, solo valida claves con una petición de lectura y muestra el plan):

```
python subir_archive.py "<carpeta>\textures.lopack" --id <identificador>
```

Subida real (requiere `--confirmar`):

```
python subir_archive.py "<carpeta>\textures.lopack" --id <identificador> --confirmar
```

Modo trozos (en la subida real y en la simulación):

```
python subir_archive.py "<ruta>\textures.lopack" --id <identificador> --trozos --hilos 4 --confirmar
```

Opciones principales:

| Opción | Por defecto | Qué hace |
|---|---|---|
| `--id` | (obligatorio al subir) | Identificador del ítem: letras, números, `.`, `_`, `-` (3-100 caracteres) |
| `--trozos` | desactivado | Modo trozos (reanudable). Sin esta opción se sube el fichero entero |
| `--hilos` | 4 | Solo `--trozos`: partes en paralelo, de 1 a 8 |
| `--tam-trozo` | 1900 | Solo `--trozos`: MB por trozo |
| `--reintentos` | 6 | Reintentos (errores de red o 5xx), con espera exponencial: 5 s, 10 s, 20 s... máx. 300 s |
| `--coleccion` | `opensource_media` | "Community Data" (datos donados por particulares) |
| `--mediatype` | `data` | |
| `--etiquetas` | `lost odyssey, lopack, recompilacion` | Separadas por comas |
| `--titulo`, `--descripcion` | nombre del fichero / texto fijo | Metadatos del ítem |
| `--indexar` | desactivado | Por defecto el ítem lleva `noindex` (no sale en búsquedas) |
| `--velocidad` | 5 | MB/s supuestos solo para la estimación de la simulación |

## Progreso y parada

Muestra porcentaje, MB transferidos, MB/s, tiempo restante estimado y partes (en modo entero, 1 parte).
Ctrl+C detiene los envíos y sale con código 130. Si hay un envío en curso, puede tardar unos segundos
en pararse (lo que tarde en vaciarse el búfer del socket).

## Reanudar

Vuelve a ejecutar **exactamente el mismo comando** con `--confirmar`:

- Modo entero: si el fichero ya está completo en el ítem, no hace nada; si no, sube de nuevo desde cero.
- Modo `--trozos`: se suben solo las partes que falten.

Ficheros que se crean junto al original (en la misma carpeta):

- `textures.lopack.parts.json`: manifiesto con SHA-256 total y MD5 de cada parte. Se reutiliza mientras el
  fichero no cambie. En modo entero también se crea (queda en local, no se sube).
- `textures.lopack.subida.json`: estado informativo. La comprobación real se hace contra archive.org.

## Verificación final

Al terminar, el script consulta `https://archive.org/metadata/<id>` (puede tardar unos minutos en reflejar
la subida) y compara tamaño y MD5 de cada fichero con el local. Muestra las URL de descarga
(`https://archive.org/download/<id>/<fichero>`) y el SHA-256 total del fichero.

## Cómo elegir `--hilos` (solo `--trozos`)

- Cada hilo es una subida simultánea. Con poca subida (p. ej. 20-30 Mbit/s), 2-4 hilos llenan la línea.
- Con fibra simétrica (500 Mbit/s o más), 4-8 hilos ayudan.
- Si archive.org devuelve muchos 503 (`SlowDown`), baja a 2 hilos.
- No necesita espacio extra en disco: los trozos se leen del original, no se copian.

## Límites y riesgos

- archive.org no documenta un tamaño máximo por fichero. El modo entero envía los 28 GB en un único
  PUT; si archive.org lo rechaza por tamaño, usa `--trozos`.
- Un PUT interrumpido en modo entero se repite desde cero: con una conexión inestable, `--trozos` es
  más seguro.
- Un ítem es público: `noindex` solo lo oculta de las búsquedas; quien tenga la URL puede descargarlo.
  Solo se deben subir ficheros cifrados (`.lopack`), nunca datos sin cifrar, código generado ni BIOS.
- La colección `opensource_media` y el permiso de subida de la cuenta no se han podido comprobar sin
  publicar. Si la primera subida da 403, prueba otra colección con `--coleccion`.
- La validación de claves de la simulación usa un listado de solo lectura; archive.org no documenta un
  endpoint específico para ello, así que el resultado es orientativo.
