// lostodyssey - ReXGlue Recompiled Project
//
// Menú de opciones (F2). Ver lo_options.h.
#include "lo_options.h"
#ifdef LO_DEV
#include "disc_texture_dump.h"
#endif
#include "turbo.h"

#include <cwchar>
#include <iterator>
#include <string>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Cvars propios que guardan la elección de alto nivel; de ellos se derivan los
// cvars de bajo nivel al escribir el toml.
REXCVAR_DEFINE_STRING(lo_video_preset, "1080p", "LostOdyssey/Video",
                      "Escala 3D: x1 a x7 de 0,25 en 0,25 (x1.25, x1.5, x2.5...).");
REXCVAR_DEFINE_INT32(lo_ssaa, 1, "LostOdyssey/Video",
                     "Supersampling adicional (1 = off, 2 = 2x, 3 = 3x)");
REXCVAR_DEFINE_STRING(lo_gpu_backend, "d3d12", "LostOdyssey/Video",
                      "Backend gráfico del plugin GPU: d3d12 o vulkan");
REXCVAR_DEFINE_STRING(lo_dlss_mode, "", "LostOdyssey/Video",
                      "Modo de DLSS: off, dlaa, quality, balanced, performance o ultra. Con un "
                      "modo, la escala 3D sale de él y de la resolución de salida. Vacío = el de "
                      "antes (odisea_dlss con la escala 3D elegida a mano).");
REXCVAR_DEFINE_STRING(lo_output_resolution, "", "LostOdyssey/Video",
                      "Resolución de salida (imagen e interfaz): 720p, 900p, 1080p, 1440p, 1620p, "
                      "1800p, 4k o steamdeck. Vacío = la de lo_video_preset (configuraciones "
                      "anteriores al 6-oct-2026).");

REXCVAR_DECLARE(std::string, lo_aspect_ratio);
REXCVAR_DECLARE(bool, lo_60fps);
REXCVAR_DECLARE(bool, lo_flicker_fix);
REXCVAR_DECLARE(bool, lo_postfx_upscale_fix);
REXCVAR_DECLARE(bool, lo_disable_dof);
REXCVAR_DECLARE(bool, lo_disable_motion_blur);
REXCVAR_DECLARE(bool, lo_disable_dynamic_shadows);
REXCVAR_DECLARE(std::string, lo_shader_precache);
REXCVAR_DECLARE(bool, fullscreen);
REXCVAR_DECLARE(bool, lo_turbo_enabled);
REXCVAR_DECLARE(double, lo_turbo_scalar);
REXCVAR_DECLARE(int32_t, lo_turbo_button);
REXCVAR_DECLARE(bool, lo_turbo_hold);
REXCVAR_DECLARE(bool, lo_save_anywhere);
REXCVAR_DECLARE(bool, lo_show_fps);
REXCVAR_DECLARE(bool, lo_no_random_battles);
// vsync y swap_post_effect viven en la DLL del plugin GPU: no se enlazan,
// se consultan por nombre.
REXCVAR_DECLARE(std::string, present_effect);

namespace lo {

namespace {

const char* kDlssModeKeys[] = {"off", "dlaa", "quality", "balanced", "performance", "ultra"};
const char* kDlssModeLabels[] = {"No",          "DLAA",        "Calidad",
                                 "Equilibrado", "Rendimiento", "Ultra rendimiento"};
const char* kSharpnessLabels[] = {"No", "Baja", "Media", "Alta"};
// Claves de lo_video_preset de antes del 6-oct-2026 (resoluciones) -> escala.
const char* kLegacyPresetKeys[] = {"720p", "900p", "1080p", "1440p", "1620p", "1800p", "4k"};
const int kLegacyPresetQuarters[] = {4, 6, 6, 8, 8, 12, 12};
const char* kOutputKeys[] = {"720p",  "900p", "1080p",     "1440p",  "1620p",
                             "1800p", "4k",   "steamdeck", "uw1080", "uw1440"};
const char* kOutputLabels[] = {"720p (1280x720)",
                               "900p (1600x900)",
                               "1080p (1920x1080)",
                               "1440p (2560x1440)",
                               "1620p (2880x1620)",
                               "1800p (3200x1800)",
                               "4K (3840x2160)",
                               "Steam Deck (1280x800, 16:10)",
                               "Ultrapanorámica 2560x1080 (21:9)",
                               "Ultrapanorámica 3440x1440 (21:9)"};
const int kOutputSizes[][2] = {{1280, 720},  {1600, 900},  {1920, 1080}, {2560, 1440},
                               {2880, 1620}, {3200, 1800}, {3840, 2160}, {1280, 800},
                               {2560, 1080}, {3440, 1440}};
const char* kOutputAspectKeys[] = {"16:9", "16:9", "16:9",  "16:9",  "16:9",
                                   "16:9", "16:9", "16:10", "64:27", "43:18"};
static_assert(sizeof(kOutputSizes) / sizeof(kOutputSizes[0]) == kOutputResCount);
const char* kFxaaKeys[] = {"none", "fxaa", "fxaa_extreme"};
// kFxaaKeys son los valores de swap_post_effect; la cuarta opcion (SMAA) no
// existe en ese cvar del SDK: es odisea_smaa, del plugin.
constexpr int kAaSmaa = 3;
const char* kFxaaLabels[] = {"Desactivado", "FXAA", "FXAA extremo", "SMAA 1x"};
const char* kPresentKeys[] = {"bilinear", "cas", "fsr"};
const char* kPresentLabels[] = {"Bilineal", "CAS (nitidez AMD)", "FSR 1.0 (AMD)"};
const char* kGpuBackendKeys[] = {"d3d12", "vulkan"};
const char* kGpuBackendLabels[] = {"Direct3D 12", "Vulkan (experimental)"};
const double kTurboScalars[] = {1.5, 2.0, 3.0, 4.0, 6.0, 8.0};
const char* kTurboScalarLabels[] = {"1.5x", "2x", "3x", "4x", "6x", "8x"};
constexpr int kTurboScalarCount = 6;
const int kTurboButtons[] = {0x0040, 0x0080, 0x0020, 0x0100, 0x0200};
const char* kTurboButtonLabels[] = {"L3 (pulsar stick izquierdo)", "R3 (pulsar stick derecho)",
                                    "Back / Select", "LB", "RB"};

int IndexOf(const char* const* keys, int n, const std::string& v, int fallback) {
  for (int i = 0; i < n; ++i) {
    if (v == keys[i]) return i;
  }
  return fallback;
}

std::string Bool(bool b) { return b ? "true" : "false"; }

// Clave de lo_video_preset: "x1", "x1.25", "x1.5"... (punto, como en el toml de siempre).
std::string ScaleKey(int q) {
  q = std::clamp(q, kScaleMinQuarters, kScaleMaxQuarters);
  std::string key = "x" + std::to_string(q / 4);
  static const char* kFrac[] = {"", ".25", ".5", ".75"};
  return key + kFrac[q % 4];
}
// "x2.5" -> 10; -1 si no es una escala.
int ParseScaleKey(const std::string& key) {
  if (key.size() < 2 || key[0] != 'x') return -1;
  char* end = nullptr;
  const double v = std::strtod(key.c_str() + 1, &end);
  if (!end || *end || !(v > 0.0)) return -1;
  return std::clamp(int(std::lround(v * 4.0)), kScaleMinQuarters, kScaleMaxQuarters);
}

}  // namespace

const float kSharpnessLevels[kSharpnessCount] = {0.0f, 0.3f, 0.6f, 1.0f};
const int kDlssModelPresets[kDlssModelCount] = {0, 10, 11, 12, 13};

float DlssModeFactor(int mode) {
  static const float kFactors[kDlssModeCount] = {1.0f, 1.0f, 1.5f, 1.724f, 2.0f, 3.0f};
  return kFactors[std::clamp(mode, 0, kDlssModeCount - 1)];
}

std::string ScaleLabel(int q) {
  q = std::clamp(q, kScaleMinQuarters, kScaleMaxQuarters);
  const Lang lang = GameLanguage();
  const char sep = lang == Lang::kEnglish || lang == Lang::kJapanese ? '.' : ',';
  std::string s = "x" + std::to_string(q / 4);
  static const char* kFrac[] = {"", "25", "5", "75"};
  if (q % 4) s += sep + std::string(kFrac[q % 4]);
  return s;
}

AspectRatio Options::aspect() const {
  if (output == OutputRes::kSteamDeck) return AspectRatio::k16x10;
  if (output == OutputRes::kUltra1080 || output == OutputRes::kUltra1440) return AspectRatio::k21x9;
  return AspectRatio::k16x9;
}
const char* Options::aspect_key() const {
  return kOutputAspectKeys[std::clamp(int(output), 0, kOutputResCount - 1)];
}
int Options::output_w() const { return kOutputSizes[std::clamp(int(output), 0, kOutputResCount - 1)][0]; }
int Options::output_h() const { return kOutputSizes[std::clamp(int(output), 0, kOutputResCount - 1)][1]; }

int Options::master_w() const { return 1280; }
int Options::master_h() const { return 720; }
// Igual que aspect_ratio_hooks.cpp: mas estrecho = 720 de alto, mas ancho = 1280.
int Options::backbuffer_w() const {
  const double a = double(output_w()) / double(output_h());
  return a < 16.0 / 9.0 - 0.01 ? int(std::lround(720.0 * a / 16.0)) * 16 : 1280;
}
int Options::backbuffer_h() const {
  const double a = double(output_w()) / double(output_h());
  return a > 16.0 / 9.0 + 0.01 ? int(std::lround(1280.0 / a / 4.0)) * 4 : 720;
}
// Factor de almacenamiento del plugin (draw_resolution_scale_*): el entero por
// encima de la escala real; con escala fraccionaria el render solo usa la parte
// proporcional de cada textura de destino.
int Options::base_scale() const { return (total_quarters() + 3) / 4; }
int Options::total_quarters() const {
  if (dlss_mode != kDlssOff) return DlssQuarters(dlss_mode);
  return std::clamp(scale_q, kScaleMinQuarters, kScaleMaxQuarters);
}
// El eje que manda es el que llena la salida (con 21:9 / 16:10 el backbuffer cambia).
float Options::fill_quarters() const {
  return 4.0f * std::min(float(output_w()) / float(backbuffer_w()), float(output_h()) / float(backbuffer_h()));
}
int Options::ScalePercent(int q) const { return int(std::lround(100.0f * float(q) / fill_quarters())); }
int Options::DlssQuarters(int mode) const {
  return std::clamp(int(std::lround(fill_quarters() / DlssModeFactor(mode))), kScaleMinQuarters,
                    kScaleMaxQuarters);
}
bool Options::DlssModeUseful(int mode) const {
  if (mode <= kDlssDlaa) return true;
  for (int better = kDlssDlaa; better < mode; ++better) {
    if (DlssQuarters(better) == DlssQuarters(mode)) return false;
  }
  return true;
}
void Options::NormalizeDlss() {
  if (dlss_mode == kDlssOff || DlssModeUseful(dlss_mode)) return;
  for (int m = kDlssDlaa; m < dlss_mode; ++m) {
    if (DlssQuarters(m) == DlssQuarters(dlss_mode)) {
      dlss_mode = m;
      break;
    }
  }
}
int Options::render_scale() const { return (total_quarters() + 3) / 4; }
int Options::render_scale_quarters() const {
  const int q = total_quarters();
  return q % 4 ? q : 0;
}
// La ventana es la resolución de salida.
int Options::window_w() const { return output_w(); }
int Options::window_h() const { return output_h(); }

bool Options::NeedsRestartFrom(const Options& r) const {
  return total_quarters() != r.total_quarters() || fxaa != r.fxaa || present != r.present ||
         fullscreen != r.fullscreen || vsync != r.vsync || gpu_backend != r.gpu_backend ||
         output != r.output || language != r.language || dlss_mode != r.dlss_mode;
}

Options ReadFromCvars() {
  Options o;
  std::string preset = REXCVAR_GET(lo_video_preset);
  std::transform(preset.begin(), preset.end(), preset.begin(), ::tolower);
  // "1080p_x15" (clave de un dia) = el 1080p actual; "2160p" = 4k.
  if (preset == "1080p_x15") preset = "1080p";
  if (preset == "2160p") preset = "4k";
  const int legacy_preset = IndexOf(kLegacyPresetKeys, 7, preset, -1);
  {
    const int parsed = ParseScaleKey(preset);
    o.scale_q = legacy_preset >= 0 ? kLegacyPresetQuarters[legacy_preset] : parsed > 0 ? parsed : 6;
  }
  // Supermuestreo de antes del 7-oct-2026 (2x / 3x sobre la escala): es la misma escala mayor.
  o.scale_q = std::min(kScaleMaxQuarters, o.scale_q * std::clamp(REXCVAR_GET(lo_ssaa), 1, 3));
  std::string backend = REXCVAR_GET(lo_gpu_backend);
  std::transform(backend.begin(), backend.end(), backend.begin(), ::tolower);
  o.gpu_backend = GpuBackend(IndexOf(kGpuBackendKeys, 2, backend, int(GpuBackend::kD3D12)));
  {
    std::string output = REXCVAR_GET(lo_output_resolution);
    std::transform(output.begin(), output.end(), output.begin(), ::tolower);
    if (output.empty()) {
      // Configuraciones de antes de la resolución de salida: la ventana era la
      // del preset, y 16:10 era la Steam Deck.
      output = REXCVAR_GET(lo_aspect_ratio) == "16:10" ? "steamdeck"
               : legacy_preset >= 0                     ? std::string(kLegacyPresetKeys[legacy_preset])
                                                        : std::string("1080p");
    }
    o.output = OutputRes(IndexOf(kOutputKeys, kOutputResCount, output, int(OutputRes::k1080p)));
  }
  o.fxaa = IndexOf(kFxaaKeys, 3, rex::cvar::GetFlagByName("swap_post_effect"), 0);
  if (rex::cvar::GetFlagByName("odisea_smaa") == "true") {
    o.fxaa = kAaSmaa;
  }
  o.present = IndexOf(kPresentKeys, 3, REXCVAR_GET(present_effect), 0);
  {
    std::string mode = REXCVAR_GET(lo_dlss_mode);
    std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
    if (mode.empty()) {
      // Configuraciones de antes del 7-oct-2026: DLSS sí/no con la escala 3D
      // a mano; se coge el modo cuyo factor se parece más al de entonces.
      if (rex::cvar::GetFlagByName("odisea_dlss") == "true") {
        o.dlss_mode = kDlssDlaa;
        const float fill = std::min(float(o.output_w()) / float(o.backbuffer_w()),
                                    float(o.output_h()) / float(o.backbuffer_h()));
        const float factor = 4.0f * fill / float(o.scale_q);
        for (int m = kDlssDlaa; m < kDlssModeCount; ++m) {
          if (std::abs(DlssModeFactor(m) - factor) <
              std::abs(DlssModeFactor(o.dlss_mode) - factor)) {
            o.dlss_mode = m;
          }
        }
      }
    } else {
      o.dlss_mode = IndexOf(kDlssModeKeys, kDlssModeCount, mode, kDlssOff);
    }
    double sharpness = 0.0;
    try {
      sharpness = std::stod(rex::cvar::GetFlagByName("odisea_dlss_sharpness"));
    } catch (...) {
    }
    o.dlss_model = 0;  // siempre automático
    o.sharpness = 0;
    for (int i = 0; i < kSharpnessCount; ++i) {
      if (std::abs(kSharpnessLevels[i] - sharpness) <
          std::abs(kSharpnessLevels[o.sharpness] - sharpness)) {
        o.sharpness = i;
      }
    }
  }
  o.fullscreen = REXCVAR_GET(fullscreen);
  o.vsync = rex::cvar::GetFlagByName("vsync") == "true";
  o.fps60 = REXCVAR_GET(lo_60fps);
#ifdef LO_DEV
  o.flicker_fix = REXCVAR_GET(lo_flicker_fix);
#else
  o.flicker_fix = false;  // version publica: los personajes siempre se animan en la GPU
#endif
  o.disable_dof = REXCVAR_GET(lo_disable_dof);
  o.disable_motion_blur = REXCVAR_GET(lo_disable_motion_blur);
  o.disable_dynamic_shadows = REXCVAR_GET(lo_disable_dynamic_shadows);
  o.crossfade = rex::cvar::GetFlagByName("odisea_skip_crossfade") != "true";
  {
    const std::string& precache = REXCVAR_GET(lo_shader_precache);
    o.shader_precache = precache == "todo" ? 0 : precache == "parte" ? 1 : -1;
  }
  o.save_anywhere = REXCVAR_GET(lo_save_anywhere);
  o.show_fps = REXCVAR_GET(lo_show_fps);
  o.no_random_battles = REXCVAR_GET(lo_no_random_battles);
  o.turbo_enabled = REXCVAR_GET(lo_turbo_enabled);
  {
    const double s = REXCVAR_GET(lo_turbo_scalar);
    o.turbo_scalar = 1;  // el valor del toml puede no ser uno de la lista: se coge el mas cercano
    for (int i = 0; i < kTurboScalarCount; ++i) {
      if (std::abs(kTurboScalars[i] - s) < std::abs(kTurboScalars[o.turbo_scalar] - s)) {
        o.turbo_scalar = i;
      }
    }
    int b = REXCVAR_GET(lo_turbo_button);
    o.turbo_button = 0;
    for (int i = 0; i < 5; ++i) {
      if (kTurboButtons[i] == b) o.turbo_button = i;
    }
  }
  o.turbo_hold = REXCVAR_GET(lo_turbo_hold);
  o.language = GameLanguage();
#ifdef LO_DEV
  o.dump_textures = rex::cvar::GetFlagByName("odisea_dump_textures") == "true";
  o.texture_pack = rex::cvar::GetFlagByName("odisea_texture_pack") == "true";
#else
  o.dump_textures = false;  // version publica: el pack HD siempre activo, sin volcados
  o.texture_pack = true;
#endif
  return o;
}

namespace {

// Escribe claves en el toml conservando las lineas que no gestionamos
// (game_data_root, idioma, binds...). Los valores van ya en sintaxis TOML.
bool UpsertTomlKeys(const std::filesystem::path& config_path,
                    const std::vector<std::pair<std::string, std::string>>& kv) {
  std::vector<std::string> lines;
  {
    std::ifstream in(config_path);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  std::map<std::string, size_t> index;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& l = lines[i];
    size_t eq = l.find('=');
    if (eq == std::string::npos || l.empty() || l[0] == '#' || l[0] == '[') continue;
    std::string key = l.substr(0, eq);
    key.erase(key.find_last_not_of(" \t") + 1);
    key.erase(0, key.find_first_not_of(" \t"));
    index[key] = i;
  }
  for (const auto& [key, value] : kv) {
    std::string line = key + " = " + value;
    auto it = index.find(key);
    if (it != index.end()) {
      lines[it->second] = line;
    } else {
      lines.push_back(line);
      index[key] = lines.size() - 1;
    }
  }
  if (lines.empty() || lines[0].rfind("# ", 0) != 0) {
    lines.insert(lines.begin(), "# Configuracion de Lost Odyssey (menu F2 / rexglue cvars)");
  }
  std::ofstream out(config_path, std::ios::trunc);
  if (!out) {
    REXLOG_ERROR("lo_options: no se puede escribir {}", config_path.string());
    return false;
  }
  for (const auto& l : lines) out << l << '\n';
  REXLOG_INFO("lo_options: configuracion guardada en {}", config_path.string());
  return true;
}

}  // namespace

bool SetTomlValue(const std::filesystem::path& config_path, const std::string& key,
                  const std::string& value) {
  return UpsertTomlKeys(config_path, {{key, value}});
}

namespace {

// Claves gestionadas por el menú, en orden de escritura (valores ya en sintaxis TOML).
std::vector<std::pair<std::string, std::string>> BuildTomlKeys(const Options& o) {
  std::vector<std::pair<std::string, std::string>> kv = {
      {"lo_video_preset", "\"" + ScaleKey(o.total_quarters()) + "\""},
      {"lo_ssaa", "1"},  // ya no se usa: la escala lo lleva dentro
      {"lo_gpu_backend", "\"" + std::string(kGpuBackendKeys[int(o.gpu_backend)]) + "\""},
      // Vulkan: sin esto elige el camino de framebuffers de host, que emula la
      // EDRAM con copias entre render targets. Rompe las sombras dinamicas de
      // Lost Odyssey (van de EDRAM a textura y vuelven) y cuesta muchos fps.
      // "fsi" la emula exacta en un buffer via fragment shader interlock. El
      // cvar solo lo mira el backend Vulkan, asi que escribirlo siempre es
      // inocuo. Ojo: es kInitOnly, no se puede cambiar en caliente.
      {"render_target_path_vulkan", "\"fsi\""},
      // Resolución del juego (siempre 720p) y escala de render del plugin.
      {"video_mode_width", std::to_string(o.master_w())},
      {"video_mode_height", std::to_string(o.master_h())},
      {"draw_resolution_scale_x", std::to_string(o.render_scale())},
      {"draw_resolution_scale_y", std::to_string(o.render_scale())},
      // Escala fraccionaria del plugin (1080p x1,5); pisa draw_resolution_scale.
      {"odisea_render_scale_q", std::to_string(o.render_scale_quarters())},
      // Relación de aspecto (hooks del exe) y aspecto de presentación (plugin).
      {"lo_output_resolution", "\"" + std::string(kOutputKeys[int(o.output)]) + "\""},
      {"lo_aspect_ratio", "\"" + std::string(o.aspect_key()) + "\""},
      {"odisea_display_aspect",
       "\"" + std::string(o.aspect() == AspectRatio::k16x9 ? "" : o.aspect_key()) + "\""},
      // Interfaz a la resolución de salida, fuera de la EDRAM (plugin), en un
      // rectángulo 16:9 centrado si la salida tiene otro aspecto.
      {"odisea_hud_resolution", std::to_string(o.output_h())},
      {"odisea_hud_output_width", std::to_string(o.output_w())},
      {"window_width", std::to_string(o.window_w())},
      {"window_height", std::to_string(o.window_h())},
      {"fullscreen", Bool(o.fullscreen)},
      {"vsync", Bool(o.vsync)},
      // Antialiasing / presentación.
      {"swap_post_effect",
       "\"" + std::string(kFxaaKeys[o.fxaa == kAaSmaa ? 0 : o.fxaa]) + "\""},
      {"odisea_smaa", Bool(o.fxaa == kAaSmaa)},
      {"present_effect", "\"" + std::string(kPresentKeys[o.present]) + "\""},
      {"lo_dlss_mode", "\"" + std::string(kDlssModeKeys[o.dlss_mode]) + "\""},
      {"odisea_dlss", Bool(o.dlss_mode != kDlssOff)},
      {"odisea_dlss_sharpness", std::to_string(kSharpnessLevels[o.sharpness])},
      {"odisea_dlss_preset", std::to_string(kDlssModelPresets[o.dlss_model])},
      // Parches.
      {"lo_60fps", Bool(o.fps60)},
#ifdef LO_DEV
      {"lo_flicker_fix", Bool(o.flicker_fix)},
#else
      {"lo_flicker_fix", "false"},
#endif
      {"lo_postfx_upscale_fix", Bool(o.render_scale_quarters() != 4)},  // automatico por escala
      {"lo_disable_dof", Bool(o.disable_dof)},
      {"lo_disable_motion_blur", Bool(o.disable_motion_blur)},
      {"lo_disable_dynamic_shadows", Bool(o.disable_dynamic_shadows)},
      {"odisea_skip_crossfade", Bool(!o.crossfade)},
      // Turbo.
      {"lo_turbo_enabled", Bool(o.turbo_enabled)},
      {"lo_turbo_scalar", std::to_string(kTurboScalars[o.turbo_scalar])},
      {"lo_turbo_button", std::to_string(kTurboButtons[o.turbo_button])},
      {"lo_turbo_hold", Bool(o.turbo_hold)},
      // Calidad de vida.
      {"lo_save_anywhere", Bool(o.save_anywhere)},
      {"lo_show_fps", Bool(o.show_fps)},
      // Trucos.
      {"lo_no_random_battles", Bool(o.no_random_battles)},
      // Idioma del juego (XC_LANGUAGE del SDK).
      {"user_language", std::to_string(XLanguageOf(o.language))},
      // Texturas.
      {"odisea_dump_textures", Bool(o.dump_textures)},
      {"odisea_texture_pack", Bool(o.texture_pack)},
      // Constantes del proyecto (antes iban en los .bat).
      {"gpu_plugin", "\"odisea\""},
      {"render_target_path_d3d12", "\"rov\""},
      {"rexcrt_heap_enable", "true"},
      {"rexcrt_heap_size_mb", "512"},
      {"odisea_dump_draws_after_ms", "0"},
  };

  // Precarga de sombreadores: solo si ya se eligio (si no, se seguiria sin preguntar).
  if (o.shader_precache >= 0) {
    kv.emplace_back("lo_shader_precache", o.shader_precache == 0 ? "\"todo\"" : "\"parte\"");
  }
  return kv;
}

}  // namespace

bool WriteToml(const std::filesystem::path& config_path, const Options& o) {
  return UpsertTomlKeys(config_path, BuildTomlKeys(o));
}

// Idioma del juego por defecto en una instalacion nueva: el de Windows (ingles si no es uno de los seis).
static Lang SystemLanguage() {
#ifdef _WIN32
  wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
  if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) {
    const std::wstring n(name);
    if (n.rfind(L"es", 0) == 0) return Lang::kSpanish;
    if (n.rfind(L"fr", 0) == 0) return Lang::kFrench;
    if (n.rfind(L"de", 0) == 0) return Lang::kGerman;
    if (n.rfind(L"it", 0) == 0) return Lang::kItalian;
    if (n.rfind(L"ja", 0) == 0) return Lang::kJapanese;
  }
#endif
  return Lang::kEnglish;
}

namespace {
std::optional<Options> g_first_run;  // opciones por defecto aplicadas en esta ejecucion (instalacion nueva)

void ApplyConstants() {
  // Constantes del proyecto: siempre (el toml las lleva, pero el proceso que lo crea ya las habia leido; sin esto
  // la primera ejecucion iba con la EDRAM por RTV y sin el heap, a ~16 fps).
  rex::cvar::SetFlagByName("render_target_path_d3d12", "rov");
  rex::cvar::SetFlagByName("render_target_path_vulkan", "fsi");
  rex::cvar::SetFlagByName("rexcrt_heap_enable", "true");
  rex::cvar::SetFlagByName("rexcrt_heap_size_mb", "512");
#ifndef LO_DEV
  rex::cvar::SetFlagByName("lo_flicker_fix", "false");  // animar personajes por CPU rompe el juego: solo en depuracion
#endif
}

void ApplyKeys(const Options& o) {
  for (const auto& [key, value] : BuildTomlKeys(o)) {
    std::string v = value;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    rex::cvar::SetFlagByName(key, v);
  }
}
}  // namespace

void ApplyStartupDefaults(const std::filesystem::path& config_path) {
  ApplyConstants();
  // Instalacion nueva (sin ajustes guardados): las opciones por defecto, escritas y aplicadas YA.
  bool has_settings = false;
  {
    std::ifstream in(config_path);
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("lo_video_preset", 0) == 0) {
        has_settings = true;
        break;
      }
    }
  }
  if (has_settings) return;
  Options o;
  o.language = SystemLanguage();
  o.fps60 = true;
  WriteToml(config_path, o);
  ApplyKeys(o);
  g_first_run = o;
  REXLOG_INFO("lo_options: primera ejecucion, opciones por defecto aplicadas (idioma {})", int(o.language));
}

void ApplyStartupDefaultsAfterPlugin() {
  // Los cvars del plugin grafico no existen hasta cargarlo: se repite para ellos.
  ApplyConstants();
  if (g_first_run) ApplyKeys(*g_first_run);
}

void ApplyHot(const Options& o) {
  auto set = [](const char* name, bool v) { rex::cvar::SetFlagByName(name, v ? "true" : "false"); };
  set("lo_60fps", o.fps60);
#ifdef LO_DEV
  set("lo_flicker_fix", o.flicker_fix);
#endif
  set("lo_postfx_upscale_fix", o.render_scale_quarters() != 4);
  set("lo_disable_dof", o.disable_dof);
  set("lo_disable_motion_blur", o.disable_motion_blur);
  set("lo_disable_dynamic_shadows", o.disable_dynamic_shadows);
  set("odisea_skip_crossfade", !o.crossfade);
  set("lo_turbo_enabled", o.turbo_enabled);
  rex::cvar::SetFlagByName("lo_turbo_scalar", std::to_string(kTurboScalars[o.turbo_scalar]));
  rex::cvar::SetFlagByName("lo_turbo_button", std::to_string(kTurboButtons[o.turbo_button]));
  set("lo_turbo_hold", o.turbo_hold);
  set("lo_save_anywhere", o.save_anywhere);
  set("lo_show_fps", o.show_fps);
  set("lo_no_random_battles", o.no_random_battles);
  if (o.shader_precache >= 0) {
    rex::cvar::SetFlagByName("lo_shader_precache", o.shader_precache == 0 ? "todo" : "parte");
  }
  TurboRefresh();
  set("odisea_dump_textures", o.dump_textures);
  set("odisea_texture_pack", o.texture_pack);
  rex::cvar::SetFlagByName("lo_video_preset", ScaleKey(o.total_quarters()));
  rex::cvar::SetFlagByName("lo_ssaa", "1");
  rex::cvar::SetFlagByName("odisea_dlss_sharpness", std::to_string(kSharpnessLevels[o.sharpness]));
  rex::cvar::SetFlagByName("odisea_dlss_preset", std::to_string(kDlssModelPresets[o.dlss_model]));
}

const char* GpuBackendName() {
  // Se llama en OnPreSetup, antes de que exista ningún diálogo: se lee del cvar
  // que el SDK ya cargó del toml. Cualquier valor raro cae a d3d12.
  std::string v = REXCVAR_GET(lo_gpu_backend);
  std::transform(v.begin(), v.end(), v.begin(), ::tolower);
  return kGpuBackendKeys[IndexOf(kGpuBackendKeys, 2, v, int(GpuBackend::kD3D12))];
}

void ReloadTexturePack() {
  // El plugin GPU lo consume en su siguiente frame: vacía las cachés de
  // texturas y vuelve a leer los PNG de la carpeta del pack.
  rex::cvar::SetFlagByName("odisea_texture_pack_reload", "true");
}

#ifdef _WIN32
namespace {
constexpr const wchar_t* kRestartParentPidVariable = L"LO_RESTART_PARENT_PID";
}  // namespace
#endif

void WaitForRestartParent() {
#ifdef _WIN32
  wchar_t value[32];
  DWORD length = GetEnvironmentVariableW(kRestartParentPidVariable, value, DWORD(std::size(value)));
  if (!length || length >= std::size(value)) return;
  // Para que no lo herede un reinicio posterior.
  SetEnvironmentVariableW(kRestartParentPidVariable, nullptr);
  const DWORD parent_pid = DWORD(std::wcstoul(value, nullptr, 10));
  if (!parent_pid || parent_pid == GetCurrentProcessId()) return;
  if (HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid)) {
    WaitForSingleObject(parent, 20000);
    CloseHandle(parent);
  }
#endif
}

bool SpawnRestart() {
#ifdef _WIN32
  // Misma línea de comandos (el .bat solo pasa game_data_root); el resto
  // viene del toml recién escrito. El proceso actual sale justo después.
  std::wstring cmd = GetCommandLineW();
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  wchar_t exe_path[MAX_PATH];
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::filesystem::path cwd = std::filesystem::path(exe_path).parent_path();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  // El hijo hereda el entorno: con este PID espera a que salgamos antes de abrir nada
  // (WaitForRestartParent).
  SetEnvironmentVariableW(kRestartParentPidVariable,
                          std::to_wstring(GetCurrentProcessId()).c_str());
  BOOL ok = CreateProcessW(exe_path, buf.data(), nullptr, nullptr, FALSE,
                           CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, nullptr, cwd.c_str(), &si,
                           &pi);
  SetEnvironmentVariableW(kRestartParentPidVariable, nullptr);
  if (!ok) {
    REXLOG_ERROR("lo_options: CreateProcess fallo ({})", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  REXLOG_INFO("lo_options: reiniciando con la nueva configuracion");
  return true;
#else
  return false;
#endif
}

OptionsDialog::OptionsDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                             std::function<void()> request_hide,
                             std::function<void()> request_close_window)
    : rex::ui::ImGuiDialog(drawer),
      config_path_(std::move(config_path)),
      request_hide_(std::move(request_hide)),
      request_close_window_(std::move(request_close_window)) {
  running_ = ReadFromCvars();
  edit_ = running_;
}

void OptionsDialog::OnDraw(ImGuiIO& io) {
  (void)io;
  ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.92f);
  bool open = true;
  if (!ImGui::Begin("Opciones de Lost Odyssey (F2)", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) request_hide_();
    return;
  }

  ImGui::SeparatorText("Backend gráfico (requiere reiniciar)");
  int backend = int(edit_.gpu_backend);
  if (ImGui::Combo("API", &backend, kGpuBackendLabels, 2)) edit_.gpu_backend = GpuBackend(backend);
  ImGui::TextWrapped(
      "D3D12 es la ruta probada. Vulkan es la misma emulación del Xenos sobre otra API y es "
      "la que hace falta para compilar en Linux. Las dos traen todas las resoluciones, el "
      "volcado y el pack de texturas. Si el plugin no trae Vulkan compilado, el juego "
      "vuelve solo a D3D12.");

  ImGui::SeparatorText("Resolución");
  int output = int(edit_.output);
  if (ImGui::Combo("Resolución", &output, kOutputLabels, kOutputResCount)) {
    edit_.output = OutputRes(output);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Tamaño de la imagen y resolución de la interfaz (textos, menús,\n"
                      "mapa). Steam Deck (16:10) y las ultrapanorámicas (21:9) amplían\n"
                      "el campo de visión del 3D; la interfaz queda en 16:9 en el centro,\n"
                      "sin estirarse. Requiere reiniciar.");
  }
  {
    const std::string label = ScaleLabel(edit_.total_quarters()) + " (" +
                              std::to_string(edit_.ScalePercent(edit_.total_quarters())) + " %)";
    int q = edit_.total_quarters();
    if (ImGui::SliderInt("Escala 3D", &q, kScaleMinQuarters, kScaleMaxQuarters, label.c_str())) {
      edit_.scale_q = q;
      edit_.dlss_mode = kDlssOff;  // una escala a mano quita el DLSS
    }
  }
  ImGui::TextWrapped(
      "El juego corre en su modo original de 720p y la escena 3D se dibuja a la escala "
      "elegida (entre paréntesis, la resolución a la que equivale); luego se ajusta a la "
      "resolución de salida. La interfaz va siempre a la resolución de salida. Cada escala "
      "cuesta lo que su resolución. Requiere reiniciar.");
  ImGui::Checkbox("Pantalla completa", &edit_.fullscreen);
  ImGui::SameLine();
  ImGui::Checkbox("VSync", &edit_.vsync);

  ImGui::SeparatorText("Antialiasing");
  ImGui::Combo("Antialiasing (post-proceso)", &edit_.fxaa, kFxaaLabels, 4);
  ImGui::Combo("Filtro de presentación", &edit_.present, kPresentLabels, 3);
  if (ImGui::Combo("DLSS (NVIDIA RTX)", &edit_.dlss_mode, kDlssModeLabels, kDlssModeCount)) {
    edit_.NormalizeDlss();
    if (edit_.dlss_mode != kDlssOff) edit_.scale_q = edit_.DlssQuarters(edit_.dlss_mode);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Reconstruye el 3D a la resolución de salida con DLSS. Con un modo,\n"
                      "la escala 3D la elige DLSS (nunca por debajo de 720p). Sustituye\n"
                      "al SMAA. Requiere reiniciar.");
  }
  ImGui::Combo("Nitidez 3D", &edit_.sharpness, kSharpnessLabels, kSharpnessCount);
  ImGui::TextWrapped(
      "MSAA no está disponible: la escena principal del juego no lo usa y el emulador no "
      "puede forzarlo sin cambiar el reparto de la EDRAM que calcula el propio juego. Una "
      "escala 3D por encima del 100 % es supermuestreo: suaviza también texturas y sombras.");
  const int render_q = edit_.total_quarters();
  ImGui::Text("Render interno: %dx%d   Ventana: %dx%d", edit_.backbuffer_w() * render_q / 4,
              edit_.backbuffer_h() * render_q / 4, edit_.window_w(), edit_.window_h());

  ImGui::SeparatorText("Parches (se aplican al instante)");
  ImGui::Checkbox("60 fps", &edit_.fps60);
  ImGui::SameLine(220);
  ImGui::Checkbox("Animar personajes en la CPU (requiere reiniciar)", &edit_.flicker_fix);
  ImGui::Checkbox("Desactivar profundidad de campo", &edit_.disable_dof);
  ImGui::SameLine(220);
  ImGui::Checkbox("Desactivar motion blur", &edit_.disable_motion_blur);
  ImGui::Checkbox("Desactivar sombras dinámicas", &edit_.disable_dynamic_shadows);

  ImGui::SeparatorText("Turbo (se aplica al instante)");
  ImGui::Checkbox("Permitir turbo", &edit_.turbo_enabled);
  ImGui::SameLine(220);
  ImGui::Checkbox("Solo mientras se mantiene", &edit_.turbo_hold);
  ImGui::Combo("Velocidad", &edit_.turbo_scalar, kTurboScalarLabels, kTurboScalarCount);
  ImGui::Combo("Botón del mando", &edit_.turbo_button, kTurboButtonLabels, 5);

  ImGui::SeparatorText("Calidad de vida (se aplica al instante)");
  ImGui::Checkbox("Contador de fps", &edit_.show_fps);
  ImGui::Checkbox("Guardar en cualquier sitio", &edit_.save_anywhere);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Habilita Guardar en el menú System aunque no estés en un punto\n"
                      "de guardado. Se nota al volver a abrir el menú.\n"
                      "Úsalo en exploración: guardar a mitad de una escena o de un\n"
                      "evento puede dejar la partida en un estado inconsistente.");
  }

  ImGui::SeparatorText("Trucos (se aplican al instante)");
  ImGui::Checkbox("Sin batallas aleatorias", &edit_.no_random_battles);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("El contador de encuentros deja de avanzar mientras está activo.\n"
                      "Los combates de la historia siguen saliendo. Al quitarlo, el\n"
                      "contador sigue donde estaba.");
  }

#ifdef LO_DEV
  ImGui::SeparatorText("Texturas (se aplican al recargar)");
  ImGui::Checkbox("Volcar texturas a dump/textures", &edit_.dump_textures);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Guarda cada textura que carga el juego como PNG.\n"
                      "El nombre lleva el hash que identifica la textura.");
  }
  ImGui::Checkbox("Usar pack de texturas (carpeta textures)", &edit_.texture_pack);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Sustituye una textura por el PNG que tenga su mismo\n"
                      "prefijo tex_<hash>. Puede ser de mayor resolución.");
  }
  if (ImGui::Button("Recargar texturas (F7)", ImVec2(200, 0))) {
    ApplyHot(edit_);
    ReloadTexturePack();
    status_ = "Texturas recargadas desde la carpeta del pack.";
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(sin reiniciar; puede tirar un par de frames)");
  const bool volcando = DiscTextureDumpRunning();
  ImGui::BeginDisabled(volcando);
  if (ImGui::Button(volcando ? "Volcando texturas del disco..." : "Volcar todas las texturas del disco",
                    ImVec2(280, 0))) {
    DiscTextureDumpStart();
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Lee todas las texturas de los discos (sin pasar por las zonas) y las\n"
                      "guarda en dump/disc_textures, ordenadas por paquete y con su nombre.\n"
                      "Llevan el mismo tex_<hash> que usa el pack. Tarda unos minutos y\n"
                      "se puede seguir jugando mientras tanto.");
  }
  ImGui::TextWrapped("%s", DiscTextureDumpStatus().c_str());
#endif
  ImGui::TextWrapped("También con la tecla F6. El botón elegido queda reservado: el juego no lo ve. "
                     "Estado actual: %s", TurboActive() ? "TURBO ACTIVO" : "normal");
  ImGui::Separator();
  bool needs_restart = edit_.NeedsRestartFrom(running_);
  if (ImGui::Button("Guardar", ImVec2(120, 0))) {
    if (WriteToml(config_path_, edit_)) {
      ApplyHot(edit_);
      pending_restart_ = needs_restart;
      status_ = needs_restart ? "Guardado. Los cambios de resolución/antialiasing requieren reiniciar."
                              : "Guardado y aplicado.";
      ask_restart_ = needs_restart;
    } else {
      status_ = "Error al guardar el fichero de configuración.";
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Guardar y reiniciar", ImVec2(160, 0))) {
    if (WriteToml(config_path_, edit_)) {
      ApplyHot(edit_);
      if (SpawnRestart()) {
        request_close_window_();
      } else {
        status_ = "No se pudo relanzar el juego; reinícialo a mano.";
      }
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Cerrar", ImVec2(100, 0))) open = false;
  if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
  if (ask_restart_) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "¿Reiniciar ahora con los nuevos ajustes?");
    if (ImGui::Button("Sí, reiniciar")) {
      if (SpawnRestart()) {
        request_close_window_();
      } else {
        status_ = "No se pudo relanzar el juego; reinícialo a mano.";
        ask_restart_ = false;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Más tarde")) ask_restart_ = false;
  } else if (pending_restart_) {
    ImGui::TextDisabled("Hay cambios pendientes de reinicio.");
  }
  ImGui::End();
  // Nunca ImGuiDialog::Close(): hace delete this y la app posee el objeto.
  if (!open) request_hide_();
}

}  // namespace lo
