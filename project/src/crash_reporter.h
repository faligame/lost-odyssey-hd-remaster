// lostodyssey - ReXGlue Recompiled Project
//
// Aviso de cierres inesperados. Si la ejecucion anterior dejo un informe (logs\crash_*.txt, lo
// escribe crash_handler.cpp), al arrancar se le ofrece al jugador abrir en GitHub un issue ya
// rellenado: version, compilacion, sistema, tarjeta grafica y la pila del fallo. El jugador lo
// revisa y lo envia el mismo; no hay envio automatico, ni tokens en el ejecutable, y no se incluye
// nada personal (rutas y nombre de usuario se quitan) ni datos del juego.
#pragma once

#include <filesystem>

namespace lo {

// Pregunta una sola vez por informe (deja <informe>.visto). No hace nada si no hay informes nuevos.
void CrashReporterOffer(const std::filesystem::path& log_dir);

}  // namespace lo
