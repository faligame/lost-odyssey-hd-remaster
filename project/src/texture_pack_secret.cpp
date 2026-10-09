// lostodyssey - ReXGlue Recompiled Project
// Ver texture_pack_secret.h.
#include "texture_pack_secret.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <rex/logging.h>

#include "disc_sources.h"
#include "lo_hash.h"
#include "multidisc.h"

namespace {

std::mutex g_mutex;
bool g_have = false;
uint8_t g_secret[32];

bool ComputeSecret(uint8_t out[32]) {
  std::lock_guard lock(g_mutex);
  if (!g_have) {
    for (const lo::DiscSource& d : lo::MultiDiscAllSources()) {
      if (d.number != 1) continue;
      auto reader = lo::DiscReader::Open(d);
      std::vector<uint8_t> xex, fpi;
      if (!reader || !reader->ReadFile("default.xex", 0, ~0ull, xex) || !reader->ReadFile("LO.fpi", 0, ~0ull, fpi)) {
        break;
      }
      uint8_t buf[7 + 32 + 32];
      std::memcpy(buf, "LOPK-v1", 7);
      if (!lo::Sha256Of(xex.data(), xex.size(), buf + 7) || !lo::Sha256Of(fpi.data(), fpi.size(), buf + 39) ||
          !lo::Sha256Of(buf, sizeof(buf), g_secret)) {
        break;
      }
      g_have = true;
      break;
    }
  }
  if (g_have) std::memcpy(out, g_secret, 32);
  return g_have;
}

}  // namespace

namespace lo {

void RegisterTexturePackSecret() {
  using SetFn = void (*)(bool (*)(uint8_t*));
  if (HMODULE plugin = GetModuleHandleW(L"rexgpu-odisea.dll")) {
    if (auto set = reinterpret_cast<SetFn>(GetProcAddress(plugin, "odisea_SetTexturePackSecretProvider"))) {
      set(&ComputeSecret);
    }
  }
}

}  // namespace lo
