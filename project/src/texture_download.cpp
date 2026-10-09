// lostodyssey - ReXGlue Recompiled Project
// Ver texture_download.h.
#include "texture_download.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "lo_hash.h"
#include "lo_http.h"
#include "lo_i18n.h"
#include "lo_options.h"
#include "lo_version.h"

REXCVAR_DEFINE_STRING(lo_texture_manifest_url, LO_TEXTURE_MANIFEST_URL, "LostOdyssey/Texturas",
                      "URL del manifiesto del pack de texturas HD (texture-pack.json)");

REXCVAR_DEFINE_INT32(lo_texture_connections, 6, "LostOdyssey/Texturas",
                     "Conexiones en paralelo para descargar el pack de texturas HD (1-16)");

namespace lo {

namespace {

std::mutex g_mutex;
TextureDownloadInfo g_info;
std::thread g_thread;
std::atomic<bool> g_cancel{false};
std::atomic<bool> g_running{false};

using namespace lo::http;

void SetInfo(const std::function<void(TextureDownloadInfo&)>& fn) {
  std::lock_guard lock(g_mutex);
  fn(g_info);
}

struct ManifestPart {
  std::string url;
  uint64_t start = 0;
  uint64_t size = 0;
};

struct Manifest {
  std::string file;
  uint64_t size = 0;
  std::string sha256;
  std::vector<ManifestPart> parts;  // tramos consecutivos del fichero final (uno solo si va entero)
};

// "parts": [{"name": "...", "size": N}, ...] con "base_urls" donde estan; o "urls" (fichero entero).
bool ParseManifest(const std::string& json, Manifest& m, std::string& error) {
  if (JsonNumber(json, "schema") != 1) {
    error = Tr("El manifiesto del pack tiene un formato desconocido.");
    return false;
  }
  m.file = JsonString(json, "file");
  m.size = JsonNumber(json, "size");
  m.sha256 = JsonString(json, "sha256");
  std::transform(m.sha256.begin(), m.sha256.end(), m.sha256.begin(), [](char c) { return char(std::tolower(c)); });
  if (m.size < 1000 || m.sha256.size() != 64) {
    error = Tr("El manifiesto del pack está incompleto.");
    return false;
  }
  const size_t parts_pos = json.find("\"parts\"");
  if (parts_pos != std::string::npos) {
    const std::vector<std::string> bases = JsonStrings(json, "base_urls");
    const size_t end = json.find(']', parts_pos);
    uint64_t start = 0;
    for (size_t p = json.find('{', parts_pos); p != std::string::npos && p < end; p = json.find('{', p + 1)) {
      const size_t close = json.find('}', p);
      if (close == std::string::npos) break;
      const std::string obj = json.substr(p, close - p + 1);
      const std::string name = JsonString(obj, "name");
      const uint64_t size = JsonNumber(obj, "size");
      if (name.empty() || !size || bases.empty()) {
        error = Tr("El manifiesto del pack está incompleto.");
        return false;
      }
      m.parts.push_back({bases.front() + name, start, size});
      start += size;
    }
    if (start != m.size) {
      error = Tr("El manifiesto del pack no cuadra con sus trozos.");
      return false;
    }
  } else {
    const std::vector<std::string> urls = JsonStrings(json, "urls");
    if (urls.empty()) {
      error = Tr("El manifiesto del pack está incompleto.");
      return false;
    }
    m.parts.push_back({urls.front(), 0, m.size});
  }
  return !m.parts.empty();
}

uint64_t FreeBytes(const std::filesystem::path& dir) {
  ULARGE_INTEGER free_bytes{};
  if (!GetDiskFreeSpaceExW(dir.c_str(), &free_bytes, nullptr, nullptr)) return ~0ull;  // no se sabe: no bloquear
  return free_bytes.QuadPart;
}

void Fail(const std::string& message) {
  REXLOG_WARN("[texturas] descarga: {}", message);
  SetInfo([&](TextureDownloadInfo& i) {
    i.state = TextureDownloadState::kError;
    i.message = message;
  });
}

void Worker() {
  using clock = std::chrono::steady_clock;
  Http http;
  if (!http.ok()) {
    Fail(Tr("No se pudo iniciar la red."));
    return;
  }
  SetInfo([](TextureDownloadInfo& i) { i = TextureDownloadInfo{TextureDownloadState::kManifest, 0, 0, 0.0, {}}; });

  // 1. Manifiesto.
  const std::string manifest_url = REXCVAR_GET(lo_texture_manifest_url);
  std::string json, error;
  int status = 0;
  uint64_t total_unused = 0;
  if (manifest_url.empty() || manifest_url.find("OWNER") != std::string::npos ||
      !http.Get(manifest_url, 0,
                [&](const uint8_t* d, size_t n) {
                  json.append(reinterpret_cast<const char*>(d), n);
                  return json.size() < (1u << 20);
                },
                status, total_unused, error)) {
    Fail(manifest_url.empty() || manifest_url.find("OWNER") != std::string::npos
             ? Tr("El pack de texturas HD aún no está publicado.")
             : Tr("No se pudo leer el manifiesto del pack: ") + error);
    return;
  }
  Manifest m;
  if (!ParseManifest(json, m, error)) {
    Fail(error);
    return;
  }

  // 2. Destino y espacio. El .part se crea del tamano final y se rellena por bloques; <dest>.part.map
  //    apunta los bloques completos (uno por linea) para reanudar sin repetirlos.
  const std::filesystem::path dest = TexturePackPath();
  const std::filesystem::path part = std::filesystem::path(dest).concat(".part");
  const std::filesystem::path map_path = std::filesystem::path(dest).concat(".part.map");
  std::error_code ec;
  std::filesystem::create_directories(dest.parent_path(), ec);
  constexpr uint64_t kBlock = 32ull << 20;
  struct Block {
    uint64_t start, size;
    const ManifestPart* part;
  };
  std::vector<Block> blocks;
  for (const ManifestPart& mp : m.parts) {
    for (uint64_t off = 0; off < mp.size; off += kBlock) {
      blocks.push_back({mp.start + off, std::min(kBlock, mp.size - off), &mp});
    }
  }
  std::vector<uint8_t> block_done(blocks.size(), 0);
  const bool had_part = std::filesystem::is_regular_file(part, ec);
  if (had_part && std::filesystem::file_size(part, ec) != m.size) {
    // .part de otra version o de la descarga secuencial antigua: se empieza de cero.
    std::filesystem::remove(part, ec);
    std::filesystem::remove(map_path, ec);
  }
  if (!std::filesystem::is_regular_file(part, ec)) {
    std::filesystem::remove(map_path, ec);
    if (FreeBytes(dest.parent_path()) < m.size + (512ull << 20)) {
      Fail(Tr("No hay espacio suficiente en el disco para el pack de texturas HD."));
      return;
    }
    std::ofstream create(part, std::ios::binary);
    create.close();
    std::filesystem::resize_file(part, m.size, ec);
    if (ec) {
      Fail(Tr("No se puede reservar el espacio del pack en el disco."));
      return;
    }
  }
  {
    std::ifstream map_in(map_path);
    for (size_t idx; map_in >> idx;) {
      if (idx < block_done.size()) block_done[idx] = 1;
    }
  }
  std::atomic<uint64_t> bytes_done{0};
  for (size_t i = 0; i < blocks.size(); ++i) {
    if (block_done[i]) bytes_done += blocks[i].size;
  }

  // 3. Descarga con varias conexiones (bloques de 32 MB, reintentos con espera creciente por bloque).
  SetInfo([&](TextureDownloadInfo& i) {
    i.state = TextureDownloadState::kDownloading;
    i.total = m.size;
    i.done = bytes_done.load();
  });
  std::mutex map_mutex;
  std::ofstream map_out(map_path, std::ios::app);
  std::atomic<size_t> next_block{0};
  std::atomic<bool> failed{false};
  std::atomic<int> workers_left{0};
  std::string worker_error;
  std::mutex error_mutex;
  const int connections = std::clamp(REXCVAR_GET(lo_texture_connections), 1, 16);
  auto worker = [&] {
    HANDLE file = CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      failed = true;
      std::lock_guard lock(error_mutex);
      worker_error = Tr("No se puede escribir el pack en el disco.");
      --workers_left;
      return;
    }
    for (;;) {
      const size_t i = next_block.fetch_add(1);
      if (i >= blocks.size() || failed.load() || g_cancel.load()) break;
      if (block_done[i]) continue;
      const Block& blk = blocks[i];
      bool ok_block = false;
      for (int attempt = 0; attempt < 8 && !g_cancel.load() && !failed.load(); ++attempt) {
        uint64_t written = 0;
        std::string get_error;
        int http_status = 0;
        uint64_t total = 0;
        bool write_ok = true;
        const uint64_t part_off = blk.start - blk.part->start;
        const bool ok = http.Get(
            blk.part->url, part_off,
            [&](const uint8_t* data, size_t n) {
              const size_t take = size_t(std::min<uint64_t>(n, blk.size - written));
              OVERLAPPED ov = {};
              const uint64_t at = blk.start + written;
              ov.Offset = DWORD(at);
              ov.OffsetHigh = DWORD(at >> 32);
              DWORD w = 0;
              if (!WriteFile(file, data, DWORD(take), &w, &ov) || w != take) {
                write_ok = false;
                return false;
              }
              written += take;
              bytes_done += take;
              return !g_cancel.load() && written < blk.size;
            },
            http_status, total, get_error, part_off + blk.size - 1);
        if (!write_ok) {
          bytes_done -= written;
          failed = true;
          std::lock_guard lock(error_mutex);
          worker_error = Tr("No se puede escribir el pack en el disco.");
          break;
        }
        if (written == blk.size) {
          ok_block = true;
          break;
        }
        bytes_done -= written;  // el bloque se repite entero
        if (g_cancel.load()) break;
        REXLOG_WARN("[texturas] descarga: bloque {} ({}) intento {} falló: {}", i, blk.part->url, attempt + 1,
                    get_error.empty() ? std::string("incompleto") : get_error);
        for (int sec = 0; sec < std::min(30, 2 << std::min(attempt, 4)) && !g_cancel.load(); ++sec) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
      }
      if (!ok_block) {
        if (!g_cancel.load() && !failed.load()) {
          failed = true;
          std::lock_guard lock(error_mutex);
          worker_error = Tr("La descarga no se pudo completar. Vuelve a intentarlo: continuará donde se quedó.");
        }
        break;
      }
      block_done[i] = 1;
      std::lock_guard lock(map_mutex);
      map_out << i << '\n';
      map_out.flush();
    }
    CloseHandle(file);
    --workers_left;
  };
  std::vector<std::thread> pool;
  workers_left = connections;
  for (int c = 0; c < connections; ++c) pool.emplace_back(worker);
  {
    auto last = clock::now();
    uint64_t last_bytes = bytes_done.load();
    while (workers_left.load() > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      const auto now = clock::now();
      const double dt = std::chrono::duration<double>(now - last).count();
      if (dt >= 0.5) {
        const uint64_t cur = bytes_done.load();
        SetInfo([&](TextureDownloadInfo& i) {
          i.done = std::min(cur, m.size);
          i.speed = double(cur - last_bytes) / dt;
        });
        last = now;
        last_bytes = cur;
      }
    }
  }
  for (std::thread& t : pool) t.join();
  map_out.close();
  if (g_cancel.load()) {
    SetInfo([](TextureDownloadInfo& i) { i = TextureDownloadInfo{}; });
    return;
  }
  if (failed.load() || bytes_done.load() < m.size) {
    Fail(worker_error.empty() ? Tr("La descarga no se pudo completar. Vuelve a intentarlo: continuará donde se quedó.")
                              : worker_error);
    return;
  }

  // 4. SHA-256 del fichero completo.
  SetInfo([&](TextureDownloadInfo& i) {
    i.state = TextureDownloadState::kVerifying;
    i.done = 0;
    i.total = m.size;
    i.speed = 0.0;
  });
  {
    Sha256 hash;
    std::ifstream in(part, std::ios::binary);
    std::vector<char> buf(4 << 20);
    uint64_t done = 0;
    auto last = clock::now();
    while (in && !g_cancel.load()) {
      in.read(buf.data(), std::streamsize(buf.size()));
      const std::streamsize got = in.gcount();
      if (got <= 0) break;
      hash.Update(buf.data(), size_t(got));
      done += uint64_t(got);
      if (clock::now() - last >= std::chrono::milliseconds(500)) {
        last = clock::now();
        SetInfo([&](TextureDownloadInfo& i) { i.done = done; });
      }
    }
    if (g_cancel.load()) {
      SetInfo([](TextureDownloadInfo& i) { i = TextureDownloadInfo{}; });
      return;
    }
    uint8_t digest[32];
    if (!hash.Final(digest) || HexOf(digest, 32) != m.sha256) {
      std::filesystem::remove(part, ec);
      Fail(Tr("El pack descargado no coincide con el original (se ha borrado). Vuelve a intentarlo."));
      return;
    }
  }

  // 5. Definitivo.
  std::filesystem::remove(dest, ec);
  std::filesystem::rename(part, dest, ec);
  std::filesystem::remove(map_path, ec);
  if (ec) {
    Fail(Tr("No se pudo guardar el pack descargado."));
    return;
  }
  SetInfo([&](TextureDownloadInfo& i) {
    i.state = TextureDownloadState::kDone;
    i.done = i.total = m.size;
    i.message.clear();
  });
  REXLOG_INFO("[texturas] pack HD descargado y verificado: {}", dest.string());
  ReloadTexturePack();  // el plugin lo abre sin reiniciar
}

void Run() {
  g_running.store(true);
  Worker();
  g_running.store(false);
}

}  // namespace

std::filesystem::path TexturePackPath() {
  std::string v = rex::cvar::GetFlagByName("odisea_texture_pack_file");
  if (v.empty()) v = "textures.lopack";
  std::filesystem::path p(std::u8string(v.begin(), v.end()));
  if (p.is_relative()) p = rex::filesystem::GetExecutableFolder() / p;
  return p;
}

bool TexturePackInstalled() {
  std::error_code ec;
  const std::filesystem::path p = TexturePackPath();
  if (!std::filesystem::is_regular_file(p, ec) || std::filesystem::file_size(p, ec) < 4096) return false;
  char magic[4] = {};
  std::ifstream f(p, std::ios::binary);
  f.read(magic, 4);
  return f && std::memcmp(magic, "LOPK", 4) == 0;
}

void TextureDownloadStart() {
  if (g_running.load() || TexturePackInstalled()) return;
  if (g_thread.joinable()) g_thread.join();
  g_cancel.store(false);
  g_running.store(true);
  g_thread = std::thread(Run);
}

void TextureDownloadMarkPending() {
  std::error_code ec;
  const std::filesystem::path part = std::filesystem::path(TexturePackPath()).concat(".part");
  if (TexturePackInstalled() || std::filesystem::exists(part, ec)) return;
  std::ofstream(part, std::ios::binary | std::ios::app);  // marca vacia: el siguiente arranque la retoma
}

void TextureDownloadResumeIfPartial() {
  std::error_code ec;
  const std::filesystem::path part = std::filesystem::path(TexturePackPath()).concat(".part");
  if (!TexturePackInstalled() && std::filesystem::is_regular_file(part, ec)) TextureDownloadStart();
}

void TextureDownloadCancel() {
  g_cancel.store(true);
}

TextureDownloadInfo TextureDownloadStatus() {
  std::lock_guard lock(g_mutex);
  return g_info;
}

bool TextureDownloadActive() {
  const TextureDownloadState s = TextureDownloadStatus().state;
  return g_running.load() &&
         (s == TextureDownloadState::kManifest || s == TextureDownloadState::kDownloading ||
          s == TextureDownloadState::kVerifying);
}

std::string TextureDownloadText() {
  const TextureDownloadInfo i = TextureDownloadStatus();
  char buf[160];
  const int pct = i.total ? int(i.done * 100 / i.total) : 0;
  switch (i.state) {
    case TextureDownloadState::kManifest:
      return Tr("Buscando el pack de texturas HD…");
    case TextureDownloadState::kDownloading:
      std::snprintf(buf, sizeof(buf), Tr("Descargando texturas HD: %d %% (%.1f MB/s)"), pct, i.speed / 1e6);
      return buf;
    case TextureDownloadState::kVerifying:
      std::snprintf(buf, sizeof(buf), Tr("Comprobando el pack de texturas HD: %d %%"), pct);
      return buf;
    case TextureDownloadState::kDone:
      return Tr("Texturas HD instaladas.");
    case TextureDownloadState::kError:
      return i.message;
    default:
      return {};
  }
}

}  // namespace lo
