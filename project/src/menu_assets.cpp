// lostodyssey - ReXGlue Recompiled Project
//
// Lectura de los recursos del menu del juego desde los datos del disco. Ver
// menu_assets.h. Todo lo que se lee se valida contra los limites del buffer: un
// fichero inesperado da un error, nunca un acceso fuera de rango.

#include "menu_assets.h"
#include "ue3_package.h"
#include "lo_i18n.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string_view>
#include <unordered_set>

#include <fmt/format.h>
#include <lzokay.hpp>
#include <rex/cvar.h>
#include <rex/system/flags.h>

namespace lo::menu_assets {

namespace {

using namespace lo::ue3;

// Huella de las paginas: FNV-1a por palabras de los bytes en mosaico del nivel base (alineados a bloques de 32),
// la misma que usa el plugin grafico y disc_texture_dump.cpp.
uint64_t HashBytes(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  const size_t words = n / 8;
  for (size_t i = 0; i < words; ++i) {
    uint64_t w;
    std::memcpy(&w, p + i * 8, 8);
    h = (h ^ w) * 1099511628211ull;
  }
  for (size_t i = words * 8; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h ^ uint64_t(n);
}

Image DecodeTexture(const Package& pkg, const Export& e) {
  Reader r(pkg.bytes(), e.offset);
  auto props = ReadProperties(pkg, r);
  const int32_t width = props.count("SizeX") ? props["SizeX"] : 0;
  const int32_t height = props.count("SizeY") ? props["SizeY"] : 0;
  if (props["Format"] != 7) Fail(fmt::format("{}: formato {} no es DXT5", e.name, props["Format"]));
  if (width < 4 || height < 4 || width > 4096 || height > 4096 || (width | height) & 3) {
    Fail(fmt::format("{}: tamano {}x{} no soportado", e.name, width, height));
  }
  for (int i = 0; i < 3; ++i) r.U32();
  if (r.U32() != r.pos()) Fail(fmt::format("{}: datos de mip incoherentes", e.name));
  r.U32();  // numero de mips
  const uint32_t flags = r.U32(), raw_size = r.U32(), stored_size = r.U32(), offset = r.U32();
  if (flags != 0x10 || offset != r.pos() || raw_size != uint32_t(width * height)) {
    Fail(fmt::format("{}: mip 0 no esta comprimido con LZO", e.name));
  }

  // Flujo comprimido: cabecera, tabla de trozos y trozos LZO1X.
  const size_t stream_begin = r.pos();
  r.Take(stored_size);
  Reader s(pkg.bytes(), stream_begin);
  if (s.U32() != 0x9E2A83C1) Fail(fmt::format("{}: flujo sin firma", e.name));
  const uint32_t block_size = s.U32();
  s.U32();  // comprimido total
  const uint32_t decoded = s.U32();
  if (!block_size || decoded != raw_size) Fail(fmt::format("{}: flujo incoherente", e.name));
  std::vector<std::pair<uint32_t, uint32_t>> chunks((decoded + block_size - 1) / block_size);
  for (auto& [chunk_stored, chunk_decoded] : chunks) {
    chunk_stored = s.U32();
    chunk_decoded = s.U32();
  }
  Bytes raw(raw_size);
  size_t done = 0;
  for (const auto& [chunk_stored, chunk_decoded] : chunks) {
    if (chunk_decoded > raw.size() - done) Fail(fmt::format("{}: trozo demasiado grande", e.name));
    const uint8_t* src = s.Take(chunk_stored);
    size_t out_size = 0;
    if (lzokay::decompress(src, chunk_stored, raw.data() + done, chunk_decoded, out_size) !=
            lzokay::EResult::Success ||
        out_size != chunk_decoded) {
      Fail(fmt::format("{}: LZO invalido", e.name));
    }
    done += out_size;
  }
  if (done != raw.size()) Fail(fmt::format("{}: faltan datos", e.name));

  Image image;
  {
    // DXT5: bloques de 4x4 y 16 bytes; el plugin hashea el rango alineado a 32 bloques.
    const size_t bw = (size_t(width) / 4 + 31) & ~size_t(31), bh = (size_t(height) / 4 + 31) & ~size_t(31);
    const size_t extension = bw * bh * 16;
    Bytes padded = raw;
    padded.resize(extension, 0);
    image.hash = HashBytes(padded.data(), extension);
  }
  image.width = uint32_t(width);
  image.height = uint32_t(height);
  image.rgba.resize(size_t(width) * height * 4);
  const uint32_t blocks_x = uint32_t(width) / 4, blocks_y = uint32_t(height) / 4;
  std::array<uint8_t, 16> block{};
  std::array<uint8_t, 64> pixels{};
  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      const uint32_t address = TiledOffset2D(int32_t(bx), int32_t(by), blocks_x, 4);
      if (address > raw.size() || raw.size() - address < 16) Fail(fmt::format("{}: mosaico fuera de rango", e.name));
      for (uint32_t i = 0; i < 16; ++i) block[i] = raw[address + (i ^ 1)];  // palabras de 16 bits big-endian
      DecodeBc3Block(block.data(), pixels.data());
      for (uint32_t i = 0; i < 16; ++i) {
        std::memcpy(&image.rgba[((by * 4 + i / 4) * width + bx * 4 + i % 4) * 4], &pixels[i * 4], 4);
      }
    }
  }
  return image;
}

// --- Font ---------------------------------------------------------------------
Font DecodeFont(const Package& pkg, const Export& e) {
  Reader r(pkg.bytes(), e.offset);
  ReadProperties(pkg, r);
  const uint32_t count = r.U32();
  if (count > 65536) Fail(fmt::format("{}: demasiados glifos", e.name));
  std::vector<Glyph> glyphs(count);
  for (Glyph& g : glyphs) {
    const uint32_t x = r.U32(), y = r.U32(), w = r.U32(), h = r.U32();
    if (x > 4096 || y > 4096 || w > 512 || h > 512) Fail(fmt::format("{}: glifo corrupto", e.name));
    g = {uint16_t(x), uint16_t(y), uint16_t(w), uint16_t(h), r.U8()};
  }
  const uint32_t page_count = r.U32();
  if (page_count == 0 || page_count > 16) Fail(fmt::format("{}: paginas no soportadas", e.name));
  std::vector<uint32_t> page_refs(page_count);
  for (uint32_t& ref : page_refs) ref = r.U32();

  Font font;
  font.kerning = r.I32();
  const uint32_t remap_count = r.U32();
  for (uint32_t i = 0; i < remap_count; ++i) {
    const uint16_t codepoint = r.U16();
    const uint16_t index = r.U16();
    if (index >= glyphs.size()) Fail(fmt::format("{}: tabla de caracteres corrupta", e.name));
    font.glyphs[codepoint] = glyphs[index];
  }
  for (uint32_t ref : page_refs) {
    if (ref == 0 || ref > pkg.exports().size()) Fail(fmt::format("{}: pagina fuera de tabla", e.name));
    font.pages.push_back(DecodeTexture(pkg, pkg.exports()[ref - 1]));
  }
  for (const auto& [codepoint, g] : font.glyphs) {
    if (g.page >= font.pages.size() || g.x + g.w > font.pages[g.page].width ||
        g.y + g.h > font.pages[g.page].height) {
      Fail(fmt::format("{}: glifo U+{:04X} fuera de su pagina", e.name, codepoint));
    }
    font.height = std::max(font.height, g.h);
  }
  return font;
}

// Carpeta de idioma preferida segun el idioma de la consola emulada. Las
// carpetas del disco son int, fra, deu, ita, spa y jpn (antes se buscaban
// eng/fre/ger, que no existen: en esos idiomas caia siempre al espanol).
std::vector<std::string> LanguagePreference() {
  std::vector<std::string> order;
  order.push_back(LangFolder(GameLanguage()));
  for (const char* l : {"int", "spa", "fra", "deu", "ita", "jpn"}) {
    if (std::find(order.begin(), order.end(), l) == order.end()) order.push_back(l);
  }
  return order;
}

}  // namespace

bool Load(const DiscSource& disc, MenuAssets& out, std::string& error) {
  try {
    const auto reader = DiscReader::Open(disc);
    if (!reader) Fail("no se puede abrir el disco");
    Fpi fpi(ReadDiscFile(*reader, "LO.fpi"));
    const auto files = fpi.Files([](const std::string& path) {
      return path.starts_with("bin\\xenon\\loc\\") &&
             (path.find("\\menu\\rpfontscommon_") != std::string::npos ||
              path.find("\\menu\\rpmenurescommon_") != std::string::npos);
    });
    auto load_package = [&](const std::string& family, const std::string& lang) -> std::unique_ptr<Package> {
      const auto it = files.find(fmt::format("bin\\xenon\\loc\\{0}\\menu\\{1}_{0}.xxx", lang, family));
      if (it == files.end()) return nullptr;
      return std::make_unique<Package>(
          CpxDecode(ReadDiscFile(*reader, it->second.archive, it->second.offset, it->second.size)));
    };

    for (const std::string& lang : LanguagePreference()) {
      auto menu = load_package("rpmenurescommon", lang);
      auto fonts = load_package("rpfontscommon", lang);
      if (!menu || !fonts) continue;
      MenuAssets assets;
      assets.language = lang;
      assets.ui_main = DecodeTexture(*menu, menu->Find("UI_MAIN_00", "Texture2D"));
      assets.window = DecodeTexture(*menu, menu->Find("window", "Texture2D"));
      assets.text = DecodeFont(*fonts, fonts->Find("Maru23", "Font"));
      assets.title = DecodeFont(*fonts, fonts->Find("LocTit1", "Font"));
      out = std::move(assets);
      return true;
    }
    error = "no hay paquetes de menu en LO.fpi";
  } catch (const ParseError& e) {
    error = e.what;
  } catch (const std::exception& e) {
    error = e.what();
  }
  return false;
}

}  // namespace lo::menu_assets
