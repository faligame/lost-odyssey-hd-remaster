// Fork (odisea): pack de texturas personalizadas (HD). Ver texture_replace.h.
#include <rex/graphics/odisea_texture_pack.h>

#include "odisea_lopack.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/image_decode.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <thread>
#include <deque>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(odisea_texture_pack, false, "GPU/Textures",
                    "odisea: cargar texturas personalizadas desde la carpeta del pack");
REXCVAR_DEFINE_STRING(odisea_texture_pack_dir, "textures", "GPU/Textures",
                      "odisea: carpeta del pack de texturas (junto al ejecutable)");
REXCVAR_DEFINE_STRING(odisea_texture_ui_pack_file, "ui.lopack", "GPU/Textures",
                      "Pack cifrado de la interfaz que viaja con el juego (logo, fuentes x4, botones del mando); "
                      "relativo a la carpeta del exe. Siempre se carga y manda sobre el pack descargado");
REXCVAR_DEFINE_STRING(odisea_texture_pack_file, "textures.lopack", "GPU/Textures",
                      "odisea: pack de texturas cifrado (.lopack). Si existe y los discos son los suyos, "
                      "manda sobre la carpeta suelta");
REXCVAR_DEFINE_BOOL(odisea_texture_pack_reload, false, "GPU/Textures",
                    "odisea: releer el pack de texturas del disco (se apaga solo)");

// Fork (odisea): volcado de texturas del guest (bytes en formato/tiling
// nativos + metadatos en el nombre) a dump/textures/, para localizar assets
// (p. ej. el atlas de glifos de botones) y preparar sustituciones. Lo leen los
// texture caches de los dos backends.
REXCVAR_DEFINE_STRING(odisea_watch_texture, "", "GPU/Textures",
                      "Hash (hex) de una textura del pack cuyo uso se vigila; el exe lo consulta con "
                      "odisea_WatchedTextureLastSeenMs");
REXCVAR_DEFINE_BOOL(odisea_texture_pack_async, true, "GPU/Textures",
                    "Carga las texturas HD en segundo plano (se ve la original un instante en vez "
                    "de pararse el juego)");
REXCVAR_DEFINE_BOOL(odisea_dump_textures, false, "GPU/Debug",
                    "odisea: dump every loaded guest texture (base level) to dump/textures/");

namespace rex::graphics::odisea {

namespace {

std::mutex g_mutex;
bool g_index_built = false;
std::map<uint64_t, std::filesystem::path> g_index;
// Pack cifrado (.lopack): si esta abierto, las texturas salen de el y no de la carpeta suelta.
std::shared_ptr<lopack::Pack> g_pack;
// Pack cifrado de la interfaz (viene con el juego, no se descarga): manda sobre g_pack.
std::shared_ptr<lopack::Pack> g_ui_pack;

// Pack que tiene esta textura (el de interfaz primero), o nullptr.
std::shared_ptr<lopack::Pack> PackFor(uint64_t hash) {
  if (g_ui_pack && g_ui_pack->Has(hash)) return g_ui_pack;
  if (g_pack && g_pack->Has(hash)) return g_pack;
  return nullptr;
}
std::map<uint64_t, ReplacementImage> g_cache;
std::map<std::pair<uint64_t, uint64_t>, uint64_t> g_keys;
std::set<std::pair<uint64_t, uint64_t>> g_uploaded;
uint64_t g_upload_skips = 0;
bool g_reload_pending = false;
// Carga en segundo plano (ver EnqueueLoad).
std::set<uint64_t> g_loading;                     // hashes en cola o cargandose
std::deque<uint64_t> g_load_queue;
std::map<uint64_t, std::vector<std::pair<uint64_t, uint64_t>>> g_pending_keys;  // hash -> texturas
std::vector<std::pair<uint64_t, uint64_t>> g_ready_keys;  // texturas a expulsar
std::map<std::pair<uint64_t, uint64_t>, uint64_t> g_key_content;  // llave -> huella del contenido
std::condition_variable g_load_cond;
std::vector<std::thread> g_load_threads;
uint64_t g_reload_generation = 0;

// Nombres válidos: tex_<16 hex>[_loquesea].png (el mismo prefijo que genera el
// volcado, para poder editar los PNG sin renombrarlos).
bool ParseHash(const std::string& name, uint64_t& hash_out) {
  if (name.size() < 20 || name.compare(0, 4, "tex_") != 0) return false;
  uint64_t h = 0;
  for (size_t i = 4; i < 20; ++i) {
    char c = name[i];
    uint64_t v;
    if (c >= '0' && c <= '9') {
      v = uint64_t(c - '0');
    } else if (c >= 'A' && c <= 'F') {
      v = uint64_t(c - 'A' + 10);
    } else if (c >= 'a' && c <= 'f') {
      v = uint64_t(c - 'a' + 10);
    } else {
      return false;
    }
    h = (h << 4) | v;
  }
  hash_out = h;
  return true;
}

uint32_t ReadU32(const std::vector<uint8_t>& file, size_t offset) {
  uint32_t v;
  std::memcpy(&v, file.data() + offset, 4);
  return v;  // los DDS son little-endian, como el host
}

// DDS 2D con BC1, BC3 o BC7 (cabecera clasica DXT1/DXT5 o extension DX10).
// Se queda con el fichero tal cual: los niveles van seguidos tras la cabecera.
bool ParseDds(std::vector<uint8_t>& file, ReplacementImage& out) {
  if (file.size() < 128 || ReadU32(file, 4) != 124) return false;
  uint32_t height = ReadU32(file, 12), width = ReadU32(file, 16);
  uint32_t levels = std::max(ReadU32(file, 28), uint32_t(1));
  uint32_t fourcc = ReadU32(file, 84);
  size_t data_offset = 128;
  ReplacementFormat format;
  if (fourcc == 0x30315844) {  // "DX10"
    if (file.size() < 148) return false;
    data_offset = 148;
    switch (ReadU32(file, 128)) {  // DXGI_FORMAT
      case 71:
      case 72:
        format = ReplacementFormat::kBC1;
        break;
      case 77:
      case 78:
        format = ReplacementFormat::kBC3;
        break;
      case 98:
      case 99:
        format = ReplacementFormat::kBC7;
        break;
      default:
        return false;
    }
    if (ReadU32(file, 132) != 3 || ReadU32(file, 140) != 1) return false;  // 2D, sin array
  } else if (fourcc == 0x31545844) {  // "DXT1"
    format = ReplacementFormat::kBC1;
  } else if (fourcc == 0x35545844) {  // "DXT5"
    format = ReplacementFormat::kBC3;
  } else {
    return false;
  }
  if (!width || !height || width > 16384 || height > 16384) return false;
  out.width = width;
  out.height = height;
  out.format = format;
  size_t offset = data_offset;
  for (uint32_t level = 0; level < levels; ++level) {
    uint32_t lw, lh, row_bytes, rows;
    ReplacementLevelLayout(out, level, lw, lh, row_bytes, rows);
    size_t size = size_t(row_bytes) * rows;
    if (offset + size > file.size()) break;  // fichero con menos niveles de los que dice
    out.level_offsets.push_back(offset);
    offset += size;
  }
  if (out.level_offsets.empty()) return false;
  out.rgba = std::move(file);
  return true;
}

// Abre el pack cifrado si existe. Con g_mutex tomado.
std::shared_ptr<lopack::Pack> OpenPackFile(std::filesystem::path file) {
  std::shared_ptr<lopack::Pack> none;
  std::error_code ec;
  if (file.empty()) return none;
  if (file.is_relative()) {
    // Relativa a la carpeta del exe (no al directorio de trabajo: un acceso directo puede tener otro).
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) file = std::filesystem::path(exe).parent_path() / file;
  }
  if (!std::filesystem::is_regular_file(file, ec)) return none;
  const lopack::SecretProviderFn provider = lopack::GetSecretProvider();
  uint8_t secret[32] = {};
  if (!provider || !provider(secret)) {
    REXGPU_WARN("odisea texture pack: '{}' existe pero no se encuentra el disco 1 para abrirlo", file.string());
    return none;
  }
  auto pack = std::make_shared<lopack::Pack>();
  const bool ok = pack->Open(file, secret);
  std::memset(secret, 0, sizeof(secret));
  if (!ok) {
    REXGPU_ERROR("odisea texture pack: '{}' no se puede abrir con los discos instalados (otra edicion, "
                 "discos modificados o pack danado)", file.string());
    return none;
  }
  REXGPU_INFO("odisea texture pack: {} texturas en '{}' (cifrado, contenido v{})", pack->size(), file.string(),
              pack->content_version());
  return pack;
}

void OpenPack() {
  if (!g_ui_pack) g_ui_pack = OpenPackFile(REXCVAR_GET(odisea_texture_ui_pack_file));
  g_pack = OpenPackFile(REXCVAR_GET(odisea_texture_pack_file));
}

void BuildIndex() {
  g_index_built = true;
  OpenPack();
  if (g_pack || g_ui_pack) return;  // con un pack cifrado no se mira la carpeta suelta
  std::filesystem::path dir = REXCVAR_GET(odisea_texture_pack_dir);
  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
    REXGPU_INFO("odisea texture pack: carpeta '{}' no encontrada", dir.string());
    return;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;
    std::string ext = entry.path().extension().string();
    for (char& c : ext) c = char(tolower(c));
    if (ext != ".png" && ext != ".dds") continue;
    uint64_t hash = 0;
    if (!ParseHash(entry.path().filename().string(), hash)) continue;
    // Si una textura esta en los dos formatos manda el DDS (ya comprimido).
    if (ext == ".dds") {
      g_index[hash] = entry.path();
    } else {
      g_index.emplace(hash, entry.path());
    }
  }
  REXGPU_INFO("odisea texture pack: {} texturas en '{}'", g_index.size(), dir.string());
}

}  // namespace

bool TexturePackEnabled() { return REXCVAR_GET(odisea_texture_pack); }

TexturePackReloadStep TexturePackPollReload() {
  if (g_reload_pending) {
    g_reload_pending = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_index.clear();
    g_pack.reset();
    g_ui_pack.reset();
    g_cache.clear();
    g_keys.clear();
    g_key_content.clear();
    g_uploaded.clear();
    g_pending_keys.clear();
    g_ready_keys.clear();
    g_load_queue.clear();
    ++g_reload_generation;
    g_index_built = false;
    REXGPU_INFO("odisea texture pack: releyendo del disco");
    return TexturePackReloadStep::kFinish;
  }
  if (!REXCVAR_GET(odisea_texture_pack_reload)) {
    return TexturePackReloadStep::kNone;
  }
  rex::cvar::SetFlagByName("odisea_texture_pack_reload", "false");
  g_reload_pending = true;
  return TexturePackReloadStep::kRequestCacheClear;
}

// --- Textura vigilada ---------------------------------------------------------
// El exe pone en odisea_watch_texture el hash de una textura del pack (p. ej.
// el titulo) y pregunta con odisea_WatchedTextureLastSeenMs (exportada)
// cuando se uso por ultima vez en un dibujo: asi sabe que pantalla esta
// mostrando el juego sin tener que adivinar sus estados.
namespace {
std::atomic<bool> g_watch_valid{false};
std::atomic<uint64_t> g_watch_lo{0}, g_watch_hi{0};
std::atomic<uint32_t> g_watch_page{0};
std::atomic<int64_t> g_watch_seen_ms{0};

uint64_t WatchedHash() {
  const std::string text = REXCVAR_GET(odisea_watch_texture);
  if (text.empty()) return 0;
  return std::strtoull(text.c_str(), nullptr, 16);
}
}  // namespace

bool IsWatchedTextureKey(uint64_t key_lo, uint64_t key_hi) {
  return g_watch_valid.load(std::memory_order_relaxed) &&
         g_watch_lo.load(std::memory_order_relaxed) == key_lo &&
         g_watch_hi.load(std::memory_order_relaxed) == key_hi;
}

bool TextureWatchActive() { return g_watch_valid.load(std::memory_order_relaxed); }

uint32_t WatchedTextureBasePage() { return g_watch_page.load(std::memory_order_relaxed); }

void MarkWatchedTextureSeen() {
  g_watch_seen_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count(),
                        std::memory_order_relaxed);
}

int64_t WatchedTextureLastSeenMs() { return g_watch_seen_ms.load(std::memory_order_relaxed); }

void SetReplacementForKey(uint64_t key_lo, uint64_t key_hi, uint64_t hash) {
  if (hash && hash == WatchedHash()) {
    g_watch_lo.store(key_lo, std::memory_order_relaxed);
    g_watch_hi.store(key_hi, std::memory_order_relaxed);
    g_watch_page.store(uint32_t(key_lo & 0x1FFFF), std::memory_order_relaxed);  // TextureKey::base_page
    g_watch_valid.store(true, std::memory_order_release);
    REXGPU_INFO("odisea: textura vigilada {:016X} creada", hash);
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  auto k = std::make_pair(key_lo, key_hi);
  g_uploaded.erase(k);  // textura recién creada: su recurso está vacío
  if (hash) {
    g_keys[k] = hash;
  } else {
    g_keys.erase(k);  // esta clave ya no tiene sustitución
  }
}

bool ReplacementNeedsUpload(uint64_t key_lo, uint64_t key_hi) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_uploaded.insert(std::make_pair(key_lo, key_hi)).second) return true;
  // Aviso con cuentagotas: sirve para ver en el log cuántas recargas se ahorran.
  ++g_upload_skips;
  if (g_upload_skips == 1 || g_upload_skips % 1000 == 0) {
    REXGPU_INFO("odisea texture pack: {} recargas de texturas ya subidas evitadas",
                g_upload_skips);
  }
  return false;
}

const ReplacementImage* FindReplacementForKey(uint64_t key_lo, uint64_t key_hi) {
  if (!TexturePackEnabled()) return nullptr;
  uint64_t hash;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_keys.find(std::make_pair(key_lo, key_hi));
    if (it == g_keys.end()) return nullptr;
    hash = it->second;
    auto cached = g_cache.find(hash);
    if (cached != g_cache.end()) return cached->second.width ? &cached->second : nullptr;
  }
  return FindReplacement(hash);
}

namespace {

// Lee y descodifica un fichero del pack (DDS ya comprimido o PNG). Sin cerrojos:
// la usan el hilo de la GPU (modo sincrono) y los hilos de carga.
bool LoadReplacementFile(const std::filesystem::path& path, ReplacementImage& out) {
  std::vector<uint8_t> file;
  {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size < 256 * 1024 * 1024) {
      file.resize(size_t(size));
      if (fread(file.data(), 1, file.size(), f) != file.size()) file.clear();
    }
    fclose(f);
  }
  if (file.empty()) return false;
  if (file.size() >= 4 && std::memcmp(file.data(), "DDS ", 4) == 0) {
    ReplacementImage dds;
    if (!ParseDds(file, dds)) {
      REXGPU_ERROR("odisea texture pack: DDS no admitido (solo BC1/BC3/BC7 2D) {}", path.string());
      return false;
    }
    out = std::move(dds);
    return true;
  }
  int w = 0, h = 0;
  std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(file.data(), file.size(), w, h);
  if (rgba.empty() || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
    REXGPU_ERROR("odisea texture pack: no se pudo decodificar {}", path.string());
    return false;
  }
  out.width = uint32_t(w);
  out.height = uint32_t(h);
  out.rgba = std::move(rgba);
  return true;
}

// Imagen del pack cifrado (DDS o PNG de la interfaz). Sin cerrojos.
bool LoadFromPack(const lopack::Pack& pack, uint64_t hash, ReplacementImage& out) {
  std::vector<uint8_t> bytes;
  lopack::Kind kind;
  if (!pack.Read(hash, bytes, kind)) {
    REXGPU_ERROR("odisea texture pack: no se pudo leer/descifrar la textura {:016X}", hash);
    return false;
  }
  if (kind == lopack::Kind::kDds) {
    ReplacementImage dds;
    if (!ParseDds(bytes, dds)) return false;
    out = std::move(dds);
    return true;
  }
  int w = 0, h = 0;
  std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), w, h);
  if (rgba.empty() || w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
  out.width = uint32_t(w);
  out.height = uint32_t(h);
  out.rgba = std::move(rgba);
  return true;
}

// Hay imagen para ese hash (en el pack o en la carpeta). Con g_mutex tomado.
bool HasImage(uint64_t hash) { return PackFor(hash) != nullptr || g_index.count(hash) != 0; }

// --- Carga en segundo plano ------------------------------------------------------
// Leer un DDS de varios MB en el hilo de la GPU bloquea el dibujo (al entrar en
// una zona se juntaban ~90 cargas y casi 1 s de bloqueo cada 10 s). En modo
// asincrono la primera peticion encola la carga y la textura se crea con la
// imagen original; cuando la HD esta lista, la cache de texturas expulsa esa
// textura (TexturePackTakeReadyKeys) y al volver a pedirla se crea ya con la HD.


void LoadThread() {
  for (;;) {
    uint64_t hash;
    std::filesystem::path path;
    std::shared_ptr<lopack::Pack> pack;
    uint64_t generation;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_load_cond.wait(lock, [] { return !g_load_queue.empty(); });
      hash = g_load_queue.front();
      g_load_queue.pop_front();
      if (auto owner = PackFor(hash)) {
        pack = owner;
      } else {
        auto it = g_index.find(hash);
        if (it == g_index.end()) {
          g_loading.erase(hash);
          continue;
        }
        path = it->second;
      }
      generation = g_reload_generation;
    }
    ReplacementImage image;
    const bool ok = pack ? LoadFromPack(*pack, hash, image) : LoadReplacementFile(path, image);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_loading.erase(hash);
    if (generation != g_reload_generation) continue;  // el pack se recargo mientras tanto
    ReplacementImage& slot = g_cache[hash];
    if (ok) slot = std::move(image);
    auto pending = g_pending_keys.find(hash);
    if (pending != g_pending_keys.end()) {
      if (ok) {
        g_ready_keys.insert(g_ready_keys.end(), pending->second.begin(), pending->second.end());
      }
      g_pending_keys.erase(pending);
    }
  }
}

// Con g_mutex tomado.
void EnqueueLoad(uint64_t hash) {
  if (!g_loading.insert(hash).second) return;
  if (g_load_threads.empty()) {
    const unsigned n = std::max(2u, std::min(4u, std::thread::hardware_concurrency() / 4));
    for (unsigned i = 0; i < n; ++i) {
      g_load_threads.emplace_back(LoadThread);
      g_load_threads.back().detach();
    }
  }
  g_load_queue.push_back(hash);
  g_load_cond.notify_one();
}

}  // namespace

// Imagen de la interfaz (ui.lopack) para el exe: las paginas HD de las fuentes del menu del asistente. Abre el
// pack si hace falta (el exe lo pide antes de que el juego cargue texturas).
bool UiImageRead(uint64_t hash, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_ui_pack) g_ui_pack = OpenPackFile(REXCVAR_GET(odisea_texture_ui_pack_file));
  if (!g_ui_pack || !g_ui_pack->Has(hash)) return false;
  ReplacementImage image;
  if (!LoadFromPack(*g_ui_pack, hash, image) || !image.width || !image.height) return false;
  width = image.width;
  height = image.height;
  rgba = std::move(image.rgba);
  return true;
}

void NotePendingReplacement(uint64_t key_lo, uint64_t key_hi, uint64_t hash) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_loading.count(hash)) {
    g_pending_keys[hash].emplace_back(key_lo, key_hi);
    return;
  }
  // Termino entre la creacion de la textura y este aviso: expulsarla ya.
  auto cached = g_cache.find(hash);
  if (cached != g_cache.end() && cached->second.width) g_ready_keys.emplace_back(key_lo, key_hi);
}

void NoteTextureContent(uint64_t key_lo, uint64_t key_hi, uint64_t content_hash) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_key_content[std::make_pair(key_lo, key_hi)] = content_hash;
}

bool TextureContentTracked(uint64_t key_lo, uint64_t key_hi) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_key_content.count(std::make_pair(key_lo, key_hi)) != 0;
}

bool TextureContentChanged(uint64_t key_lo, uint64_t key_hi, uint64_t content_hash) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto k = std::make_pair(key_lo, key_hi);
  auto it = g_key_content.find(k);
  if (it == g_key_content.end() || it->second == content_hash) return false;
  it->second = content_hash;
  if (g_keys.count(k) || (content_hash && HasImage(content_hash))) g_ready_keys.push_back(k);
  return true;
}

void TexturePackTakeReadyKeys(std::vector<std::pair<uint64_t, uint64_t>>& out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  out.insert(out.end(), g_ready_keys.begin(), g_ready_keys.end());
  g_ready_keys.clear();
}

const ReplacementImage* FindReplacement(uint64_t hash) {
  if (!TexturePackEnabled()) return nullptr;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_index_built) BuildIndex();
  auto cached = g_cache.find(hash);
  if (cached != g_cache.end()) {
    return cached->second.width ? &cached->second : nullptr;
  }
  const std::shared_ptr<lopack::Pack> owner = PackFor(hash);
  const bool in_pack = owner != nullptr;
  auto it = g_index.find(hash);
  if (!in_pack && it == g_index.end()) {
    g_cache[hash];  // vacía = "no hay sustitución"
    return nullptr;
  }
  // Los PNG del pack son la interfaz (paginas de las fuentes, botones, logo):
  // pocos y pequenos, y deben verse desde el primer momento. Van al instante.
  bool is_png;
  if (in_pack) {
    lopack::Kind kind = lopack::Kind::kDds;
    owner->KindOf(hash, kind);
    is_png = kind == lopack::Kind::kPng;
  } else {
    std::string ext = it->second.extension().string();
    for (char& c : ext) c = char(tolower(c));
    is_png = ext == ".png";
  }
  if (REXCVAR_GET(odisea_texture_pack_async) && !is_png) {
    EnqueueLoad(hash);
    return nullptr;  // de momento, la original
  }
  ReplacementImage& slot = g_cache[hash];
  if (!(in_pack ? LoadFromPack(*owner, hash, slot) : LoadReplacementFile(it->second, slot))) return nullptr;
  return slot.width ? &slot : nullptr;
}

}  // namespace rex::graphics::odisea

// Consulta desde el exe (GetProcAddress sobre rexgpu-odisea.dll): ultima vez,
// en ms de steady_clock, que un dibujo uso la textura vigilada (0 = nunca).
extern "C" __declspec(dllexport) int64_t odisea_WatchedTextureLastSeenMs() {
  return rex::graphics::odisea::WatchedTextureLastSeenMs();
}

// Consulta desde el exe: imagen RGBA8 de la interfaz por huella de textura. Con out == nullptr solo devuelve
// el tamano. 1 = hay imagen, 0 = no.
extern "C" __declspec(dllexport) int odisea_UiImageRgba(uint64_t hash, uint32_t* width, uint32_t* height,
                                                        uint8_t* out, uint64_t capacity) {
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
  if (!rex::graphics::odisea::UiImageRead(hash, w, h, rgba)) return 0;
  *width = w;
  *height = h;
  if (!out) return 1;
  if (capacity < rgba.size()) return 0;
  std::memcpy(out, rgba.data(), rgba.size());
  return 1;
}
