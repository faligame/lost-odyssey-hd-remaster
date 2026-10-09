// lostodyssey - ReXGlue Recompiled Project
//
// Modo TURBO: escala el reloj del guest (como el "Time Scalar" de Xenia
// Canary). El SDK genera el vblank del guest con ese mismo reloj, así que con
// x2 el juego avanza el doble de rápido (cinemáticas, combates...).
//
// Activación: un botón del mando (cvar lo_turbo_button, máscara XInput) o la
// tecla F6 (bind en lostodyssey_app.h). El juego lee el mando en UN solo sitio
// (sub_822A8EB0: "addi r4,r31,52 ; bl sub_82292D28" = XamInputGetState(user,
// 1, r31+52)). Se hookea la instrucción de retorno (0x822A8F30, r3 = estado,
// r31 = base del struct) y se lee X_INPUT_STATE en r31+52: packet(4) +
// buttons(2, big-endian). El botón de turbo se borra del estado para que el
// juego no lo vea.
#include "crash_handler.h"
#include "settings_page.h"
#include "shader_prep.h"
#include "shader_prewarm.h"
#include "turbo.h"

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_BOOL(lo_turbo_enabled, true, "LostOdyssey/Turbo",
                    "Permitir el modo turbo (botón del mando / F6)");
REXCVAR_DEFINE_DOUBLE(lo_turbo_scalar, 2.0, "LostOdyssey/Turbo",
                      "Multiplicador de velocidad del turbo (1.5, 2, 3...)");
REXCVAR_DEFINE_INT32(lo_turbo_button, 0x0040, "LostOdyssey/Turbo",
                     "Botón XInput del turbo (máscara: L3=64, R3=128, Back=32, LB=256, RB=512)");
REXCVAR_DEFINE_BOOL(lo_turbo_hold, false, "LostOdyssey/Turbo",
                    "true = turbo solo mientras se mantiene el botón; false = conmutar");

namespace lo {

namespace {
std::atomic<bool> g_turbo_active{false};
uint16_t g_prev_buttons = 0;

void ApplyScalar(bool on) {
  double s = on ? REXCVAR_GET(lo_turbo_scalar) : 1.0;
  if (s < 0.25) s = 0.25;
  if (s > 8.0) s = 8.0;
  rex::chrono::Clock::set_guest_time_scalar(s);
  REXLOG_INFO("[turbo] {} (time scalar x{:.2f})", on ? "ON" : "OFF", s);
}
}  // namespace

// El juego lee el mando en dos rutas (sub_822A8EB0 en juego normal y
// sub_82291E40 en cinemáticas); ambas terminan en XamInputGetState. Se procesa
// aquí, en el hilo del guest, sin tocar el sistema de entrada del host (hacerlo
// desde este hilo compite con el hilo de interfaz y tumbaba el proceso).
void TurboProcessButtons(uint16_t* buttons) {
  if (!buttons || !REXCVAR_GET(lo_turbo_enabled)) return;
  uint16_t mask = uint16_t(REXCVAR_GET(lo_turbo_button) & 0xFFFF);
  if (!mask) return;
  uint16_t b = *buttons;
  bool down = (b & mask) == mask;
  bool was_down = (g_prev_buttons & mask) == mask;
  g_prev_buttons = b;
  if (REXCVAR_GET(lo_turbo_hold)) {
    if (down != TurboActive()) TurboSet(down);
  } else if (down && !was_down) {
    TurboToggle();
  }
  *buttons = uint16_t(b & ~mask);  // el juego no debe ver el botón de turbo
}

bool TurboActive() { return g_turbo_active.load(std::memory_order_relaxed); }

void TurboSet(bool on) {
  if (!REXCVAR_GET(lo_turbo_enabled)) on = false;
  bool was = g_turbo_active.exchange(on);
  if (was != on) ApplyScalar(on);
}

void TurboToggle() { TurboSet(!TurboActive()); }

// Si cambia el multiplicador con el turbo activo, reaplicarlo.
void TurboRefresh() {
  if (TurboActive()) ApplyScalar(true);
}

}  // namespace lo

// Retorno de XamInputGetState en las dos rutas del juego:
//   0x822A8F30 en sub_822A8EB0 (juego normal, estado en r31+52)
//   0x82291F48 en sub_82291E40 (cinemáticas/menús, estado en r1+176)
// r3 = X_RESULT; el estado es X_INPUT_STATE (packet u32 + buttons u16 BE).
namespace {
void ProcessGuestInputState(uint32_t result, uint32_t state_va) {
  if (result != 0) return;
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  uint8_t* state = memory->TranslateVirtual<uint8_t*>(state_va);
  if (!state) return;
  // Pantalla "Preparando sombreadores" (la del arranque o la generacion
  // completa de pipelines del primer arranque): el juego sigue funcionando
  // detras (los pipelines se crean en su ciclo de fotogramas) pero no debe
  // recibir el mando, para no empezar partida a ciegas.
  // Eleccion de la precarga de sombreadores (primera vez): arriba/abajo y A o Start.
  if (lo::PrecacheChoicePending()) {
    static uint16_t prev = 0;
    const uint16_t buttons = uint16_t((uint16_t(state[4]) << 8) | state[5]);
    const uint16_t pressed = uint16_t(buttons & ~prev);
    prev = buttons;
    if (pressed & (0x0001 | 0x0002)) lo::PrecacheChoiceMove((pressed & 0x0001) ? -1 : 1);
    if (pressed & (0x1000 | 0x0010)) lo::PrecacheChoiceConfirm();
    std::memset(state + 4, 0, 12);
    return;
  }
  {
    int phase = 0;
    uint32_t done = 0, total = 0;
    if (lo::QueryShaderPrep(phase, done, total) && phase != 0) {
      // "Jugar ya" (A o Start, al pulsar): la generacion completa sigue en
      // segundo plano y la pantalla se cierra.
      static uint16_t prev = 0;
      const uint16_t buttons = uint16_t((uint16_t(state[4]) << 8) | state[5]);
      const uint16_t pressed = uint16_t(buttons & ~prev);
      prev = buttons;
      if ((pressed & (0x1000 | 0x0010)) && lo::QueryPrewarmCanSkip()) lo::PrewarmSkip();
      std::memset(state + 4, 0, 12);  // XINPUT_GAMEPAD: botones, gatillos y sticks
      return;
    }
  }
  // La pagina de opciones dentro de Configuracion va primero: con ella abierta
  // el juego (y el turbo) deben ver el mando en reposo.
  lo::CrashHandlerTestTick();  // solo si lo_crash_test > 0
  lo::SettingsPageProcessInput(state);
  if (!REXCVAR_GET(lo_turbo_enabled)) return;
  uint16_t buttons = uint16_t((uint16_t(state[4]) << 8) | state[5]);
  lo::TurboProcessButtons(&buttons);
  state[4] = uint8_t(buttons >> 8);
  state[5] = uint8_t(buttons & 0xFF);
}
}  // namespace

// after_instruction=false, registers=["r3","r31"]
void LoTurboInputHook(PPCRegister& r3, PPCRegister& r31) {
  ProcessGuestInputState(r3.u32, r31.u32 + 52);
}

// after_instruction=false, registers=["r3","r1"]
void LoTurboInputCutsceneHook(PPCRegister& r3, PPCRegister& r1) {
  ProcessGuestInputState(r3.u32, r1.u32 + 176);
}
