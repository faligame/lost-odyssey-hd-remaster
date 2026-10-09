// lostodyssey - ReXGlue Recompiled Project
//
// Pestanas "Opciones del port" dentro de la pantalla de Configuracion del juego
// (opcion A del diseno, design/menu-ajustes). LB/RB recorre Juego (la pagina
// nativa, intacta) y las pestanas del port: Graficos, Parches, Extras y
// Texturas. Se dibujan con la fuente y las texturas del propio menu del juego,
// leidas de sus datos (menu_assets.h), imitando su maquetacion.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include "installer.h"
#include "lo_options.h"
#include "menu_assets.h"

namespace lo {

// Llamada desde los dos puntos donde el juego lee el mando (turbo_hooks.cpp).
void SettingsPageProcessInput(uint8_t* state);

struct GameMenuTextures;
struct SettingsPainter;
enum class SettingsRowAction : int;

class SettingsPageDialog : public rex::ui::ImGuiDialog {
 public:
  // request_close_window: cierra la ventana del juego (tras lanzar el reinicio).
  // Los recursos del menu se leen del disco de arranque en cuanto se conoce
  // (lo::MultiDiscBootSource).
  SettingsPageDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
                     std::filesystem::path config_path, std::function<void()> request_close_window);
  ~SettingsPageDialog() override;

  // Asistente de instalacion (primera ejecucion): mientras exista, la pagina solo lo dibuja a el.
  void StartInstaller(std::shared_ptr<Installer> installer) { installer_ = std::move(installer); }
  void EndInstaller() { installer_.reset(); }
  // El asistente ha cambiado ajustes que solo se aplican reiniciando (idioma, resolucion...).
  bool WizardRestartNeeded() const { return edit_.NeedsRestartFrom(running_); }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct AssetLoad {
    bool ok = false;
    menu_assets::MenuAssets assets;
    std::string error;
  };

  void PollAssets();
  void HandleInput(int page, int64_t now_ms);
  void RunAction(SettingsRowAction action, int64_t now_ms);
  void Commit(int64_t now_ms);
  void SetStatus(std::string text, int64_t now_ms);
  bool ActionEnabled(SettingsRowAction action) const;
  void DrawPage(const SettingsPainter& p, int page, int64_t now_ms) const;

  rex::ui::ImmediateDrawer* immediate_drawer_;
  std::filesystem::path config_path_;
  std::function<void()> request_close_window_;
  // La lectura de los datos del juego va en otro hilo; las texturas se crean
  // en el de interfaz cuando termina.
  std::future<std::unique_ptr<AssetLoad>> loading_;
  std::unique_ptr<AssetLoad> loaded_;
  std::unique_ptr<GameMenuTextures> textures_;
  std::shared_ptr<Installer> installer_;
  int wizard_cursor_ = 0;
  std::vector<std::pair<std::unique_ptr<GameMenuTextures>, int64_t>> retired_;

  Options running_;  // con lo que arranco el proceso
  Options edit_;     // lo elegido en la pagina (ya guardado en el toml)
  std::array<int, 5> cursor_{};
  int last_page_ = 0;
  uint16_t repeat_held_ = 0;
  int64_t repeat_at_ms_ = 0;
  int64_t restart_armed_until_ms_ = 0;
  std::string status_;
  int64_t status_until_ms_ = 0;
};

}  // namespace lo
