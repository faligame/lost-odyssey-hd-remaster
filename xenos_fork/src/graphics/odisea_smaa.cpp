// Fork (odisea): cvar y texturas de consulta del SMAA, comunes a los dos
// backends. Ver include/rex/graphics/odisea_smaa.h.
#include <rex/graphics/odisea_smaa.h>

#include "thirdparty/smaa/AreaTex.h"
#include "thirdparty/smaa/SearchTex.h"

REXCVAR_DEFINE_BOOL(odisea_smaa, false, "GPU",
                    "odisea: SMAA 1x on the swap output (replaces FXAA when enabled)");

namespace rex::graphics::odisea {

const uint8_t* SmaaAreaTexBytes() {
  static_assert(sizeof(areaTexBytes) == kSmaaAreaTexWidth * kSmaaAreaTexHeight * 2);
  return areaTexBytes;
}

const uint8_t* SmaaSearchTexBytes() {
  static_assert(sizeof(searchTexBytes) == kSmaaSearchTexWidth * kSmaaSearchTexHeight);
  return searchTexBytes;
}

}  // namespace rex::graphics::odisea
