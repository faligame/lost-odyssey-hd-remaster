// lostodyssey - ReXGlue Recompiled Project
//
// Progreso de la preparacion de sombreadores del arranque: el plugin grafico
// (Odisea) traduce la cache de sombreadores y compila los pipelines guardados
// antes de que el juego dibuje nada; aqui se lee su progreso (exportado por
// rexgpu-odisea.dll) para pintar la pantalla "Preparando sombreadores" con la
// fuente y las texturas del propio juego (settings_page.cpp).
#pragma once

#include <cstdint>

namespace lo {

// El plugin ya dejo hecha la generacion completa de ese disco (marca): tick de la pantalla.
bool QueryDiscPrepared(int disc);

// phase: 0 = nada que preparar, 1 = traduciendo sombreadores, 2 = creando
// pipelines. Devuelve false si el plugin cargado no informa del progreso.
bool QueryShaderPrep(int& phase, uint32_t& done, uint32_t& total);

// Fotogramas del juego por segundo (media del ultimo medio segundo) y duracion
// media y maxima de un fotograma en ms. false si el plugin no lo exporta.
bool QueryGameFrameStats(float& fps, float& avg_ms, float& max_ms);

// Generacion completa de pipelines del primer arranque: si se puede pulsar
// "Jugar ya" y, al pulsarlo, seguir en segundo plano sin la pantalla.
bool QueryPrewarmCanSkip();
// Hay una generacion completa de pipelines en marcha (con pantalla o en segundo plano).
bool QueryPrewarmBulkRunning();
void PrewarmSkip();

// Enter del teclado = "Jugar ya" (por fotograma, desde el hook de D3DDevice_Swap): sirve sin mando.
void ShaderPrepKeyboardTick();

}  // namespace lo
