// Fork (odisea): estado comun de DLSS. Ver include/rex/graphics/odisea_dlss.h.
#include <rex/graphics/odisea_dlss.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>

REXCVAR_DEFINE_BOOL(odisea_dlss, false, "GPU",
                    "odisea: reescalar la escena 3D con NVIDIA DLSS (solo D3D12 con tarjeta RTX; "
                    "necesita la interfaz aparte, odisea_hud_resolution). El modo sale de la "
                    "relacion entre la escala 3D y la resolucion de salida (igual = DLAA).");

REXCVAR_DEFINE_INT32(odisea_dlss_mask_formats, 0, "GPU/Debug",
                     "odisea: formatos de destino (bit = ColorRenderTargetFormat) cuyos dibujos "
                     "con mezcla y sin escribir profundidad van a la mascara de efectos de DLSS; "
                     "0 = sin mascara. En caliente.");

REXCVAR_DEFINE_BOOL(odisea_dlss_object_motion, false, "GPU",
                    "odisea: vectores de movimiento propios de los personajes para DLSS (se "
                    "capturan sus vertices con stream output). En caliente.");

REXCVAR_DEFINE_BOOL(odisea_dlss_mip_bias, false, "GPU",
                    "odisea: con DLSS, sesgo de mip de las texturas de la escena para que tengan "
                    "el detalle de la resolucion de salida (log2(render / salida)). En caliente. "
                    "EXPERIMENTAL: el 7-oct-2026 con Vulkan la escena salio casi negra.");
REXCVAR_DEFINE_DOUBLE(odisea_dlss_mip_bias_extra, 0.0, "GPU/Debug",
                      "odisea: sesgo de mip anadido al de DLSS (negativo = mas nitido). En "
                      "caliente.");
REXCVAR_DEFINE_DOUBLE(odisea_dlss_sharpness, 0.0, "GPU",
                      "odisea: enfocado (tipo CAS) de la escena 3D tras DLSS, 0 = nada, 1 = "
                      "maximo; no toca la interfaz. En caliente.");

namespace rex::graphics::odisea::dlss {

namespace {

constexpr uint32_t kViewProjectionFirst = 233;
// Huesos del skinning (matrices 3x4) en Lost Odyssey.
constexpr uint32_t kBonesFirst = 113;
constexpr uint32_t kBonesLast = 202;

bool g_active = false;
uint64_t g_frame = 0;
uint32_t g_phase_count = 8;
float g_jitter[2] = {0.0f, 0.0f};
float g_mip_bias = 0.0f;

float g_current[16];
float g_previous[16];
bool g_current_valid = false;
float g_depth_scale = 1.0f;
float g_depth_offset = 0.0f;
bool g_previous_valid = false;

float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, result = 0.0f;
  for (uint32_t i = index; i > 0; i /= base) {
    f /= float(base);
    result += f * float(i % base);
  }
  return result;
}

}  // namespace

bool Requested() { return REXCVAR_GET(odisea_dlss); }

void SetFrame(bool active, uint32_t render_width, uint32_t render_height, uint32_t output_width,
              uint32_t output_height) {
  g_active = active && render_width && render_height;
  if (!g_active) {
    g_jitter[0] = g_jitter[1] = 0.0f;
    g_mip_bias = 0.0f;
    return;
  }
  g_mip_bias = std::log2(float(render_width) / float(std::max(output_width, 1u)));
  // Guia de DLSS: 8 fases por cada pixel de salida que cubre un pixel de entrada.
  float ratio = float(output_width) / float(render_width);
  g_phase_count = uint32_t(std::clamp(std::ceil(8.0f * ratio * ratio), 8.0f, 128.0f));
  ++g_frame;
  uint32_t index = uint32_t(g_frame % g_phase_count) + 1;
  g_jitter[0] = Halton(index, 2) - 0.5f;
  g_jitter[1] = Halton(index, 3) - 0.5f;
}

bool JitterActive() { return g_active; }

void GetJitter(float& x, float& y) {
  x = g_jitter[0];
  y = g_jitter[1];
}

bool IsSceneDraw(const RegisterFile& regs) {
  if (!(regs[XE_GPU_REG_RB_DEPTHCONTROL] & 0x2)) return false;  // z_enable
  uint32_t surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
  uint32_t pitch = surface_info & 0x3FFF;
  if (pitch < 1024 || pitch > 1280 || ((surface_info >> 16) & 3) != 0) return false;
  float x_scale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE);
  return x_scale * 2.0f == float(pitch) &&
         regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) == x_scale;
}

bool IsEffectDraw(const RegisterFile& regs) {
  if (!IsSceneDraw(regs)) return false;
  if (regs[XE_GPU_REG_RB_DEPTHCONTROL] & 0x4) return false;  // z_write_enable
  if (!(regs[XE_GPU_REG_RB_COLOR_MASK] & 0xF)) return false;
  // Solo los formatos de destino de la pasada de efectos (Lost Odyssey: 12 =
  // 2_10_10_10_FLOAT_AS_16_16_16_16); las pasadas de luz y de sombras van en
  // otros y cubren media pantalla.
  uint32_t format = (regs[XE_GPU_REG_RB_COLOR_INFO] >> 16) & 0xF;
  if (!((uint32_t(REXCVAR_GET(odisea_dlss_mask_formats)) >> format) & 1)) return false;
  // Mezcla de verdad en el destino 0 (no ONE / ZERO en color).
  uint32_t blend = regs[XE_GPU_REG_RB_BLENDCONTROL0];
  uint32_t src = blend & 0x1F, dest = (blend >> 8) & 0x1F;
  return !(src == 1 && dest == 0);
}

bool IsDynamicDraw(const RegisterFile& regs, const Shader& vertex_shader) {
  if (!REXCVAR_GET(odisea_dlss_object_motion)) return false;
  if (!IsSceneDraw(regs)) return false;
  if (!(regs[XE_GPU_REG_RB_DEPTHCONTROL] & 0x4)) return false;  // z_write_enable
  const Shader::ConstantRegisterMap& map = vertex_shader.constant_register_map();
  for (uint32_t i = kBonesFirst; i <= kBonesLast; ++i) {
    if (map.float_bitmap[i >> 6] & (uint64_t(1) << (i & 63))) return true;
  }
  return false;
}

void NoteSceneDraw(const RegisterFile& regs, const Shader& vertex_shader) {
  if (g_current_valid) return;
  const Shader::ConstantRegisterMap& map = vertex_shader.constant_register_map();
  for (uint32_t i = kViewProjectionFirst; i < kViewProjectionFirst + 4; ++i) {
    if (!(map.float_bitmap[i >> 6] & (uint64_t(1) << (i & 63)))) return;
  }
  float m[16];
  for (uint32_t i = 0; i < 16; ++i) {
    m[i] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + kViewProjectionFirst * 4 + i);
  }
  // Perspectiva: la columna w (m[3], m[7], m[11]) no es nula y la escala
  // horizontal es razonable (campo de vision entre ~10 y ~170 grados).
  float w_column = std::abs(m[3]) + std::abs(m[7]) + std::abs(m[11]);
  float x_scale = std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
  if (!(w_column > 1e-3f) || !(x_scale > 0.08f && x_scale < 12.0f)) return;
  std::memcpy(g_current, m, sizeof(m));
  g_current_valid = true;
  g_depth_scale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE);
  g_depth_offset = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET);
  if (!(std::abs(g_depth_scale) > 1e-6f)) {
    g_depth_scale = 1.0f;
    g_depth_offset = 0.0f;
  }
}

bool GetMatrices(float current[16], float previous[16]) {
  if (!g_current_valid) return false;
  std::memcpy(current, g_current, sizeof(g_current));
  // Sin anterior: sin movimiento (la anterior = la actual).
  std::memcpy(previous, g_previous_valid ? g_previous : g_current, sizeof(g_previous));
  return true;
}

void GetDepthMapping(float& scale, float& offset) {
  scale = g_depth_scale;
  offset = g_depth_offset;
}

void EndFrame() {
  if (g_current_valid) {
    std::memcpy(g_previous, g_current, sizeof(g_current));
    g_previous_valid = true;
  }
  g_current_valid = false;
}

bool MipBiasActive() { return g_active && REXCVAR_GET(odisea_dlss_mip_bias); }

void ApplyMipBias(uint32_t* fetch_constants) {
  float bias = g_mip_bias + float(REXCVAR_GET(odisea_dlss_mip_bias_extra));
  int32_t delta = int32_t(std::lround(bias * 32.0f));
  if (!delta) return;
  for (uint32_t i = 0; i < 32; ++i) {
    uint32_t* fetch = fetch_constants + i * 6;
    if ((fetch[0] & 3) != 2) continue;  // solo constantes de textura
    // Palabra 4, bits 12-21: sesgo de LOD x32, con signo.
    int32_t lod_bias = int32_t(fetch[4] << 10) >> 22;
    lod_bias = std::clamp(lod_bias + delta, -512, 511);
    fetch[4] = (fetch[4] & ~(0x3FFu << 12)) | ((uint32_t(lod_bias) & 0x3FFu) << 12);
  }
}

float Sharpness() {
  return std::clamp(float(REXCVAR_GET(odisea_dlss_sharpness)), 0.0f, 1.0f);
}

}  // namespace rex::graphics::odisea::dlss
