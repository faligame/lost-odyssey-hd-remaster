// lostodyssey - ReXGlue Recompiled Project
//
// Precreacion de pipelines de los materiales que el juego va cargando, para
// que un material nuevo no aparezca tarde la primera vez que se ve.
//
// Los sombreadores compilados de la 360 vienen en los paquetes UE3 del disco,
// en exports de clase ShaderCache: cada sombreador con su tipo (FName), su GUID
// y su bloque 0x102A1100 (pixeles) / 0x102A1101 (vertices); detras, por
// material, un mapa tipo -> GUID para cada vertex factory y el nombre del
// material. Un VS "TLightVertexShader<P>" va con el PS "TLightPixelShader<P>"
// del mismo mapa (comprobado: el 100 % de los pipelines vistos en partida que
// estan en el disco salen asi).
//
// Aqui:
//   - un midasm_hook en la lectura de ficheros del juego (sub_82BE6370, justo
//     en la llamada a NtReadFile) dice que trozo de que xenon_*.fpd se lee; con
//     el indice LO.fpi se sabe a que paquete pertenece;
//   - cada paquete nuevo se lee aparte (hilo propio, prioridad baja): sus
//     ShaderCache dan sombreadores y materiales, y sus imports/exports dicen que
//     materiales usa;
//   - los pares VS/PS de los materiales usados (sin las pasadas que el juego no
//     usa: decals, velocidad, sombras estaticas en mallas animadas...) se mandan
//     al plugin grafico (odisea_PrewarmSubmit), que aprende las declaraciones de
//     vertices y los estados de lo ya visto y crea los pipelines en segundo plano.
// Primer arranque (o disco/escala nuevos): si falta la marca de la generacion
// completa junto a la biblioteca de pipelines del plugin, se leen TODOS los
// paquetes del disco montado y se mandan de golpe los pares de todos los
// materiales usados (odisea_PrewarmSubmitBulk); la pantalla nativa "Preparando
// sombreadores" muestra la lectura (fase 1) y la creacion (fase 2).
// Nada sale del equipo: se calcula en local desde los discos del usuario.

#include <algorithm>
#include <chrono>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xfile.h>
#include <rex/system/xmemory.h>

#include "lo_paths.h"
#include "lo_options.h"
#include "multidisc.h"
#include "shader_prewarm.h"
#include "ue3_package.h"

REXCVAR_DEFINE_BOOL(lo_shader_prewarm, true, "LostOdyssey/Graphics",
                    "Precrea en segundo plano los pipelines de los materiales que el juego va cargando "
                    "(lee los sombreadores de los paquetes del disco). Requiere el plugin Odisea.");
REXCVAR_DEFINE_BOOL(lo_shader_prewarm_log, false, "LostOdyssey/Graphics",
                    "Registra cada paquete leido por la precreacion de pipelines y lo que aporta.");
REXCVAR_DEFINE_STRING(lo_shader_precache, "", "LostOdyssey/Graphics",
                      "Precarga de sombreadores: \"todo\" = todos los discos disponibles, \"parte\" = "
                      "la parte actual (las siguientes al llegar); vacio = preguntar la primera vez.");

namespace {

using namespace lo;
using namespace lo::ue3;

// Formato estable compartido con odisea_prewarm.cpp (plugin).
struct OdiseaPrewarmPair {
  const uint32_t* vs_ucode;
  uint32_t vs_dword_count;
  const uint32_t* vs_header;
  uint32_t vs_header_dword_count;
  const uint32_t* ps_ucode;
  uint32_t ps_dword_count;
  uint32_t vertex_factory;
  uint32_t pass_class;
};
using SubmitFn = void (*)(const OdiseaPrewarmPair*, uint32_t);
using BulkNeededFn = int (*)(uint32_t);
using BulkStateFn = int (*)();
using NextBulkDetachedFn = void (*)(int);
using ScanBeginFn = void (*)(uint32_t);
using ScanStepFn = void (*)();

using Guid = std::array<uint8_t, 16>;
struct GuidHash {
  size_t operator()(const Guid& g) const {
    uint64_t a, b;
    std::memcpy(&a, g.data(), 8);
    std::memcpy(&b, g.data() + 8, 8);
    return size_t(a ^ (b * 0x9E3779B97F4A7C15ull));
  }
};

uint32_t Fnv(std::string_view s) {
  uint32_t h = 2166136261u;
  for (char c : s) h = (h ^ uint8_t(c)) * 16777619u;
  return h;
}

inline uint32_t Be32(const Bytes& b, size_t p) {
  Need(b, p, 4, "be32");
  return (uint32_t(b[p]) << 24) | (uint32_t(b[p + 1]) << 16) | (uint32_t(b[p + 2]) << 8) | b[p + 3];
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](char c) { return char((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c); });
  return s;
}

// Pasadas que el juego no usa en lo jugado (medido): se dejan fuera para no
// crear miles de pipelines inutiles.
bool PassAllowed(std::string_view vf, std::string_view type) {
  auto has = [](std::string_view s, std::string_view x) { return s.find(x) != std::string_view::npos; };
  if (has(vf, "Decal")) return false;
  if (has(type, "FVelocity") || has(type, "FTextureDensity") || has(type, "FHitProxy")) return false;
  const bool local = vf == "FLocalVertexFactory";
  if ((has(type, "ShadowTexturePolicy") || has(type, "ShadowVertexBufferPolicy")) && !local) return false;
  if (has(type, "SpotLightPolicyFShadowTexture") || has(type, "PointLightPolicyFShadowTexture") ||
      has(type, "CharacterLightPolicyRPGFShadow")) {
    return false;
  }
  if ((has(type, "FLightMapTexturePolicy") || has(type, "FVertexLightMapPolicy")) && !local) return false;
  return true;
}

struct DiscShader {
  bool vertex = false;
  std::vector<uint32_t> ucode;   // bytes del disco tal cual (big-endian)
  std::vector<uint32_t> header;  // solo VS
};

struct PairRef {
  Guid vs, ps;
  uint32_t vertex_factory = 0;
  uint32_t pass_class = 0;
};

struct PackageRange {
  uint64_t begin = 0, end = 0;
  std::string path;
};

struct State {
  std::mutex mutex;
  std::condition_variable cond;
  // Lecturas vistas por el hook: (fichero en minusculas, desplazamiento).
  std::vector<std::pair<std::string, uint64_t>> reads;
  bool worker_started = false;

  // Solo en el hilo de trabajo:
  int index_disc = 0;
  std::unordered_map<std::string, std::vector<PackageRange>> archives;  // ordenados por begin
  std::unordered_set<std::string> packages_done;
  std::unordered_map<Guid, DiscShader, GuidHash> shaders;
  std::unordered_map<Guid, std::vector<PairRef>, GuidHash> material_pairs;
  std::unordered_map<std::string, std::vector<Guid>> material_by_name;
  std::unordered_set<std::string> wanted_names;
  std::unordered_set<Guid, GuidHash> sent_materials;
  std::unordered_set<uint64_t> sent_pairs;
  std::unique_ptr<DiscReader> reader;
  SubmitFn submit = nullptr;
  uint64_t pairs_sent = 0;
  int bulk_checked_disc = 0;
  // Lectura de todo el disco con varios hilos: protege lo de arriba.
  std::mutex merge_mutex;
  // Precarga de todos los discos ("todo"): discos ya revisados y si los que quedan siguen con
  // pantalla (la del disco montado se espero con pantalla y nadie ha pulsado "Jugar ya"); si no,
  // en segundo plano, que ya se esta jugando.
  std::unordered_set<int> other_discs_checked;
  bool chain_foreground = false;
  // El disco montado tuvo que esperar a otra generacion (la del disco anterior, en segundo plano):
  // ya se esta jugando, asi que la suya tambien va en segundo plano (sin pantalla a media partida).
  int mounted_disc_waited = 0;
};

// Eleccion de la primera vez (la pinta settings_page.cpp; el mando, turbo_hooks.cpp).
std::atomic<bool> g_choice_pending{false};
std::atomic<int> g_choice_selected{0};
std::atomic<int> g_generating_disc{0};
std::atomic<int> g_discs_queued{0};
std::atomic<int> g_current_disc{0};

bool ChoiceIsAll() { return REXCVAR_GET(lo_shader_precache) == "todo"; }
bool ChoiceMade() {
  const std::string& c = REXCVAR_GET(lo_shader_precache);
  return c == "todo" || c == "parte";
}

State& G() {
  static State* s = new State;  // nunca se destruye: el hilo vive hasta el final
  return *s;
}

template <typename Fn>
Fn PluginFn(const char* name) {
#if defined(_WIN32)
  if (HMODULE plugin = GetModuleHandleW(L"rexgpu-odisea.dll")) {
    return reinterpret_cast<Fn>(GetProcAddress(plugin, name));
  }
#endif
  return nullptr;
}

SubmitFn FindSubmit() { return PluginFn<SubmitFn>("odisea_PrewarmSubmit"); }

std::vector<uint32_t> ToDwords(const Bytes& b, size_t begin, size_t size) {
  Need(b, begin, size, "sombreador");
  std::vector<uint32_t> out(size / 4);
  std::memcpy(out.data(), b.data() + begin, out.size() * 4);
  return out;
}

// Indice LO.fpi de un disco: archivo -> rangos de paquetes.
bool BuildIndexFor(State& s, const DiscSource& d) {
  const int number = d.number;
  try {
    auto reader = DiscReader::Open(d);
    if (!reader) return false;
    Fpi fpi(ReadDiscFile(*reader, "LO.fpi"));
    s.archives.clear();
    for (auto& [path, f] : fpi.Files([](const std::string& p) { return p.ends_with(".xxx"); })) {
      s.archives[Lower(f.archive)].push_back({f.offset, f.offset + f.size, path});
    }
    for (auto& [name, list] : s.archives) {
      std::sort(list.begin(), list.end(), [](const PackageRange& a, const PackageRange& b) { return a.begin < b.begin; });
    }
    s.reader = std::move(reader);
    s.index_disc = number;
    s.packages_done.clear();
    REXLOG_INFO("lo_shader_prewarm: indice del disco {} listo ({} archivos)", number, s.archives.size());
    return true;
  } catch (const ParseError& e) {
    REXLOG_WARN("lo_shader_prewarm: no se puede leer LO.fpi del disco {}: {}", number, e.what);
    return false;
  }
}

// Indice del disco montado.
bool BuildIndex(State& s) {
  const int number = MultiDiscMountedNumber();
  if (number == s.index_disc && !s.archives.empty()) return true;
  for (const DiscSource& d : MultiDiscAllSources()) {
    if (d.number == number) return BuildIndexFor(s, d);
  }
  return false;
}

const PackageRange* FindPackage(const State& s, const std::string& archive, uint64_t offset) {
  auto it = s.archives.find(archive);
  if (it == s.archives.end()) return nullptr;
  const auto& list = it->second;
  auto r = std::upper_bound(list.begin(), list.end(), offset,
                            [](uint64_t o, const PackageRange& p) { return o < p.begin; });
  if (r == list.begin()) return nullptr;
  --r;
  return offset < r->end ? &*r : nullptr;
}

// Un export ShaderCache: sombreadores y, por material, sus pares VS/PS.
// Devuelve los nombres de material encontrados.
void ParseShaderCache(State& s, const Package& pkg, const Export& e, uint32_t& new_shaders,
                      uint32_t& new_materials) {
  const Bytes& b = pkg.bytes();
  const size_t end = size_t(e.offset) + e.size;
  if (end > b.size()) Fail("ShaderCache fuera del paquete");
  static constexpr uint8_t kMagic[3] = {0x10, 0x2A, 0x11};
  auto magic_at = [&](size_t p) { return p + 3 <= end && !std::memcmp(b.data() + p, kMagic, 3); };
  size_t q = e.offset;
  while (q + 3 <= end && !magic_at(q)) ++q;
  if (q + 3 > end || q < size_t(e.offset) + 34) return;
  q -= 34;
  while (q + 38 <= end && magic_at(q + 34)) {
    Reader r(b, q);
    const std::string type = pkg.ReadName(r);
    Guid guid;
    std::memcpy(guid.data(), b.data() + q + 8, 16);
    const uint32_t skip = Be32(b, q + 24);
    const size_t blob = q + 34;
    const bool vertex = b[blob + 3] == 1;
    const uint32_t h1 = Be32(b, blob + 4), h2 = Be32(b, blob + 8);
    if (h1 && h1 < 0x10000 && h2 > 0x40 && (h2 - 0x40) % 12 == 0 && !s.shaders.count(guid)) {
      DiscShader sh;
      sh.vertex = vertex;
      sh.ucode = ToDwords(b, blob + h1 + 0x40, h2 - 0x40);
      if (vertex) sh.header = ToDwords(b, blob, (h1 + 0x40) & ~size_t(3));
      s.shaders.emplace(guid, std::move(sh));
      ++new_shaders;
    }
    if (skip <= q) break;
    q = skip;
  }
  if (q + 4 > end) return;
  const uint32_t material_count = Be32(b, q);
  q += 4;
  if (material_count > 100000) Fail("ShaderCache: demasiados materiales");
  auto read_map = [&](size_t& p) {
    std::vector<std::pair<std::string, Guid>> m;
    const uint32_t n = Be32(b, p);
    if (n > 4096) Fail("ShaderCache: mapa corrupto");
    p += 4;
    for (uint32_t i = 0; i < n; ++i) {
      Reader r(b, p);
      std::string type = pkg.ReadName(r);
      Need(b, p + 8, 24, "mapa");
      Guid g;
      std::memcpy(g.data(), b.data() + p + 8, 16);
      m.emplace_back(std::move(type), g);
      p += 32;
    }
    return m;
  };
  for (uint32_t mi = 0; mi < material_count; ++mi) {
    Guid mguid;
    Need(b, q, 40, "material");
    std::memcpy(mguid.data(), b.data() + q, 16);
    const uint32_t skip = Be32(b, q + 24);
    const uint32_t vf_count = Be32(b, q + 32);
    if (skip <= q || skip > end || vf_count > 64) Fail("ShaderCache: material corrupto");
    q += 36;
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, Guid>>>> groups;
    auto map0 = read_map(q);
    for (uint32_t v = 0; v + 1 < vf_count; ++v) {
      Reader r(b, q);
      std::string vf = pkg.ReadName(r);
      q += 8;
      groups.emplace_back(std::move(vf), read_map(q));
    }
    {
      Reader r(b, q);
      groups.emplace_back(pkg.ReadName(r), std::move(map0));
    }
    q += 8;
    std::string name;
    if (q + 20 <= skip && !std::memcmp(b.data() + q, mguid.data(), 16)) {
      Reader r(b, q + 16);
      name = r.String();
    }
    q = skip;
    if (s.material_pairs.count(mguid)) continue;
    std::vector<PairRef> pairs;
    for (const auto& [vf, map] : groups) {
      for (const auto& [type, vs_guid] : map) {
        const size_t at = type.find("VertexShader");
        if (at == std::string::npos || !PassAllowed(vf, type)) continue;
        std::string ps_type = type;
        ps_type.replace(at, 12, "PixelShader");
        for (const auto& [t2, ps_guid] : map) {
          if (t2 == ps_type) {
            pairs.push_back({vs_guid, ps_guid, Fnv(vf), Fnv(vf + "|" + type)});
            break;
          }
        }
      }
    }
    s.material_pairs.emplace(mguid, std::move(pairs));
    if (!name.empty()) s.material_by_name[name].push_back(mguid);
    ++new_materials;
  }
}

void SubmitMaterial(State& s, const Guid& mguid, std::vector<OdiseaPrewarmPair>& out) {
  auto it = s.material_pairs.find(mguid);
  if (it == s.material_pairs.end() || !s.sent_materials.insert(mguid).second) return;
  for (const PairRef& p : it->second) {
    auto vs = s.shaders.find(p.vs);
    auto ps = s.shaders.find(p.ps);
    if (vs == s.shaders.end() || ps == s.shaders.end() || !vs->second.vertex || ps->second.vertex) continue;
    uint64_t key = 1469598103934665603ull;
    for (uint8_t c : p.vs) key = (key ^ c) * 1099511628211ull;
    for (uint8_t c : p.ps) key = (key ^ c) * 1099511628211ull;
    key ^= uint64_t(p.pass_class) << 17;
    if (!s.sent_pairs.insert(key).second) continue;
    const DiscShader& v = vs->second;
    const DiscShader& f = ps->second;
    out.push_back({v.ucode.data(), uint32_t(v.ucode.size()), v.header.data(), uint32_t(v.header.size()),
                   f.ucode.data(), uint32_t(f.ucode.size()), p.vertex_factory, p.pass_class});
  }
}

// Resuelve lo pedido con lo conocido y lo manda al plugin.
void Resolve(State& s) {
  std::vector<OdiseaPrewarmPair> out;
  for (const std::string& name : s.wanted_names) {
    auto it = s.material_by_name.find(name);
    if (it == s.material_by_name.end()) continue;
    for (const Guid& g : it->second) SubmitMaterial(s, g, out);
  }
  if (!out.empty() && s.submit) {
    s.submit(out.data(), uint32_t(out.size()));
    s.pairs_sent += out.size();
  }
}

struct ParseStats {
  uint32_t new_shaders = 0, new_materials = 0, wants = 0;
};

// Sombreadores, materiales y materiales pedidos de un paquete (con merge_mutex
// si hay varios hilos).
// wanted: donde se apuntan los materiales que pide el paquete (los de la
// lectura completa van aparte para que el modo por paquete no los reenvie).
void ParsePackage(State& s, const Package& pkg, const std::string& path, ParseStats& st,
                  std::unordered_set<std::string>& wanted) {
  uint32_t& new_shaders = st.new_shaders;
  uint32_t& new_materials = st.new_materials;
  uint32_t& wants = st.wants;
  {
    bool meshy = false;
    for (const Export& e : pkg.exports()) {
      const std::string_view cls = pkg.ClassName(e);
      if (cls == "ShaderCache") {
        try {
          ParseShaderCache(s, pkg, e, new_shaders, new_materials);
        } catch (const ParseError& err) {
          if (REXCVAR_GET(lo_shader_prewarm_log)) {
            REXLOG_WARN("lo_shader_prewarm: {}: ShaderCache raro: {}", path, err.what);
          }
        }
      }
      if (cls.find("Mesh") != std::string_view::npos || cls.find("Terrain") != std::string_view::npos ||
          cls.find("Particle") != std::string_view::npos) {
        meshy = true;
      }
    }
    auto want = [&](const std::string& name) { wants += wanted.insert(name).second; };
    const auto& imports = pkg.imports();
    const auto& classes = pkg.import_classes();
    for (size_t i = 0; i < imports.size() && i < classes.size(); ++i) {
      if (classes[i].starts_with("Material") && !classes[i].starts_with("MaterialExpression")) want(imports[i]);
    }
    for (const Export& e : pkg.exports()) {
      const std::string_view cls = pkg.ClassName(e);
      if (cls.starts_with("MaterialInstance") || (meshy && cls == "Material")) want(e.name);
    }
  }
}

void ProcessPackage(State& s, const PackageRange& range, const std::string& archive) {
  try {
    const Bytes raw = ReadDiscFile(*s.reader, archive, range.begin, range.end - range.begin);
    const Package pkg(CpxDecode(raw));
    ParseStats st;
    {
      std::lock_guard lock(s.merge_mutex);
      ParsePackage(s, pkg, range.path, st, s.wanted_names);
    }
    const uint32_t new_shaders = st.new_shaders, new_materials = st.new_materials, wants = st.wants;
    const uint64_t before = s.pairs_sent;
    Resolve(s);
    if (REXCVAR_GET(lo_shader_prewarm_log)) {
      REXLOG_INFO("lo_shader_prewarm: {}: {} sombreadores y {} materiales nuevos, {} materiales pedidos, {} pares "
                  "enviados (total {})",
                  range.path, new_shaders, new_materials, wants, s.pairs_sent - before, s.pairs_sent);
    }
  } catch (const ParseError& err) {
    if (REXCVAR_GET(lo_shader_prewarm_log)) REXLOG_WARN("lo_shader_prewarm: {}: {}", range.path, err.what);
  } catch (const std::exception& err) {
    REXLOG_WARN("lo_shader_prewarm: {}: {}", range.path, err.what());
  }
}

// Lee todos los paquetes de un disco y deja en out los pares de todos los materiales pedidos por
// algun paquete (apuntan a s.shaders: valen mientras viva s). progress: avanza la fase 1 de la
// pantalla nativa.
void CollectDiscPairs(State& s, int disc, const DiscSource& source,
                      const std::unordered_map<std::string, std::vector<PackageRange>>& archives,
                      bool progress, std::vector<OdiseaPrewarmPair>& out, size_t& material_count) {
  const auto scan_begin = PluginFn<ScanBeginFn>("odisea_PrewarmScanBegin");
  const auto scan_step = PluginFn<ScanStepFn>("odisea_PrewarmScanStep");
  std::vector<std::pair<std::string, const PackageRange*>> work;
  for (const auto& [archive, list] : archives) {
    for (const PackageRange& r : list) work.emplace_back(archive, &r);
  }
  if (progress && scan_begin) scan_begin(uint32_t(work.size()));
  REXLOG_INFO("lo_shader_prewarm: lectura completa del disco {}: {} paquetes", disc, work.size());
  std::unordered_set<std::string> bulk_wanted;
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  const uint32_t threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
  for (uint32_t t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
#if defined(_WIN32)
      SetThreadPriority(GetCurrentThread(), progress ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
#endif
      auto reader = DiscReader::Open(source);
      for (size_t k; (k = next.fetch_add(1)) < work.size();) {
        const auto& [archive, range] = work[k];
        if (reader) {
          try {
            const Package pkg(CpxDecode(ReadDiscFile(*reader, archive, range->begin, range->end - range->begin)));
            ParseStats st;
            std::lock_guard lock(s.merge_mutex);
            ParsePackage(s, pkg, range->path, st, bulk_wanted);
          } catch (const ParseError&) {
          } catch (const std::exception&) {
          }
        }
        if (progress && scan_step) scan_step();
      }
    });
  }
  for (auto& t : pool) t.join();

  // Todos los pares de los materiales pedidos por algun paquete del disco.
  std::unordered_set<uint64_t> seen;
  std::unordered_set<Guid, GuidHash> materials;
  for (const std::string& name : bulk_wanted) {
    auto it = s.material_by_name.find(name);
    if (it == s.material_by_name.end()) continue;
    for (const Guid& g : it->second) materials.insert(g);
  }
  for (const Guid& g : materials) {
    auto it = s.material_pairs.find(g);
    if (it == s.material_pairs.end()) continue;
    for (const PairRef& p : it->second) {
      auto vs = s.shaders.find(p.vs);
      auto ps = s.shaders.find(p.ps);
      if (vs == s.shaders.end() || ps == s.shaders.end() || !vs->second.vertex || ps->second.vertex) continue;
      uint64_t key = 1469598103934665603ull;
      for (uint8_t c : p.vs) key = (key ^ c) * 1099511628211ull;
      for (uint8_t c : p.ps) key = (key ^ c) * 1099511628211ull;
      key ^= uint64_t(p.pass_class) << 17;
      if (!seen.insert(key).second) continue;
      const DiscShader& v = vs->second;
      const DiscShader& f = ps->second;
      out.push_back({v.ucode.data(), uint32_t(v.ucode.size()), v.header.data(), uint32_t(v.header.size()),
                     f.ucode.data(), uint32_t(f.ucode.size()), p.vertex_factory, p.pass_class});
    }
  }
  material_count = materials.size();
}

// Lee todos los paquetes de un disco (montado o no, desde su carpeta, ISO o GOD) y manda al
// plugin los pares de todos sus materiales. background: sin pantalla (ni la lectura ni la
// creacion).
void GenerateDisc(State& s, int disc, const DiscSource& source,
                  const std::unordered_map<std::string, std::vector<PackageRange>>& archives,
                  bool background) {
  const auto submit_bulk = PluginFn<SubmitFn>("odisea_PrewarmSubmitBulk");
  const auto next_detached = PluginFn<NextBulkDetachedFn>("odisea_PrewarmNextBulkDetached");
  if (!PluginFn<ScanBeginFn>("odisea_PrewarmScanBegin") || !PluginFn<ScanStepFn>("odisea_PrewarmScanStep") ||
      !submit_bulk) {
    return;
  }
  g_generating_disc.store(disc);
  const auto start = std::chrono::steady_clock::now();
  std::vector<OdiseaPrewarmPair> out;
  size_t material_count = 0;
  CollectDiscPairs(s, disc, source, archives, !background, out, material_count);
  if (next_detached) next_detached(background ? 1 : 0);
  submit_bulk(out.data(), uint32_t(out.size()));
  REXLOG_INFO("lo_shader_prewarm: disco {} leido en {:.1f} s: {} sombreadores, {} materiales, {} pares enviados",
              disc, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
              s.shaders.size(), material_count, out.size());
}

// Pipelines esenciales de la primera ejecucion: el plugin grafico pide, antes de leer su
// almacen de sombreadores, el microcodigo del disco 1 (sincrono: esta lectura es obligatoria y
// se ve en la pantalla de preparacion). Los pares valen solo durante la llamada.
using EssentialsCb = void (*)(void*, const OdiseaPrewarmPair*, uint32_t);
int EssentialsProvider(int disc, EssentialsCb cb, void* ctx) {
  for (const DiscSource& d : MultiDiscAllSources()) {
    if (d.number != disc) continue;
    auto state = std::make_unique<State>();
    if (!BuildIndexFor(*state, d)) return 0;
    std::vector<OdiseaPrewarmPair> out;
    size_t material_count = 0;
    const auto start = std::chrono::steady_clock::now();
    CollectDiscPairs(*state, disc, d, state->archives, true, out, material_count);
    REXLOG_INFO("lo_shader_prewarm: esenciales: disco {} leido en {:.1f} s ({} pares)", disc,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), out.size());
    cb(ctx, out.data(), uint32_t(out.size()));
    return 1;
  }
  return 0;
}

// Disco montado: la primera vez con esta biblioteca, todo el disco de golpe (con pantalla). La
// primera de todas se pregunta antes que precargar.
void BulkIfNeeded(State& s) {
  const int disc = s.index_disc;
  g_current_disc.store(disc);
  if (!disc || disc == s.bulk_checked_disc) return;
  const auto needed = PluginFn<BulkNeededFn>("odisea_PrewarmBulkNeeded");
  const auto state_fn = PluginFn<BulkStateFn>("odisea_PrewarmBulkState");
  if (!needed || !state_fn) {
    s.bulk_checked_disc = disc;
    return;
  }
  // Una generacion (de otro disco, en segundo plano) en marcha: odisea_PrewarmBulkNeeded cambia
  // el disco de su marca, asi que se espera a que acabe.
  if (state_fn() != 0) {
    s.mounted_disc_waited = disc;
    return;
  }
  const int state = needed(uint32_t(disc));
  if (state < 0) return;  // el plugin aun no ha abierto su biblioteca: mas tarde
  if (state == 0) {
    s.bulk_checked_disc = disc;
    s.chain_foreground = false;  // ya se esta jugando: los demas discos, en segundo plano
    return;
  }
  const bool background = s.mounted_disc_waited == disc;
  if (!ChoiceMade() && !background) {
    g_choice_pending.store(true);
    return;  // se vuelve a mirar en la siguiente vuelta, con la eleccion hecha
  }
  s.bulk_checked_disc = disc;
  DiscSource source;
  for (const DiscSource& d : MultiDiscAllSources()) {
    if (d.number == disc) source = d;
  }
  s.chain_foreground = !background;
  GenerateDisc(s, disc, source, s.archives, background);
}

// "todo": los demas discos disponibles, uno detras de otro, cuando no hay otra generacion en
// marcha. Con pantalla mientras el jugador la espera; tras "Jugar ya", en segundo plano.
void PrecacheOtherDiscs(State& s) {
  if (!ChoiceIsAll()) {
    s.chain_foreground = false;
    g_discs_queued.store(0);
    return;
  }
  if (!s.index_disc || s.bulk_checked_disc != s.index_disc) return;
  const auto needed = PluginFn<BulkNeededFn>("odisea_PrewarmBulkNeeded");
  const auto state_fn = PluginFn<BulkStateFn>("odisea_PrewarmBulkState");
  if (!needed || !state_fn) return;
  const int state = state_fn();
  if (state == 2) s.chain_foreground = false;  // "Jugar ya"
  if (state != 0) return;
  std::vector<DiscSource> pending;
  for (const DiscSource& d : MultiDiscAllSources()) {
    if (d.number != s.index_disc && !s.other_discs_checked.count(d.number)) pending.push_back(d);
  }
  std::sort(pending.begin(), pending.end(),
            [](const DiscSource& a, const DiscSource& b) { return a.number < b.number; });
  g_discs_queued.store(int(pending.size()));
  if (pending.empty()) {
    g_generating_disc.store(0);
    s.chain_foreground = false;
    return;
  }
  const DiscSource& d = pending.front();
  const int disc_state = needed(uint32_t(d.number));
  if (disc_state < 0) return;
  s.other_discs_checked.insert(d.number);
  if (disc_state == 0) return;  // ya precargado
  try {
    auto reader = DiscReader::Open(d);
    if (!reader) return;
    Fpi fpi(ReadDiscFile(*reader, "LO.fpi"));
    std::unordered_map<std::string, std::vector<PackageRange>> archives;
    for (auto& [path, f] : fpi.Files([](const std::string& p) { return p.ends_with(".xxx"); })) {
      archives[Lower(f.archive)].push_back({f.offset, f.offset + f.size, path});
    }
    GenerateDisc(s, d.number, d, archives, !s.chain_foreground);
  } catch (const ParseError& e) {
    REXLOG_WARN("lo_shader_prewarm: no se puede leer LO.fpi del disco {}: {}", d.number, e.what);
  }
}

void Worker() {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  State& s = G();
  std::vector<std::pair<std::string, uint64_t>> reads;
  for (;;) {
    {
      std::unique_lock lock(s.mutex);
      // Sin lecturas, se despierta cada medio segundo para la comprobacion
      // de la generacion completa (el plugin abre su biblioteca algo despues).
      s.cond.wait_for(lock, std::chrono::milliseconds(500), [&] { return !s.reads.empty(); });
      reads.swap(s.reads);
    }
    if (!s.submit) s.submit = FindSubmit();
    if (!s.submit || !BuildIndex(s)) {
      reads.clear();
      continue;
    }
    BulkIfNeeded(s);
    PrecacheOtherDiscs(s);
    for (const auto& [archive, offset] : reads) {
      const PackageRange* range = FindPackage(s, archive, offset);
      if (!range || !s.packages_done.insert(range->path).second) continue;
      ProcessPackage(s, *range, archive);
    }
    reads.clear();
  }
}

}  // namespace

namespace {

// Lectura vista: handle del fichero y direccion del desplazamiento (u64 BE).
void NoteRead(uint32_t handle, uint32_t offset_address, uint32_t length) {
  if (!REXCVAR_GET(lo_shader_prewarm)) return;
  auto* kernel = REX_KERNEL_STATE();
  auto* memory = REX_KERNEL_MEMORY();
  if (!kernel || !memory || !offset_address) return;
  auto file = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XFile>(handle);
  static std::atomic<int> diag{0};
  if (REXCVAR_GET(lo_shader_prewarm_log) && diag.fetch_add(1) < 12) {
    REXLOG_INFO("lo_shader_prewarm: lectura {} ({} bytes)", file ? file->path() : std::string("?"), length);
  }
  if (!file) return;
  std::string name = Lower(file->name());
  if (!name.ends_with(".fpd")) return;
  const uint8_t* p = memory->TranslateVirtual<const uint8_t*>(offset_address);
  if (!p) return;
  uint64_t offset = 0;
  for (int i = 0; i < 8; ++i) offset = (offset << 8) | p[i];
  State& s = G();
  {
    std::lock_guard lock(s.mutex);
    if (!s.reads.empty() && s.reads.back().first == name && s.reads.back().second == offset) return;
    s.reads.emplace_back(std::move(name), offset);
    if (!s.worker_started) {
      s.worker_started = true;
      std::thread(Worker).detach();
    }
  }
  s.cond.notify_one();
}

}  // namespace

// Import NtReadFile del kernel (lo implementa el SDK). El juego lo llama por un
// puntero guardado en sus datos (llamada indirecta al thunk 0x830DA03C), asi
// que se envuelve la entrada de la tabla de funciones de ese thunk.
// NtReadFile(handle r3, event r4, apc r5, apc_ctx r6, iosb r7, buffer r8,
//            length r9, byte_offset r10)
REX_EXTERN(__imp__NtReadFile);

static void LoPrewarmNtReadFile(PPCContext& ctx, uint8_t* base) {
  NoteRead(ctx.r3.u32, ctx.r10.u32, ctx.r9.u32);
  __imp__NtReadFile(ctx, base);
}

// La llamada directa de sub_82BE6370 (midasm_hook del manifiesto).
void LoShaderPrewarmReadHook(PPCRegister& r3, PPCRegister& r9, PPCRegister& r10) {
  NoteRead(r3.u32, r10.u32, r9.u32);
}

namespace lo {

bool PrecacheChoicePending() { return g_choice_pending.load(); }
PrecacheChoice PrecacheChoiceSelected() { return PrecacheChoice(g_choice_selected.load()); }
void PrecacheChoiceMove(int delta) {
  if (!g_choice_pending.load() || !delta) return;
  g_choice_selected.store(g_choice_selected.load() == 0 ? 1 : 0);
}
void PrecacheChoiceConfirm() {
  if (!g_choice_pending.exchange(false)) return;
  const char* value = PrecacheChoiceSelected() == PrecacheChoice::kAll ? "todo" : "parte";
  rex::cvar::SetFlagByName("lo_shader_precache", value);
  SetTomlValue(lo::ConfigFile(), "lo_shader_precache",
               std::string("\"") + value + "\"");
  REXLOG_INFO("lo_shader_prewarm: precarga elegida: {}", value);
  G().cond.notify_all();
}
int PrecacheDiscsAvailable() { return int(MultiDiscAllSources().size()); }
int PrecacheCurrentDisc() { return g_current_disc.load(); }
int PrecacheGeneratingDisc() { return g_generating_disc.load(); }
int PrecacheDiscsQueued() { return g_discs_queued.load(); }

void ShaderPrewarmInstall() {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  constexpr uint32_t kNtReadFileThunk = 0x830DA03C;
  {
    State& s = G();
    std::lock_guard lock(s.mutex);
    if (REXCVAR_GET(lo_shader_prewarm) && !s.worker_started) {
      s.worker_started = true;
      std::thread(Worker).detach();
    }
  }
  if (memory->SetFunction(kNtReadFileThunk, &LoPrewarmNtReadFile)) {
    REXLOG_INFO("lo_shader_prewarm: lecturas del disco vigiladas");
  } else {
    REXLOG_WARN("lo_shader_prewarm: no se pudo envolver NtReadFile");
  }
}

// Antes de que el plugin grafico abra su almacen de sombreadores (primera ejecucion): le da el
// proveedor del microcodigo del disco para fabricar los pipelines esenciales.
void ShaderPrewarmRegisterEssentials() {
  using SetFn = void (*)(int (*)(int, EssentialsCb, void*));
  if (auto set = PluginFn<SetFn>("odisea_SetEssentialsProvider")) set(&EssentialsProvider);
}

}  // namespace lo
