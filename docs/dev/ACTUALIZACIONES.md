# Auto-actualización (hecha el 8-oct-2026, v0.0.1)

## Cómo funciona
1. `OnPreSetup` → `UpdateCleanupLeftovers()` (borra `update\staging|backup|package.zip`) y `UpdateCheckStart()` (hilo).
   Solo versión pública (`LO_DEV` no comprueba). Máx. cada 6 h si no hay novedades (`lo_update_last_check`).
2. Lee `latest.json` + `latest.json.sig` de `LO_UPDATE_BASE_URL` (raíz del repo, rama main). Firma ECDSA P-256/SHA-256
   sobre los bytes exactos; clave pública en `project/src/lo_update_key.h`. Sin firma válida no se ofrece nada.
3. `OnFinalizePaths` → `UpdateOfferAndApply`: MessageBox Sí (actualizar) / No (más tarde) / Cancelar (saltar esa
   versión, `lo_update_skip`). Si Sí: ventana de progreso, descarga (tamaño y SHA-256 comprobados), extrae con
   `tar.exe` de Windows en `update\staging`, copia `updater.exe` a `update\updater.exe` y lo lanza; el juego se cierra.
4. `updater.exe` (`project/src/updater.cpp`): espera al PID, copia a `update\backup` lo que reemplaza, copia lo
   nuevo (temporal + renombrado, 20 reintentos), si algo falla RESTAURA y borra lo creado, abre el juego.
   Nunca toca: `config saves data cache logs update textures textures.lopack`, `*.part`. No borra ficheros ausentes del paquete.
   Registro: `update\update.log`.

## Publicar una versión nueva
1. Cambiar versión en `lo_version.h`, compilar, `tools\empaquetar_release.ps1 -Version X`.
2. `python tools\publicar\publicar_version.py release\LostOdysseyHD-X-win64.zip --version X --notas-es "..." --notas-en "..."`
   → crea `latest.json` y `latest.json.sig` (usa la clave privada `%USERPROFILE%\.lo_signing\update_private.pem`).
3. Crear release `vX` en GitHub con el zip; DESPUÉS subir `latest.json` y `latest.json.sig` a la raíz del repo (main).
- La clave privada NO está en el proyecto: hacer COPIA DE SEGURIDAD. Si se pierde hay que sacar a mano una versión con otra clave pública.
- Pruebas locales: `--lo_update_base_url=http://127.0.0.1:PUERTO/` (la firma sigue siendo obligatoria).
- Hosts permitidos del descargador: `lo_http.h` (`HostAllowed`; ya incluye `*.r2.dev` por si el pack va a R2).

## Pruebas hechas (8-oct)
- Actualización 0.0.1→0.0.2 falsa (servidor local, firma real): LEEME reemplazado, fichero nuevo creado, `saves\` y
  `config\` intactos aunque el paquete traía un `saves\slot1.bin`, juego relanzado.
- Fallo forzado (fichero bloqueado): restaura LEEME anterior, borra el fichero nuevo, aviso al usuario.
  (Pendiente de comprobar: que el juego se reabra tras un fallo; en la prueba no se vio abierto a los 12 s.)

## Pendiente
- Botón "Buscar actualizaciones" en Configuración > Extras y casilla `lo_update_check` (la función `UpdateCheckNow` ya existe).
- Textos FR/DE/IT/JA; probar con GitHub real (publicar `latest.json` firmado).
