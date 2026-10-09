// lostodyssey - ReXGlue Recompiled Project
//
// Ajuste en vivo: relee "lo_live_tuning.txt" (junto al exe) cada ~30 frames si
// cambio. Cada linea "nombre valor" hace SetFlagByName, asi que sirve para
// CUALQUIER cvar hot-reload (los del exe y los del plugin grafico, p. ej. el
// volcado de draws re-armable del fork). '#' comenta.
//
// Es lo unico que queda de los hooks de diagnostico del antiguo modo "1080p
// nativo" (hud_pin_hooks / debug_scan_hooks / output_resolution_hooks),
// retirados el 30-sep-2026 cuando 1080p paso a ser 720p x1,5 en el plugin.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "crash_handler.h"
#include "shader_prep.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

namespace {

#ifdef LO_DEV
void LoLiveTuningTick() {
  static uint32_t frame = 0;
  if (++frame % 30 != 0) return;
  static std::filesystem::file_time_type last_time{};
  std::error_code ec;
  auto t = std::filesystem::last_write_time("lo_live_tuning.txt", ec);
  if (ec || t == last_time) return;
  last_time = t;
  std::ifstream in("lo_live_tuning.txt");
  std::string key, value;
  uint32_t applied = 0;
  while (in >> key) {
    if (!key.empty() && key[0] == '#') {
      in.ignore(4096, '\n');
      continue;
    }
    if (!(in >> value)) break;
    if (rex::cvar::SetFlagByName(key, value)) {
      ++applied;
    } else {
      REXLOG_WARN("lo_tuning: no se pudo aplicar '{} {}'", key, value);
    }
  }
  REXLOG_INFO("lo_tuning: {} valores aplicados desde lo_live_tuning.txt", applied);
}
#endif

}  // namespace

// Hook por frame al inicio de D3DDevice_Swap (0x827B4E50, midasm_hook del
// manifiesto sin registros).
void LoDebugSwapTickHook() {
  lo::HangWatchFrame();  // latido del detector de cuelgues (crash_handler.h)
  lo::ShaderPrepKeyboardTick();  // Enter = "Jugar ya" en la pantalla de preparacion
#ifdef LO_DEV
  LoLiveTuningTick();
#endif
}
