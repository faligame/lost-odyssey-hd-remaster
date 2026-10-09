// lostodyssey - ReXGlue Recompiled Project
//
// Pestanas del port dentro de la pantalla de Configuracion del juego. Ver
// settings_page.h.
//
// Deteccion: se envuelve sub_822F19B0, la tarea de la pantalla de ajustes del
// juego (r3 = objeto de la tarea). En +4 esta su estado (0/1 = inactiva,
// 4 = interactiva) y en +0x1804 un dialogo modal abierto encima. Las
// direcciones salen de la investigacion publicada por freefrank/
// LostOdysseyRecomp; el codigo es propio. La tarea NO se sustituye: siempre se
// llama a la original, que sigue dibujando su pantalla debajo de la nuestra.
//
// Mando: el juego lee el mando en dos sitios (turbo_hooks.cpp) y ambos llaman a
// SettingsPageProcessInput. Con una pestana del port abierta se interpretan las
// pulsaciones y el juego recibe el mando en reposo, asi el menu nativo no
// reacciona. Al cambiar de pestana, los botones que siguen pulsados no se le
// entregan hasta que se sueltan: si no, el mismo B que vuelve al menu del juego
// cerraria tambien la Configuracion nativa.
//
// Aspecto: maquetacion de 1280x720 medida sobre la pantalla nativa (captura a
// 1920x1080): metal de UI_MAIN_00 tintado, cabecera con la esquina curva del
// panel izquierdo, placas con bisel, celda elegida hundida, fila del cursor en
// placa clara con sombra, caja de ayuda hundida. Texto con Maru23 y titulo con
// LocTit1; las variantes de color (cara oscura con contorno claro, texto
// inactivo) se generan recoloreando las paginas de la fuente.

#include "settings_page.h"
#include "texture_download.h"
#include "lo_system_info.h"

#include "lo_i18n.h"
#include "shader_prep.h"
#include "shader_prewarm.h"
#include "multidisc.h"

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REX_EXTERN(__imp__sub_822F19B0);

REXCVAR_DEFINE_BOOL(lo_show_fps, false, "LostOdyssey/QoL",
                    "Contador de fps del juego (y ms por fotograma) arriba a la izquierda")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#ifdef LO_DEV
REXCVAR_DEFINE_BOOL(lo_shader_prep_preview, false, "LostOdyssey/Diagnostico",
                    "Muestra la pantalla de preparacion de sombreadores (para revisarla)");
#endif

namespace {

constexpr uint32_t kTaskStateOffset = 4;
constexpr uint32_t kTaskModalOffset = 0x1804;
constexpr uint32_t kTaskStateInteractive = 4;
// Si la tarea deja de ejecutarse (pantalla cerrada), su ultimo estado caduca.
constexpr int64_t kTaskStaleMs = 250;

// X_INPUT_STATE: paquete (4) + X_INPUT_GAMEPAD: botones (2), gatillos (1+1),
// sticks (2 x 4), todo big-endian.
constexpr size_t kGamepadOffset = 4;
constexpr size_t kGamepadSize = 12;
constexpr uint16_t kDpadUp = 0x0001;
constexpr uint16_t kDpadDown = 0x0002;
constexpr uint16_t kDpadLeft = 0x0004;
constexpr uint16_t kDpadRight = 0x0008;
constexpr uint16_t kDpadMask = kDpadUp | kDpadDown | kDpadLeft | kDpadRight;
constexpr uint16_t kButtonLB = 0x0100;
constexpr uint16_t kButtonRB = 0x0200;
constexpr uint16_t kButtonA = 0x1000;
constexpr uint16_t kButtonB = 0x2000;
constexpr int16_t kStickThreshold = 16000;

// Pestanas: la 0 es la pagina nativa del juego; el resto son del port.
// La pagina de texturas muestra si el pack HD esta instalado y permite descargarlo; volcar y
// recargar texturas son herramientas de desarrollo (LO_DEV).
constexpr int kPageCount = 5;
constexpr const char* kPageNames[kPageCount] = {"Juego", "Gráficos", "Parches", "Extras", "Texturas"};

std::atomic<int64_t> g_task_seen_ms{0};
std::atomic<bool> g_task_interactive{false};
std::atomic<int> g_page{0};
// Cruceta (y stick izquierdo) con una pestana del port abierta: lo mantenido y
// los flancos acumulados hasta que los consume el hilo de interfaz.
std::atomic<uint16_t> g_nav_held{0};
std::atomic<uint16_t> g_nav_pressed{0};

// Solo desde los hilos del juego que leen el mando.
uint16_t g_prev_nav = 0;
uint16_t g_swallow = 0;  // botones retenidos hasta que se suelten

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint16_t LoadBe16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }

bool SettingsScreenInteractive() {
  return g_task_interactive.load(std::memory_order_relaxed) &&
         NowMs() - g_task_seen_ms.load(std::memory_order_relaxed) < kTaskStaleMs;
}

void SetPage(int page, const char* why) {
  const int old = g_page.exchange(page);
  if (old != page) {
    REXLOG_INFO("[settings] pestana {} -> {} ({})", kPageNames[old], kPageNames[page], why);
  }
}

}  // namespace

REX_EXTERN(sub_822F19B0) {
  const uint32_t task = ctx.r3.u32;
  __imp__sub_822F19B0(ctx, base);

  auto* memory = REX_KERNEL_MEMORY();
  const uint8_t* object = memory ? memory->TranslateVirtual<const uint8_t*>(task) : nullptr;
  if (!object) {
    return;
  }
  const uint32_t state = LoadBe32(object + kTaskStateOffset);
  const uint32_t modal = LoadBe32(object + kTaskModalOffset);
  const bool interactive = state == kTaskStateInteractive && modal == 0;
  g_task_seen_ms.store(NowMs(), std::memory_order_relaxed);
  g_task_interactive.store(interactive, std::memory_order_relaxed);
  if (!interactive) {
    SetPage(0, "la pantalla de Configuracion no esta interactiva");
  }
}

namespace lo {

void SettingsPageProcessInput(uint8_t* state) {
  uint8_t* pad = state + kGamepadOffset;
  const uint16_t buttons = LoadBe16(pad);
  const int16_t lx = int16_t(LoadBe16(pad + 4));
  const int16_t ly = int16_t(LoadBe16(pad + 6));
  uint16_t nav = buttons;
  if (ly > kStickThreshold) nav |= kDpadUp;
  if (ly < -kStickThreshold) nav |= kDpadDown;
  if (lx < -kStickThreshold) nav |= kDpadLeft;
  if (lx > kStickThreshold) nav |= kDpadRight;
  const uint16_t pressed = nav & ~g_prev_nav;
  g_prev_nav = nav;
  g_swallow &= buttons;  // lo que ya se ha soltado deja de retenerse

  const int page = g_page.load(std::memory_order_relaxed);
  int next = page;
  const char* why = "";
  if (!SettingsScreenInteractive()) {
    next = 0;
    why = "fuera de la pantalla de Configuracion";
  } else if (pressed & kButtonRB) {
    next = (page + 1) % kPageCount;
    why = "RB";
  } else if (pressed & kButtonLB) {
    next = (page + kPageCount - 1) % kPageCount;
    why = "LB";
  } else if (page != 0 && (pressed & kButtonB)) {
    next = 0;
    why = "B";
  }
  if (next != page) {
    SetPage(next, why);
    g_swallow = buttons;
  }

  if (next != 0) {
    // El juego ve el mando en reposo mientras la pestana es nuestra.
    g_nav_held.store(nav, std::memory_order_relaxed);
    if (next == page) {
      g_nav_pressed.fetch_or(pressed & (kDpadMask | kButtonA), std::memory_order_relaxed);
    }
    std::memset(pad, 0, kGamepadSize);
    return;
  }
  g_nav_held.store(0, std::memory_order_relaxed);
  if (g_swallow) {
    const uint16_t visible = buttons & ~g_swallow;
    pad[0] = uint8_t(visible >> 8);
    pad[1] = uint8_t(visible & 0xFF);
  }
}

// --- Filas de cada pestana ----------------------------------------------------

enum class SettingsRowAction : int { kNone, kRestart, kReloadTextures, kDownloadTextures };

namespace {

struct Row {
  const char* label;
  const char* help;
  std::vector<const char*> choices;
  int (*get)(const Options&);
  void (*set)(Options&, int);
  SettingsRowAction action = SettingsRowAction::kNone;
  // Ayuda propia de cada opcion (la de la opcion elegida); vacio = help.
  std::vector<const char*> choice_help = {};
  // Fila que no se puede cambiar con las opciones actuales (en gris, con
  // disabled_help en la barra de ayuda).
  bool (*disabled)(const Options&) = nullptr;
  const char* disabled_help = nullptr;
  // Opcion que no se ofrece con lo demas elegido (en gris; el cursor se la salta).
  bool (*choice_enabled)(const Options&, int) = nullptr;
  // Texto de cada opcion calculado (deslizadores: Escala 3D con su %); vacio = choices.
  std::string (*choice_label)(const Options&, int) = nullptr;
  // Ayuda calculada (sustituye a help y choice_help).
  std::string (*help_fn)(const Options&) = nullptr;
};

// Mas opciones que esto: deslizador (valor en una celda y barra) en vez de celdas.
constexpr int kSliderMinChoices = 8;

// Resoluciones de salida de menor a mayor numero de pixeles (el enum sigue el orden historico
// de las claves del toml).
constexpr OutputRes kOutputOrder[] = {OutputRes::k720p,      OutputRes::kSteamDeck, OutputRes::k900p,
                                      OutputRes::k1080p,     OutputRes::kUltra1080, OutputRes::k1440p,
                                      OutputRes::k1620p,     OutputRes::kUltra1440, OutputRes::k1800p,
                                      OutputRes::k4K};
int OutputSliderIndex(const Options& o) {
  for (int i = 0; i < int(std::size(kOutputOrder)); ++i) {
    if (kOutputOrder[i] == o.output) return i;
  }
  return 0;
}

// Escala 3D: 4 ... 28 cuartos.
const std::vector<const char*>& ScaleChoices() {
  static const std::vector<const char*> kChoices(kScaleMaxQuarters - kScaleMinQuarters + 1, "");
  return kChoices;
}
std::string ScaleChoiceLabel(const Options& o, int i) {
  const int q = kScaleMinQuarters + i;
  return ScaleLabel(q) + "  (" + std::to_string(o.ScalePercent(q)) + " %)";
}
std::string ScaleHelp(const Options& o) {
  const int q = o.total_quarters();
  char text[256];
  if (o.dlss_mode != kDlssOff) {
    std::snprintf(text, sizeof(text),
                  Tr("Con DLSS el 3D va a %dx%d (lo pone el modo). Al mover la escala se quita el DLSS."),
                  o.backbuffer_w() * q / 4, o.backbuffer_h() * q / 4);
  } else if (o.ScalePercent(q) > 100) {
    std::snprintf(text, sizeof(text),
                  Tr("El 3D se dibuja a %dx%d y se reduce a la salida: supermuestreo, más nítido. Requiere reiniciar."),
                  o.backbuffer_w() * q / 4, o.backbuffer_h() * q / 4);
  } else {
    std::snprintf(text, sizeof(text), Tr("El 3D se dibuja a %dx%d. Requiere reiniciar."), o.backbuffer_w() * q / 4,
                  o.backbuffer_h() * q / 4);
  }
  return text;
}

// Si/No sobre un bool de Options (Invertido: la opcion es un "desactivar").
template <bool Options::*F>
int GetYes(const Options& o) { return o.*F ? 0 : 1; }
template <bool Options::*F>
void SetYes(Options& o, int v) { o.*F = v == 0; }
template <bool Options::*F>
int GetYesInverted(const Options& o) { return o.*F ? 1 : 0; }
template <bool Options::*F>
void SetYesInverted(Options& o, int v) { o.*F = v != 0; }
int GetNone(const Options&) { return 0; }
void SetNone(Options&, int) {}
// 0 = no instalado, 1 = descargando, 2 = instalado.
int GetTexturePackState(const Options&) {
  if (TexturePackInstalled()) return 2;
  return TextureDownloadActive() ? 1 : 0;
}

const std::vector<Row>& PageRows(int page) {
  static const std::vector<Row> kRows[kPageCount] = {
      {},
      {
          {"Resolución",
           "Tamaño de la imagen y nitidez de la interfaz. Deck: 1280x800; UW: 21:9. Más "
           "ancho = más 3D a los lados; la interfaz no se estira. Requiere reiniciar.",
           {"1280x720 (720p)", "1280x800 (Steam Deck)", "1600x900 (900p)", "1920x1080 (1080p)",
            "2560x1080 (21:9)", "2560x1440 (1440p)", "2880x1620 (1620p)", "3440x1440 (21:9)",
            "3200x1800 (1800p)", "3840x2160 (4K)"},
           &OutputSliderIndex,
           [](Options& o, int v) {
             o.output = kOutputOrder[std::clamp(v, 0, int(std::size(kOutputOrder)) - 1)];
             o.NormalizeDlss();
           }},
          {"Escala 3D",
           "",
           ScaleChoices(),
           [](const Options& o) { return o.total_quarters() - kScaleMinQuarters; },
           [](Options& o, int v) {
             o.scale_q = kScaleMinQuarters + v;
             o.dlss_mode = kDlssOff;  // una escala a mano quita el DLSS
           },
           SettingsRowAction::kNone,
           {},
           nullptr,
           nullptr,
           nullptr,
           &ScaleChoiceLabel,
           &ScaleHelp},
          {"Pantalla completa", "Pantalla completa o ventana. Requiere reiniciar.", {"Sí", "No"},
           &GetYes<&Options::fullscreen>, &SetYes<&Options::fullscreen>},
          {"Sincronización vertical", "Evita el desgarro de la imagen. Requiere reiniciar.",
           {"Sí", "No"}, &GetYes<&Options::vsync>, &SetYes<&Options::vsync>},
          {"Antialiasing", "Suavizado de bordes en post-proceso; SMAA es el más limpio. Requiere reiniciar.",
           {"No", "FXAA", "FXAA extremo", "SMAA"},
           [](const Options& o) { return o.fxaa; },
           [](Options& o, int v) { o.fxaa = v; },
           SettingsRowAction::kNone,
           {},
           [](const Options& o) { return o.dlss_mode != kDlssOff; },
           "Con DLSS no se usa: DLSS ya suaviza el 3D."},
          {"DLSS",
           "",
           {"No", "DLAA", "Calidad", "Equilibrado", "Rendimiento", "Ultra"},
           [](const Options& o) { return o.dlss_mode; },
           [](Options& o, int v) {
             o.dlss_mode = v;
             if (v != kDlssOff) o.scale_q = o.DlssQuarters(v);
           },
           SettingsRowAction::kNone,
           {"Sin DLSS: el 3D se reescala desde la escala 3D elegida. Requiere reiniciar.",
            "DLAA: el 3D a la resolución de salida; DLSS solo suaviza. Lo más nítido. Requiere reiniciar.",
            "Calidad: el 3D a 2/3 de la resolución (4K: 1440p) y DLSS lo reconstruye. Requiere reiniciar.",
            "Equilibrado: el 3D al 58 % de la resolución (4K: 1260p). Nunca baja de 720p. Requiere reiniciar.",
            "Rendimiento: el 3D a la mitad de la resolución (4K: 1080p). Nunca baja de 720p. Requiere reiniciar.",
            "Ultra rendimiento: el 3D a un tercio (4K: 720p). Nunca baja de 720p. Requiere reiniciar."},
           nullptr,
           nullptr,
           // Solo los modos que dan una escala propia a esta resolucion (los demas caen a 720p).
           [](const Options& o, int v) { return o.DlssModeUseful(v); }},
          {"Nitidez 3D",
           "Enfoca el 3D (sobre todo con DLSS) sin tocar la interfaz. Se aplica al instante.",
           {"No", "Baja", "Media", "Alta"},
           [](const Options& o) { return o.sharpness; },
           [](Options& o, int v) { o.sharpness = v; }},
          {"API gráfica", "Direct3D 12 es la ruta probada; Vulkan es experimental. Requiere reiniciar.",
           {"Direct3D 12", "Vulkan"},
           [](const Options& o) { return int(o.gpu_backend); },
           [](Options& o, int v) { o.gpu_backend = GpuBackend(v); }},
          {"Reiniciar el juego",
           "Aplica los cambios que requieren reiniciar. Guarda la partida antes.",
           {"Reiniciar ahora"}, &GetNone, &SetNone, SettingsRowAction::kRestart},
      },
      {
          {"60 fps", "Desbloquea 60 fotogramas por segundo. Se aplica al instante.", {"Sí", "No"},
           &GetYes<&Options::fps60>, &SetYes<&Options::fps60>},
#ifdef LO_DEV
          {"Animación por CPU", "Anima los personajes en el procesador (parche de Xenia). Cuesta mucho rendimiento en combates; requiere reiniciar.",
           {"Sí", "No"}, &GetYes<&Options::flicker_fix>, &SetYes<&Options::flicker_fix>},
#endif
          {"Profundidad de campo", "Desenfoque del fondo en escenas y combates.", {"Sí", "No"},
           &GetYesInverted<&Options::disable_dof>, &SetYesInverted<&Options::disable_dof>},
          {"Desenfoque de movimiento", "Desenfoque al mover la cámara.", {"Sí", "No"},
           &GetYesInverted<&Options::disable_motion_blur>,
           &SetYesInverted<&Options::disable_motion_blur>},
          {"Sombras dinámicas", "Sombras en tiempo real; quitarlas mejora el rendimiento.",
           {"Sí", "No"}, &GetYesInverted<&Options::disable_dynamic_shadows>,
           &SetYesInverted<&Options::disable_dynamic_shadows>},
          {"Fundido entre escenas",
           "La imagen anterior se desvanece sobre la nueva al cambiar de escena. Se aplica al instante.",
           {"Sí", "No"}, &GetYes<&Options::crossfade>, &SetYes<&Options::crossfade>},
      },
      {
          {"Idioma", "Idioma del juego: textos, voces y menús. Requiere reiniciar.",
           {"English", "Français", "Deutsch", "Italiano", "Español", "日本語"},
           [](const Options& o) { return int(o.language); },
           [](Options& o, int v) { o.language = Lang(v); }},
          {"Contador de fps",
           "Muestra arriba a la izquierda los fotogramas por segundo y lo que tarda cada uno.",
           {"Sí", "No"}, &GetYes<&Options::show_fps>, &SetYes<&Options::show_fps>},
          {"Precarga de sombreadores",
           "Qué pipelines se crean de golpe la primera vez. Evita tirones y bajones de FPS.",
           {"Todos los discos", "Parte actual"},
           // Sin elegir en el menu: lo elegido en la pantalla de la primera vez (cvar).
           [](const Options& o) {
             if (o.shader_precache >= 0) return o.shader_precache;
             return rex::cvar::GetFlagByName("lo_shader_precache") == "todo" ? 0 : 1;
           },
           [](Options& o, int v) { o.shader_precache = v; },
           SettingsRowAction::kNone,
           {"Todos los discos disponibles, uno detrás de otro (en segundo plano si ya estás jugando).",
            "Solo la parte actual; las siguientes se precargan al llegar a ellas."}},
          {"Guardar en cualquier sitio",
           "Permite Guardar en el menú System fuera de los puntos de guardado. Úsalo explorando.",
           {"Sí", "No"}, &GetYes<&Options::save_anywhere>, &SetYes<&Options::save_anywhere>},
          {"Batallas aleatorias",
           "Quítalas para explorar sin encuentros. Los combates de la historia siguen saliendo.",
           {"Sí", "No"}, &GetYesInverted<&Options::no_random_battles>,
           &SetYesInverted<&Options::no_random_battles>},
          {"Turbo", "Permite acelerar el juego con un botón del mando o con F6.", {"Sí", "No"},
           &GetYes<&Options::turbo_enabled>, &SetYes<&Options::turbo_enabled>},
          {"Velocidad del turbo", "Multiplicador de velocidad mientras el turbo está activo.",
           {"1.5x", "2x", "3x", "4x", "6x", "8x"},
           [](const Options& o) { return o.turbo_scalar; },
           [](Options& o, int v) { o.turbo_scalar = v; }},
          {"Botón del turbo", "Botón que activa el turbo; el juego deja de verlo.",
           {"L3", "R3", "Back", "LB", "RB"},
           [](const Options& o) { return o.turbo_button; },
           [](Options& o, int v) { o.turbo_button = v; }},
          {"Modo del turbo", "Conmutar: pulsar para activarlo o quitarlo. Mantener: solo mientras se pulsa.",
           {"Conmutar", "Mantener"}, &GetYesInverted<&Options::turbo_hold>,
           &SetYesInverted<&Options::turbo_hold>},
          {"Reiniciar el juego",
           "Aplica los cambios que requieren reiniciar. Guarda la partida antes.",
           {"Reiniciar ahora"}, &GetNone, &SetNone, SettingsRowAction::kRestart},
      },
      {
          {"Pack de texturas HD", "Texturas en alta definición, cifradas y ligadas a tus discos.",
           {"No instalado", "Descargando", "Instalado"}, &GetTexturePackState, &SetNone},
          {"Descargar texturas HD",
           "Descarga el pack (unos 28 GB). Sigue mientras juegas y se reanuda si se corta.",
           {"Descargar ahora"}, &GetNone, &SetNone, SettingsRowAction::kDownloadTextures},
#ifdef LO_DEV
          {"Pack de texturas", "Sustituye texturas por los PNG de la carpeta textures.", {"Sí", "No"},
           &GetYes<&Options::texture_pack>, &SetYes<&Options::texture_pack>},
          {"Volcar texturas", "Guarda cada textura que carga el juego en dump/textures.",
           {"Sí", "No"}, &GetYes<&Options::dump_textures>, &SetYes<&Options::dump_textures>},
          {"Recargar texturas", "Vuelve a leer la carpeta del pack sin reiniciar (también con F7).",
           {"Recargar"}, &GetNone, &SetNone, SettingsRowAction::kReloadTextures},
#endif
      },
  };
  return kRows[page];
}

// --- Maquetacion nativa (coordenadas de 1280x720) ------------------------------

constexpr float kRowTop = 150.0f;
constexpr float kRowHeight = 43.0f;
constexpr float kRowsBottom = 640.0f - 150.0f;  // alto disponible para las filas
constexpr float kLabelX0 = 65.0f;
constexpr float kLabelX1 = 364.0f;
constexpr float kLabelTextX = 80.6f;
constexpr float kLabelTextMaxWidth = 278.0f;
constexpr float kCellsX = 386.0f;
constexpr float kCellsWidth = 639.0f;
constexpr float kCellGap = 6.0f;
constexpr float kTextScale = 0.88f;     // Maru23 en filas y valores
constexpr float kTextDy = 6.7f;         // origen del texto respecto a la fila
constexpr float kTitleScale = 0.64f;    // LocTit1
constexpr float kSectionScale = 0.88f;  // "Menu" sobre las filas
constexpr float kHelpLabelXScale = 0.73f;  // "Ayuda", estrechada
constexpr float kHelpXScale = 0.77f;       // texto de ayuda, estrechado
constexpr float kHelpTextX = 128.0f;
constexpr float kHelpTextMaxWidth = 1070.0f;
constexpr float kMetalTileW = 402.0f;
constexpr float kMetalTileH = 135.0f;

ImU32 Rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return IM_COL32(r, g, b, a); }
ImU32 Black(uint8_t a) { return IM_COL32(0, 0, 0, a); }

// El metal de UI_MAIN_00 es algo mas claro que en pantalla (105 frente a 96).
const ImU32 kMetalTint = Rgb(233, 233, 233);
const ImU32 kSteel = Rgb(96, 97, 96);
const ImU32 kInk = Rgb(229, 229, 229);
const ImU32 kTitleInk = Rgb(241, 240, 241);
const ImU32 kDisabledTint = Rgb(180, 180, 180);
// Variantes recoloreadas de las paginas de la fuente (cara, contorno).
constexpr std::array<uint8_t, 3> kSelectedFace = {27, 25, 27};
constexpr std::array<uint8_t, 3> kSelectedEdge = {236, 235, 236};
constexpr std::array<uint8_t, 3> kInactiveFace = {150, 150, 150};
constexpr std::array<uint8_t, 3> kInactiveEdge = {88, 88, 88};

enum class TextStyle { kNormal, kTitle, kSelected, kInactive, kDisabled };

ImTextureID Id(const rex::ui::ImmediateTexture* t) {
  return reinterpret_cast<ImTextureID>(const_cast<rex::ui::ImmediateTexture*>(t));
}

// Solo BMP en 1-3 bytes: el juego no tiene glifos fuera de Latin-1 y simbolos.
char32_t NextCodepoint(std::string_view s, size_t& i) {
  const uint8_t c = uint8_t(s[i++]);
  if (c < 0x80) return c;
  auto cont = [&]() -> uint32_t {
    if (i < s.size() && (uint8_t(s[i]) & 0xC0) == 0x80) return uint8_t(s[i++]) & 0x3F;
    return 0;
  };
  if ((c & 0xE0) == 0xC0) {
    const uint32_t a = cont();
    return char32_t(((c & 0x1F) << 6) | a);
  }
  if ((c & 0xF0) == 0xE0) {
    const uint32_t a = cont();
    const uint32_t b = cont();
    return char32_t(((c & 0x0F) << 12) | (a << 6) | b);
  }
  return U'?';
}

// Imagen de ui.lopack por huella de textura, pedida al plugin grafico (ya cargado). false si no hay.
bool QueryUiImage(uint64_t hash, menu_assets::Image& out) {
  using Fn = int (*)(uint64_t, uint32_t*, uint32_t*, uint8_t*, uint64_t);
  static Fn fn = [] {
    HMODULE plugin = GetModuleHandleW(L"rexgpu-odisea.dll");
    return plugin ? reinterpret_cast<Fn>(GetProcAddress(plugin, "odisea_UiImageRgba")) : nullptr;
  }();
  if (!fn || !hash) return false;
  uint32_t w = 0, h = 0;
  if (!fn(hash, &w, &h, nullptr, 0) || !w || !h || w > 16384 || h > 16384) return false;
  out.width = w;
  out.height = h;
  out.hash = hash;
  out.rgba.resize(size_t(w) * h * 4);
  return fn(hash, &w, &h, out.rgba.data(), out.rgba.size()) != 0;
}

void AttachHdPages(menu_assets::MenuAssets& assets) {
  int found = 0, total = 0;
  for (menu_assets::Font* font : {&assets.text, &assets.title}) {
    font->hd_pages.assign(font->pages.size(), {});
    for (size_t i = 0; i < font->pages.size(); ++i) {
      ++total;
      if (QueryUiImage(font->pages[i].hash, font->hd_pages[i])) ++found;
    }
  }
  REXLOG_INFO("[settings] fuentes HD de la interfaz: {} de {} paginas", found, total);
}

menu_assets::Image Crop(const menu_assets::Image& src, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  menu_assets::Image out;
  out.width = w;
  out.height = h;
  out.rgba.resize(size_t(w) * h * 4);
  for (uint32_t row = 0; row < h; ++row) {
    std::memcpy(&out.rgba[size_t(row) * w * 4], &src.rgba[(size_t(y + row) * src.width + x) * 4], size_t(w) * 4);
  }
  return out;
}

// Las paginas traen cara blanca y contorno negro: el brillo decide la mezcla
// entre los dos colores nuevos; el alfa se conserva.
std::vector<uint8_t> Recolor(const menu_assets::Image& page, const std::array<uint8_t, 3>& face,
                             const std::array<uint8_t, 3>& edge) {
  std::vector<uint8_t> out = page.rgba;
  for (size_t i = 0; i < out.size(); i += 4) {
    const int l = out[i];
    for (int k = 0; k < 3; ++k) {
      out[i + k] = uint8_t(edge[k] + (int(face[k]) - int(edge[k])) * l / 255);
    }
  }
  return out;
}

}  // namespace

struct GameMenuTextures {
  struct Font {
    const menu_assets::Font* data = nullptr;
    // Por variante (0 clara para tintar, 1 seleccionada, 2 inactiva) y pagina.
    std::array<std::vector<std::unique_ptr<rex::ui::ImmediateTexture>>, 3> pages;
    // Tamano de la pagina ORIGINAL (las coordenadas de los glifos son de ella; la textura puede ser HD).
    std::vector<std::pair<float, float>> page_size;
  };
  int hd_factor = 1;  // 1, 2 o 4: tamano de las paginas HD respecto a las originales
  std::unique_ptr<rex::ui::ImmediateTexture> metal;
  std::unique_ptr<rex::ui::ImmediateTexture> ui_main;
  std::unique_ptr<rex::ui::ImmediateTexture> window;
  Font text;
  Font title;
};

namespace {

// Pagina HD (k veces la original) reducida a factor veces con promedio por area (con el alfa premultiplicado).
// Vacia si no cuadra con la original.
menu_assets::Image DownscaleHd(const menu_assets::Image& hd, uint32_t orig_w, uint32_t orig_h, int factor) {
  menu_assets::Image out;
  if (hd.rgba.empty() || !orig_w || !orig_h || hd.width % orig_w || hd.height % orig_h) return out;
  const uint32_t k = hd.width / orig_w;
  if (k == 0 || hd.height != orig_h * k) return out;
  uint32_t f = uint32_t(std::max(1, factor));
  if (f > k || k % f) f = 1;
  const uint32_t step = k / f;
  out.width = orig_w * f;
  out.height = orig_h * f;
  out.rgba.resize(size_t(out.width) * out.height * 4);
  for (uint32_t y = 0; y < out.height; ++y) {
    for (uint32_t x = 0; x < out.width; ++x) {
      uint64_t r = 0, g = 0, b = 0, a = 0;
      for (uint32_t dy = 0; dy < step; ++dy) {
        for (uint32_t dx = 0; dx < step; ++dx) {
          const uint8_t* px = &hd.rgba[(size_t(y * step + dy) * hd.width + x * step + dx) * 4];
          r += uint64_t(px[0]) * px[3];
          g += uint64_t(px[1]) * px[3];
          b += uint64_t(px[2]) * px[3];
          a += px[3];
        }
      }
      uint8_t* o = &out.rgba[(size_t(y) * out.width + x) * 4];
      if (a) {
        o[0] = uint8_t(r / a);
        o[1] = uint8_t(g / a);
        o[2] = uint8_t(b / a);
      } else {
        o[0] = o[1] = o[2] = 0;
      }
      o[3] = uint8_t(a / (uint64_t(step) * step));
    }
  }
  return out;
}

std::unique_ptr<GameMenuTextures> CreateTextures(rex::ui::ImmediateDrawer& drawer,
                                                 const menu_assets::MenuAssets& assets, int hd_factor) {
  auto t = std::make_unique<GameMenuTextures>();
  t->hd_factor = hd_factor;
  bool ok = true;
  auto upload = [&](const menu_assets::Image& image, bool repeat, const std::vector<uint8_t>& rgba) {
    auto texture = drawer.CreateTexture(image.width, image.height, rex::ui::ImmediateTextureFilter::kLinear,
                                        repeat, rgba.data());
    ok = ok && texture;
    return texture;
  };
  const menu_assets::Image metal = Crop(assets.ui_main, 3, 62, 402, 135);
  t->metal = upload(metal, true, metal.rgba);
  t->ui_main = upload(assets.ui_main, false, assets.ui_main.rgba);
  t->window = upload(assets.window, false, assets.window.rgba);
  for (auto [font, data] : {std::pair{&t->text, &assets.text}, std::pair{&t->title, &assets.title}}) {
    font->data = data;
    for (size_t i = 0; i < data->pages.size(); ++i) {
      const menu_assets::Image& original = data->pages[i];
      font->page_size.emplace_back(float(original.width), float(original.height));
      // La pagina HD (si la hay) en lugar de la original: mismas coordenadas, mas detalle.
      menu_assets::Image hd;
      if (i < data->hd_pages.size()) hd = DownscaleHd(data->hd_pages[i], original.width, original.height, hd_factor);
      const menu_assets::Image& page = hd.rgba.empty() ? original : hd;
      font->pages[0].push_back(upload(page, false, page.rgba));
      font->pages[1].push_back(upload(page, false, Recolor(page, kSelectedFace, kSelectedEdge)));
      font->pages[2].push_back(upload(page, false, Recolor(page, kInactiveFace, kInactiveEdge)));
    }
  }
  if (!ok) return nullptr;  // dispositivo aun no disponible: se reintenta
  return t;
}

}  // namespace

// Dibuja en la lista de primer plano de imgui con coordenadas de 1280x720
// escaladas y centradas en la ventana. Sin texturas del juego (datos ausentes)
// cae a colores planos y la fuente de imgui.
struct SettingsPainter {
  ImDrawList* dl = nullptr;
  const GameMenuTextures* tex = nullptr;
  ImFont* fallback_font = nullptr;
  // s = escala vertical; sx = horizontal (distinta solo con 16:10 / 21:9, donde
  // el menu nativo se estira con la imagen y el nuestro tiene que seguirlo).
  float s = 1.0f, sx = 1.0f, ox = 0.0f, oy = 0.0f;
  float left = 0.0f, right = 1280.0f, top = 0.0f, bottom = 720.0f;  // bordes de la ventana

  ImVec2 Raw(float x, float y) const { return ImVec2(ox + x * sx, oy + y * s); }
  ImVec2 Snap(float x, float y) const { return ImVec2(std::round(ox + x * sx), std::round(oy + y * s)); }

  void Fill(float x0, float y0, float x1, float y1, ImU32 color) const {
    ImVec2 a = Snap(x0, y0), b = Snap(x1, y1);
    if (b.x <= a.x) b.x = a.x + 1.0f;
    if (b.y <= a.y) b.y = a.y + 1.0f;
    dl->AddRectFilled(a, b, color);
  }

  void VGradient(float x0, float y0, float x1, float y1, ImU32 from, ImU32 to) const {
    dl->AddRectFilledMultiColor(Snap(x0, y0), Snap(x1, y1), from, from, to, to);
  }

  void HGradient(float x0, float y0, float x1, float y1, ImU32 from, ImU32 to) const {
    dl->AddRectFilledMultiColor(Snap(x0, y0), Snap(x1, y1), from, to, to, from);
  }

  // Metal cepillado a la densidad nativa (una unidad logica = un texel).
  void Metal(float x0, float y0, float x1, float y1) const {
    if (!tex) {
      Fill(x0, y0, x1, y1, kSteel);
      return;
    }
    const ImVec2 a = Snap(x0, y0), b = Snap(x1, y1);
    const float u0 = (a.x - ox) / sx / kMetalTileW, v0 = (a.y - oy) / s / kMetalTileH;
    const float u1 = (b.x - ox) / sx / kMetalTileW, v1 = (b.y - oy) / s / kMetalTileH;
    dl->AddImage(Id(tex->metal.get()), a, b, ImVec2(u0, v0), ImVec2(u1, v1), kMetalTint);
  }

  void Sprite(const rex::ui::ImmediateTexture* t, float sx, float sy, float sw, float sh, float x, float y,
              float w, float h, ImU32 tint = IM_COL32_WHITE) const {
    const float tw = float(t->width), th = float(t->height);
    dl->AddImage(Id(t), Snap(x, y), Snap(x + w, y + h), ImVec2(sx / tw, sy / th),
                 ImVec2((sx + sw) / tw, (sy + sh) / th), tint);
  }

  // Texto del port en el idioma del juego; si a la fuente del juego le falta
  // algun glifo (p. ej. textos japoneses propios), en ingles, y si tampoco, el
  // original.
  bool Covers(bool title, std::string_view text) const {
    if (!tex) return true;
    const auto& glyphs = (title ? tex->title : tex->text).data->glyphs;
    for (size_t i = 0; i < text.size();) {
      const char32_t c = NextCodepoint(text, i);
      if (c != U' ' && !glyphs.count(uint32_t(c))) return false;
    }
    return true;
  }
  const char* L(const char* es, bool title = false) const {
    const char* t = Tr(es);
    if (Covers(title, t)) return t;
    // "日本語" en una fuente sin kanji: el nombre del idioma en el idioma actual.
    if (std::string_view(es) == "日本語") {
      t = Tr("Japonés");
      if (Covers(title, t)) return t;
      return TrIn(Lang::kEnglish, "Japonés");
    }
    t = TrIn(Lang::kEnglish, es);
    return Covers(title, t) ? t : es;
  }

  float TextWidth(const GameMenuTextures::Font& f, std::string_view text, float scale) const {
    float width = 0.0f;
    for (size_t i = 0; i < text.size();) {
      const auto it = f.data->glyphs.find(NextCodepoint(text, i));
      if (it != f.data->glyphs.end()) width += float(it->second.w + f.data->kerning) * scale;
    }
    return width;
  }

  // cell_width > 0 centra en la celda; max_width > 0 estrecha el texto que no
  // cabe (como hace el juego con las opciones largas); x_scale estrecha siempre
  // (la barra de ayuda del juego usa la fuente comprimida en horizontal).
  void Text(TextStyle style, float x, float y, float scale, std::string_view text, float cell_width = 0.0f,
            float max_width = 0.0f, float x_scale = 1.0f) const {
    ImU32 tint = IM_COL32_WHITE;
    int variant = 0;
    switch (style) {
      case TextStyle::kNormal: tint = kInk; break;
      case TextStyle::kTitle: tint = kTitleInk; break;
      case TextStyle::kSelected: variant = 1; break;
      case TextStyle::kInactive: variant = 2; break;
      case TextStyle::kDisabled: variant = 2; tint = kDisabledTint; break;
    }
    if (!tex) {
      static const ImU32 kFallback[] = {kInk, kTitleInk, Rgb(30, 30, 30), Rgb(160, 160, 160), Rgb(120, 120, 120)};
      const float size = (style == TextStyle::kTitle ? 40.0f : 26.0f) * scale * s;
      float width = fallback_font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x / sx;
      if (cell_width > 0.0f) x += (cell_width - width) * 0.5f;
      dl->AddText(fallback_font, size, Raw(x, y), kFallback[int(style)], text.data(), text.data() + text.size());
      return;
    }
    const GameMenuTextures::Font& f = style == TextStyle::kTitle ? tex->title : tex->text;
    float width = TextWidth(f, text, scale) * x_scale;
    float squeeze = x_scale;
    if (max_width > 0.0f && width > max_width) {
      squeeze *= max_width / width;
      width = max_width;
    }
    if (cell_width > 0.0f) x += (cell_width - width) * 0.5f;
    for (size_t i = 0; i < text.size();) {
      const auto it = f.data->glyphs.find(NextCodepoint(text, i));
      if (it == f.data->glyphs.end()) continue;
      const menu_assets::Glyph& g = it->second;
      const rex::ui::ImmediateTexture* page = f.pages[variant][g.page].get();
      const float pw = f.page_size[g.page].first, ph = f.page_size[g.page].second;
      dl->AddImage(Id(page), Raw(x, y), Raw(x + g.w * scale * squeeze, y + g.h * scale),
                   ImVec2(g.x / pw, g.y / ph), ImVec2((g.x + g.w) / pw, (g.y + g.h) / ph), tint);
      x += float(g.w + f.data->kerning) * scale * squeeze;
    }
  }

  // Texto en varias lineas, partido por palabras para no pasar de max_width. Devuelve la y de
  // debajo de la ultima linea.
  float WrappedText(TextStyle style, float x, float y, float scale, std::string_view text, float max_width,
                    float line_height) const {
    auto width_of = [&](std::string_view t) {
      if (!tex) {
        const float size = (style == TextStyle::kTitle ? 40.0f : 26.0f) * scale * s;
        return fallback_font->CalcTextSizeA(size, FLT_MAX, 0.0f, t.data(), t.data() + t.size()).x / sx;
      }
      return TextWidth(style == TextStyle::kTitle ? tex->title : tex->text, t, scale);
    };
    while (!text.empty()) {
      size_t end = text.size();
      if (width_of(text) > max_width) {
        // La ultima palabra que cabe entera; si ni la primera cabe, va sola (Text la estrecha).
        size_t fit = text.find(' ');
        for (size_t space = fit; space != std::string_view::npos; space = text.find(' ', space + 1)) {
          if (width_of(text.substr(0, space)) > max_width) break;
          fit = space;
        }
        if (fit != std::string_view::npos) end = fit;
      }
      Text(style, x, y, scale, text.substr(0, end), 0.0f, max_width);
      y += line_height;
      text.remove_prefix(end);
      while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    }
    return y;
  }

  // Placa en relieve: etiquetas y valores no elegidos.
  void RaisedPlate(float x0, float y0, float x1, float y1) const {
    Metal(x0, y0, x1, y1);
    Fill(x0, y0, x1, y1, IM_COL32(255, 255, 255, 20));
    Fill(x0, y0, x1, y0 + 1.0f, Rgb(172, 176, 172));
    Fill(x0, y0, x0 + 1.0f, y1, Rgb(153, 157, 153));
    Fill(x0, y1 - 1.5f, x1, y1, Rgb(100, 99, 100));
  }

  // Celda hundida: el valor elegido en una fila sin el cursor.
  void InsetPlate(float x0, float y0, float x1, float y1) const {
    Metal(x0, y0, x1, y1);
    Fill(x0, y0, x1, y1, Black(19));
    Fill(x0, y0, x1, y0 + 1.0f, Rgb(46, 46, 46));
    Fill(x0, y0 + 1.0f, x1, y0 + 2.0f, Rgb(68, 68, 68));
    Fill(x0, y0, x0 + 1.0f, y1, Rgb(55, 55, 55));
    Fill(x0 + 1.0f, y0 + 2.0f, x0 + 2.0f, y1, Rgb(70, 70, 70));
    Fill(x1 - 1.3f, y0, x1, y1, Rgb(80, 80, 80));
  }

  // Placa clara con sombra (fila del cursor). x1/y1 incluyen la sombra.
  void SelectedPlate(float x0, float y0, float x1, float y1) const {
    const float bx1 = x1 - 3.3f, by1 = y1 - 4.3f;
    Metal(x0, y0 + 1.0f, bx1, by1);
    Fill(x0, y0 + 1.0f, bx1, by1, IM_COL32(255, 255, 255, 128));
    Fill(x0, y0 + 1.0f, bx1, y0 + 2.0f, Rgb(194, 198, 194));
    Fill(x0, y0 + 2.0f, bx1, y0 + 2.7f, Rgb(186, 188, 186));
    Fill(x0, y0 + 1.0f, x0 + 1.0f, by1, Rgb(150, 152, 153));
    HGradient(bx1 - 3.0f, y0 + 2.0f, bx1, by1, Rgb(176, 176, 176, 0), Rgb(149, 150, 149));
    VGradient(x0, by1 - 2.7f, bx1, by1, Rgb(171, 170, 171), Rgb(128, 126, 128));
    Fill(x0, by1, bx1, by1 + 2.0f, Black(172));
    VGradient(x0, by1 + 2.0f, bx1, y1, Black(150), Black(0));
    HGradient(bx1, y0 + 2.0f, x1, by1 + 2.0f, Black(130), Black(0));
  }
};

namespace {

// Esqueleto de la pantalla: metal, cabecera con la esquina del panel izquierdo,
// sombra del panel, surco derecho, franja inferior y caja de ayuda.
void DrawFrame(const SettingsPainter& p) {
  p.Metal(p.left, p.top, p.right, p.bottom);
  // Separador vertical de la cabecera.
  p.Fill(361.0f, p.top, 362.0f, 93.0f, Rgb(51, 51, 51));
  p.Fill(362.7f, p.top, 364.3f, 93.0f, Rgb(153, 153, 153));
  // Borde inferior de la cabecera.
  p.Fill(p.left, 93.0f, p.right, 94.0f, Rgb(82, 83, 82));
  p.Fill(p.left, 94.0f, p.right, 96.0f, Rgb(58, 61, 58));
  p.Fill(p.left, 96.0f, p.right, 97.0f, Rgb(30, 31, 30));
  p.Fill(p.left, 97.0f, p.right, 104.0f, Rgb(2, 3, 3));
  p.Fill(p.left, 104.0f, p.right, 106.0f, Rgb(157, 158, 157));
  p.Fill(p.left, 106.0f, p.right, 107.0f, Rgb(146, 147, 146));
  p.Fill(p.left, 107.0f, p.right, 107.7f, Rgb(109, 110, 109));
  // Borde superior del panel izquierdo y su esquina curva.
  p.Fill(p.left, 118.0f, 277.0f, 119.0f, Rgb(85, 86, 85));
  p.Fill(p.left, 119.0f, 277.0f, 122.0f, Rgb(152, 154, 152));
  if (p.tex) p.Sprite(p.tex->ui_main.get(), 410, 4, 89, 32, 277.0f, 98.0f, 89.0f, 32.0f, kMetalTint);
  // Sombra del panel izquierdo sobre el central.
  p.Fill(366.0f, 107.0f, 367.0f, 639.0f, Black(130));
  p.Fill(367.0f, 107.0f, 369.0f, 639.0f, Black(202));
  p.HGradient(369.0f, 107.0f, 375.0f, 639.0f, Black(166), Black(0));
  // Surco entre el panel central y la columna derecha.
  p.Fill(1090.0f, 107.0f, 1091.3f, 635.0f, Rgb(50, 50, 50));
  p.Fill(1091.3f, 107.0f, 1092.0f, 635.0f, Rgb(98, 98, 98));
  p.Fill(1092.0f, 107.0f, 1093.3f, 635.0f, Rgb(150, 150, 150));
  p.Fill(1093.3f, 107.0f, 1094.0f, 635.0f, Rgb(127, 127, 127));
  // Franja inferior con sombra hacia la barra de ayuda.
  p.Fill(p.left, 634.7f, p.right, 635.3f, Rgb(80, 81, 80));
  p.Fill(p.left, 635.3f, p.right, 638.0f, Rgb(58, 62, 58));
  p.Fill(p.left, 638.0f, p.right, 638.7f, Rgb(36, 37, 36));
  p.Fill(p.left, 638.7f, p.right, 640.7f, Rgb(6, 6, 6));
  p.VGradient(p.left, 640.7f, p.right, 650.0f, Black(212), Black(0));
  // Caja de ayuda hundida.
  p.Fill(114.0f, 650.0f, 1210.0f, 692.0f, Rgb(67, 67, 67));
  p.Fill(114.0f, 650.0f, 1210.0f, 650.7f, Rgb(30, 30, 30));
  p.Fill(114.0f, 650.7f, 1210.0f, 651.3f, Rgb(44, 44, 44));
  p.Fill(114.0f, 651.3f, 1210.0f, 652.7f, Rgb(57, 58, 57));
  p.Fill(114.0f, 650.0f, 114.7f, 692.0f, Rgb(32, 32, 32));
  p.Fill(114.7f, 651.0f, 115.3f, 692.0f, Rgb(45, 46, 45));
  p.Fill(115.3f, 651.0f, 117.3f, 692.0f, Rgb(59, 59, 59));
  p.Fill(1208.7f, 651.0f, 1210.0f, 692.0f, Rgb(59, 59, 59));
  // Titulo.
  if (p.tex) p.Sprite(p.tex->ui_main.get(), 373, 777, 41, 41, 82.0f, 43.0f, 42.0f, 42.0f);
  p.Text(TextStyle::kTitle, 128.6f, 43.3f, kTitleScale, p.L("Configuración", true));
}

// Pestanas sobre las filas, en el hueco vacio del panel central. Tambien se
// pintan sobre la pagina nativa para que se vea que hay mas.
void DrawTabs(const SettingsPainter& p, int page) {
  // Hueco entre la cabecera (107) y la primera fila (150): pestanas finas y
  // pegadas arriba para dejar aire antes de las filas.
  constexpr float y0 = 109.0f, y1 = 138.0f, x0 = 430.0f, x1 = 981.0f;
  constexpr float kTabTextScale = 0.8f;
  const float pitch = (x1 - x0 + kCellGap) / float(kPageCount);
  p.Text(TextStyle::kNormal, 386.0f, y0 + 4.0f, 0.72f, "LB", 38.0f);
  p.Text(TextStyle::kNormal, 987.0f, y0 + 4.0f, 0.72f, "RB", 38.0f);
  for (int i = 0; i < kPageCount; ++i) {
    const float cx0 = x0 + float(i) * pitch, cx1 = cx0 + pitch - kCellGap;
    if (i == page) {
      p.InsetPlate(cx0, y0, cx1, y1);
    } else {
      p.RaisedPlate(cx0, y0, cx1, y1);
    }
    p.Text(i == page ? TextStyle::kNormal : TextStyle::kInactive, cx0, y0 + 3.0f, kTabTextScale, p.L(kPageNames[i]),
           cx1 - cx0, cx1 - cx0 - 8.0f);
  }
}

// Pantalla "Preparando sombreadores" del arranque: mientras el plugin traduce la
// cache y compila los pipelines guardados el juego no dibuja nada, asi que se
// pinta una ventana del propio juego (metal, fuentes LocTit1 y Maru23) con una
// barra de progreso.
void DrawShaderPrep(const SettingsPainter& p, int phase, uint32_t done, uint32_t total, int64_t now_ms) {
  p.Fill(p.left, p.top, p.right, p.bottom, Rgb(0, 0, 0));
  // Generacion completa de pipelines del primer arranque: se puede jugar ya y
  // dejarla en segundo plano (A o Start, ver turbo_hooks.cpp).
  const bool can_skip = phase == 2 && QueryPrewarmCanSkip();
  constexpr float x0 = 250.0f, x1 = 1030.0f, y0 = 255.0f;
  const int disc_count = std::max(1, PrecacheDiscsAvailable());
  const bool show_discs = disc_count > 1 || PrecacheGeneratingDisc() > 0;
  const float y1 = (can_skip ? 495.0f : 465.0f) + (show_discs ? 52.0f : 0.0f);
  // Sombra y panel en relieve.
  p.VGradient(x0 + 6.0f, y1, x1 + 6.0f, y1 + 14.0f, Black(160), Black(0));
  p.HGradient(x1, y0 + 6.0f, x1 + 10.0f, y1 + 6.0f, Black(150), Black(0));
  p.RaisedPlate(x0, y0, x1, y1);
  // Cabecera con el icono del titulo, como la de la configuracion.
  p.Fill(x0, y0 + 64.0f, x1, y0 + 65.0f, Rgb(82, 83, 82));
  p.Fill(x0, y0 + 65.0f, x1, y0 + 67.0f, Rgb(30, 31, 30));
  p.Fill(x0, y0 + 67.0f, x1, y0 + 68.0f, Rgb(150, 152, 150));
  if (p.tex) p.Sprite(p.tex->ui_main.get(), 373, 777, 41, 41, x0 + 22.0f, y0 + 12.0f, 42.0f, 42.0f);
  p.Text(TextStyle::kTitle, x0 + 70.0f, y0 + 12.0f, kTitleScale, p.L("Preparando sombreadores", true));

  // Fase y cuenta (con el disco, en la precarga de la generacion completa).
  const char* label = phase == 1 ? "Traduciendo sombreadores" : "Creando pipelines";
  char label_text[160];
  if (const int disc = PrecacheGeneratingDisc(); disc > 0 && QueryPrewarmBulkRunning()) {
    std::snprintf(label_text, sizeof(label_text), p.L("%s (disco %d)"), p.L(label), disc);
  } else {
    std::snprintf(label_text, sizeof(label_text), "%s", p.L(label));
  }
  p.Text(TextStyle::kNormal, x0 + 30.0f, y0 + 84.0f, kTextScale, label_text, 0.0f, x1 - x0 - 290.0f);
  char count[48];
  if (total > 0) {
    std::snprintf(count, sizeof(count), "%u / %u", std::min(done, total), total);
  } else {
    std::snprintf(count, sizeof(count), "%u", done);
  }
  const float count_w = 220.0f;
  p.Text(TextStyle::kNormal, x1 - 30.0f - count_w, y0 + 84.0f, kTextScale, count, count_w);

  // Barra: surco hundido y relleno de metal claro; sin total, un tramo que va y
  // viene para que se vea que sigue trabajando.
  constexpr float bx0 = x0 + 30.0f, bx1 = x1 - 30.0f, by0 = y0 + 124.0f, by1 = y0 + 146.0f;
  p.InsetPlate(bx0, by0, bx1, by1);
  if (total > 0) {
    const float f = std::min(1.0f, float(done) / float(total));
    if (f > 0.0f) {
      const float fx1 = bx0 + 3.0f + (bx1 - bx0 - 6.0f) * f;
      p.Metal(bx0 + 3.0f, by0 + 3.0f, fx1, by1 - 3.0f);
      p.Fill(bx0 + 3.0f, by0 + 3.0f, fx1, by1 - 3.0f, IM_COL32(255, 255, 255, 110));
      p.Fill(bx0 + 3.0f, by0 + 3.0f, fx1, by0 + 4.0f, Rgb(200, 202, 200));
    }
  } else {
    const float span = 160.0f, travel = bx1 - bx0 - 6.0f - span;
    const float t = float(now_ms % 1600) / 1600.0f;
    const float pos = bx0 + 3.0f + travel * (t < 0.5f ? t * 2.0f : 2.0f - t * 2.0f);
    p.Metal(pos, by0 + 3.0f, pos + span, by1 - 3.0f);
    p.Fill(pos, by0 + 3.0f, pos + span, by1 - 3.0f, IM_COL32(255, 255, 255, 110));
  }

  p.Text(TextStyle::kInactive, x0 + 30.0f, y0 + 160.0f, kTextScale * 0.85f,
         p.L("Solo ocurre la primera vez y al cambiar la resolución."), 0.0f, x1 - x0 - 60.0f);
  if (can_skip) {
    p.Text(TextStyle::kNormal, x0 + 30.0f, y0 + 190.0f, kTextScale * 0.85f,
           p.L("Pulsa (A), Start o Intro para jugar ya: el resto se prepara mientras juegas."), 0.0f,
           x1 - x0 - 60.0f);
  }
  // Un tick por disco cuando su generacion esta hecha; el que se esta preparando, resaltado.
  if (show_discs) {
    const float ry = y1 - 46.0f;
    const int current = PrecacheGeneratingDisc();
    for (int d = 1; d <= std::max(disc_count, 4); ++d) {
      if (d > disc_count && !QueryDiscPrepared(d)) continue;
      const float cx = x0 + 30.0f + float(d - 1) * 170.0f;
      const bool done = QueryDiscPrepared(d);
      p.InsetPlate(cx, ry + 4.0f, cx + 24.0f, ry + 28.0f);
      if (done) {
        const ImU32 white = Rgb(235, 238, 235);
        p.dl->AddLine(p.Snap(cx + 5.0f, ry + 16.0f), p.Snap(cx + 10.0f, ry + 22.0f), white, 3.0f * p.s);
        p.dl->AddLine(p.Snap(cx + 10.0f, ry + 22.0f), p.Snap(cx + 20.0f, ry + 9.0f), white, 3.0f * p.s);
      }
      char label[32];
      std::snprintf(label, sizeof(label), p.L("Disco %d"), d);
      p.Text(d == current && !done ? TextStyle::kSelected : (done ? TextStyle::kNormal : TextStyle::kInactive),
             cx + 34.0f, ry + 2.0f, kTextScale * 0.8f, label);
    }
  }
}

// Primera vez: que sombreadores precargar (todos los discos o la parte actual). Arriba/abajo y A,
// Start o Intro (turbo_hooks.cpp, shader_prep.cpp).
void DrawPrecacheChoice(const SettingsPainter& p) {
  p.Fill(p.left, p.top, p.right, p.bottom, Rgb(0, 0, 0));
  constexpr float x0 = 190.0f, x1 = 1090.0f, y0 = 150.0f, y1 = 580.0f;
  p.VGradient(x0 + 6.0f, y1, x1 + 6.0f, y1 + 14.0f, Black(160), Black(0));
  p.HGradient(x1, y0 + 6.0f, x1 + 10.0f, y1 + 6.0f, Black(150), Black(0));
  p.RaisedPlate(x0, y0, x1, y1);
  p.Fill(x0, y0 + 64.0f, x1, y0 + 65.0f, Rgb(82, 83, 82));
  p.Fill(x0, y0 + 65.0f, x1, y0 + 67.0f, Rgb(30, 31, 30));
  p.Fill(x0, y0 + 67.0f, x1, y0 + 68.0f, Rgb(150, 152, 150));
  if (p.tex) p.Sprite(p.tex->ui_main.get(), 373, 777, 41, 41, x0 + 22.0f, y0 + 12.0f, 42.0f, 42.0f);
  p.Text(TextStyle::kTitle, x0 + 70.0f, y0 + 12.0f, kTitleScale, p.L("Precarga de sombreadores", true));

  constexpr float tx = x0 + 30.0f, tw = x1 - x0 - 60.0f, line = 30.0f;
  p.WrappedText(TextStyle::kNormal, tx, y0 + 84.0f, kTextScale * 0.85f,
                p.L("Precargar los sombreadores evita tirones y bajones de FPS durante la partida."), tw, line);

  const int discs = PrecacheDiscsAvailable();
  const int current = std::max(1, PrecacheCurrentDisc());
  char all_text[160], part_text[160];
  if (discs == 1) {
    std::snprintf(all_text, sizeof(all_text), "%s", p.L("Precargar todos los sombreadores (1 disco encontrado)"));
  } else {
    std::snprintf(all_text, sizeof(all_text), p.L("Precargar todos los sombreadores (%d discos)"), discs);
  }
  std::snprintf(part_text, sizeof(part_text),
                current == 1 ? p.L("Precargar los de la primera parte (disco %d)")
                             : p.L("Precargar los de esta parte (disco %d)"),
                current);
  const char* options[2] = {all_text, part_text};
  const char* option_help[2] = {
      p.L("Todo de una vez, unos minutos por disco. Después no hay que esperar más."),
      p.L("Más rápido ahora. Las partes siguientes se precargan al llegar a ellas."),
  };
  const int selected = int(PrecacheChoiceSelected());
  for (int i = 0; i < 2; ++i) {
    const float top = y0 + 130.0f + float(i) * 66.0f, bottom = top + 52.0f;
    if (i == selected) {
      p.SelectedPlate(x0 + 24.0f, top, x1 - 20.0f, bottom);
      if (p.tex) p.Sprite(p.tex->window.get(), 6, 126, 40, 23, x0 + 30.0f, top + 14.0f, 40.0f, 23.0f);
    } else {
      p.RaisedPlate(x0 + 24.0f, top, x1 - 24.0f, bottom - 4.0f);
    }
    p.Text(i == selected ? TextStyle::kSelected : TextStyle::kNormal, x0 + 80.0f, top + 10.0f, kTextScale,
           options[i], 0.0f, x1 - x0 - 120.0f);
  }
  float y = p.WrappedText(TextStyle::kNormal, tx, y0 + 268.0f, kTextScale * 0.85f, option_help[selected], tw, line);
  y = p.WrappedText(TextStyle::kInactive, tx, y + 8.0f, kTextScale * 0.8f,
                    p.L("Durante la precarga puedes pulsar (A), Start o Intro para jugar ya: seguirá en segundo "
                        "plano, aunque puede haber tirones y bajones hasta que acabe."),
                    tw, line);
  p.WrappedText(TextStyle::kInactive, tx, std::max(y + 8.0f, y1 - 46.0f), kTextScale * 0.8f,
                p.L("Arriba/abajo para elegir; (A), Start o Intro para aceptar. Se puede cambiar en Config > Extras."),
                tw, line);
}

// --- Asistente de instalacion ------------------------------------------------------------------
// Mismo panel de metal que la precarga. Ratón (clic) y teclado (Intro = siguiente, Esc = atras).
struct WizardMouse {
  float x = -1.0f, y = -1.0f;  // coordenadas logicas (1280x720)
  bool clicked = false;
  bool In(float x0, float y0, float x1, float y1) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

// Boton con el aspecto de las placas del menu. Devuelve true si se ha pulsado.
bool WizardButton(const SettingsPainter& p, const WizardMouse& m, float x0, float y0, float x1, float y1,
                  const char* label, bool enabled) {
  const bool hover = enabled && m.In(x0, y0, x1, y1);
  if (hover) {
    p.SelectedPlate(x0, y0, x1 + 3.3f, y1 + 4.3f);
  } else {
    p.RaisedPlate(x0, y0, x1, y1 - 4.0f);
  }
  p.Text(!enabled ? TextStyle::kDisabled : (hover ? TextStyle::kSelected : TextStyle::kNormal), x0, y0 + 9.0f,
         kTextScale * 0.9f, label, x1 - x0, x1 - x0 - 16.0f);
  return hover && m.clicked;
}

const char* DiscStatusText(const SettingsPainter& p, DiscStatus st) {
  switch (st) {
    case DiscStatus::kOk: return p.L("Verificado");
    case DiscStatus::kEmpty: return p.L("Sin añadir");
    case DiscStatus::kWrongEdition: return p.L("Otra edición");
    case DiscStatus::kModified: return p.L("Modificado o dañado");
    default: return p.L("No válido");
  }
}

std::string ShortPath(const std::filesystem::path& path, size_t max_chars) {
  const auto u8 = path.u8string();
  std::string s(u8.begin(), u8.end());
  if (s.size() > max_chars) s = "…" + s.substr(s.size() - (max_chars - 1));
  return s;
}

// Aviso discreto arriba al centro mientras se descarga el pack de texturas HD.
void DrawDownloadBadge(const SettingsPainter& p) {
  const std::string text = TextureDownloadText();
  if (text.empty()) return;
  constexpr float x0 = 340.0f, x1 = 940.0f, y0 = 8.0f, y1 = 46.0f;
  p.RaisedPlate(x0, y0, x1, y1);
  p.Text(TextStyle::kNormal, x0, y0 + 6.0f, kTextScale * 0.8f, text, x1 - x0, x1 - x0 - 24.0f);
}

// --- Ajustes iniciales del asistente (idioma, resolucion...) ------------------------------------
struct WizardSettings {
  Options* edit = nullptr;
  const Options* running = nullptr;
  std::filesystem::path config_path;
  int* cursor = nullptr;
};

// Las opciones que se piden antes del primer arranque, de las paginas del menu.
const std::vector<const Row*>& WizardRows() {
  static const std::vector<const Row*> rows = [] {
    std::vector<const Row*> v;
    auto add = [&](int page, const char* label) {
      for (const Row& r : PageRows(page)) {
        if (std::string_view(r.label) == label) v.push_back(&r);
      }
    };
    add(3, "Idioma");
    add(1, "Resolución");
    add(1, "Escala 3D");
    add(1, "Pantalla completa");
    add(1, "Sincronización vertical");
    add(1, "Antialiasing");
    add(1, "DLSS");
    add(1, "API gráfica");
    return v;
  }();
  return rows;
}

void WizardChange(WizardSettings& ws, const Row& row, int value) {
  row.set(*ws.edit, value);
  WriteToml(ws.config_path, *ws.edit);
  ApplyHot(*ws.edit);
}

void DrawWizardSettings(const SettingsPainter& p, const WizardMouse& m, WizardSettings& ws, float tx, float top0,
                        float tw, float right) {
  const auto& rows = WizardRows();
  const int count = int(rows.size());
  if (count == 0) return;
  int& cursor = *ws.cursor;
  cursor = std::clamp(cursor, 0, count - 1);
  auto offered = [&](const Row& r, int k) { return !r.choice_enabled || r.choice_enabled(*ws.edit, k); };
  auto usable = [&](const Row& r) { return !(r.disabled && r.disabled(*ws.edit)); };

  // Teclado: arriba/abajo cambia de fila; izquierda/derecha, de valor.
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) cursor = (cursor + count - 1) % count;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) cursor = (cursor + 1) % count;
  {
    const Row& row = *rows[cursor];
    const int choices = int(row.choices.size());
    const int value = std::clamp(row.get(*ws.edit), 0, choices - 1);
    int next = value;
    if (usable(row)) {
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        for (int k = value - 1; k >= 0; --k) {
          if (offered(row, k)) {
            next = k;
            break;
          }
        }
      }
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        for (int k = value + 1; k < choices; ++k) {
          if (offered(row, k)) {
            next = k;
            break;
          }
        }
      }
      if (next != value) WizardChange(ws, row, next);
    }
  }

  constexpr float rh = 32.0f, label_w = 270.0f;
  const float label_x1 = tx + label_w, cx0 = tx + label_w + 20.0f, cx1 = right;
  const float scale = kTextScale * 0.78f, dy = 4.0f;
  for (int i = 0; i < count; ++i) {
    const Row& row = *rows[i];
    const float top = top0 + float(i) * rh, bottom = top + rh - 3.0f;
    const bool selected = i == cursor, enabled = usable(row);
    if (m.clicked && m.In(tx, top, cx1, bottom)) cursor = i;
    if (selected) {
      p.SelectedPlate(tx, top, label_x1 + 2.0f, bottom);
      if (p.tex) p.Sprite(p.tex->window.get(), 6, 126, 40, 23, tx - 36.0f, top + 5.0f, 32.0f, 18.0f);
    } else {
      p.RaisedPlate(tx, top, label_x1, bottom);
    }
    p.Text(selected ? TextStyle::kSelected : (enabled ? TextStyle::kNormal : TextStyle::kDisabled), tx + 14.0f,
           top + dy, scale, p.L(row.label), 0.0f, label_w - 28.0f);

    const int choices = int(row.choices.size());
    const int value = std::clamp(row.get(*ws.edit), 0, choices - 1);
    if (choices >= kSliderMinChoices) {
      const float vx0 = cx0, vx1 = cx0 + 250.0f;
      if (selected) {
        p.SelectedPlate(vx0 - 1.0f, top, vx1, bottom);
      } else {
        p.InsetPlate(vx0, top, vx1, bottom);
      }
      const std::string label =
          row.choice_label ? row.choice_label(*ws.edit, value) : std::string(p.L(row.choices[value]));
      p.Text(!enabled ? TextStyle::kDisabled : (selected ? TextStyle::kSelected : TextStyle::kNormal), vx0, top + dy,
             scale, label, vx1 - vx0, vx1 - vx0 - 12.0f);
      const float bx0 = vx1 + 20.0f, bx1 = cx1 - 4.0f, by = (top + bottom) * 0.5f;
      p.InsetPlate(bx0, by - 5.0f, bx1, by + 5.0f);
      const float f = choices > 1 ? float(value) / float(choices - 1) : 0.0f;
      const float kx = bx0 + 3.0f + (bx1 - bx0 - 6.0f) * f;
      if (kx > bx0 + 3.0f) {
        p.Metal(bx0 + 3.0f, by - 2.0f, kx, by + 3.0f);
        p.Fill(bx0 + 3.0f, by - 2.0f, kx, by + 3.0f, IM_COL32(255, 255, 255, 90));
      }
      p.RaisedPlate(kx - 6.0f, by - 10.0f, kx + 6.0f, by + 10.0f);
      if (enabled && m.clicked && m.In(bx0 - 8.0f, top, bx1 + 8.0f, bottom)) {
        const int k = std::clamp(int(std::lround((m.x - bx0) / (bx1 - bx0) * float(choices - 1))), 0, choices - 1);
        if (k != value && offered(row, k)) WizardChange(ws, row, k);
        cursor = i;
      }
      continue;
    }
    const float gap = 6.0f, pitch = (cx1 - cx0 + gap) / float(choices);
    for (int k = 0; k < choices; ++k) {
      const float x0 = cx0 + float(k) * pitch, x1 = x0 + pitch - gap;
      const bool chosen = k == value;
      TextStyle style = TextStyle::kInactive;
      if (chosen && selected) {
        p.SelectedPlate(x0 - 1.0f, top, x1, bottom);
        style = TextStyle::kSelected;
      } else if (chosen) {
        p.InsetPlate(x0, top, x1, bottom);
        style = TextStyle::kNormal;
      } else {
        p.RaisedPlate(x0, top, x1, bottom);
      }
      if (!enabled || !offered(row, k)) style = TextStyle::kDisabled;
      p.Text(style, x0, top + dy, scale, p.L(row.choices[k]), x1 - x0, x1 - x0 - 10.0f);
      if (m.clicked && enabled && offered(row, k) && m.In(x0, top, x1, bottom)) {
        if (k != value) WizardChange(ws, row, k);
        cursor = i;
      }
    }
  }

  // Ayuda de la fila elegida y aviso de reinicio.
  const Row& cur = *rows[cursor];
  const int value = std::clamp(cur.get(*ws.edit), 0, int(cur.choices.size()) - 1);
  std::string help;
  if (cur.help_fn) {
    help = cur.help_fn(*ws.edit);
  } else {
    help = p.L(value < int(cur.choice_help.size()) ? cur.choice_help[value] : cur.help);
  }
  if (!usable(cur) && cur.disabled_help) help = p.L(cur.disabled_help);
  float y = top0 + float(count) * rh + 6.0f;
  y = p.WrappedText(TextStyle::kInactive, tx, y, kTextScale * 0.75f, help, tw, 24.0f);
  if (ws.edit->NeedsRestartFrom(*ws.running)) {
    p.WrappedText(TextStyle::kNormal, tx, y + 2.0f, kTextScale * 0.75f,
                  p.L("Al pulsar Jugar, el juego se reiniciará una vez para aplicar estos ajustes."), tw, 24.0f);
  }
}

// La fuente de reserva de imgui no trae los puntos suspensivos.
std::string PlainEllipsis(std::string text) {
  for (size_t at; (at = text.find("â¦")) != std::string::npos;) text.replace(at, 3, "...");
  return text;
}

// Primera pantalla mientras no hay fuente del juego (hace falta un disco para leerla: el juego no se incluye).
void DrawPrestart(const SettingsPainter& p, Installer& inst, const WizardMouse& m) {
  constexpr float x0 = 240.0f, x1 = 1040.0f, y0 = 170.0f, y1 = 550.0f;
  p.VGradient(x0 + 6.0f, y1, x1 + 6.0f, y1 + 14.0f, Black(160), Black(0));
  p.HGradient(x1, y0 + 6.0f, x1 + 10.0f, y1 + 6.0f, Black(150), Black(0));
  p.RaisedPlate(x0, y0, x1, y1);
  p.Fill(x0, y0 + 64.0f, x1, y0 + 65.0f, Rgb(82, 83, 82));
  p.Fill(x0, y0 + 65.0f, x1, y0 + 67.0f, Rgb(30, 31, 30));
  p.Fill(x0, y0 + 67.0f, x1, y0 + 68.0f, Rgb(150, 152, 150));
  p.Text(TextStyle::kTitle, x0 + 30.0f, y0 + 14.0f, kTitleScale, "Lost Odyssey HD Remaster");
  constexpr float tx = x0 + 30.0f, tw = x1 - x0 - 60.0f, line = 28.0f;
  float y = p.WrappedText(TextStyle::kNormal, tx, y0 + 90.0f, kTextScale * 0.85f,
                          Tr("Para empezar, indica dónde está el disco 1 de tu copia del juego: una carpeta extraída, "
                             "una imagen .iso o un paquete GOD (edición USA/Europa)."),
                          tw, line);
  if (inst.Disc1Ready()) {
    p.WrappedText(TextStyle::kNormal, tx, y + 24.0f, kTextScale * 0.85f, PlainEllipsis(Tr("Disco 1 verificado. Preparando el asistente…")),
                  tw, line);
    return;
  }
  const float by = y + 24.0f;
  if (WizardButton(p, m, tx, by, tx + 220.0f, by + 40.0f, PlainEllipsis(Tr("Añadir archivo…")).c_str(), true)) inst.BrowseFile();
  if (WizardButton(p, m, tx + 236.0f, by, tx + 456.0f, by + 40.0f, PlainEllipsis(Tr("Añadir carpeta…")).c_str(), true)) inst.BrowseFolder();
  if (WizardButton(p, m, tx + 472.0f, by, tx + 692.0f, by + 40.0f,
                   inst.scanning() ? PlainEllipsis(Tr("Buscando…")).c_str() : Tr("Buscar discos"), !inst.scanning())) {
    inst.StartScan();
  }
  if (!inst.message().empty()) {
    p.WrappedText(TextStyle::kInactive, tx, by + 56.0f, kTextScale * 0.8f, inst.message(), tw, line);
  }
}

void DrawInstaller(const SettingsPainter& p, Installer& inst, ImGuiIO& io, int64_t now_ms, WizardSettings& ws,
                   bool font_pending) {
  p.Fill(p.left, p.top, p.right, p.bottom, Rgb(0, 0, 0));
  WizardMouse m;
  m.x = (io.MousePos.x - p.ox) / p.sx;
  m.y = (io.MousePos.y - p.oy) / p.s;
  m.clicked = ImGui::IsMouseClicked(0);
  // Aun no se ha leido la fuente del juego: primero el disco 1 (con el, todo el asistente va con la fuente del juego).
  if (font_pending && inst.step() == InstallStep::kWelcome) {
    DrawPrestart(p, inst, m);
    return;
  }

  constexpr float x0 = 140.0f, x1 = 1140.0f, y0 = 80.0f, y1 = 640.0f;
  p.VGradient(x0 + 6.0f, y1, x1 + 6.0f, y1 + 14.0f, Black(160), Black(0));
  p.HGradient(x1, y0 + 6.0f, x1 + 10.0f, y1 + 6.0f, Black(150), Black(0));
  p.RaisedPlate(x0, y0, x1, y1);
  p.Fill(x0, y0 + 64.0f, x1, y0 + 65.0f, Rgb(82, 83, 82));
  p.Fill(x0, y0 + 65.0f, x1, y0 + 67.0f, Rgb(30, 31, 30));
  p.Fill(x0, y0 + 67.0f, x1, y0 + 68.0f, Rgb(150, 152, 150));
  if (p.tex) p.Sprite(p.tex->ui_main.get(), 373, 777, 41, 41, x0 + 22.0f, y0 + 12.0f, 42.0f, 42.0f);
  p.Text(TextStyle::kTitle, x0 + 70.0f, y0 + 12.0f, kTitleScale, p.L("Instalación de Lost Odyssey", true));
  char step_text[64];
  std::snprintf(step_text, sizeof(step_text), p.L("Paso %d de 7"), int(inst.step()) + 1);
  p.Text(TextStyle::kInactive, x1 - 250.0f, y0 + 24.0f, kTextScale * 0.8f, step_text, 220.0f);

  constexpr float tx = x0 + 34.0f, tw = x1 - x0 - 68.0f, line = 30.0f;
  const float body = y0 + 90.0f;
  const float fine = kTextScale * 0.85f;

  switch (inst.step()) {
    case InstallStep::kWelcome: {
      float y = p.WrappedText(TextStyle::kNormal, tx, body, kTextScale, p.L("Bienvenido. Este asistente prepara Lost Odyssey para jugar en PC con tus propios discos."), tw, line + 4.0f);
      y = p.WrappedText(TextStyle::kNormal, tx, y + 14.0f, fine, p.L("Necesitas tu copia del juego: el disco 1 es obligatorio y los otros tres puedes añadirlos ahora o más tarde. Se admiten carpetas extraídas, imágenes .iso y paquetes GOD de la edición USA/Europa. Se instalan en la carpeta del juego, o se leen donde estén, como prefieras."), tw, line);
      y = p.WrappedText(TextStyle::kInactive, tx, y + 14.0f, fine, p.L("Proyecto no oficial, sin relación con Microsoft, Mistwalker ni Square Enix. El juego no se incluye."), tw, line);
      y = p.WrappedText(TextStyle::kInactive, tx, y + 14.0f, fine, p.L("Versión 0.0.1: tiene muchos fallos. Si algo se cierra, el juego te ofrecerá avisarnos."), tw, line);
      {
        // Requisitos: tarjeta grafica y memoria.
        static const SystemInfo sys = QuerySystemInfo();
        std::string eq = std::string(p.L("Tu equipo: ")) + sys.gpu_primary + ", " + std::to_string(sys.ram_mb / 1024) + " GB RAM.";
        y = p.WrappedText(TextStyle::kInactive, tx, y + 14.0f, kTextScale * 0.75f, eq, tw, line - 4.0f);
        if (sys.vram_mb && sys.vram_mb < 3000) {
          y = p.WrappedText(TextStyle::kNormal, tx, y + 6.0f, fine, p.L("Aviso: tu tarjeta tiene poca memoria de vídeo (se recomiendan 4 GB o más)."), tw, line);
        }
        if (sys.ram_mb && sys.ram_mb < 7500) {
          p.WrappedText(TextStyle::kNormal, tx, y + 6.0f, fine, p.L("Aviso: se recomiendan 16 GB de RAM."), tw, line);
        }
      }
      break;
    }
    case InstallStep::kDiscs: {
      p.Text(TextStyle::kNormal, tx, body - 6.0f, kTextScale, p.L("Elige tus discos"));
      for (int n = 1; n <= 4; ++n) {
        const InstallDisc& d = inst.disc(n);
        const float ry = body + 36.0f + float(n - 1) * 50.0f;
        p.InsetPlate(tx, ry, x1 - 34.0f, ry + 40.0f);
        char label[32];
        std::snprintf(label, sizeof(label), p.L("Disco %d"), n);
        p.Text(TextStyle::kNormal, tx + 14.0f, ry + 6.0f, kTextScale * 0.9f, label);
        p.Text(d.status == DiscStatus::kOk ? TextStyle::kSelected : TextStyle::kInactive, tx + 150.0f, ry + 6.0f,
               kTextScale * 0.9f, DiscStatusText(p, d.status), 0.0f, 250.0f);
        if (d.status != DiscStatus::kEmpty) {
          p.Text(TextStyle::kInactive, tx + 400.0f, ry + 8.0f, kTextScale * 0.75f, ShortPath(d.path, 52), 0.0f, tw - 430.0f);
        }
      }
      const float by = body + 36.0f + 4.0f * 50.0f + 6.0f;
      if (WizardButton(p, m, tx, by, tx + 250.0f, by + 40.0f, p.L("Añadir archivo…"), true)) inst.BrowseFile();
      if (WizardButton(p, m, tx + 270.0f, by, tx + 520.0f, by + 40.0f, p.L("Añadir carpeta…"), true)) inst.BrowseFolder();
      if (WizardButton(p, m, tx + 540.0f, by, tx + 800.0f, by + 40.0f,
                       inst.scanning() ? p.L("Buscando…") : p.L("Buscar discos"), !inst.scanning())) {
        inst.StartScan();
      }
      float y = by + 52.0f;
      if (!inst.message().empty()) y = p.WrappedText(TextStyle::kNormal, tx, y, fine, inst.message(), tw, line);
      p.WrappedText(TextStyle::kInactive, tx, y, kTextScale * 0.8f, inst.Disc1Ready() ? p.L("Disco 1 listo. Puedes seguir; los demás discos se pueden añadir ahora o más tarde.") : p.L("El disco 1 es obligatorio."), tw, line);
      break;
    }
    case InstallStep::kInstall: {
      p.Text(TextStyle::kNormal, tx, body - 6.0f, kTextScale, p.L("Instalar los discos"));
      const bool running = inst.install_running();
      const bool copy_mode = inst.install_mode() == InstallMode::kCopy;
      float y = body + 34.0f;
      const char* options[2] = {p.L("Instalar en la carpeta del juego (recomendado)"),
                                p.L("Usar los discos donde están")};
      for (int i = 0; i < 2; ++i) {
        const float top = y + float(i) * 62.0f, bottom = top + 50.0f;
        const bool selected = (i == 0) == copy_mode;
        if (m.clicked && !running && m.In(tx, top, x1 - 34.0f, bottom)) {
          inst.SetInstallMode(i == 0 ? InstallMode::kCopy : InstallMode::kInPlace);
        }
        if (selected) {
          p.SelectedPlate(tx, top, x1 - 34.0f, bottom + 4.0f);
          if (p.tex) p.Sprite(p.tex->window.get(), 6, 126, 40, 23, tx + 6.0f, top + 12.0f, 40.0f, 23.0f);
        } else {
          p.RaisedPlate(tx, top, x1 - 34.0f, bottom - 4.0f);
        }
        p.Text(selected ? TextStyle::kSelected : TextStyle::kNormal, tx + 56.0f, top + 10.0f, kTextScale * 0.9f,
               options[i], 0.0f, tw - 90.0f);
      }
      y += 2.0f * 62.0f + 6.0f;
      if (!copy_mode) {
        p.WrappedText(TextStyle::kInactive, tx, y, kTextScale * 0.8f,
                      p.L("No se copia nada: los discos (carpeta, .iso o GOD) tienen que seguir en su sitio para jugar."), tw, line);
        break;
      }
      if (!running && !inst.install_done()) {
        y = p.WrappedText(TextStyle::kInactive, tx, y, kTextScale * 0.8f,
                          p.L("Copia los ficheros de tus discos a la carpeta \"data\" junto al juego (unos 17 GB en total: lo que es igual en varios discos se guarda una sola vez en data\\common). Después no necesitas los discos y puedes copiar la carpeta entera: queda como un juego de PC."), tw, line);
        if (WizardButton(p, m, tx, y + 8.0f, tx + 300.0f, y + 52.0f, p.L("Instalar ahora"), true)) inst.StartInstall();
        if (!inst.install_error().empty()) {
          p.WrappedText(TextStyle::kNormal, tx, y + 64.0f, fine, inst.install_error(), tw, line);
        }
        break;
      }
      if (inst.install_done()) {
        p.WrappedText(TextStyle::kSelected, tx, y, kTextScale, p.L("Instalación completada. Ya no necesitas los discos."), tw, line);
        break;
      }
      // Copiando: disco, fichero, barra y velocidad.
      const uint64_t done = inst.install_bytes_done(), total = std::max<uint64_t>(inst.install_bytes_total(), 1);
      char head[160];
      std::snprintf(head, sizeof(head), p.L("Copiando disco %d de %d"), inst.install_disc(), std::max(1, inst.install_discs_total()));
      p.Text(TextStyle::kNormal, tx, y, kTextScale * 0.9f, head);
      char count[96];
      std::snprintf(count, sizeof(count), "%.1f / %.1f GB   %.0f MB/s", double(done) / 1073741824.0,
                    double(total) / 1073741824.0, inst.install_speed() / 1048576.0);
      p.Text(TextStyle::kNormal, x1 - 34.0f - 360.0f, y, kTextScale * 0.9f, count, 360.0f);
      const float bx0 = tx, bx1 = x1 - 34.0f, by0 = y + 40.0f, by1 = y + 62.0f;
      p.InsetPlate(bx0, by0, bx1, by1);
      const float f = std::min(1.0f, float(done) / float(total));
      if (f > 0.0f) {
        const float fx1 = bx0 + 3.0f + (bx1 - bx0 - 6.0f) * f;
        p.Metal(bx0 + 3.0f, by0 + 3.0f, fx1, by1 - 3.0f);
        p.Fill(bx0 + 3.0f, by0 + 3.0f, fx1, by1 - 3.0f, IM_COL32(255, 255, 255, 110));
        p.Fill(bx0 + 3.0f, by0 + 3.0f, fx1, by0 + 4.0f, Rgb(200, 202, 200));
      }
      p.Text(TextStyle::kInactive, tx, by1 + 12.0f, kTextScale * 0.75f, inst.install_file(), 0.0f, tw);
      if (WizardButton(p, m, tx, by1 + 52.0f, tx + 260.0f, by1 + 96.0f, p.L("Cancelar"), true)) inst.CancelInstall();
      break;
    }
    case InstallStep::kTextures: {
      p.Text(TextStyle::kNormal, tx, body - 6.0f, kTextScale, p.L("Texturas en alta definición"));
      float y = p.WrappedText(TextStyle::kNormal, tx, body + 34.0f, fine, p.L("El pack HD mejora todas las texturas del juego. Va cifrado y solo funciona con tus discos. Pesa unos 28 GB."), tw, line);
      if (TexturePackInstalled()) {
        p.WrappedText(TextStyle::kSelected, tx, y + 16.0f, kTextScale, p.L("El pack de texturas HD ya está instalado."), tw, line);
        break;
      }
      const char* options[2] = {p.L("Descargar ahora (continúa mientras juegas y se reanuda si se corta)"),
                                p.L("Más tarde (podrás descargarlo en Configuración > Texturas)")};
      for (int i = 0; i < 2; ++i) {
        const float top = y + 16.0f + float(i) * 62.0f, bottom = top + 50.0f;
        const bool selected = (i == 0) == (inst.texture_choice() == TextureChoice::kNow);
        if (m.clicked && m.In(tx, top, x1 - 34.0f, bottom)) inst.SetTextureChoice(i == 0 ? TextureChoice::kNow : TextureChoice::kLater);
        if (selected) {
          p.SelectedPlate(tx, top, x1 - 34.0f, bottom + 4.0f);
          if (p.tex) p.Sprite(p.tex->window.get(), 6, 126, 40, 23, tx + 6.0f, top + 12.0f, 40.0f, 23.0f);
        } else {
          p.RaisedPlate(tx, top, x1 - 34.0f, bottom - 4.0f);
        }
        p.Text(selected ? TextStyle::kSelected : TextStyle::kNormal, tx + 56.0f, top + 10.0f, kTextScale * 0.9f, options[i], 0.0f, tw - 90.0f);
      }
      p.WrappedText(TextStyle::kInactive, tx, y + 16.0f + 2.0f * 62.0f + 10.0f, kTextScale * 0.8f, p.L("Mientras no esté instalado, el juego se ve con las texturas originales."), tw, line);
      break;
    }
    case InstallStep::kSettings: {
      p.Text(TextStyle::kNormal, tx, body - 6.0f, kTextScale, p.L("Ajustes iniciales"));
      p.WrappedText(TextStyle::kInactive, tx, body + 24.0f, kTextScale * 0.75f,
                    p.L("Elige idioma y gráficos para el primer arranque (se cambian luego con F2)."), tw, 22.0f);
      DrawWizardSettings(p, m, ws, tx + 40.0f, body + 58.0f, tw - 40.0f, x1 - 34.0f);
      break;
    }
    case InstallStep::kShaders: {
      p.Text(TextStyle::kNormal, tx, body - 6.0f, kTextScale, p.L("Preparar sombreadores"));
      float y = p.WrappedText(TextStyle::kNormal, tx, body + 34.0f, fine, p.L("Se hace una vez y evita tirones y parpadeos de texturas. Al empezar el juego saldrá una barra de progreso; podrás pulsar para jugar ya."), tw, line);
      const int discs = std::max(1, inst.DiscsReady());
      char all_text[160];
      std::snprintf(all_text, sizeof(all_text), discs == 1 ? p.L("Todos los discos disponibles (%d disco) - recomendado") : p.L("Todos los discos disponibles (%d discos) - recomendado"), discs);
      const char* options[2] = {all_text, p.L("Solo la primera parte; las siguientes, al llegar a ellas")};
      for (int i = 0; i < 2; ++i) {
        const float top = y + 16.0f + float(i) * 62.0f, bottom = top + 50.0f;
        const bool selected = (i == 0) == inst.precache_all();
        if (m.clicked && m.In(tx, top, x1 - 34.0f, bottom)) inst.SetPrecacheAll(i == 0);
        if (selected) {
          p.SelectedPlate(tx, top, x1 - 34.0f, bottom + 4.0f);
          if (p.tex) p.Sprite(p.tex->window.get(), 6, 126, 40, 23, tx + 6.0f, top + 12.0f, 40.0f, 23.0f);
        } else {
          p.RaisedPlate(tx, top, x1 - 34.0f, bottom - 4.0f);
        }
        p.Text(selected ? TextStyle::kSelected : TextStyle::kNormal, tx + 56.0f, top + 10.0f, kTextScale * 0.9f, options[i], 0.0f, tw - 90.0f);
      }
      p.WrappedText(TextStyle::kInactive, tx, y + 16.0f + 2.0f * 62.0f + 10.0f, kTextScale * 0.8f, p.L("Todos los discos tarda unos minutos por disco; se puede cambiar luego en Configuración > Extras."), tw, line);
      break;
    }
    case InstallStep::kFinish: {
      p.Text(TextStyle::kNormal, tx, body - 6.0f, kTextScale, p.L("Todo listo"));
      char summary[200];
      std::snprintf(summary, sizeof(summary),
                    inst.install_mode() == InstallMode::kCopy ? p.L("Discos instalados en la carpeta del juego: %d de 4.")
                                                              : p.L("Discos verificados (se leen donde están): %d de 4."),
                    inst.DiscsReady());
      float y = p.WrappedText(TextStyle::kNormal, tx, body + 34.0f, fine, summary, tw, line);
      y = p.WrappedText(TextStyle::kNormal, tx, y + 8.0f, fine,
                        TexturePackInstalled() ? p.L("Texturas HD: ya instaladas.") : (inst.texture_choice() == TextureChoice::kNow ? p.L("Texturas HD: se descargarán al empezar.") : p.L("Texturas HD: más tarde.")), tw, line);
      y = p.WrappedText(TextStyle::kNormal, tx, y + 8.0f, fine,
                        inst.precache_all() ? p.L("Sombreadores: todos los discos.") : p.L("Sombreadores: solo la primera parte."), tw, line);
      if (ws.edit->NeedsRestartFrom(*ws.running)) {
        y = p.WrappedText(TextStyle::kNormal, tx, y + 8.0f, fine,
                          p.L("Al pulsar Jugar, el juego se reiniciará una vez para aplicar los ajustes elegidos."), tw, line);
      }
      p.WrappedText(TextStyle::kInactive, tx, y + 16.0f, fine, p.L("Pulsa Jugar para empezar. Las opciones se cambian después con F2 o en la Configuración del juego."), tw, line);
      break;
    }
  }

  // Botones de abajo.
  constexpr float by0 = y1 - 62.0f, by1 = y1 - 18.0f;
  const bool first = inst.step() == InstallStep::kWelcome;
  const bool last = inst.step() == InstallStep::kFinish;
  if (!first && WizardButton(p, m, x0 + 34.0f, by0, x0 + 234.0f, by1, p.L("Atrás"), !inst.install_running())) inst.Back();
  if (WizardButton(p, m, x1 - 254.0f, by0, x1 - 34.0f, by1, last ? p.L("Jugar") : p.L("Siguiente"), inst.CanGoNext() || last)) {
    if (last) inst.Finish(); else inst.Next();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
    if (inst.step() == InstallStep::kInstall && inst.install_mode() == InstallMode::kCopy && !inst.install_running() &&
        !inst.install_done()) {
      inst.StartInstall();  // Intro en esta pagina = "Instalar ahora"
    } else if (last) {
      inst.Finish();
    } else {
      inst.Next();
    }
  }
  if (!first && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
    inst.Back();
  }
  (void)now_ms;
}

}  // namespace

SettingsPageDialog::SettingsPageDialog(rex::ui::ImGuiDrawer* drawer,
                                       rex::ui::ImmediateDrawer* immediate_drawer,
                                       std::filesystem::path config_path,
                                       std::function<void()> request_close_window)
    : ImGuiDialog(drawer),
      immediate_drawer_(immediate_drawer),
      config_path_(std::move(config_path)),
      request_close_window_(std::move(request_close_window)) {
  running_ = ReadFromCvars();
  edit_ = running_;
}

SettingsPageDialog::~SettingsPageDialog() = default;

void SettingsPageDialog::PollAssets() {
  // La lectura empieza cuando se sabe de que disco se arranca (carpeta, ISO o GOD).
  if (!loading_.valid() && !loaded_) {
    if (auto disc = MultiDiscBootSource()) {
      loading_ = std::async(std::launch::async, [disc = std::move(*disc)] {
        auto load = std::make_unique<AssetLoad>();
        load->ok = menu_assets::Load(disc, load->assets, load->error);
        if (load->ok) AttachHdPages(load->assets);
        return load;
      });
    }
  }
  if (loading_.valid() && loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    loaded_ = loading_.get();
    if (loaded_->ok) {
      REXLOG_INFO("[settings] menu del juego leido de los datos (idioma {}, {} glifos)", loaded_->assets.language,
                  loaded_->assets.text.glyphs.size());
    } else {
      REXLOG_WARN("[settings] no se pudo leer el menu del juego ({}); se usa el estilo simple", loaded_->error);
    }
  }
  if (loaded_ && loaded_->ok && !textures_ && immediate_drawer_) {
    textures_ = CreateTextures(*immediate_drawer_, loaded_->assets, 1);
  }
}

bool SettingsPageDialog::ActionEnabled(SettingsRowAction action) const {
  if (action == SettingsRowAction::kDownloadTextures) return !TexturePackInstalled() && !TextureDownloadActive();
  return action != SettingsRowAction::kRestart || edit_.NeedsRestartFrom(running_);
}

void SettingsPageDialog::SetStatus(std::string text, int64_t now_ms) {
  status_ = std::move(text);
  status_until_ms_ = now_ms + 4000;
}

void SettingsPageDialog::Commit(int64_t now_ms) {
  if (!WriteToml(config_path_, edit_)) {
    SetStatus(Tr("No se pudo guardar config.toml."), now_ms);
  }
  ApplyHot(edit_);
}

void SettingsPageDialog::RunAction(SettingsRowAction action, int64_t now_ms) {
  switch (action) {
    case SettingsRowAction::kReloadTextures:
      ApplyHot(edit_);
      ReloadTexturePack();
      SetStatus(Tr("Texturas recargadas desde la carpeta del pack."), now_ms);
      break;
    case SettingsRowAction::kDownloadTextures:
      if (!ActionEnabled(action)) break;
      TextureDownloadStart();
      SetStatus(TextureDownloadText(), now_ms);
      break;
    case SettingsRowAction::kRestart:
      if (!ActionEnabled(action)) break;
      // Dos pulsaciones: reiniciar pierde lo no guardado de la partida.
      if (now_ms > restart_armed_until_ms_) {
        restart_armed_until_ms_ = now_ms + 3000;
        break;
      }
      restart_armed_until_ms_ = 0;
      if (WriteToml(config_path_, edit_) && SpawnRestart()) {
        request_close_window_();
      } else {
        SetStatus(Tr("No se pudo relanzar el juego; reinícialo a mano."), now_ms);
      }
      break;
    case SettingsRowAction::kNone:
      break;
  }
}

void SettingsPageDialog::HandleInput(int page, int64_t now_ms) {
  uint16_t nav = g_nav_pressed.exchange(0, std::memory_order_relaxed);
  const uint16_t held = g_nav_held.load(std::memory_order_relaxed) & kDpadMask;
  if (held != repeat_held_) {
    repeat_held_ = held;
    repeat_at_ms_ = now_ms + 380;
  } else if (held && now_ms >= repeat_at_ms_) {
    nav |= held;
    repeat_at_ms_ = now_ms + 90;
  }
  const auto& rows = PageRows(page);
  if (rows.empty() || !nav) return;
  const int count = int(rows.size());
  int& cursor = cursor_[page];
  cursor = std::clamp(cursor, 0, count - 1);
  if (nav & kDpadUp) cursor = (cursor + count - 1) % count;
  if (nav & kDpadDown) cursor = (cursor + 1) % count;
  if (nav & (kDpadUp | kDpadDown)) restart_armed_until_ms_ = 0;

  const Row& row = rows[cursor];
  if (row.action != SettingsRowAction::kNone) {
    if (nav & kButtonA) RunAction(row.action, now_ms);
    return;
  }
  if (row.disabled && row.disabled(edit_)) return;
  const int choices = int(row.choices.size());
  const int value = std::clamp(row.get(edit_), 0, choices - 1);
  auto offered = [&](int k) { return !row.choice_enabled || row.choice_enabled(edit_, k); };
  int next = value;
  if (nav & kDpadLeft) {
    for (int k = value - 1; k >= 0; --k) {
      if (offered(k)) {
        next = k;
        break;
      }
    }
  }
  if (nav & kDpadRight) {
    for (int k = value + 1; k < choices; ++k) {
      if (offered(k)) {
        next = k;
        break;
      }
    }
  }
  if (nav & kButtonA) {
    for (int step = 1; step < choices; ++step) {
      if (offered((value + step) % choices)) {
        next = (value + step) % choices;
        break;
      }
    }
  }
  if (next != value) {
    row.set(edit_, next);
    Commit(now_ms);
  }
}

void SettingsPageDialog::DrawPage(const SettingsPainter& p, int page, int64_t now_ms) const {
  DrawFrame(p);
  p.Text(TextStyle::kNormal, 64.7f, 125.5f, kSectionScale, p.L(kPageNames[page]));
  DrawTabs(p, page);

  const auto& rows = PageRows(page);
  const int cursor = std::clamp(cursor_[page], 0, int(rows.size()) - 1);
  for (int i = 0; i < int(rows.size()); ++i) {
    const Row& row = rows[i];
    // Con muchas filas (Gráficos) se juntan un poco para no pisar la ayuda.
    const float row_height = std::min(kRowHeight, kRowsBottom / float(rows.size()));
    const float top = kRowTop + float(i) * row_height, bottom = top + row_height;
    const bool selected = i == cursor;
    const bool enabled = ActionEnabled(row.action) && !(row.disabled && row.disabled(edit_));

    if (selected) {
      p.SelectedPlate(kLabelX0, top, 366.0f, bottom);
    } else {
      p.RaisedPlate(kLabelX0, top, kLabelX1, bottom);
    }
    p.Text(selected ? TextStyle::kSelected : (enabled ? TextStyle::kNormal : TextStyle::kDisabled), kLabelTextX,
           top + kTextDy, kTextScale, p.L(row.label), 0.0f, kLabelTextMaxWidth);
    if (selected && p.tex) {
      p.Sprite(p.tex->window.get(), 6, 126, 40, 23, 36.0f, top + 10.0f, 40.0f, 23.0f);
    }

    const int choices = int(row.choices.size());
    const int value = row.get(edit_);
    if (choices >= kSliderMinChoices) {
      // Deslizador: el valor en una celda (hundida, o clara con el cursor) y a la derecha una barra
      // con la posicion entre la primera y la ultima opcion.
      constexpr float value_w = 300.0f;
      const float vx0 = kCellsX, vx1 = kCellsX + value_w;
      if (selected) {
        p.SelectedPlate(vx0 - 1.0f, top, vx1, bottom);
      } else {
        p.InsetPlate(vx0, top, vx1, bottom);
      }
      const std::string label = row.choice_label ? row.choice_label(edit_, value)
                                                 : std::string(p.L(row.choices[std::clamp(value, 0, choices - 1)]));
      TextStyle style = selected ? TextStyle::kSelected : TextStyle::kNormal;
      if (!enabled) style = TextStyle::kDisabled;
      p.Text(style, vx0, top + kTextDy, kTextScale, label, vx1 - vx0, vx1 - vx0 - 16.0f);
      const float bx0 = vx1 + 24.0f, bx1 = kCellsX + kCellsWidth - 4.0f;
      const float by = (top + bottom) * 0.5f - 2.0f;
      p.InsetPlate(bx0, by - 5.0f, bx1, by + 7.0f);
      const float f = choices > 1 ? float(std::clamp(value, 0, choices - 1)) / float(choices - 1) : 0.0f;
      const float kx = bx0 + 3.0f + (bx1 - bx0 - 6.0f) * f;
      if (kx > bx0 + 3.0f) {
        p.Metal(bx0 + 3.0f, by - 2.0f, kx, by + 4.0f);
        p.Fill(bx0 + 3.0f, by - 2.0f, kx, by + 4.0f, IM_COL32(255, 255, 255, 90));
      }
      // Tirador.
      p.RaisedPlate(kx - 7.0f, by - 11.0f, kx + 7.0f, by + 13.0f);
      if (selected) p.Fill(kx - 7.0f, by - 11.0f, kx + 7.0f, by + 13.0f, IM_COL32(255, 255, 255, 70));
      continue;
    }
    const float pitch = (kCellsWidth + kCellGap) / float(choices);
    for (int k = 0; k < choices; ++k) {
      const float x0 = kCellsX + float(k) * pitch, x1 = x0 + pitch - kCellGap;
      const bool action = row.action != SettingsRowAction::kNone;
      const bool chosen = action || k == value;
      TextStyle style = TextStyle::kInactive;
      if (chosen && selected) {
        p.SelectedPlate(x0 - 1.0f, top, x1, bottom);
        style = TextStyle::kSelected;
      } else if (chosen && !action) {
        p.InsetPlate(x0, top, x1, bottom);
        style = TextStyle::kNormal;
      } else {
        p.RaisedPlate(x0, top, x1, bottom);
        style = action ? TextStyle::kNormal : TextStyle::kInactive;
      }
      if (!enabled || (row.choice_enabled && !row.choice_enabled(edit_, k))) style = TextStyle::kDisabled;
      const bool armed = row.action == SettingsRowAction::kRestart && now_ms <= restart_armed_until_ms_;
      p.Text(style, x0, top + kTextDy, kTextScale, p.L(armed ? "Pulsa A otra vez" : row.choices[k]), x1 - x0,
             x1 - x0 - 12.0f);
    }
  }

  // Barra de ayuda: la del elemento con el cursor o el ultimo aviso.
  std::string_view help;
  std::string help_text;
  if (!rows.empty()) {
    const Row& row = rows[cursor];
    const int value = row.get(edit_);
    if (row.help_fn) {
      help_text = row.help_fn(edit_);
      help = help_text;
    } else {
      help = p.L(value >= 0 && value < int(row.choice_help.size()) ? row.choice_help[value] : row.help);
    }
  }
  if (!rows.empty() && !ActionEnabled(rows[cursor].action)) {
    help = rows[cursor].action == SettingsRowAction::kDownloadTextures
               ? (TexturePackInstalled() ? p.L("El pack de texturas HD ya está instalado.") : p.L("Descargando el pack de texturas HD…"))
               : p.L("No hay cambios pendientes de reinicio.");
  }
  if (!rows.empty() && rows[cursor].disabled && rows[cursor].disabled(edit_)) {
    help = p.L(rows[cursor].disabled_help);
  }
  if (now_ms <= status_until_ms_) help = status_;
  p.Text(TextStyle::kNormal, 65.4f, 658.7f, kTextScale, p.L("Ayuda"), 0.0f, 0.0f, kHelpLabelXScale);
  p.Text(TextStyle::kNormal, kHelpTextX, 655.4f, kTextScale, help, 0.0f, kHelpTextMaxWidth, kHelpXScale);
}

// Contador de fps: fotogramas del juego (los cuenta Odisea en cada presentacion),
// no los de la ventana; con el maximo del ultimo medio segundo se ven los tirones.
static void DrawFpsCounter(ImGuiIO& io) {
  float fps = 0.0f, avg_ms = 0.0f, max_ms = 0.0f;
  if (!QueryGameFrameStats(fps, avg_ms, max_ms) || fps <= 0.0f) return;
  char text[96];
  std::snprintf(text, sizeof(text), "%.0f fps  %.1f ms  (max %.1f)", fps, avg_ms, max_ms);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const float size = std::max(16.0f, io.DisplaySize.y / 45.0f);
  const ImVec2 text_size = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const ImVec2 pos(size * 0.6f, size * 0.5f);
  dl->AddRectFilled(ImVec2(pos.x - size * 0.3f, pos.y - size * 0.15f),
                    ImVec2(pos.x + text_size.x + size * 0.3f, pos.y + text_size.y + size * 0.15f),
                    IM_COL32(0, 0, 0, 150), size * 0.2f);
  // Verde a 55 fps o mas, amarillo por encima de 40, rojo por debajo.
  const ImU32 color = fps >= 55.0f   ? IM_COL32(120, 255, 120, 255)
                      : fps >= 40.0f ? IM_COL32(255, 220, 90, 255)
                                     : IM_COL32(255, 110, 110, 255);
  dl->AddText(font, size, pos, color, text);
}

void SettingsPageDialog::OnDraw(ImGuiIO& io) {
  PollAssets();
  if (REXCVAR_GET(lo_show_fps)) DrawFpsCounter(io);
  auto make_painter = [&]() {
  SettingsPainter p;
  p.dl = ImGui::GetForegroundDrawList();
  p.tex = textures_.get();
  p.fallback_font = ImGui::GetFont();
  {
    // El mismo rectangulo en el que el presentador encaja la imagen del juego
    // (odisea_display_aspect): con 16:10 / 21:9 el lienzo de 1280x720 se estira.
    const float output_aspect = float(running_.output_w()) / float(running_.output_h());
    const float aspect = running_.aspect() != AspectRatio::k16x9 ? output_aspect
                                                                 : 16.0f / 9.0f;
    float w = io.DisplaySize.x, h = io.DisplaySize.y;
    if (w > h * aspect) {
      w = h * aspect;
    } else {
      h = w / aspect;
    }
    p.sx = w / 1280.0f;
    p.s = h / 720.0f;
    // Con D3D12 el plugin dibuja la interfaz del juego aparte, en un
    // rectangulo 16:9 centrado (odisea_hud_resolution): la pagina, igual.
    if (running_.gpu_backend == GpuBackend::kD3D12) {
      p.sx = p.s = std::min(p.sx, p.s);
    }
  }
  // Paginas HD de la fuente reducidas a lo que de verdad se ve (el texto va a ~0,88 de su tamano nativo).
  if (textures_ && loaded_ && loaded_->ok && immediate_drawer_) {
    const float need = 0.88f * std::min(p.sx, p.s);
    const int want = need <= 1.25f ? 1 : (need <= 2.5f ? 2 : 4);
    if (want != textures_->hd_factor) {
      if (auto next = CreateTextures(*immediate_drawer_, loaded_->assets, want)) {
        retired_.emplace_back(std::move(textures_), NowMs());
        textures_ = std::move(next);
      } else {
        textures_->hd_factor = want;  // no se pudo: se sigue con las actuales
      }
    }
    p.tex = textures_.get();
  }
  p.ox = std::round((io.DisplaySize.x - 1280.0f * p.sx) * 0.5f);
  p.oy = std::round((io.DisplaySize.y - 720.0f * p.s) * 0.5f);
  p.left = -p.ox / p.sx;
  p.right = (io.DisplaySize.x - p.ox) / p.sx;
  p.top = -p.oy / p.s;
  p.bottom = (io.DisplaySize.y - p.oy) / p.s;
  return p;
  };
  // Las texturas sustituidas se sueltan unos segundos despues (la GPU puede estar aun leyendolas).
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                [](const auto& r) { return NowMs() - r.second > 3000; }),
                 retired_.end());
  if (installer_) {
    const SettingsPainter wp = make_painter();
    WizardSettings ws{&edit_, &running_, config_path_, &wizard_cursor_};
    const bool font_pending = !textures_ && !(loaded_ && !loaded_->ok);
    DrawInstaller(wp, *installer_, io, NowMs(), ws, font_pending);
    return;
  }
  // Descarga de texturas en marcha (o error reciente): aviso arriba; en la pagina de texturas, ademas,
  // el progreso en la barra de ayuda.
  const TextureDownloadInfo download = TextureDownloadStatus();
  if (TextureDownloadActive() && !PrecacheChoicePending()) {
    const SettingsPainter bp = make_painter();
    DrawDownloadBadge(bp);
  }
  if (g_page.load(std::memory_order_relaxed) == 4 &&
      (TextureDownloadActive() || download.state == TextureDownloadState::kError)) {
    SetStatus(TextureDownloadText(), NowMs());
  }
  int prep_phase = 0;
  uint32_t prep_done = 0, prep_total = 0;
  const bool choosing = PrecacheChoicePending();
  bool preparing = QueryShaderPrep(prep_phase, prep_done, prep_total) && prep_phase != 0;
#ifdef LO_DEV
  if (!preparing && REXCVAR_GET(lo_shader_prep_preview)) {
    // Vista previa para ajustar la pantalla (la preparacion real suele durar
    // menos de un segundo si el controlador ya tiene su cache).
    preparing = true;
    prep_phase = 2;
    prep_total = 1233;
    prep_done = uint32_t((NowMs() / 15) % (prep_total + 200));
  }
#endif
  if (!preparing && !choosing && !SettingsScreenInteractive()) {
    last_page_ = 0;
    return;
  }
  const int64_t now_ms = NowMs();
  const int page = g_page.load(std::memory_order_relaxed);

  SettingsPainter p = make_painter();

  if (choosing) {
    DrawPrecacheChoice(p);
    return;
  }
  if (preparing) {
    DrawShaderPrep(p, prep_phase, prep_done, prep_total, now_ms);
    return;
  }
  if (page == 0) {
    DrawTabs(p, 0);
    last_page_ = 0;
    return;
  }
  if (page != last_page_) {
    restart_armed_until_ms_ = 0;
    last_page_ = page;
  }
  HandleInput(page, now_ms);
  DrawPage(p, page, now_ms);
}

}  // namespace lo
