// lostodyssey - ReXGlue Recompiled Project
//
// Ver crash_handler.h.
//
// Se instala con SetUnhandledExceptionFilter, asi que solo ve los cierres de
// verdad: las excepciones que el runtime usa para vigilar la memoria del guest
// son de primera oportunidad y no pasan por aqui.
//
// El informe se escribe en dos fases, para que un fallo al simbolizar no nos
// deje sin datos:
//   1. Datos crudos con API de Windows: excepcion, registros y la pila
//      desenrollada con RtlVirtualUnwind.
//   2. Mejor esfuerzo: traducir cada direccion a su funcion leyendo el .map del
//      enlazador, y anotar los ajustes en uso.
//
// Las funciones del juego se llaman sub_XXXXXXXX, donde XXXXXXXX es su
// direccion en el ejecutable original de Xbox 360. Eso es lo que convierte un
// "se ha cerrado" en una pista: se puede mirar el codigo generado de esa
// funcion, ponerle un hook o compararla con Xenia.

#include "crash_handler.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <chrono>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

#ifdef LO_DEV
REXCVAR_DEFINE_BOOL(lo_crash_trace_cpp, false, "LostOdyssey/Diagnostico",
                    "Diagnostico: deja un informe cada vez que se lanza una excepcion de C++ (las "
                    "tres primeras), aunque alguien la atrape");
#endif
REXCVAR_DEFINE_INT32(lo_hang_seconds, 12, "LostOdyssey/Diagnostico",
                     "Detector de cuelgues: segundos sin fotogramas para dejar un informe con la pila de "
                     "todos los hilos en logs/cuelgue_<fecha>.txt (0 = desactivado)");
#ifdef LO_DEV
REXCVAR_DEFINE_INT32(lo_hang_test, 0, "LostOdyssey/Diagnostico",
                     "Prueba del detector de cuelgues: segundos de juego hasta congelarlo 20 s a proposito "
                     "(0 = desactivado)");
REXCVAR_DEFINE_INT32(lo_crash_test, 0, "LostOdyssey/Diagnostico",
                     "Prueba del capturador de cierres: segundos de juego hasta provocar un cierre a "
                     "proposito (0 = desactivado)");
#endif

namespace lo {

#ifdef _WIN32

namespace {

constexpr int kMaxFrames = 64;

std::filesystem::path g_log_dir;
std::filesystem::path g_map_path;
LONG g_handling = 0;
LONG g_informes_en_curso = 0;  // informes a medio escribir (pueden caer dos hilos)

void Write(HANDLE file, const char* text) {
  DWORD written = 0;
  WriteFile(file, text, DWORD(std::strlen(text)), &written, nullptr);
}

void WriteFmt(HANDLE file, const char* format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (n > 0) Write(file, buffer);
}

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "invalid memory access";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating-point divide by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_IN_PAGE_ERROR: return "page-in error";
    case 0xE06D7363: return "uncaught C++ exception";
    default: return "unknown";
  }
}

// Modulo (exe o dll) al que pertenece una direccion.
bool ModuleOf(uint64_t address, char* name, size_t name_size, uint64_t* base) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(address), &module) ||
      !module) {
    return false;
  }
  char path[MAX_PATH] = {};
  if (!GetModuleFileNameA(module, path, MAX_PATH)) return false;
  const char* file = std::strrchr(path, '\\');
  std::snprintf(name, name_size, "%s", file ? file + 1 : path);
  *base = uint64_t(module);
  return true;
}

// Tamano en memoria del modulo, para saber si una direccion es suya.
uint64_t ImageSizeOf(uint64_t base) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
  return nt->OptionalHeader.SizeOfImage;
}

// Pila del hilo que ha fallado, desenrollada sin necesidad de simbolos.
int UnwindStack(const CONTEXT& start, uint64_t* frames, int max_frames) {
  CONTEXT context = start;
  int count = 0;
  while (count < max_frames && context.Rip) {
    frames[count++] = context.Rip;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!function) {
      // Funcion hoja: la direccion de retorno esta en la cima de la pila.
      if (!context.Rsp) break;
      context.Rip = *reinterpret_cast<uint64_t*>(context.Rsp);
      context.Rsp += 8;
      continue;
    }
    PVOID handler_data = nullptr;
    DWORD64 establisher_frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                     &establisher_frame, nullptr);
  }
  return count;
}

struct Symbol {
  uint64_t rva = 0;
  std::string name;
  std::string object;
};

// Lee el .map del enlazador. Sus lineas de simbolo son:
//   " 0001:01c008b0       nombre        0000000141c018b0     fichero.obj"
std::vector<Symbol> LoadMapSymbols(const std::filesystem::path& path) {
  std::vector<Symbol> symbols;
  std::ifstream map(path);
  if (!map) return symbols;
  uint64_t base = 0;
  std::string line;
  while (std::getline(map, line)) {
    if (!base && line.find("Preferred load address is") != std::string::npos) {
      const size_t at = line.find_last_of(' ');
      if (at != std::string::npos) base = std::strtoull(line.c_str() + at + 1, nullptr, 16);
      continue;
    }
    if (!base || line.size() < 30 || line[0] != ' ' || line[5] != ':') continue;
    const char* cursor = line.c_str() + 1;
    char* end = nullptr;
    std::strtoul(cursor, &end, 16);  // seccion
    if (!end || *end != ':') continue;
    std::strtoull(end + 1, &end, 16);  // offset dentro de la seccion
    if (!end) continue;
    while (*end == ' ') ++end;
    const char* name_begin = end;
    while (*end && *end != ' ') ++end;
    Symbol symbol;
    symbol.name.assign(name_begin, size_t(end - name_begin));
    while (*end == ' ') ++end;
    const uint64_t va = std::strtoull(end, &end, 16);
    if (!va || va < base || symbol.name.empty()) continue;
    symbol.rva = va - base;
    while (*end == ' ') ++end;
    symbol.object = end;
    symbols.push_back(std::move(symbol));
  }
  std::sort(symbols.begin(), symbols.end(),
            [](const Symbol& a, const Symbol& b) { return a.rva < b.rva; });
  return symbols;
}

const Symbol* FindSymbol(const std::vector<Symbol>& symbols, uint64_t rva) {
  if (symbols.empty()) return nullptr;
  const auto it = std::upper_bound(symbols.begin(), symbols.end(), rva,
                                   [](uint64_t value, const Symbol& s) { return value < s.rva; });
  if (it == symbols.begin()) return nullptr;
  return &*(it - 1);
}

// Si el nombre lleva sub_XXXXXXXX, XXXXXXXX es la direccion en el ejecutable de
// Xbox 360: lo mas util del informe.
std::string GuestAddressOf(const std::string& name) {
  const size_t at = name.find("sub_");
  if (at == std::string::npos || name.size() < at + 12) return {};
  const std::string hex = name.substr(at + 4, 8);
  if (hex.find_first_not_of("0123456789ABCDEFabcdef") != std::string::npos) return {};
  if (hex[0] != '8') return {};  // el codigo del juego vive en 0x82.. / 0x83..
  return "0x" + hex;
}

// Direccion del anfitrion que provoco el acceso invalido (0 si no aplica).
thread_local uint64_t g_fault_address = 0;  // por hilo: pueden caer dos a la vez

void DescribeRegion(std::ofstream& out, const char* titulo, uint32_t guest) {
  auto* memoria = REX_KERNEL_MEMORY();
  auto* monton = memoria ? memoria->LookupHeap(guest) : nullptr;
  out << "  " << titulo << " 0x" << std::hex << guest << ": ";
  rex::memory::HeapAllocationInfo info{};
  if (monton == nullptr || !monton->QueryRegionInfo(guest, &info)) {
    out << "outside every heap\n" << std::dec;
    return;
  }
  const bool comprometida = (info.state & rex::memory::kMemoryAllocationCommit) != 0;
  const bool reservada = (info.state & rex::memory::kMemoryAllocationReserve) != 0;
  out << (comprometida ? "COMMITTED"
                       : reservada ? "reserved only" : "FREE (never reserved or already released)")
      << ", state 0x" << info.state << " protection 0x" << info.protect;
  if (info.state != 0) {
    out << ", reservation from 0x" << info.allocation_base << " of 0x" << info.allocation_size
        << " bytes";
  } else {
    out << ", free gap of 0x" << info.region_size << " bytes";
  }
  out << std::dec << "\n";
}

// Que habia en la direccion del guest que fallo. Distingue un puntero a memoria
// liberada, uno que nunca existio y el caso "una direccion FISICA usada como si
// fuera virtual" (la memoria fisica se ve en 0xA0000000, 0xC0000000 y 0xE0000000).
void DescribeGuestFault(std::ofstream& out) {
  constexpr uint64_t kVirtual = 0x100000000ull;
  if (g_fault_address < kVirtual || g_fault_address >= kVirtual + 0x100000000ull) {
    return;
  }
  const uint32_t guest = uint32_t(g_fault_address - kVirtual);
  out << "\n== Guest memory at the fault\n";
  try {
    DescribeRegion(out, "virtual", guest);
    if (guest < 0x20000000u) {
      out << "  (if it were a PHYSICAL address used as virtual, it would be at:)\n";
      DescribeRegion(out, "  physical view 64K", 0xA0000000u + guest);
      DescribeRegion(out, "  physical view 4K ", 0xE0000000u + guest);
    }
  } catch (...) {
    out << "  (could not query)\n";
  }
}

void AppendDetails(const std::filesystem::path& report, const uint64_t* frames, int frame_count,
                   uint64_t module_base, uint64_t image_size) {
  std::ofstream out(report, std::ios::app);
  if (!out) return;
  const std::vector<Symbol> symbols = LoadMapSymbols(g_map_path);
  out << "\n== Stack with names\n";
  if (symbols.empty()) {
    out << "  Linker map not available: no function names.\n";
  } else {
    for (int i = 0; i < frame_count; ++i) {
      out << "  #" << i << "  0x" << std::hex << frames[i] << std::dec;
      if (frames[i] < module_base || (image_size && frames[i] - module_base >= image_size)) {
        out << "  (another dll: outside lostodyssey.exe)\n";
        continue;
      }
      const uint64_t rva = frames[i] - module_base;
      const Symbol* symbol = FindSymbol(symbols, rva);
      if (symbol) {
        out << "  " << symbol->name << " +" << (rva - symbol->rva) << "  [" << symbol->object << "]";
        const std::string guest = GuestAddressOf(symbol->name);
        if (!guest.empty()) out << "  <- game function " << guest;
      } else {
        out << "  (no symbol)";
      }
      out << "\n";
    }
  }
  DescribeGuestFault(out);
  out << "\n== Settings in use\n";
  for (const char* name :
       {"lo_video_preset", "lo_ssaa", "lo_gpu_backend", "draw_resolution_scale_x", "lo_60fps",
        "lo_turbo_enabled", "odisea_smaa", "odisea_texture_pack", "lo_save_anywhere", "lo_no_random_battles"}) {
    out << "  " << name << " = " << rex::cvar::GetFlagByName(name) << "\n";
  }
  out << "\nIf the stack names a game function (sub_XXXXXXXX), that is its original Xbox 360 address.\n";
}

void ReportCrash(EXCEPTION_POINTERS* pointers, const char* reason) {
  // Hasta dos informes: si otro hilo cae a la vez (paso el 27-sep: dos hilos
  // leyendo 0x10006610 a la vez), el segundo deja el suyo en crash_..._hilo2.txt,
  // que es justo el que dice quien mas tocaba lo mismo. A partir del tercero, o
  // cuando el segundo termina, se espera para no cerrar el proceso con el primer
  // informe a medias.
  const LONG turno = InterlockedIncrement(&g_handling);
  if (turno > 2) {
    Sleep(8000);
    return;
  }
  const bool segundo = turno == 2;
  InterlockedIncrement(&g_informes_en_curso);

  SYSTEMTIME now = {};
  GetLocalTime(&now);
  char file_name[64] = {};
  std::snprintf(file_name, sizeof(file_name), "crash_%04d%02d%02d_%02d%02d%02d%s.txt", now.wYear,
                now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                segundo ? "_hilo2" : "");
  std::error_code ec;
  std::filesystem::create_directories(g_log_dir, ec);
  const std::filesystem::path report = g_log_dir / file_name;

  HANDLE file = CreateFileW(report.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;

  uint64_t frames[kMaxFrames] = {};
  int frame_count = 0;
  uint64_t module_base = uint64_t(GetModuleHandleW(nullptr));
  char module_name[MAX_PATH] = "lostodyssey.exe";

  WriteFmt(file, "Lost Odyssey - crash report\n%04d-%02d-%02d %02d:%02d:%02d\n\n", now.wYear,
           now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  WriteFmt(file, "Reason: %s\n", reason);

  if (pointers && pointers->ExceptionRecord) {
    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
    const uint64_t address = uint64_t(record.ExceptionAddress);
    uint64_t base = module_base;
    ModuleOf(address, module_name, sizeof(module_name), &base);
    WriteFmt(file, "Exception: 0x%08lX (%s)\n", record.ExceptionCode,
             ExceptionName(record.ExceptionCode));
    WriteFmt(file, "Address: 0x%llX  in %s + 0x%llX\n", address, module_name, address - base);
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
      const char* kind = record.ExceptionInformation[0] == 1
                             ? "writing"
                             : (record.ExceptionInformation[0] == 8 ? "executing" : "reading");
      WriteFmt(file, "Memory: %s 0x%llX\n", kind, uint64_t(record.ExceptionInformation[1]));
    }
    WriteFmt(file, "Thread: %lu\n", GetCurrentThreadId());
    if (pointers->ContextRecord) {
      const CONTEXT& c = *pointers->ContextRecord;
      WriteFmt(file, "\n== Registers\nRIP %016llX  RSP %016llX  RBP %016llX\n", c.Rip, c.Rsp, c.Rbp);
      WriteFmt(file, "RAX %016llX  RBX %016llX  RCX %016llX  RDX %016llX\n", c.Rax, c.Rbx, c.Rcx,
               c.Rdx);
      WriteFmt(file, "RSI %016llX  RDI %016llX  R8  %016llX  R9  %016llX\n", c.Rsi, c.Rdi, c.R8, c.R9);
      WriteFmt(file, "R12 %016llX  R13 %016llX  R14 %016llX  R15 %016llX\n", c.R12, c.R13, c.R14,
               c.R15);
      frame_count = UnwindStack(c, frames, kMaxFrames);
    }
    // OJO: `base` es el modulo donde cayo la excepcion (puede ser VCRUNTIME o el
    // runtime del SDK); los nombres se buscan siempre en el mapa del EXE, asi
    // que module_base se queda con la base del exe.
    g_fault_address = record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                              record.NumberParameters >= 2
                          ? uint64_t(record.ExceptionInformation[1])
                          : 0;
  } else {
    WriteFmt(file, "Thread: %lu\n", GetCurrentThreadId());
    frame_count = int(CaptureStackBackTrace(0, kMaxFrames, reinterpret_cast<PVOID*>(frames), nullptr));
  }

  Write(file, "\n== Stack (addresses)\n");
  for (int i = 0; i < frame_count; ++i) {
    WriteFmt(file, "  #%d  0x%llX  (+0x%llX)\n", i, frames[i], frames[i] - module_base);
  }
  FlushFileBuffers(file);
  CloseHandle(file);

  // Segunda fase: nombres y ajustes. Si algo falla aqui, el informe ya sirve.
  try {
    AppendDetails(report, frames, frame_count, module_base, ImageSizeOf(module_base));
  } catch (...) {
  }
  try {
    REXLOG_ERROR("[crash] el juego se ha cerrado; informe en {}", report.string());
  } catch (...) {
  }
  InterlockedDecrement(&g_informes_en_curso);
  // El que termine primero no puede volver todavia: al volver se cierra el
  // proceso y el otro informe se quedaria a medias (paso el 28-sep con el
  // _hilo2, que salio sin nombres). Se espera a que no quede ninguno en curso.
  for (int espera = 0; espera < 80 && InterlockedCompareExchange(&g_informes_en_curso, 0, 0) > 0;
       ++espera) {
    Sleep(100);
  }
  if (segundo) {
    Sleep(8000);  // que el primer hilo acabe su informe antes de cerrar el proceso
  }
}

// Traza de excepciones de C++ en el momento de lanzarlas (cvar
// lo_crash_trace_cpp). Sirve cuando el SDK las atrapa y solo deja el mensaje:
// aqui se ve QUIEN la lanza. Como maximo tres, para no inundar.
LONG WINAPI OnVectoredException(EXCEPTION_POINTERS* pointers) {
  static LONG traced = 0;
  if (pointers && pointers->ExceptionRecord &&
      pointers->ExceptionRecord->ExceptionCode == 0xE06D7363 &&
#ifdef LO_DEV
      REXCVAR_GET(lo_crash_trace_cpp) &&
#else
      false &&
#endif
      InterlockedIncrement(&traced) <= 3) {
    ReportCrash(pointers, "C++ exception trace (lo_crash_trace_cpp)");
    InterlockedExchange(&g_handling, 0);  // el informe de un cierre real sigue saliendo
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers) {
  ReportCrash(pointers, "unhandled exception");
  return EXCEPTION_EXECUTE_HANDLER;  // cerrar sin el dialogo de Windows
}

// El SDK atrapa algunos fallos de los hilos del juego y termina el proceso por
// la via rapida (abort). Con una senal instalada, abort() nos avisa antes.
void OnAbortSignal(int) {
  ReportCrash(nullptr, "abort(): fatal error (possibly trapped by the runtime)");
  std::signal(SIGABRT, SIG_DFL);
  std::raise(SIGABRT);
}

void OnTerminate() {
  ReportCrash(nullptr, "terminate(): uncaught C++ exception");
  std::abort();
}

}  // namespace

void CrashHandlerTestTick() {
#ifndef LO_DEV
  return;
#else
  const int seconds = REXCVAR_GET(lo_crash_test);
  if (seconds <= 0) return;
  static const auto start = std::chrono::steady_clock::now();
  if (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) return;
  REXLOG_WARN("[crash] lo_crash_test: provocando un cierre a proposito");
  *reinterpret_cast<volatile int*>(0) = 0x42;
#endif
}

void CrashHandlerInit(const std::filesystem::path& log_dir, const std::filesystem::path& map_path) {
  g_log_dir = log_dir;
  g_map_path = map_path;
  SetUnhandledExceptionFilter(OnUnhandledException);
  std::set_terminate(OnTerminate);
  std::signal(SIGABRT, OnAbortSignal);
  AddVectoredExceptionHandler(1, OnVectoredException);
}

// Cadena de llamadas del juego en este instante, sin cerrar nada (para
// investigar: "quien llama a esto"). Solo se escriben las funciones del juego
// (sub_XXXXXXXX -> su direccion original del Xbox 360), de dentro a fuera.
void LogGuestStack(const char* etiqueta) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  static const std::vector<Symbol> simbolos = LoadMapSymbols(g_map_path);
  if (simbolos.empty()) {
    return;
  }
  void* marcos[62] = {};
  const USHORT n = RtlCaptureStackBackTrace(1, 62, marcos, nullptr);
  const uint64_t base = uint64_t(GetModuleHandleW(nullptr));
  const uint64_t tam = ImageSizeOf(base);
  std::string cadena;
  for (USHORT i = 0; i < n; ++i) {
    const uint64_t pc = uint64_t(marcos[i]);
    if (pc < base || (tam != 0 && pc - base >= tam)) {
      continue;
    }
    const Symbol* simbolo = FindSymbol(simbolos, pc - base);
    if (simbolo == nullptr) {
      continue;
    }
    const std::string guest = GuestAddressOf(simbolo->name);
    if (guest.empty()) {
      continue;
    }
    if (!cadena.empty()) {
      cadena += " <- ";
    }
    cadena += guest;
  }
  REXLOG_INFO("lo_pila [{}]: {}", etiqueta, cadena);
}

// --- Detector de cuelgues -----------------------------------------------------

namespace {

std::atomic<int64_t> g_last_frame_ms{0};
std::atomic<uint64_t> g_frame_count{0};
std::atomic<bool> g_hang_watch_started{false};

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct ThreadStack {
  DWORD id = 0;
  int frame_count = 0;
  uint64_t frames[kMaxFrames] = {};
  bool captured = false;
};

std::string ThreadName(DWORD id) {
  using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (!get_description) return {};
  HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
  if (!thread) return {};
  std::string name;
  PWSTR wide = nullptr;
  if (SUCCEEDED(get_description(thread, &wide)) && wide) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
      name.resize(size_t(n - 1));
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), n, nullptr, nullptr);
    }
    LocalFree(wide);
  }
  CloseHandle(thread);
  return name;
}

// Pila de cada hilo del proceso (menos el propio vigilante). Mientras un hilo
// esta suspendido solo se lee su contexto y se desenrolla sobre un array ya
// reservado: nada de memoria dinamica, por si el hilo tenia cogido el monton.
std::vector<ThreadStack> CaptureAllThreads() {
  std::vector<DWORD> ids;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
      if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self) ids.push_back(entry.th32ThreadID);
    }
    CloseHandle(snapshot);
  }
  std::vector<ThreadStack> stacks(ids.size());
  for (size_t i = 0; i < ids.size(); ++i) {
    ThreadStack& s = stacks[i];
    s.id = ids[i];
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                               s.id);
    if (!thread) continue;
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT context{};
      context.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(thread, &context)) {
        s.frame_count = UnwindStack(context, s.frames, kMaxFrames);
        s.captured = true;
      }
      ResumeThread(thread);
    }
    CloseHandle(thread);
  }
  return stacks;
}

void WriteHangReport(int64_t stalled_ms, int report_index) {
  SYSTEMTIME now = {};
  GetLocalTime(&now);
  char file_name[64] = {};
  std::snprintf(file_name, sizeof(file_name), "cuelgue_%04d%02d%02d_%02d%02d%02d%s.txt", now.wYear, now.wMonth,
                now.wDay, now.wHour, now.wMinute, now.wSecond, report_index > 1 ? "_2" : "");
  std::error_code ec;
  std::filesystem::create_directories(g_log_dir, ec);
  const std::filesystem::path report = g_log_dir / file_name;

  const std::vector<ThreadStack> stacks = CaptureAllThreads();
  const std::vector<Symbol> symbols = LoadMapSymbols(g_map_path);
  const uint64_t base = uint64_t(GetModuleHandleW(nullptr));
  const uint64_t size = ImageSizeOf(base);

  std::ofstream out(report);
  if (!out) return;
  out << "Lost Odyssey - informe de cuelgue\n";
  char when[64];
  std::snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d", now.wYear, now.wMonth, now.wDay, now.wHour,
                now.wMinute, now.wSecond);
  out << when << "\n\n";
  out << "El juego lleva " << stalled_ms / 1000 << " s sin dibujar un fotograma (" << g_frame_count.load()
      << " fotogramas desde el arranque).\n";
  out << "Si vuelve a dibujar, el log lo dice ('[cuelgue] el juego volvio a dibujar'): entonces era una\n"
         "carga larga y no un cuelgue.\n";
  out << "\n== Settings in use\n";
  for (const char* name : {"lo_video_preset", "lo_gpu_backend", "lo_60fps", "lo_turbo_enabled", "lo_turbo_scalar",
                           "odisea_texture_pack", "lo_no_random_battles", "user_language", "lo_discs"}) {
    out << "  " << name << " = " << rex::cvar::GetFlagByName(name) << "\n";
  }

  // Primero, en una linea por hilo, solo las funciones del juego: es lo que se
  // compara entre dos cuelgues. Despues, la pila completa.
  auto describe = [&](uint64_t pc, bool guest_only) -> std::string {
    if (pc < base || (size && pc - base >= size)) return guest_only ? std::string() : "(otra dll)";
    const Symbol* symbol = FindSymbol(symbols, pc - base);
    if (!symbol) return guest_only ? std::string() : "(sin simbolo)";
    const std::string guest = GuestAddressOf(symbol->name);
    if (guest_only) return guest;
    return symbol->name + (guest.empty() ? std::string() : "  <- juego " + guest);
  };
  out << "\n== Resumen: funciones del juego en cada hilo (de dentro a fuera)\n";
  for (const ThreadStack& s : stacks) {
    std::string chain;
    for (int i = 0; i < s.frame_count; ++i) {
      const std::string g = describe(s.frames[i], true);
      if (g.empty()) continue;
      if (!chain.empty()) chain += " <- ";
      chain += g;
    }
    if (chain.empty()) continue;
    out << "  hilo " << s.id;
    const std::string name = ThreadName(s.id);
    if (!name.empty()) out << " (" << name << ")";
    out << ": " << chain << "\n";
  }
  out << "\n== Pila completa de cada hilo\n";
  for (const ThreadStack& s : stacks) {
    out << "\n-- hilo " << s.id;
    const std::string name = ThreadName(s.id);
    if (!name.empty()) out << " (" << name << ")";
    if (!s.captured) {
      out << ": no se pudo leer\n";
      continue;
    }
    out << "\n";
    for (int i = 0; i < s.frame_count; ++i) {
      char pc[32];
      std::snprintf(pc, sizeof(pc), "0x%llX", static_cast<unsigned long long>(s.frames[i]));
      out << "  #" << i << "  " << pc << "  " << describe(s.frames[i], false) << "\n";
    }
  }
  out.close();
  try {
    REXLOG_ERROR("[cuelgue] {} s sin fotogramas; informe en {}", stalled_ms / 1000, report.string());
  } catch (...) {
  }
}

void HangWatchThread() {
  int reports = 0;
  int64_t stall_start = 0;
  for (;;) {
    Sleep(1000);
    const int seconds = REXCVAR_GET(lo_hang_seconds);
    const int64_t last = g_last_frame_ms.load(std::memory_order_relaxed);
    if (seconds <= 0 || last == 0) continue;
    const int64_t stalled = SteadyMs() - last;
    if (stalled < int64_t(seconds) * 1000) {
      if (reports > 0 && stall_start) {
        try {
          REXLOG_WARN("[cuelgue] el juego volvio a dibujar tras {} s parado", (SteadyMs() - stall_start) / 1000);
        } catch (...) {
        }
      }
      reports = 0;
      stall_start = 0;
      continue;
    }
    if (!stall_start) stall_start = last;
    // Un informe al pasar el umbral y otro a los 5x: si las pilas cambian entre
    // los dos, el juego avanza (carga lenta); si son iguales, esta atascado.
    if (reports == 0 || (reports == 1 && stalled >= int64_t(seconds) * 5000)) {
      ++reports;
      try {
        WriteHangReport(stalled, reports);
      } catch (...) {
      }
    }
  }
}

}  // namespace

void HangWatchFrame() {
  g_last_frame_ms.store(SteadyMs(), std::memory_order_relaxed);
  g_frame_count.fetch_add(1, std::memory_order_relaxed);
  // Prueba (lo_hang_test = segundos): una vez, congela el hilo que dibuja 20 s.
#ifdef LO_DEV
  const int test = REXCVAR_GET(lo_hang_test);
  if (test > 0) {
    static const int64_t start = SteadyMs();
    static bool done = false;
    if (!done && SteadyMs() - start >= int64_t(test) * 1000) {
      done = true;
      REXLOG_WARN("[cuelgue] lo_hang_test: congelando el juego 20 s a proposito");
      Sleep(20000);
    }
  }
#endif
}

void HangWatchInit() {
  if (g_hang_watch_started.exchange(true)) return;
  std::thread(HangWatchThread).detach();
}

#else

void HangWatchInit() {}
void HangWatchFrame() {}

void LogGuestStack(const char*) {}

void CrashHandlerInit(const std::filesystem::path&, const std::filesystem::path&) {}

void CrashHandlerTestTick() {}

#endif

}  // namespace lo
