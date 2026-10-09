// Fork (odisea): dibujos que usan la textura vigilada (odisea_watch_texture).
//
// Uso en Lost Odyssey: el titulo de la pantalla de inicio (textura LO_TITLE,
// 512x128). El juego lo dibuja con un lote de cuadrados de la maqueta de
// 1280x720 (vertice de 12 palabras: posicion float4 en +0, UV float2 en +4,
// color float4 en +6 cuya alfa hace los fundidos): la tira de letras (UV v de
// 0 a 0,25), encima su copia difuminada para el brillo (v de 0,25 a 0,5) y el
// TM. La tira mide 512x32: no hay sitio para un logo.
//
// Aqui se agrandan esos cuadrados en la memoria del guest justo antes de que
// se suban los vertices: la tira y el brillo pasan a medir
// odisea_watch_quad_width x odisea_watch_quad_height, con la base donde
// estaba la tira. Como sigue siendo el dibujo del juego, el logo conserva sus
// fundidos, su orden (queda detras de los cuadros de mensaje) y desaparece
// cuando el juego deja de dibujar el titulo. El PNG del pack trae el logo en la
// banda de la tira y su version difuminada en la del brillo.
#include <rex/graphics/odisea_watched_draw.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/odisea_texture_pack.h>
#include <rex/graphics/odisea_render_scale.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ui/renderdoc_api.h>
#include <memory>
#include <unordered_map>

REXCVAR_DEFINE_DOUBLE(odisea_watch_quad_width, 515.0, "GPU/Textures",
                      "Ancho (maqueta 1280x720) al que se agrandan las bandas superiores de la textura "
                      "vigilada (0 = no tocar)");
REXCVAR_DEFINE_DOUBLE(odisea_watch_quad_height, 215.0, "GPU/Textures",
                      "Alto (maqueta 1280x720) al que se agrandan las bandas superiores de la textura vigilada");
REXCVAR_DEFINE_DOUBLE(odisea_watch_quad_dx, 12.0, "GPU/Textures",
                      "Desplazamiento horizontal del centro de las bandas agrandadas");
REXCVAR_DEFINE_BOOL(odisea_vertex_nan_check, false, "GPU/Debug",
                    "Diagnostico: anota en el log los dibujos con vertices o constantes de vertices "
                    "invalidos (NaN, infinitos, |x| > 1e7). Caro: solo para investigar");
REXCVAR_DEFINE_BOOL(odisea_vertex_nan_fix, false, "GPU/Debug",
                    "Prueba: pone a 0 los valores no validos (NaN, infinitos, |x| > 1e20) de los vertices "
                    "que genera la CPU (fetch constant 95) antes de dibujarlos");
REXCVAR_DEFINE_DOUBLE(odisea_watch_quad_max_v, 0.5, "GPU/Textures",
                      "Solo se agrandan los cuadrados cuya V no pasa de este valor (tira y brillo)");

namespace rex::graphics::odisea {

namespace {

uint32_t Swap32(uint32_t v, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      return ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);
    case xenos::Endian::k8in32:
      return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    case xenos::Endian::k16in32:
      return (v >> 16) | (v << 16);
    default:
      return v;
  }
}

float LoadFloat(const uint8_t* p, xenos::Endian endian) {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  raw = Swap32(raw, endian);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

void StoreFloat(uint8_t* p, float f, xenos::Endian endian) {
  uint32_t raw;
  std::memcpy(&raw, &f, 4);
  raw = Swap32(raw, endian);  // el intercambio es su propia inversa
  std::memcpy(p, &raw, 4);
}

}  // namespace

// --- Detector de vertices invalidos ---------------------------------------------
// Diagnostico (cvar odisea_vertex_nan_check): en cada dibujo revisa los
// atributos de coma flotante de 32 bits de los buferes de vertices y las
// constantes del sombreador de vertices que usa, y anota en el log los dibujos
// con NaN, infinitos o valores enormes (|x| > 1e7). Sirve para saber si unos
// triangulos negros o una "explosion" de vertices vienen ya rotos del juego
// (CPU) o los estropea la GPU. Es caro: dejarlo apagado salvo para investigar.
namespace {
// Vertices: NaN, infinitos o enormes. Constantes: solo NaN o infinitos (el juego
// usa a proposito valores como 1e8 para "lejisimos").
bool BadVertex(float f) { return !std::isfinite(f) || std::fabs(f) > 1e7f; }
bool BadConstant(float f) { return !std::isfinite(f); }

// Cada origen (sombreador + constante, o sombreador + bufer + atributo) se
// anota una vez con un ejemplo, y cada 10 s un resumen de los que han dado
// datos rotos en ese intervalo. Todo corre en el hilo del procesador de
// comandos.
struct Tally {
  std::string example;
  uint64_t draws_total = 0;
  uint64_t draws_window = 0;
  uint64_t bad_window = 0;
};
std::map<std::string, Tally> g_tally;
int64_t g_window_start_ms = 0;

bool Note(const std::string& key, uint32_t bad_count, const std::string& example) {
  Tally& t = g_tally[key];
  const bool is_new = t.draws_total++ == 0;
  if (is_new && g_tally.size() <= 400) {
    REXGPU_WARN("odisea nan: NUEVO {} -> {}", key, example);
    t.example = example;
  }
  ++t.draws_window;
  t.bad_window += bad_count;
  return is_new;
}

// Primera vez de un origen de vertices: fichero logs/nan_<vs>.txt con el
// desensamblado del sombreador y los vertices completos (el malo y dos buenos).
void DumpVertexOrigin(const Shader& vs, const Shader::VertexBinding& vb, const uint8_t* data,
                      xenos::Endian endian, uint32_t count, uint32_t bad_index) {
  std::string out = fmt::format("vs {:016X}  fetch constant {}  paso {} palabras  {} vertices\n\nAtributos:\n",
                                vs.ucode_data_hash(), vb.fetch_constant, vb.stride_words, count);
  for (const auto& a : vb.attributes) {
    out += fmt::format("  +{}w formato {} signed={} integer={} exp_adjust={}\n", a.fetch_instr.attributes.offset,
                       uint32_t(a.fetch_instr.attributes.data_format), a.fetch_instr.attributes.is_signed,
                       a.fetch_instr.attributes.is_integer, a.fetch_instr.attributes.exp_adjust);
  }
  auto dump = [&](uint32_t v, const char* label) {
    out += fmt::format("\n{} v{}:\n", label, v);
    for (uint32_t w = 0; w < vb.stride_words; ++w) {
      uint32_t raw;
      std::memcpy(&raw, data + v * vb.stride_words * 4 + w * 4, 4);
      raw = Swap32(raw, endian);
      float fl;
      std::memcpy(&fl, &raw, 4);
      out += fmt::format("  +{:2}w  {:08X}  {}\n", w, raw, fl);
    }
  };
  dump(bad_index, "MALO");
  for (uint32_t v = 0, n = 0; v < count && n < 2; ++v) {
    if (v == bad_index) continue;
    dump(v, "bueno");
    ++n;
  }
  out += "\nDesensamblado del sombreador de vertices:\n";
  out += vs.ucode_disassembly();
  std::error_code ec;
  std::filesystem::create_directories("logs", ec);
  const std::string path = fmt::format("logs/nan_{:016X}_fc{}.txt", vs.ucode_data_hash(), vb.fetch_constant);
  if (std::FILE* fp = std::fopen(path.c_str(), "wb")) {
    std::fwrite(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    REXGPU_WARN("odisea nan: detalle en {}", path);
  }
}

void MaybeSummarize() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (!g_window_start_ms) g_window_start_ms = now;
  if (now - g_window_start_ms < 10000) return;
  g_window_start_ms = now;
  std::vector<std::pair<std::string, Tally*>> active;
  for (auto& [key, t] : g_tally) {
    if (t.draws_window) active.emplace_back(key, &t);
  }
  if (active.empty()) return;
  std::sort(active.begin(), active.end(),
            [](const auto& a, const auto& b) { return a.second->draws_window > b.second->draws_window; });
  std::string out = fmt::format("odisea nan: resumen de 10 s, {} origenes con datos invalidos:", active.size());
  for (size_t i = 0; i < active.size() && i < 12; ++i) {
    out += fmt::format("\n  {} dibujos, {} valores: {}", active[i].second->draws_window,
                       active[i].second->bad_window, active[i].first);
  }
  REXGPU_WARN("{}", out);
  for (auto& [key, t] : g_tally) {
    t.draws_window = 0;
    t.bad_window = 0;
  }
}
}  // namespace

std::vector<PatchedVertexRange> CheckDrawForInvalidFloats(const RegisterFile& regs, const Shader& vertex_shader,
                                                          memory::Memory& memory) {
  std::vector<PatchedVertexRange> patched;
  const bool fix = REXCVAR_GET(odisea_vertex_nan_fix);
  const bool check = REXCVAR_GET(odisea_vertex_nan_check);
  if (!check && !fix) return patched;
  if (check) MaybeSummarize();
  const uint64_t vs_hash = vertex_shader.ucode_data_hash();

  // Constantes del sombreador de vertices (matrices de huesos, etc.).
  if (check) {
  const uint32_t vs_base = regs.Get<reg::SQ_VS_CONST>().base;
  const Shader::ConstantRegisterMap& map = vertex_shader.constant_register_map();
  for (uint32_t block = 0; block < 4; ++block) {
    uint64_t bits = map.float_bitmap[block];
    while (bits) {
      const uint32_t bit = uint32_t(std::countr_zero(bits));
      bits &= bits - 1;
      const uint32_t c = block * 64 + bit;
      const uint32_t reg = XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * ((vs_base + c) & 511);
      float v[4];
      std::memcpy(v, &regs.values[reg], 16);
      uint32_t bad = 0;
      for (float f : v) bad += BadConstant(f) ? 1 : 0;
      if (bad) {
        Note(fmt::format("vs {:016X} constante c{}", vs_hash, c), bad,
             fmt::format("({}, {}, {}, {}) base {}", v[0], v[1], v[2], v[3], vs_base));
      }
    }
  }
  }

  // Atributos de 32 bits en coma flotante de cada bufer.
  for (const Shader::VertexBinding& vb : vertex_shader.vertex_bindings()) {
    if (!vb.stride_words) continue;
    const xenos::xe_gpu_vertex_fetch_t f = regs.GetVertexFetch(vb.fetch_constant);
    uint8_t* data = memory.TranslatePhysical<uint8_t*>(uint32_t(f.address) << 2);
    if (!data) continue;
    const uint32_t stride = vb.stride_words * 4;
    const uint32_t count = std::min<uint32_t>((uint32_t(f.size) * 4) / stride, 65536);
    bool cleaned = false;
    for (const auto& a : vb.attributes) {
      uint32_t comps = 0;
      switch (a.fetch_instr.attributes.data_format) {
        case xenos::VertexFormat::k_32_FLOAT: comps = 1; break;
        case xenos::VertexFormat::k_32_32_FLOAT: comps = 2; break;
        case xenos::VertexFormat::k_32_32_32_FLOAT: comps = 3; break;
        case xenos::VertexFormat::k_32_32_32_32_FLOAT: comps = 4; break;
        default: break;
      }
      if (!comps) continue;
      const int32_t off = a.fetch_instr.attributes.offset;
      if (off < 0 || uint32_t(off) + comps > vb.stride_words) continue;
      uint32_t bad = 0, first = 0;
      float first_values[4] = {};
      for (uint32_t v = 0; v < count; ++v) {
        const uint8_t* p = data + v * stride + off * 4;
        for (uint32_t k = 0; k < comps; ++k) {
          if (BadVertex(LoadFloat(p + k * 4, f.endian))) {
            if (!bad++) {
              first = v;
              for (uint32_t j = 0; j < comps; ++j) first_values[j] = LoadFloat(p + j * 4, f.endian);
            }
            break;
          }
        }
      }
      if (bad && fix && vb.fetch_constant == 95) {
        for (uint32_t v = 0; v < count; ++v) {
          uint8_t* p = data + v * stride + off * 4;
          for (uint32_t k = 0; k < comps; ++k) {
            const float x = LoadFloat(p + k * 4, f.endian);
            if (!std::isfinite(x) || std::fabs(x) > 1e20f) {
              StoreFloat(p + k * 4, 0.0f, f.endian);
              cleaned = true;
            }
          }
        }
      }
      if (bad && check) {
        const bool is_new = Note(fmt::format("vs {:016X} fc {} atributo +{}w ({} comp., formato {}, paso {}w)", vs_hash,
                         vb.fetch_constant, off, comps, uint32_t(a.fetch_instr.attributes.data_format),
                         vb.stride_words),
             bad,
             fmt::format("{} de {} vertices, el primero v{} = ({}, {}, {}, {}) en 0x{:08X}", bad, count, first,
                         first_values[0], first_values[1], first_values[2], first_values[3],
                         uint32_t(f.address) << 2));
        if (is_new) DumpVertexOrigin(vertex_shader, vb, data, f.endian, count, first);
      }
    }
    if (cleaned) patched.push_back({uint32_t(f.address) << 2, uint32_t(f.size) << 2, vb.fetch_constant});
  }
  return patched;
}

bool DrawUsesWatchedTexture(const RegisterFile& regs, const Shader* pixel_shader) {
  if (!pixel_shader || !TextureWatchActive()) return false;
  const uint32_t page = WatchedTextureBasePage();
  for (const Shader::TextureBinding& b : pixel_shader->texture_bindings()) {
    const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(b.fetch_constant);
    if ((fetch.base_address & 0x1FFFF) == page) return true;
  }
  return false;
}

std::vector<PatchedVertexRange> ProcessWatchedDraw(const RegisterFile& regs, const Shader& vertex_shader,
                                                   const Shader* pixel_shader, memory::Memory& memory,
                                                   xenos::PrimitiveType primitive_type, uint32_t index_count) {
  (void)primitive_type;
  (void)index_count;
  std::vector<PatchedVertexRange> patched;
  const float target_w = float(REXCVAR_GET(odisea_watch_quad_width));
  const float target_h = float(REXCVAR_GET(odisea_watch_quad_height));
  if (target_w <= 0.0f || target_h <= 0.0f) return patched;
  if (!DrawUsesWatchedTexture(regs, pixel_shader)) return patched;
  const float dx = float(REXCVAR_GET(odisea_watch_quad_dx));
  const float max_v = float(REXCVAR_GET(odisea_watch_quad_max_v));

  for (const Shader::VertexBinding& vb : vertex_shader.vertex_bindings()) {
    // Posicion: float4 en +0; UV: float2. Si el formato no es el esperado, no
    // se toca nada.
    int pos_offset = -1, uv_offset = -1;
    for (const auto& a : vb.attributes) {
      const auto fmt = a.fetch_instr.attributes.data_format;
      if (fmt == xenos::VertexFormat::k_32_32_32_32_FLOAT && pos_offset < 0) {
        pos_offset = a.fetch_instr.attributes.offset;
      } else if (fmt == xenos::VertexFormat::k_32_32_FLOAT && uv_offset < 0) {
        uv_offset = a.fetch_instr.attributes.offset;
      }
    }
    if (pos_offset < 0 || uv_offset < 0 || vb.stride_words == 0) continue;
    const xenos::xe_gpu_vertex_fetch_t f = regs.GetVertexFetch(vb.fetch_constant);
    const uint32_t stride = vb.stride_words * 4;
    const uint32_t vertex_count = (f.size * 4) / stride;
    uint8_t* data = memory.TranslatePhysical<uint8_t*>(f.address << 2);
    if (!data) continue;

    // Los cuadrados van de 4 en 4 vertices.
    bool changed = false;
    for (uint32_t q = 0; q + 4 <= vertex_count; q += 4) {
      float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, v_max = -1.0f;
      for (uint32_t k = 0; k < 4; ++k) {
        const uint8_t* v = data + (q + k) * stride;
        const float x = LoadFloat(v + pos_offset * 4, f.endian);
        const float y = LoadFloat(v + pos_offset * 4 + 4, f.endian);
        const float vv = LoadFloat(v + uv_offset * 4 + 4, f.endian);
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
        v_max = std::max(v_max, vv);
      }
      if (!(v_max <= max_v + 1e-4f)) continue;  // el TM y lo que no sea tira o brillo
      const float w = x1 - x0, h = y1 - y0;
      if (w <= 0.0f || h <= 0.0f) continue;
      // Ya agrandado (el mismo bufer dibujado dos veces): no repetir.
      if (std::fabs(h - target_h) < 0.5f && std::fabs(w - target_w) < 0.5f) continue;
      const float cx = (x0 + x1) * 0.5f + dx;
      for (uint32_t k = 0; k < 4; ++k) {
        uint8_t* v = data + (q + k) * stride;
        const float x = LoadFloat(v + pos_offset * 4, f.endian);
        const float y = LoadFloat(v + pos_offset * 4 + 4, f.endian);
        const float nx = cx + (x - (x0 + x1) * 0.5f) * (target_w / w);
        const float ny = y1 - (y1 - y) * (target_h / h);
        StoreFloat(v + pos_offset * 4, nx, f.endian);
        StoreFloat(v + pos_offset * 4 + 4, ny, f.endian);
      }
      changed = true;
    }
    if (changed) {
      patched.push_back({uint32_t(f.address) << 2, uint32_t(f.size) << 2, vb.fetch_constant});
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXGPU_INFO("odisea: titulo agrandado a {}x{} (bufer 0x{:08X})", target_w, target_h, f.address << 2);
      }
    }
  }
  return patched;
}

namespace {

REXCVAR_DEFINE_BOOL(odisea_pass_check, false, "GPU/Debug",
                    "Diagnostico: catalogo de sombreadores por dibujo y aviso si un bufer de vertices "
                    "cambia entre dos dibujos del mismo fotograma");

struct PassUse {
  uint64_t hash;
  uint64_t vs_hash;
  uint64_t frame;
  uint32_t zfunc;
  bool z_write;
};

std::unordered_map<uint64_t, PassUse> pass_uses;
std::map<std::string, uint32_t> pass_mismatch_seen;
std::map<std::pair<uint64_t, uint64_t>, uint64_t> pass_pairs;  // (vs, ps) -> dibujos en el intervalo
uint64_t pass_draws = 0, pass_mismatches = 0;
uint32_t pass_zfunc_count[8] = {};
auto pass_last_summary = std::chrono::steady_clock::now();

void PassSummary() {
  auto now = std::chrono::steady_clock::now();
  if (now - pass_last_summary < std::chrono::seconds(10)) return;
  pass_last_summary = now;
  REXGPU_INFO(
      "odisea pasadas: resumen de 10 s: {} dibujos, {} buferes cambiados dentro del mismo fotograma, "
      "{} combinaciones vs/ps; zfunc never/less/equal/lequal/greater/notequal/gequal/always = "
      "{}/{}/{}/{}/{}/{}/{}/{}",
      pass_draws, pass_mismatches, pass_pairs.size(), pass_zfunc_count[0], pass_zfunc_count[1],
      pass_zfunc_count[2], pass_zfunc_count[3], pass_zfunc_count[4], pass_zfunc_count[5],
      pass_zfunc_count[6], pass_zfunc_count[7]);
  std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> top;
  for (const auto& [pair, draws] : pass_pairs) top.emplace_back(draws, pair);
  std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::string line;
  for (size_t i = 0; i < top.size() && i < 60; ++i) {
    line += fmt::format(" {:016X}/{:016X}={}", top[i].second.first, top[i].second.second, top[i].first);
  }
  REXGPU_INFO("odisea pasadas: dibujos por vs/ps en 10 s:{}", line);
  pass_draws = pass_mismatches = 0;
  std::memset(pass_zfunc_count, 0, sizeof(pass_zfunc_count));
  pass_pairs.clear();
  if (pass_uses.size() > 200000) pass_uses.clear();
}

}  // namespace

void CheckPassConsistency(const RegisterFile& regs, const Shader& vertex_shader, const Shader* pixel_shader,
                          memory::Memory& memory, uint64_t frame) {
  if (!REXCVAR_GET(odisea_pass_check)) return;
  PassSummary();
  const auto depth = regs.Get<reg::RB_DEPTHCONTROL>();
  const uint32_t zfunc = depth.z_enable ? uint32_t(depth.zfunc) : uint32_t(xenos::CompareFunction::kAlways);
  const bool z_write = depth.z_enable && depth.z_write_enable;
  ++pass_draws;
  ++pass_zfunc_count[zfunc & 7];
  const uint64_t vs_hash = vertex_shader.ucode_data_hash();
  const uint64_t ps_hash = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
  ++pass_pairs[{vs_hash, ps_hash}];

  // Catalogo: una linea por combinacion vs/ps la primera vez que se ve.
  static std::map<std::pair<uint64_t, uint64_t>, uint32_t> seen_pairs;
  if (seen_pairs.size() < 2000 && seen_pairs.emplace(std::make_pair(vs_hash, ps_hash), 1).second) {
    std::string attrs;
    for (const auto& vb : vertex_shader.vertex_bindings()) {
      attrs += fmt::format(" fc{} paso {}w [", vb.fetch_constant, vb.stride_words);
      for (const auto& a : vb.attributes) {
        attrs += fmt::format("f{}@{} ", uint32_t(a.fetch_instr.attributes.data_format),
                             a.fetch_instr.attributes.offset);
      }
      attrs += "]";
    }
    REXGPU_INFO("odisea pasadas: sombreador nuevo vs {:016X} ps {:016X} huesos {} zfunc {} escribe z {} "
                "mascara color {:X} texturas {} atributos:{}",
                vs_hash, ps_hash, vertex_shader.constant_register_map().float_dynamic_addressing ? 1 : 0, zfunc,
                z_write ? 1 : 0, regs.Get<reg::RB_COLOR_MASK>().value,
                pixel_shader ? pixel_shader->texture_bindings().size() : 0, attrs);
  }

  for (const auto& vb : vertex_shader.vertex_bindings()) {
    const auto f = regs.GetVertexFetch(vb.fetch_constant);
    const uint32_t address = uint32_t(f.address) << 2;
    const uint32_t size = uint32_t(f.size) << 2;
    if (!size || size > (8u << 20)) continue;
    const uint8_t* data = memory.TranslatePhysical<const uint8_t*>(address);
    if (!data) continue;
    const uint64_t hash = XXH3_64bits(data, size);
    const uint64_t key = (uint64_t(address) << 32) | size;
    auto it = pass_uses.find(key);
    if (it != pass_uses.end() && it->second.frame == frame && it->second.hash != hash) {
      ++pass_mismatches;
      const std::string id = fmt::format("vs {:016X} ps {:016X} fc {} tras vs {:016X}", vs_hash, ps_hash,
                                         vb.fetch_constant, it->second.vs_hash);
      if (pass_mismatch_seen.size() < 100 && pass_mismatch_seen.emplace(id, 1).second) {
        REXGPU_INFO("odisea pasadas: NUEVO {}: bufer 0x{:08X} ({} bytes) cambio entre dos dibujos del "
                    "fotograma {} (antes zfunc {}, escribe z {})",
                    id, address, size, frame, it->second.zfunc, it->second.z_write ? 1 : 0);
      }
    }
    pass_uses[key] = {hash, vs_hash, frame, zfunc, z_write};
  }
}

REXCVAR_DEFINE_STRING(odisea_dump_vs, "", "GPU/Debug",
                      "Diagnostico: vuelca a logs/vb/ los buferes de vertices de los dibujos que usan este "
                      "sombreador de vertices (huella hex) durante odisea_dump_vs_frames fotogramas");
REXCVAR_DEFINE_INT32(odisea_dump_vs_frames, 3, "GPU/Debug", "Fotogramas a volcar por odisea_dump_vs");

static void DumpShaderText(const Shader& shader, const char* kind) {
  const std::string path = fmt::format("logs/vb/{}_{:016X}.txt", kind, shader.ucode_data_hash());
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) return;
  FILE* out = std::fopen(path.c_str(), "wb");
  if (!out) return;
  for (const auto& vb : shader.vertex_bindings()) {
    std::fprintf(out, "binding fc %u stride %u words\n", vb.fetch_constant, vb.stride_words);
    for (const auto& a : vb.attributes) {
      const auto& at = a.fetch_instr.attributes;
      std::fprintf(out, "  attr r%u: format %u offset %u stride %u signed %d integer %d signed_rf_mode %u exp_adjust %d\n",
                   a.fetch_instr.result.storage_index, uint32_t(at.data_format), at.offset, at.stride,
                   at.is_signed ? 1 : 0, at.is_integer ? 1 : 0, uint32_t(at.signed_rf_mode), at.exp_adjust);
    }
  }
  for (const auto& t : shader.texture_bindings()) {
    std::fprintf(out, "texture fc %u dim %u\n", t.fetch_constant, uint32_t(t.fetch_instr.dimension));
  }
  const std::string dis = shader.ucode_disassembly();
  std::fprintf(out, "\n%s\n", dis.c_str());
  std::fclose(out);
}

void DumpVertexBuffers(const RegisterFile& regs, const Shader& vertex_shader, const Shader* pixel_shader,
                       memory::Memory& memory, uint64_t frame, const void* index_info_raw) {
  static std::string last;
  static uint64_t wanted = 0, first_frame = 0;
  static uint32_t draw_index = 0;
  const std::string cur = REXCVAR_GET(odisea_dump_vs);
  if (cur != last) {
    last = cur;
    wanted = cur.empty() ? 0 : (cur == "huesos" ? ~0ull : std::strtoull(cur.c_str(), nullptr, 16));
    first_frame = 0;
    draw_index = 0;
    if (wanted) REXGPU_INFO("odisea volcado: esperando dibujos del vs {:016X}", wanted);
  }
  if (!wanted) return;
  if (wanted == ~0ull) {
    if (!vertex_shader.constant_register_map().float_dynamic_addressing) return;
  } else if (vertex_shader.ucode_data_hash() != wanted) {
    return;
  }
  if (!first_frame) first_frame = frame;
  const uint64_t frames = uint64_t(std::max(REXCVAR_GET(odisea_dump_vs_frames), 1));
  if (frame >= first_frame + frames) {
    if (wanted) {
      REXGPU_INFO("odisea volcado: terminado ({} dibujos en {} fotogramas)", draw_index, frames);
      wanted = 0;
    }
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories("logs/vb", ec);
  DumpShaderText(vertex_shader, "vs");
  if (pixel_shader) DumpShaderText(*pixel_shader, "ps");
  // Estado por dibujo (registros y constantes), para comparar pasadas repetidas.
  if (FILE* st = std::fopen("logs/vb/draws.txt", "ab")) {
    const auto* ii = static_cast<const IndexDumpInfo*>(index_info_raw);
    std::fprintf(st, "f%llu d%03u ps %016llX idx %u | depthcontrol %08X colormask %08X blend0 %08X colorcontrol %08X "
                 "sc_mode_cntl %08X stencilref %08X depthinfo %08X surfaceinfo %08X polyoffset f %g/%g b %g/%g alpharef %g\n",
                 (unsigned long long)(frame - first_frame + 1), draw_index,
                 (unsigned long long)(pixel_shader ? pixel_shader->ucode_data_hash() : 0), ii ? ii->count : 0,
                 regs[XE_GPU_REG_RB_DEPTHCONTROL], regs[XE_GPU_REG_RB_COLOR_MASK], regs[XE_GPU_REG_RB_BLENDCONTROL0],
                 regs[XE_GPU_REG_RB_COLORCONTROL], regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL], regs[XE_GPU_REG_RB_STENCILREFMASK],
                 regs[XE_GPU_REG_RB_DEPTH_INFO], regs[XE_GPU_REG_RB_SURFACE_INFO],
                 regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE), regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET),
                 regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE), regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET),
                 regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF));
    const uint32_t vs_base = regs.Get<reg::SQ_VS_CONST>().base;
    const uint32_t ps_base = regs.Get<reg::SQ_PS_CONST>().base;
    for (int which = 0; which < 2; ++which) {
      const uint32_t base = which ? ps_base : vs_base;
      std::fprintf(st, "   %s:", which ? "ps" : "vs");
      for (uint32_t c = 0; c < 256; ++c) {
        const bool all_vs = !which && vertex_shader.constant_register_map().float_dynamic_addressing;
        if (!all_vs && c > 16 && c != 255) continue;
        float v[4];
        std::memcpy(v, &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * ((base + c) & 511)], 16);
        std::fprintf(st, " c%u=(%.6g,%.6g,%.6g,%.6g)", c, v[0], v[1], v[2], v[3]);
      }
      std::fprintf(st, "\n");
    }
    std::fclose(st);
  }
  if (index_info_raw) {
    const auto* ii = static_cast<const IndexDumpInfo*>(index_info_raw);
    const uint8_t* idx = memory.TranslatePhysical<const uint8_t*>(ii->guest_base);
    if (idx && ii->length && ii->length < (4u << 20)) {
      const std::string path = fmt::format("logs/vb/f{}_d{:03}_idx_{}bit_e{}_n{}.bin", frame - first_frame + 1,
                                           draw_index, ii->is_32bit ? 32 : 16, ii->endian, ii->count);
      if (FILE* out = std::fopen(path.c_str(), "wb")) {
        std::fwrite(idx, 1, ii->length, out);
        std::fclose(out);
      }
    }
  }
  for (const auto& vb : vertex_shader.vertex_bindings()) {
    const auto f = regs.GetVertexFetch(vb.fetch_constant);
    const uint32_t address = uint32_t(f.address) << 2;
    const uint32_t size = uint32_t(f.size) << 2;
    if (!size || size > (8u << 20)) continue;
    const uint8_t* data = memory.TranslatePhysical<const uint8_t*>(address);
    if (!data) continue;
    const std::string path = fmt::format("logs/vb/f{}_d{:03}_ps{:016X}_fc{}_{:08X}_{}w.bin", frame - first_frame + 1,
                                         draw_index, pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                                         vb.fetch_constant, address, vb.stride_words);
    if (FILE* out = std::fopen(path.c_str(), "wb")) {
      std::fwrite(data, 1, size, out);
      std::fclose(out);
    }
  }
  ++draw_index;
}

REXCVAR_DEFINE_STRING(odisea_skip_shader, "", "GPU/Debug",
                      "Diagnostico: no dibuja nada que use estos sombreadores (huellas en hexadecimal "
                      "separadas por comas, de vertices o de pixeles; se cambia en caliente)");

REXCVAR_DEFINE_STRING(odisea_skip_face, "", "GPU/Debug",
                      "Diagnostico: no dibuja los dibujos cuya cara frontal es 'cw' o 'ccw' (vacio = todo)");

REXCVAR_DEFINE_BOOL(odisea_hide_hud, false, "GPU/Debug",
                    "Diagnostico: no dibuja la interfaz (dibujos 2D con la proyeccion de diseno "
                    "1280x720 y sin profundidad; se cambia en caliente)");

bool IsDesignHudDraw(const RegisterFile& regs) {
  // Interfaz = matriz 2D en pixeles del lienzo de diseno 1280x720 (volcados del 6-oct).
  // Lo normal es la proyeccion exacta (c0.x = 2/1280, c1.y = -2/720, c3 = (-1 - 1/1280,
  // 1 + 1/720)), pero el cursor y los puntos del minimapa llevan giro y escala dentro
  // de la matriz: se acepta cualquier 2x2 de escala de pixeles (|v| < 0.01) con
  // c2 = (0,0,1,0) y c3.zw = (0,1). La copia de la escena con el mismo sombreador usa
  // la identidad (escala 1) y el Clear del XDK no escribe color.
  if (regs[XE_GPU_REG_RB_DEPTHCONTROL] & 0x7) return false;
  if (!(regs[XE_GPU_REG_RB_COLOR_MASK] & 0xF)) return false;
  auto c = [&](uint32_t i) { return regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i); };
  if (c(2) != 0.0f || c(3) != 0.0f || c(6) != 0.0f || c(7) != 0.0f) return false;
  if (c(8) != 0.0f || c(9) != 0.0f || c(10) != 1.0f || c(11) != 0.0f) return false;
  if (c(14) != 0.0f || c(15) != 1.0f) return false;
  const float m00 = c(0), m01 = c(1), m10 = c(4), m11 = c(5);
  if (std::fabs(m00) > 0.01f || std::fabs(m01) > 0.01f || std::fabs(m10) > 0.01f ||
      std::fabs(m11) > 0.01f) {
    return false;
  }
  return std::fabs(m00 * m11 - m01 * m10) > 1e-9f;
}

REXCVAR_DEFINE_INT32(odisea_hud_resolution, 0, "GPU",
                     "odisea: alto de la resolucion de salida con la interfaz dibujada aparte "
                     "(720, 1080, 1440, 2160...; el 3D sigue a la escala del preset). 0 = la "
                     "interfaz va con el 3D, como en el juego original. Se cambia en caliente.");

REXCVAR_DEFINE_INT32(odisea_hud_output_width, 0, "GPU",
                     "odisea: ancho de la salida con la interfaz aparte (0 = 16:9 a partir de "
                     "odisea_hud_resolution). Con otro aspecto (16:10, 21:9) la interfaz va en "
                     "un rectangulo 16:9 centrado.");

uint32_t HudOutputHeight() {
  int32_t height = REXCVAR_GET(odisea_hud_resolution);
  return height >= 360 && height <= 4320 ? uint32_t(height) : 0;
}

uint32_t HudOutputWidth() {
  uint32_t height = HudOutputHeight();
  if (!height) return 0;
  int32_t width = REXCVAR_GET(odisea_hud_output_width);
  if (width >= 360 && width <= 10240) return uint32_t(width);
  return (height * 1280 + 360) / 720;
}

bool IsHudMainSurfaceDraw(const RegisterFile& regs) {
  // Solo la pantalla principal (backbuffer de 1280x720, o el de 16:10 / 21:9)
  // sin MSAA, con un viewport que la cubre desde la esquina y escribiendo nada
  // mas que el destino 0 en 8_8_8_8: lo que se dibuja asi en otras superficies
  // (texturas que el juego lee despues) se queda en la EDRAM.
  uint32_t surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
  uint32_t pitch = surface_info & 0x3FFF;
  if (pitch < 1024 || pitch > 1280 || ((surface_info >> 16) & 3) != 0) return false;
  uint32_t color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  if (!(color_mask & 0xF) || (color_mask & 0xFFF0)) return false;
  uint32_t color_format = (regs[XE_GPU_REG_RB_COLOR_INFO] >> 16) & 0xF;
  if (color_format != 0 && color_format != 1) return false;
  float x_scale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE);
  float y_scale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE);
  return x_scale * 2.0f == float(pitch) &&
         regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) == x_scale && y_scale <= -256.0f &&
         y_scale >= -360.0f && regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) == -y_scale;
}

REXCVAR_DEFINE_BOOL(odisea_skip_crossfade, false, "GPU",
                    "Quita el fundido entre escenas (la imagen anterior encima de la nueva, que se va "
                    "desvaneciendo). Se cambia en caliente.");

namespace {
std::atomic<uint32_t> g_frontbuffer_base{0};

// Textura del tamano de la pantalla principal en algun fetch del sombreador de pixeles.
bool ScreenSizedTexture(const RegisterFile& regs, const Shader* pixel_shader, uint32_t& base) {
  if (!pixel_shader) return false;
  const uint32_t width = regs[XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
  const uint32_t height = uint32_t(2.0f * std::fabs(regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE)) + 0.5f);
  for (const auto& binding : pixel_shader->texture_bindings()) {
    const auto fetch = regs.GetTextureFetch(binding.fetch_constant);
    const uint32_t w = (fetch.dword_2 & 0x1FFF) + 1, h = ((fetch.dword_2 >> 13) & 0x1FFF) + 1;
    if (w == width && h == height) {
      base = fetch.dword_1 & 0x1FFFF000u;
      return true;
    }
  }
  return false;
}
}  // namespace

void NoteFrontbuffer(uint32_t frontbuffer_ptr) {
  g_frontbuffer_base.store(frontbuffer_ptr & 0x1FFFF000u, std::memory_order_relaxed);
}

bool IsScreenCopyDraw(const RegisterFile& regs, const Shader* pixel_shader) {
  uint32_t base = 0;
  return IsDesignHudDraw(regs) && IsHudMainSurfaceDraw(regs) && ScreenSizedTexture(regs, pixel_shader, base);
}

bool ShouldSkipDraw(const RegisterFile& regs, const Shader& vertex_shader, const Shader* pixel_shader) {
  {
    if (REXCVAR_GET(odisea_hide_hud) && IsDesignHudDraw(regs)) return true;
  }
  // Fundido entre escenas (8-oct-2026): la imagen guardada (14EB0000) pegada encima con
  // transparencia (mezcla 01000706) por el sombreador 5326028A693C45BB, que solo lee c0 del de pixeles:
  // la opacidad. Al cambiar de escena va de 1 a 0 en ~250 ms; en el menu y el orbe (oscurecido) se
  // queda en 1. Con c0 = 1 no se toca nunca: en el menu y en el primer fotograma del cambio el juego
  // no redibuja el 3D y saltarla deja ver basura (destello corrupto). Por debajo de 1 la escena nueva
  // ya esta debajo: saltarla es un corte limpio.
  if (REXCVAR_GET(odisea_skip_crossfade) && pixel_shader &&
      pixel_shader->ucode_data_hash() == 0x5326028A693C45BBull &&
      regs[XE_GPU_REG_RB_BLENDCONTROL0] == 0x01000706u && IsHudMainSurfaceDraw(regs)) {
    const uint32_t c0 = XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * (regs.Get<reg::SQ_PS_CONST>().base & 511);
    if (regs.Get<float>(c0 + 3) < 0.999f) return true;
  }
  {
    const std::string face = REXCVAR_GET(odisea_skip_face);
    if (!face.empty() && face != "0") {
      const bool cw = ((regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL] >> 2) & 1) != 0;
      if ((face == "cw" && cw) || (face == "ccw" && !cw)) return true;
    }
  }
  static std::string last;
  static std::vector<uint64_t> hashes;
  const std::string cur = REXCVAR_GET(odisea_skip_shader);
  if (cur != last) {
    last = cur;
    hashes.clear();
    size_t pos = 0;
    while (pos < cur.size()) {
      size_t end = cur.find(',', pos);
      if (end == std::string::npos) end = cur.size();
      std::string item = cur.substr(pos, end - pos);
      item.erase(std::remove_if(item.begin(), item.end(), [](char c) { return c == ' '; }), item.end());
      if (!item.empty()) hashes.push_back(std::strtoull(item.c_str(), nullptr, 16));
      pos = end + 1;
    }
    REXGPU_INFO("odisea: ocultando {} sombreadores ({})", hashes.size(), cur);
  }
  if (hashes.empty()) return false;
  for (uint64_t h : hashes) {
    if (vertex_shader.ucode_data_hash() == h) return true;
    if (pixel_shader && pixel_shader->ucode_data_hash() == h) return true;
  }
  return false;
}

// --- Prueba de profundidad forzada (diagnostico del parpadeo del abrigo) -------
REXCVAR_DEFINE_STRING(odisea_force_zfunc_shader, "", "GPU/Debug",
                      "Hashes (vs o ps, separados por comas) cuyos dibujos usan odisea_force_zfunc");
REXCVAR_DEFINE_INT32(odisea_force_zfunc, 7, "GPU/Debug",
                     "Funcion de profundidad forzada: 0 nunca, 1 <, 2 =, 3 <=, 4 >, 5 !=, 6 >=, 7 siempre");

DepthFuncOverride::DepthFuncOverride(RegisterFile& regs, const Shader& vertex_shader,
                                     const Shader* pixel_shader) {
  static std::string last;
  static std::vector<uint64_t> hashes;
  const std::string cur = REXCVAR_GET(odisea_force_zfunc_shader);
  if (cur != last) {
    last = cur;
    hashes.clear();
    size_t pos = 0;
    while (pos < cur.size()) {
      size_t end = cur.find(',', pos);
      if (end == std::string::npos) end = cur.size();
      std::string item = cur.substr(pos, end - pos);
      item.erase(std::remove_if(item.begin(), item.end(), [](char c) { return c == ' '; }), item.end());
      if (!item.empty() && item != "0") hashes.push_back(std::strtoull(item.c_str(), nullptr, 16));
      pos = end + 1;
    }
    REXGPU_INFO("odisea: profundidad forzada a {} en {} sombreadores ({})",
                REXCVAR_GET(odisea_force_zfunc), hashes.size(), cur);
  }
  if (hashes.empty()) return;
  bool match = false;
  for (uint64_t h : hashes) {
    if (vertex_shader.ucode_data_hash() == h || (pixel_shader && pixel_shader->ucode_data_hash() == h)) {
      match = true;
      break;
    }
  }
  if (!match) return;
  regs_ = &regs;
  old_ = regs[XE_GPU_REG_RB_DEPTHCONTROL];
  // RB_DEPTHCONTROL: zfunc en los bits 4-6.
  const uint32_t zfunc = uint32_t(std::clamp(int(REXCVAR_GET(odisea_force_zfunc)), 0, 7));
  regs[XE_GPU_REG_RB_DEPTHCONTROL] = (old_ & ~(7u << 4)) | (zfunc << 4);
}

DepthFuncOverride::~DepthFuncOverride() {
  if (regs_) (*regs_)[XE_GPU_REG_RB_DEPTHCONTROL] = old_;
}

// --- Sombras dinamicas mas suaves ---------------------------------------------
// El proyector de sombras del juego (UE3) filtra el mapa de sombras de 864x864 con
// varias muestras desplazadas (constantes "SampleOffsets", del orden de
// 0,5/864..1,5/864). Multiplicar esos desplazamientos ensancha el filtro: borde
// de sombra mas suave, sin escalones ni picos. Se reconoce el dibujo porque su
// sombreador de pixeles lee una textura 2D de 864x864 (el tamano lo fija el
// juego; ver sub_824DD6F8) y solo se tocan las constantes float4 cuyos cuatro
// componentes son desplazamientos pequenos (|v| <= ~8,7 texeles), nunca
// ShadowBufferSize (864) ni matrices.
REXCVAR_DEFINE_DOUBLE(odisea_shadow_softness, 1.0, "GPU",
                      "Suavizado de las sombras dinamicas: multiplica el radio del filtrado del "
                      "mapa de sombras (1 = original del juego, 2-4 = mas suave)");
// Al ensanchar el filtro, las muestras lejanas caen sobre la propia superficie
// inclinada que recibe la sombra y se "autosombrea": la sombra crece y parece
// moverse en vez de difuminarse. Se compensa restando un sesgo a la profundidad
// del receptor (columna z de la fila de traslacion c5 del proyector B0CB…: la
// profundidad es x*c2.z + y*c3.z + z*c4.z + c5.z), proporcional a lo que se ha
// ensanchado el filtro (factor - 1).
REXCVAR_DEFINE_DOUBLE(odisea_shadow_bias, 0.0, "GPU",
                      "Sesgo de profundidad extra por unidad de suavizado (se multiplica por "
                      "odisea_shadow_softness - 1) para que la sombra no crezca al difuminarla");
// Autosombreado de los personajes: el proyector compara la profundidad del receptor
// (prepasada de la escena) con el mapa de sombras del propio personaje; con el sesgo
// del juego sobran triangulos sueltos del abrigo de Jansen que se autosombrean y
// cambian con la animacion (manchas oscuras que parpadean, tambien en Xenia). Este
// sesgo fijo se resta siempre a la profundidad del receptor (c5.z de los dos
// proyectores, el lejano B0CB y el cercano 1BB9); positivo = menos autosombreado.
// 0,03 elegido probando en la nieve con la camara cerca: sin parpadeos; se pierde
// algo de autosombreado fino (cuello sobre el pecho), la sombra del suelo se queda.
REXCVAR_DEFINE_DOUBLE(odisea_shadow_receiver_bias, 0.03, "GPU",
                      "Sesgo fijo de profundidad del receptor de sombras dinamicas (quita el "
                      "autosombreado en triangulos sueltos que parpadea; 0 = el del juego)");
REXCVAR_DEFINE_BOOL(odisea_shadow_probe, false, "GPU/Debug",
                    "Anota los sombreadores del proyector de sombras y las constantes que se suavizan");

namespace {
// Radio maximo de los desplazamientos del filtrado del proyector de este dibujo,
// en coordenadas de textura (7,5 texeles del mapa de sombras que lee).
thread_local float g_shadow_offset_max = 7.5f / 864.0f + 0.001f;
thread_local uint64_t g_shadow_ps = 0;
constexpr uint64_t kShadowProjectorPs = 0xB0CBB6E5FD9526DEull;
// Huella del proyector tras el parche de penumbra lineal (PatchShaderUcode): se
// cambia para que la traduccion y los pipelines guardados no se confundan.
constexpr uint64_t kPatchedHashXor = 0x0D15EA5E0D15EA5Eull;
constexpr uint64_t kShadowProjectorPsLinear = kShadowProjectorPs ^ kPatchedHashXor;
// Variante cercana del proyector (camara cerca del personaje): mismas filas c2-c5
// de proyeccion (profundidad del receptor = columna z, c5.z), 8 muestras fijas y un
// bucle de muestras extra en los bordes de la sombra.
constexpr uint64_t kShadowProjectorPsNear = 0x1BB9C39F84C94FA0ull;
}  // namespace

float ShadowSoftenFactor(const RegisterFile& regs, const Shader* pixel_shader) {
  if (!pixel_shader) return 1.0f;
  const float factor = float(REXCVAR_GET(odisea_shadow_softness));
  const bool probe = REXCVAR_GET(odisea_shadow_probe);
  const bool receiver_bias = REXCVAR_GET(odisea_shadow_receiver_bias) != 0.0;
  if (factor == 1.0f && !probe && !receiver_bias) return 1.0f;
  // Fork (Odisea): el fetch constant que el sombreador lee 4 o mas veces no cambia por sombreador;
  // se busca una vez (antes era un bucle doble por dibujo). Con la sonda se usa el camino completo.
  if (!probe) {
    static std::unordered_map<const Shader*, int32_t> pcf_fetch_by_shader;
    auto [it, inserted] = pcf_fetch_by_shader.try_emplace(pixel_shader, -1);
    if (inserted) {
      const auto& bindings = pixel_shader->texture_bindings();
      for (size_t i = 0; i < bindings.size() && it->second < 0; ++i) {
        uint32_t count = 0;
        for (const auto& b : bindings) count += b.fetch_constant == bindings[i].fetch_constant;
        if (count >= 4) it->second = int32_t(bindings[i].fetch_constant);
      }
    }
    if (it->second < 0) return 1.0f;
    const auto fetch = regs.GetTextureFetch(uint32_t(it->second));
    const bool depth = fetch.format == xenos::TextureFormat::k_24_8 ||
                       fetch.format == xenos::TextureFormat::k_24_8_FLOAT ||
                       fetch.format == xenos::TextureFormat::k_16;
    if (!depth) return 1.0f;
    const uint32_t w = fetch.size_2d.width + 1, h = fetch.size_2d.height + 1;
    g_shadow_offset_max = 7.5f / float(std::max(w, h)) + 0.001f;
    g_shadow_ps = pixel_shader->ucode_data_hash();
    if (factor == 1.0f && receiver_bias) return std::nextafter(1.0f, 2.0f);
    return factor;
  }
  // Un proyector de sombras lee el mismo mapa de profundidad muchas veces (filtrado
  // PCF): 4 o mas fetches sobre el mismo fetch constant de una textura de
  // profundidad. Asi se cogen todas las variantes (personajes, enemigos, suelo),
  // sea cual sea el tamano del mapa.
  const auto& bindings = pixel_shader->texture_bindings();
  for (size_t i = 0; i < bindings.size(); ++i) {
    const uint32_t fc = bindings[i].fetch_constant;
    uint32_t count = 0;
    for (const auto& b : bindings) count += b.fetch_constant == fc ? 1 : 0;
    if (count < 4) continue;
    const auto fetch = regs.GetTextureFetch(fc);
    const bool depth = fetch.format == xenos::TextureFormat::k_24_8 ||
                       fetch.format == xenos::TextureFormat::k_24_8_FLOAT ||
                       fetch.format == xenos::TextureFormat::k_16;
    const uint32_t w = fetch.size_2d.width + 1, h = fetch.size_2d.height + 1;
    if (probe) {
      static std::map<std::pair<uint64_t, uint32_t>, uint32_t> seen;
      if (seen.size() < 128 &&
          seen.emplace(std::make_pair(pixel_shader->ucode_data_hash(), w * 65536 + h), 1).second) {
        REXGPU_INFO("odisea sombras: ps {:016X} lee {} veces fc {} ({}x{}, formato {}){}",
                    pixel_shader->ucode_data_hash(), count, fc, w, h, uint32_t(fetch.format),
                    depth ? " -> proyector de sombras" : "");
        if (depth) {
          std::error_code ec;
          std::filesystem::create_directories("logs/vb", ec);
          DumpShaderText(*pixel_shader, "ps_sombra");
        }
      }
    }
    if (!depth) return 1.0f;
    g_shadow_offset_max = 7.5f / float(std::max(w, h)) + 0.001f;
    g_shadow_ps = pixel_shader->ucode_data_hash();
    // Con sesgo fijo y sin suavizado hay que pasar por SoftenShadowConstant igual:
    // un factor minimamente distinto de 1 no cambia el filtrado.
    if (factor == 1.0f && receiver_bias) return std::nextafter(1.0f, 2.0f);
    return factor;
  }
  return 1.0f;
}

namespace {
// Tabla global inicializada al cargar la DLL (un static local costaria una guarda con TLS por llamada).
std::vector<uint8_t> MakeRegisterSkipTable() {
  std::vector<uint8_t> t(RegisterFile::kRegisterCount, 1);
  for (uint32_t r = XE_GPU_REG_SCRATCH_REG0; r <= XE_GPU_REG_SCRATCH_REG7; ++r) t[r] = 0;
  t[XE_GPU_REG_COHER_STATUS_HOST] = 0;
  t[XE_GPU_REG_DC_LUT_RW_INDEX] = 0;
  t[XE_GPU_REG_DC_LUT_SEQ_COLOR] = 0;
  t[XE_GPU_REG_DC_LUT_PWL_DATA] = 0;
  t[XE_GPU_REG_DC_LUT_30_COLOR] = 0;
  return t;
}
const std::vector<uint8_t> g_register_skip_table = MakeRegisterSkipTable();
// Registros "planos": escribirlos solo guarda el valor. CommandProcessor::WriteRegister solo hace
// algo mas con scratch, COHER y la rampa de gamma, y los backends (D3D12/Vulkan) con las constantes
// de sombreador 0x4000-0x4927 (float, fetch, bool y loop).
std::vector<uint8_t> MakeRegisterPlainTable() {
  std::vector<uint8_t> t = MakeRegisterSkipTable();
  for (uint32_t r = XE_GPU_REG_SHADER_CONSTANT_000_X; r <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31; ++r) {
    t[r] = 0;
  }
  return t;
}
const std::vector<uint8_t> g_register_plain_table = MakeRegisterPlainTable();
std::vector<uint8_t> MakeKnownRegisterTable() {
  std::vector<uint8_t> t(RegisterFile::kRegisterCount, 0);
  for (uint32_t i = 0; i < RegisterFile::kRegisterCount; ++i) {
    t[i] = RegisterFile::GetRegisterInfo(i) != nullptr;
  }
  return t;
}
const std::vector<uint8_t> g_known_register_table = MakeKnownRegisterTable();
}  // namespace

const uint8_t* RegisterWriteSkipTable() { return g_register_skip_table.data(); }
const uint8_t* RegisterPlainWriteTable() { return g_register_plain_table.data(); }
const uint8_t* KnownRegisterTable() { return g_known_register_table.data(); }

bool IsRedundantRegisterWrite(const uint32_t* values, uint32_t index, uint32_t value) {
  return index < RegisterFile::kRegisterCount && values[index] == value &&
         g_register_skip_table.data()[index];
}

void SoftenShadowConstant(uint32_t constant_index, float* v, float factor) {
  // Proyector B0CBB6E5FD9526DE: c2-c5 son las filas de la matriz de proyeccion al
  // mapa de sombras (componentes pequenos porque el mundo es enorme, pero con w = 0)
  // y c6-c13 los 16 desplazamientos del filtrado (dos por float4, los cuatro
  // componentes distintos de cero). Escalar la matriz desplaza la sombra entera,
  // asi que solo se tocan las float4 con los cuatro componentes pequenos y no nulos.
  if (constant_index == 5 &&
      (g_shadow_ps == kShadowProjectorPs || g_shadow_ps == kShadowProjectorPsLinear ||
       g_shadow_ps == kShadowProjectorPsNear)) {
    const float bias = float(REXCVAR_GET(odisea_shadow_bias)) * (factor - 1.0f) +
                       float(REXCVAR_GET(odisea_shadow_receiver_bias));
    if (REXCVAR_GET(odisea_shadow_probe)) {
      static uint32_t logged = 0;
      if (logged < 16) {
        ++logged;
        REXGPU_INFO("odisea sombras: c5 ({}, {}, {}, {}) sesgo extra {}", v[0], v[1], v[2], v[3], bias);
      }
    }
    v[2] -= bias;
    return;
  }
  const float kMax = g_shadow_offset_max;
  for (int k = 0; k < 4; ++k) {
    if (!(std::fabs(v[k]) <= kMax) || v[k] == 0.0f) return;  // tambien descarta NaN
  }
  if (REXCVAR_GET(odisea_shadow_probe)) {
    static uint32_t logged = 0;
    if (logged < 64) {
      ++logged;
      REXGPU_INFO("odisea sombras: c{} ({}, {}, {}, {}) x{}", constant_index, v[0], v[1], v[2], v[3],
                  factor);
    }
  }
  for (int k = 0; k < 4; ++k) v[k] *= factor;
}

// --- Penumbra lineal del proyector de sombras -----------------------------------
// El proyector B0CB… termina con: x = z*z (muls, instr. 64); y = x*z (mul, 65);
// x = 1 - y; color = x*c14.x + y, es decir, eleva al CUBO la fraccion de muestras
// iluminadas z. Eso concentra la penumbra en un borde casi duro: por mucho que se
// ensanche el filtro no se ve difuminada. Cambiando el opcode vectorial de la
// instruccion 65 de MUL a MAX, y = max(z*z, z) = z para z en [0,1]: la sombra pasa
// a ser lineal y la penumbra del filtrado se ve entera.
// El mapa de sombras se lee sin filtrar: cada muestra compara la profundidad de un
// solo texel, asi que al moverse el personaje o la camara el borde salta de texel en
// texel ("tiembla"). Con filtrado bilineal de la profundidad la comparacion usa un
// valor interpolado y el borde se desplaza de forma continua.
REXCVAR_DEFINE_BOOL(odisea_shadow_bilinear, true, "GPU",
                    "Filtrado bilineal del mapa de sombras (borde estable en movimiento)");

bool ShadowMapBilinear(const xenos::xe_gpu_texture_fetch_t& fetch) {
  if (!REXCVAR_GET(odisea_shadow_bilinear)) return false;
  return fetch.size_2d.width == 863 && fetch.size_2d.height == 863 &&
         (fetch.format == xenos::TextureFormat::k_24_8 ||
          fetch.format == xenos::TextureFormat::k_24_8_FLOAT);
}

REXCVAR_DEFINE_BOOL(odisea_shadow_linear, true, "GPU",
                    "Penumbra lineal en las sombras dinamicas (el juego eleva el resultado al cubo, "
                    "lo que endurece el borde). Requiere reiniciar.");

namespace {
// El juego pide el sombreador en cada SetPixelShader y la cache lo busca por
// huella: el parche se hace UNA vez; despues solo se traduce la huella.
std::atomic<bool> g_linear_patch_failed{false};
}  // namespace

uint64_t PatchedShaderHash(uint64_t hash) {
  if (hash != kShadowProjectorPs || !REXCVAR_GET(odisea_shadow_linear) ||
      g_linear_patch_failed.load(std::memory_order_relaxed)) {
    return hash;
  }
  return hash ^ kPatchedHashXor;
}

uint64_t PatchShaderUcode(const uint32_t* guest_dwords, uint32_t dword_count, uint64_t hash,
                          std::vector<uint32_t>& patched) {
  patched.clear();
  if (PatchedShaderHash(hash) == hash) return hash;
  constexpr uint32_t kInstr = 65;
  if (dword_count < (kInstr + 1) * 3) {
    g_linear_patch_failed = true;
    return hash;
  }
  patched.assign(guest_dwords, guest_dwords + dword_count);
  // Las palabras vienen en el orden de bytes de la consola (big-endian).
  const uint32_t w0 = __builtin_bswap32(patched[kInstr * 3 + 0]);
  uint32_t w2 = __builtin_bswap32(patched[kInstr * 3 + 2]);
  const uint32_t vector_dest = w0 & 0x3F;
  const uint32_t vector_write_mask = (w0 >> 16) & 0xF;
  const uint32_t vector_opc = (w2 >> 24) & 0x1F;
  if (vector_dest != 0 || vector_write_mask != 0x2 || vector_opc != 1 /* kMul */) {
    REXGPU_WARN("odisea sombras: el proyector no tiene la forma esperada (dest {} mascara {:X} "
                "opcode {}); no se aplica la penumbra lineal",
                vector_dest, vector_write_mask, vector_opc);
    g_linear_patch_failed = true;
    patched.clear();
    return hash;
  }
  w2 = (w2 & ~(0x1Fu << 24)) | (2u /* kMax */ << 24);
  patched[kInstr * 3 + 2] = __builtin_bswap32(w2);
  REXGPU_INFO("odisea sombras: penumbra lineal aplicada al proyector {:016X} -> {:016X}", hash,
              hash ^ kPatchedHashXor);
  return hash ^ kPatchedHashXor;
}

// --- Progreso de la preparacion de sombreadores (pantalla del arranque) ----------
// La caché de sombreadores y pipelines se traduce y compila al arrancar el juego
// (bloqueando). El exe consulta este progreso para pintar su pantalla.
namespace {
std::atomic<int> g_prep_phase{0};
std::atomic<uint32_t> g_prep_done{0};
std::atomic<uint32_t> g_prep_total{0};
}  // namespace

// --- Estadistica de pipelines creados en partida ----------------------------------
// Cuantos pipelines nuevos aparecen jugando y cuantos dibujos se saltan mientras
// se compilan en segundo plano (aparicion tardia). Una linea cada 10 s si hubo.
namespace {
std::atomic<uint32_t> g_new_pipelines{0}, g_skipped_draws{0};
std::atomic<uint32_t> g_noeffect_draws{0};
std::atomic<uint32_t> g_occlusion_events[4] = {};
std::atomic<uint64_t> g_draws{0};
std::atomic<uint64_t> g_frames{0};
std::atomic<int64_t> g_stats_last_ms{0};
void MaybeLogPipelineStats() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  int64_t last = g_stats_last_ms.load(std::memory_order_relaxed);
  if (now - last < 10000) return;
  if (!g_stats_last_ms.compare_exchange_strong(last, now)) return;
  const uint32_t created = g_new_pipelines.exchange(0), skipped = g_skipped_draws.exchange(0);
  const uint64_t draws = g_draws.exchange(0), frames = g_frames.exchange(0);
  const uint32_t noeffect = g_noeffect_draws.exchange(0);
  uint32_t occlusion[4];
  for (int i = 0; i < 4; ++i) occlusion[i] = g_occlusion_events[i].exchange(0);
  REXGPU_INFO("odisea rendimiento: {:.1f} fps, {} dibujos por fotograma; pipelines {} nuevos, {} "
              "dibujos saltados esperando compilacion (10 s); {} dibujos sin efecto omitidos por "
              "fotograma; occlusion queries por fotograma: {} iniciadas, {} leidas, {} falsas, {} "
              "sin host",
              double(frames) / ((now - last) / 1000.0), frames ? draws / frames : 0, created, skipped,
              frames ? noeffect / frames : 0, frames ? occlusion[0] / frames : 0,
              frames ? occlusion[1] / frames : 0, frames ? occlusion[2] / frames : 0,
              frames ? occlusion[3] / frames : 0);
}
}  // namespace

// --- Captura de un fotograma del juego con RenderDoc ---------------------------
// RenderDoc corta los fotogramas en el Present de la ventana, que va por otro
// hilo: con F12 solo sale la copia final. Con odisea_renderdoc_capture = true (en
// caliente) se captura exactamente un fotograma del juego, de un IssueSwap al
// siguiente.
REXCVAR_DEFINE_BOOL(odisea_renderdoc_capture, false, "GPU/Debug",
                    "Captura con RenderDoc el siguiente fotograma completo del juego");

REXCVAR_DEFINE_INT32(odisea_renderdoc_capture_frames, 2, "GPU/Debug",
                     "Fotogramas del juego seguidos en cada captura de RenderDoc (1-8)");

void RenderDocFrameBoundary() {
  static int state = 0;  // 0 nada; >0 fotogramas que faltan por capturar
  static std::unique_ptr<rex::ui::RenderDocAPI> api;
  if (state == 0 && !REXCVAR_GET(odisea_renderdoc_capture)) return;
  if (!api) api = rex::ui::RenderDocAPI::CreateIfConnected();
  if (!api) {
    REXGPU_WARN("odisea: RenderDoc no esta conectado; no se captura");
    rex::cvar::SetFlagByName("odisea_renderdoc_capture", "false");
    return;
  }
  if (state == 0) {
    api->api_1_0_0()->StartFrameCapture(nullptr, nullptr);
    state = std::clamp(int(REXCVAR_GET(odisea_renderdoc_capture_frames)), 1, 8);
    REXGPU_INFO("odisea: captura de RenderDoc empezada ({} fotogramas)", state);
  } else if (--state > 0) {
    REXGPU_INFO("odisea: captura de RenderDoc, fin de fotograma");
  } else {
    api->api_1_0_0()->EndFrameCapture(nullptr, nullptr);
    state = 0;
    rex::cvar::SetFlagByName("odisea_renderdoc_capture", "false");
    REXGPU_INFO("odisea: captura de RenderDoc terminada");
  }
}

// --- Efectos transparentes en FSI/ROV ------------------------------------------
// En el modo de alta fidelidad la EDRAM se emula en el sombreador: cada pixel entra
// en una seccion critica, lee el destino, mezcla y escribe, aunque su aportacion
// sea nula (la mayor parte de un brillo o de una tira de niebla es transparente).
// Si con la mezcla del dibujo un alfa 0 (o un color 0) deja el destino igual, el
// sombreador se salta la lectura, la mezcla y la escritura del color: misma imagen.
REXCVAR_DEFINE_BOOL(odisea_fsi_skip_transparent, true, "GPU",
                    "Alta fidelidad (ROV/FSI): no tocar la EDRAM en pixeles de mezcla que no "
                    "cambian nada (partes transparentes de los efectos)");
REXCVAR_DEFINE_BOOL(odisea_fsi_late_tests, true, "GPU",
                    "Alta fidelidad (ROV/FSI): en dibujos que no escriben profundidad, sombrear "
                    "fuera de la seccion critica y probar la profundidad despues");

uint32_t FsiBlendNoOpFlags(uint32_t bc, uint32_t rt0_write_mask) {
  if (!REXCVAR_GET(odisea_fsi_skip_transparent) || !(rt0_write_mask & 0xF)) return 0;
  const uint32_t sf = bc & 0x1F, cop = (bc >> 5) & 7, df = (bc >> 8) & 0x1F;
  const uint32_t aop = (bc >> 21) & 7, asf = (bc >> 16) & 0x1F, adf = (bc >> 24) & 0x1F;
  const bool rgb = (rt0_write_mask & 7) != 0, alpha = (rt0_write_mask & 8) != 0;
  using F = xenos::BlendFactor;
  auto op_keeps = [](uint32_t op) {  // dst*DF (+ o -) 0 = dst si DF = 1
    return op == uint32_t(xenos::BlendOp::kAdd) || op == uint32_t(xenos::BlendOp::kRevSubtract);
  };
  auto is = [](uint32_t v, F f) { return v == uint32_t(f); };
  // Alfa 0: src.rgb * SF = 0 si SF depende del alfa de origen; el termino alfa
  // siempre es 0 * ASF; DF tiene que valer 1 con alfa 0.
  bool a0 = true;
  if (rgb) {
    a0 = a0 && op_keeps(cop) &&
         (is(sf, F::kZero) || is(sf, F::kSrcAlpha) || is(sf, F::kSrcAlphaSaturate)) &&
         (is(df, F::kOne) || is(df, F::kOneMinusSrcAlpha));
  }
  if (alpha) {
    a0 = a0 && op_keeps(aop) && (is(adf, F::kOne) || is(adf, F::kOneMinusSrcAlpha));
  }
  if (a0) return kFsiSysFlag_NoOpIfAlphaZero;
  // Color 0 (aditivos): src.rgb * SF = 0 con cualquier SF; el alfa no puede cambiar.
  bool c0 = true;
  if (rgb) {
    c0 = c0 && op_keeps(cop) && (is(df, F::kOne) || is(df, F::kOneMinusSrcColor));
  }
  if (alpha) {
    c0 = c0 && op_keeps(aop) && is(asf, F::kZero) && is(adf, F::kOne);
  }
  if (c0) return kFsiSysFlag_NoOpIfColorZero;
  // Color y alfa 0.
  bool ca0 = true;
  if (rgb) {
    ca0 = ca0 && op_keeps(cop) &&
          (is(df, F::kOne) || is(df, F::kOneMinusSrcAlpha) || is(df, F::kOneMinusSrcColor));
  }
  if (alpha) {
    ca0 = ca0 && op_keeps(aop) &&
          (is(adf, F::kOne) || is(adf, F::kOneMinusSrcAlpha) || is(adf, F::kOneMinusSrcColor));
  }
  if (ca0) return kFsiSysFlag_NoOpIfAlphaZero | kFsiSysFlag_NoOpIfColorZero;
  return 0;
}

bool FsiLateDepthStencilAllowed() { return REXCVAR_GET(odisea_fsi_late_tests); }

REXCVAR_DEFINE_BOOL(odisea_skip_noeffect_draws, true, "GPU",
                    "No envia los dibujos sin ningun efecto (sin color, sin Z, sin stencil, sin "
                    "memexport y fuera de una occlusion query del host). En caliente.");

bool IsNoEffectDraw(const RegisterFile& regs, xenos::EdramMode edram_mode, bool memexport_used) {
  if (memexport_used || !REXCVAR_GET(odisea_skip_noeffect_draws)) return false;
  const reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(regs);
  if (depth_control.z_write_enable || depth_control.stencil_enable) return false;
  if (edram_mode == xenos::EdramMode::kColorDepth && regs[XE_GPU_REG_RB_COLOR_MASK] != 0) {
    return false;
  }
  g_noeffect_draws.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void NoteOcclusionQuery(int kind) {
  if (kind >= 0 && kind < 4) g_occlusion_events[kind].fetch_add(1, std::memory_order_relaxed);
}

void NoteDraw() {
  g_draws.fetch_add(1, std::memory_order_relaxed);
  MaybeLogPipelineStats();
}
// Fotogramas del juego por segundo para el contador del exe (odisea_GameFrameStats):
// media de cada medio segundo y el fotograma mas largo de ese intervalo.
std::atomic<float> g_fps_shown{0.0f};
std::atomic<float> g_frame_ms_avg{0.0f};
std::atomic<float> g_frame_ms_max{0.0f};

void NoteFrame() {
  g_frames.fetch_add(1, std::memory_order_relaxed);
  using clock = std::chrono::steady_clock;
  static clock::time_point window_start = clock::now(), last = window_start;
  static uint32_t window_frames = 0;
  static double window_max_ms = 0.0;
  const clock::time_point now = clock::now();
  const double frame_ms = std::chrono::duration<double, std::milli>(now - last).count();
  last = now;
  ++window_frames;
  window_max_ms = std::max(window_max_ms, frame_ms);
  const double window_ms = std::chrono::duration<double, std::milli>(now - window_start).count();
  if (window_ms >= 500.0) {
    g_fps_shown.store(float(window_frames * 1000.0 / window_ms), std::memory_order_relaxed);
    g_frame_ms_avg.store(float(window_ms / window_frames), std::memory_order_relaxed);
    g_frame_ms_max.store(float(window_max_ms), std::memory_order_relaxed);
    window_start = now;
    window_frames = 0;
    window_max_ms = 0.0;
  }
}

void NotePipelineCreatedInGame() {
  g_new_pipelines.fetch_add(1, std::memory_order_relaxed);
  MaybeLogPipelineStats();
}
void NoteDrawSkippedForPipeline() {
  g_skipped_draws.fetch_add(1, std::memory_order_relaxed);
  MaybeLogPipelineStats();
}

void ShaderPrepBegin(int phase, uint32_t total) {
  g_prep_done.store(0, std::memory_order_relaxed);
  g_prep_total.store(total, std::memory_order_relaxed);
  g_prep_phase.store(phase, std::memory_order_release);
}
void ShaderPrepSetTotal(uint32_t total) { g_prep_total.store(total, std::memory_order_relaxed); }
void ShaderPrepStep(int phase) {
  if (g_prep_phase.load(std::memory_order_relaxed) == phase) {
    g_prep_done.fetch_add(1, std::memory_order_relaxed);
  }
}
void ShaderPrepEnd() { g_prep_phase.store(0, std::memory_order_release); }

}  // namespace rex::graphics::odisea

// Consulta desde el exe (GetProcAddress sobre rexgpu-odisea.dll): fase (0 = nada,
// 1 = traduciendo sombreadores, 2 = creando pipelines), hechos y total.
// Consulta desde el exe para el contador de fps: fotogramas del juego por segundo,
// duracion media y maxima de un fotograma (ms) en el ultimo medio segundo.
extern "C" __declspec(dllexport) void odisea_GameFrameStats(float* fps, float* avg_ms,
                                                           float* max_ms) {
  using namespace rex::graphics::odisea;
  if (fps) *fps = g_fps_shown.load(std::memory_order_relaxed);
  if (avg_ms) *avg_ms = g_frame_ms_avg.load(std::memory_order_relaxed);
  if (max_ms) *max_ms = g_frame_ms_max.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) void odisea_ShaderPrepProgress(int* phase, uint32_t* done,
                                                               uint32_t* total) {
  using namespace rex::graphics::odisea;
  if (phase) *phase = g_prep_phase.load(std::memory_order_acquire);
  if (done) *done = g_prep_done.load(std::memory_order_relaxed);
  if (total) *total = g_prep_total.load(std::memory_order_relaxed);
}


// --- Fork (odisea): borrado rapido de profundidad (ver la cabecera) ---
REXCVAR_DEFINE_BOOL(odisea_fast_depth_clear, true, "GPU",
                    "Hace los borrados de profundidad del juego (rectangulo con zfunc siempre) "
                    "con el sombreador de borrado de la EDRAM en vez de pixel a pixel. En caliente.");

namespace rex::graphics::odisea {

namespace {
// Sombreador de vertices de los rectangulos en coordenadas de pantalla del XDK
// (posicion xyz + color), el que usa Clear.
constexpr uint64_t kScreenRectVs = 0x0A6D1DD7767FDF27ull;
std::atomic<uint32_t> g_fast_clear_accepts{0};
// Diagnostico: descartes por motivo (solo desde el hilo de la GPU).
struct FastClearReason {
  const char* why;
  uint32_t count;
};
FastClearReason g_fast_clear_reasons[16] = {};
uint32_t g_fast_clear_evaluated = 0;
// true si aun quedan detalles por registrar para este motivo (6 por motivo).
bool FastClearNote(const char* why) {
  for (FastClearReason& reason : g_fast_clear_reasons) {
    if (!reason.why) {
      reason.why = why;
    }
    if (reason.why == why) {
      uint32_t seen = reason.count++;
      return seen < 6 || (seen % 4000) == 0;
    }
  }
  return false;
}
void FastClearSummary() {
  if (++g_fast_clear_evaluated % 3000 != 0) {
    return;
  }
  std::string line;
  for (const FastClearReason& reason : g_fast_clear_reasons) {
    if (reason.why) {
      line += fmt::format(" | {}: {}", reason.why, reason.count);
    }
  }
  REXGPU_INFO("odisea borrado rapido: resumen de {} dibujos: aceptados {}{}",
              g_fast_clear_evaluated, g_fast_clear_accepts.load(std::memory_order_relaxed), line);
}
}  // namespace

bool GetFastDepthClear(const RegisterFile& regs, const Shader& vertex_shader,
                       const Shader* pixel_shader, const memory::Memory& memory,
                       xenos::PrimitiveType primitive_type, uint32_t index_count,
                       FastDepthClear& out) {
  if (pixel_shader || vertex_shader.ucode_data_hash() != kScreenRectVs ||
      !REXCVAR_GET(odisea_fast_depth_clear)) {
    return false;
  }
  auto depth_control = regs.Get<reg::RB_DEPTHCONTROL>();
  auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
  auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  FastClearSummary();
  // Diagnostico: enlaces de vertices del sombreador y palabras crudas del bufer.
  auto describe = [&]() {
    std::string text;
    for (const Shader::VertexBinding& binding : vertex_shader.vertex_bindings()) {
      xenos::xe_gpu_vertex_fetch_t f = regs.GetVertexFetch(binding.fetch_constant);
      text += fmt::format(" [vf{} paso {} fetch {:08X} {:08X}:", binding.fetch_constant,
                          binding.stride_words, f.dword_0, f.dword_1);
      uint32_t address = f.address << 2;
      const uint32_t* words = address && address < 0x20000000u
                                  ? memory.TranslatePhysical<const uint32_t*>(address)
                                  : nullptr;
      if (words) {
        for (uint32_t i = 0; i < 21; ++i) {
          text += fmt::format(" {:08X}", xenos::GpuSwap(words[i], f.endian));
        }
      }
      text += "]";
    }
    draw_util::Scissor sc;
    draw_util::GetScissor(regs, sc, true);
    text += fmt::format(" tijera {},{} {}x{} win_off {:08X} modo {:08X} superficie {:08X} "
                        "depth_info {:08X}",
                        sc.offset[0], sc.offset[1], sc.extent[0], sc.extent[1],
                        regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET], regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
                        regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_DEPTH_INFO]);
    return text;
  };
  auto reject = [&](const char* why) {
    if (FastClearNote(why)) {
      REXGPU_INFO("odisea borrado rapido: descartado ({}) prim={} n={} depthcontrol={:08X} "
                  "stencilrefmask={:08X} vte={:08X}{}",
                  why, uint32_t(primitive_type), index_count, depth_control.value,
                  stencil_ref_mask.value, vte.value, describe());
    }
    return false;
  };
  if (primitive_type != xenos::PrimitiveType::kRectangleList || index_count != 3) {
    return reject("primitiva");
  }
  if (vte.vport_x_scale_ena || vte.vport_x_offset_ena || vte.vport_y_scale_ena ||
      vte.vport_y_offset_ena || vte.vport_z_scale_ena || vte.vport_z_offset_ena ||
      !vte.vtx_xy_fmt || !vte.vtx_z_fmt) {
    return reject("transformacion de viewport");
  }
  if (!depth_control.z_enable || !depth_control.z_write_enable ||
      depth_control.zfunc != xenos::CompareFunction::kAlways) {
    return reject("estado de profundidad");
  }
  // El sombreador de borrado escribe los 32 bits: el stencil tiene que quedar
  // determinado por el dibujo (siempre pasa y se reemplaza entero).
  if (!depth_control.stencil_enable ||
      depth_control.stencilfunc != xenos::CompareFunction::kAlways ||
      depth_control.stencilzpass != xenos::StencilOp::kReplace ||
      stencil_ref_mask.stencilwritemask != 0xFF) {
    return reject("estado de stencil");
  }
  auto sc_mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (sc_mode.poly_offset_front_enable || sc_mode.poly_offset_back_enable ||
      sc_mode.poly_offset_para_enable) {
    if ((regs[XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE] & 0x7FFFFFFFu) ||
        (regs[XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET] & 0x7FFFFFFFu)) {
      return reject("desplazamiento de poligono");
    }
  }
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  if (!surface_info.surface_pitch || surface_info.msaa_samples > xenos::MsaaSamples::k4X) {
    return reject("superficie");
  }
  // Vertices: el unico flujo del sombreador (posicion xyz al principio de cada
  // vertice, luego el color), 3 vertices. Ojo: el "vf95" del desensamblado es
  // la constante de fetch 0.
  const auto& bindings = vertex_shader.vertex_bindings();
  if (bindings.size() != 1 || bindings[0].stride_words < 3) {
    return reject("enlaces de vertices");
  }
  const uint32_t stride = bindings[0].stride_words;
  xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(bindings[0].fetch_constant);
  if ((fetch.type != xenos::FetchConstantType::kVertex &&
       fetch.type != xenos::FetchConstantType::kInvalidVertex) ||
      fetch.size < 2 * stride + 3 || !fetch.address || (fetch.address << 2) >= 0x20000000u) {
    return reject("bufer de vertices");
  }
  const float* vertices =
      reinterpret_cast<const float*>(memory.TranslatePhysical(fetch.address * sizeof(uint32_t)));
  if (!vertices) {
    return reject("bufer de vertices sin memoria");
  }
  float x[3], y[3], z[3];
  for (uint32_t i = 0; i < 3; ++i) {
    x[i] = xenos::GpuSwap(vertices[i * stride + 0], fetch.endian);
    y[i] = xenos::GpuSwap(vertices[i * stride + 1], fetch.endian);
    z[i] = xenos::GpuSwap(vertices[i * stride + 2], fetch.endian);
  }
  // El Clear del XDK pasa el valor de 24 bits como flotante con un pelin de
  // error: borrar a 0 llega como -3.7e-9. Sin recorte (clip_disable) la
  // profundidad se satura a 0..1, asi que vale como 0.
  if (z[0] == z[1] && z[1] == z[2] && z[0] < 0.0f && z[0] > -1e-6f) {
    if (!regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable) {
      return reject("z negativa con recorte activo");
    }
    z[0] = z[1] = z[2] = 0.0f;
  }
  if (!(z[0] == z[1] && z[1] == z[2]) || !(z[0] >= 0.0f && z[0] <= 1.0f)) {
    if (FastClearNote("z (detalle 2)")) {
      REXGPU_INFO("odisea borrado rapido: descartado (z, crudo){}", describe());
    }
    if (FastClearNote("z (detalle)")) {
      REXGPU_INFO("odisea borrado rapido: descartado (z) fetch {:08X} {:08X} v0=({}, {}, {}) "
                  "v1=({}, {}, {}) v2=({}, {}, {}) crudo {:08X} {:08X} {:08X} {:08X}",
                  fetch.dword_0, fetch.dword_1, x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2],
                  z[2], reinterpret_cast<const uint32_t*>(vertices)[0],
                  reinterpret_cast<const uint32_t*>(vertices)[1],
                  reinterpret_cast<const uint32_t*>(vertices)[2],
                  reinterpret_cast<const uint32_t*>(vertices)[3]);
    }
    return false;
  }
  // Pixeles cubiertos, con la misma regla que los resolves (centro del pixel).
  float half =
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  float min_x = std::min(std::min(x[0], x[1]), x[2]);
  float max_x = std::max(std::max(x[0], x[1]), x[2]);
  float min_y = std::min(std::min(y[0], y[1]), y[2]);
  float max_y = std::max(std::max(y[0], y[1]), y[2]);
  if (!(min_x > -65536.0f && max_x < 65536.0f && min_y > -65536.0f && max_y < 65536.0f)) {
    return reject("coordenadas");
  }
  int32_t x0 = int32_t(std::ceil(min_x + half - 0.5f));
  int32_t y0 = int32_t(std::ceil(min_y + half - 0.5f));
  int32_t x1 = int32_t(std::ceil(max_x + half - 0.5f));
  int32_t y1 = int32_t(std::ceil(max_y + half - 0.5f));
  if (sc_mode.vtx_window_offset_enable) {
    auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    x0 += window_offset.window_x_offset;
    y0 += window_offset.window_y_offset;
    x1 += window_offset.window_x_offset;
    y1 += window_offset.window_y_offset;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, true);
  int32_t scissor_right = int32_t(scissor.offset[0] + scissor.extent[0]);
  int32_t scissor_bottom = int32_t(scissor.offset[1] + scissor.extent[1]);
  x0 = std::clamp(x0, int32_t(scissor.offset[0]), scissor_right);
  y0 = std::clamp(y0, int32_t(scissor.offset[1]), scissor_bottom);
  x1 = std::clamp(x1, int32_t(scissor.offset[0]), scissor_right);
  y1 = std::clamp(y1, int32_t(scissor.offset[1]), scissor_bottom);
  if (x0 >= x1 || y0 >= y1) {
    return reject("rectangulo vacio");
  }
  // El borrado de la EDRAM va por bloques de 8 pixeles: sin redondear nada.
  if (((x0 | y0 | x1 | y1) & 7) || (x1 - x0) > int32_t(xenos::kMaxResolveSize) ||
      (y1 - y0) > int32_t(xenos::kMaxResolveSize)) {
    if (FastClearNote("rectangulo no alineado a 8")) {
      REXGPU_INFO("odisea borrado rapido: descartado (rectangulo {},{}-{},{} no alineado a 8)", x0,
                  y0, x1, y1);
    }
    return false;
  }
  uint32_t depth24;
  if (regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    depth24 = xenos::Float32To20e4(z[0], true) & 0xFFFFFF;
  } else {
    depth24 = std::min(uint32_t(z[0] * 16777215.0f + 0.5f), uint32_t(0xFFFFFF));
  }
  out.x0 = uint32_t(x0);
  out.y0 = uint32_t(y0);
  out.x1 = uint32_t(x1);
  out.y1 = uint32_t(y1);
  out.depth_clear = (depth24 << 8) | stencil_ref_mask.stencilref;
  if (g_fast_clear_accepts.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXGPU_INFO("odisea borrado rapido: {},{}-{},{} z={} valor={:08X}", x0, y0, x1, y1, z[0],
                out.depth_clear);
  }
  return true;
}

void FillDepthClearResolveInfo(const RegisterFile& regs, const FastDepthClear& clear,
                               uint32_t draw_resolution_scale_x,
                               uint32_t draw_resolution_scale_y, draw_util::ResolveInfo& info) {
  std::memset(static_cast<void*>(&info), 0, sizeof(info));
  // Copia de profundidad (seleccion 4) con borrado de profundidad: es lo que
  // miran IsCopyingDepth e IsClearingDepth.
  info.rb_copy_control.copy_src_select = xenos::kMaxColorRenderTargets;
  info.rb_copy_control.depth_clear_enable = 1;
  info.coordinate_info.width_div_8 = (clear.x1 - clear.x0) >> xenos::kResolveAlignmentPixelsLog2;
  info.height_div_8 = (clear.y1 - clear.y0) >> xenos::kResolveAlignmentPixelsLog2;
  info.coordinate_info.draw_resolution_scale_x = draw_resolution_scale_x;
  info.coordinate_info.draw_resolution_scale_y = draw_resolution_scale_y;
  if (IsRenderScaleFractional()) {
    info.coordinate_info.resolution_scale_q_x = GetRenderScaleQuartersRaw();
    info.coordinate_info.resolution_scale_q_y = GetRenderScaleQuartersRaw();
  }
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  uint32_t sample_count_log2_x = uint32_t(surface_info.msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t sample_count_log2_y = uint32_t(surface_info.msaa_samples >= xenos::MsaaSamples::k2X);
  uint32_t x0_samples = clear.x0 << sample_count_log2_x;
  uint32_t y0_samples = clear.y0 << sample_count_log2_y;
  uint32_t base_offset_x_tiles = x0_samples / xenos::kEdramTileWidthSamples;
  uint32_t base_offset_y_tiles = y0_samples / xenos::kEdramTileHeightSamples;
  info.coordinate_info.edram_offset_x_div_8 =
      (x0_samples % xenos::kEdramTileWidthSamples) >> (sample_count_log2_x + 3);
  info.coordinate_info.edram_offset_y_div_8 =
      (y0_samples % xenos::kEdramTileHeightSamples) >> (sample_count_log2_y + 3);
  uint32_t surface_pitch_tiles =
      xenos::GetSurfacePitchTiles(surface_info.surface_pitch, surface_info.msaa_samples, false);
  info.depth_edram_info.pitch_tiles = surface_pitch_tiles;
  info.depth_edram_info.msaa_samples = surface_info.msaa_samples;
  info.depth_edram_info.is_depth = 1;
  info.depth_edram_info.base_tiles =
      depth_info.depth_base + base_offset_y_tiles * surface_pitch_tiles + base_offset_x_tiles;
  info.depth_edram_info.format = uint32_t(depth_info.depth_format);
  info.depth_edram_info.format_is_64bpp = 0;
  info.depth_edram_info.fill_half_pixel_offset = 0;
  info.depth_original_base = depth_info.depth_base;
  info.rb_depth_clear = clear.depth_clear;
}

}  // namespace rex::graphics::odisea
