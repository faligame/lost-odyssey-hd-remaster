// lostodyssey - ReXGlue Recompiled Project
//
// updater.exe: aplica una actualizacion ya descargada y descomprimida. Lo lanza el juego (copiado en
// update\updater.exe para poder reemplazar el updater.exe de la carpeta del juego) con:
//   updater.exe --pid <proceso del juego> --source <carpeta con los ficheros nuevos> --target <carpeta del juego>
// 1. Espera a que el juego termine.  2. Copia a update\backup lo que va a reemplazar.  3. Copia lo nuevo
// (fichero temporal + renombrado).  4. Si algo falla, restaura la copia y borra lo que se habia creado.
// 5. Abre el juego. Nunca toca config, saves, data, cache, logs, update, textures ni textures.lopack, ni borra
// ficheros que el paquete no traiga. Registro: update\update.log. Sin dependencias del SDK ni del juego.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::ofstream g_log;

void Log(const std::wstring& line) {
  if (!g_log.is_open()) return;
  const int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
  WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, s.data(), n, nullptr, nullptr);
  g_log << s << std::endl;
}

bool Protected(const fs::path& rel) {
  static const std::set<std::wstring> kNames = {L"config", L"saves",  L"data",         L"cache",
                                                L"logs",   L"update", L"textures",     L"textures.lopack"};
  std::wstring first = rel.begin()->wstring();
  for (auto& c : first) c = wchar_t(towlower(c));
  if (kNames.count(first)) return true;
  const std::wstring ext = rel.extension().wstring();
  return ext == L".part" || ext == L".new";
}

bool CopyWithRetry(const fs::path& from, const fs::path& to, bool replace) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (!replace) {
      if (fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) return true;
    } else {
      const fs::path tmp = to.wstring() + L".new";
      if (fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec) &&
          MoveFileExW(tmp.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return true;
      }
      fs::remove(tmp, ec);
    }
    Sleep(500);  // antivirus u otro proceso con el fichero abierto
  }
  return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  DWORD pid = 0;
  fs::path source, target;
  for (int i = 1; i + 1 < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--pid") pid = DWORD(_wtoi(argv[++i]));
    else if (a == L"--source") source = argv[++i];
    else if (a == L"--target") target = argv[++i];
  }
  LocalFree(argv);
  if (source.empty() || target.empty()) return 2;

  const fs::path work = target / "update";
  const fs::path backup = work / "backup";
  std::error_code ec;
  fs::create_directories(work, ec);
  g_log.open(work / "update.log", std::ios::app);
  Log(L"== actualizacion: " + source.wstring() + L" -> " + target.wstring());

  if (pid) {
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
      WaitForSingleObject(h, 60000);
      CloseHandle(h);
    }
  }
  Sleep(500);

  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(source, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const fs::path rel = fs::relative(e.path(), source, ec);
    if (!rel.empty() && !Protected(rel)) files.push_back(rel);
  }
  Log(L"ficheros del paquete: " + std::to_wstring(files.size()));

  fs::remove_all(backup, ec);
  std::vector<fs::path> replaced, created;
  bool ok = !files.empty();
  std::wstring failed;
  for (const auto& rel : files) {
    const fs::path dst = target / rel;
    const bool existed = fs::exists(dst, ec);
    if (existed && !CopyWithRetry(dst, backup / rel, false)) {
      ok = false;
      failed = L"copia de seguridad de " + rel.wstring();
      break;
    }
    (existed ? replaced : created).push_back(rel);
    if (!CopyWithRetry(source / rel, dst, true)) {
      ok = false;
      failed = L"copiando " + rel.wstring();
      break;
    }
  }

  if (!ok) {
    Log(L"FALLO " + failed + L": se restaura la version anterior");
    for (const auto& rel : replaced) CopyWithRetry(backup / rel, target / rel, true);
    for (const auto& rel : created) fs::remove(target / rel, ec);
    MessageBoxW(nullptr,
                (L"The update could not be installed (" + failed +
                 L"). Your previous version was restored.\n\nNo se pudo instalar la actualización; se ha restaurado la versión anterior.")
                    .c_str(),
                L"Lost Odyssey HD Remaster", MB_OK | MB_ICONWARNING | MB_TOPMOST);
  } else {
    Log(L"actualizacion aplicada");
  }

  // Abrir el juego (nuevo o restaurado).
  const fs::path exe = target / "lostodyssey.exe";
  std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, target.c_str(), &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  return ok ? 0 : 1;
}
