// lostodyssey - ReXGlue Recompiled Project
//
// Menú de opciones propio (tecla F2): resolución, antialiasing y parches.
// Escribe lostodyssey.toml (el fichero que el SDK carga al arrancar) y, si
// algún cambio lo requiere, relanza el proceso con la misma línea de
// comandos. Pensado para integrarse más adelante en los menús del juego.
#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

#include "lo_i18n.h"

namespace lo {

// Escala 3D (antes "preset de resolución"). El juego sigue en su modo original
// (1280x720: interfaz, textos y callouts exactos) y el plugin dibuja la escena a
// esta escala del 720p original: x1, x1,5, x2, x3... hasta x7, el maximo del SDK
// (TextureCache::kMaxDrawResolutionScaleAlongAxis). x1,5 usa la escala
// fraccionaria del fork (odisea_render_scale_q = 6 cuartos). Desde el
// 6-oct-2026 la interfaz no sigue esta escala sino la resolucion de salida
// (OutputRes), y antes habia escalas de 1,25 / 2,25 / 2,5 (900p, 1620p, 1800p).
// Desde el 7-oct-2026 la escala va de cuarto en cuarto (x1, x1,25, x1,5... x7) y el menu la
// ensena tambien en % de la resolucion de salida; el supermuestreo (SSAA) era lo mismo que subirla
// y se ha quitado (lo_ssaa > 1 de configuraciones viejas se pasa a la escala al leerla).
inline constexpr int kScaleMinQuarters = 4;   // x1 (720p)
inline constexpr int kScaleMaxQuarters = 28;  // x7 (8960x5040)
// "x2,5" (coma o punto segun el idioma del juego).
std::string ScaleLabel(int quarters);

// Backend gráfico del plugin GPU. El plugin lleva los dos compilados; el SDK
// pediría "any", que en Windows elige siempre D3D12 por orden de los #if, así
// que la app lo carga a mano (ver LostodysseyApp::OnPreSetup).
enum class GpuBackend : int { kD3D12 = 0, kVulkan = 1 };

// Relación de aspecto (parches de Xenia de boma, aspect_ratio_hooks.cpp): 16:9
// original, 16:10 (Steam Deck: backbuffer 1152x720) o 21:9 (1280x548). Ya no se
// elige en el menú: sale de la resolución de salida. Pide reiniciar.
enum class AspectRatio : int { k16x9 = 0, k16x10 = 1, k21x9 = 2 };
inline constexpr int kAspectCount = 3;

// Resolución de salida: tamaño de la imagen que se presenta y de la ventana, y
// resolución a la que se dibuja la interfaz (odisea_hud_resolution; el 3D va a
// la Escala 3D y se reescala a esta). Steam Deck = 1280x800 (16:10);
// ultrapanoramicas 2560x1080 (64:27) y 3440x1440 (43:18). Fuera de 16:9 el 3D
// ocupa toda la pantalla y la interfaz va en un rectangulo 16:9 centrado.
enum class OutputRes : int {
  k720p = 0, k900p = 1, k1080p = 2, k1440p = 3, k1620p = 4, k1800p = 5, k4K = 6, kSteamDeck = 7,
  kUltra1080 = 8, kUltra1440 = 9
};
inline constexpr int kOutputResCount = 10;

// Modos de DLSS (cvar lo_dlss_mode). Factor = resolución de salida / resolución
// del 3D que recomienda NVIDIA.
enum DlssMode : int {
  kDlssOff = 0, kDlssDlaa = 1, kDlssQuality = 2, kDlssBalanced = 3, kDlssPerformance = 4,
  kDlssUltra = 5
};
inline constexpr int kDlssModeCount = 6;
float DlssModeFactor(int mode);
inline constexpr int kSharpnessCount = 4;
extern const float kSharpnessLevels[kSharpnessCount];
// Auto (el de NVIDIA para cada modo), J, K, L, M: valores de odisea_dlss_preset.
inline constexpr int kDlssModelCount = 5;
extern const int kDlssModelPresets[kDlssModelCount];

struct Options {
  // Escala 3D en cuartos del 720p (6 = x1,5). Con DLSS manda el modo (total_quarters) y al
  // elegir un modo se copia aqui su escala.
  int scale_q = 6;
  OutputRes output = OutputRes::k1080p;
  GpuBackend gpu_backend = GpuBackend::kD3D12;
  int fxaa = 0;         // 0 none, 1 fxaa, 2 fxaa_extreme, 3 smaa (cvar odisea_smaa)
  int present = 0;      // 0 bilinear, 1 cas, 2 fsr
  // NVIDIA DLSS para el 3D (cvar odisea_dlss del plugin; RTX, D3D12 y Vulkan).
  // Con un modo elegido, la escala 3D sale de él y de la resolución de salida
  // (DlssModeFactor), nunca por debajo de x1 (720p). Pide reiniciar.
  int dlss_mode = 0;  // DlssMode
  // Nitidez del 3D (enfocado tipo CAS tras DLSS, cvar odisea_dlss_sharpness),
  // índice en kSharpnessLevels. En caliente.
  int sharpness = 0;
  // Modelo (red) de DLSS, índice en kDlssModelPresets (cvar odisea_dlss_preset).
  // No sale en el menú (el usuario lo quiere automático, 7-oct-2026): se queda
  // en 0 = el que NVIDIA elige para cada modo.
  int dlss_model = 0;
  bool fullscreen = true;
  bool vsync = true;
  // Parches (Xenia Canary) — cvars lo_* del exe, cambiables en caliente.
  bool fps60 = true;
  bool flicker_fix = false;  // animar por CPU: solo en la version de depuracion
  bool disable_dof = false;
  bool disable_motion_blur = false;
  bool disable_dynamic_shadows = false;
  // Fundido entre escenas (cvar odisea_skip_crossfade del plugin, al reves). En caliente.
  bool crossfade = true;
  // Turbo (en caliente).
  bool turbo_enabled = true;
  int turbo_scalar = 1;   // índice: 0 = 1.5x, 1 = 2x, 2 = 3x
  int turbo_button = 0;   // índice en kTurboButtons
  bool turbo_hold = false;
  // Calidad de vida (en caliente).
  bool save_anywhere = false;
  bool show_fps = false;
  // Trucos (en caliente).
  bool no_random_battles = false;
  // Precarga de sombreadores (cvar lo_shader_precache): 0 = todos los discos, 1 = la parte actual
  // (las siguientes al llegar); -1 = sin elegir (se pregunta la primera vez, shader_prewarm.cpp,
  // y el menu no lo escribe hasta que se toca). En caliente.
  int shader_precache = -1;
  // Idioma del juego (user_language); pide reiniciar.
  Lang language = Lang::kSpanish;
  // Texturas: volcado a PNG y pack de sustituciones (cvars del plugin GPU).
  bool dump_textures = false;
  bool texture_pack = false;

  // Relación de aspecto que sale de la resolución de salida.
  AspectRatio aspect() const;
  int output_w() const;
  int output_h() const;
  // Relación exacta ancho:alto de la salida para lo_aspect_ratio y
  // odisea_display_aspect ("16:9", "16:10", "64:27", "43:18").
  const char* aspect_key() const;

  // Valores derivados de la escala.
  int master_w() const;
  int master_h() const;
  // Tamaño del backbuffer que crea el juego con la relación de aspecto elegida.
  int backbuffer_w() const;
  int backbuffer_h() const;
  int base_scale() const;
  int render_scale() const;  // = base_scale
  // Escala de render en cuartos: la Escala 3D o, con DLSS, la que pone el modo.
  int total_quarters() const;
  // Cuartos que llenan la resolucion de salida (100 %) y % de una escala sobre ella.
  float fill_quarters() const;
  int ScalePercent(int quarters) const;
  // Escala que pone un modo de DLSS a esta resolucion de salida (nunca menos de x1).
  int DlssQuarters(int mode) const;
  // El modo da una escala distinta de la de los modos mejores; si no (a 1080p Equilibrado,
  // Rendimiento y Ultra caen a 720p, como Calidad), no se ofrece.
  bool DlssModeUseful(int mode) const;
  // Tras cambiar la resolucion: un modo que ya no se ofrece pasa al mejor con su misma escala.
  void NormalizeDlss();
  // Valor de odisea_render_scale_q: total_quarters() si no es multiplo de 4
  // (escala fraccionaria); 0 si la escala es entera (ruta normal de Xenia).
  int render_scale_quarters() const;
  int window_w() const;
  int window_h() const;
  // Cambios que solo se aplican tras reiniciar.
  bool NeedsRestartFrom(const Options& running) const;
};

// Lee las opciones de los cvars actuales (lo que el proceso arrancó).
Options ReadFromCvars();
// Escribe las opciones en el toml (conserva las claves que no gestiona).
bool WriteToml(const std::filesystem::path& config_path, const Options& o);
// Escribe una sola clave en el toml (conserva el resto). value va ya en sintaxis TOML.
bool SetTomlValue(const std::filesystem::path& config_path, const std::string& key,
                  const std::string& value);
// Al empezar (OnPreSetup, antes de crear nada): fija las constantes del proyecto en los cvars y, si no hay ajustes
// guardados (instalacion nueva), escribe y aplica las opciones por defecto con el idioma de Windows. Sin esto
// la primera ejecucion usaba los valores del SDK (EDRAM por RTV, sin heap): ~16 fps.
void ApplyStartupDefaults(const std::filesystem::path& config_path);
// Tras cargar el plugin grafico (sus cvars ya existen): repite lo anterior para ellos.
void ApplyStartupDefaultsAfterPlugin();
// Aplica en caliente las opciones que lo permiten (parches lo_*).
void ApplyHot(const Options& o);
// Relanza el proceso con la misma línea de comandos. Devuelve false si falla.
bool SpawnRestart();
// Lo llama el proceso relanzado nada mas arrancar (antes de abrir ningun fichero): espera a que
// el proceso que lo relanzo haya terminado (como mucho 20 s). Sin esto, el nuevo podia abrir la
// cache de pipelines mientras el viejo aun la tenia abierta, y la desactivaba (pantalla negra y
// todo apareciendo poco a poco al cambiar la escala 3D).
void WaitForRestartParent();
// Backend gráfico elegido en el toml ("d3d12" o "vulkan"), para pasárselo a
// LoadGpuPlugin al arrancar. Solo se lee en el arranque: cambiarlo en el menú
// pide reinicio.
const char* GpuBackendName();
// Pide al plugin GPU releer la carpeta del pack y rehacer las texturas ya
// cargadas, sin reiniciar el juego (F7 o el botón del menú).
void ReloadTexturePack();

class OptionsDialog : public rex::ui::ImGuiDialog {
 public:
  // request_hide: la app destruye el diálogo (diferido, fuera de OnDraw);
  // request_close_window: cierra la ventana del juego (tras lanzar el reinicio).
  OptionsDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                std::function<void()> request_hide, std::function<void()> request_close_window);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::filesystem::path config_path_;
  std::function<void()> request_hide_;
  std::function<void()> request_close_window_;
  Options running_;   // con lo que arrancó el proceso
  Options edit_;      // lo que el usuario está editando
  std::string status_;
  bool pending_restart_ = false;
  bool ask_restart_ = false;
};

}  // namespace lo
