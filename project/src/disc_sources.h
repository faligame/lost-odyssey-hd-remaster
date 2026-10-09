// lostodyssey - ReXGlue Recompiled Project
//
// Discos del juego en cualquiera de sus formas, leidos en su sitio (nada se
// copia): carpeta extraida (o su default.xex), imagen ISO (XDVDFS) o paquete
// GOD (cabecera CON/LIVE/PIRS con su carpeta .data). Cada disco se identifica
// por la cabecera de su default.xex: juego, media ID y "disco N de M".
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace rex::filesystem {
class Device;
}

namespace lo {

inline constexpr uint32_t kLostOdysseyTitleId = 0x4D5307FA;

enum class DiscSourceKind { kFolder, kIso, kGod };

struct DiscSource {
  DiscSourceKind kind = DiscSourceKind::kFolder;
  std::filesystem::path path;  // carpeta, fichero .iso o cabecera del paquete GOD
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  int number = 0;  // disco N...
  int count = 0;   // ...de M
  // Carpeta de ficheros comunes a varios discos (data\\common) cuando este disco es data\\discN: el dispositivo
  // del disco busca primero en path y luego aqui. Vacia = el disco esta completo.
  std::filesystem::path common;
};

const char* DiscSourceKindName(DiscSourceKind kind);

// Identifica lo que haya en path: carpeta con default.xex, el propio
// default.xex, una .iso, la cabecera de un paquete GOD o su carpeta .data.
std::optional<DiscSource> IdentifyDiscSource(const std::filesystem::path& path);

// Busca discos dentro de las carpetas dadas, hasta depth niveles. Ordenados:
// carpetas, ISO y GOD.
std::vector<DiscSource> ScanDiscSources(const std::vector<std::filesystem::path>& roots, int depth);

// Dispositivo del sistema de ficheros virtual que lee el disco en su sitio.
std::unique_ptr<rex::filesystem::Device> CreateDiscDevice(const DiscSource& source,
                                                          std::string_view mount_path, bool read_only);

struct DiscFileInfo {
  std::string path;  // relativa a la raiz del disco, con '/'
  uint64_t size = 0;
};

// Lectura de ficheros de la raiz de un disco sin pasar por el VFS del juego.
class DiscReader {
 public:
  static std::unique_ptr<DiscReader> Open(const DiscSource& source);
  ~DiscReader();

  // Lee [offset, offset + size) recortado al final del fichero (size = ~0:
  // hasta el final). false si el fichero no existe o falla la lectura.
  bool ReadFile(std::string_view name, uint64_t offset, uint64_t size, std::vector<uint8_t>& out);

  // Compara el contenido del fichero name del disco con un fichero local (mismo tamano y mismos bytes).
  bool SameAs(std::string_view name, const std::filesystem::path& other, const std::atomic<bool>* cancel);

  // Todos los ficheros del disco (recursivo).
  bool List(std::vector<DiscFileInfo>& out);

  // Copia un fichero del disco a dest (por bloques, en dest.part y luego renombrado). Si dest ya existe con
  // el mismo tamano, solo cuenta el progreso. progress suma los bytes tratados; cancel corta la copia.
  bool CopyFileTo(std::string_view name, const std::filesystem::path& dest, std::atomic<uint64_t>* progress,
                  const std::atomic<bool>* cancel);

 private:
  explicit DiscReader(std::unique_ptr<rex::filesystem::Device> device);
  std::unique_ptr<rex::filesystem::Device> device_;
};

// Copia un fichero del disco a dest (si ya esta con el mismo tamano, no hace nada).
bool ExtractDiscFile(const DiscSource& source, std::string_view name, const std::filesystem::path& dest);

}  // namespace lo
