# Diseño: `data\common` (ficheros iguales entre discos, guardados una vez)

Estado: **decidido el 8-oct-2026 (opción B)**. Implementación: ver `PENDIENTES.md`.

## Problema
Los 4 discos de Lost Odyssey ocupan ~23 GB instalados. Nueve ficheros son **idénticos** en los cuatro (1,93 GB cada copia → 5,78 GB repetidos, 25 %):
`LO.fpd, xenon_chr.fpd, xenon_battle.fpd, xenon_obj.fpd, xenon_world.fpd, xenon_vfx.fpd, xenon_loc.fpd, xenon_sys.fpd, xenon_scr.fpd`.
Distintos por disco: `default.xex`, `LO.fpi` y los `xenon_snd/event/field/mov/...`. (Análisis: blake2b por bloques de 1 MB; a nivel de bloque solo se ganan 0,7 GB más → no compensa.)

## Alternativas descartadas
- **`data.bin` único:** un blob de ~17 GB; reparar/actualizar/modear es difícil; rompe la sensación de carpeta de juego de PC.
- **Enlaces duros (NTFS):** solo NTFS → problemas con Steam Deck (exFAT/ext4) y Linux; copiar la carpeta con el Explorador los expande. Se implementó y se **revirtió**.
- **Dedup por bloques:** complejidad alta para 0,7 GB.

## Diseño B
```
data\
  common\      xenon_chr.fpd, xenon_battle.fpd, ... (los ficheros iguales en ≥2 discos, UNA copia)
  disc1\       default.xex, LO.fpi, xenon_snd.fpd, ...   (solo lo propio del disco)
  disc2\ disc3\ disc4\
```
- Es **portable**: ficheros normales, cualquier sistema de ficheros, copiar la carpeta no expande nada, los mods pueden reemplazar ficheros.
- **Lectura:** un dispositivo VFS "unión" (`UnionDevice`) para cada disco: `ResolvePath` busca primero en `discN\` y si no está en `common\`. El listado de directorios
  es la unión de los dos (se construye un árbol propio de entradas que delegan `Open()` en la entrada real del `HostPathDevice` que la contiene). Sin compresión: cada lectura
  se traduce a un fichero físico + offset, coste despreciable.
- **Arranque:** el SDK solo ve `default.xex` (copia en `cache\boot_xex\<mediaid>\`) y nuestro dispositivo se monta en `game:`/`d:` (el mismo camino que ISO/GOD: `g_boot_is_image`).
  Por eso una carpeta `discN` con `common\` hermano se trata como "imagen", no como carpeta directa.
- **Detección:** `IdentifyDiscSource(<...>\discN)` rellena `DiscSource::common = <...>\common` si existe junto a la carpeta y esta se llama `disc<N>`.

## Instalador (`Installer::RunInstall`)
Un solo paso de lectura, sin hash previo:
1. Para cada fichero del disco que se instala: si `data\common\<ruta>` existe con el mismo tamaño y **contenido idéntico** (comparación por bloques ISO ↔ local) → no se copia.
2. Si no, si `data\disc<k>\<ruta>` (otro disco ya instalado) tiene el mismo tamaño y contenido → **se mueve** (rename) a `data\common\<ruta>` y no se copia.
3. Si no, se copia a `data\disc<N>\<ruta>`.
- Reinstalar / añadir discos después funciona igual (los comunes ya están en `common\`).
- Verificación: tras cada disco, `Verify(data\discN)` (media ID + SHA-256 del XEX) y lectura vía el dispositivo unión.

## Riesgos y comprobaciones
- El guest podría enumerar el directorio raíz: el árbol unión debe listar también los ficheros de `common\`.
- Atributos/tamaños/fechas deben coincidir con los de un `HostPathDevice` normal.
- Fallo si falta `common\` (instalación a medias): el dispositivo debe devolver "no encontrado" y el juego pedir el disco (comportamiento actual) — `Verify` y `--lo_setup` lo repararían.
- Prueba objetiva: instalar con el esquema nuevo y comparar, fichero a fichero, los bytes leídos por el dispositivo unión contra los de las 4 carpetas completas.
- Casos: todas las demás lecturas de disco del proyecto (`DiscReader`, volcado de texturas, esenciales, secreto del `.lopack`) usan `CreateDiscDevice` y heredan el cambio.

## Resultado (8-oct-2026)
Implementado en `disc_sources.cpp` (`UnionEntry`/`UnionDevice`, `DiscReader::SameAs`, `DiscSource::common` en `IdentifyDiscSource`), `multidisc_hooks.cpp` (carpeta con `common` = "imagen") e `installer.cpp` (`RunInstall`: omitir si ya está en `common`, mover desde otro disco, o copiar).
Prueba con las 4 ISO reales en carpeta limpia: **27 s**, 16,3 GB reales; `common` = 8 `.fpd` + `LO.fpd` + `$SystemUpdate\` (1,8 GB); `disc1..4` = 3,5 / 2,7 / 4,0 / 4,0 GB. Con las ISO quitadas de la vista el juego arranca desde `data\disc1` (unión) y la precarga lee los paquetes de los discos 1, 2 y 3 con los **mismos números** que desde las ISO (6253/5957/6702 paquetes; 11648/11544/12641 pares).
