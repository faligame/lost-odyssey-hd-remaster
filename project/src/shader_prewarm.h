// lostodyssey - ReXGlue Recompiled Project
//
// Precreacion de pipelines de los materiales que el juego va cargando (ver
// shader_prewarm.cpp).
#pragma once

namespace lo {

// Con el modulo ya cargado (tabla de funciones lista): envuelve el thunk de
// NtReadFile para saber que paquetes del disco lee el juego.
void ShaderPrewarmInstall();

// Con el plugin grafico ya cargado, antes de arrancar el runtime: registra el proveedor del
// microcodigo del disco para los pipelines esenciales de la primera ejecucion.
void ShaderPrewarmRegisterEssentials();

// Precarga de sombreadores (generacion completa de pipelines por disco). La primera vez se
// pregunta: todos los discos disponibles o solo la parte actual (las siguientes, al llegar). Se
// guarda en config.toml (lo_shader_precache = "todo" | "parte"; vacio = preguntar).
enum class PrecacheChoice { kAll = 0, kPart = 1 };
// La pantalla de eleccion debe mostrarse (el juego no recibe el mando mientras tanto).
bool PrecacheChoicePending();
// Opcion resaltada en la pantalla de eleccion.
PrecacheChoice PrecacheChoiceSelected();
void PrecacheChoiceMove(int delta);
// Confirma la opcion resaltada: la guarda y sigue con la precarga.
void PrecacheChoiceConfirm();
// Discos disponibles (carpeta, ISO o GOD) y disco de la parte actual, para los textos.
int PrecacheDiscsAvailable();
int PrecacheCurrentDisc();
// Disco cuya precarga se esta haciendo ahora (0 = ninguno) y cuantos quedan en la cola.
int PrecacheGeneratingDisc();
int PrecacheDiscsQueued();

}  // namespace lo
