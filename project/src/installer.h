// lostodyssey - ReXGlue Recompiled Project
//
// Asistente de instalacion de la primera ejecucion (como el de reblue y Unleashed: el propio juego
// lo abre si no encuentra el disco 1). Esta parte es la logica; el dibujo, con el estilo del menu
// del juego, esta en settings_page.cpp (DrawInstaller).
//
// Pasos: bienvenida -> discos (validados por SHA256 del XEX y media ID, edicion USA/Europa v3) ->
// texturas HD (.lopack) -> ajustes iniciales (idioma, resolucion...) -> sombreadores -> fin. Los discos se leen donde estan (carpeta, ISO o GOD);
// sus rutas quedan en lo_discs.
#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "disc_sources.h"

namespace lo {

enum class DiscStatus {
  kEmpty,
  kOk,
  kNotLostOdyssey,  // no es un disco del juego
  kWrongEdition,    // otra edicion (la asiatica aun no esta admitida)
  kModified,        // el default.xex no es el del disco original
};

struct InstallDisc {
  std::filesystem::path path;
  DiscSource source;
  DiscStatus status = DiscStatus::kEmpty;
};

enum class InstallStep { kWelcome = 0, kDiscs, kInstall, kTextures, kSettings, kShaders, kFinish };

// Copiar los discos a la carpeta del juego (data\\discN, como reblue/Unleashed: despues no hacen falta) o
// leerlos donde estan.
enum class InstallMode { kCopy = 0, kInPlace = 1 };

enum class TextureChoice { kNow = 0, kLater = 1 };

class Installer {
 public:
  // on_done: se llama al terminar (Jugar); el caller resuelve el disco de arranque y reanuda el SDK.
  // on_disc1_ready: el disco 1 acaba de quedar verificado (y recordado en lo_discs): el caller puede
  // resolver el disco de arranque para leer los recursos del menu y dar al asistente el estilo del juego.
  Installer(std::filesystem::path config_path, std::function<void()> on_done,
            std::function<void()> on_disc1_ready = {});
  ~Installer();

  InstallStep step() const { return step_; }
  bool CanGoNext() const;
  void Next();
  void Back();
  void Finish();

  // --- Discos ---
  const InstallDisc& disc(int number) const { return discs_[size_t(number - 1)]; }
  // Identifica lo que haya en path (carpeta, default.xex, .iso, GOD) y lo pone en su hueco.
  // Devuelve un texto de error o vacio.
  std::string AddDiscPath(const std::filesystem::path& path);
  void BrowseFile();
  void BrowseFolder();
  void StartScan();            // busca junto al exe y en data\ (hilo propio)
  bool scanning() const { return scanning_.load(); }
  const std::string& message() const { return message_; }
  bool Disc1Ready() const { return disc(1).status == DiscStatus::kOk; }
  int DiscsReady() const;

  // --- Instalacion de los discos en la carpeta del juego ---
  InstallMode install_mode() const { return install_mode_; }
  void SetInstallMode(InstallMode mode) { if (!install_running()) install_mode_ = mode; }
  void StartInstall();
  void CancelInstall();
  bool install_running() const { return copy_running_.load(); }
  bool install_done() const { return copy_done_.load(); }
  uint64_t install_bytes_done() const { return copy_bytes_done_.load(); }
  uint64_t install_bytes_total() const { return copy_bytes_total_.load(); }
  int install_disc() const { return copy_disc_.load(); }
  int install_discs_total() const { return copy_discs_total_.load(); }
  double install_speed();  // bytes/s (se refresca ~1 vez por segundo)
  std::string install_error() const;
  std::string install_file() const;

  // --- Texturas HD ---
  TextureChoice texture_choice() const { return texture_choice_; }
  void SetTextureChoice(TextureChoice c) { texture_choice_ = c; }

  // --- Sombreadores ---
  bool precache_all() const { return precache_all_; }
  void SetPrecacheAll(bool all) { precache_all_ = all; }

 private:
  void SetDisc(int number, const std::filesystem::path& path, DiscStatus status, const DiscSource& source);

  std::filesystem::path config_path_;
  std::function<void()> on_done_;
  std::function<void()> on_disc1_ready_;
  InstallStep step_ = InstallStep::kWelcome;
  std::array<InstallDisc, 4> discs_;
  std::string message_;
  std::mutex mutex_;
  std::atomic<bool> scanning_{false};
  std::thread scan_thread_;
  TextureChoice texture_choice_ = TextureChoice::kNow;
  bool precache_all_ = true;
  bool auto_scanned_ = false;

  void RunInstall();
  InstallMode install_mode_ = InstallMode::kCopy;
  std::thread copy_thread_;
  std::atomic<bool> copy_running_{false};
  std::atomic<bool> copy_done_{false};
  std::atomic<bool> copy_cancel_{false};
  std::atomic<uint64_t> copy_bytes_done_{0};
  std::atomic<uint64_t> copy_bytes_total_{0};
  std::atomic<int> copy_disc_{0};
  std::atomic<int> copy_discs_total_{0};
  mutable std::mutex copy_text_mutex_;
  std::string copy_error_;
  std::string copy_file_;
  double copy_speed_ = 0.0;
  uint64_t speed_last_bytes_ = 0;
  int64_t speed_last_ms_ = 0;
};

}  // namespace lo
