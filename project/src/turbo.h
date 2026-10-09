// lostodyssey - ReXGlue Recompiled Project
//
// Modo TURBO (ver turbo_hooks.cpp).
#pragma once

#include <cstdint>

namespace lo {
// Procesa el estado XInput que el juego acaba de leer (ambas rutas de
// XamInputGetState). Devuelve los botones ya sin el de turbo.
void TurboProcessButtons(uint16_t* buttons);
bool TurboActive();
void TurboSet(bool on);
void TurboToggle();
void TurboRefresh();
}  // namespace lo
