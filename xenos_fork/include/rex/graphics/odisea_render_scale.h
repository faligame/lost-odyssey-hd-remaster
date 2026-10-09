// Fork (odisea): escala de render fraccionaria (fase 2: D3D12 + ROV).
//
// El juego sigue a su resolucion (Lost Odyssey: 1280x720) y cada draw se
// rasteriza a una escala s = q / 4 (q en CUARTOS: 6 = x1.5 = 1080p). La escala
// entera de Xenia (draw_resolution_scale_x/y) pasa a ser el FACTOR DE
// ALMACENAMIENTO S = ceil(s): los bufferes de texturas escaladas, sus
// direcciones y la EDRAM usan S, exactamente como en Xenia; solo lo geometrico
// (tamanos del host, tiles de la EDRAM en el ROV, viewport, scissor, clears,
// resolves) usa q. Con q = 4 * S todo es identico a Xenia.
//
// Pixel del invitado g -> primer pixel del host (q * g + 1) >> 2 (el centro del
// pixel del host cae dentro de g); n pixeles del invitado -> (q * n + 1) >> 2
// pixeles del host (720 -> 1080, tile 80x16 -> 120x24). El formato de las
// texturas escaladas con escala no entera se describe en resolve.xesli
// (XeResolveScaledToHostPixel).
#ifndef REX_GRAPHICS_XENOS1080_RENDER_SCALE_H_
#define REX_GRAPHICS_XENOS1080_RENDER_SCALE_H_

#include <cstdint>

namespace rex::graphics::odisea {

// q activo (0 = sin escala fraccionaria: se usa 4 * factor entero). Lo fija el
// procesador de comandos D3D12 al crear las caches; Vulkan no lo toca.
void SetRenderScaleQuarters(uint32_t q);
uint32_t GetRenderScaleQuartersRaw();

// q pedido en la configuracion (cvar odisea_render_scale_q), 0 si no.
uint32_t GetConfigRenderScaleQuarters();

// Escala en cuartos de un eje cuyo factor de almacenamiento es storage_scale.
inline uint32_t RenderScaleQuarters(uint32_t storage_scale) {
  uint32_t q = GetRenderScaleQuartersRaw();
  return q ? q : storage_scale * 4;
}

inline bool IsRenderScaleFractional() {
  uint32_t q = GetRenderScaleQuartersRaw();
  return q != 0 && (q & 3) != 0;
}

// n pixeles del invitado -> pixeles del host (tambien: primer pixel del host del
// pixel n del invitado).
inline uint32_t ScaleToHost(uint32_t n, uint32_t q) { return (q * n + 1) >> 2; }

// Escala en coma flotante.
inline float RenderScaleFloat(uint32_t q) { return float(q) * 0.25f; }

// Plan B (7-oct-2026): los sombreadores ya no llevan la escala compilada (pipelines comunes a
// todas las escalas); la leen de estas constantes de sistema. Mismo contenido en DXBC (D3D12) y
// SPIR-V (Vulkan); ver DxbcShaderTranslator::SystemConstants.
struct ScaleShaderConstants {
  // Tile de la EDRAM en muestras a la escala: ancho (20 q_x), alto (4 q_y), medio ancho, tamano.
  uint32_t edram_tile_dims[4];
  // ceil(2^32 / d) para dividir con umulhi: ancho, alto, medio ancho, 0.
  uint32_t edram_tile_divide_magic[4];
  // -ancho, -alto, -medio ancho, 0.
  int32_t edram_tile_dims_negative[4];
  // Pasos entre muestras: ancho, 1 - ancho, 2 - ancho, -1 - ancho.
  int32_t edram_sample_steps[4];
  // q_x, q_y, escala entera x / y (0 si la escala es fraccionaria).
  uint32_t quarters[4];
  // s_x, s_y, 1 / s_x, 1 / s_y.
  float scale_float[4];
  // Tamano de la EDRAM en muestras de 32 bpp a la escala, su reciproco (bits de un float),
  // -tamano, 0.
  uint32_t edram_size_32bpp_samples[4];
};
// Constantes para la escala actual (factor de almacenamiento entero + q fraccionario de D3D12),
// o para x1 (unscaled: dibujos fuera de la EDRAM, traducidos sin escala).
const ScaleShaderConstants& GetScaleShaderConstants(uint32_t draw_scale_x, uint32_t draw_scale_y,
                                                    bool unscaled);

}  // namespace rex::graphics::odisea

#endif  // REX_GRAPHICS_XENOS1080_RENDER_SCALE_H_
