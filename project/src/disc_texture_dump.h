// lostodyssey - ReXGlue Recompiled Project
//
// Volcado de TODAS las texturas del juego leyendolas de los discos, sin tener
// que pasar por cada zona. Ver disc_texture_dump.cpp.
#pragma once

#include <string>

namespace lo {

// Empieza el volcado en segundo plano (no hace nada si ya esta en marcha).
// Salida: dump/disc_textures/<color|normales|iluminacion>/<paquete>/tex_<HASH>_<w>x<h>_f<fmt>_<nombre>.png
// con el mismo hash que usa el pack de texturas (odisea_texture_pack).
void DiscTextureDumpStart();

// Si lo_dump_disc_textures esta activo (arranque con el modulo ya cargado).
void DiscTextureDumpAutoStart();

bool DiscTextureDumpRunning();

// Una linea para el menu: progreso o resumen final.
std::string DiscTextureDumpStatus();

}  // namespace lo
