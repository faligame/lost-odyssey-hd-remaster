# Arquitectura y flujo de trabajo

## Dos versiones del mismo código
El exe tiene **dos modos de compilación** controlados por la opción de CMake `LO_DEV` (define el símbolo `LO_DEV`):

| | Versión pública (`LO_DEV=OFF`, por defecto) | Versión debug (`LO_DEV=ON`) |
|---|---|---|
| Overlays del SDK F3 / F4 / consola (`º`) | **quitados** (`UnregisterBind` en `OnCreateDialogs`) | activos |
| Sondas (`d3d_probe_hooks`, `heap_watch`, `encounter_hooks`), volcado de texturas del disco, F7 recargar texturas | **no se compilan** | compilados |
| Ajuste en vivo (`lo_live_tuning.txt`), pruebas de cierre/cuelgue, vista previa de la pantalla de sombreadores | fuera | dentro |
| Página **Texturas** de la Configuración | solo estado + botón *Descargar ahora* | además volcar / recargar |
| Código común (instalador, descarga del pack, crash reporter, rutas portables, esenciales) | igual | igual |

**Regla:** todo lo que sea de jugador (instalador, opciones, descarga, informes) va **sin** `#ifdef LO_DEV`. Lo que sea herramienta de investigación va
detrás de `#ifdef LO_DEV` (exe) o de un cvar `GPU/Debug` apagado por defecto (plugin gráfico, que no tiene `LO_DEV`).
Las mejoras hechas en cualquiera de las dos se escriben en `docs/PENDIENTES.md` y se portan a la otra.

## Árbol
```
Rexglue/
  project/                 exe (lostodyssey.exe): src/*.cpp, CMakeLists.txt, lostodyssey_manifest.toml (recompilación), generated/ (NO se publica)
    out/build/win-amd64-release/   build + carpeta donde se juega en desarrollo (config/, saves/, cache/, textures/, discos en data/ o isos...)
    src_backup_2026-10-08/         copias antes de cada gran cambio (el proyecto NO tiene git)
  xenos_fork/              plugin gráfico rexgpu-odisea.dll (D3D12 + Vulkan): src/graphics, include/rex/graphics (cabeceras que pisan las del SDK)
  rexglue-sdk-src/         SDK ReXGlue compilado con Vulkan (out/install/win-amd64)
  tools/lopack/            crear/verificar .lopack, subir_archive.py, generar_manifiesto.py, ia.ini (SECRETO)
  tools/empaquetar_release.ps1     genera release/LostOdysseyHD-<v>-win64.zip
  release_docs/LEEME.txt           texto que va dentro del zip (ES/EN)
  publicacion/texture-pack.json    manifiesto del pack de texturas (se sube a GitHub, rama main)
  release/                 zip, carpeta del zip y prueba-manual/; release/simbolos/<v>/lostodyssey.map (NO se publica)
  pruebas-instalacion-limpia/      copia de pruebas con el .lopack completo (28 GB)
  docs/                    estas notas
```

## Compilar
- **Exe:** `project\build_vk.bat` (CMake preset `win-amd64-release`, clang de VS 2022, SDK Vulkan). Por defecto `LO_DEV=OFF`.
  Para la debug: `project\build_vk.bat dev` (añade `-DLO_DEV=ON`; vuelve a la pública con `project\build_vk.bat`). Cambiar el modo reconfigura y recompila.
- **Plugin:** `xenos_fork\build_vk.bat` (copia `rexgpu-odisea.dll` junto al exe de `project\out\build\win-amd64-release`).
- **Codegen** (solo si cambia `lostodyssey_manifest.toml`): lo lanza el propio build (~85 s).
- **Siempre con el juego cerrado** (la copia de la DLL falla si está abierto). Desde PowerShell: `Start-Process -Wait -NoNewWindow -FilePath <bat> -RedirectStandardOutput log -RedirectStandardError err`.
- Un fichero de cabecera NUEVO en `xenos_fork/include` obliga a borrar `xenos_fork/out` y recompilar entero (ver CMakeLists).

## Empaquetar la versión pública
1. Compilar exe **sin** `dev` y el plugin.
2. `tools\empaquetar_release.ps1 [-Version 0.0.1]` → zip + carpeta + `SHA256SUMS.txt`; deja el `.map` en `release/simbolos/<v>/`.
3. NO va en el zip: `d3d12.dll` (proxy de otra herramienta), `rexgpu-xenos*.dll` antiguos, `.map`, config/saves/cache/logs, discos, `.lopack`, `ia.ini`.
4. Probar el zip extraído en una carpeta limpia **lejos de discos** (el juego busca discos en la carpeta, `data\`, y la carpeta padre hasta 4 niveles).

## Cómo probar sin el usuario
- Asistente: `lostodyssey.exe --lo_setup=true` (fuerza el asistente aunque haya discos). Con Windows: `PostMessage(WM_KEYDOWN VK_RETURN)` avanza pasos si la ventana tiene el foco (AppActivate + SetForegroundWindow).
- Descarga del pack: servidor Python con soporte de `Range` (`serve_range.py`, ver abajo) y `--lo_texture_manifest_url=http://127.0.0.1:<puerto>/texture-pack.local.json`.
- Aviso de cierre: poner `logs\crash_AAAAMMDD_HHMMSS.txt` (informe falso en inglés) y abrir el exe; se marca `.visto` tras preguntar.
- Capturas: solo de la ventana del juego (PrintWindow), nunca la pantalla entera.
- **No usar la carpeta de desarrollo con `simular_instalacion_nueva.ps1`:** ya se perdió una caché de pipelines rica.

## Subsistemas y dónde viven
| Qué | Dónde |
|---|---|
| Rutas portables (`config\`, `saves\`, `saves\perfil`, migración) | `project/src/lo_paths.h` |
| Versión, URLs de manifiesto y de issues | `project/src/lo_version.h` (`LO_TEXTURE_MANIFEST_URL`, `LO_ISSUES_URL`) |
| Asistente de instalación (lógica / dibujo) | `installer.{h,cpp}` / `DrawInstaller` en `settings_page.cpp` |
| Discos: ISO/GOD/carpeta, lectura, copia | `disc_sources.{h,cpp}`, `multidisc_hooks.cpp` |
| Descarga del pack HD (WinHTTP, N conexiones por bloques, reanuda, SHA-256) | `texture_download.{h,cpp}` |
| Formato .lopack (AES-256-GCM + zstd, clave derivada del disco 1) | `tools/lopack/lopack_formato.py`, `xenos_fork/src/graphics/odisea_lopack.{h,cpp}`, `texture_pack_secret.cpp` |
| Crash reporter (informe + issue de GitHub prefijado) | `crash_handler.cpp` (informe, inglés), `crash_reporter.cpp` (aviso al arrancar) |
| Pipelines esenciales (primera ejecución sin caché) | plugin `OdiseaSeedEssentials` (D3D12 y Vulkan), `shader_prewarm.cpp` (`EssentialsProvider`), `prewarm_essentials.*.xpso` |
| Precarga de sombreadores por disco, ticks | `shader_prewarm.cpp`, `DrawShaderPrep` (`settings_page.cpp`), `odisea_PrewarmDiscDone` (plugin) |
| Textos (ES/EN; FR/DE/IT/JA caen al inglés) | `lo_i18n.cpp` (clave = texto en español) |
| Requisitos/equipo | `lo_system_info.h` |

## Convenciones y avisos
- Cadenas de jugador: se escriben en español en el código y se traducen con `Tr()` / `p.L()`; añadir la entrada en `lo_i18n.cpp` (columnas EN,FR,DE,IT,ES,JA; vacías = inglés).
- El informe de cierre y el issue van **siempre en inglés**.
- Nunca imprimir ni guardar las claves de Archive.org; `tools/lopack/ia.ini` no se comparte.
- Publicar: solo el exe/runtime/DLL propias, el `.lopack` cifrado y los manifiestos. Nunca `generated/`, datos de discos ni texturas sin cifrar.
