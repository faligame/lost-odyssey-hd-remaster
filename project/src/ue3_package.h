// lostodyssey - ReXGlue Recompiled Project
//
// Lectura de los datos del disco de Lost Odyssey, compartida por el menu del
// port (menu_assets.cpp) y el volcado de texturas (disc_texture_dump.cpp).
// Todo lo que se lee se valida contra los limites del buffer: un fichero
// inesperado da un ParseError, nunca un acceso fuera de rango.
//
// Cadena (formatos documentados publicamente; codigo propio):
//   LO.fpi (indice) -> xenon_*.fpd (archivos) -> bloque "cpx" (LZ propio)
//   -> paquete Unreal Engine 3 big-endian -> exports (Texture2D, Font...).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "disc_sources.h"

namespace lo::ue3 {

struct ParseError {
  std::string what;
};

[[noreturn]] inline void Fail(std::string what) { throw ParseError{std::move(what)}; }

using Bytes = std::vector<uint8_t>;

inline void Need(const Bytes& b, size_t pos, size_t count, const char* what) {
  if (pos > b.size() || count > b.size() - pos) {
    Fail(fmt::format("{}: lectura fuera de rango ({} + {} > {})", what, pos, count, b.size()));
  }
}

inline uint16_t Le16(const Bytes& b, size_t p) {
  Need(b, p, 2, "le16");
  return uint16_t(b[p] | (b[p + 1] << 8));
}

inline uint32_t Le32(const Bytes& b, size_t p) {
  Need(b, p, 4, "le32");
  return uint32_t(b[p]) | (uint32_t(b[p + 1]) << 8) | (uint32_t(b[p + 2]) << 16) |
         (uint32_t(b[p + 3]) << 24);
}

// Lee [offset, offset + size) de un fichero de la raiz del disco; size = ~0
// hasta el final. Un rango que no cabe entero es un error.
inline Bytes ReadDiscFile(DiscReader& reader, const std::string& name, uint64_t offset = 0, uint64_t size = ~0ull) {
  Bytes out;
  if (!reader.ReadFile(name, offset, size, out)) Fail(fmt::format("no se puede leer {} del disco", name));
  if (size != ~0ull && out.size() != size) {
    Fail(fmt::format("{}: rango {}+{} fuera del fichero", name, offset, size));
  }
  return out;
}

// --- LO.fpi -------------------------------------------------------------------
// Nombres en base 40 con diccionario comun, sufijos frecuentes por indice y
// extensiones en tabla aparte. Entradas de 24 bytes; los directorios apuntan a
// un rango de hijos.
constexpr std::string_view kAlphabet("\0" "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_.\\", 40);
constexpr std::array<std::string_view, 32> kSuffixes = {
    "",       "_sndw",  "_scrw",  "_mapw",  "_lvdw",  "_navw", "_colw", "_camw",
    "_map",   "_cam",   "_bx",    "_mw",    "_a",     "_d",    "_f",    "_m",
    "_p",     "_u",     "_w",     "_0",     "_1",     "_2",    "_00",   "_01",
    "_0mw",   "_nav",   "_elgt",  "_000a0", "_010a0", "_020a0", "_030a0", "_040a0"};

struct FpiFile {
  std::string archive;
  uint64_t offset = 0;
  uint32_t size = 0;
};

class Fpi {
 public:
  explicit Fpi(Bytes b) : b_(std::move(b)) {
    dictionary_ = Le32(b_, 40);
    extensions_ = Le32(b_, 44);
  }

  // Recorre todo el indice y devuelve los ficheros cuya ruta cumpla filter.
  template <typename Filter>
  std::unordered_map<std::string, FpiFile> Files(Filter&& filter) const {
    std::unordered_map<std::string, FpiFile> result;
    const uint16_t count = Le16(b_, 26);
    const uint32_t begin = Le32(b_, 32);
    for (uint32_t i = 0; i < count; ++i) {
      const size_t ar = size_t(begin) + size_t(i) * 48;
      const std::string archive = Name(Le32(b_, ar + 24));
      const size_t base = ar + Le32(b_, ar + 4);
      const std::string prefix = Name(Le32(b_, ar + 20));
      struct Pending {
        uint32_t index, count;
        std::string path;
      };
      std::vector<Pending> pending{{0, Le16(b_, ar + 2), prefix.empty() ? "" : prefix + "\\"}};
      std::unordered_set<uint32_t> seen;
      while (!pending.empty()) {
        Pending dir = std::move(pending.back());
        pending.pop_back();
        for (uint32_t j = 0; j < dir.count; ++j) {
          const uint32_t idx = dir.index + j;
          if (!seen.insert(idx).second) continue;
          const size_t p = base + size_t(idx) * 24;
          const uint32_t bits = Le32(b_, p);
          std::string full = dir.path + Name(bits);
          if (bits & 0x10000000) {
            pending.push_back({Le32(b_, p + 20), Le16(b_, p + 14), full + "\\"});
          } else if (filter(full)) {
            result[std::move(full)] = {archive, uint64_t(Le32(b_, p + 8) & 0xFFFFFF) * 2048,
                                       Le32(b_, p + 16)};
          }
        }
      }
    }
    return result;
  }

 private:
  std::string Unpack(size_t off) const {
    auto letter = [](uint32_t v) {
      if (v >= kAlphabet.size()) Fail("fpi: nombre corrupto");
      return kAlphabet[v];
    };
    const uint16_t word = Le16(b_, off);
    std::string out;
    out += letter(word / 40 % 40);
    out += letter(word / 1600);
    for (uint32_t i = 0; i < uint32_t(word % 40); ++i) {
      const uint16_t w = Le16(b_, off + 2 + 2 * size_t(i));
      out += letter(w % 40);
      out += letter(w / 40 % 40);
      out += letter(w / 1600);
    }
    out.resize(std::min(out.find('\0'), out.size()));
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return char((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c); });
    return out;
  }

  std::string Name(uint32_t bits) const {
    if (!(bits & 0x3FFFF)) return {};
    std::string name = Unpack(dictionary_ + size_t(bits & 0x3FFFF) * 2);
    name += kSuffixes[(bits >> 18) & 31];
    if (const uint32_t ext = (bits >> 23) & 31) {
      name += '.';
      name += Unpack(dictionary_ + size_t(Le16(b_, extensions_ + (ext - 1) * 2)) * 2);
    }
    return name;
  }

  Bytes b_;
  uint32_t dictionary_ = 0;
  uint32_t extensions_ = 0;
};

// --- cpx ----------------------------------------------------------------------
// Bloques de 64 KiB con LZ de bits; cabecera con la anchura de la palabra de
// bits (8 o 16), numero de bloques, tamanos y tabla de offsets.
class CpxBits {
 public:
  CpxBits(const uint8_t* data, size_t size, uint32_t width) : data_(data), size_(size), width_(width) {
    Refill();
  }

  uint32_t Get(uint32_t count) {
    uint32_t r = 0;
    while (count) {
      const uint32_t take = std::min(count, left_);
      left_ -= take;
      r = (r << take) | ((value_ >> left_) & ((1u << take) - 1));
      count -= take;
      if (!left_) Refill();
    }
    return r;
  }

  uint32_t DirectWord() {
    if (pos_ + 2 > size_) Fail("cpx: palabra directa fuera de rango");
    const uint32_t r = (uint32_t(data_[pos_]) << 8) | data_[pos_ + 1];
    pos_ += 2;
    return r;
  }

 private:
  void Refill() {
    uint32_t v = 0;
    for (uint32_t i = 0; i < width_ / 8; ++i) {
      v <<= 8;
      if (pos_ < size_) v |= data_[pos_];
      ++pos_;
    }
    value_ = v;
    left_ = width_;
  }

  const uint8_t* data_;
  size_t size_;
  uint32_t width_;
  size_t pos_ = 0;
  uint32_t value_ = 0;
  uint32_t left_ = 0;
};

inline void CpxBlock(const uint8_t* blk, size_t blk_size, uint32_t width, uint8_t* out, size_t size) {
  if (blk_size < 4) Fail("cpx: bloque corto");
  if (blk[0] == 255) {
    if (blk_size - 4 < size) Fail("cpx: bloque sin comprimir corto");
    std::memcpy(out, blk + 4, size);
    return;
  }
  const uint32_t length_mode = blk[0] >> 6;
  const uint32_t offset_mode = (blk[0] >> 4) & 3;
  std::array<uint32_t, 3> widths = {uint32_t(blk[1] & 15), uint32_t(blk[1] >> 4), 0};
  if (offset_mode < 3) widths[offset_mode] = 16;
  static constexpr uint8_t kOrders[4][3] = {{0, 1, 2}, {1, 0, 2}, {2, 0, 1}, {2, 2, 2}};
  const uint8_t* order = kOrders[(blk[0] >> 2) & 3];

  CpxBits bits(blk + 4, blk_size - 4, width);
  size_t pos = 0;
  while (pos < size) {
    if (!bits.Get(1)) {
      out[pos++] = uint8_t(bits.Get(8));
      continue;
    }
    uint32_t offset = 0;
    if (offset_mode == 0) {
      offset = width == 16 ? bits.DirectWord() : bits.Get(16);
    } else if (offset_mode == 1) {
      offset = bits.Get(widths[bits.Get(1)]);
    } else if (offset_mode == 2) {
      const uint32_t choice = !bits.Get(1) ? order[0] : order[1 + bits.Get(1)];
      offset = bits.Get(widths[choice]);
    }
    uint32_t length_bits = 9;
    if (length_mode == 1 && !bits.Get(1)) {
      length_bits = 2;
    } else if (length_mode == 2 && !bits.Get(1)) {
      length_bits = 3;
    }
    const size_t length = bits.Get(length_bits) + 3;
    if (offset + 1 > pos || length > size - pos) Fail("cpx: referencia fuera de rango");
    for (size_t i = 0; i < length; ++i, ++pos) {
      out[pos] = out[pos - offset - 1];
    }
  }
}

inline Bytes CpxDecode(const Bytes& data) {
  if (data.size() < 16 || std::memcmp(data.data(), "cpx", 3) != 0) return data;
  const uint16_t blocks = Le16(data, 6);
  const uint32_t stored = Le32(data, 8);
  const uint32_t decoded = Le32(data, 12);
  const uint32_t width = (data[4] & 0xF0) == 0x10 ? 8 : 16;
  if (stored != data.size()) Fail("cpx: tamano almacenado incoherente");
  Bytes out(decoded);
  size_t done = 0;
  for (uint32_t i = 0; i < blocks && done < decoded; ++i) {
    const uint32_t begin = Le32(data, 16 + 4 * size_t(i));
    const uint32_t end = i + 1 < blocks ? Le32(data, 20 + 4 * size_t(i)) : stored;
    if (begin > end || end > data.size()) Fail("cpx: tabla de bloques corrupta");
    const size_t count = std::min<size_t>(65536, decoded - done);
    CpxBlock(data.data() + begin, end - begin, width, out.data() + done, count);
    done += count;
  }
  if (done != decoded) Fail("cpx: faltan bloques");
  return out;
}

// --- Paquete UE3 (big-endian) -------------------------------------------------
class Reader {
 public:
  Reader(const Bytes& b, size_t pos = 0) : b_(b), pos_(pos) {}

  const uint8_t* Take(size_t n) {
    Need(b_, pos_, n, "paquete");
    const uint8_t* p = b_.data() + pos_;
    pos_ += n;
    return p;
  }
  uint32_t U32() {
    const uint8_t* p = Take(4);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  uint16_t U16() {
    const uint8_t* p = Take(2);
    return uint16_t((p[0] << 8) | p[1]);
  }
  int32_t I32() { return int32_t(U32()); }
  uint8_t U8() { return *Take(1); }
  std::string String() {
    const int32_t n = I32();
    if (n < -4096 || n > 4096) Fail("paquete: cadena no soportada");
    if (n < 0) {
      // UTF-16 (big-endian en estos paquetes): lo que no es ASCII sale como '?'.
      const uint8_t* p = Take(size_t(-n) * 2);
      std::string out;
      for (int32_t i = 0; i + 1 < -n; ++i) {
        const uint16_t c = uint16_t((p[i * 2] << 8) | p[i * 2 + 1]);
        out.push_back(c >= 32 && c < 127 ? char(c) : '?');
      }
      return out;
    }
    const uint8_t* p = Take(size_t(n));
    return std::string(reinterpret_cast<const char*>(p), n ? size_t(n - 1) : 0);
  }
  size_t pos() const { return pos_; }

 private:
  const Bytes& b_;
  size_t pos_;
};

struct Export {
  int32_t cls = 0;
  std::string name;
  uint32_t size = 0;
  uint32_t offset = 0;
};

class Package {
 public:
  explicit Package(Bytes b) : b_(std::move(b)) {
    Reader r(b_);
    if (r.U32() != 0x9E2A83C1) Fail("paquete: firma incorrecta");
    r.U32();  // version
    r.U32();  // tamano de cabecera
    r.String();  // grupo
    r.U32();  // flags
    const uint32_t name_count = r.U32(), name_offset = r.U32();
    const uint32_t export_count = r.U32(), export_offset = r.U32();
    const uint32_t import_count = r.U32(), import_offset = r.U32();
    if (name_count > 100000 || export_count > 100000 || import_count > 100000) {
      Fail("paquete: tablas demasiado grandes");
    }

    Reader names(b_, name_offset);
    for (uint32_t i = 0; i < name_count; ++i) {
      names_.push_back(names.String());
      names.Take(8);
    }
    Reader imports(b_, import_offset);
    for (uint32_t i = 0; i < import_count; ++i) {
      ReadName(imports);
      import_classes_.push_back(ReadName(imports));
      imports.I32();
      imports_.push_back(ReadName(imports));
    }
    Reader exports(b_, export_offset);
    for (uint32_t i = 0; i < export_count; ++i) {
      Export e;
      e.cls = exports.I32();
      exports.I32();  // super
      exports.I32();  // outer
      e.name = ReadName(exports);
      exports.I32();
      exports.Take(8);
      e.size = exports.U32();
      e.offset = exports.U32();
      const uint32_t componentes = exports.U32();  // TMap<FName, int32>
      if (componentes > 65536) Fail("paquete: mapa de componentes corrupto");
      exports.Take(size_t(componentes) * 12);
      exports.U32();  // ExportFlags
      const uint32_t red = exports.U32();  // GenerationNetObjectCount
      if (red > 65536) Fail("paquete: lista de red corrupta");
      exports.Take(size_t(red) * 4);
      exports.Take(16);  // PackageGuid
      exports_.push_back(std::move(e));
    }
  }

  std::string ReadName(Reader& r) const {
    const uint32_t index = r.U32(), number = r.U32();
    if (index >= names_.size()) Fail("paquete: nombre fuera de tabla");
    return number ? fmt::format("{}_{}", names_[index], number - 1) : names_[index];
  }

  std::string_view ClassName(const Export& e) const {
    if (e.cls < 0 && size_t(-int64_t(e.cls) - 1) < imports_.size()) return imports_[size_t(-int64_t(e.cls) - 1)];
    return {};
  }

  const Export& Find(std::string_view name, std::string_view cls) const {
    for (const Export& e : exports_) {
      if (e.name == name && ClassName(e) == cls) return e;
    }
    Fail(fmt::format("paquete: no esta {} ({})", name, cls));
  }

  const std::vector<Export>& exports() const { return exports_; }
  const Bytes& bytes() const { return b_; }
  // Imports: nombre del objeto y nombre de su clase (mismo indice).
  const std::vector<std::string>& imports() const { return imports_; }
  const std::vector<std::string>& import_classes() const { return import_classes_; }

 private:
  Bytes b_;
  std::vector<std::string> names_;
  std::vector<std::string> imports_;
  std::vector<std::string> import_classes_;
  std::vector<Export> exports_;
};

// Propiedades serializadas hasta "None"; solo interesan enteros y bytes. Si se
// pasa nombres, ahi van las propiedades cuyo valor es un nombre (enumerados
// guardados como FName y NameProperty).
inline std::unordered_map<std::string, int32_t> ReadProperties(
    const Package& pkg, Reader& r, std::unordered_map<std::string, std::string>* nombres = nullptr) {
  std::unordered_map<std::string, int32_t> props;
  r.I32();  // indice de red
  for (int guard = 0; guard < 4096; ++guard) {
    std::string name = pkg.ReadName(r);
    if (name == "None") return props;
    const std::string type = pkg.ReadName(r);
    const uint32_t size = r.U32();
    r.U32();  // indice de array
    if (type == "StructProperty") pkg.ReadName(r);
    if (type == "BoolProperty") props[name] = int32_t(r.U32());
    if (nombres && (type == "ByteProperty" || type == "NameProperty") && size == 8) {
      (*nombres)[name] = pkg.ReadName(r);
      continue;
    }
    const uint8_t* value = r.Take(size);
    if (type == "IntProperty" && size == 4) {
      props[name] = int32_t((uint32_t(value[0]) << 24) | (uint32_t(value[1]) << 16) |
                            (uint32_t(value[2]) << 8) | value[3]);
    } else if (type == "ByteProperty" && size >= 1) {
      props[name] = value[0];
    }
  }
  Fail("paquete: demasiadas propiedades");
}

// --- Texture2D ----------------------------------------------------------------
// Offset de un bloque en una textura en mosaico de la Xbox 360 (mismo orden que
// usa la GPU Xenos para las texturas de guest).
inline uint32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  pitch = (pitch + 31) & ~31u;
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytes_per_block_log2 + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return uint32_t(((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
                  (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F));
}

// Bloque DXT5 (little-endian ya corregido) -> 16 pixeles RGBA.
inline void DecodeBc3Block(const uint8_t* b, uint8_t* out16) {
  std::array<uint8_t, 8> alpha{};
  const uint32_t a0 = b[0], a1 = b[1];
  alpha[0] = uint8_t(a0);
  alpha[1] = uint8_t(a1);
  if (a0 > a1) {
    for (uint32_t i = 1; i < 7; ++i) alpha[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (uint32_t i = 1; i < 5; ++i) alpha[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    alpha[6] = 0;
    alpha[7] = 255;
  }
  uint64_t alpha_bits = 0;
  for (int i = 0; i < 6; ++i) alpha_bits |= uint64_t(b[2 + i]) << (8 * i);
  std::array<std::array<uint8_t, 3>, 4> colors{};
  for (int i = 0; i < 2; ++i) {
    const uint32_t c = b[8 + 2 * i] | (uint32_t(b[9 + 2 * i]) << 8);
    const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
    colors[i] = {uint8_t((r << 3) | (r >> 2)), uint8_t((g << 2) | (g >> 4)), uint8_t((bl << 3) | (bl >> 2))};
  }
  for (int k = 0; k < 3; ++k) {
    colors[2][k] = uint8_t((2 * colors[0][k] + colors[1][k]) / 3);
    colors[3][k] = uint8_t((colors[0][k] + 2 * colors[1][k]) / 3);
  }
  const uint32_t color_bits = b[12] | (uint32_t(b[13]) << 8) | (uint32_t(b[14]) << 16) | (uint32_t(b[15]) << 24);
  for (int i = 0; i < 16; ++i) {
    const auto& c = colors[(color_bits >> (2 * i)) & 3];
    out16[i * 4 + 0] = c[0];
    out16[i * 4 + 1] = c[1];
    out16[i * 4 + 2] = c[2];
    out16[i * 4 + 3] = alpha[(alpha_bits >> (3 * i)) & 7];
  }
}

}  // namespace lo::ue3
