// lostodyssey - ReXGlue Recompiled Project
//
// Reparacion de partidas traidas de otro sitio (Xenia, otra instalacion...).
#pragma once

#include <filesystem>

namespace lo {

// El SDK lista las partidas con un fichero .header por ranura, en
// <perfil>/<juego>/Headers/<tipo>/<ranura>.header. Las partidas copiadas de un
// emulador no lo traen, y al listarlas el juego se cierra. Esto reconstruye los
// que falten al arrancar, con el propio escritor del SDK.
void RepairSaveHeaders(const std::filesystem::path& user_data_root);

// Partidas en saves/saveNN/save.bin junto al exe (cvars savegame_root y
// savegame_folder_prefix del SDK). La primera vez copia las de old_root
// (savedata/<perfil>/4D5307FA/00000001/userNN) sin borrarlas.
void SetupFlatSaves(const std::filesystem::path& flat_root, const std::filesystem::path& old_root);

}  // namespace lo
