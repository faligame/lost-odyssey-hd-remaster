// lostodyssey - ReXGlue Recompiled Project
//
// Multidisco: Lost Odyssey viene en 4 discos, cada uno como carpeta extraida,
// ISO o paquete GOD. Ver multidisc_hooks.cpp.
#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "disc_sources.h"

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace lo {

// Antes de construir el runtime: reune los discos disponibles (lo_discs, la
// carpeta de datos, y lo que haya junto a ella y junto al exe) y elige el de
// arranque. Devuelve la carpeta que el SDK usara como game_data_root: la del
// disco si esta extraido, o una de la cache con solo su default.xex si es una
// ISO o un GOD (el SDK carga el ejecutable desde una carpeta). nullopt si no
// hay ningun disco.
std::optional<std::filesystem::path> MultiDiscResolveBoot(const std::filesystem::path& game_data_root,
                                                          const std::filesystem::path& cache_root);

// Pide al usuario el disco 1 (default.xex, .iso o cabecera GOD). nullopt si cancela.
std::optional<std::filesystem::path> MultiDiscPickSource();

// Anade una ruta a lo_discs y la guarda en el toml.
void MultiDiscRememberPath(const std::filesystem::path& config_path, const std::filesystem::path& path);

// Runtime construido y ejecutable cargado: si el disco de arranque es una ISO
// o un GOD, se monta ya en game: y d:.
void MultiDiscAttach(rex::filesystem::VirtualFileSystem* fs);

// Depuracion (lo_start_disc): monta otro disco antes de que empiece el juego.
// Llamar cuando el modulo ya esta preparado: el SDK lee game:\default.xex al
// prepararlo y no debe ver el de otro disco.
void MultiDiscApplyStartDisc();

// Disco de arranque (para leer los recursos del menu), si ya se ha resuelto.
std::optional<DiscSource> MultiDiscBootSource();

// Numero del disco montado ahora mismo (1..4; 0 si aun no hay ninguno).
int MultiDiscMountedNumber();

// Un disco por numero (1..N) de los que se encuentren ahora mismo, empezando por
// el de arranque. Para leer datos de todos los discos (volcado de texturas).
std::vector<DiscSource> MultiDiscAllSources();

}  // namespace lo
