// lostodyssey - ReXGlue Recompiled Project
// Ver installer.h.
#include "installer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "lo_hash.h"
#include "lo_i18n.h"
#include "lo_options.h"
#include "multidisc.h"
#include "texture_download.h"

REXCVAR_DEFINE_INT32(lo_setup_step, 0, "LostOdyssey/Instalacion",
                     "Paso con el que empieza el asistente (0 = bienvenida; solo para pruebas)");
REXCVAR_DEFINE_BOOL(lo_setup, false, "LostOdyssey/Instalacion",
                    "Abre el asistente de instalacion aunque ya haya discos (--lo_setup; como --setup de Unleashed)");

namespace lo {

namespace {

struct EditionDisc {
  int number;
  uint32_t media_id;
  const char* xex_sha256;
};

// USA/Europa v3 (la unica edicion admitida en la v0.0.1). Valores de la auditoria de
// freefrank/LostOdysseyRecomp (docs/notes/europe-support.md), comprobados con nuestros discos.
constexpr EditionDisc kUsaEurope[] = {
    {1, 0x368DE6DD, "175ae53d109d480a83bebbd186e7b6871f7b03ce80af69ab388db2f747640de3"},
    {2, 0x1888BE4E, "1d8a78379349e4583957d34148d5dbf8a091955edb6c6877bc24bdc9c7b87541"},
    {3, 0x6DD59D08, "dd323967d7f4b99b48c00aa6a15a643c96df525e539669e9be49a876513b86f6"},
    {4, 0x0C0E80B5, "9204ba8b91836853ae1e9f0dc49090abd5e63709935599c5551c23b28ecf48d4"},
};

std::string PathUtf8(const std::filesystem::path& p) {
  const auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

// Identifica y comprueba un disco. number_out = numero de disco segun su cabecera (0 si no es de
// Lost Odyssey).
DiscStatus Verify(const std::filesystem::path& path, int& number_out, DiscSource& source_out) {
  number_out = 0;
  const auto source = IdentifyDiscSource(path);
  if (!source || source->title_id != kLostOdysseyTitleId) return DiscStatus::kNotLostOdyssey;
  source_out = *source;
  for (const EditionDisc& e : kUsaEurope) {
    if (e.media_id != source->media_id) continue;
    number_out = e.number;
    auto reader = DiscReader::Open(*source);
    std::vector<uint8_t> xex;
    uint8_t hash[32];
    if (!reader || !reader->ReadFile("default.xex", 0, ~0ull, xex) || !Sha256Of(xex.data(), xex.size(), hash)) {
      return DiscStatus::kModified;
    }
    return HexOf(hash, 32) == e.xex_sha256 ? DiscStatus::kOk : DiscStatus::kModified;
  }
  number_out = std::clamp(source->number, 1, 4);
  return DiscStatus::kWrongEdition;
}

#ifdef _WIN32
std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}
#endif

}  // namespace

Installer::Installer(std::filesystem::path config_path, std::function<void()> on_done,
                     std::function<void()> on_disc1_ready)
    : config_path_(std::move(config_path)),
      on_done_(std::move(on_done)),
      on_disc1_ready_(std::move(on_disc1_ready)) {
  // Busca ya los discos junto al exe (data\, carpeta de arriba): si estan, el asistente toma la fuente del juego
  // desde la primera pantalla.
  auto_scanned_ = true;
  StartScan();
  step_ = InstallStep(std::clamp(REXCVAR_GET(lo_setup_step), 0, int(InstallStep::kFinish)));
}

Installer::~Installer() {
  copy_cancel_.store(true);
  if (copy_thread_.joinable()) copy_thread_.join();
  if (scan_thread_.joinable()) scan_thread_.join();
}

int Installer::DiscsReady() const {
  int n = 0;
  for (const InstallDisc& d : discs_) n += d.status == DiscStatus::kOk;
  return n;
}

bool Installer::CanGoNext() const {
  switch (step_) {
    case InstallStep::kDiscs: return Disc1Ready();
    case InstallStep::kInstall: return !install_running() && (install_mode_ == InstallMode::kInPlace || install_done());
    case InstallStep::kFinish: return false;
    default: return true;
  }
}

void Installer::Next() {
  if (!CanGoNext()) return;
  step_ = InstallStep(int(step_) + 1);
  // Al llegar a los discos se busca una vez lo que haya junto al exe (data\, carpeta de arriba).
  if (step_ == InstallStep::kDiscs && !auto_scanned_) {
    auto_scanned_ = true;
    StartScan();
  }
}

void Installer::Back() {
  if (install_running()) return;
  if (step_ != InstallStep::kWelcome) step_ = InstallStep(int(step_) - 1);
}

void Installer::SetDisc(int number, const std::filesystem::path& path, DiscStatus status, const DiscSource& source) {
  std::lock_guard lock(mutex_);
  InstallDisc& d = discs_[size_t(number - 1)];
  d.path = path;
  d.status = status;
  d.source = source;
}

std::string Installer::AddDiscPath(const std::filesystem::path& path) {
  int number = 0;
  DiscSource source;
  const DiscStatus status = Verify(path, number, source);
  std::string error;
  switch (status) {
    case DiscStatus::kNotLostOdyssey:
      error = Tr("Eso no es un disco de Lost Odyssey. Elige su default.xex, la imagen .iso o el paquete GOD.");
      break;
    case DiscStatus::kWrongEdition:
      error = Tr("Es de otra edición del juego (la asiática aún no está admitida). Hace falta USA/Europa.");
      break;
    case DiscStatus::kModified:
      error = Tr("Los ficheros del disco no son los originales (modificados o dañados).");
      break;
    default: break;
  }
  if (number >= 1 && number <= 4) SetDisc(number, path, status, source);
  if (status == DiscStatus::kOk) {
    MultiDiscRememberPath(config_path_, path);
    if (number == 1 && on_disc1_ready_) on_disc1_ready_();
  }
  message_ = error;
  REXLOG_INFO("[instalador] {} -> disco {} estado {}", PathUtf8(path), number, int(status));
  return error;
}


std::string Installer::install_error() const {
  std::lock_guard lock(copy_text_mutex_);
  return copy_error_;
}

std::string Installer::install_file() const {
  std::lock_guard lock(copy_text_mutex_);
  return copy_file_;
}

double Installer::install_speed() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  const uint64_t bytes = copy_bytes_done_.load();
  if (speed_last_ms_ == 0 || now - speed_last_ms_ >= 1000) {
    if (speed_last_ms_) copy_speed_ = double(bytes - speed_last_bytes_) * 1000.0 / double(now - speed_last_ms_);
    speed_last_ms_ = now;
    speed_last_bytes_ = bytes;
  }
  return copy_speed_;
}

void Installer::StartInstall() {
  if (copy_running_.exchange(true)) return;
  if (copy_thread_.joinable()) copy_thread_.join();
  copy_done_.store(false);
  copy_cancel_.store(false);
  copy_bytes_done_.store(0);
  copy_bytes_total_.store(0);
  speed_last_ms_ = 0;
  copy_speed_ = 0.0;
  {
    std::lock_guard lock(copy_text_mutex_);
    copy_error_.clear();
    copy_file_.clear();
  }
  copy_thread_ = std::thread([this] {
    RunInstall();
    copy_running_.store(false);
  });
}

void Installer::CancelInstall() {
  copy_cancel_.store(true);
}

// Copia los ficheros de cada disco verificado a <exe>\data\disc<N>, comprueba el resultado y deja la
// configuracion sin rutas a los discos originales (lo_discs vacio): el juego encuentra data\ solo.
void Installer::RunInstall() {
  auto fail = [&](const std::string& message) {
    std::lock_guard lock(copy_text_mutex_);
    copy_error_ = message;
  };
  const std::filesystem::path data_dir = rex::filesystem::GetExecutableFolder() / "data";
  struct Job {
    int disc;
    DiscSource source;
    std::vector<DiscFileInfo> files;
    std::filesystem::path dest;
  };
  std::vector<Job> jobs;
  uint64_t total = 0;
  for (int n = 1; n <= 4; ++n) {
    InstallDisc d;
    {
      std::lock_guard lock(mutex_);
      d = discs_[size_t(n - 1)];
    }
    if (d.status != DiscStatus::kOk) continue;
    Job job;
    job.disc = n;
    job.source = d.source;
    job.dest = data_dir / ("disc" + std::to_string(n));
    std::error_code ec;
    if (std::filesystem::equivalent(d.source.path, job.dest, ec)) continue;  // ya esta instalado ahi
    auto reader = DiscReader::Open(d.source);
    if (!reader || !reader->List(job.files)) {
      fail(Tr("No se pudo leer la lista de ficheros del disco.") + std::string(" (") + std::to_string(n) + ")");
      return;
    }
    for (const DiscFileInfo& f : job.files) total += f.size;
    jobs.push_back(std::move(job));
  }
  copy_discs_total_.store(int(jobs.size()));
  copy_bytes_total_.store(total);
  if (jobs.empty()) {
    copy_done_.store(true);
    return;
  }
  {
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    const auto space = std::filesystem::space(data_dir, ec);
    if (!ec && space.available < total + (uint64_t(1) << 30)) {
      fail(Tr("No hay espacio suficiente en el disco para instalar los discos."));
      return;
    }
  }
  int index = 0;
  for (Job& job : jobs) {
    ++index;
    copy_disc_.store(index);
    auto reader = DiscReader::Open(job.source);
    if (!reader) {
      fail(Tr("No se pudo abrir el disco."));
      return;
    }
    for (const DiscFileInfo& f : job.files) {
      if (copy_cancel_.load()) return;
      {
        std::lock_guard lock(copy_text_mutex_);
        copy_file_ = f.path;
      }
      const std::filesystem::path rel(std::u8string(f.path.begin(), f.path.end()));
      // Fichero igual en otros discos: se guarda una sola vez en data\\common (ver docs/DISENO_DATA_COMMON.md).
      const std::filesystem::path common_file = data_dir / "common" / rel;
      std::error_code ec;
      if (reader->SameAs(f.path, common_file, &copy_cancel_)) {  // ya esta en common
        copy_bytes_done_ += f.size;
        continue;
      }
      if (copy_cancel_.load()) return;
      bool moved = false;
      for (int k = 1; k <= 4 && !moved; ++k) {
        if (k == job.disc) continue;
        const std::filesystem::path other = data_dir / ("disc" + std::to_string(k)) / rel;
        if (reader->SameAs(f.path, other, &copy_cancel_)) {
          // Otro disco ya instalado lo tiene igual: pasa a common y este disco no lo copia.
          std::filesystem::create_directories(common_file.parent_path(), ec);
          std::filesystem::remove(common_file, ec);
          std::filesystem::rename(other, common_file, ec);
          moved = !ec;
        }
      }
      if (moved) {
        copy_bytes_done_ += f.size;
        continue;
      }
      if (copy_cancel_.load()) return;
      if (!reader->CopyFileTo(f.path, job.dest / rel, &copy_bytes_done_, &copy_cancel_)) {
        if (!copy_cancel_.load()) fail(Tr("Falló la copia de un fichero del disco: ") + f.path);
        return;
      }
    }
    // Comprobar el disco instalado (mismo default.xex que el original).
    int number = 0;
    DiscSource installed;
    if (Verify(job.dest, number, installed) != DiscStatus::kOk) {
      fail(Tr("El disco instalado no coincide con el original. Vuelve a intentarlo."));
      return;
    }
    {
      std::lock_guard lock(mutex_);
      InstallDisc& d = discs_[size_t(job.disc - 1)];
      d.path = job.dest;
      d.source = installed;
    }
  }
  // Ya no hacen falta los discos originales: fuera sus rutas (el juego busca en data\).
  rex::cvar::SetFlagByName("lo_discs", "");
  SetTomlValue(config_path_, "lo_discs", "''");
  copy_done_.store(true);
  REXLOG_INFO("[instalador] discos instalados en {}", PathUtf8(data_dir));
}

void Installer::BrowseFile() {
#ifdef _WIN32
  std::wstring buffer(4096, L'\0');
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  std::wstring filter = Wide(Tr("Disco de Lost Odyssey (default.xex, .iso, paquete GOD)"));
  filter += std::wstring(L"\0*.xex;*.iso;*.\0", 16);
  filter += Wide(Tr("Todos los archivos"));
  filter += std::wstring(L"\0*.*\0\0", 7);
  const std::wstring title = Wide(Tr("Elige un disco de Lost Odyssey"));
  ofn.lpstrFilter = filter.c_str();
  ofn.lpstrFile = buffer.data();
  ofn.nMaxFile = DWORD(buffer.size());
  ofn.lpstrTitle = title.c_str();
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (GetOpenFileNameW(&ofn)) AddDiscPath(std::filesystem::path(buffer.c_str()));
#endif
}

void Installer::BrowseFolder() {
#ifdef _WIN32
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    const std::wstring title = Wide(Tr("Elige la carpeta del disco (con su default.xex)"));
    dialog->SetTitle(title.c_str());
    if (SUCCEEDED(dialog->Show(nullptr))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
          AddDiscPath(std::filesystem::path(raw));
          CoTaskMemFree(raw);
        }
        item->Release();
      }
    }
    dialog->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
#endif
}

void Installer::StartScan() {
  if (scanning_.exchange(true)) return;
  if (scan_thread_.joinable()) scan_thread_.join();
  scan_thread_ = std::thread([this] {
    const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
    const std::vector<std::filesystem::path> roots = {exe_dir, exe_dir / "data", exe_dir.parent_path()};
    int found = 0;
    const auto sources = ScanDiscSources(roots, 4);
    REXLOG_INFO("[instalador] busqueda de discos: {} candidatos en {} (y data, y la carpeta de arriba)", sources.size(),
                PathUtf8(exe_dir));
    for (const DiscSource& s : sources) {
      if (s.title_id != kLostOdysseyTitleId) continue;
      if (AddDiscPath(s.path).empty()) ++found;
    }
    message_ = found ? std::string() : Tr("No se ha encontrado ningún disco. Añádelos a mano.");
    scanning_.store(false);
  });
}

void Installer::Finish() {
  // Discos: sus rutas ya quedaron en lo_discs (config.toml) al verificarlos.
  // Sombreadores: la eleccion que haria la pantalla de la primera vez.
  const char* value = precache_all_ ? "todo" : "parte";
  rex::cvar::SetFlagByName("lo_shader_precache", value);
  SetTomlValue(config_path_, "lo_shader_precache", std::string("\"") + value + "\"");
  // Texturas HD: la descarga empieza ya y sigue mientras se juega.
  if (texture_choice_ == TextureChoice::kNow && !TexturePackInstalled()) {
    TextureDownloadMarkPending();
    TextureDownloadStart();
  }
  REXLOG_INFO("[instalador] terminado: {} discos, precarga {}", DiscsReady(), value);
  if (on_done_) on_done_();
}

}  // namespace lo
