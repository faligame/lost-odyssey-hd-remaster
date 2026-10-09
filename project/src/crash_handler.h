// lostodyssey - ReXGlue Recompiled Project
//
// Capturador de cierres: cuando el proceso muere por una excepcion no
// controlada, deja un informe legible en logs\crash_<fecha>.txt con la
// direccion del fallo, la pila y, lo importante, EN QUE FUNCION DEL JUEGO
// estaba (el mapa del enlazador traduce la direccion a sub_XXXXXXXX, que es la
// direccion original del ejecutable de Xbox 360).
#pragma once

#include <filesystem>

namespace lo {

// Instala el capturador. Se puede llamar varias veces (la ultima manda).
// map_path es el .map que genera el enlazador junto al exe; si falta, el
// informe sale igual pero sin nombres de funcion.
void CrashHandlerInit(const std::filesystem::path& log_dir, const std::filesystem::path& map_path);

// Prueba del capturador (cvar lo_crash_test = segundos): provoca un cierre a
// proposito desde el hilo del juego, para comprobar que el informe sale y que
// nombra las funciones. Se llama desde la lectura del mando.
void CrashHandlerTestTick();

// Escribe en el log la cadena de llamadas del juego en este instante
// ("lo_pila [etiqueta]: 0x82.. <- 0x82.. <- ..."), sin cerrar nada. Para
// investigar quien llega a una funcion. Tiene un coste: usarla con moderacion.
void LogGuestStack(const char* etiqueta);

// Detector de cuelgues. El juego llama a D3DDevice_Swap una vez por fotograma
// (HangWatchFrame, desde el hook de live_tuning_hooks.cpp). Si pasan
// lo_hang_seconds sin fotogramas, se deja en logs\cuelgue_<fecha>.txt la pila de
// TODOS los hilos con las funciones del juego que estaban ejecutando, y otro
// informe si sigue parado mucho mas tiempo. Si el juego vuelve a dibujar, se
// anota en el log cuanto duro (asi se distingue una carga larga de un cuelgue).
// No cierra ni toca nada.
void HangWatchInit();
void HangWatchFrame();

}  // namespace lo
