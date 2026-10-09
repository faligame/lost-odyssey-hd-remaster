// lostodyssey - ReXGlue Recompiled Project
//
// Secreto para abrir el pack de texturas cifrado (.lopack): sale de los ficheros del disco 1 del
// jugador (default.xex y LO.fpi), asi que el pack no se abre sin los discos de su edicion. Mismo
// calculo que Rexglue/tools/lopack/lopack_formato.py (secreto_de_discos).
#pragma once

namespace lo {

// Con el plugin grafico ya cargado: le da el proveedor del secreto (se calcula la primera vez que
// el plugin lo pide, con los discos ya localizados).
void RegisterTexturePackSecret();

}  // namespace lo
