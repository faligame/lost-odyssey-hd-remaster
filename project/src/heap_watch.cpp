// lostodyssey - ReXGlue Recompiled Project
//
// Ver heap_watch.h.

#include "heap_watch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>
#include <utility>
#include <string>
#include <cstdio>
#include <thread>

#include <rex/cvar.h>
#include <rex/kernel/crt/heap.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>

REXCVAR_DEFINE_UINT32(lo_heap_watch_seconds, 60, "LostOdyssey/Diagnostico",
                      "Escribe en el log el uso de la memoria del guest cada N segundos (0 = no)");

namespace lo {
void EncounterNameSelfTest(uint8_t* base);  // encounter_hooks.cpp
namespace {

std::atomic<bool> g_arrancado{false};

struct Uso {
  uint32_t base = 0;
  uint32_t total_mb = 0;
  uint32_t usado_mb = 0;
  uint32_t libre_mb = 0;
};

// Un monton del guest, en megas. Solo usa lo publico de BaseHeap: paginas
// totales, paginas sin reservar y tamano de pagina.
bool LeerUso(rex::memory::Memory* memoria, bool fisico, uint32_t tam_pagina, Uso* salida) {
  auto* monton = memoria ? memoria->LookupHeapByType(fisico, tam_pagina) : nullptr;
  if (!monton) {
    return false;
  }
  const uint64_t pagina = monton->page_size();
  const uint64_t total = uint64_t(monton->total_page_count()) * pagina;
  const uint64_t libre = uint64_t(monton->unreserved_page_count()) * pagina;
  salida->base = monton->heap_base();
  salida->total_mb = uint32_t(total >> 20);
  salida->libre_mb = uint32_t(libre >> 20);
  salida->usado_mb = uint32_t((total - libre) >> 20);
  return true;
}

// La "ventana de sistema" son los ultimos 256 MB del monton virtual de 4 KB
// (AllocSystemHeap busca solo ahi: heap_base + heap_size - 0x10000000). Es de
// donde salen el bloque de estado, el TLS y el scratch de cada hilo, y los APC,
// asi que es la que se agota primero. Se recorre con QueryRegionInfo para saber
// cuanto queda, en cuantos trozos y cual es el hueco mayor: si queda sitio pero
// el hueco mayor es diminuto, el problema es fragmentacion, no falta de memoria.
struct Ventana {
  uint32_t usado_mb = 0;
  uint32_t libre_mb = 0;
  uint32_t hueco_mayor_kb = 0;
  uint32_t regiones = 0;
  uint32_t de_pool = 0;  // regiones que son ExAllocatePool (marca 0xAA en +2)
  uint32_t de_apc = 0;   // regiones que son un APC encolado
  uint32_t muestras = 0;
  char muestra[3][64] = {};
  // Las 4 etiquetas de pool mas repetidas: ExAllocatePoolTypeWithTag guarda en
  // la cabecera de 8 bytes la etiqueta de 4 letras que pasa quien reserva, asi
  // que dicen QUIEN se esta comiendo la ventana.
  uint32_t etiqueta[4] = {0, 0, 0, 0};
  uint32_t cuenta[4] = {0, 0, 0, 0};
};

// Cuenta una etiqueta mas. Tabla pequena y busqueda lineal: hay pocas
// etiquetas distintas y esto corre una vez cada N segundos.
void Anotar(std::vector<std::pair<uint32_t, uint32_t>>* tabla, uint32_t etiqueta) {
  for (auto& fila : *tabla) {
    if (fila.first == etiqueta) {
      ++fila.second;
      return;
    }
  }
  if (tabla->size() < 256) {
    tabla->emplace_back(etiqueta, 1u);
  }
}

bool LeerVentanaSistema(rex::memory::Memory* memoria, Ventana* salida) {
  auto* monton = memoria ? memoria->LookupHeapByType(false, 4096) : nullptr;
  if (!monton) {
    return false;
  }
  const uint32_t fin = monton->heap_base() + monton->heap_size();
  const uint32_t inicio = fin - 0x10000000u;
  uint64_t usado = 0, libre = 0, mayor = 0;
  uint32_t regiones = 0, de_pool = 0, sin_nombre = 0;
  std::vector<std::pair<uint32_t, uint32_t>> etiquetas;
  uint32_t direccion = inicio;
  for (int vueltas = 0; direccion < fin && vueltas < 200000; ++vueltas) {
    rex::memory::HeapAllocationInfo info{};
    if (!monton->QueryRegionInfo(direccion, &info)) {
      break;
    }
    uint32_t tamano = info.region_size;
    if (tamano == 0) {
      tamano = monton->page_size();
    }
    if (direccion + tamano > fin) {
      tamano = fin - direccion;
    }
    if (info.state == 0) {
      libre += tamano;
      mayor = std::max<uint64_t>(mayor, tamano);
    } else {
      usado += tamano;
      ++regiones;
      if ((info.state & rex::memory::kMemoryAllocationCommit) != 0) {
        const auto* bytes = memoria->TranslateVirtual<const uint8_t*>(direccion);
        if (bytes != nullptr) {
          const auto palabra = [bytes](int off) {
            return (uint32_t(bytes[off]) << 24) | (uint32_t(bytes[off + 1]) << 16) |
                   (uint32_t(bytes[off + 2]) << 8) | uint32_t(bytes[off + 3]);
          };
          if (bytes[2] == 0xAA) {
            // Cabecera de ExAllocatePoolTypeWithTag: 0xAA en +2, etiqueta en +4.
            ++de_pool;
            Anotar(&etiquetas, palabra(4));
          } else if (palabra(16) == 0xF00DFF00u) {
            // XAPC con la rutina de kernel de relleno: es un APC encolado.
            ++salida->de_apc;
          } else {
            // Lo que no reconocemos: guardamos las TRES ULTIMAS (las reservas
            // van de abajo hacia arriba, asi que las de arriba son las nuevas).
            std::snprintf(salida->muestra[sin_nombre % 3], sizeof(salida->muestra[0]),
                          "0x%08X (%u B): %08X %08X %08X %08X", direccion, tamano, palabra(0),
                          palabra(4), palabra(8), palabra(12));
            ++sin_nombre;
            salida->muestras = std::min<uint32_t>(sin_nombre, 3u);
          }
        }
      }
    }
    direccion += tamano;
  }

  std::sort(etiquetas.begin(), etiquetas.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  for (size_t i = 0; i < etiquetas.size() && i < 4; ++i) {
    salida->etiqueta[i] = etiquetas[i].first;
    salida->cuenta[i] = etiquetas[i].second;
  }

  salida->usado_mb = uint32_t(usado >> 20);
  salida->libre_mb = uint32_t(libre >> 20);
  salida->hueco_mayor_kb = uint32_t(mayor >> 10);
  salida->regiones = regiones;
  salida->de_pool = de_pool;
  return true;
}

// La etiqueta son 4 letras ASCII; si no lo son, se muestra en hexadecimal.
std::string TextoEtiqueta(uint32_t etiqueta) {
  std::string texto;
  for (int i = 3; i >= 0; --i) {
    const auto c = char((etiqueta >> (i * 8)) & 0xFF);
    if (c < 32 || c > 126) {
      char hex[16] = {};
      std::snprintf(hex, sizeof(hex), "0x%08X", etiqueta);
      return hex;
    }
    texto.push_back(c);
  }
  return texto;
}

void Vigilar(uint32_t segundos) {
  for (;;) {
    auto* memoria = REX_KERNEL_MEMORY();
    Uso v4{}, v64{};
    const bool hay4 = LeerUso(memoria, false, 4096, &v4);
    const bool hay64 = LeerUso(memoria, false, 65536, &v64);

    // Montones fisicos (los usan el video, el audio y la GPU): si uno se llena,
    // MmAllocatePhysicalMemoryEx devuelve 0 y el juego sigue con un nulo.
    Uso f4{}, f64{}, f16m{};
    const bool hayf4 = LeerUso(memoria, true, 4096, &f4);
    const bool hayf64 = LeerUso(memoria, true, 65536, &f64);
    const bool hayf16 = LeerUso(memoria, true, 16u * 1024u * 1024u, &f16m);
    if (hayf4 || hayf64 || hayf16) {
      REXLOG_INFO(
          "lo_heap: fisico 4K 0x{:08X} usado {} MB de {} | fisico 64K 0x{:08X} usado {} MB de {} | "
          "fisico 16M 0x{:08X} usado {} MB de {}",
          f4.base, f4.usado_mb, f4.total_mb, f64.base, f64.usado_mb, f64.total_mb, f16m.base,
          f16m.usado_mb, f16m.total_mb);
    }

    Ventana ventana{};
    if (LeerVentanaSistema(memoria, &ventana)) {
      REXLOG_INFO(
          "lo_heap: ventana de sistema (ultimos 256 MB del 4K) usado {} MB, libre {} MB, "
          "hueco mayor {} KB, {} regiones ({} de ExAllocatePool, {} APC)",
          ventana.usado_mb, ventana.libre_mb, ventana.hueco_mayor_kb, ventana.regiones,
          ventana.de_pool, ventana.de_apc);
      if (ventana.de_pool != 0) {
        REXLOG_INFO("lo_heap: etiquetas de pool mas repetidas: {} x{}, {} x{}, {} x{}, {} x{}",
                    TextoEtiqueta(ventana.etiqueta[0]), ventana.cuenta[0],
                    TextoEtiqueta(ventana.etiqueta[1]), ventana.cuenta[1],
                    TextoEtiqueta(ventana.etiqueta[2]), ventana.cuenta[2],
                    TextoEtiqueta(ventana.etiqueta[3]), ventana.cuenta[3]);
      }
    }

    if (hay4 || hay64) {
      REXLOG_INFO(
          "lo_heap: virtual 4K 0x{:08X} usado {} MB de {} ({} libres) | virtual 64K 0x{:08X} "
          "usado {} MB de {} ({} libres)",
          v4.base, v4.usado_mb, v4.total_mb, v4.libre_mb, v64.base, v64.usado_mb, v64.total_mb,
          v64.libre_mb);
    }

    // Censo de objetos del kernel. Cada objeto (evento, semaforo, hilo,
    // fichero...) reserva su estructura con SystemHeapAlloc, y esa reserva se
    // redondea a una pagina de 4 KB, asi que cada objeto vivo ocupa 4 KB de la
    // ventana de 256 MB: 65.536 objetos y se acabo. Si esta cuenta sube y no
    // baja, el juego (o nosotros) no esta cerrando handles.
    if (auto* objetos = REX_KERNEL_OBJECTS()) {
      const auto todos = objetos->GetAllObjects();
      uint32_t por_tipo[16] = {};
      for (const auto& obj : todos) {
        const auto tipo = uint32_t(obj->type());
        if (tipo < 16) {
          ++por_tipo[tipo];
        }
      }
      REXLOG_INFO(
          "lo_heap: objetos del kernel {} (eventos {}, semaforos {}, mutex {}, hilos {}, "
          "ficheros {}, temporizadores {}, avisos {}, enumeradores {})",
          todos.size(), por_tipo[uint32_t(rex::system::XObject::Type::Event)],
          por_tipo[uint32_t(rex::system::XObject::Type::Semaphore)],
          por_tipo[uint32_t(rex::system::XObject::Type::Mutant)],
          por_tipo[uint32_t(rex::system::XObject::Type::Thread)],
          por_tipo[uint32_t(rex::system::XObject::Type::File)],
          por_tipo[uint32_t(rex::system::XObject::Type::Timer)],
          por_tipo[uint32_t(rex::system::XObject::Type::NotifyListener)],
          por_tipo[uint32_t(rex::system::XObject::Type::Enumerator)]);
    }

    if (REXCVAR_GET(rexcrt_heap_enable)) {
      const auto d = rex::kernel::crt::GetHeap().GetDiagnostics();
      REXLOG_INFO(
          "lo_heap: rexcrt capacidad {} MB, en uso {} MB, maximo {} MB, peticion mayor {} KB, "
          "fallos {}",
          d.capacity >> 20, d.allocated >> 20, d.peak_allocated >> 20, d.peak_request_size >> 10,
          d.oom_count);
    }

    // Prueba de lectura de nombres de UnrealScript (una vez, al minuto).
    static int vueltas = 0;
    if (++vueltas == 2 && memoria != nullptr) {
      EncounterNameSelfTest(memoria->virtual_membase());
    }

    std::this_thread::sleep_for(std::chrono::seconds(segundos));
  }
}

}  // namespace

void HeapWatchStart() {
  const uint32_t segundos = REXCVAR_GET(lo_heap_watch_seconds);
  if (segundos == 0) {
    return;
  }
  bool esperado = false;
  if (!g_arrancado.compare_exchange_strong(esperado, true)) {
    return;
  }
  std::thread(Vigilar, segundos).detach();
  REXLOG_INFO("lo_heap: vigilante de memoria del guest cada {} s", segundos);
}

}  // namespace lo
