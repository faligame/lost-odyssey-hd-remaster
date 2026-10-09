# Pendientes y mejoras acordadas (viva)

## En marcha
- [x] `data\common` (opción B) HECHO el 8-oct-2026: ver `DISENO_DATA_COMMON.md`. Falta probar a mano el clic del asistente y jugar un rato leyendo de `data\` (el juego arranca y lee los 4 discos desde la unión).

## Para publicar la v0.0.1
- [ ] Subir `textures.lopack` a Archive.org (lo sube el usuario por el navegador): comprobar id `lost-odyssey-hd-remaster-texturas` y tamaño 28.075.019.365 B; SHA-256 `a38d1cb4615e255e7ab754a3e7e998d3b8f39f25f331757074ab72e313473847` (si cambia el fichero: `tools/lopack/generar_manifiesto.py --fichero ... --id ...`).
- [ ] Subir `publicacion/texture-pack.json` a la raíz del repo `faligame/lost-odyssey-hd-remaster` (rama main).
- [ ] Probar a mano: clic del ratón en el asistente (Instalar ahora / Cancelar / opciones), botón *Descargar ahora* de Configuración > Texturas (estado habilitado/deshabilitado), jugar un rato en D3D12 y Vulkan con el paquete limpio.
- [ ] Crear la release en GitHub con el zip (`release/LostOdysseyHD-0.0.1-win64.zip`) y `SHA256SUMS.txt`; el `.map` queda privado (`release/simbolos/0.0.1/`).

## Mejoras conocidas (no bloquean la 0.0.1)
- [ ] Lista de pipelines esenciales: ampliar con sesiones largas (D3D12 ~1048 y Vulkan ~400 entradas), discos 2-4, y regenerar la semilla (hoy resuelve ~90 %).
- [ ] Reparar / desinstalar desde el asistente; añadir un disco que falte sin repetir todo el asistente.
- [ ] Gamepad en el asistente (hoy ratón + Intro/Esc).
- [ ] Textos FR/DE/IT/JA del asistente (caen al inglés).
- [ ] Limpiar logs periódicos del plugin ("odisea rendimiento", "tirones", "borrado rapido: resumen") o ponerlos detrás de un cvar.
- [ ] Cambiar el nombre del exe (`lostodyssey.exe`) y de ciertos textos que aún dicen "ReXGlue" para el jugador (los créditos se quedan).
- [ ] Layouts de mando (Switch / PlayStation / Xbox 360): versión futura, opción en Configuración > Extras.
- [ ] Edición asiática (v4): mismo ejecutable descifrado; hay que añadir sus SHA-256/media IDs y un pack de texturas propio.
- [ ] Linux / Steam Deck: instalador sin Win32 (diálogos, WinHTTP, BCrypt), hay que portar; por eso no se usan enlaces duros.
- [ ] Dedup más allá de ficheros enteros: no compensa (0,7 GB).

## Hecho (resumen, ver memoria `version-publica-v0-0-1.md` para detalle)
Modo release (`LO_DEV`), rutas portables, esenciales D3D12/Vulkan, `.lopack` cifrado, descarga paralela con reanudación, crash reporter, ticks por disco, instalación de discos en `data\discN`, asistente ES/EN, paquete zip y licencias.

## Auto-actualización (8-oct-2026)
- [x] Implementada y probada en local: ver `ACTUALIZACIONES.md`. Falta botón en Extras, probar reapertura tras fallo y publicar el primer `latest.json` real.

## Interfaz propia que viaja con el juego (9-oct-2026)
- `ui_assets/ui.lopack` (4 MB, cifrado y atado a los discos como el pack grande) lleva 28 PNG: logo del título, 25 páginas de fuentes x4 y los 2 atlas de botones DualSense (fuente en `ui_assets/src/`). Va dentro del zip del juego, NO se descarga.
- El plugin (`odisea_texture_replace.cpp`) abre `ui.lopack` (cvar `odisea_texture_ui_pack_file`) además de `textures.lopack`; el de interfaz manda sobre el descargado. Regenerar: `python tools/lopack/crear_lopack.py --texturas ui_assets/src --disco1 <data\disc1> --salida ui_assets/ui.lopack`.
- Versión pública: `OnPreSetup` fuerza `odisea_texture_pack=true` (en instalación nueva el plugin lo traía apagado y el pack no se aplicaba hasta tocar opciones).
- Al terminar la descarga del pack ya se pedía recargar (`ReloadTexturePack`); probado en caliente (ver ACTUALIZACIONES/ARQUITECTURA).
- Mapeos de otros mandos (Switch, Xbox 360): versión futura; añadir sus atlas a `ui_assets/src` y regenerar.

## 9-oct-2026 (tarde)
- [x] Primera ejecución rápida, ajustes iniciales en el asistente, fuentes HD, sin animación por CPU: ver `CAMBIOS_2026-10-09.md`.
- [ ] Probar a mano con ratón/teclado: pantalla previa (sin disco 1), paso de ajustes (clics en celdas/deslizadores, flechas), reinicio al pulsar Jugar tras cambiar idioma/resolución.
- [ ] Textos FR/DE/IT/JA de las cadenas nuevas.
