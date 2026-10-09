// Fork (odisea): precreacion de pipelines a partir de los sombreadores del disco.
// Ver odisea_prewarm.h.
#include <rex/graphics/odisea_prewarm.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/odisea_watched_draw.h>
#include <rex/logging.h>

#include <rex/hash.h>

REXCVAR_DEFINE_BOOL(odisea_shader_prewarm, true, "GPU",
                    "Precrea en segundo plano los pipelines de los materiales que el juego va cargando "
                    "(pares VS/PS de los ShaderCache del disco que manda el exe).");
REXCVAR_DEFINE_INT32(odisea_prewarm_background_threads, 2, "GPU",
                     "Pipelines precreados que se crean a la vez en segundo plano (tras \"Jugar ya\" "
                     "y en la precreacion normal). Con todos los hilos de creacion, sus hermanos de "
                     "hyperthreading le quitan ~8 fps al juego. En caliente.");
REXCVAR_DEFINE_BOOL(odisea_async_no_frame_wait, true, "GPU",
                    "Con async_shader_compilation, no esperar al final del fotograma a que se creen los "
                    "pipelines en cola (los dibujos que los necesitan ya se saltan).");

namespace rex::graphics::odisea::prewarm {

namespace {

inline uint32_t Swap(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

std::vector<uint32_t> ToNative(const uint32_t* be, size_t n) {
  std::vector<uint32_t> out(n);
  for (size_t i = 0; i < n; ++i) out[i] = Swap(be[i]);
  return out;
}

inline bool IsVfetch(uint32_t a) { return (a & 0x1F) == 0 && ((a >> 19) & 1); }

// Tamano en dwords de cada formato de vertice del Xenos.
uint32_t FormatDwords(uint32_t fmt) {
  switch (fmt) {
    case 26: case 32: case 34: case 37: return 2;  // 16_16_16_16, 16_16_16_16_FLOAT, 32_32, 32_32_FLOAT
    case 57: return 3;                              // 32_32_32_FLOAT
    case 35: case 38: return 4;                     // 32_32_32_32, 32_32_32_32_FLOAT
    default: return 1;
  }
}

std::array<uint32_t, 4> Swizzle(uint32_t b) {
  return {b & 7, (b >> 3) & 7, (b >> 6) & 7, (b >> 9) & 7};
}

// Vfetch del disco aun sin rellenar.
std::vector<uint32_t> DiscFetches(const std::vector<uint32_t>& w) {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i + 2 < w.size(); i += 3) {
    if (IsVfetch(w[i]) && w[i + 2] == 0 && ((w[i + 1] >> 16) & 63) == 0) out.push_back(i / 3);
  }
  return out;
}

std::mutex g_jobs_mutex;
std::deque<Job> g_jobs;
std::vector<Job> g_deferred;
uint64_t g_deferred_generation = 0;
constexpr size_t kMaxDeferred = 50000;

// Generacion completa del primer arranque.
std::mutex g_bulk_mutex;
std::filesystem::path g_library_path;
uint32_t g_bulk_disc = 0;
std::atomic<bool> g_bulk_active{false};
// El bombeo ya corre: el hilo del procesador ha terminado la carga de la cache
// del arranque (va antes en ese mismo hilo). Hasta entonces no se empieza la
// generacion completa, que si no taparia la barra de esa carga.
std::atomic<bool> g_pump_alive{false};
std::atomic<bool> g_bulk_detached{false};
// La siguiente generacion completa empieza ya en segundo plano (sin pantalla): la pidio el exe para
// los discos que siguen tras "Jugar ya" o con el juego ya en marcha.
std::atomic<bool> g_next_bulk_detached{false};
std::atomic<uint32_t> g_bulk_total{0};
std::atomic<uint32_t> g_bulk_done{0};      // revisados una vez
std::atomic<uint32_t> g_bulk_resolved{0};  // con sus pipelines en cola (barra)

std::filesystem::path BulkMarker() {
  std::filesystem::path m = g_library_path;
  m += ".disc" + std::to_string(g_bulk_disc) + ".prewarm";
  return m;
}

std::atomic<uint32_t> g_received{0};
std::atomic<uint32_t> g_queued{0};
std::atomic<uint32_t> g_hits{0};

}  // namespace

bool Enabled() { return REXCVAR_GET(odisea_shader_prewarm); }
uint32_t BackgroundCreationLimit() {
  return uint32_t(std::max(REXCVAR_GET(odisea_prewarm_background_threads), 1));
}
bool AsyncNoFrameWait() {
  return REXCVAR_GET(async_shader_compilation) && REXCVAR_GET(odisea_async_no_frame_wait);
}

void TakeJobs(std::vector<Job>& out, size_t max, uint64_t learn_generation) {
  std::lock_guard lock(g_jobs_mutex);
  if (learn_generation != g_deferred_generation && !g_deferred.empty()) {
    // Se ha aprendido algo nuevo: los aplazados vuelven a la cola.
    g_deferred_generation = learn_generation;
    for (Job& j : g_deferred) g_jobs.push_back(std::move(j));
    g_deferred.clear();
  }
  while (out.size() < max && !g_jobs.empty()) {
    out.push_back(std::move(g_jobs.front()));
    g_jobs.pop_front();
  }
}

void Defer(Job&& job) {
  std::lock_guard lock(g_jobs_mutex);
  if (g_deferred.size() < kMaxDeferred) g_deferred.push_back(std::move(job));
}

uint64_t BlankedHash(const uint32_t* be_dwords, size_t count) {
  return BlankedHashNative(ToNative(be_dwords, count));
}

uint64_t BlankedHashNative(std::vector<uint32_t> w) {
  for (size_t i = 0; i + 2 < w.size(); i += 3) {
    if (IsVfetch(w[i])) {
      w[i] &= ~(uint32_t(7) << 27) & ~(uint32_t(0x7F) << 20) & ~(uint32_t(63) << 12);
      w[i + 1] = 0;
      w[i + 2] = 0;
    }
  }
  return XXH3_64bits(w.data(), w.size() * sizeof(uint32_t));
}

// --- VsBinder -----------------------------------------------------------------

void VsBinder::ObserveRuntimeVs(uint64_t hash, const uint32_t* native_dwords, size_t count) {
  if (runtime_blank_.count(hash)) return;
  std::vector<uint32_t> native(native_dwords, native_dwords + count);
  const uint64_t blank = BlankedHashNative(native);
  runtime_blank_[hash] = blank;
  auto disc = disc_.find(blank);
  if (disc != disc_.end() && disc->second.valid) Learn(disc->second, native);
  runtime_all_by_blank_[blank].push_back(native);
  runtime_by_blank_.emplace(blank, std::move(native));
}

void VsBinder::AddDiscVs(const Job& job) {
  const uint64_t blank = BlankedHash(job.vs_disc.data(), job.vs_disc.size());
  auto [it, inserted] = disc_.try_emplace(blank);
  DiscVs& d = it->second;
  const bool new_class = d.classes.insert(job.pass_class).second;
  const bool new_vf = d.vertex_factories.insert(job.vertex_factory).second;
  if (new_class) ++class_generation_;  // ClassesOfRuntime cambia: reaprender estados
  if (!inserted) {
    if (new_vf) {
      // Las declaraciones ya aprendidas de este VS valen tambien para esta factory.
      auto all = runtime_all_by_blank_.find(blank);
      if (all != runtime_all_by_blank_.end() && d.valid) {
        for (const auto& rt : all->second) Learn(d, rt);
      }
    }
    return;
  }
  d.ucode = ToNative(job.vs_disc.data(), job.vs_disc.size());
  d.fetch_index = DiscFetches(d.ucode);
  // Tabla de la cabecera: entradas cuyo byte bajo son los indices de las vfetch.
  if (!d.fetch_index.empty()) {
    std::vector<uint32_t> hdr = ToNative(job.vs_header.data(), job.vs_header.size());
    const size_t n = d.fetch_index.size();
    for (size_t p = 0; p + n <= hdr.size(); ++p) {
      bool ok = true;
      for (size_t k = 0; k < n && ok; ++k) ok = (hdr[p + k] & 0xFF) == d.fetch_index[k];
      if (ok) {
        for (size_t k = 0; k < n; ++k) d.fetch_key.push_back((hdr[p + k] >> 12) & 0xFF);
        d.valid = true;
        break;
      }
    }
  }
  if (!d.valid) return;
  auto all = runtime_all_by_blank_.find(blank);
  if (all != runtime_all_by_blank_.end()) {
    for (const auto& rt : all->second) Learn(d, rt);
  }
}

void VsBinder::Learn(const DiscVs& disc, const std::vector<uint32_t>& gw) {
  if (gw.size() != disc.ucode.size()) return;
  std::vector<uint32_t> gidx;
  for (uint32_t i = 0; i + 2 < gw.size(); i += 3) {
    if (IsVfetch(gw[i])) gidx.push_back(i / 3);
  }
  Decl decl;
  std::unordered_set<uint32_t> used;
  for (size_t k = 0; k < disc.fetch_index.size(); ++k) {
    const uint32_t i = disc.fetch_index[k];
    const uint32_t a = disc.ucode[3 * i], b = disc.ucode[3 * i + 1];
    const auto ds = Swizzle(b);
    // La vfetch del juego con el mismo registro de destino y las mismas
    // componentes enmascaradas; entre varias, la mas cercana.
    int64_t best = -1;
    for (uint32_t j : gidx) {
      if (used.count(j) || ((gw[3 * j] >> 12) & 63) != ((a >> 12) & 63)) continue;
      const auto gs = Swizzle(gw[3 * j + 1]);
      bool same_mask = true;
      for (int c = 0; c < 4; ++c) same_mask &= (gs[c] == 7) == (ds[c] == 7);
      if (!same_mask) continue;
      if (best < 0 || std::abs(int64_t(j) - int64_t(i)) < std::abs(best - int64_t(i))) best = j;
    }
    if (best < 0) return;
    used.insert(uint32_t(best));
    const uint32_t j = uint32_t(best);
    Elem e;
    e.a = gw[3 * j] & ~(uint32_t(7) << 27) & ~uint32_t(0x0007F000);
    e.b = gw[3 * j + 1] & ~uint32_t(0xFFF) & ~(uint32_t(1) << 30);
    e.c = gw[3 * j + 2];
    const auto gs = Swizzle(gw[3 * j + 1]);
    for (int c = 0; c < 4; ++c) {
      if (ds[c] >= 4) continue;
      if (e.remap[ds[c]] >= 0 && uint32_t(e.remap[ds[c]]) != gs[c]) return;
      e.remap[ds[c]] = int8_t(gs[c]);
    }
    const uint32_t key = disc.fetch_key[k];
    auto prev = decl.find(key);
    if (prev != decl.end()) {
      // La misma entrada leida dos veces: debe coincidir.
      if (prev->second.a != e.a || prev->second.b != e.b || prev->second.c != e.c) return;
      for (int c = 0; c < 4; ++c) {
        if (e.remap[c] >= 0) prev->second.remap[c] = e.remap[c];
      }
    } else {
      decl.emplace(key, e);
    }
  }
  // Solo si reproduce exactamente el VS del juego.
  std::vector<uint32_t> check;
  if (!Apply(disc, decl, check) || check != gw) return;
  // Firma de la declaracion para no repetirla.
  std::vector<uint32_t> sig;
  std::vector<uint32_t> keys;
  for (const auto& [k, e] : decl) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  for (uint32_t k : keys) {
    const Elem& e = decl[k];
    sig.insert(sig.end(), {k, e.a, e.b, e.c, uint32_t(uint8_t(e.remap[0])) | uint32_t(uint8_t(e.remap[1])) << 8 |
                                                   uint32_t(uint8_t(e.remap[2])) << 16 |
                                                   uint32_t(uint8_t(e.remap[3])) << 24});
  }
  for (uint32_t vf : disc.vertex_factories) {
    std::vector<uint32_t> s = sig;
    s.push_back(vf);
    if (!decl_signatures_.insert(XXH3_64bits(s.data(), s.size() * 4)).second) continue;
    decls_[vf].push_back(decl);
    ++generation_;
  }
}

bool VsBinder::Apply(const DiscVs& disc, const Decl& decl, std::vector<uint32_t>& w) {
  w = disc.ucode;
  // Grupos de vfetch consecutivas.
  std::vector<std::vector<size_t>> groups;  // indices dentro de fetch_index
  for (size_t k = 0; k < disc.fetch_index.size(); ++k) {
    if (groups.empty() || disc.fetch_index[k] != disc.fetch_index[groups.back().back()] + 1) groups.emplace_back();
    groups.back().push_back(k);
  }
  struct Ins {
    uint32_t a, b, c;
  };
  for (const auto& g : groups) {
    std::vector<Ins> ins;
    for (size_t k : g) {
      auto it = decl.find(disc.fetch_key[k]);
      if (it == decl.end()) return false;
      const Elem& e = it->second;
      const uint32_t i = disc.fetch_index[k];
      Ins x;
      x.a = (disc.ucode[3 * i] & 0x0007F000) | e.a;
      const auto ds = Swizzle(disc.ucode[3 * i + 1]);
      uint32_t ns = 0;
      for (int c = 0; c < 4; ++c) {
        uint32_t v = ds[c];
        if (v < 4) {
          if (e.remap[v] < 0) return false;
          v = uint32_t(e.remap[v]);
        }
        ns |= v << (3 * c);
      }
      x.b = ns | e.b;
      x.c = e.c;
      ins.push_back(x);
    }
    std::stable_sort(ins.begin(), ins.end(), [](const Ins& l, const Ins& r) {
      const uint32_t sl = (l.a >> 20) & 127, sr = (r.a >> 20) & 127;
      if (sl != sr) return sl > sr;
      return ((l.c >> 8) & 0x7FFFFF) < ((r.c >> 8) & 0x7FFFFF);
    });
    Ins* first = nullptr;
    uint32_t start = 0, end = 0, count = 0;
    auto close = [&] {
      if (first && count > 1) first->a |= ((end - start - 1) & 7) << 27;
    };
    for (Ins& t : ins) {
      const uint32_t off = (t.c >> 8) & 0x7FFFFF;
      const uint32_t size = FormatDwords((t.b >> 16) & 63);
      if (first && ((t.a >> 20) & 127) == ((first->a >> 20) & 127) && off + size - start <= 8 && off >= start) {
        t.b |= uint32_t(1) << 30;
        end = std::max(end, off + size);
        ++count;
      } else {
        close();
        first = &t;
        start = off;
        end = off + size;
        count = 1;
      }
    }
    close();
    for (size_t n = 0; n < g.size(); ++n) {
      const uint32_t i = disc.fetch_index[g[n]];
      w[3 * i] = ins[n].a;
      w[3 * i + 1] = ins[n].b;
      w[3 * i + 2] = ins[n].c;
    }
  }
  return true;
}

void VsBinder::AddSeed(const Seed& seed) {
  for (const auto& [vf, list] : seed.decls) {
    for (const auto& elems : list) {
      Decl decl;
      for (const SeedElem& e : elems) {
        Elem x;
        x.a = e.a;
        x.b = e.b;
        x.c = e.c;
        std::memcpy(x.remap, e.remap, 4);
        decl.emplace(e.key, x);
      }
      std::vector<uint32_t> keys;
      for (const auto& [k, e] : decl) keys.push_back(k);
      std::sort(keys.begin(), keys.end());
      std::vector<uint32_t> sig;
      for (uint32_t k : keys) {
        const Elem& e = decl[k];
        sig.insert(sig.end(), {k, e.a, e.b, e.c,
                               uint32_t(uint8_t(e.remap[0])) | uint32_t(uint8_t(e.remap[1])) << 8 |
                                   uint32_t(uint8_t(e.remap[2])) << 16 | uint32_t(uint8_t(e.remap[3])) << 24});
      }
      sig.push_back(vf);
      if (!decl_signatures_.insert(XXH3_64bits(sig.data(), sig.size() * 4)).second) continue;
      decls_[vf].push_back(std::move(decl));
      ++generation_;
    }
  }
}

std::vector<std::vector<uint32_t>> VsBinder::Bind(const Job& job) const {
  std::vector<std::vector<uint32_t>> out;
  const uint64_t blank = BlankedHash(job.vs_disc.data(), job.vs_disc.size());
  auto d = disc_.find(blank);
  if (d == disc_.end() || !d->second.valid) return out;
  auto decls = decls_.find(job.vertex_factory);
  if (decls == decls_.end()) return out;
  std::unordered_set<uint64_t> seen;
  // Preferencia: declaraciones con exactamente los elementos que lee el VS
  // (la malla tipica de ese material); si no hay, las que los contienen.
  std::unordered_set<uint32_t> keys(d->second.fetch_key.begin(), d->second.fetch_key.end());
  bool any_exact = false;
  for (const Decl& decl : decls->second) {
    if (decl.size() != keys.size()) continue;
    bool same = true;
    for (uint32_t k : keys) same &= decl.count(k) != 0;
    any_exact |= same;
  }
  for (const Decl& decl : decls->second) {
    bool covers = true;
    for (uint32_t k : d->second.fetch_key) covers &= decl.count(k) != 0;
    if (!covers || (any_exact && decl.size() != keys.size())) continue;
    std::vector<uint32_t> w;
    if (!Apply(d->second, decl, w)) continue;
    for (uint32_t& v : w) v = Swap(v);
    if (seen.insert(XXH3_64bits(w.data(), w.size() * 4)).second) out.push_back(std::move(w));
  }
  return out;
}

const std::unordered_set<uint32_t>* VsBinder::ClassesOfRuntime(uint64_t runtime_hash) const {
  auto b = runtime_blank_.find(runtime_hash);
  if (b == runtime_blank_.end()) return nullptr;
  auto d = disc_.find(b->second);
  return d == disc_.end() ? nullptr : &d->second.classes;
}

size_t VsBinder::decl_count() const {
  size_t n = 0;
  for (const auto& [vf, v] : decls_) n += v.size();
  return n;
}

// --- TemplateStore ------------------------------------------------------------

void TemplateStore::Clear() { by_class_.clear(); }

void TemplateStore::Add(uint32_t pass_class, const void* data, size_t size, uint32_t count) {
  auto& list = by_class_[pass_class];
  for (Counted& c : list) {
    if (c.bytes.size() == size && !std::memcmp(c.bytes.data(), data, size)) {
      c.count += count;
      return;
    }
  }
  Counted c;
  c.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  c.count = count;
  list.push_back(std::move(c));
}

std::vector<const std::vector<uint8_t>*> TemplateStore::Pick(uint32_t pass_class) const {
  std::vector<const std::vector<uint8_t>*> out;
  auto it = by_class_.find(pass_class);
  if (it == by_class_.end()) return out;
  uint32_t total = 0;
  for (const Counted& c : it->second) total += c.count;
  std::vector<const Counted*> sorted;
  for (const Counted& c : it->second) sorted.push_back(&c);
  std::sort(sorted.begin(), sorted.end(), [](const Counted* a, const Counted* b) { return a->count > b->count; });
  for (const Counted* c : sorted) {
    if (out.size() >= 2 || c->count * 4 < total) break;
    out.push_back(&c->bytes);
  }
  return out;
}

// --- Semilla -------------------------------------------------------------------

namespace {

Seed LoadSeed() {
  Seed seed;
  HMODULE module = nullptr;
  wchar_t path_buf[MAX_PATH];
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&GetSeed), &module) ||
      !GetModuleFileNameW(module, path_buf, MAX_PATH)) {
    return seed;
  }
  const std::filesystem::path path = std::filesystem::path(path_buf).parent_path() / "prewarm_seed.bin";
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    REXLOG_INFO("odisea prewarm: sin semilla ({})", path.string());
    return seed;
  }
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  size_t p = 0;
  bool ok = true;
  auto u32 = [&]() -> uint32_t {
    if (p + 4 > b.size()) {
      ok = false;
      return 0;
    }
    uint32_t v;
    std::memcpy(&v, b.data() + p, 4);
    p += 4;
    return v;
  };
  if (u32() != SeedTag("LOPS") || u32() != 1) {
    REXLOG_WARN("odisea prewarm: semilla con formato desconocido ({})", path.string());
    return {};
  }
  for (uint32_t nvf = u32(); ok && nvf > 0; --nvf) {
    const uint32_t vf = u32();
    auto& list = seed.decls[vf];
    for (uint32_t nd = u32(); ok && nd > 0; --nd) {
      std::vector<SeedElem> elems;
      const uint32_t ne = u32();
      if (ne > 64) ok = false;
      for (uint32_t i = 0; ok && i < ne; ++i) {
        SeedElem e;
        e.key = u32();
        e.a = u32();
        e.b = u32();
        e.c = u32();
        if (p + 4 > b.size()) {
          ok = false;
          break;
        }
        std::memcpy(e.remap, b.data() + p, 4);
        p += 4;
        elems.push_back(e);
      }
      list.push_back(std::move(elems));
    }
  }
  for (uint32_t nset = u32(); ok && nset > 0; --nset) {
    const uint32_t tag = u32();
    Seed::Set& set = seed.sets[tag];
    set.desc_size = u32();
    for (uint32_t nc = u32(); ok && nc > 0; --nc) {
      auto& states = set.by_class[u32()];
      for (uint32_t ns = u32(); ok && ns > 0; --ns) {
        Seed::State st;
        st.count = u32();
        if (p + set.desc_size > b.size()) {
          ok = false;
          break;
        }
        st.bytes.assign(b.begin() + p, b.begin() + p + set.desc_size);
        p += set.desc_size;
        states.push_back(std::move(st));
      }
    }
  }
  if (!ok) {
    REXLOG_WARN("odisea prewarm: semilla truncada ({})", path.string());
    return {};
  }
  size_t ndecl = 0;
  for (const auto& [vf, l] : seed.decls) ndecl += l.size();
  REXLOG_INFO("odisea prewarm: semilla cargada: {} declaraciones, {} juegos de estados", ndecl, seed.sets.size());
  return seed;
}

}  // namespace

const Seed& GetSeed() {
  static const Seed seed = LoadSeed();
  return seed;
}

// --- Esenciales ------------------------------------------------------------------

namespace {
std::atomic<EssentialsProviderFn> g_essentials_provider{nullptr};
}  // namespace

EssentialsProviderFn EssentialsProvider() { return g_essentials_provider.load(std::memory_order_acquire); }

std::vector<uint8_t> LoadEssentialsFile(const char* kind) {
  HMODULE module = nullptr;
  wchar_t path_buf[MAX_PATH];
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&GetSeed), &module) ||
      !GetModuleFileNameW(module, path_buf, MAX_PATH)) {
    return {};
  }
  const std::filesystem::path path =
      std::filesystem::path(path_buf).parent_path() / (std::string("prewarm_essentials.") + kind + ".xpso");
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    REXLOG_INFO("odisea prewarm: sin pipelines esenciales ({})", path.string());
    return {};
  }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// --- Generacion completa ----------------------------------------------------------

void SetLibraryPath(const std::filesystem::path& library_path) {
  std::lock_guard lock(g_bulk_mutex);
  g_library_path = library_path;
}

bool BulkActive() { return g_bulk_active.load(std::memory_order_acquire); }
bool BulkFast() { return BulkActive() && !g_bulk_detached.load(std::memory_order_acquire); }

// Solo la generacion completa con pantalla. Los urgentes (zona en carga) van delante pero al ritmo
// de siempre: a toda maquina eran miles de pipelines durante minutos con el juego ya en marcha
// (20 fps, 7-oct-2026); lo que de verdad pide un dibujo ya se crea aparte sin limite.
bool CreationFast() { return BulkFast(); }

size_t PendingJobs() {
  std::lock_guard lock(g_jobs_mutex);
  return g_jobs.size();
}

void NoteBulkJobDone(Job& job) {
  if (!job.bulk || job.counted) return;
  job.counted = true;
  g_bulk_done.fetch_add(1, std::memory_order_relaxed);
  // La barra cuenta pares revisados (antes, solo los resueltos: los aplazados por falta de
  // estados nunca avanzaban y parecia atascada, 18 / 11648).
  if (!g_bulk_detached.load(std::memory_order_relaxed)) ShaderPrepStep(2);
}

void NoteBulkJobResolved(Job& job) {
  if (!job.bulk || job.resolved) return;
  job.resolved = true;
  g_bulk_resolved.fetch_add(1, std::memory_order_relaxed);
}

bool BulkAllProcessed() {
  return BulkActive() &&
         g_bulk_done.load(std::memory_order_relaxed) >= g_bulk_total.load(std::memory_order_relaxed);
}

void FinishBulk() {
  if (!g_bulk_active.exchange(false)) return;
  std::filesystem::path marker;
  {
    std::lock_guard lock(g_bulk_mutex);
    marker = BulkMarker();
  }
  std::ofstream(marker, std::ios::binary) << "lost odyssey: pipelines del disco precreados\n";
  if (!g_bulk_detached.exchange(false)) ShaderPrepEnd();
  REXLOG_INFO("odisea prewarm: generacion completa terminada ({} pares, {} precreados, {} omitidos); marca {}",
              g_bulk_total.load(), g_bulk_resolved.load(), g_bulk_total.load() - g_bulk_resolved.load(),
              marker.filename().string());
}

int LowerThreadPriority() {
  const int old = GetThreadPriority(GetCurrentThread());
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
  return old;
}
void RestoreThreadPriority(int priority) { SetThreadPriority(GetCurrentThread(), priority); }

// --- Tirones ---------------------------------------------------------------------

void NoteSubmissionForStutter() {
  g_pump_alive.store(true, std::memory_order_release);
  using clock = std::chrono::steady_clock;
  static clock::time_point last{}, window = clock::now();
  static uint32_t over50 = 0, over100 = 0, submissions = 0;
  static double worst = 0.0;
  const clock::time_point now = clock::now();
  if (last != clock::time_point{}) {
    const double ms = std::chrono::duration<double, std::milli>(now - last).count();
    over50 += ms > 50.0;
    over100 += ms > 100.0;
    worst = std::max(worst, ms);
  }
  last = now;
  ++submissions;
  if (now - window >= std::chrono::seconds(10)) {
    REXLOG_INFO("odisea tirones: {} huecos de mas de 50 ms, {} de mas de 100 ms, peor {:.0f} ms ({} envios en 10 s)",
                over50, over100, worst, submissions);
    window = now;
    over50 = over100 = submissions = 0;
    worst = 0.0;
  }
}

// --- Estadistica ----------------------------------------------------------------

void NoteQueued(uint32_t pipelines) { g_queued.fetch_add(pipelines, std::memory_order_relaxed); }
void NotePrewarmHit() { g_hits.fetch_add(1, std::memory_order_relaxed); }

void MaybeLogStats(size_t in_flight, size_t decls) {
  using clock = std::chrono::steady_clock;
  static clock::time_point last = clock::now();
  static uint32_t last_queued = 0, last_hits = 0, last_received = 0;
  const auto now = clock::now();
  if (now - last < std::chrono::seconds(10)) return;
  last = now;
  const uint32_t q = g_queued.load(), h = g_hits.load(), r = g_received.load();
  if (q == last_queued && h == last_hits && r == last_received && !BulkActive()) return;
  last_queued = q;
  last_hits = h;
  last_received = r;
  size_t pending;
  {
    std::lock_guard lock(g_jobs_mutex);
    pending = g_jobs.size() + g_deferred.size();
  }
  REXLOG_INFO("odisea prewarm: {} pares recibidos, {} pipelines precreados, {} usados en partida, "
              "{} pares esperando, {} creandose{}, {} declaraciones de vertices; generacion: {} revisados, "
              "{} resueltos de {}",
              r, q, h, pending, in_flight, BulkActive() ? " (generacion completa)" : "", decls,
              g_bulk_done.load(), g_bulk_resolved.load(), g_bulk_total.load());
}

}  // namespace rex::graphics::odisea::prewarm

// Lo llama el exe (shader_prewarm.cpp). Formato estable, C puro.
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

extern "C" __declspec(dllexport) void odisea_PrewarmSubmit(const OdiseaPrewarmPair* pairs, uint32_t count) {
  using namespace rex::graphics::odisea::prewarm;
  if (!pairs || !count || !Enabled()) return;
  std::lock_guard lock(g_jobs_mutex);
  // Tope: si el backend no consume la cola (Vulkan aun no precrea), que no crezca sin fin.
  constexpr size_t kMaxQueued = 20000;
  // Lo del paquete que se esta cargando va delante de todo (de la generacion completa en segundo
  // plano, sobre todo: detras de ella llegaria tarde y la zona saldria a trozos), en su orden.
  size_t front = 0;
  for (uint32_t i = 0; i < count && g_jobs.size() < kMaxQueued; ++i) {
    const OdiseaPrewarmPair& p = pairs[i];
    if (!p.vs_ucode || !p.ps_ucode || !p.vs_dword_count || !p.ps_dword_count) continue;
    Job j;
    j.vs_disc.assign(p.vs_ucode, p.vs_ucode + p.vs_dword_count);
    if (p.vs_header) j.vs_header.assign(p.vs_header, p.vs_header + p.vs_header_dword_count);
    j.ps.assign(p.ps_ucode, p.ps_ucode + p.ps_dword_count);
    j.vertex_factory = p.vertex_factory;
    j.pass_class = p.pass_class;
    j.urgent = true;
    g_jobs.insert(g_jobs.begin() + ptrdiff_t(front++), std::move(j));
  }
  g_received.fetch_add(count, std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) void odisea_PrewarmStats(uint32_t* received, uint32_t* queued, uint32_t* hits) {
  using namespace rex::graphics::odisea::prewarm;
  if (received) *received = g_received.load();
  if (queued) *queued = g_queued.load();
  if (hits) *hits = g_hits.load();
}

// Generacion completa (exe): 1 = hace falta para este disco, 0 = ya esta (o
// precreacion apagada), -1 = el plugin aun no ha abierto su biblioteca.
extern "C" __declspec(dllexport) int odisea_PrewarmBulkNeeded(uint32_t disc) {
  using namespace rex::graphics::odisea::prewarm;
  if (!Enabled()) return 0;
  if (!g_pump_alive.load(std::memory_order_acquire)) return -1;  // carga del arranque en curso
  std::lock_guard lock(g_bulk_mutex);
  if (g_library_path.empty()) return -1;
  g_bulk_disc = disc;
  std::error_code ec;
  return std::filesystem::exists(BulkMarker(), ec) ? 0 : 1;
}

// Lectura del disco por el exe: fase 1 de la pantalla nativa.
extern "C" __declspec(dllexport) void odisea_PrewarmScanBegin(uint32_t packages) {
  rex::graphics::odisea::ShaderPrepBegin(1, packages);
}
extern "C" __declspec(dllexport) void odisea_PrewarmScanStep() { rex::graphics::odisea::ShaderPrepStep(1); }

// Todos los pares del disco; la pantalla pasa a la fase 2 ("Creando pipelines").
extern "C" __declspec(dllexport) void odisea_PrewarmSubmitBulk(const OdiseaPrewarmPair* pairs, uint32_t count) {
  using namespace rex::graphics::odisea;
  using namespace rex::graphics::odisea::prewarm;
  if (!Enabled()) {
    ShaderPrepEnd();
    return;
  }
  // Otra generacion aun en marcha (no deberia: el exe espera a odisea_PrewarmBulkState() == 0):
  // los pares se crean igual, pero sin pantalla ni marca, para no mezclar contadores y discos.
  const bool overlapped = BulkActive();
  {
    std::lock_guard lock(g_jobs_mutex);
    for (uint32_t i = 0; i < count; ++i) {
      const OdiseaPrewarmPair& p = pairs[i];
      if (!p.vs_ucode || !p.ps_ucode || !p.vs_dword_count || !p.ps_dword_count) continue;
      Job j;
      j.vs_disc.assign(p.vs_ucode, p.vs_ucode + p.vs_dword_count);
      if (p.vs_header) j.vs_header.assign(p.vs_header, p.vs_header + p.vs_header_dword_count);
      j.ps.assign(p.ps_ucode, p.ps_ucode + p.ps_dword_count);
      j.vertex_factory = p.vertex_factory;
      j.pass_class = p.pass_class;
      j.bulk = !overlapped;
      g_jobs.push_back(std::move(j));
    }
  }
  g_received.fetch_add(count, std::memory_order_relaxed);
  if (overlapped) {
    g_next_bulk_detached = false;
    REXLOG_WARN("odisea prewarm: generacion del disco {} pedida con otra en marcha: {} pares sin pantalla",
                g_bulk_disc, count);
    return;
  }
  g_bulk_done = 0;
  g_bulk_resolved = 0;
  g_bulk_total = count;
  const bool detached = g_next_bulk_detached.exchange(false);
  g_bulk_detached = detached && count != 0;
  g_bulk_active = count != 0;
  if (count && !detached) {
    ShaderPrepBegin(2, count);
  } else {
    ShaderPrepEnd();
  }
  REXLOG_INFO("odisea prewarm: generacion completa del disco {}: {} pares", g_bulk_disc, count);
}

// Estado de la generacion completa para el exe: 0 = ninguna, 1 = en marcha con pantalla,
// 2 = en marcha en segundo plano. El exe espera a 0 antes de pasar al disco siguiente
// (odisea_PrewarmBulkNeeded cambia el disco de la marca).
extern "C" __declspec(dllexport) int odisea_PrewarmBulkState() {
  using namespace rex::graphics::odisea::prewarm;
  if (!BulkActive()) return 0;
  return BulkFast() ? 1 : 2;
}

// La siguiente generacion completa (odisea_PrewarmSubmitBulk) empieza en segundo plano.
extern "C" __declspec(dllexport) void odisea_PrewarmNextBulkDetached(int detached) {
  rex::graphics::odisea::prewarm::g_next_bulk_detached.store(detached != 0);
}

// Pantalla de preparacion (exe): 1 si se puede pulsar "Jugar ya" (generacion
// completa en marcha y aun no soltada).
extern "C" __declspec(dllexport) int odisea_PrewarmCanSkip() {
  return rex::graphics::odisea::prewarm::BulkFast() ? 1 : 0;
}

// "Jugar ya": la pantalla se cierra y la generacion sigue en segundo plano con
// la prioridad baja de la precreacion normal (la marca se pone al acabar).
extern "C" __declspec(dllexport) void odisea_PrewarmSkip() {
  using namespace rex::graphics::odisea::prewarm;
  if (!BulkFast() || g_bulk_detached.exchange(true)) return;
  rex::graphics::odisea::ShaderPrepEnd();
  REXLOG_INFO("odisea prewarm: Jugar ya: la generacion completa sigue en segundo plano ({} de {} pares)",
              g_bulk_done.load(), g_bulk_total.load());
}

// El exe registra el proveedor del microcodigo del disco (ver LoadEssentialsFile).
extern "C" __declspec(dllexport) void odisea_SetEssentialsProvider(
    rex::graphics::odisea::prewarm::EssentialsProviderFn fn) {
  rex::graphics::odisea::prewarm::g_essentials_provider.store(fn, std::memory_order_release);
}

// La generacion completa de ese disco ya esta hecha (marca junto a la biblioteca): para los ticks de
// la pantalla de preparacion del exe.
extern "C" __declspec(dllexport) int odisea_PrewarmDiscDone(uint32_t disc) {
  using namespace rex::graphics::odisea::prewarm;
  std::filesystem::path marker;
  {
    std::lock_guard lock(g_bulk_mutex);
    if (g_library_path.empty()) return 0;
    marker = g_library_path;
  }
  marker += ".disc" + std::to_string(disc) + ".prewarm";
  std::error_code ec;
  return std::filesystem::exists(marker, ec) ? 1 : 0;
}
