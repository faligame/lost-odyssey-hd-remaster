// lostodyssey - ReXGlue Recompiled Project
//
// Ver save_headers.h. Sintomas sin esto: al listar las partidas, el hilo de
// despacho del SDK lanza "Invalid UTF-8" y el juego se cierra
// ("[FATAL] Dispatch thread: deferred completion threw 'Invalid UTF-8'").

#include <algorithm>
#include "save_headers.h"

#include <cstdint>
#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

namespace lo {

using rex::X_RESULT;  // lo usa la macro X_ERROR_SUCCESS

namespace {

using namespace rex::system;
using namespace rex::system::xam;

std::string NameOf(const std::filesystem::path& path) {
  const auto u8 = path.filename().u8string();
  return std::string(u8.begin(), u8.end());
}

bool IsHexName(const std::string& name, size_t length) {
  return name.size() == length &&
         name.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

std::u16string ToUtf16Ascii(const std::string& name) {
  return std::u16string(name.begin(), name.end());
}

}  // namespace

namespace {
constexpr uint32_t kTitleId = 0x4D5307FA;
constexpr uint64_t kDefaultXuid = 0xB13EBABEBABEBABEull;
std::filesystem::path g_flat_root;
uint64_t g_flat_xuid = kDefaultXuid;

// "user00" -> "save00" (lo mismo que hace el SDK con savegame_folder_prefix).
std::string DiskName(const std::string& game_name) {
  return game_name.rfind("user", 0) == 0 ? "save" + game_name.substr(4) : game_name;
}
std::string GameName(const std::string& disk_name) {
  return disk_name.rfind("save", 0) == 0 ? "user" + disk_name.substr(4) : disk_name;
}
}  // namespace

void SetupFlatSaves(const std::filesystem::path& flat_root, const std::filesystem::path& old_root) {
  g_flat_root = flat_root;
  std::error_code ec;
  std::filesystem::create_directories(g_flat_root / ".cabeceras", ec);
  // Con barras normales: el SDK guarda las cadenas de config.toml entre comillas
  // dobles sin escapar, y una ruta con barras invertidas deja el fichero entero
  // sin poder leerse (el juego arranca sin plugin grafico y se queda en negro).
  {
    std::string flat_root_utf8 = rex::path_to_utf8(g_flat_root);
    std::replace(flat_root_utf8.begin(), flat_root_utf8.end(), char(92), '/');
    rex::cvar::SetFlagByName("savegame_root", flat_root_utf8);
  }
  rex::cvar::SetFlagByName("savegame_folder_prefix", "save");

  // Primera vez: copiar las partidas de savedata/<perfil>/4D5307FA/00000001/userNN
  // a saves/saveNN (con sus cabeceras). Las originales se quedan como copia.
  bool any = false;
  for (std::filesystem::directory_iterator it(g_flat_root, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (it->is_directory(ec) && NameOf(it->path()).rfind("save", 0) == 0) any = true;
  }
  if (any || old_root.empty()) return;
  int copied = 0;
  for (std::filesystem::directory_iterator profile(old_root, ec), end; !ec && profile != end;
       profile.increment(ec)) {
    const std::string profile_name = NameOf(profile->path());
    if (!profile->is_directory(ec) || !IsHexName(profile_name, 16)) continue;
    const auto saves = profile->path() / "4D5307FA" / "00000001";
    const auto headers = profile->path() / "4D5307FA" / "Headers" / "00000001";
    std::error_code ec2;
    for (std::filesystem::directory_iterator slot(saves, ec2), end2; !ec2 && slot != end2;
         slot.increment(ec2)) {
      if (!slot->is_directory(ec2)) continue;
      const std::string game_name = NameOf(slot->path());
      const auto target = g_flat_root / DiskName(game_name);
      std::error_code ec3;
      std::filesystem::copy(slot->path(), target, std::filesystem::copy_options::recursive, ec3);
      if (ec3) continue;
      std::filesystem::copy_file(headers / (game_name + ".header"),
                                 g_flat_root / ".cabeceras" / (DiskName(game_name) + ".header"),
                                 std::filesystem::copy_options::skip_existing, ec3);
      ++copied;
    }
    if (copied) {
      g_flat_xuid = std::strtoull(profile_name.c_str(), nullptr, 16);
      break;
    }
  }
  if (copied) {
    REXLOG_INFO("[partidas] {} partidas copiadas de {} a {} (las originales se conservan)", copied,
                rex::path_to_utf8(old_root), rex::path_to_utf8(g_flat_root));
  }
}

void RepairSaveHeaders(const std::filesystem::path& user_data_root) {
  // Partidas en saves/saveNN: cabeceras que falten (partidas copiadas a mano).
  if (!g_flat_root.empty()) {
    auto* kernel = REX_KERNEL_STATE();
    auto* content = kernel ? kernel->content_manager() : nullptr;
    if (!content) return;
    std::error_code ec;
    int repaired = 0;
    for (std::filesystem::directory_iterator it(g_flat_root, ec), end; !ec && it != end;
         it.increment(ec)) {
      const std::string disk_name = NameOf(it->path());
      if (!it->is_directory(ec) || disk_name.empty() || disk_name[0] == '.') continue;
      std::error_code exists_ec;
      if (std::filesystem::exists(g_flat_root / ".cabeceras" / (disk_name + ".header"), exists_ec)) {
        continue;
      }
      const std::string game_name = GameName(disk_name);
      XCONTENT_AGGREGATE_DATA data;
      data.device_id = 1;
      data.content_type = XContentType::kSavedGame;
      data.set_display_name(ToUtf16Ascii(game_name));
      data.set_file_name(game_name);
      data.title_id = kTitleId;
      data.xuid = g_flat_xuid;
      if (content->WriteContentHeaderFile(g_flat_xuid, data) == X_ERROR_SUCCESS) ++repaired;
    }
    if (repaired > 0) {
      REXLOG_INFO("[partidas] reconstruidas {} cabeceras en saves/.cabeceras", repaired);
    }
    return;
  }

  auto* kernel = REX_KERNEL_STATE();
  auto* content = kernel ? kernel->content_manager() : nullptr;
  if (!content || user_data_root.empty()) return;

  std::error_code ec;
  int repaired = 0;
  // <perfil hex de 16>/<juego hex de 8>/<tipo hex de 8>/<ranura>
  for (std::filesystem::directory_iterator profile(user_data_root, ec), end; !ec && profile != end;
       profile.increment(ec)) {
    const std::string profile_name = NameOf(profile->path());
    if (!profile->is_directory(ec) || !IsHexName(profile_name, 16)) continue;
    const uint64_t xuid = std::strtoull(profile_name.c_str(), nullptr, 16);

    for (std::filesystem::directory_iterator title(profile->path(), ec); !ec && title != end;
         title.increment(ec)) {
      const std::string title_name = NameOf(title->path());
      if (!title->is_directory(ec) || !IsHexName(title_name, 8)) continue;
      const uint32_t title_id = uint32_t(std::strtoul(title_name.c_str(), nullptr, 16));

      for (std::filesystem::directory_iterator type(title->path(), ec); !ec && type != end;
           type.increment(ec)) {
        const std::string type_name = NameOf(type->path());
        // La carpeta "Headers" no es hexadecimal: se salta sola.
        if (!type->is_directory(ec) || !IsHexName(type_name, 8)) continue;
        const uint32_t content_type = uint32_t(std::strtoul(type_name.c_str(), nullptr, 16));

        for (std::filesystem::directory_iterator slot(type->path(), ec); !ec && slot != end;
             slot.increment(ec)) {
          if (!slot->is_directory(ec)) continue;
          const std::string slot_name = NameOf(slot->path());
          const std::filesystem::path header =
              title->path() / "Headers" / type_name / (slot_name + ".header");
          std::error_code exists_ec;
          if (std::filesystem::exists(header, exists_ec)) continue;

          XCONTENT_AGGREGATE_DATA data;
          data.device_id = 1;
          data.content_type = static_cast<XContentType>(content_type);
          data.set_display_name(ToUtf16Ascii(slot_name));
          data.set_file_name(slot_name);
          data.title_id = title_id;
          data.xuid = xuid;
          if (content->WriteContentHeaderFile(xuid, data) == X_ERROR_SUCCESS) ++repaired;
        }
      }
    }
    ec.clear();
  }

  if (repaired > 0) {
    REXLOG_INFO("[partidas] reconstruidos {} ficheros .header que faltaban (partidas traidas de "
                "otro emulador o instalacion)",
                repaired);
  }
}

}  // namespace lo
