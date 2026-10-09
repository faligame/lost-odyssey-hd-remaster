// lostodyssey - ReXGlue Recompiled Project
//
// Recursos graficos del menu del juego leidos EN TIEMPO DE EJECUCION de los
// datos del disco del usuario (nada del juego va en el ejecutable ni en el
// repositorio): la textura UI_MAIN_00 (metal, esquina, iconos), la textura
// "window" (cursor) y las fuentes Maru23 (texto) y LocTit1 (titulos).
//
// Cadena de lectura (formatos documentados publicamente; codigo propio):
//   LO.fpi (indice) -> xenon_loc.fpd (archivo) -> bloque "cpx" (LZ propio)
//   -> paquete Unreal Engine 3 big-endian -> Texture2D (DXT5 en bloques de
//   la Xbox 360: LZO1X + mosaico Xenos + bytes intercambiados) y Font
//   (tabla de glifos sobre paginas Texture2D).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "disc_sources.h"

namespace lo::menu_assets {

struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;  // R8G8B8A8
  // Huella de la textura en el juego (la misma clave que el pack de texturas): con ella se pide la version HD.
  uint64_t hash = 0;
};

struct Glyph {
  uint16_t x = 0, y = 0, w = 0, h = 0;
  uint8_t page = 0;
};

struct Font {
  std::vector<Image> pages;  // cara blanca y contorno negro sobre alfa
  // Version HD de cada pagina (x4, de ui.lopack) si existe; rgba vacio = solo la original. Mismos indices.
  std::vector<Image> hd_pages;
  std::unordered_map<uint32_t, Glyph> glyphs;  // punto de codigo -> glifo
  int32_t kerning = 0;  // se suma al ancho de cada glifo
  uint16_t height = 0;  // alto de celda (el mayor de los glifos)
};

struct MenuAssets {
  std::string language;  // carpeta de idioma usada ("spa", ...)
  Image ui_main;         // UI_MAIN_00 (512x1024)
  Image window;          // window (256x1024)
  Font text;             // Maru23
  Font title;            // LocTit1
};

// Lee los recursos del disco (carpeta, ISO o GOD: donde estan LO.fpi y los
// .fpd). Rellena error y devuelve false si falta algo o los datos no tienen el
// formato esperado (el menu del port recurre entonces a su estilo propio).
bool Load(const DiscSource& disc, MenuAssets& out, std::string& error);

}  // namespace lo::menu_assets
