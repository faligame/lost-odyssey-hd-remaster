// Fork (odisea): volcado de texturas del guest a PNG.
//
// Al cargar una textura (D3D12TextureCache::LoadTextureDataFromResidentMemoryImpl)
// se copian sus bytes del nivel base de forma segura, se calcula un hash del
// contenido (XXH3, estable entre sesiones: la dirección no lo es), se decodifica
// en CPU (tiling 2D de Xenos, byteswap, DXT1/DXT3/DXT5/8888/8) y se escribe
// dump/textures/tex_<hash>_<w>x<h>_f<fmt>.png (+ index.txt). Pensado para que
// el usuario prepare packs de texturas HD / glifos; la sustitución por hash se
// hará en el mismo punto de carga.
#include <rex/graphics/odisea_texture_pack.h>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace rex::graphics::odisea {

namespace {

// Hash de contenido propio (FNV-1a 64): estable entre sesiones y sin depender
// de xxhash, cuya versión en línea abortaba el hilo de GPU en este plugin.
uint64_t HashBytes(const uint8_t* p, size_t n) {
  // FNV-1a por palabras de 8 bytes (rápido y estable entre sesiones).
  uint64_t h = 1469598103934665603ull;
  size_t words = n / 8;
  for (size_t i = 0; i < words; ++i) {
    uint64_t w;
    std::memcpy(&w, p + i * 8, 8);
    h = (h ^ w) * 1099511628211ull;
  }
  for (size_t i = words * 8; i < n; ++i) {
    h = (h ^ p[i]) * 1099511628211ull;
  }
  return h ^ uint64_t(n);
}

// --- Lectura segura de memoria del guest --------------------------------------
bool RangeReadable(const uint8_t* p, size_t n) {
  const uint8_t* end = p + n;
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    if (mbi.Protect & PAGE_GUARD) return false;
    if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE) return false;
    p = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  }
  return true;
}

bool SafeCopy(uint8_t* dst, const uint8_t* src, size_t n) {
  __try {
    std::memcpy(dst, src, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// --- Escritor PNG mínimo (deflate "stored", sin compresión) --------------------
uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void Put32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24));
  v.push_back(uint8_t(x >> 16));
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}

void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  Put32(out, uint32_t(data.size()));
  size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  Put32(out, Crc32(out.data() + start, out.size() - start));
}

bool WritePngRgba(const std::filesystem::path& path, uint32_t w, uint32_t h,
                  const std::vector<uint8_t>& rgba) {
  // Raw scanlines con filtro 0.
  std::vector<uint8_t> raw;
  raw.reserve((size_t(w) * 4 + 1) * h);
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
  }
  // zlib: cabecera + bloques stored de <= 65535 + adler32.
  std::vector<uint8_t> z;
  z.push_back(0x78);
  z.push_back(0x01);
  size_t pos = 0;
  while (pos < raw.size() || raw.empty()) {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    bool last = pos + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t((~n) >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
    if (raw.empty()) break;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  Put32(z, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  Put32(ihdr, w);
  Put32(ihdr, h);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(6);  // RGBA
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", z);
  Chunk(png, "IEND", {});
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
  fwrite(png.data(), 1, png.size(), f);
  fclose(f);
  return true;
}

// --- Decodificación -------------------------------------------------------------
void Swap(std::vector<uint8_t>& d, xenos::Endian e) {
  if (e == xenos::Endian::k8in16) {
    for (size_t i = 0; i + 1 < d.size(); i += 2) std::swap(d[i], d[i + 1]);
  } else if (e == xenos::Endian::k8in32) {
    for (size_t i = 0; i + 3 < d.size(); i += 4) {
      std::swap(d[i], d[i + 3]);
      std::swap(d[i + 1], d[i + 2]);
    }
  } else if (e == xenos::Endian::k16in32) {
    for (size_t i = 0; i + 3 < d.size(); i += 4) {
      std::swap(d[i], d[i + 2]);
      std::swap(d[i + 1], d[i + 3]);
    }
  }
}

struct Rgba {
  uint8_t r, g, b, a;
};

void C565(uint16_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
  r = uint8_t(((v >> 11) & 31) * 255 / 31);
  g = uint8_t(((v >> 5) & 63) * 255 / 63);
  b = uint8_t((v & 31) * 255 / 31);
}

void DecodeDxtColor(const uint8_t* blk, Rgba out[16], bool dxt1) {
  uint16_t c0 = uint16_t(blk[0] | (blk[1] << 8)), c1 = uint16_t(blk[2] | (blk[3] << 8));
  uint32_t idx = uint32_t(blk[4] | (blk[5] << 8) | (blk[6] << 16) | (uint32_t(blk[7]) << 24));
  Rgba pal[4];
  C565(c0, pal[0].r, pal[0].g, pal[0].b);
  C565(c1, pal[1].r, pal[1].g, pal[1].b);
  pal[0].a = pal[1].a = 255;
  if (c0 > c1 || !dxt1) {
    pal[2] = {uint8_t((2 * pal[0].r + pal[1].r) / 3), uint8_t((2 * pal[0].g + pal[1].g) / 3),
              uint8_t((2 * pal[0].b + pal[1].b) / 3), 255};
    pal[3] = {uint8_t((pal[0].r + 2 * pal[1].r) / 3), uint8_t((pal[0].g + 2 * pal[1].g) / 3),
              uint8_t((pal[0].b + 2 * pal[1].b) / 3), 255};
  } else {
    pal[2] = {uint8_t((pal[0].r + pal[1].r) / 2), uint8_t((pal[0].g + pal[1].g) / 2),
              uint8_t((pal[0].b + pal[1].b) / 2), 255};
    pal[3] = {0, 0, 0, 0};
  }
  for (int i = 0; i < 16; ++i) out[i] = pal[(idx >> (2 * i)) & 3];
}

void DecodeDxt5Alpha(const uint8_t* blk, uint8_t out[16]) {
  uint8_t a0 = blk[0], a1 = blk[1];
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(blk[2 + i]) << (8 * i);
  uint8_t pal[8] = {a0, a1};
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) pal[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (int i = 1; i < 5; ++i) pal[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    pal[6] = 0;
    pal[7] = 255;
  }
  for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (3 * i)) & 7];
}

}  // namespace

namespace {
bool DumpGuestTexturePngImpl(const DumpTextureDesc& key,
                             const texture_util::TextureGuestLayout& layout,
                             memory::Memory* memory) {
  static std::mutex mutex;
  static std::set<uint64_t> done;
  static std::set<uint64_t> seen_slots;
  if (!memory) return false;
  uint32_t address = key.address;
  uint32_t bytes = layout.base.level_data_extent_bytes;
  if (!address || !bytes || bytes > 64u * 1024 * 1024) return false;
  if (uint64_t(address) + bytes > 0x20000000ull) return false;
  // El juego recarga sus render targets como textura cada frame: sin este
  // filtro por (dirección, tamaño, formato) se volcarían miles de veces.
  uint64_t slot = (uint64_t(address) << 32) ^ (uint64_t(bytes) << 8) ^ key.format;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!seen_slots.insert(slot).second) return true;
  }
  REXGPU_INFO("odisea texture dump: procesando @0x{:08X} {}x{} fmt {} tiled {} endian {} bytes {}",
              address, key.width, key.height, key.format, key.tiled, key.endian, bytes);
  rex::FlushLogging();
  const uint8_t* src = memory->TranslatePhysical<const uint8_t*>(address);
  if (!src || !RangeReadable(src, bytes)) {
    return false;
  }
  std::vector<uint8_t> data(bytes);
  if (!SafeCopy(data.data(), src, bytes)) return false;

  uint64_t hash = HashBytes(data.data(), data.size());
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!done.insert(hash).second) return true;
  }
  uint32_t w = key.width, h = key.height;
  uint32_t fmt = key.format;
  std::error_code ec;
  std::filesystem::create_directories("dump/textures", ec);
  char name[128];
  snprintf(name, sizeof(name), "tex_%016llX_%ux%u_f%u.png", (unsigned long long)hash, w, h, fmt);
  {
    // Índice: hash, dirección de esta sesión, formato, tiling, endianness.
    FILE* idx = nullptr;
    if (_wfopen_s(&idx, L"dump/textures/index.txt", L"ab") == 0 && idx) {
      fprintf(idx, "%s addr=0x%08X fmt=%u tiled=%u endian=%u pitch_bytes=%u bytes=%u dim=%u mips=%u\n", name,
              address, fmt, uint32_t(key.tiled), key.endian,
              layout.base.row_pitch_bytes, bytes, key.dimension, key.mips);
      fclose(idx);
    }
  }
  std::filesystem::path out = std::filesystem::path("dump/textures") / name;
  if (std::filesystem::exists(out, ec)) return true;
  if (key.dimension == uint32_t(xenos::DataDimension::k3D) || w == 0 || h == 0 || w > 8192 || h > 8192) {
    return false;
  }

  Swap(data, xenos::Endian(key.endian));
  std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
  auto put = [&](uint32_t x, uint32_t y, Rgba c) {
    if (x >= w || y >= h) return;
    uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
  };
  bool tiled = key.tiled;
  auto block_offset = [&](uint32_t bx, uint32_t by, uint32_t blocks_per_row, uint32_t bpb,
                          uint32_t bpb_log2) -> size_t {
    if (tiled) {
      return size_t(texture_util::GetTiledOffset2D(int32_t(bx), int32_t(by), blocks_per_row, bpb_log2));
    }
    return (size_t(by) * blocks_per_row + bx) * bpb;
  };

  if (fmt == 18 || fmt == 19 || fmt == 20) {  // DXT1 / DXT2_3 / DXT4_5
    uint32_t bpb = fmt == 18 ? 8 : 16, bpb_log2 = fmt == 18 ? 3 : 4;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint32_t blocks_per_row = layout.base.row_pitch_bytes ? layout.base.row_pitch_bytes / bpb : bw;
    if (blocks_per_row < bw) blocks_per_row = bw;
    for (uint32_t by = 0; by < bh; ++by) {
      for (uint32_t bx = 0; bx < bw; ++bx) {
        size_t off = block_offset(bx, by, blocks_per_row, bpb, bpb_log2);
        if (off + bpb > data.size()) continue;
        const uint8_t* blk = data.data() + off;
        Rgba c[16];
        uint8_t a[16];
        if (fmt == 18) {
          DecodeDxtColor(blk, c, true);
        } else {
          DecodeDxtColor(blk + 8, c, false);
          if (fmt == 20) {
            DecodeDxt5Alpha(blk, a);
          } else {
            for (int i = 0; i < 16; ++i) a[i] = uint8_t(((blk[i / 2] >> ((i & 1) * 4)) & 15) * 17);
          }
          for (int i = 0; i < 16; ++i) c[i].a = a[i];
        }
        for (int i = 0; i < 16; ++i) put(bx * 4 + (i & 3), by * 4 + (i >> 2), c[i]);
      }
    }
  } else if (fmt == 6 || fmt == 2) {  // 8_8_8_8 (ARGB en memoria tras swap) / 8
    uint32_t bpp = fmt == 6 ? 4 : 1, bpp_log2 = fmt == 6 ? 2 : 0;
    uint32_t ppr = layout.base.row_pitch_bytes ? layout.base.row_pitch_bytes / bpp : w;
    if (ppr < w) ppr = w;
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        size_t off = block_offset(x, y, ppr, bpp, bpp_log2);
        if (off + bpp > data.size()) continue;
        const uint8_t* p = data.data() + off;
        if (fmt == 6) {
          put(x, y, {p[1], p[2], p[3], p[0]});
        } else {
          put(x, y, {p[0], p[0], p[0], 255});
        }
      }
    }
  } else {
    return false;  // formato no soportado por el decodificador CPU (queda en index.txt)
  }
  if (!WritePngRgba(out, w, h, rgba)) return false;
  REXGPU_INFO("odisea texture dump: {} ({}x{} fmt {} @0x{:08X})", name, w, h, fmt, address);
  return true;
}

}  // namespace

// Envoltorio con SEH: una textura mal formada no debe tumbar el hilo de GPU.
bool DumpGuestTexturePng(const DumpTextureDesc& key, const texture_util::TextureGuestLayout& layout,
                         memory::Memory* memory) {
  __try {
    return DumpGuestTexturePngImpl(key, layout, memory);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    REXGPU_ERROR("odisea texture dump: excepcion con @0x{:08X} {}x{} fmt {} (omitida)",
                 key.address, key.width, key.height, key.format);
    return false;
  }
}

// Hash público: lee los bytes del nivel base y los resume. Se usa al crear
// la textura (una vez por textura, no por frame) para buscarla en el pack.
uint64_t HashGuestTexture(const DumpTextureDesc& d, const texture_util::TextureGuestLayout& layout,
                          memory::Memory* memory) {
  if (!memory) return 0;
  uint32_t bytes = layout.base.level_data_extent_bytes;
  if (!d.address || !bytes || bytes > 64u * 1024 * 1024) return 0;
  if (uint64_t(d.address) + bytes > 0x20000000ull) return 0;
  const uint8_t* src = memory->TranslatePhysical<const uint8_t*>(d.address);
  if (!src || !RangeReadable(src, bytes)) return 0;
  std::vector<uint8_t> data(bytes);
  if (!SafeCopy(data.data(), src, bytes)) return 0;
  return HashBytes(data.data(), data.size());
}

}  // namespace rex::graphics::odisea
