// lostodyssey - ReXGlue Recompiled Project
//
// Descarga del pack de texturas HD cifrado (.lopack). Se lee un manifiesto pequeno (JSON) con el
// nombre, tamano, SHA-256 y las URL del fichero (Archive.org); se baja con WinHTTP, reanudable
// (fichero .part y cabecera Range), se comprueba el SHA-256 y solo entonces se renombra al nombre
// definitivo. Solo se aceptan servidores de la lista (archive.org, GitHub y, para pruebas, el
// bucle local).
//
// Manifiesto (texture-pack.json):
//   { "schema": 1, "version": 1, "file": "textures.lopack", "size": 30147000000,
//     "sha256": "<64 hex>", "urls": ["https://archive.org/download/<id>/textures.lopack"] }
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace lo {

enum class TextureDownloadState {
  kIdle,      // nada en marcha
  kManifest,  // leyendo el manifiesto
  kDownloading,
  kVerifying,  // SHA-256 del fichero completo
  kDone,
  kError,
};

struct TextureDownloadInfo {
  TextureDownloadState state = TextureDownloadState::kIdle;
  uint64_t done = 0;   // bytes (descargados o comprobados)
  uint64_t total = 0;
  double speed = 0.0;  // bytes/s
  std::string message;  // error, o vacio
};

// Donde esta (o estara) el pack: cvar odisea_texture_pack_file, relativa a la carpeta del exe.
std::filesystem::path TexturePackPath();
// El pack esta descargado (existe y no es un .part).
bool TexturePackInstalled();

// Empieza la descarga en segundo plano (si ya hay una en marcha o el pack ya esta, no hace nada).
void TextureDownloadStart();
// Deja un .part vacio para que el siguiente arranque empiece la descarga (p. ej. si el asistente reinicia el juego
// para aplicar los ajustes y corta la que acababa de empezar).
void TextureDownloadMarkPending();
// Al arrancar: si una descarga se quedo a medias (fichero .part), sigue donde estaba.
void TextureDownloadResumeIfPartial();
void TextureDownloadCancel();
TextureDownloadInfo TextureDownloadStatus();
bool TextureDownloadActive();

// Texto de progreso para la interfaz ("Descargando texturas HD: 34 % (12 MB/s)").
std::string TextureDownloadText();

}  // namespace lo
