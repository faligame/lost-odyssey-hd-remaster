// lostodyssey - ReXGlue Recompiled Project
//
// Frenos de estabilidad del guest (ver guest_guards.cpp).
#pragma once

#include <filesystem>

namespace lo {

// Compatibilidad: deja la pagina cero del guest (0x0-0xFFFF) accesible
// (`protect_zero = false`), igual que la configuracion de Xenia Canary con la que
// se juega a Lost Odyssey, salvo que el toml diga otra cosa. Hay que llamarla
// ANTES de construir el runtime: la memoria del guest lee el cvar al crearse.
void ApplyZeroPageCompat(const std::filesystem::path& config_path);

// Escribe en el log si la pagina cero ha quedado accesible (con el runtime ya
// construido).
void LogZeroPageState();

// Reserva a ceros la zona que XACT lee como banco mientras un banco se carga
// (cvar lo_xact_bank_sink). Con el runtime ya construido.
void ReserveXactBankSink();

}  // namespace lo
