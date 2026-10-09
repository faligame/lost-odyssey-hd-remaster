// lostodyssey - ReXGlue Recompiled Project
//
// Auto-actualizacion. Ver lo_update.h y docs/ACTUALIZACIONES.md.
#include "lo_update.h"

#include <functional>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "lo_hash.h"
#include "lo_http.h"

#include <commctrl.h>
#include "lo_i18n.h"
#include "lo_options.h"
#include "lo_paths.h"
#include "lo_update_key.h"
#include "lo_version.h"

REXCVAR_DEFINE_BOOL(lo_update_check, true, "LostOdyssey/Actualizaciones",
                    "Buscar actualizaciones del juego al arrancar (como mucho cada 6 horas)");
REXCVAR_DEFINE_STRING(lo_update_skip, "", "LostOdyssey/Actualizaciones",
                      "Version que el jugador ha elegido no instalar (no se vuelve a ofrecer)");
REXCVAR_DEFINE_INT64(lo_update_last_check, 0, "LostOdyssey/Actualizaciones",
                     "Instante (segundos Unix) de la ultima comprobacion sin novedades");
REXCVAR_DEFINE_STRING(lo_update_base_url, LO_UPDATE_BASE_URL, "LostOdyssey/Actualizaciones",
                      "Carpeta con latest.json y latest.json.sig (solo para pruebas; la firma sigue siendo obligatoria)");

namespace lo {

namespace {

using namespace lo::http;

struct Update {
  std::string version, notes, url, sha256;
  uint64_t size = 0;
};

std::atomic<int> g_check_state{0};  // 0 sin lanzar, 1 en curso, 2 terminada
Update g_found;
bool g_has_update = false;
std::mutex g_mutex;

int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::filesystem::path UpdateDir() { return ExeDir() / "update"; }

bool GetText(const std::string& url, std::string& out, std::string& error) {
  Http http(8000, 15000);
  int status = 0;
  uint64_t total = 0;
  out.clear();
  const bool ok = http.Get(
      url, 0,
      [&](const uint8_t* data, size_t size) {
        out.append(reinterpret_cast<const char*>(data), size);
        return out.size() < (1u << 20);  // el manifiesto es diminuto
      },
      status, total, error);
  return ok && status == 200;
}

// ECDSA P-256 / SHA-256: signature_hex = r||s (64 bytes) en hexadecimal.
bool VerifySignature(const std::string& data, const std::string& signature_hex) {
  std::string hex;
  for (char c : signature_hex) {
    if (!std::isspace(static_cast<unsigned char>(c))) hex += c;
  }
  if (hex.size() != 128) return false;
  uint8_t sig[64];
  for (int i = 0; i < 64; ++i) {
    char pair[3] = {hex[size_t(i) * 2], hex[size_t(i) * 2 + 1], 0};
    char* end = nullptr;
    sig[i] = uint8_t(std::strtoul(pair, &end, 16));
    if (end != pair + 2) return false;
  }
  uint8_t digest[32];
  if (!Sha256Of(data.data(), data.size(), digest)) return false;

  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0) return false;
  uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
  auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
  header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
  header->cbKey = 32;
  std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), kUpdateKeyX, 32);
  std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB) + 32, kUpdateKeyY, 32);
  BCRYPT_KEY_HANDLE key = nullptr;
  bool ok = BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob, sizeof(blob), 0) >= 0 &&
            BCryptVerifySignature(key, nullptr, digest, 32, sig, 64, 0) >= 0;
  if (key) BCryptDestroyKey(key);
  BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

// Descarga y comprueba latest.json. true si hay un manifiesto valido y firmado (aunque no sea mas nuevo).
bool FetchLatest(Update& out, std::string& error) {
  std::string base = REXCVAR_GET(lo_update_base_url);
  if (!base.empty() && base.back() != '/') base += '/';
  std::string json, sig;
  if (!GetText(base + "latest.json", json, error) || !GetText(base + "latest.json.sig", sig, error)) return false;
  if (!VerifySignature(json, sig)) {
    error = "firma no válida";
    return false;
  }
  if (JsonNumber(json, "schema") != 1) {
    error = "formato desconocido";
    return false;
  }
  out.version = JsonString(json, "version");
  out.url = JsonString(json, "url");
  out.sha256 = JsonString(json, "sha256");
  out.size = JsonNumber(json, "size");
  out.notes = JsonString(json, GameLanguage() == Lang::kSpanish ? "notes_es" : "notes_en");
  if (out.notes.empty()) out.notes = JsonString(json, "notes_en");
  if (out.version.empty() || out.url.empty() || out.sha256.size() != 64 || out.size == 0) {
    error = "manifiesto incompleto";
    return false;
  }
  return true;
}

// --- ventana de progreso --------------------------------------------------------------------------

struct Progress {
  std::atomic<uint64_t> done{0}, total{0};
  std::atomic<bool> cancel{false}, finished{false};
  std::wstring text;
};

LRESULT CALLBACK ProgressProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  auto* p = reinterpret_cast<Progress*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  switch (m) {
    case WM_CREATE:
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
      return 0;
    case WM_COMMAND:
      if (LOWORD(w) == IDCANCEL && p) p->cancel = true;
      return 0;
    case WM_CLOSE:
      if (p) p->cancel = true;
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

// Corre `work` en un hilo mientras muestra una ventana con barra; devuelve lo que devuelva work.
bool RunWithProgress(const std::wstring& title, const std::wstring& label, Progress& prog,
                     const std::function<bool()>& work) {
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);
  WNDCLASSW wc{};
  wc.lpfnWndProc = ProgressProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"LoUpdateProgress";
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  RegisterClassW(&wc);
  const int w = 460, h = 150;
  HWND win = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, title.c_str(), WS_CAPTION | WS_VISIBLE,
                             (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                             nullptr, nullptr, wc.hInstance, &prog);
  HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  HWND text = CreateWindowExW(0, L"STATIC", label.c_str(), WS_CHILD | WS_VISIBLE, 16, 12, w - 48, 20, win, nullptr,
                              wc.hInstance, nullptr);
  HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 16, 40, w - 48, 20, win, nullptr,
                             wc.hInstance, nullptr);
  HWND cancel = CreateWindowExW(0, L"BUTTON", Wide(Tr("Cancelar")).c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                w - 130, 72, 100, 26, win, reinterpret_cast<HMENU>(IDCANCEL), wc.hInstance, nullptr);
  for (HWND c : {text, cancel}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  SendMessageW(bar, PBM_SETRANGE32, 0, 1000);

  bool result = false;
  std::thread worker([&] {
    result = work();
    prog.finished = true;
  });
  while (!prog.finished) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    const uint64_t total = prog.total.load();
    if (total) SendMessageW(bar, PBM_SETPOS, WPARAM(prog.done.load() * 1000 / total), 0);
    wchar_t buf[160];
    swprintf(buf, 160, L"%ls  %.1f / %.1f MB", label.c_str(), double(prog.done.load()) / 1048576.0,
             double(total) / 1048576.0);
    SetWindowTextW(text, total ? buf : label.c_str());
    Sleep(50);
  }
  worker.join();
  DestroyWindow(win);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return result;
}

// --- aplicar -----------------------------------------------------------------------------------

bool Download(const Update& u, const std::filesystem::path& zip, Progress& prog, std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(zip.parent_path(), ec);
  std::ofstream out(zip, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "no se puede escribir en la carpeta update";
    return false;
  }
  Sha256 hash;
  uint64_t written = 0;
  prog.total = u.size;
  Http http(15000, 60000);
  int status = 0;
  uint64_t total = 0;
  const bool ok = http.Get(
      u.url, 0,
      [&](const uint8_t* data, size_t size) {
        if (prog.cancel) return false;
        out.write(reinterpret_cast<const char*>(data), std::streamsize(size));
        hash.Update(data, size);
        written += size;
        prog.done = written;
        return written <= u.size;  // mas bytes de los anunciados: se corta
      },
      status, total, error);
  out.close();
  if (prog.cancel) {
    error = "cancelado";
    return false;
  }
  if (!ok) return false;
  uint8_t digest[32];
  if (written != u.size || !hash.Final(digest) || HexOf(digest, 32) != u.sha256) {
    error = "la descarga no coincide con el manifiesto (tamaño o SHA-256)";
    return false;
  }
  return true;
}

bool Extract(const std::filesystem::path& zip, const std::filesystem::path& dest, std::string& error) {
  std::error_code ec;
  std::filesystem::remove_all(dest, ec);
  std::filesystem::create_directories(dest, ec);
  wchar_t sys[MAX_PATH] = {};
  GetSystemDirectoryW(sys, MAX_PATH);
  const std::wstring tar = std::wstring(sys) + L"\\tar.exe";
  std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + dest.wstring() + L"\"";
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    error = "no se encuentra tar.exe (Windows 10 o posterior)";
    return false;
  }
  WaitForSingleObject(pi.hProcess, 120000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0) {
    error = "no se pudo descomprimir el paquete";
    return false;
  }
  return true;
}

// El zip puede traer una carpeta raiz (LostOdysseyHD-x.y.z-win64\...): se baja hasta encontrar el exe.
std::filesystem::path PackageRoot(const std::filesystem::path& staging) {
  std::error_code ec;
  if (std::filesystem::exists(staging / "lostodyssey.exe", ec)) return staging;
  for (const auto& e : std::filesystem::directory_iterator(staging, ec)) {
    if (e.is_directory(ec) && std::filesystem::exists(e.path() / "lostodyssey.exe", ec)) return e.path();
  }
  return {};
}

bool LaunchUpdater(const std::filesystem::path& root, std::string& error) {
  std::error_code ec;
  const std::filesystem::path src = root / "updater.exe";
  const std::filesystem::path run = UpdateDir() / "updater.exe";
  // Se ejecuta una copia fuera de la carpeta del juego: asi puede reemplazar el updater.exe del juego.
  if (!std::filesystem::exists(src, ec) ||
      !std::filesystem::copy_file(src, run, std::filesystem::copy_options::overwrite_existing, ec)) {
    error = "el paquete no trae updater.exe";
    return false;
  }
  const std::wstring cmd = L"\"" + run.wstring() + L"\" --pid " + std::to_wstring(GetCurrentProcessId()) +
                           L" --source \"" + root.wstring() + L"\" --target \"" + ExeDir().wstring() + L"\"";
  std::wstring mutable_cmd = cmd;
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(run.c_str(), mutable_cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                      ExeDir().c_str(), &si, &pi)) {
    error = "no se pudo lanzar el actualizador";
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

void ShowError(const std::string& what) {
  const std::wstring text = Wide(std::string(Tr("No se pudo actualizar el juego: ")) + what +
                                 Tr("\n\nPuedes seguir jugando con la versión actual; se volverá a intentar más adelante."));
  MessageBoxW(nullptr, text.c_str(), L"Lost Odyssey HD Remaster", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
}

// Descarga, extrae y lanza el updater. true = el juego debe cerrarse.
bool DownloadAndApply(const Update& u) {
  const std::filesystem::path dir = UpdateDir();
  const std::filesystem::path zip = dir / "package.zip";
  const std::filesystem::path staging = dir / "staging";
  Progress prog;
  std::string error;
  const std::wstring title = L"Lost Odyssey HD Remaster";
  bool ok = RunWithProgress(title, Wide(Tr("Descargando la actualización")), prog,
                            [&] { return Download(u, zip, prog, error); });
  if (!ok) {
    if (!prog.cancel) ShowError(error);
    return false;
  }
  Progress prog2;
  ok = RunWithProgress(title, Wide(Tr("Preparando la actualización")), prog2, [&] { return Extract(zip, staging, error); });
  const std::filesystem::path root = ok ? PackageRoot(staging) : std::filesystem::path{};
  if (!ok || root.empty()) {
    ShowError(ok ? "paquete incompleto" : error);
    return false;
  }
  if (!LaunchUpdater(root, error)) {
    ShowError(error);
    return false;
  }
  REXLOG_INFO("[actualizacion] updater lanzado: {} -> {}", LO_VERSION_STRING, u.version);
  return true;
}

// Pregunta y aplica. skip_allowed=false (busqueda manual): no hay "saltar".
bool Offer(const Update& u, const std::filesystem::path& config_path) {
  std::string text = std::string(Tr("Hay una versión nueva de Lost Odyssey HD Remaster: ")) + u.version + " (" +
                     Tr("tienes la ") + LO_VERSION_STRING + ").\n\n";
  if (!u.notes.empty()) text += u.notes + "\n\n";
  text += Tr("Sí = actualizar ahora (se descarga y el juego se reinicia).\nNo = más tarde.\nCancelar = no volver a avisar de esta versión.");
  const int r = MessageBoxW(nullptr, Wide(text).c_str(), L"Lost Odyssey HD Remaster",
                            MB_YESNOCANCEL | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
  if (r == IDCANCEL) {
    rex::cvar::SetFlagByName("lo_update_skip", u.version);
    SetTomlValue(config_path, "lo_update_skip", "\"" + u.version + "\"");
    return false;
  }
  if (r != IDYES) return false;
  return DownloadAndApply(u);
}

}  // namespace

bool VersionNewer(const char* a, const char* b) {
  int x[3] = {0, 0, 0}, y[3] = {0, 0, 0};
  std::sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
  std::sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
  for (int i = 0; i < 3; ++i) {
    if (x[i] != y[i]) return x[i] > y[i];
  }
  return false;
}

void UpdateCleanupLeftovers() {
  std::error_code ec;
  for (const char* name : {"staging", "backup", "package.zip"}) std::filesystem::remove_all(UpdateDir() / name, ec);
}

void UpdateCheckStart() {
#ifdef LO_DEV
  return;  // la version de desarrollo nunca se actualiza sola
#else
  if (!REXCVAR_GET(lo_update_check) || g_check_state != 0) return;
  if (NowSeconds() - REXCVAR_GET(lo_update_last_check) < 6 * 3600) return;
  g_check_state = 1;
  std::thread([] {
    Update u;
    std::string error;
    if (FetchLatest(u, error)) {
      if (VersionNewer(u.version.c_str(), LO_VERSION_STRING) && u.version != REXCVAR_GET(lo_update_skip)) {
        std::lock_guard lock(g_mutex);
        g_found = u;
        g_has_update = true;
      }
    } else {
      REXLOG_INFO("[actualizacion] sin comprobar: {}", error);
    }
    g_check_state = 2;
  }).detach();
#endif
}

bool UpdateOfferAndApply(const std::filesystem::path& config_path) {
  if (g_check_state == 0) return false;
  for (int i = 0; i < 60 && g_check_state != 2; ++i) Sleep(50);  // hasta 3 s; si no llega, ya sera la proxima vez
  if (g_check_state != 2) return false;
  Update u;
  {
    std::lock_guard lock(g_mutex);
    if (!g_has_update) {
      const int64_t now = NowSeconds();
      rex::cvar::SetFlagByName("lo_update_last_check", std::to_string(now));
      SetTomlValue(config_path, "lo_update_last_check", std::to_string(now));
      return false;
    }
    u = g_found;
  }
  return Offer(u, config_path);
}

bool UpdateCheckNow(const std::filesystem::path& config_path) {
  Update u;
  std::string error;
  if (!FetchLatest(u, error)) {
    ShowError(error);
    return false;
  }
  if (!VersionNewer(u.version.c_str(), LO_VERSION_STRING)) {
    MessageBoxW(nullptr, Wide(Tr("Ya tienes la última versión.")).c_str(), L"Lost Odyssey HD Remaster",
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    return false;
  }
  return Offer(u, config_path);
}

}  // namespace lo
