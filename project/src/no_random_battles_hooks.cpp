// lostodyssey - ReXGlue Recompiled Project
//
// Truco "sin batallas aleatorias" (cvar lo_no_random_battles).
//
// sub_829E3048 es el contador de encuentros aleatorios del controlador del
// jugador en el campo (r3 = controlador; +2252 = pawn). En cada tick, si la zona
// admite encuentros, mide lo que se ha movido el pawn desde el tick anterior
// (posicion guardada en pawn+11568), lo suma a un acumulado en +1616 (float) y,
// cuando el acumulado pasa del umbral de la zona, lo pone a cero, tira los dados
// (metodos virtuales +1644 y +1636) y, si sale combate, pide el viaje al mapa de
// combate con sub_828278A0 (hueco 1 de la cola de viajes) y devuelve 1.
//
// Los combates de guion NO pasan por aqui (los pide sub_82A4CC70), asi que no se
// tocan: medido con 1 combate de guion y 2 aleatorios en la misma sesion.
//
// Con el truco activo se pone el acumulado a un numero negativo enorme antes de
// llamar al original y se restaura despues: nunca llega al umbral, pero la
// funcion sigue haciendo todo lo demas (guardar la posicion del pawn, el
// contador global de distancia). Al quitar el truco el acumulado sigue donde
// estaba y la posicion esta al dia, asi que no salta un combate de golpe.
//
// Investigacion propia (hooks lo_viaje de encounter_hooks.cpp, 29-sep-2026).

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(lo_no_random_battles, false, "LostOdyssey/Trucos",
                    "Sin batallas aleatorias: el contador de encuentros no avanza (los combates "
                    "de guion siguen saliendo)");

REX_EXTERN(__imp__sub_829E3048);

namespace {

constexpr uint32_t kAcumulado = 1616;  // float BE: distancia desde el ultimo encuentro

uint32_t LeerBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void EscribirBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

uint32_t FloatABits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return bits;
}

bool g_avisado = false;

}  // namespace

REX_EXTERN(sub_829E3048) {
  const uint32_t controlador = ctx.r3.u32;
  if (!REXCVAR_GET(lo_no_random_battles) || controlador == 0) {
    __imp__sub_829E3048(ctx, base);
    return;
  }
  uint8_t* acumulado = base + controlador + kAcumulado;
  const uint32_t guardado = LeerBe32(acumulado);
  EscribirBe32(acumulado, FloatABits(-3.0e38f));
  __imp__sub_829E3048(ctx, base);
  EscribirBe32(acumulado, guardado);
  if (!g_avisado) {
    g_avisado = true;
    REXLOG_INFO("lo_trucos: sin batallas aleatorias activo (contador de encuentros congelado)");
  }
}
