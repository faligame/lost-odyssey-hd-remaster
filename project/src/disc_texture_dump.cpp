// lostodyssey - ReXGlue Recompiled Project
//
// Volcado de todas las texturas del juego desde los discos (ver
// disc_texture_dump.h). Pensado para hacer packs de texturas HD sin tener que
// recorrer el juego con odisea_dump_textures activado.
//
// Cadena: LO.fpi -> xenon_*.fpd -> cpx -> paquete UE3 (ue3_package.h) -> cada
// Texture2D -> mip 0 (LZO1X, ya en el mosaico de la Xbox 360 y con los bytes
// en el orden de la memoria de la consola).
//
// La clave del pack es el hash (FNV-1a 64 por palabras, igual que
// HashBytes de odisea_texture_dump.cpp) de los bytes del nivel base TAL
// COMO ESTAN EN LA MEMORIA DEL GUEST: level_data_extent_bytes, que para una
// textura en mosaico es ancho y alto en bloques alineados a 32 por bytes por
// bloque. El mip 0 del paquete es justo eso (el juego lo copia tal cual a
// memoria fisica), asi que se hashea el mismo rango. Al terminar se comparan
// los hashes con los de dump/textures (volcado en juego) y se escribe en el
// log cuantos coinciden: es la prueba de que el pack los reconocera.
//
// Salida (el pack busca en subcarpetas y acepta cualquier sufijo tras el hash):
//   dump/disc_textures/<tipo>/<ruta del paquete>/tex_<HASH>_<w>x<h>_f<fmt>_<nombre>.png
//   (tipo: color, normales = mapas de relieve, iluminacion = mapas de luz/sombra)
//   dump/disc_textures/indice.txt  (hash, tamano, formato, disco, paquete, nombre)
// Una textura repetida en varios paquetes se escribe una sola vez (la primera);
// el indice lista todas sus apariciones.

#include "disc_texture_dump.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <lzokay.hpp>
#include <rex/cvar.h>
#include <rex/logging.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../thirdparty/stb/stb_image_write.h"

#include "multidisc.h"
#include "ue3_package.h"

REXCVAR_DEFINE_BOOL(lo_dump_disc_textures, false, "LostOdyssey/Texturas",
                    "Al arrancar, vuelca todas las texturas de los discos a dump/disc_textures (para packs HD)");

namespace lo {

namespace {

using namespace lo::ue3;

// --- Formatos ------------------------------------------------------------------
// EPixelFormat de UE3 -> formato Xenos (el numero que usa el volcado en juego
// en el nombre, f<fmt>), tamano de bloque y orden de bytes en memoria.
struct Formato {
  uint32_t xenos = 0;
  uint32_t bloque = 1;       // pixeles por lado de bloque
  uint32_t bytes = 0;        // bytes por bloque
  uint32_t bytes_log2 = 0;
  uint32_t endian = 0;       // 0 ninguno, 1 8in16, 2 8in32
};

bool FormatoDe(int32_t ue3, Formato& f) {
  switch (ue3) {
    case 5: f = {18, 4, 8, 3, 1}; return true;   // DXT1
    case 6: f = {19, 4, 16, 4, 1}; return true;  // DXT3
    case 7: f = {20, 4, 16, 4, 1}; return true;  // DXT5
    case 2: f = {6, 1, 4, 2, 2}; return true;    // A8R8G8B8
    case 3: f = {2, 1, 1, 0, 0}; return true;    // G8
    default: return false;
  }
}

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

// --- Decodificacion (la misma que el volcado en juego) --------------------------
void C565(uint16_t v, uint8_t* c) {
  c[0] = uint8_t(((v >> 11) & 31) * 255 / 31);
  c[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
  c[2] = uint8_t((v & 31) * 255 / 31);
}

void DxtColor(const uint8_t* b, uint8_t out[16][4], bool dxt1) {
  const uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
  const uint32_t idx = uint32_t(b[4] | (b[5] << 8) | (b[6] << 16) | (uint32_t(b[7]) << 24));
  uint8_t pal[4][4] = {};
  C565(c0, pal[0]);
  C565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (c0 > c1 || !dxt1) {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int k = 0; k < 3; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
    pal[2][3] = 255;
  }
  for (int i = 0; i < 16; ++i) std::memcpy(out[i], pal[(idx >> (2 * i)) & 3], 4);
}

void Dxt5Alpha(const uint8_t* b, uint8_t out[16]) {
  const uint8_t a0 = b[0], a1 = b[1];
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
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

// datos = el nivel base tal como esta en memoria (en mosaico, sin intercambiar).
std::vector<uint8_t> Decodificar(Bytes datos, const Formato& f, uint32_t w, uint32_t h) {
  if (f.endian == 1) {
    for (size_t i = 0; i + 1 < datos.size(); i += 2) std::swap(datos[i], datos[i + 1]);
  } else if (f.endian == 2) {
    for (size_t i = 0; i + 3 < datos.size(); i += 4) {
      std::swap(datos[i], datos[i + 3]);
      std::swap(datos[i + 1], datos[i + 2]);
    }
  }
  std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
  const uint32_t bw = (w + f.bloque - 1) / f.bloque, bh = (h + f.bloque - 1) / f.bloque;
  const uint32_t fila = (bw + 31) & ~31u;
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const size_t off = TiledOffset2D(int32_t(bx), int32_t(by), fila, f.bytes_log2);
      if (off + f.bytes > datos.size()) continue;
      const uint8_t* b = datos.data() + off;
      uint8_t px[16][4];
      int n = 1;
      if (f.bloque == 4) {
        n = 16;
        if (f.xenos == 18) {
          DxtColor(b, px, true);
        } else {
          DxtColor(b + 8, px, false);
          uint8_t a[16];
          if (f.xenos == 20) {
            Dxt5Alpha(b, a);
          } else {
            for (int i = 0; i < 16; ++i) a[i] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
          }
          for (int i = 0; i < 16; ++i) px[i][3] = a[i];
        }
      } else if (f.xenos == 6) {
        px[0][0] = b[1];
        px[0][1] = b[2];
        px[0][2] = b[3];
        px[0][3] = b[0];
      } else {
        px[0][0] = px[0][1] = px[0][2] = b[0];
        px[0][3] = 255;
      }
      for (int i = 0; i < n; ++i) {
        const uint32_t x = bx * f.bloque + (i & 3), y = by * f.bloque + (i >> 2);
        if (x < w && y < h) std::memcpy(&rgba[(size_t(y) * w + x) * 4], px[i], 4);
      }
    }
  }
  return rgba;
}

bool EscribirPng(const std::filesystem::path& ruta, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
  std::ofstream f(ruta, std::ios::binary);
  if (!f) return false;
  stbi_write_png_compression_level = 6;
  const int ok = stbi_write_png_to_func(
      [](void* ctx, void* data, int size) {
        static_cast<std::ofstream*>(ctx)->write(static_cast<const char*>(data), size);
      },
      &f, int(w), int(h), 4, rgba.data(), int(w) * 4);
  return ok != 0 && bool(f);
}

std::string Limpio(std::string_view s) {
  std::string out;
  for (char c : s) {
    out.push_back((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ? c : '_');
  }
  return out.substr(0, 80);
}

// --- Estado ---------------------------------------------------------------------
struct Aparicion {
  uint32_t w, h, fmt, disco;
  std::string paquete, nombre, categoria;
};

struct Estado {
  std::mutex mutex;
  std::string linea = "Sin empezar.";
  std::unordered_map<uint64_t, std::vector<Aparicion>> vistas;
  std::map<std::string, uint32_t> sin_soporte;  // motivo -> cuantas
  std::map<std::string, uint32_t> tamanos;      // formato + (igual/mayor/menor) -> cuantas
  std::map<std::string, uint32_t> tipos;        // CompressionSettings/LODGroup -> cuantas (+ ejemplo)
  std::map<std::string, std::string> ejemplo_tipo;
  std::map<std::string, uint32_t> categorias;   // categoria -> texturas distintas
  std::atomic<uint32_t> paquetes_hechos{0};
  std::atomic<uint32_t> paquetes_fallidos{0};
  std::atomic<uint32_t> texturas{0};
  std::atomic<uint32_t> escritas{0};
  std::atomic<uint32_t> ya_estaban{0};
};

std::atomic<bool> g_en_marcha{false};
Estado* g_estado = new Estado();  // nunca se destruye: el hilo puede seguir al cerrar

void Anotar(std::map<std::string, uint32_t>& m, const std::string& k) {
  std::lock_guard lock(g_estado->mutex);
  ++m[k];
}

// Tipo de textura, para separar las de color (las que interesa redibujar) de
// las que no son una imagen: mapas de normales (relieve; se ven azul-violeta)
// y mapas de luz/sombra precalculados. Se decide con lo que guarda el propio
// motor: la clase, CompressionSettings (1 = TC_Normalmap, 3 = TC_NormalmapAlpha
// en esta version de UE3) y LODGroup (grupos *NormalMap).
std::string Categoria(std::string_view clase, const std::unordered_map<std::string, int32_t>& props,
                      const std::unordered_map<std::string, std::string>& nombres, const std::string& export_name) {
  std::string cs, lod;
  if (auto it = nombres.find("CompressionSettings"); it != nombres.end()) cs = it->second;
  else if (auto it2 = props.find("CompressionSettings"); it2 != props.end()) cs = std::to_string(it2->second);
  if (auto it = nombres.find("LODGroup"); it != nombres.end()) lod = it->second;
  else if (auto it2 = props.find("LODGroup"); it2 != props.end()) lod = std::to_string(it2->second);
  const std::string clave = fmt::format("{} cs={} lod={}", clase, cs.empty() ? "-" : cs, lod.empty() ? "-" : lod);
  {
    std::lock_guard lock(g_estado->mutex);
    if (++g_estado->tipos[clave] == 1) g_estado->ejemplo_tipo[clave] = export_name;
  }
  if (clase == "LightMapTexture2D" || clase == "ShadowMapTexture2D") return "iluminacion";
  if (cs == "1" || cs == "3" || cs.find("Normal") != std::string::npos || lod.find("NormalMap") != std::string::npos) {
    return "normales";
  }
  return "color";
}

// dump/disc_textures/<paquete> -> dump/disc_textures/<categoria>/<paquete>
std::filesystem::path CarpetaCategoria(const std::filesystem::path& carpeta, const std::string& categoria) {
  const std::filesystem::path raiz = "dump/disc_textures";
  return raiz / categoria / carpeta.lexically_relative(raiz);
}

// Una Texture2D: propiedades, SourceArt (vacio) y la lista de mips. Solo el mip 0.
void VolcarTextura(const Package& pkg, const Export& e, int disco, const std::string& paquete,
                   const std::filesystem::path& carpeta) {
  Reader r(pkg.bytes(), e.offset);
  std::unordered_map<std::string, std::string> nombres;
  auto props = ReadProperties(pkg, r, &nombres);
  const std::string categoria = Categoria(pkg.ClassName(e), props, nombres, e.name);
  const int32_t formato_ue3 = props.count("Format") ? props["Format"] : 0;
  Formato f;
  if (!FormatoDe(formato_ue3, f)) {
    Anotar(g_estado->sin_soporte, fmt::format("formato UE3 {}", formato_ue3));
    return;
  }
  // SourceArt: banderas, elementos, tamano, offset (+ datos si los hay).
  const uint32_t sa_flags = r.U32();
  r.U32();
  const uint32_t sa_size = r.U32();
  const uint32_t sa_off = r.U32();
  if (sa_off != r.pos()) Fail("SourceArt incoherente");
  if (!(sa_flags & 0x21) && sa_size && sa_size != 0xFFFFFFFFu) r.Take(sa_size);
  const uint32_t mips = r.U32();
  if (mips == 0) {
    Anotar(g_estado->sin_soporte, "sin mips");
    return;
  }
  const uint32_t flags = r.U32(), raw_size = r.U32(), stored = r.U32(), offset = r.U32();
  if ((flags & 0x21) || stored == 0 || stored == 0xFFFFFFFFu) {
    Anotar(g_estado->sin_soporte, fmt::format("mip 0 fuera del paquete (banderas 0x{:X})", flags));
    return;
  }
  if (offset != r.pos()) Fail("mip 0 incoherente");
  Bytes raw;
  if (flags & 0x10) {
    const size_t inicio = r.pos();
    r.Take(stored);
    Reader s(pkg.bytes(), inicio);
    if (s.U32() != 0x9E2A83C1) Fail("flujo LZO sin firma");
    const uint32_t bloque = s.U32();
    s.U32();
    const uint32_t decoded = s.U32();
    if (!bloque || decoded != raw_size || decoded > 64u * 1024 * 1024) Fail("flujo LZO incoherente");
    std::vector<std::pair<uint32_t, uint32_t>> trozos((decoded + bloque - 1) / bloque);
    for (auto& [st, de] : trozos) {
      st = s.U32();
      de = s.U32();
    }
    raw.resize(raw_size);
    size_t hecho = 0;
    for (const auto& [st, de] : trozos) {
      if (de > raw.size() - hecho) Fail("trozo LZO demasiado grande");
      const uint8_t* src = s.Take(st);
      size_t n = 0;
      if (lzokay::decompress(src, st, raw.data() + hecho, de, n) != lzokay::EResult::Success || n != de) {
        Fail("LZO invalido");
      }
      hecho += n;
    }
    if (hecho != raw.size()) Fail("faltan datos LZO");
  } else if (flags == 0) {
    const uint8_t* p = r.Take(stored);
    raw.assign(p, p + stored);
  } else {
    Anotar(g_estado->sin_soporte, fmt::format("compresion 0x{:X}", flags));
    return;
  }
  const int32_t w = r.I32(), h = r.I32();
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192) Fail("tamano de mip no valido");

  // Rango que hashea el pack: nivel base en mosaico, bloques alineados a 32.
  const uint32_t bw = (uint32_t(w) + f.bloque - 1) / f.bloque, bh = (uint32_t(h) + f.bloque - 1) / f.bloque;
  const size_t extension = size_t((bw + 31) & ~31u) * ((bh + 31) & ~31u) * f.bytes;
  Anotar(g_estado->tamanos, fmt::format("f{} {}", f.xenos,
                                        raw.size() == extension ? "igual" : (raw.size() > extension ? "mayor" : "menor")));
  if (raw.size() < extension) raw.resize(extension, 0);
  const uint64_t hash = HashBytes(raw.data(), extension);
  ++g_estado->texturas;

  bool primera;
  {
    std::lock_guard lock(g_estado->mutex);
    auto& lista = g_estado->vistas[hash];
    primera = lista.empty();
    lista.push_back({uint32_t(w), uint32_t(h), f.xenos, uint32_t(disco), paquete, e.name, categoria});
  }
  if (!primera) return;
  Anotar(g_estado->categorias, categoria);

  const std::string nombre =
      fmt::format("tex_{:016X}_{}x{}_f{}_{}.png", hash, w, h, f.xenos, Limpio(e.name));
  const std::filesystem::path ruta = CarpetaCategoria(carpeta, categoria) / nombre;
  std::error_code ec;
  if (std::filesystem::exists(ruta, ec)) {
    ++g_estado->ya_estaban;
    return;
  }
  raw.resize(extension);
  const auto rgba = Decodificar(std::move(raw), f, uint32_t(w), uint32_t(h));
  std::filesystem::create_directories(ruta.parent_path(), ec);
  if (EscribirPng(ruta, uint32_t(w), uint32_t(h), rgba)) ++g_estado->escritas;
}

struct Trabajo {
  int disco;
  std::string ruta;  // dentro del disco (bin\xenon\...\x.xxx)
  FpiFile fichero;
};

void VolcarPaquete(DiscReader& lector, const Trabajo& t, const std::filesystem::path& raiz) {
  std::string rel = t.ruta;
  if (rel.starts_with("bin\\xenon\\")) rel = rel.substr(10);
  if (const auto punto = rel.rfind('.'); punto != std::string::npos) rel.resize(punto);
  std::replace(rel.begin(), rel.end(), '\\', '/');
  const std::filesystem::path carpeta = raiz / std::filesystem::path(std::u8string(rel.begin(), rel.end()));
  try {
    Package pkg(CpxDecode(ReadDiscFile(lector, t.fichero.archive, t.fichero.offset, t.fichero.size)));
    for (const Export& e : pkg.exports()) {
      const std::string_view clase = pkg.ClassName(e);
      if (clase != "Texture2D" && clase != "LightMapTexture2D" && clase != "ShadowMapTexture2D") continue;
      try {
        VolcarTextura(pkg, e, t.disco, t.ruta, carpeta);
      } catch (const ParseError& err) {
        Anotar(g_estado->sin_soporte, "error: " + err.what.substr(0, 40));
      }
    }
  } catch (const ParseError& err) {
    ++g_estado->paquetes_fallidos;
    REXLOG_WARN("lo_texturas: {} no se pudo leer: {}", t.ruta, err.what);
  } catch (const std::exception& err) {
    ++g_estado->paquetes_fallidos;
    REXLOG_WARN("lo_texturas: {} no se pudo leer: {}", t.ruta, err.what());
  }
  ++g_estado->paquetes_hechos;
}

// Hashes del volcado en juego (dump/textures/tex_<HASH>_...): para comprobar
// que los del disco son los mismos que ve el pack.
std::unordered_set<uint64_t> HashesVolcadosEnJuego() {
  std::unordered_set<uint64_t> out;
  std::error_code ec;
  for (const auto& entrada : std::filesystem::directory_iterator("dump/textures", ec)) {
    const std::string n = entrada.path().filename().string();
    if (n.size() >= 20 && n.starts_with("tex_")) {
      out.insert(std::strtoull(n.substr(4, 16).c_str(), nullptr, 16));
    }
  }
  return out;
}

void Volcar() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  const auto inicio = std::chrono::steady_clock::now();
  const std::filesystem::path raiz = "dump/disc_textures";
  const auto discos = MultiDiscAllSources();
  if (discos.empty()) {
    std::lock_guard lock(g_estado->mutex);
    g_estado->linea = "No se encuentra ningún disco.";
    g_en_marcha = false;
    return;
  }

  // Lista de paquetes de todos los discos. Un paquete con la misma ruta y el
  // mismo tamano en otro disco es el mismo fichero: se lee una vez.
  std::vector<Trabajo> trabajos;
  std::set<std::pair<std::string, uint32_t>> repetidos;
  std::string discos_txt;
  for (const auto& d : discos) {
    try {
      auto lector = DiscReader::Open(d);
      if (!lector) continue;
      Fpi fpi(ReadDiscFile(*lector, "LO.fpi"));
      const auto ficheros = fpi.Files([](const std::string& p) { return p.ends_with(".xxx"); });
      uint32_t nuevos = 0;
      for (const auto& [ruta, fichero] : ficheros) {
        if (!repetidos.insert({ruta, fichero.size}).second) continue;
        trabajos.push_back({d.number, ruta, fichero});
        ++nuevos;
      }
      discos_txt += fmt::format(" disco {} ({} paquetes nuevos);", d.number, nuevos);
    } catch (const ParseError& err) {
      REXLOG_WARN("lo_texturas: disco {}: {}", d.number, err.what);
    }
  }
  REXLOG_INFO("lo_texturas: volcado de texturas de los discos:{} {} paquetes", discos_txt, trabajos.size());

  // Hilos: cada uno con su lector por disco.
  const uint32_t hilos = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
  std::atomic<size_t> siguiente{0};
  std::vector<std::thread> pool;
  for (uint32_t i = 0; i < hilos; ++i) {
    pool.emplace_back([&] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      std::map<int, std::unique_ptr<DiscReader>> lectores;
      for (size_t k; (k = siguiente.fetch_add(1)) < trabajos.size();) {
        const Trabajo& t = trabajos[k];
        auto& lector = lectores[t.disco];
        if (!lector) {
          for (const auto& d : discos) {
            if (d.number == t.disco) lector = DiscReader::Open(d);
          }
        }
        if (lector) VolcarPaquete(*lector, t, raiz);
      }
    });
  }
  while (g_estado->paquetes_hechos.load() < trabajos.size()) {
    {
      std::lock_guard lock(g_estado->mutex);
      g_estado->linea = fmt::format("Volcando: paquete {} de {}, {} texturas distintas.",
                                    g_estado->paquetes_hechos.load(), trabajos.size(), g_estado->vistas.size());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  for (auto& t : pool) t.join();

  // Indice y comprobacion contra el volcado en juego.
  const auto en_juego = HashesVolcadosEnJuego();
  uint32_t coinciden = 0;
  std::error_code ec;
  std::filesystem::create_directories(raiz, ec);
  {
    std::ofstream idx(raiz / "indice.txt", std::ios::binary);
    idx << "# hash  tamano  formato  disco  tipo  paquete  textura (una linea por aparicion)\n";
    std::lock_guard lock(g_estado->mutex);
    std::map<uint64_t, const std::vector<Aparicion>*> orden;
    for (const auto& [hash, lista] : g_estado->vistas) orden[hash] = &lista;
    for (const auto& [hash, lista] : orden) {
      if (en_juego.count(hash)) ++coinciden;
      for (const auto& a : *lista) {
        idx << fmt::format("{:016X} {}x{} f{} disco{} {} {} {}\n", hash, a.w, a.h, a.fmt, a.disco, a.categoria,
                           a.paquete, a.nombre);
      }
    }
  }

  const auto segundos =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - inicio).count();
  std::string sin_soporte, tamanos;
  {
    std::lock_guard lock(g_estado->mutex);
    for (const auto& [k, v] : g_estado->sin_soporte) sin_soporte += fmt::format(" {} x{};", k, v);
    for (const auto& [k, v] : g_estado->tamanos) tamanos += fmt::format(" {} x{};", k, v);
    g_estado->linea = fmt::format(
        "Hecho en {} s: {} texturas distintas ({} PNG nuevos) en dump/disc_textures. "
        "De las {} volcadas jugando, {} están aquí.",
        segundos, g_estado->vistas.size(), g_estado->escritas.load(), en_juego.size(), coinciden);
  }
  REXLOG_INFO("lo_texturas: {} paquetes ({} fallidos), {} texturas leidas, {} distintas, {} PNG escritos, "
              "{} ya estaban, {} s",
              trabajos.size(), g_estado->paquetes_fallidos.load(), g_estado->texturas.load(),
              g_estado->vistas.size(), g_estado->escritas.load(), g_estado->ya_estaban.load(), segundos);
  REXLOG_INFO("lo_texturas: comprobacion con el volcado en juego: {} de {} hashes de dump/textures estan en "
              "el disco",
              coinciden, en_juego.size());
  REXLOG_INFO("lo_texturas: mip 0 frente al rango del pack:{}", tamanos);
  REXLOG_INFO("lo_texturas: sin volcar:{}", sin_soporte.empty() ? " nada" : sin_soporte);
  {
    std::lock_guard lock(g_estado->mutex);
    std::string cats;
    for (const auto& [k, v] : g_estado->categorias) cats += fmt::format(" {} x{};", k, v);
    REXLOG_INFO("lo_texturas: por tipo (distintas):{}", cats);
    for (const auto& [k, v] : g_estado->tipos) {
      REXLOG_INFO("lo_texturas:   {} x{} (p. ej. {})", k, v, g_estado->ejemplo_tipo[k]);
    }
  }
  g_en_marcha = false;
}

}  // namespace

void DiscTextureDumpStart() {
  bool esperado = false;
  if (!g_en_marcha.compare_exchange_strong(esperado, true)) return;
  {
    std::lock_guard lock(g_estado->mutex);
    g_estado->linea = "Preparando...";
    g_estado->vistas.clear();
    g_estado->sin_soporte.clear();
    g_estado->tamanos.clear();
    g_estado->tipos.clear();
    g_estado->ejemplo_tipo.clear();
    g_estado->categorias.clear();
    g_estado->paquetes_hechos = 0;
    g_estado->paquetes_fallidos = 0;
    g_estado->texturas = 0;
    g_estado->escritas = 0;
    g_estado->ya_estaban = 0;
  }
  std::thread(Volcar).detach();
}

void DiscTextureDumpAutoStart() {
  if (REXCVAR_GET(lo_dump_disc_textures)) DiscTextureDumpStart();
}

bool DiscTextureDumpRunning() { return g_en_marcha.load(); }

std::string DiscTextureDumpStatus() {
  std::lock_guard lock(g_estado->mutex);
  return g_estado->linea;
}

}  // namespace lo
