// lostodyssey - ReXGlue Recompiled Project
//
// App del proyecto: añade el menú de opciones propio (tecla F2, ver
// lo_options.h) al overlay imgui del SDK.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/keybinds.h>

#include "crash_handler.h"
#include "crash_reporter.h"
#include "guest_guards.h"
#include "installer.h"
#include "lo_update.h"

#ifdef LO_DEV
#include "disc_texture_dump.h"
#include "heap_watch.h"
#endif
#include "lo_options.h"
#include "lo_paths.h"
#include "lo_version.h"
#include "multidisc.h"
#include "save_headers.h"
#include "settings_page.h"
#include "shader_prewarm.h"
#include "texture_download.h"
#include "texture_pack_secret.h"
#include "title_logo.h"
#include "turbo.h"

REXCVAR_DECLARE(std::string, user_data_root);
REXCVAR_DECLARE(bool, lo_setup);

class LostodysseyApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<LostodysseyApp>(new LostodysseyApp(ctx, "lostodyssey",
        PPCImageConfig));
  }

  // Estructura de distribución (sin .bat ni flags): junto al exe viven
  //   config\config.toml - toda la configuración (la escribe el menú F2 y la
  //                  carga el SDK antes de inicializar los cvars)
  //   data\        - los discos del juego: data\disc1 ... data\disc4 (carpetas
  //                  extraidas). Tambien valen .iso o paquetes GOD dentro, y
  //                  otras rutas con lo_discs (ver multidisc_hooks.cpp).
  //   saves\       - partidas (saves\saveNN) y, en saves\perfil, el perfil del
  //                  jugador (XUID) y los logros. Ver lo_paths.h.
  //   cache\       - caché de shaders (regenerable).
  // Se ejecuta antes de que el SDK cargue el toml, así que basta con
  // cambiar las rutas aquí.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    // Relanzado desde el menu: esperar a que el proceso anterior suelte sus ficheros.
    lo::WaitForRestartParent();
    std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
    lo::MigrateLegacyLayout();
    paths.config_path = lo::ConfigFile();
    config_path_ = paths.config_path;
    // Un --game_data_root explicito (carpeta, .iso o GOD) manda sobre data\.
    std::filesystem::path data_dir = exe_dir / "data";
    if (paths.game_data_root.empty() && std::filesystem::is_directory(data_dir)) {
      paths.game_data_root = data_dir;
    }
    std::error_code ec;
    // Un --user_data_root explicito manda (util para probar con partidas de
    // otra persona sin tocar las tuyas).
    if (REXCVAR_GET(user_data_root).empty()) {
      paths.user_data_root = lo::ProfileDir();
      // Partidas propias en saves/saveNN/save.bin (se activa en OnPreSetup, ya con
      // los cvars cargados). Con --user_data_root (partidas de prueba) se deja la
      // estructura de la consola.
      flat_saves_ = true;
    }
    std::filesystem::create_directories(paths.user_data_root, ec);
    paths.cache_root = exe_dir / "cache";
    std::filesystem::create_directories(paths.cache_root, ec);

    // Capturador de cierres: deja logs\crash_<fecha>.txt con la funcion del
    // juego en la que se murio (traducida con el .map del enlazador).
    lo::CrashHandlerInit(exe_dir / "logs", exe_dir / "lostodyssey.map");
    // Detector de cuelgues: logs\cuelgue_<fecha>.txt si el juego deja de dibujar.
    lo::HangWatchInit();
  }

  // El SDK carga el plugin GPU con backend "any", y "any" en Windows elige
  // siempre D3D12 porque su bloque va primero en plugin_main.cpp. Cargándolo
  // aquí a mano podemos elegir desde el menú F2: SetupPresentation respeta
  // config.graphics si ya viene puesto. Si el backend pedido no está compilado
  // en el plugin, se vuelve a D3D12 en vez de abortar el arranque.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    lo::ApplyStartupDefaults(config_path_);
    // Instalacion nueva (sin config.toml): el plugin grafico propio es el unico que se usa.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "odisea";
    }
    const char* backend = lo::GpuBackendName();
    config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, backend);
    if (!config.graphics && std::string_view(backend) != "d3d12") {
      REXLOG_ERROR("lostodyssey: el plugin '{}' no trae backend '{}'; se usa d3d12",
                   config.gpu_plugin, backend);
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, "d3d12");
    }
#ifndef LO_DEV
    // Version publica: el pack HD y la interfaz propia (logo, fuentes, botones) siempre activos, aunque la
    // instalacion sea nueva y el toml no diga nada (el plugin los trae apagados por defecto).
    rex::cvar::SetFlagByName("odisea_texture_pack", "true");
    rex::cvar::SetFlagByName("odisea_dump_textures", "false");
#endif
    lo::UpdateCleanupLeftovers();
    lo::UpdateCheckStart();
    lo::ApplyStartupDefaultsAfterPlugin();
    lo::ShaderPrewarmRegisterEssentials();
    lo::RegisterTexturePackSecret();
    // El juego crea decenas de hilos de trabajo por segundo (animacion, skinning) y
    // cuenta con sus prioridades y afinidades; ignorarlas (valor por defecto del
    // runtime) deja enemigos congelados y vertices disparados de forma aleatoria
    // (1-oct-2026). Se fijan aqui para no depender del config.toml.
    rex::cvar::SetFlagByName("ignore_thread_priorities", "false");
    rex::cvar::SetFlagByName("ignore_thread_affinities", "false");
    // Logo de la pantalla de titulo: el plugin vigila la textura del titulo.
    lo::TitleLogoWatch();
    if (flat_saves_) {
      const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
      lo::SetupFlatSaves(lo::SavesDir(), lo::ProfileDir());
    }
  }

  // Discos: carpeta extraida, ISO o GOD, leidos en su sitio. Se resuelven con
  // el toml ya cargado y la ventana abierta (por si hay que preguntar), antes
  // de construir el runtime. Si no hay disco 1, se pide con un selector.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    // Antes de construir el runtime: la memoria del guest lee protect_zero al
    // crearse (ver guest_guards.cpp).
    lo::ApplyZeroPageCompat(config_path_);
    // Si la ejecucion anterior termino en un cierre, se ofrece avisar (informe de GitHub).
    lo::CrashReporterOffer(rex::filesystem::GetExecutableFolder() / "logs");
    // Version nueva firmada: si el jugador acepta, el actualizador ya esta lanzado y se cierra sin arrancar nada.
    if (lo::UpdateOfferAndApply(config_path_)) {
      if (window()) window()->RequestClose();
      return std::nullopt;
    }
    rex::PathConfig paths = defaults;
    if (!REXCVAR_GET(lo_setup)) {
      if (auto root = lo::MultiDiscResolveBoot(paths.game_data_root, paths.cache_root)) {
        paths.game_data_root = *root;
        return paths;
      }
    }
    // Sin disco 1 (o con --lo_setup): asistente de instalacion dentro de la propia ventana (como reblue / Unleashed).
    if (settings_page_) {
      installer_ = std::make_shared<lo::Installer>(
          config_path_,
          // Jugar: se resuelve el disco de arranque y se reanuda el arranque del SDK.
          [this, paths, resume]() mutable {
            app_context().CallInUIThreadDeferred([this, paths, resume]() mutable {
              // Ajustes del asistente que solo se aplican reiniciando (idioma, resolucion...): una vez, ya con los
              // discos en su sitio, asi el primer arranque va con todo lo elegido. La descarga del pack (si se
              // eligio) queda marcada con un .part y sigue en el nuevo proceso.
              if (settings_page_ && settings_page_->WizardRestartNeeded()) {
                lo::TextureDownloadCancel();
                settings_page_->EndInstaller();
                if (lo::SpawnRestart()) {
                  if (window()) window()->RequestClose();
                  return;
                }
              }
              if (settings_page_) settings_page_->EndInstaller();
              auto root = lo::MultiDiscResolveBoot(paths.game_data_root, paths.cache_root);
              if (!root) {
                if (window()) window()->RequestClose();
                return;
              }
              paths.game_data_root = *root;
              resume(paths);
            });
          },
          // Disco 1 verificado: arranque provisional para leer del disco los recursos del menu y dar al
          // asistente el estilo del juego.
          [paths] { lo::MultiDiscResolveBoot(paths.game_data_root, paths.cache_root); });
      settings_page_->StartInstaller(installer_);
      return std::nullopt;
    }
    for (;;) {
      const auto picked = lo::MultiDiscPickSource();
      if (!picked) return paths;  // cancelado: el SDK avisa de que faltan los datos
      lo::MultiDiscRememberPath(config_path_, *picked);
      if (auto root = lo::MultiDiscResolveBoot(paths.game_data_root, paths.cache_root)) {
        paths.game_data_root = *root;
        return paths;
      }
    }
  }

  // Runtime construido y ejecutable cargado: montar el disco si es ISO/GOD.
  void OnPostSetup() override {
    ApplyBranding();
    lo::TextureDownloadResumeIfPartial();
    lo::MultiDiscAttach(runtime() ? runtime()->file_system() : nullptr);
    // Partidas traidas de otro emulador: reconstruir los .header que falten.
    lo::RepairSaveHeaders(user_data_root());
  }

  // El SDK titula la ventana "<nombre> <build del SDK>": aqui es el juego, con su nombre.
  void ApplyBranding() {
    if (window()) window()->SetTitle("Lost Odyssey HD Remaster");
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    ApplyBranding();
#ifndef LO_DEV
    // Version publica: sin los overlays de depuracion del SDK (F3 estadisticas,
    // F4 ajustes crudos, ` consola). Los ajustes de jugador estan en F2 y en el
    // menu del juego; el overlay de logros (F7) se queda.
    rex::ui::UnregisterBind("bind_debug_overlay");
    rex::ui::UnregisterBind("bind_console");
    rex::ui::UnregisterBind("bind_settings");
#endif
    // Pagina "Opciones del port" dentro de la Configuracion del juego: siempre
    // registrada, solo dibuja cuando esta abierta (LB/RB en esa pantalla). Usa
    // la fuente y las texturas del menu del juego, leidas de sus datos.
    settings_page_ = std::make_unique<lo::SettingsPageDialog>(
        drawer, immediate_drawer(), config_path_,
        // Reinicio: el nuevo proceso ya esta lanzado; cerrar esta ventana.
        [this] {
          app_context().CallInUIThreadDeferred([this] {
            if (window()) window()->RequestClose();
          });
        });
    rex::ui::RegisterBind("bind_lo_turbo", "F6", "Turbo (velocidad x2)", [] { lo::TurboToggle(); });
#ifdef LO_DEV
    rex::ui::RegisterBind("bind_lo_texture_reload", "F7", "Recargar el pack de texturas",
                          [] { lo::ReloadTexturePack(); });
#endif
    rex::ui::RegisterBind("bind_lo_options", "F2", "Opciones de Lost Odyssey", [this, drawer] {
      if (options_dialog_) {
        options_dialog_.reset();
      } else {
        options_dialog_ = std::make_unique<lo::OptionsDialog>(
            drawer, config_path_,
            // Ocultar: destruir el diálogo fuera de su propio OnDraw.
            [this] {
              app_context().CallInUIThreadDeferred([this] { options_dialog_.reset(); });
            },
            // Reinicio: el nuevo proceso ya está lanzado; cerrar esta ventana
            // (el SDK termina el título y sale del proceso).
            [this] {
              app_context().CallInUIThreadDeferred([this] {
                if (window()) window()->RequestClose();
              });
            });
      }
    });
  }

  // Modulo cargado e hilo principal creado (aun sin ejecutar).
  void OnPostLaunchModule(rex::system::XThread* thread) override {
    (void)thread;
    lo::MultiDiscApplyStartDisc();
#ifdef LO_DEV
    lo::HeapWatchStart();
    lo::DiscTextureDumpAutoStart();
#endif
    lo::LogZeroPageState();
    lo::ReserveXactBankSink();
    lo::ShaderPrewarmInstall();
  }

  void OnShutdown() override {
    rex::ui::UnregisterBind("bind_lo_options");
    rex::ui::UnregisterBind("bind_lo_turbo");
#ifdef LO_DEV
    rex::ui::UnregisterBind("bind_lo_texture_reload");
#endif
    options_dialog_.reset();
    settings_page_.reset();
  }

 private:
  bool flat_saves_ = false;
  std::filesystem::path config_path_;
  std::unique_ptr<lo::OptionsDialog> options_dialog_;
  std::unique_ptr<lo::SettingsPageDialog> settings_page_;
  std::shared_ptr<lo::Installer> installer_;
};
