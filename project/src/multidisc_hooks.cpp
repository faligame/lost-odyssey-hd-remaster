// lostodyssey - ReXGlue Recompiled Project
//
// Multidisco. Lost Odyssey viene en 4 discos; cada uno trae su propio
// default.xex (misma imagen, cabecera con "disco N de 4") y su LO.fpi, y cambian
// los archivos de eventos, campo, videos y sonido (xenon_event/field/mov/snd).
// Cada disco puede estar extraido en una carpeta, como ISO o como paquete GOD,
// y se lee en su sitio (disc_sources.h).
//
// Cuando hace falta otro disco el juego llama a XamSwapDisc(disco, evento,
// mensaje) desde un unico sitio, sub_82B88020, y espera al evento; despues
// vuelve a leer game:\LO.FPI para comprobar que el disco es el correcto. En el
// SDK XamSwapDisc es un stub que no hace nada, asi que se envuelve sub_82B88020
// (patron weak alias) y, tras la llamada original:
//   1. se busca el disco pedido en el catalogo (juego y numero de disco segun la
//      cabecera de su default.xex); si no esta, se vuelve a buscar;
//   2. se reapuntan los enlaces game: y d: del sistema de ficheros virtual a un
//      dispositivo sobre ese disco (los anteriores se conservan: puede haber
//      ficheros abiertos);
//   3. se senala el evento para que el juego siga.
// Si el disco no esta, se avisa con un cuadro de dialogo (como la consola, que
// espera a que se inserte) y se puede reintentar tras anadirlo.
//
// Arranque desde ISO/GOD: el SDK exige una carpeta con default.xex, asi que se
// copia solo ese fichero a la cache y, con el runtime ya construido, se monta
// la imagen del disco en game: y d: antes de que empiece el juego.

#include "lo_i18n.h"
#include "multidisc.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/xchar.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/device.h>
#include <rex/filesystem/vfs.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

#include "lo_options.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif

REXCVAR_DEFINE_STRING(lo_discs, "", "LostOdyssey/Discos",
                      "Discos del juego: carpetas extraidas, default.xex, .iso o paquetes GOD, separados "
                      "por ';' (ademas se buscan junto a la carpeta de datos y junto al exe)");
REXCVAR_DEFINE_INT32(lo_start_disc, 0, "LostOdyssey/Discos",
                     "Depuracion: disco que se monta al arrancar (0 = el de arranque)");

REX_EXTERN(__imp__sub_82B88020);

namespace {

#ifdef _WIN32
// UTF-8 -> UTF-16 para los cuadros de dialogo de Windows.
std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}
#endif

// Dispositivo que el SDK monta sobre game_data_root (Runtime::SetupVfs).
constexpr const char* kBootDevice = "\\Device\\Harddisk0\\Partition1";
// Niveles de busqueda: <raiz>\<carpeta GOD>\<titulo>\00007000\<cabecera>.
constexpr int kScanDepth = 4;

std::mutex g_mutex;
rex::filesystem::VirtualFileSystem* g_fs = nullptr;
std::filesystem::path g_data_hint;  // game_data_root con el que se arranco
std::vector<lo::DiscSource> g_catalog;
std::optional<lo::DiscSource> g_boot;
bool g_boot_is_image = false;  // arranque desde ISO/GOD: el SDK solo ve su default.xex
int g_mounted = 0;
std::map<int, std::string> g_devices;  // disco -> dispositivo registrado por nosotros

std::string PathUtf8(const std::filesystem::path& p) {
  const auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

std::vector<std::filesystem::path> ExplicitPaths() {
  std::vector<std::filesystem::path> out;
  const std::string list = REXCVAR_GET(lo_discs);
  for (size_t start = 0; start <= list.size();) {
    size_t end = list.find(';', start);
    if (end == std::string::npos) end = list.size();
    std::string item = list.substr(start, end - start);
    item.erase(0, item.find_first_not_of(" \t\""));
    item.erase(item.find_last_not_of(" \t\"") + 1);
    if (!item.empty()) out.emplace_back(std::u8string(item.begin(), item.end()));
    start = end + 1;
  }
  return out;
}

// Orden de preferencia: lo_discs, la carpeta de datos, y lo encontrado junto a
// ellas y junto al exe (carpetas antes que ISO y GOD).
void RefreshCatalog() {
  std::vector<lo::DiscSource> catalog;
  std::vector<std::filesystem::path> seen;
  auto add = [&](const lo::DiscSource& s) {
    if (s.title_id != lo::kLostOdysseyTitleId) return;
    for (const auto& p : seen) {
      if (p == s.path) return;
    }
    seen.push_back(s.path);
    catalog.push_back(s);
  };
  std::vector<std::filesystem::path> roots;
  for (const auto& p : ExplicitPaths()) {
    if (const auto s = lo::IdentifyDiscSource(p)) {
      add(*s);
      roots.push_back(s->path.parent_path());
    } else {
      for (const auto& found : lo::ScanDiscSources({p}, kScanDepth)) add(found);
    }
  }
  if (!g_data_hint.empty()) {
    if (const auto s = lo::IdentifyDiscSource(g_data_hint)) {
      add(*s);
      roots.push_back(s->path.parent_path());
    } else {
      // Estructura de distribucion: data\disc1 ... data\disc4 junto al exe.
      for (const auto& found : lo::ScanDiscSources({g_data_hint}, kScanDepth)) add(found);
    }
  }
  const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
  roots.push_back(exe_dir);
  roots.push_back(exe_dir.parent_path());
  for (const auto& found : lo::ScanDiscSources(roots, kScanDepth)) add(found);
  g_catalog = std::move(catalog);
}

const lo::DiscSource* FindSource(int number) {
  // El disco de arranque no cambia de fuente a mitad de partida.
  if (g_boot && g_boot->number == number) return &*g_boot;
  for (const auto& s : g_catalog) {
    if (s.number == number) return &s;
  }
  return nullptr;
}

std::string CatalogSummary() {
  if (!g_boot) return {};
  std::string out;
  for (int n = 1; n <= g_boot->count; ++n) {
    const lo::DiscSource* s = FindSource(n);
    out += s ? fmt::format(" disco {}: {} {};", n, lo::DiscSourceKindName(s->kind), PathUtf8(s->path))
             : fmt::format(" disco {}: no encontrado;", n);
  }
  return out;
}

bool MountDisc(int number) {
  auto* fs = g_fs ? g_fs : REX_KERNEL_FS();
  const lo::DiscSource* source = FindSource(number);
  if (!fs || !source) return false;
  std::string device;
  if (g_boot && number == g_boot->number && !g_boot_is_image) {
    device = kBootDevice;
  } else if (const auto it = g_devices.find(number); it != g_devices.end()) {
    device = it->second;
  } else {
    device = fmt::format("\\Device\\LoDisc{}", number);
    auto dev = lo::CreateDiscDevice(*source, device, !REXCVAR_GET(allow_game_relative_writes));
    if (!dev || !fs->RegisterDevice(std::move(dev))) {
      REXLOG_ERROR("[multidisco] no se pudo montar {} como disco {}", PathUtf8(source->path), number);
      return false;
    }
    g_devices[number] = device;
  }
  fs->UnregisterSymbolicLink("game:");
  fs->UnregisterSymbolicLink("d:");
  fs->RegisterSymbolicLink("game:", device);
  fs->RegisterSymbolicLink("d:", device);
  REXLOG_INFO("[multidisco] disco {} montado en game: y d: ({} {})", number,
              lo::DiscSourceKindName(source->kind), PathUtf8(source->path));
  g_mounted = number;
  return true;
}

void HandleSwapDisc(int number) {
  std::lock_guard lock(g_mutex);
  REXLOG_INFO("[multidisco] el juego pide el disco {} (montado: {})", number, g_mounted);
  if (!g_boot || number == g_mounted) return;
  if (number < 1 || number > g_boot->count) {
    REXLOG_WARN("[multidisco] numero de disco no valido: {}", number);
    return;
  }
  for (;;) {
    if (MountDisc(number)) return;
    RefreshCatalog();  // quiza se ha anadido despues de arrancar
    if (MountDisc(number)) return;
#ifdef _WIN32
    const std::wstring text = Wide(fmt::format(
        fmt::runtime(lo::Tr("Lost Odyssey necesita el disco {0}.\n\nPon ese disco (carpeta extraída, .iso o "
                            "paquete GOD) junto a los demás discos o junto al ejecutable y pulsa "
                            "Reintentar.\n\nTambién puedes indicar su ruta en config.toml con lo_discs.")),
        number));
    const std::wstring title = Wide(lo::Tr("Lost Odyssey - Cambio de disco"));
    if (MessageBoxW(nullptr, text.c_str(), title.c_str(),
                    MB_RETRYCANCEL | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND) == IDRETRY) {
      continue;
    }
#endif
    REXLOG_WARN("[multidisco] disco {} no encontrado; sigue montado el disco {}", number, g_mounted);
    return;
  }
}

std::string TomlString(const std::string& s) {
  if (s.find('\'') == std::string::npos && s.find('\n') == std::string::npos) return "'" + s + "'";
  std::string out = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
}

}  // namespace

REX_EXTERN(sub_82B88020) {
  const int disc = int(ctx.r3.u32 & 0xFF);
  const uint32_t event_handle = ctx.r4.u32;
  // La original llama al aviso del juego y a XamSwapDisc (stub del SDK, devuelve
  // exito sin tocar el evento); se conserva su resultado en r3.
  __imp__sub_82B88020(ctx, base);
  HandleSwapDisc(disc);
  if (event_handle) {
    rex::kernel::xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  }
}

namespace lo {

std::optional<std::filesystem::path> MultiDiscResolveBoot(const std::filesystem::path& game_data_root,
                                                          const std::filesystem::path& cache_root) {
  std::lock_guard lock(g_mutex);
  g_data_hint = game_data_root;
  RefreshCatalog();
  std::optional<DiscSource> boot;
  if (!game_data_root.empty()) {
    boot = IdentifyDiscSource(game_data_root);
    if (boot && boot->title_id != kLostOdysseyTitleId) boot.reset();
  }
  if (!boot) {
    for (const auto& s : g_catalog) {
      if (s.number == 1) {
        boot = s;
        break;
      }
    }
  }
  if (!boot) {
    REXLOG_WARN("[multidisco] no se encuentra el disco 1 (lo_discs, carpeta de datos, junto al exe)");
    return std::nullopt;
  }
  g_boot = boot;
  g_mounted = boot->number;
  REXLOG_INFO("[multidisco] arranque con el disco {} de {} ({} {}).{}", boot->number, boot->count,
              DiscSourceKindName(boot->kind), PathUtf8(boot->path), CatalogSummary());
  // Una carpeta con data\\common al lado no esta completa: el SDK solo ve default.xex y el disco se monta aparte.
  if (boot->kind == DiscSourceKind::kFolder && boot->common.empty()) {
    g_boot_is_image = false;
    return boot->path;
  }
  g_boot_is_image = true;
  std::filesystem::path dir = (cache_root.empty() ? std::filesystem::temp_directory_path() : cache_root) /
                              "boot_xex" / fmt::format("{:08X}", boot->media_id);
  if (!ExtractDiscFile(*boot, "default.xex", dir / "default.xex")) {
    REXLOG_ERROR("[multidisco] no se pudo copiar el default.xex de {} a {}", PathUtf8(boot->path),
                 PathUtf8(dir));
    return std::nullopt;
  }
  return dir;
}

std::optional<std::filesystem::path> MultiDiscPickSource() {
#ifdef _WIN32
  for (;;) {
    std::wstring buffer(4096, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    std::wstring filter = Wide(lo::Tr("Disco de Lost Odyssey (default.xex, .iso, paquete GOD)"));
    filter += std::wstring(L"\0*.xex;*.iso;*.\0", 16);
    filter += Wide(lo::Tr("Todos los archivos"));
    filter += std::wstring(L"\0*.*\0\0", 7);
    const std::wstring dialog_title =
        Wide(lo::Tr("Lost Odyssey: elige el disco 1 (default.xex, imagen ISO o paquete GOD)"));
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = DWORD(buffer.size());
    ofn.lpstrTitle = dialog_title.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    const std::filesystem::path picked(buffer.c_str());
    const auto source = IdentifyDiscSource(picked);
    if (source && source->title_id == kLostOdysseyTitleId) return picked;
    const std::wstring wrong = Wide(lo::Tr(
        "Eso no es un disco de Lost Odyssey.\n\nElige su default.xex, la imagen .iso o la cabecera del paquete "
        "GOD (el fichero sin extensión que está junto a la carpeta .data)."));
    MessageBoxW(nullptr, wrong.c_str(), L"Lost Odyssey", MB_OK | MB_ICONWARNING);
  }
#else
  return std::nullopt;
#endif
}

void MultiDiscRememberPath(const std::filesystem::path& config_path, const std::filesystem::path& path) {
  const std::string entry = PathUtf8(path);
  std::string list = REXCVAR_GET(lo_discs);
  for (const auto& p : ExplicitPaths()) {
    if (p == path) return;
  }
  list = list.empty() ? entry : list + ";" + entry;
  rex::cvar::SetFlagByName("lo_discs", list);
  SetTomlValue(config_path, "lo_discs", TomlString(list));
}

void MultiDiscAttach(rex::filesystem::VirtualFileSystem* fs) {
  std::lock_guard lock(g_mutex);
  g_fs = fs;
  if (g_boot && g_boot_is_image && !MountDisc(g_boot->number)) {
    REXLOG_ERROR("[multidisco] no se pudo montar el disco de arranque {}", PathUtf8(g_boot->path));
  }
}

void MultiDiscApplyStartDisc() {
  std::lock_guard lock(g_mutex);
  const int start = REXCVAR_GET(lo_start_disc);
  if (start <= 0 || !g_boot) return;
  REXLOG_INFO("[multidisco] lo_start_disc = {}", start);
  if (start != g_mounted && !MountDisc(start)) {
    REXLOG_WARN("[multidisco] lo_start_disc = {}, pero ese disco no esta", start);
  }
}

int MultiDiscMountedNumber() {
  std::lock_guard lock(g_mutex);
  return g_mounted;
}

std::optional<DiscSource> MultiDiscBootSource() {
  std::lock_guard lock(g_mutex);
  return g_boot;
}

std::vector<DiscSource> MultiDiscAllSources() {
  std::lock_guard lock(g_mutex);
  RefreshCatalog();
  std::vector<DiscSource> out;
  if (!g_boot) return out;
  for (int n = 1; n <= g_boot->count; ++n) {
    if (const lo::DiscSource* s = FindSource(n)) out.push_back(*s);
  }
  return out;
}

}  // namespace lo
