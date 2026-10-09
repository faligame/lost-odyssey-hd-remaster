// lostodyssey - ReXGlue Recompiled Project
//
// Truco "sin batallas aleatorias" (en investigacion, 28-sep-2026).
//
// Paso 1 (este): averiguar quien decide lanzar un combate. Se envuelven
// funciones que solo corren al preparar un combate y se escribe en el log la
// cadena de llamadas del juego (lo_pila) las primeras veces. Con un solo
// combate aleatorio, esa cadena lleva desde el mapa hasta el combate.
//
// Funciones de preparacion de combate (de la investigacion publicada por
// freefrank/LostOdysseyRecomp, docs/notes/encounter-animation.md; el codigo es
// propio): 82AF5D18 y 82AF6290 escriben la configuracion de los personajes del
// combate (+0x48), 82B00398 convierte sus recursos, 82AB9E08 la lee.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "crash_handler.h"

REXCVAR_DEFINE_BOOL(lo_trace_battle_start, false, "LostOdyssey/Diagnostico",
                    "Escribe en el log quien llama a la preparacion de cada combate (para el truco "
                    "de batallas aleatorias)");

REX_EXTERN(__imp__sub_82AF5D18);
REX_EXTERN(__imp__sub_82AF6290);
REX_EXTERN(__imp__sub_82B00398);
REX_EXTERN(__imp__sub_82AB9E08);
REX_EXTERN(__imp__sub_82388BE0);
REX_EXTERN(__imp__sub_82AD20C0);
REX_EXTERN(__imp__sub_82AD5920);
REX_EXTERN(__imp__sub_82A6E510);
REX_EXTERN(__imp__sub_82293FF8);

namespace {

void Rastrear(const char* nombre, std::atomic<uint32_t>& veces) {
  if (!REXCVAR_GET(lo_trace_battle_start)) {
    return;
  }
  const uint32_t n = veces.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > 6) {
    return;
  }
  const std::string etiqueta = std::string(nombre) + " #" + std::to_string(n);
  lo::LogGuestStack(etiqueta.c_str());
}

std::atomic<uint32_t> g_5d18{0};
std::atomic<uint32_t> g_6290{0};
std::atomic<uint32_t> g_0398{0};
std::atomic<uint32_t> g_9e08{0};

// --- Pelicula del gestor del mapa --------------------------------------------
// sub_82388BE0 es el tick del objeto que gestiona la escena del mapa (r3, campos
// hasta +5724; recibe el tiempo en f1) y es quien lanza, por una llamada
// indirecta, la tarea del combate (sub_82AD20C0). Se guarda su contenido cada
// 100 ms; al empezar un combate se compara con como estaba antes para ver que
// campos cambiaron justo antes (contador de pasos, aviso de "toca combate"...).
constexpr uint32_t kTamGestor = 0x1800;  // 6 KB (usa campos hasta +5724)
constexpr int kFotos = 40;               // 4 s a 100 ms
struct Foto {
  std::chrono::steady_clock::time_point cuando;
  std::array<uint8_t, kTamGestor> datos;
};
std::mutex g_peli_mutex;
std::array<Foto, kFotos> g_fotos;
int g_foto_sig = 0;
int g_fotos_llenas = 0;
uint32_t g_gestor = 0;
std::atomic<uint32_t> g_combates{0};

bool Legible(uint32_t direccion) {
  auto* memoria = REX_KERNEL_MEMORY();
  auto* monton = memoria ? memoria->LookupHeap(direccion) : nullptr;
  rex::memory::HeapAllocationInfo info{};
  if (monton == nullptr || !monton->QueryRegionInfo(direccion, &info)) return false;
  return (info.state & rex::memory::kMemoryAllocationCommit) != 0 &&
         (info.protect & rex::memory::kMemoryProtectRead) != 0;
}

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

const Foto* FotoHace(std::chrono::milliseconds hace, std::chrono::steady_clock::time_point ahora) {
  const Foto* mejor = nullptr;
  for (int i = 0; i < g_fotos_llenas; ++i) {
    const Foto& f = g_fotos[i];
    if (ahora - f.cuando >= hace && (mejor == nullptr || f.cuando > mejor->cuando)) mejor = &f;
  }
  return mejor;
}


}  // namespace

REX_EXTERN(sub_82AF5D18) {
  Rastrear("combate 82AF5D18", g_5d18);
  __imp__sub_82AF5D18(ctx, base);
}

REX_EXTERN(sub_82AF6290) {
  Rastrear("combate 82AF6290", g_6290);
  __imp__sub_82AF6290(ctx, base);
}

REX_EXTERN(sub_82B00398) {
  Rastrear("combate 82B00398", g_0398);
  __imp__sub_82B00398(ctx, base);
}

REX_EXTERN(sub_82AB9E08) {
  Rastrear("combate 82AB9E08", g_9e08);
  __imp__sub_82AB9E08(ctx, base);
}

REX_EXTERN(sub_82388BE0) {
  if (REXCVAR_GET(lo_trace_battle_start) && ctx.r3.u32 != 0) {
    std::lock_guard<std::mutex> lock(g_peli_mutex);
    const auto ahora = std::chrono::steady_clock::now();
    if (g_gestor != ctx.r3.u32) {
      if (Legible(ctx.r3.u32) && Legible(ctx.r3.u32 + kTamGestor - 1)) {
        g_gestor = ctx.r3.u32;
        g_fotos_llenas = 0;
        g_foto_sig = 0;
        REXLOG_INFO("lo_enc: gestor del mapa en 0x{:08X}", g_gestor);
      }
    }
    if (g_gestor == ctx.r3.u32) {
      const int anterior = (g_foto_sig + kFotos - 1) % kFotos;
      if (g_fotos_llenas == 0 || ahora - g_fotos[anterior].cuando >= std::chrono::milliseconds(100)) {
        Foto& f = g_fotos[g_foto_sig];
        f.cuando = ahora;
        std::memcpy(f.datos.data(), base + g_gestor, kTamGestor);
        g_foto_sig = (g_foto_sig + 1) % kFotos;
        g_fotos_llenas = std::min(g_fotos_llenas + 1, kFotos);
      }
    }
  }
  __imp__sub_82388BE0(ctx, base);
}

// La tarea del combate (metodo virtual al que salta el tick del mapa).
REX_EXTERN(sub_82AD20C0) {
  // Puede llamarse cada fotograma durante el combate: solo cuenta como combate
  // nuevo si hace mas de 2 s de la llamada anterior.
  static std::chrono::steady_clock::time_point ultima{};
  const auto ahora_llamada = std::chrono::steady_clock::now();
  const bool nuevo_combate = ahora_llamada - ultima > std::chrono::seconds(2);
  ultima = ahora_llamada;
  if (REXCVAR_GET(lo_trace_battle_start) && nuevo_combate && g_combates.fetch_add(1) < 6) {
    const uint32_t objeto = ctx.r3.u32;
    const uint32_t tabla = objeto ? Be32(base + objeto) : 0;
    REXLOG_INFO("lo_enc: COMBATE: tarea 82AD20C0 llamada desde 0x{:08X}, objeto 0x{:08X}, vtable "
                "0x{:08X}, gestor 0x{:08X}",
                uint32_t(ctx.lr), objeto, tabla, g_gestor);
    std::lock_guard<std::mutex> lock(g_peli_mutex);
    if (g_gestor != 0 && g_fotos_llenas > 0) {
      const auto ahora = std::chrono::steady_clock::now();
      const Foto* f3 = FotoHace(std::chrono::milliseconds(3000), ahora);
      const Foto* f1 = FotoHace(std::chrono::milliseconds(1000), ahora);
      const Foto* f03 = FotoHace(std::chrono::milliseconds(300), ahora);
      const uint8_t* ya = base + g_gestor;
      int lineas = 0;
      for (uint32_t off = 0; off < kTamGestor && lineas < 80; off += 4) {
        const uint32_t v_ya = Be32(ya + off);
        const uint32_t v3 = f3 ? Be32(f3->datos.data() + off) : v_ya;
        const uint32_t v1 = f1 ? Be32(f1->datos.data() + off) : v_ya;
        const uint32_t v03 = f03 ? Be32(f03->datos.data() + off) : v_ya;
        if (v3 == v_ya && v1 == v_ya && v03 == v_ya) continue;
        REXLOG_INFO("lo_enc:   +{:5} (0x{:04X}): hace3s {:08X}  hace1s {:08X}  hace0.3s {:08X}  ahora {:08X}",
                    off, off, v3, v1, v03, v_ya);
        ++lineas;
      }
    }
  }
  __imp__sub_82AD20C0(ctx, base);
}

// PETICION DE COMBATE. sub_82AAFA40 es la UNICA funcion que pone el estado del
// gestor del combate (0x832CA0E0 + 5588) a 1 = "combate pedido: cargar y
// arrancar" (en el tick sub_82388BE0 el estado 1 va al caso 0 del switch). Su
// unico llamador es sub_82AD5920, que nadie llama directamente: se invoca de
// forma indirecta desde el objeto que dispara el combate (r3; copia su +1044,
// probablemente el grupo de enemigos, a una global y marca bits en +1092). Aqui
// se apunta quien la llama, para separar combates de guion y aleatorios.
std::atomic<uint32_t> g_peticiones{0};
REX_EXTERN(sub_82AD5920) {
  if (REXCVAR_GET(lo_trace_battle_start)) {
    const uint32_t n = g_peticiones.fetch_add(1) + 1;
    if (n <= 10) {
      const uint32_t objeto = ctx.r3.u32;
      const uint32_t tabla = objeto ? Be32(base + objeto) : 0;
      const uint32_t grupo = objeto ? Be32(base + objeto + 1044) : 0;
      const uint32_t banderas = objeto ? Be32(base + objeto + 1092) : 0;
      REXLOG_INFO(
          "lo_enc: PETICION DE COMBATE #{}: desde 0x{:08X}, objeto 0x{:08X} (vtable 0x{:08X}), "
          "+1044 = 0x{:08X} ({}), +1092 = 0x{:08X}",
          n, uint32_t(ctx.lr), objeto, tabla, grupo, grupo, banderas);
      const std::string etiqueta = "peticion de combate #" + std::to_string(n);
      lo::LogGuestStack(etiqueta.c_str());
    }
  }
  __imp__sub_82AD5920(ctx, base);
}

// --- Nombres de UnrealScript (FName) ------------------------------------------
// Tabla global de nombres del motor en 0x833690D0 (TArray: datos, cuenta, max;
// cada dato apunta a una entrada con el texto en +16; de la investigacion
// publicada por freefrank/LostOdysseyRecomp, docs/notes/debug-map-info.md).
namespace {

constexpr uint32_t kTablaNombres = 0x833690D0u;

std::string NombreFName(uint8_t* base, uint32_t indice) {
  const uint32_t datos = Be32(base + kTablaNombres);
  const uint32_t cuenta = Be32(base + kTablaNombres + 4);
  if (datos == 0 || indice >= cuenta || cuenta > 2000000 || !Legible(datos + indice * 4)) {
    return {};
  }
  const uint32_t entrada = Be32(base + datos + indice * 4);
  if (entrada == 0 || !Legible(entrada + 16)) {
    return {};
  }
  // El texto va en UTF-16 big-endian ("None", "ByteProperty"...).
  std::string texto;
  for (uint32_t i = 0; i < 64; ++i) {
    const uint32_t off = entrada + 16 + i * 2;
    const uint16_t c = uint16_t((base[off] << 8) | base[off + 1]);
    if (c == 0) break;
    if (c < 32 || c > 126) return {};
    texto.push_back(char(c));
  }
  return texto;
}

// Prueba en que desplazamiento de un UObject esta su nombre: se prueba cada
// palabra de +0x10 a +0x40 como indice de nombre y se apunta lo que sale.
std::string CandidatosNombre(uint8_t* base, uint32_t objeto) {
  std::string salida;
  if (objeto == 0 || !Legible(objeto) || !Legible(objeto + 0x44)) return "(no legible)";
  for (uint32_t off = 0x10; off <= 0x40; off += 4) {
    const std::string n = NombreFName(base, Be32(base + objeto + off));
    if (!n.empty()) salida += " +0x" + std::to_string(off) + "=" + n;  // off en decimal
  }
  return salida;
}

}  // namespace

namespace lo {

void EncounterNameSelfTest(uint8_t* base) {
  const uint32_t datos = Be32(base + kTablaNombres);
  const uint32_t cuenta = Be32(base + kTablaNombres + 4);
  REXLOG_INFO("lo_nombres: tabla en 0x{:08X}, {} nombres; 0='{}' 1='{}' 2='{}' 100='{}'", datos,
              cuenta, NombreFName(base, 0), NombreFName(base, 1), NombreFName(base, 2),
              NombreFName(base, 100));
  for (uint32_t i = 0; i < 4 && datos != 0; ++i) {
    const uint32_t entrada = Be32(base + datos + i * 4);
    std::string hex;
    if (entrada != 0 && Legible(entrada) && Legible(entrada + 47)) {
      for (uint32_t k = 0; k < 48; ++k) {
        char b[4];
        std::snprintf(b, sizeof(b), "%02X", base[entrada + k]);
        hex += b;
        if (k % 4 == 3) hex += ' ';
      }
    }
    REXLOG_INFO("lo_nombres: entrada {} en 0x{:08X}: {}", i, entrada, hex);
  }
  const uint32_t motor = Be32(base + 0x83315FB4u);
  REXLOG_INFO("lo_nombres: GEngine 0x{:08X}:{}", motor, CandidatosNombre(base, motor));
  // la clase del objeto: probar cada palabra como puntero a otro UObject con nombre
  if (motor != 0 && Legible(motor + 0x44)) {
    for (uint32_t off = 0x10; off <= 0x40; off += 4) {
      const uint32_t p = Be32(base + motor + off);
      if (p >= 0x40000000u && p < 0x90000000u && Legible(p) && Legible(p + 0x44)) {
        const std::string c = CandidatosNombre(base, p);
        if (!c.empty() && c != "(no legible)") {
          REXLOG_INFO("lo_nombres:   GEngine+0x{:X} -> 0x{:08X}:{}", off, p, c);
        }
      }
    }
  }
}

}  // namespace lo

// Disposicion de UObject en este juego (medida con GEngine = rpGameEngine):
// +0x28 Outer, +0x2C Name (indice FName), +0x34 Class, +0x38 ObjectArchetype.
namespace {

std::string NombreDe(uint8_t* base, uint32_t objeto) {
  if (objeto == 0 || !Legible(objeto) || !Legible(objeto + 0x3C)) return "?";
  const std::string n = NombreFName(base, Be32(base + objeto + 0x2C));
  return n.empty() ? "?" : n;
}

std::string ClaseDe(uint8_t* base, uint32_t objeto) {
  if (objeto == 0 || !Legible(objeto) || !Legible(objeto + 0x3C)) return "?";
  return NombreDe(base, Be32(base + objeto + 0x34));
}

// "Clase.funcion" de un UFunction: su Outer es la clase (o el estado) donde vive.
std::string FuncionDe(uint8_t* base, uint32_t funcion) {
  if (funcion == 0 || !Legible(funcion) || !Legible(funcion + 0x3C)) return "?";
  return NombreDe(base, Be32(base + funcion + 0x28)) + "." + NombreDe(base, funcion);
}

std::atomic<uint32_t> g_nativa{0};

}  // namespace

// --- Registro de llamadas de UnrealScript -------------------------------------
// sub_82293FF8 = UObject::CallFunction(this = r3, FFrame = r4, Result = r5,
// UFunction = r6): por aqui pasa toda llamada de una funcion de script a otra (y
// a las nativas). Se guarda "objeto + funcion" en un anillo, sin resolver nada
// (cuesta dos palabras); al pedir combate se vuelcan las ultimas con nombres.
namespace {

constexpr uint32_t kAnillo = 65536;  // ~10-20 s de script
struct Llamada {
  uint32_t objeto;
  uint32_t funcion;
  uint32_t ms;  // milisegundos desde el arranque
};
Llamada g_anillo[kAnillo];
std::atomic<uint32_t> g_anillo_pos{0};
const auto g_arranque = std::chrono::steady_clock::now();

uint32_t AhoraMs() {
  return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - g_arranque)
                      .count());
}

// Vuelca el anillo entero a logs\script_combate_<n>.txt (con los nombres en
// cache: se repiten muchisimo), de la llamada mas antigua a la mas reciente.
void VolcarUltimasLlamadas(uint8_t* base, uint32_t numero) {
  const uint32_t fin = g_anillo_pos.load(std::memory_order_relaxed);
  const uint32_t desde = fin > kAnillo ? fin - kAnillo : 0;
  std::unordered_map<uint32_t, std::string> funciones;
  std::unordered_map<uint32_t, std::string> objetos;
  auto funcion = [&](uint32_t f) -> const std::string& {
    auto it = funciones.find(f);
    if (it == funciones.end()) it = funciones.emplace(f, FuncionDe(base, f)).first;
    return it->second;
  };
  auto objeto = [&](uint32_t o) -> const std::string& {
    auto it = objetos.find(o);
    if (it == objetos.end()) it = objetos.emplace(o, NombreDe(base, o) + " de clase " + ClaseDe(base, o)).first;
    return it->second;
  };
  const std::string ruta = "logs/script_combate_" + std::to_string(numero) + ".txt";
  FILE* f = std::fopen(ruta.c_str(), "w");
  if (f == nullptr) return;
  const uint32_t ms_fin = AhoraMs();
  uint32_t i = desde;
  while (i < fin) {
    const Llamada l = g_anillo[i % kAnillo];
    uint32_t repes = 1;
    while (i + repes < fin && g_anillo[(i + repes) % kAnillo].funcion == l.funcion &&
           g_anillo[(i + repes) % kAnillo].objeto == l.objeto) {
      ++repes;
    }
    std::fprintf(f, "%8d ms  %s  [%s]%s\n", int(l.ms) - int(ms_fin), funcion(l.funcion).c_str(),
                 objeto(l.objeto).c_str(), repes > 1 ? (" x" + std::to_string(repes)).c_str() : "");
    i += repes;
  }
  std::fclose(f);
  REXLOG_INFO("lo_script: {} llamadas de script volcadas en {}", fin - desde, ruta);
}

}  // namespace

// Funciones de script en las que se apunta ademas la cadena NATIVA (lo_pila):
// el desmontaje del mapa de campo al viajar al combate (Actor.Destroyed de un
// fcAIController) y el arranque del mapa nuevo (GameInfo.InitGame). Se resuelve
// el nombre de cada UFunction una sola vez (cache).
std::mutex g_vigiladas_mutex;
std::unordered_map<uint32_t, uint8_t> g_vigiladas;  // 0 normal, 1 Destroyed, 2 InitGame
std::chrono::steady_clock::time_point g_ultima_pila[3];

REX_EXTERN(sub_82293FF8) {
  if (REXCVAR_GET(lo_trace_battle_start)) {
    const uint32_t n = g_anillo_pos.fetch_add(1, std::memory_order_relaxed);
    g_anillo[n % kAnillo] = Llamada{ctx.r3.u32, ctx.r6.u32, AhoraMs()};
    uint8_t tipo = 0;
    {
      std::lock_guard<std::mutex> lock(g_vigiladas_mutex);
      auto it = g_vigiladas.find(ctx.r6.u32);
      if (it == g_vigiladas.end()) {
        const std::string nombre = FuncionDe(base, ctx.r6.u32);
        tipo = nombre == "Actor.Destroyed" ? 1 : (nombre == "GameInfo.InitGame" ? 2 : 0);
        g_vigiladas.emplace(ctx.r6.u32, tipo);
      } else {
        tipo = it->second;
      }
    }
    if (tipo == 1 && ClaseDe(base, ctx.r3.u32) != "fcAIController") {
      tipo = 0;
    }
    if (tipo != 0) {
      const auto ahora = std::chrono::steady_clock::now();
      if (ahora - g_ultima_pila[tipo] > std::chrono::seconds(3)) {
        g_ultima_pila[tipo] = ahora;
        lo::LogGuestStack(tipo == 1 ? "desmontaje del mapa (Actor.Destroyed de fcAIController)"
                                    : "arranque de mapa (GameInfo.InitGame)");
      }
    }
  }
  __imp__sub_82293FF8(ctx, base);
}

// La nativa de UnrealScript que pide el combate (thunk exec: r3 = objeto, r4 =
// FFrame del script que la llama; su +784 de la vtable es sub_82AD5920). Se
// apunta la pila DE SCRIPT (FFrame: +4 Node = funcion, +8 Object, +12 Code,
// +20 PreviousFrame) para distinguir combates de guion y aleatorios.
REX_EXTERN(sub_82A6E510) {
  if (REXCVAR_GET(lo_trace_battle_start) && g_nativa.fetch_add(1) < 10) {
    const uint32_t objeto = ctx.r3.u32;
    if (g_nativa.load() <= 4) {
      VolcarUltimasLlamadas(base, g_nativa.load());
    }
    REXLOG_INFO("lo_script: PETICION DE COMBATE: objeto 0x{:08X} '{}' de clase '{}'", objeto,
                NombreDe(base, objeto), ClaseDe(base, objeto));
    uint32_t marco = ctx.r4.u32;
    for (int nivel = 0; nivel < 10 && marco != 0 && Legible(marco) && Legible(marco + 24); ++nivel) {
      const uint32_t funcion = Be32(base + marco + 4);
      const uint32_t quien = Be32(base + marco + 8);
      REXLOG_INFO("lo_script:   [{}] {}  (objeto '{}' de clase '{}')", nivel,
                  FuncionDe(base, funcion), NombreDe(base, quien), ClaseDe(base, quien));
      const uint32_t anterior = Be32(base + marco + 20);
      if (anterior == marco) break;
      marco = anterior;
    }
  }
  __imp__sub_82A6E510(ctx, base);
}

// --- Peticiones de viaje de mapa ----------------------------------------------
// El combate es OTRO MAPA: se viaja a el. El gestor de cambios de mapa (global
// 0x832631F0, lo procesan sub_8231F5E0/sub_8231FBF8 en cada tick) tiene seis
// huecos de peticion de 88 bytes en +3168 + i*88: +0 pendiente (1 = pedido),
// +4 FString con el mapa pedido, +16/+20 opciones. Cada funcion de abajo rellena
// un hueco fijo. Se envuelven todas y se apunta que hueco cambia, el mapa
// pedido y la cadena nativa, para saber cual es la del encuentro aleatorio.
namespace {

constexpr uint32_t kGestorViajes = 0x832631F0u;
constexpr uint32_t kHuecoViaje0 = 3168;
constexpr uint32_t kTamHuecoViaje = 88;

// FString de UE3 (TArray<TCHAR>: datos, cuenta, max; TCHAR = UTF-16 big-endian).
std::string LeerFString(uint8_t* base, uint32_t direccion) {
  if (!Legible(direccion) || !Legible(direccion + 8)) return "?";
  const uint32_t datos = Be32(base + direccion);
  const uint32_t cuenta = Be32(base + direccion + 4);
  if (datos == 0 || cuenta == 0) return "";
  if (cuenta > 512 || !Legible(datos) || !Legible(datos + cuenta * 2 - 1)) return "?";
  std::string texto;
  for (uint32_t i = 0; i < cuenta; ++i) {
    const uint16_t c = uint16_t((base[datos + i * 2] << 8) | base[datos + i * 2 + 1]);
    if (c == 0) break;
    texto.push_back(c >= 32 && c < 127 ? char(c) : '.');
  }
  return texto;
}

void Pendientes(uint8_t* base, uint32_t (&salida)[6]) {
  for (uint32_t i = 0; i < 6; ++i) {
    salida[i] = Be32(base + kGestorViajes + kHuecoViaje0 + i * kTamHuecoViaje);
  }
}

template <typename F>
void EnvolverViaje(const char* nombre, std::atomic<uint32_t>& veces, PPCContext& ctx,
                   uint8_t* base, F&& original) {
  if (!REXCVAR_GET(lo_trace_battle_start)) {
    original();
    return;
  }
  const uint32_t desde = uint32_t(ctx.lr);
  const uint32_t arg3 = ctx.r3.u32, arg4 = ctx.r4.u32;
  uint32_t antes[6], despues[6];
  Pendientes(base, antes);
  original();
  Pendientes(base, despues);
  bool cambio = false;
  for (uint32_t i = 0; i < 6; ++i) cambio |= antes[i] != despues[i];
  // Las llamadas que no piden nada solo se apuntan las 3 primeras veces.
  static std::atomic<uint32_t> sin_cambio{0};
  if (!cambio && sin_cambio.fetch_add(1, std::memory_order_relaxed) >= 3) return;
  const uint32_t n = veces.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > 40) return;
  std::string cambios;
  for (uint32_t i = 0; i < 6; ++i) {
    if (antes[i] != despues[i]) {
      const uint32_t hueco = kGestorViajes + kHuecoViaje0 + i * kTamHuecoViaje;
      cambios += " | hueco " + std::to_string(i) + ": " + std::to_string(antes[i]) + "->" +
                 std::to_string(despues[i]) + " mapa '" + LeerFString(base, hueco + 4) +
                 "' opciones '" + LeerFString(base, hueco + 20) + "'";
    }
  }
  REXLOG_INFO("lo_viaje: {} #{} desde 0x{:08X} (r3 0x{:08X}, r4 0x{:08X}) -> r3 {}{}", nombre, n,
              desde, arg3, arg4, int32_t(ctx.r3.u32), cambios.empty() ? " | sin cambios" : cambios);
  if (!cambios.empty()) {
    const std::string etiqueta = std::string("viaje ") + nombre + " #" + std::to_string(n);
    lo::LogGuestStack(etiqueta.c_str());
  }
}

}  // namespace

#define LO_ENVOLVER_VIAJE(dir)                                                   \
  REX_EXTERN(__imp__sub_##dir);                                                  \
  namespace {                                                                    \
  std::atomic<uint32_t> g_viaje_##dir{0};                                        \
  }                                                                              \
  REX_EXTERN(sub_##dir) {                                                        \
    EnvolverViaje(#dir, g_viaje_##dir, ctx, base, [&] { __imp__sub_##dir(ctx, base); }); \
  }

LO_ENVOLVER_VIAJE(82827580)
LO_ENVOLVER_VIAJE(82827690)
LO_ENVOLVER_VIAJE(82827798)
LO_ENVOLVER_VIAJE(828278A0)
LO_ENVOLVER_VIAJE(82827B58)
LO_ENVOLVER_VIAJE(82829460)
LO_ENVOLVER_VIAJE(82828698)
LO_ENVOLVER_VIAJE(82397198)
LO_ENVOLVER_VIAJE(82399088)
LO_ENVOLVER_VIAJE(823A0498)
LO_ENVOLVER_VIAJE(828C5440)
LO_ENVOLVER_VIAJE(828E1120)
LO_ENVOLVER_VIAJE(828FB510)
LO_ENVOLVER_VIAJE(828FFF78)
LO_ENVOLVER_VIAJE(8298D0C8)
LO_ENVOLVER_VIAJE(82D390A8)
LO_ENVOLVER_VIAJE(82D414B0)
LO_ENVOLVER_VIAJE(82D613A8)
LO_ENVOLVER_VIAJE(82D62E28)
