// Fork (odisea): volcado de fotogramas completos. Ver include/rex/graphics/odisea_frame_dump.h.
#include <rex/graphics/odisea_frame_dump.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(odisea_frame_dump_after_ms, 0, "GPU/Debug",
                     "odisea: milliseconds after start at which to dump whole frames "
                     "(draws, resolves, swaps) to odisea_frame_dump.txt (0 = off)");
REXCVAR_DEFINE_INT32(odisea_frame_dump_frames, 3, "GPU/Debug",
                     "odisea: number of whole frames to dump");
REXCVAR_DEFINE_BOOL(odisea_frame_dump_constants, true, "GPU/Debug",
                    "odisea: en el volcado de fotogramas, tambien todas las constantes float que "
                    "usa el sombreador de vertices de cada draw (lineas VSC)");
REXCVAR_DEFINE_INT32(odisea_frame_dump_now, 0, "GPU/Debug",
                     "odisea: cambiarlo a otro numero distinto de 0 (en caliente, p. ej. desde "
                     "lo_live_tuning.txt) vuelca fotogramas ya a odisea_frame_dump_<numero>.txt");

namespace rex::graphics::odisea {

namespace {

const auto g_clock_start = std::chrono::steady_clock::now();
FILE* g_file = nullptr;
bool g_done = false;
int32_t g_frames_left = 0;
uint32_t g_frame = 0;
uint32_t g_index = 0;

float F(const RegisterFile& regs, uint32_t reg) {
  float f;
  uint32_t v = regs[reg];
  std::memcpy(&f, &v, 4);
  return f;
}

void AppendTextures(std::string& out, const RegisterFile& regs, const Shader& shader,
                    const char* stage) {
  for (const Shader::TextureBinding& binding : shader.texture_bindings()) {
    xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
    char buf[96];
    std::snprintf(buf, sizeof(buf), " %s.t%u=%08X:%ux%u:f%u%s", stage, binding.fetch_constant,
                  uint32_t(fetch.base_address) << 12, uint32_t(fetch.size_2d.width) + 1,
                  uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.format),
                  fetch.tiled ? "" : ":lin");
    out += buf;
  }
}

}  // namespace

bool FrameDumpActive() { return g_file != nullptr; }

void FrameDumpSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                   uint32_t frontbuffer_height) {
  if (g_file) {
    std::fprintf(g_file, "SWAP f%u frontbuffer=%08X %ux%u\n", g_frame, frontbuffer_ptr,
                 frontbuffer_width, frontbuffer_height);
    if (--g_frames_left <= 0) {
      std::fclose(g_file);
      g_file = nullptr;
      REXGPU_INFO("odisea frame dump: terminado ({} draws)", g_index);
      return;
    }
    ++g_frame;
    std::fprintf(g_file, "FRAME %u\n", g_frame);
    return;
  }
  // Disparo en caliente: cada valor nuevo distinto de 0 vuelca a su propio fichero.
  static int32_t last_now = 0;
  int32_t now = REXCVAR_GET(odisea_frame_dump_now);
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - g_clock_start)
                     .count();
  // El valor que ya traiga lo_live_tuning.txt al arrancar (se aplica tras los
  // primeros fotogramas) no dispara nada: solo los cambios pasados 30 s.
  if (elapsed < 30000) last_now = now;
  std::string path;
  if (now != last_now) {
    last_now = now;
    if (!now) return;
    path = "odisea_frame_dump_" + std::to_string(now) + ".txt";
  } else {
    int32_t after_ms = REXCVAR_GET(odisea_frame_dump_after_ms);
    if (g_done || after_ms <= 0 || elapsed < after_ms) return;
    g_done = true;
    path = "odisea_frame_dump.txt";
  }
  g_file = std::fopen(path.c_str(), "w");
  if (!g_file) {
    REXGPU_ERROR("odisea frame dump: no se puede crear {}", path);
    return;
  }
  g_frames_left = std::max(REXCVAR_GET(odisea_frame_dump_frames), 1);
  g_frame = 0;
  std::fprintf(g_file,
               "# odisea frame dump: %d fotogramas, empieza tras el swap a %lld ms\n"
               "# DRAW: surf=pitch/msaa, rtN=base:formato:mascara (solo los que escribe), "
               "z=base:formato:control, blend=RB_BLENDCONTROL0, cc=RB_COLORCONTROL,\n"
               "#       vp=escala/desplazamiento, sc=tijera, c0..c3 = constantes VS, "
               "vs.tN/ps.tN = textura leida base:WxH:formato\n"
               "# RESOLVE: src=seleccion (0-3 color, 4 profundidad), base/formato de origen, "
               "dest=base pitch x alto formato, escrito=direccion+longitud\n"
               "FRAME 0\n",
               g_frames_left, static_cast<long long>(elapsed));
  REXGPU_INFO("odisea frame dump: empieza ({} fotogramas)", g_frames_left);
}

void FrameDumpDraw(const RegisterFile& regs, const Shader& vertex_shader,
                   const Shader* pixel_shader, xenos::PrimitiveType primitive_type,
                   uint32_t index_count) {
  if (!g_file) return;
  uint32_t surf = regs[XE_GPU_REG_RB_SURFACE_INFO];
  uint32_t mode = regs[XE_GPU_REG_RB_MODECONTROL];
  uint32_t mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  uint32_t dc = regs[XE_GPU_REG_RB_DEPTHCONTROL];
  std::string rts;
  static const uint32_t kColorInfo[4] = {XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO,
                                         XE_GPU_REG_RB_COLOR2_INFO, XE_GPU_REG_RB_COLOR3_INFO};
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t m = (mask >> (i * 4)) & 0xF;
    if (!m) continue;
    uint32_t info = regs[kColorInfo[i]];
    char buf[48];
    std::snprintf(buf, sizeof(buf), " rt%u=%u:f%u:%X", i, info & 0xFFF, (info >> 16) & 0xF, m);
    rts += buf;
  }
  if (rts.empty()) rts = " rt=-";
  uint32_t dinfo = regs[XE_GPU_REG_RB_DEPTH_INFO];
  char zbuf[48];
  if (dc & 0x7) {
    std::snprintf(zbuf, sizeof(zbuf), " z=%u:f%u:%X", dinfo & 0xFFF, (dinfo >> 16) & 1, dc);
  } else {
    std::snprintf(zbuf, sizeof(zbuf), " z=-");
  }
  uint32_t sc_tl = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
  uint32_t sc_br = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  std::string tex;
  AppendTextures(tex, regs, vertex_shader, "vs");
  if (pixel_shader) AppendTextures(tex, regs, *pixel_shader, "ps");
  float c[16];
  for (uint32_t i = 0; i < 16; ++i) c[i] = F(regs, XE_GPU_REG_SHADER_CONSTANT_000_X + i);
  std::fprintf(
      g_file,
      "DRAW f%u #%u vs=%016llX ps=%016llX prim=%u n=%u mode=%u surf=%u/%u%s%s blend=%08X "
      "cc=%08X vte=%X vp=[%.1f %.1f %.1f %.1f] sc=(%u,%u)-(%u,%u) c0=[%g %g %g %g] "
      "c1=[%g %g %g %g] c2=[%g %g %g %g] c3=[%g %g %g %g]%s\n",
      g_frame, g_index++, static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
      static_cast<unsigned long long>(pixel_shader ? pixel_shader->ucode_data_hash() : 0),
      uint32_t(primitive_type), index_count, mode & 7, surf & 0x3FFF, (surf >> 16) & 3,
      rts.c_str(), zbuf, regs[XE_GPU_REG_RB_BLENDCONTROL0], regs[XE_GPU_REG_RB_COLORCONTROL],
      regs[XE_GPU_REG_PA_CL_VTE_CNTL], F(regs, XE_GPU_REG_PA_CL_VPORT_XSCALE),
      F(regs, XE_GPU_REG_PA_CL_VPORT_XOFFSET), F(regs, XE_GPU_REG_PA_CL_VPORT_YSCALE),
      F(regs, XE_GPU_REG_PA_CL_VPORT_YOFFSET), sc_tl & 0x7FFF, (sc_tl >> 16) & 0x7FFF,
      sc_br & 0x7FFF, (sc_br >> 16) & 0x7FFF, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
      c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15], tex.c_str());
  if (REXCVAR_GET(odisea_frame_dump_constants)) {
    // Constantes del sombreador de vertices (registros 0-255 del banco de VS).
    const Shader::ConstantRegisterMap& map = vertex_shader.constant_register_map();
    std::string line;
    char buf[96];
    for (uint32_t i = 0; i < 256; ++i) {
      if (!(map.float_bitmap[i >> 6] & (uint64_t(1) << (i & 63)))) continue;
      uint32_t r = XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4;
      std::snprintf(buf, sizeof(buf), " c%u=[%g %g %g %g]", i, F(regs, r), F(regs, r + 1),
                    F(regs, r + 2), F(regs, r + 3));
      line += buf;
    }
    if (!line.empty()) std::fprintf(g_file, "VSC f%u #%u%s\n", g_frame, g_index - 1, line.c_str());
  }
}

void FrameDumpResolve(const RegisterFile& regs, bool ok, uint32_t written_address,
                      uint32_t written_length) {
  if (!g_file) return;
  uint32_t copy = regs[XE_GPU_REG_RB_COPY_CONTROL];
  uint32_t src = copy & 7;
  uint32_t src_info;
  switch (src) {
    case 0: src_info = regs[XE_GPU_REG_RB_COLOR_INFO]; break;
    case 1: src_info = regs[XE_GPU_REG_RB_COLOR1_INFO]; break;
    case 2: src_info = regs[XE_GPU_REG_RB_COLOR2_INFO]; break;
    case 3: src_info = regs[XE_GPU_REG_RB_COLOR3_INFO]; break;
    default: src_info = regs[XE_GPU_REG_RB_DEPTH_INFO]; break;
  }
  uint32_t surf = regs[XE_GPU_REG_RB_SURFACE_INFO];
  uint32_t pitch = regs[XE_GPU_REG_RB_COPY_DEST_PITCH];
  uint32_t dest_info = regs[XE_GPU_REG_RB_COPY_DEST_INFO];
  uint32_t sc_tl = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
  uint32_t sc_br = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  std::fprintf(g_file,
               "RESOLVE f%u tras#%u src=%u base=%u fmt=%u surf=%u/%u copy=%08X (cmd=%u "
               "clear_color=%u clear_depth=%u) dest=%08X %ux%u info=%08X sc=(%u,%u)-(%u,%u) "
               "escrito=%08X+%u ok=%u\n",
               g_frame, g_index ? g_index - 1 : 0, src, src_info & 0xFFF, (src_info >> 16) & 0xF,
               surf & 0x3FFF, (surf >> 16) & 3, copy, (copy >> 20) & 3, (copy >> 8) & 1,
               (copy >> 9) & 1, regs[XE_GPU_REG_RB_COPY_DEST_BASE], pitch & 0x3FFF,
               (pitch >> 16) & 0x3FFF, dest_info, sc_tl & 0x7FFF, (sc_tl >> 16) & 0x7FFF,
               sc_br & 0x7FFF, (sc_br >> 16) & 0x7FFF, written_address, written_length,
               ok ? 1u : 0u);
}

}  // namespace rex::graphics::odisea
