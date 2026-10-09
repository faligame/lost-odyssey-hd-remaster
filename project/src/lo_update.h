// lostodyssey - ReXGlue Recompiled Project
//
// Auto-actualizacion del juego (v0.0.1 en adelante). Documentacion: docs/ACTUALIZACIONES.md.
//
// Flujo: OnPreSetup lanza UpdateCheckStart (hilo): lee latest.json y latest.json.sig del repositorio; la
// firma (ECDSA P-256 sobre SHA-256, clave publica en lo_update_key.h) debe cuadrar con los bytes exactos del
// JSON. OnFinalizePaths llama a UpdateOfferAndApply: si hay version mayor, pregunta (Actualizar / Mas tarde /
// Saltar esta version), descarga el zip con una ventanita de progreso, comprueba tamano y SHA-256, lo extrae
// en update\staging con tar.exe (viene con Windows 10+) y lanza update\updater.exe, que espera a que el juego
// termine, copia los ficheros con copia de seguridad (si algo falla, restaura) y vuelve a abrir el juego.
// Nunca se tocan config, saves, data, cache, logs, update ni textures.lopack. Solo en la version publica.
#pragma once

#include <filesystem>

namespace lo {

// Lanza en segundo plano la comprobacion (no hace nada en la version debug, con lo_update_check=false o si ya
// se comprobo hace menos de 6 horas sin novedades).
void UpdateCheckStart();

// Espera un momento al resultado y, si hay una version nueva firmada, la ofrece. Devuelve true si se aplico:
// el updater ya esta lanzado y el juego debe cerrarse YA sin arrancar nada mas.
bool UpdateOfferAndApply(const std::filesystem::path& config_path);

// Menu > Buscar actualizaciones: comprueba ya (ignora el limite de horas y la version saltada) y la ofrece.
// Devuelve true si el juego debe cerrarse (actualizacion lanzada). Muestra un aviso si no hay novedades.
bool UpdateCheckNow(const std::filesystem::path& config_path);

// Al arrancar: borra restos de una actualizacion anterior (update\staging, backup, zip).
void UpdateCleanupLeftovers();

// "x.y.z": true si a > b.
bool VersionNewer(const char* a, const char* b);

}  // namespace lo
