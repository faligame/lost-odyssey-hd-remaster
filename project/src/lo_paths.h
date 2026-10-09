// lostodyssey - ReXGlue Recompiled Project
//
// Rutas de la instalacion portable: todo lo del usuario vive junto al .exe.
//   config\config.toml   configuracion
//   saves\saveNN         partidas (flat saves, ver save_headers.h)
//   saves\perfil         perfil del jugador (XUID) y logros
//   cache\ logs\         regenerables
#pragma once

#include <filesystem>

#include <rex/filesystem.h>

namespace lo {

inline std::filesystem::path ExeDir() { return rex::filesystem::GetExecutableFolder(); }
inline std::filesystem::path ConfigDir() { return ExeDir() / "config"; }
inline std::filesystem::path ConfigFile() { return ConfigDir() / "config.toml"; }
inline std::filesystem::path SavesDir() { return ExeDir() / "saves"; }
inline std::filesystem::path ProfileDir() { return SavesDir() / "perfil"; }

// Instalaciones anteriores a la v0.0.1 (config.toml, SAVE\ y savedata\ junto al
// exe): se mueven a la estructura nueva una sola vez, sin pisar nada.
inline void MigrateLegacyLayout() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(ConfigDir(), ec);
  const fs::path old_cfg = ExeDir() / "config.toml";
  if (fs::exists(old_cfg, ec) && !fs::exists(ConfigFile(), ec)) fs::rename(old_cfg, ConfigFile(), ec);
  const fs::path old_saves = ExeDir() / "SAVE";
  if (fs::is_directory(old_saves, ec) && !fs::exists(SavesDir(), ec)) fs::rename(old_saves, SavesDir(), ec);
  fs::create_directories(SavesDir(), ec);
  const fs::path old_profile = ExeDir() / "savedata";
  if (fs::is_directory(old_profile, ec) && !fs::exists(ProfileDir(), ec)) fs::rename(old_profile, ProfileDir(), ec);
}

}  // namespace lo
