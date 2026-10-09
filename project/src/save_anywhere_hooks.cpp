// lostodyssey - ReXGlue Recompiled Project
//
// Guardar en cualquier sitio (calidad de vida, cvar lo_save_anywhere).
//
// El menu System del juego guarda sus filas en una tabla de entradas de 28 bytes
// que empieza en 0x8326D690; la primera es "Guardar" (+4 = ID 34). Cada fila
// lleva en +0 dos bits: 0x80000000 visible y 0x40000000 habilitada, y el juego
// solo habilita "Guardar" estando en un punto de guardado. Con la opcion activa
// se fuerza el bit de habilitada. El guardado es el del propio juego (misma
// pantalla de ranuras, mismo fichero): no se finge ningun punto de guardado.
//
// Las direcciones salen de la investigacion publicada por el proyecto
// freefrank/LostOdysseyRecomp (docs/notes/save-anywhere.md); este codigo es
// propio. Las dos funciones y la tabla estan en las mismas direcciones en esta
// build (verificado en el codigo generado: sub_82876EA8 recorre las tablas de
// 28 bytes desde 0x8326D690 comparando el ID con r4).
//
// Se envuelven dos funciones con el patron weak alias del SDK (definir sub_X y
// llamar a __imp__sub_X):
//   sub_82876EA8  setter nativo de visible/habilitada (r4 = ID de la fila). Al
//                 volver se anota el permiso que acaba de poner el juego y se
//                 reaplica el nuestro, antes de que el menu copie la tabla a sus
//                 widgets.
//   sub_822E0E10  tarea del menu: aplica la opcion en cada frame, asi un cambio
//                 hecho en el F2 se nota al volver a abrir System.
//
// El juego no esta pensado para guardar a mitad de una escena o de un evento:
// la opcion es para exploracion.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(lo_save_anywhere, false, "LostOdyssey/QoL",
                    "Guardar en cualquier sitio: habilita Guardar en el menu System fuera de los "
                    "puntos de guardado (se nota al volver a abrir el menu)");

REX_EXTERN(__imp__sub_82876EA8);
REX_EXTERN(__imp__sub_822E0E10);

namespace {

constexpr uint32_t kSaveRow = 0x8326D690;  // primera fila de la tabla del menu System
constexpr int32_t kSaveItemId = 34;
constexpr uint32_t kRowVisible = 0x80000000u;
constexpr uint32_t kRowEnabled = 0x40000000u;

// Solo se usan desde el hilo del menu del juego (dentro de sus funciones).
bool g_native_known = false;
uint32_t g_native_enabled = 0;  // bit de habilitada tal y como lo dejo el juego
uint32_t g_last_written = 0;    // valor de la fila tras nuestra ultima pasada

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

void ApplySaveAnywhere(bool game_just_wrote) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) {
    return;
  }
  uint8_t* row = memory->TranslateVirtual<uint8_t*>(kSaveRow);
  // Hasta que el juego construye el menu la tabla esta vacia: no tocar nada.
  if (!row || LoadBe32(row + 4) != uint32_t(kSaveItemId)) {
    return;
  }
  const uint32_t flags = LoadBe32(row);
  // El permiso nativo cambia cuando el juego lo escribe (al entrar o salir de un
  // punto de guardado). Se nota por la llamada al setter, o porque la fila ya no
  // tiene el valor que dejamos nosotros.
  if (!g_native_known || game_just_wrote || flags != g_last_written) {
    g_native_enabled = flags & kRowEnabled;
    g_native_known = true;
  }
  // Solo se fuerza sobre una fila visible; al apagar la opcion se devuelve el
  // bit que puso el juego, asi en un punto de guardado "Guardar" sigue activo.
  const bool force = REXCVAR_GET(lo_save_anywhere) && (flags & kRowVisible);
  const uint32_t wanted = (flags & ~kRowEnabled) | (force ? kRowEnabled : g_native_enabled);
  if (wanted != flags) {
    StoreBe32(row, wanted);
    if (force && !g_native_enabled) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_INFO("[save_anywhere] Guardar habilitado fuera de un punto de guardado");
      }
    }
  }
  g_last_written = wanted;
}

}  // namespace

REX_EXTERN(sub_82876EA8) {
  const bool save_row = ctx.r4.s32 == kSaveItemId;
  __imp__sub_82876EA8(ctx, base);
  if (save_row) {
    ApplySaveAnywhere(true);
  }
}

REX_EXTERN(sub_822E0E10) {
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("[save_anywhere] envoltorio de la tarea del menu activo");
  }
  ApplySaveAnywhere(false);
  __imp__sub_822E0E10(ctx, base);
}
