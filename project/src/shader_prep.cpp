// lostodyssey - ReXGlue Recompiled Project
//
// Ver shader_prep.h.
#include "shader_prep.h"
#include "shader_prewarm.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace lo {

bool QueryShaderPrep(int& phase, uint32_t& done, uint32_t& total) {
#if defined(_WIN32)
  using Fn = void (*)(int*, uint32_t*, uint32_t*);
  static Fn fn = nullptr;
  if (!fn) {
    if (HMODULE plugin = GetModuleHandleW(L"rexgpu-odisea.dll")) {
      fn = reinterpret_cast<Fn>(GetProcAddress(plugin, "odisea_ShaderPrepProgress"));
    }
  }
  if (!fn) return false;
  fn(&phase, &done, &total);
  return true;
#else
  phase = 0;
  done = total = 0;
  return false;
#endif
}

bool QueryDiscPrepared(int disc) {
#if defined(_WIN32)
  using Fn = int (*)(uint32_t);
  static Fn fn = nullptr;
  if (!fn) {
    if (HMODULE plugin = GetModuleHandleW(L"rexgpu-odisea.dll")) {
      fn = reinterpret_cast<Fn>(GetProcAddress(plugin, "odisea_PrewarmDiscDone"));
    }
  }
  return fn && fn(uint32_t(disc)) != 0;
#else
  (void)disc;
  return false;
#endif
}

bool QueryGameFrameStats(float& fps, float& avg_ms, float& max_ms) {
#if defined(_WIN32)
  using Fn = void (*)(float*, float*, float*);
  static Fn fn = nullptr;
  if (!fn) {
    if (HMODULE plugin = GetModuleHandleW(L"rexgpu-odisea.dll")) {
      fn = reinterpret_cast<Fn>(GetProcAddress(plugin, "odisea_GameFrameStats"));
    }
  }
  if (!fn) return false;
  fn(&fps, &avg_ms, &max_ms);
  return true;
#else
  fps = avg_ms = max_ms = 0.0f;
  return false;
#endif
}

bool QueryPrewarmCanSkip() {
#if defined(_WIN32)
  using Fn = int (*)();
  static Fn fn = nullptr;
  if (!fn) {
    if (HMODULE plugin = GetModuleHandleW(L"rexgpu-odisea.dll")) {
      fn = reinterpret_cast<Fn>(GetProcAddress(plugin, "odisea_PrewarmCanSkip"));
    }
  }
  return fn && fn() != 0;
#else
  return false;
#endif
}

bool QueryPrewarmBulkRunning() {
#if defined(_WIN32)
  using Fn = int (*)();
  static Fn fn = nullptr;
  if (!fn) {
    if (HMODULE plugin = GetModuleHandleW(L"rexgpu-odisea.dll")) {
      fn = reinterpret_cast<Fn>(GetProcAddress(plugin, "odisea_PrewarmBulkState"));
    }
  }
  return fn && fn() != 0;
#else
  return false;
#endif
}

void PrewarmSkip() {
#if defined(_WIN32)
  using Fn = void (*)();
  static Fn fn = nullptr;
  if (!fn) {
    if (HMODULE plugin = GetModuleHandleW(L"rexgpu-odisea.dll")) {
      fn = reinterpret_cast<Fn>(GetProcAddress(plugin, "odisea_PrewarmSkip"));
    }
  }
  if (fn) fn();
#endif
}

void ShaderPrepKeyboardTick() {
#if defined(_WIN32)
  // Solo con la ventana del juego en primer plano (GetAsyncKeyState es global).
  auto key_down = [](int vk) {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) != 0;
  };
  static bool was_enter = false, was_up = false, was_down = false;
  const bool enter = key_down(VK_RETURN), up = key_down(VK_UP), down = key_down(VK_DOWN);
  const bool enter_pressed = enter && !was_enter;
  const bool up_pressed = up && !was_up, down_pressed = down && !was_down;
  was_enter = enter;
  was_up = up;
  was_down = down;
  // Eleccion de la precarga de sombreadores (primera vez): flechas e Intro.
  if (PrecacheChoicePending()) {
    if (up_pressed || down_pressed) PrecacheChoiceMove(up_pressed ? -1 : 1);
    if (enter_pressed) PrecacheChoiceConfirm();
    return;
  }
  int phase = 0;
  uint32_t done = 0, total = 0;
  if (!QueryShaderPrep(phase, done, total) || phase == 0) return;
  if (enter_pressed && QueryPrewarmCanSkip()) PrewarmSkip();
#endif
}

}  // namespace lo
