// Fork (odisea): lector del pack de texturas cifrado (.lopack). Formato y razones en
// Rexglue/tools/lopack/lopack_formato.py. Resumen: cada imagen va comprimida con zstd y cifrada con
// AES-256-GCM; la clave sale (HKDF-SHA256) de los discos del jugador, asi que sin ellos el fichero
// no se abre. Las imagenes descifradas solo existen en memoria.
#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace rex::graphics::odisea::lopack {

enum class Kind : uint8_t { kDds = 0, kPng = 1 };

// Secreto de los discos (32 bytes). Lo da el exe (odisea_SetTexturePackSecretProvider) cuando se
// necesita, ya con los discos localizados. false = no hay disco 1.
using SecretProviderFn = bool (*)(uint8_t out[32]);
void SetSecretProvider(SecretProviderFn fn);
SecretProviderFn GetSecretProvider();

class Pack {
 public:
  // Abre el fichero, deriva la clave y descifra el indice. false si no existe, no es un .lopack o
  // los discos no son los de este pack (la etiqueta del indice no cuadra).
  bool Open(const std::filesystem::path& file, const uint8_t secret[32]);
  void Close();
  bool is_open() const { return handle_ != nullptr; }
  size_t size() const { return entries_.size(); }
  uint32_t content_version() const { return content_version_; }
  bool Has(uint64_t hash) const { return entries_.count(hash) != 0; }
  bool KindOf(uint64_t hash, Kind& kind) const;
  // Lee, descifra y descomprime una imagen. Seguro entre hilos.
  bool Read(uint64_t hash, std::vector<uint8_t>& out, Kind& kind) const;
  ~Pack() { Close(); }

 private:
  struct Entry {
    uint64_t offset;
    uint32_t cipher_size;
    uint32_t raw_size;
    uint32_t index;
    uint8_t kind;
    uint8_t codec;
  };
  void* handle_ = nullptr;  // HANDLE
  uint8_t key_[32] = {};
  uint32_t content_version_ = 0;
  std::unordered_map<uint64_t, Entry> entries_;
};

}  // namespace rex::graphics::odisea::lopack
