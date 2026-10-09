// lostodyssey - ReXGlue Recompiled Project
//
// Estabilidad de la cinematica posterior al jefe Mack (el "cuelgue de Mack" de
// la guia de Xenia), 27-sep-2026.
//
// Lo que pasa, medido paso a paso con el capturador y el log:
//
//   1. XACT (el motor de sonido del XDK) hace streaming de un sonido largo en un
//      bufer de 64 KB cuyo puntero vale 0: es una CARRERA de tiempos (el puntero
//      se asigna tarde respecto a cuando arranca el streaming). Con turbo pasa
//      casi siempre; a velocidad normal, a veces; los fps no influyen.
//   2. El lector de streaming (sub_82851F50) llena ese bufer por paquetes de
//      16 KB: destinos 0x0, 0x4000, 0x8000 y 0xC000.
//   3. Luego lo entrega al decodificador XMA (sub_82CC6248), que lee su primera
//      palabra, y otros lectores (sub_82CD2DC8...) tambien leen de 0x0.
//
// Con la pagina cero protegida (lo que trae el SDK) el juego muere en 2 o en 3.
// Con ella abierta (protect_zero = false, como la config de Xenia para este
// juego) ya no muere ahi, pero los 64 KB de audio se escriben EN LA PAGINA CERO,
// y cualquier codigo que lea una estructura nula recibe basura de sonido en vez
// de ceros, la toma por un puntero (0x10006610 en un informe real) y muere en
// otro hilo. Probablemente es lo mismo que le pasa a Xenia a 60 fps.
//
// El arreglo, mejor que el de Xenia: todo destino dentro de la pagina cero se
// redirige a un ESPEJO, un bufer real reservado en memoria FISICA (que es lo que
// necesita el decodificador XMA), tanto al escribir los paquetes como al
// entregarlos al XMA. Asi:
//   - ese sonido se reproduce con sus datos de verdad;
//   - la pagina cero se queda a ceros, que es lo que esperan los que leen
//     estructuras nulas;
//   - y protect_zero = false (por defecto en este port) evita que esas lecturas
//     de estructuras nulas cierren el juego.
// Ademas, cualquier otra copia de memoria del juego cuyo destino caiga en la
// pagina cero o en memoria que no se puede escribir se descarta, para que nada
// ensucie la pagina cero.

#include "guest_guards.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "lo_options.h"

REXCVAR_DEFINE_BOOL(lo_xact_bank_sink, true, "LostOdyssey/Estabilidad",
                    "Evita el cierre al cargar la escena posterior a Mack: reserva a ceros la zona "
                    "0x10000000 que XACT lee como si fuera un banco mientras el banco se carga");

REXCVAR_DEFINE_BOOL(lo_guard_bad_copy, true, "LostOdyssey/Estabilidad",
                    "Evita el cierre de la cinematica posterior a Mack: el bufer de sonido que el "
                    "juego usa con puntero nulo se redirige a un bufer real, y la pagina cero se "
                    "mantiene limpia");

REX_EXTERN(__imp__sub_82851F50);
REX_EXTERN(__imp__sub_82B7A0B0);
REX_EXTERN(__imp__sub_82CC6248);
REX_EXTERN(__imp__sub_82B60E90);

namespace {

constexpr uint32_t kPaginaCero = 0x10000u;  // lo que protege/abre protect_zero

// Por encima de esto esta todo lo normal del juego (su monton en 0x40000000 y el
// ejecutable en 0x82000000): solo se comprueba lo que cae por debajo.
constexpr uint32_t kZonaNormal = 0x40000000u;

// --- avisos ----------------------------------------------------------------

std::atomic<uint32_t> g_redirigidas{0};
std::atomic<uint32_t> g_entregas{0};
std::atomic<uint32_t> g_descartadas{0};
std::atomic<uint32_t> g_saneadas{0};

bool Avisar(std::atomic<uint32_t>& contador, uint32_t* veces) {
  const uint32_t n = contador.fetch_add(1, std::memory_order_relaxed) + 1;
  *veces = n;
  return n <= 16 || (n % 256) == 0;
}

// --- el espejo de la pagina cero ------------------------------------------

// 64 KB del bufer + margen por si un paquete se pasa del final.
constexpr uint32_t kEspejoTam = 2u * kPaginaCero;

std::once_flag g_espejo_once;
uint32_t g_espejo = 0;  // direccion guest (vista fisica de 64 KB)

// Se reserva en memoria FISICA para que MmGetPhysicalAddress y el decodificador
// XMA puedan usarlo como cualquier bufer de audio del juego.
uint32_t Espejo() {
  std::call_once(g_espejo_once, [] {
    auto* memoria = REX_KERNEL_MEMORY();
    auto* monton = memoria ? memoria->LookupHeapByType(true, 65536) : nullptr;
    uint32_t direccion = 0;
    if (monton == nullptr ||
        !monton->Alloc(kEspejoTam, 65536,
                       rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, true,
                       &direccion) ||
        direccion == 0) {
      REXLOG_ERROR("lo_guard: no se pudo reservar el espejo de la pagina cero");
      return;
    }
    memoria->Zero(direccion, kEspejoTam);
    g_espejo = direccion;
    REXLOG_INFO("lo_guard: espejo de la pagina cero en 0x{:08X} ({} KB, memoria fisica)",
                g_espejo, kEspejoTam >> 10);
  });
  return g_espejo;
}

// --- destinos imposibles ---------------------------------------------------

thread_local uint32_t g_cache_inicio = 0;
thread_local uint32_t g_cache_fin = 0;

bool DestinoEscribible(uint32_t direccion, uint32_t longitud) {
  if (g_cache_fin != 0 && direccion >= g_cache_inicio &&
      uint64_t(direccion) + longitud <= g_cache_fin) {
    return true;
  }
  auto* memoria = REX_KERNEL_MEMORY();
  if (memoria == nullptr) {
    return true;  // sin kernel no estorbamos
  }
  auto* monton = memoria->LookupHeap(direccion);
  rex::memory::HeapAllocationInfo info{};
  if (monton == nullptr || !monton->QueryRegionInfo(direccion, &info)) {
    return false;
  }
  // Comprometida no basta: tiene que tener permiso de escritura.
  if ((info.state & rex::memory::kMemoryAllocationCommit) == 0 ||
      (info.protect & rex::memory::kMemoryProtectWrite) == 0) {
    return false;
  }
  const uint32_t pagina = direccion & ~(monton->page_size() - 1);
  g_cache_inicio = pagina;
  g_cache_fin = pagina + info.region_size;
  return true;
}

bool PaginaCeroAccesibleSinCache() {
  auto* memoria = REX_KERNEL_MEMORY();
  auto* monton = memoria ? memoria->LookupHeap(0x1000) : nullptr;
  rex::memory::HeapAllocationInfo info{};
  if (monton == nullptr || !monton->QueryRegionInfo(0, &info)) {
    return false;
  }
  return (info.state & rex::memory::kMemoryAllocationCommit) != 0 &&
         (info.protect & rex::memory::kMemoryProtectRead) != 0 &&
         (info.protect & rex::memory::kMemoryProtectWrite) != 0;
}

bool PaginaCeroAccesible() {
  static const bool accesible = PaginaCeroAccesibleSinCache();
  return accesible;
}

// Legible = comprometida y con permiso de lectura (lo mismo que DestinoEscribible
// pero para leer).
bool GuestLegible(uint32_t direccion) {
  auto* memoria = REX_KERNEL_MEMORY();
  auto* monton = memoria ? memoria->LookupHeap(direccion) : nullptr;
  rex::memory::HeapAllocationInfo info{};
  if (monton == nullptr || !monton->QueryRegionInfo(direccion, &info)) {
    return false;
  }
  return (info.state & rex::memory::kMemoryAllocationCommit) != 0 &&
         (info.protect & rex::memory::kMemoryProtectRead) != 0;
}


uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Zona que XACT lee como si fuera un banco mientras el banco se carga (entradas
// de tipo 6 en estado 3: su +12 aun no es un puntero; visto 0x10001600,
// 0x10002601, 0x10006600, 0x10008600). Nadie reserva nunca ahi.
constexpr uint32_t kSumideroBancos = 0x10000000u;
constexpr uint32_t kSumideroTam = 0x00100000u;  // 1 MB

// Lo ultimo visto en cada entrada de tipo 6 de la tabla de XACT (solo lectura).
std::mutex g_tabla_mutex;
uint32_t g_tabla_vista[16] = {};
std::atomic<uint32_t> g_volcados{0};

}  // namespace

// VIGILANTE DE SOLO LECTURA (28-sep-2026) de la tabla de bancos de XACT: 16
// entradas de 20 bytes en r3+3220 (tipo en +0; en las de tipo 6, +12 apunta a un
// banco cargado en memoria: n. de bloques en +21, indice en +64). Tras el jefe
// Mack dos hilos mueren leyendo una entrada de tipo 6 que apunta a 0x10006600,
// memoria que no existe. Aqui NO se toca nada (una red que vaciaba entradas
// provoco un "error de disco"): solo se apunta cada cambio de las entradas de
// tipo 6 y, si alguna apunta a memoria inexistente, se vuelca la tabla entera.
REX_EXTERN(sub_82B60E90) {
  if (ctx.r3.u32 != 0) {
    const uint32_t tabla = ctx.r3.u32 + 3220;
    std::lock_guard<std::mutex> lock(g_tabla_mutex);
    bool mala = false;
    for (uint32_t i = 0; i < 16; ++i) {
      const uint8_t* e = base + tabla + i * 20;
      const uint32_t valor = e[0] == 6 ? Be32(e + 12) : 0;
      if (valor != g_tabla_vista[i]) {
        const bool legible = valor == 0 || GuestLegible(valor);
        const bool en_sumidero = valor >= kSumideroBancos && valor < kSumideroBancos + kSumideroTam;
        REXLOG_INFO("lo_xact: tabla 0x{:08X} entrada {} tipo 6 -> banco 0x{:08X} ({})", tabla, i,
                    valor,
                    en_sumidero ? "banco aun cargando: lee el sumidero a ceros"
                                : (legible ? "legible" : "NO EXISTE"));
        g_tabla_vista[i] = valor;
        mala = mala || !legible;
      }
    }
    if (mala && g_volcados.fetch_add(1) < 4) {
      for (uint32_t i = 0; i < 16; ++i) {
        const uint8_t* e = base + tabla + i * 20;
        REXLOG_WARN("lo_xact:   [{:2}] {:08X} {:08X} {:08X} {:08X} {:08X}", i, Be32(e),
                    Be32(e + 4), Be32(e + 8), Be32(e + 12), Be32(e + 16));
      }
    }
  }
  __imp__sub_82B60E90(ctx, base);
}

// Lector del bufer circular de streaming (r3 = objeto, r4 = destino,
// r5 = longitud, r6 = offset; recorta la longitud al tamano en +140).
REX_EXTERN(sub_82851F50) {
  // Destino 0 con poca longitud es el "lee sin copiar" legitimo del juego (se
  // salta el memcpy): se deja tal cual, y si algun trozo se escapa a la pagina
  // cero lo para el memcpy de abajo. Lo que se redirige es el bufer nulo de
  // XACT: paquetes de 16 KB o mas, o destinos 0x4000, 0x8000... dentro de ella.
  const bool paquete_nulo =
      ctx.r4.u32 < kPaginaCero && ctx.r5.u32 != 0 && (ctx.r4.u32 != 0 || ctx.r5.u32 >= 0x4000u);
  // Con la pagina cero ABIERTA no se redirige nada: el espejo (varias voces
  // compartiendo un solo bufer y entregandolo al XMA) hizo que el decodificador
  // del anfitrion reventase en VCRUNTIME (27-sep, 19:30). Solo actua si alguien
  // vuelve a protect_zero = true.
  if (REXCVAR_GET(lo_guard_bad_copy) && paquete_nulo && !PaginaCeroAccesible()) {
    if (const uint32_t espejo = Espejo(); espejo != 0) {
      const uint32_t desplazamiento = ctx.r4.u32;
      ctx.r4.u32 = espejo + desplazamiento;
      // Que ningun paquete se salga del espejo.
      const uint32_t cabe = kEspejoTam - desplazamiento;
      if (ctx.r5.u32 > cabe) {
        ctx.r5.u32 = cabe;
      }
      uint32_t veces = 0;
      if (Avisar(g_redirigidas, &veces)) {
        REXLOG_INFO(
            "lo_guard: paquete de streaming con destino 0x{:05X} (bufer nulo de XACT) #{} -> "
            "espejo 0x{:08X}, {} bytes",
            desplazamiento, veces, ctx.r4.u32, ctx.r5.u32);
      }
    }
  }
  __imp__sub_82851F50(ctx, base);
}

// Entrega de un bufer XMA al decodificador (r3 = objeto XMA, r4 = contexto,
// r5 = bufer, r6 = tamano). Su unico llamador (sub_82CD3D70, en 0x82CD4334) no
// mira el resultado.
REX_EXTERN(sub_82CC6248) {
  if (REXCVAR_GET(lo_guard_bad_copy) && ctx.r5.u32 < kPaginaCero && !PaginaCeroAccesible()) {
    const uint32_t espejo = Espejo();
    uint32_t veces = 0;
    const bool avisar = Avisar(g_entregas, &veces);
    if (espejo != 0) {
      const uint32_t desplazamiento = ctx.r5.u32;
      ctx.r5.u32 = espejo + desplazamiento;
      if (avisar) {
        REXLOG_INFO(
            "lo_guard: entrega XMA del bufer nulo #{} (contexto {}, {} bytes) -> espejo 0x{:08X}",
            veces, ctx.r4.u32, ctx.r6.u32, ctx.r5.u32);
      }
    } else {
      // Sin espejo no hay nada que entregar: se pierde ese sonido, no el juego.
      if (avisar) {
        REXLOG_WARN("lo_guard: entrega XMA del bufer nulo #{} sin espejo -> no se entrega",
                    veces);
      }
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_82CC6248(ctx, base);
}

// memcpy del XDK: descarta las copias que ensuciarian la pagina cero o que caen
// en memoria que no se puede escribir.
REX_EXTERN(sub_82B7A0B0) {
  if (REXCVAR_GET(lo_guard_bad_copy) && ctx.r3.u32 < kZonaNormal && ctx.r5.u32 != 0 &&
      !DestinoEscribible(ctx.r3.u32, ctx.r5.u32)) {
    uint32_t veces = 0;
    if (Avisar(g_descartadas, &veces)) {
      REXLOG_WARN(
          "lo_guard: copia descartada #{}: destino 0x{:08X} ({}), {} bytes, vuelve a 0x{:08X}",
          veces, ctx.r3.u32, "no escribible",
          ctx.r5.u32, uint32_t(ctx.lr));
    }
    return;  // memcpy devuelve el destino, que ya esta en r3
  }
  // Copia nativa: recompilado es un bucle de cargas y almacenamientos de 8 bytes
  // con cambio de orden, y el hilo de render lo usa para subir los vertices de
  // cada personaje animado por CPU (175 KB por personaje y fotograma; 10 % de ese
  // hilo en la primera batalla, muestreo del 2-oct-2026). Es una copia byte a
  // byte, asi que vale un memmove sobre la memoria del guest. Misma traduccion
  // que REX_RAW_ADDR: las direcciones fisicas desde 0xE0000000 llevan 4 KB extra
  // en Windows.
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (n) {
#if defined(_WIN32)
    auto host = [base](uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); };
#else
    auto host = [base](uint32_t a) { return base + a; };
#endif
    std::memmove(host(dst), host(src), n);
  }
  ctx.r3.u64 = dst;
}

namespace lo {

void ApplyZeroPageCompat(const std::filesystem::path& config_path) {
  // Si el usuario ya ha elegido algo en el toml, se respeta.
  std::ifstream fichero(config_path);
  std::stringstream texto;
  texto << fichero.rdbuf();
  std::string linea;
  while (std::getline(texto, linea)) {
    const auto inicio = linea.find_first_not_of(" \t");
    if (inicio != std::string::npos && linea.compare(inicio, 12, "protect_zero") == 0) {
      return;
    }
  }
  // Las lecturas de estructuras nulas (que las hay en la escena de Mack) leen
  // ceros en vez de cerrar el juego. Xenia lo trae igual para este juego.
  rex::cvar::SetFlagByName("protect_zero", "false");
  SetTomlValue(config_path, "protect_zero", "false");
  REXLOG_INFO("lostodyssey: protect_zero = false por defecto (compatibilidad, como en Xenia)");
}

void LogZeroPageState() {
  REXLOG_INFO("lo_guard: pagina cero {}",
              PaginaCeroAccesible() ? "accesible (protect_zero = false)"
                                    : "PROTEGIDA (protect_zero = true)");
}

}  // namespace lo

namespace lo {

// Ver kSumideroBancos. Al leer ahi, XACT ve un banco vacio (0 bloques en +21) y
// lo salta; cuando el banco termina de cargarse, su entrada pasa a tener el
// puntero real y todo sigue normal. No se toca la tabla de XACT.
void ReserveXactBankSink() {
  if (!REXCVAR_GET(lo_xact_bank_sink)) {
    return;
  }
  auto* memoria = REX_KERNEL_MEMORY();
  auto* monton = memoria ? memoria->LookupHeap(kSumideroBancos) : nullptr;
  if (monton == nullptr ||
      !monton->AllocFixed(kSumideroBancos, kSumideroTam, monton->page_size(),
                          rex::memory::kMemoryAllocationReserve |
                              rex::memory::kMemoryAllocationCommit,
                          rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) {
    REXLOG_ERROR("lo_guard: no se pudo reservar el sumidero de bancos de XACT en 0x{:08X}",
                 kSumideroBancos);
    return;
  }
  memoria->Zero(kSumideroBancos, kSumideroTam);
  REXLOG_INFO("lo_guard: sumidero de bancos de XACT en 0x{:08X}-0x{:08X} (a ceros)",
              kSumideroBancos, kSumideroBancos + kSumideroTam - 1);
}

}  // namespace lo
