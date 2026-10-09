// lostodyssey - ReXGlue Recompiled Project
//
// Vigilante de la memoria del guest. El cierre del 27-sep-2026 (13:11) empezo
// con "BaseHeap::Alloc failed to find contiguous range" al crear un hilo: el
// juego pidio un hilo, el kernel no encontro 728 bytes libres en el monton
// virtual de 4 KB y devolvio NO_MEMORY; el juego no comprueba el error y
// murio leyendo un puntero basura.
//
// Ese monton (guest 0x00000000-0x3FFFFFFF) lo comparten TRES cosas: las
// reservas normales del juego, los apuntes del kernel (bloque de estado y TLS
// de cada hilo) y, si esta activado, el segmento del heap del CRT
// (rexcrt_heap_size_mb, que se reserva de ahi mismo y de arriba hacia abajo).
// Sin numeros no se puede saber quien lo agota, asi que esto los escribe en el
// log cada pocos segundos.
#pragma once

namespace lo {

// Arranca el vigilante (cvar lo_heap_watch_seconds; 0 = apagado). Se llama una
// vez, con el modulo ya lanzado.
void HeapWatchStart();

}  // namespace lo
