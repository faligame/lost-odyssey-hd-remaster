# Publicar una versión (con auto-actualización)

El juego mira `latest.json` y `latest.json.sig` en la raíz del repositorio (rama `main`). Si la versión es mayor que la suya y la firma es válida, la ofrece. La clave privada de firma está en el equipo de quien publica (`%USERPROFILE%\.lo_signing\update_private.pem`, nunca en el repositorio); la pública va compilada en el exe (`project/src/lo_update_key.h`).

## Pasos

1. Subir la versión en `project/src/lo_version.h` (`LO_VERSION_*`) y en `project/src/lostodyssey.rc` si hace falta.
2. Compilar plugin (`xenos_fork\build_vk.bat`) y juego (`project\build_vk.bat`), sin el juego abierto.
3. Empaquetar: `powershell -ExecutionPolicy Bypass -File tools\empaquetar_release.ps1 -Version X.Y.Z`. Limpia de los binarios la ruta del equipo, añade licencias, `SHA256SUMS.txt` y el zip en `release/`.
4. Calcular el SHA-256 del zip (`LostOdysseyHD-X.Y.Z-win64.zip.sha256`) y firmar el manifiesto:
   `python tools\publicar\publicar_version.py release\LostOdysseyHD-X.Y.Z-win64.zip --version X.Y.Z --notas-es "..." --notas-en "..."`
   (crea `latest.json` y `latest.json.sig` en `release/actualizacion-X.Y.Z/`).
5. En GitHub: crear la release `vX.Y.Z` (marcar *pre-release* mientras sea de prueba), pegar las notas de `docs/releases/vX.Y.Z.md` y subir como *assets* el zip y su `.sha256`. **El nombre del zip tiene que ser exactamente el de la URL del manifiesto.**
6. Comprobar que el zip se descarga desde la release y, **el último**, subir `latest.json` y `latest.json.sig` a la raíz del repositorio. Desde ese momento los juegos antiguos ofrecen la actualización.

## Qué hace el juego al actualizar

Descarga el zip, comprueba tamaño y SHA-256, lo extrae en `update\staging` y lanza `update\updater.exe`, que espera a que el juego se cierre, hace copia de lo que va a reemplazar, copia los ficheros nuevos y, si algo falla, restaura la copia y reabre el juego. No toca `config`, `saves`, `data`, `cache`, `logs`, `update`, `textures` ni `textures.lopack`.

## Estado de v0.0.1

`latest.json` ya apunta a `v0.0.1` (la propia versión publicada): los juegos no ven ninguna actualización, pero el circuito (descarga del manifiesto y comprobación de la firma) queda probado desde el primer día. Detalles técnicos en [dev/ACTUALIZACIONES.md](dev/ACTUALIZACIONES.md).
