// Fork (odisea): SMAA 1x en la salida del swap, compartido por D3D12 y
// Vulkan. Se aplica en IssueSwap en el mismo punto que el FXAA del SDK, y lo
// sustituye cuando odisea_smaa esta activo.
//
// Los shaders salen de src/graphics/shaders/odisea_smaa.cs.hlsl; aqui solo
// estan el cvar y las texturas de consulta del SMAA, que viven en un unico .cpp
// para no duplicar sus ~180 KB en cada backend.
#ifndef REX_GRAPHICS_XENOS1080_SMAA_H_
#define REX_GRAPHICS_XENOS1080_SMAA_H_

#include <cstdint>

#include <rex/cvar.h>

// Definido en src/graphics/odisea_smaa.cpp.
REXCVAR_DECLARE(bool, odisea_smaa);

namespace rex::graphics::odisea {

// Texturas de consulta de thirdparty/smaa, con las filas contiguas.
// AreaTex es RG8 y SearchTex es R8.
inline constexpr uint32_t kSmaaAreaTexWidth = 160;
inline constexpr uint32_t kSmaaAreaTexHeight = 560;
inline constexpr uint32_t kSmaaSearchTexWidth = 64;
inline constexpr uint32_t kSmaaSearchTexHeight = 16;
const uint8_t* SmaaAreaTexBytes();    // kSmaaAreaTexWidth * kSmaaAreaTexHeight * 2
const uint8_t* SmaaSearchTexBytes();  // kSmaaSearchTexWidth * kSmaaSearchTexHeight

}  // namespace rex::graphics::odisea

#endif  // REX_GRAPHICS_XENOS1080_SMAA_H_
