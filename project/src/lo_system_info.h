// lostodyssey - ReXGlue Recompiled Project
//
// Datos del equipo para el asistente (requisitos) y el informe de cierre.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#endif

namespace lo {

struct SystemInfo {
  std::string gpu = "desconocida";  // todas las tarjetas, sin repetir: "NVIDIA ... (driver x.y.z.w, 12288 MB)"
  std::string gpu_primary = "desconocida";  // la de mas memoria
  uint64_t vram_mb = 0;             // memoria dedicada de la mejor tarjeta
  uint64_t ram_mb = 0;
};

#ifdef _WIN32
inline std::string NarrowUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

inline SystemInfo QuerySystemInfo() {
  SystemInfo info;
  std::string gpus;
  IDXGIFactory1* factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    for (UINT i = 0;; ++i) {
      IDXGIAdapter1* adapter = nullptr;
      if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc{};
      adapter->GetDesc1(&desc);
      if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        LARGE_INTEGER driver{};
        char drv[64] = "?";
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver))) {
          std::snprintf(drv, sizeof(drv), "%u.%u.%u.%u", unsigned(HIWORD(driver.HighPart)),
                        unsigned(LOWORD(driver.HighPart)), unsigned(HIWORD(driver.LowPart)),
                        unsigned(LOWORD(driver.LowPart)));
        }
        const uint64_t vram = uint64_t(desc.DedicatedVideoMemory) >> 20;
        char extra[96];
        std::snprintf(extra, sizeof(extra), " (driver %s, %llu MB)", drv, (unsigned long long)vram);
        const std::string entry = NarrowUtf8(desc.Description) + extra;
        if (gpus.find(entry) == std::string::npos) {
          if (!gpus.empty()) gpus += "; ";
          gpus += entry;
        }
        if (vram > info.vram_mb) {
          info.vram_mb = vram;
          info.gpu_primary = entry;
        }
      }
      adapter->Release();
    }
    factory->Release();
  }
  if (!gpus.empty()) info.gpu = gpus;
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  if (GlobalMemoryStatusEx(&mem)) info.ram_mb = mem.ullTotalPhys >> 20;
  return info;
}
#endif

}  // namespace lo
