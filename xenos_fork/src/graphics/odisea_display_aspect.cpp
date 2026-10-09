// Fork (odisea): aspecto de presentacion. Ver include/rex/graphics/odisea_display_aspect.h.
#include <rex/graphics/odisea_display_aspect.h>

#include <atomic>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(odisea_display_aspect, "", "GPU",
                      "odisea: aspect ratio used to present the guest output, as W:H "
                      "(e.g. 16:10, 7:3). Empty = the guest video mode (16:9).");

namespace rex::graphics::odisea {

void ApplyDisplayAspect(uint32_t& display_width, uint32_t& display_height,
                        uint32_t guest_output_width, uint32_t guest_output_height) {
  const std::string value = REXCVAR_GET(odisea_display_aspect);
  if (!value.empty()) {
    unsigned x = 0, y = 0;
    if (std::sscanf(value.c_str(), "%u:%u", &x, &y) == 2 && x && y && x <= 1000 && y <= 1000) {
      display_width = x;
      display_height = y;
    }
  }
  static std::atomic<uint64_t> last{0};
  const uint64_t key = (uint64_t(guest_output_width) << 32) | guest_output_height;
  if (last.exchange(key) != key) {
    REXGPU_INFO("odisea: salida del juego {}x{}, presentada con aspecto {}:{}", guest_output_width,
                guest_output_height, display_width, display_height);
  }
}

}  // namespace rex::graphics::odisea
