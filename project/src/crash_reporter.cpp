// lostodyssey - ReXGlue Recompiled Project
// Ver crash_reporter.h.
#include "crash_reporter.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "lo_i18n.h"
#include "lo_system_info.h"
#include "lo_version.h"

namespace lo {

namespace {

#ifdef _WIN32

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

// Quita lo personal: rutas de usuario y la carpeta del juego.
std::string Sanitize(std::string text, const std::string& game_dir) {
  auto replace_all = [&](const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t p = 0; (p = text.find(from, p)) != std::string::npos; p += to.size()) text.replace(p, from.size(), to);
  };
  replace_all(game_dir, "<juego>");
  std::string slashed = game_dir;
  std::replace(slashed.begin(), slashed.end(), '\\', '/');
  replace_all(slashed, "<juego>");
  // <unidad>:\Users\<nombre>  ->  <usuario>
  for (const char* marker : {":\\Users\\", ":/Users/"}) {
    for (size_t p = 0; (p = text.find(marker, p)) != std::string::npos;) {
      const size_t start = p >= 1 ? p - 1 : p;
      size_t end = p + std::strlen(marker);
      while (end < text.size() && text[end] != '\\' && text[end] != '/' && text[end] != '\n' && text[end] != ' ') ++end;
      text.replace(start, end - start, "<usuario>");
      p = start + 9;
    }
  }
  if (const char* user = std::getenv("USERNAME")) replace_all(user, "<usuario>");
  if (const char* host = std::getenv("COMPUTERNAME")) replace_all(host, "<equipo>");
  return text;
}

std::string UrlEncode(const std::string& s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += char(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

std::string WindowsVersion() {
  OSVERSIONINFOEXW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) fn(&info);
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "Windows %lu.%lu (build %lu)", info.dwMajorVersion, info.dwMinorVersion,
                info.dwBuildNumber);
  return buf;
}

std::string GpuDescription() { return QuerySystemInfo().gpu; }

std::string BuildId() {
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%08lX", nt->FileHeader.TimeDateStamp);
  return buf;
}

std::string Cvar(const char* name) {
  const std::string v = rex::cvar::GetFlagByName(name);
  return v.empty() ? "?" : v;
}

#endif  // _WIN32

}  // namespace

void CrashReporterOffer(const std::filesystem::path& log_dir) {
#ifdef _WIN32
  std::error_code ec;
  if (!std::filesystem::is_directory(log_dir, ec)) return;
  std::filesystem::path newest;
  std::filesystem::file_time_type newest_time{};
  for (const auto& entry : std::filesystem::directory_iterator(log_dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("crash_", 0) != 0 || entry.path().extension() != ".txt") continue;
    std::filesystem::path seen = entry.path();
    seen += ".visto";
    if (std::filesystem::exists(seen, ec)) continue;
    const auto t = entry.last_write_time(ec);
    if (newest.empty() || t > newest_time) {
      newest = entry.path();
      newest_time = t;
    }
  }
  if (newest.empty()) return;
  // Una sola pregunta por informe (y por todos los anteriores sin ver).
  for (const auto& entry : std::filesystem::directory_iterator(log_dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("crash_", 0) == 0 && entry.path().extension() == ".txt") {
      std::ofstream(entry.path().string() + ".visto") << "visto\n";
    }
  }

  const std::wstring text = Wide(TrIn(Lang::kEnglish,
      "Lost Odyssey HD Remaster se cerró de forma inesperada la última vez.\n\n¿Quieres avisarnos para que "
      "podamos arreglarlo?\n\nSe abrirá GitHub con un informe ya rellenado (versión, tarjeta gráfica y dónde "
      "falló). Revísalo y pulsa Enviar. No se incluye ningún dato personal ni del juego."));
  const std::wstring title = Wide(TrIn(Lang::kEnglish, "Lost Odyssey HD Remaster - Informe de cierre"));
  if (MessageBoxW(nullptr, text.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) !=
      IDYES) {
    return;
  }

  // Informe del fallo (motivo, excepcion, pila y funciones del juego).
  std::ifstream in(newest);
  std::stringstream raw;
  raw << in.rdbuf();
  const std::string game_dir = rex::filesystem::GetExecutableFolder().string();
  std::string report = Sanitize(raw.str(), game_dir);
  const std::string head = std::string("**Version:** ") + LO_VERSION_STRING + " (build " + BuildId() +
                           ")\n**System:** " + WindowsVersion() + "\n**GPU:** " +
                           Sanitize(GpuDescription(), game_dir) + "\n**Backend:** " + Cvar("lo_gpu_backend") +
                           " | **Resolution:** " + Cvar("lo_video_preset") +
                           "\n\n**What were you doing?** (please fill in if you can):\n\n\n**Report:**\n```\n";
  const std::string tail = "\n```\n";
  const std::string issue_base = LO_ISSUES_URL;
  auto build_url = [&](const std::string& body_report) {
    return issue_base + "?labels=crash&title=" +
           UrlEncode(std::string("Unexpected crash (v") + LO_VERSION_STRING + ")") +
           "&body=" + UrlEncode(head + body_report + tail);
  };
  // La URL no puede pasar de ~8000 caracteres: se recorta el informe hasta que quepa.
  std::string url = build_url(report);
  while (url.size() > 7600 && report.size() > 200) {
    report.resize(report.size() * 8 / 10);
    url = build_url(report + "\n[recortado]");
  }
  if (issue_base.find("OWNER") != std::string::npos) {
    // Repositorio aun sin configurar: se abre la carpeta con el informe.
    ShellExecuteW(nullptr, L"open", log_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return;
  }
  ShellExecuteW(nullptr, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
  (void)log_dir;
#endif
}

}  // namespace lo
