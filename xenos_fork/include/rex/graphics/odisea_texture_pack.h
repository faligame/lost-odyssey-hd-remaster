// Fork (odisea): volcado de texturas y pack de sustitución (HD),
// compartido por los backends D3D12 y Vulkan.
//
// Flujo completo:
//   1. Con odisea_dump_textures se vuelcan las texturas del juego a
//      dump/textures/tex_<HASH>_<w>x<h>_f<fmt>.png (ver
//      odisea_texture_dump.cpp).
//   2. El usuario reescala/redibuja esos PNG (cualquier tamaño, RGBA) y los
//      deja en la carpeta del pack conservando el prefijo tex_<HASH>.
//   3. Con odisea_texture_pack, al crear cada textura se calcula el hash de
//      sus bytes en memoria del guest; si hay un PNG con ese hash, el recurso
//      de host se crea con el tamaño del PNG y se sube su contenido en vez de
//      la textura original. El juego sigue viendo el tamaño original (las
//      coordenadas son normalizadas), así que no hay que tocar sus shaders.
//
// Nada de aquí depende de una API gráfica: el hash, el índice del pack, la
// decodificación de PNG y la generación de mips son iguales en los dos
// backends. Lo único propio de cada uno es crear el recurso con el tamaño del
// PNG y subir los bytes.
#ifndef REX_GRAPHICS_XENOS1080_TEXTURE_PACK_H_
#define REX_GRAPHICS_XENOS1080_TEXTURE_PACK_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/system/xmemory.h>

// Definido en src/graphics/odisea_texture_replace.cpp.
REXCVAR_DECLARE(bool, odisea_dump_textures);

namespace rex::graphics::odisea {

// key desglosada (TextureKey es protegido en TextureCache): dirección física
// del nivel base, formato xenos::TextureFormat, endianness, dimensión
// (1=2D, 2=3D).
struct DumpTextureDesc {
  uint32_t address;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  bool tiled;
  uint32_t endian;
  uint32_t dimension;
  uint32_t mips;
};

bool DumpGuestTexturePng(const DumpTextureDesc& d, const texture_util::TextureGuestLayout& layout,
                         memory::Memory* memory);

// Hash del contenido de la textura en memoria del guest (0 si no se puede
// leer). Es la clave que usan tanto el volcado como el pack de sustitución.
uint64_t HashGuestTexture(const DumpTextureDesc& d, const texture_util::TextureGuestLayout& layout,
                          memory::Memory* memory);

// Formato de la imagen del pack. Los PNG se suben como RGBA8 (4 bytes por
// texel y los mips se calculan al vuelo). Los DDS vienen ya comprimidos por
// bloques de 4x4 y con su cadena de mips: no hay nada que decodificar ni que
// calcular, y ocupan entre 4 y 8 veces menos memoria de video.
enum class ReplacementFormat : uint8_t { kRGBA8, kBC1, kBC3, kBC7 };

struct ReplacementImage {
  uint32_t width = 0;
  uint32_t height = 0;
  ReplacementFormat format = ReplacementFormat::kRGBA8;
  // PNG: solo el nivel 0 (RGBA8, filas contiguas). DDS: todos los niveles del
  // fichero, uno detras de otro, empezando cada uno en level_offsets[nivel].
  std::vector<uint8_t> rgba;
  std::vector<size_t> level_offsets;
};

// Bytes por bloque de 4x4 (0 si la imagen no va comprimida).
inline uint32_t ReplacementBlockBytes(ReplacementFormat format) {
  switch (format) {
    case ReplacementFormat::kBC1:
      return 8;
    case ReplacementFormat::kBC3:
    case ReplacementFormat::kBC7:
      return 16;
    default:
      return 0;
  }
}

// Filas de datos de un nivel y bytes de cada una: texels en RGBA8, bloques en
// los formatos comprimidos.
inline void ReplacementLevelLayout(const ReplacementImage& image, uint32_t level,
                                   uint32_t& width_out, uint32_t& height_out,
                                   uint32_t& row_bytes_out, uint32_t& rows_out) {
  width_out = std::max(image.width >> level, uint32_t(1));
  height_out = std::max(image.height >> level, uint32_t(1));
  uint32_t block_bytes = ReplacementBlockBytes(image.format);
  if (block_bytes) {
    row_bytes_out = ((width_out + 3) / 4) * block_bytes;
    rows_out = (height_out + 3) / 4;
  } else {
    row_bytes_out = width_out * 4;
    rows_out = height_out;
  }
}

// Índice del pack (hash -> fichero). Se construye una vez por sesión.
bool TexturePackEnabled();

// Imagen de sustitución para ese hash, o nullptr. La imagen se cachea.
const ReplacementImage* FindReplacement(uint64_t hash);

// Texturas sustituidas, identificadas por su TextureKey (los 128 bits de la
// clave del guest). NO se puede usar el puntero del recurso: los backends
// reutilizan esas direcciones al destruir texturas y se confundirían unas con
// otras.
void SetReplacementForKey(uint64_t key_lo, uint64_t key_hi, uint64_t hash);

// Carga en segundo plano (cvar odisea_texture_pack_async): la textura con esta
// clave se creo con la imagen original porque su HD se esta leyendo; cuando este
// lista, TexturePackTakeReadyKeys la devuelve para que la cache la expulse y se
// vuelva a crear ya con la HD.
void NotePendingReplacement(uint64_t key_lo, uint64_t key_hi, uint64_t hash);
// Contenido (huella) con el que se creo una textura que puede llevar sustitucion. El juego reutiliza
// direcciones: al cargar otra escena, otra textura del mismo tamano y formato puede ocupar la misma
// llave, y el cache solo recarga los datos en el mismo objeto (8-oct-2026: personajes con la HD de
// otros hasta recargar el pack).
void NoteTextureContent(uint64_t key_lo, uint64_t key_hi, uint64_t content_hash);
bool TextureContentTracked(uint64_t key_lo, uint64_t key_hi);
// Al recargar los datos: true si la huella ha cambiado. Entonces, si la textura llevaba una HD o el
// contenido nuevo tiene una, la textura se expulsa (TexturePackTakeReadyKeys) para crearla de nuevo
// con lo que le toca.
bool TextureContentChanged(uint64_t key_lo, uint64_t key_hi, uint64_t content_hash);
void TexturePackTakeReadyKeys(std::vector<std::pair<uint64_t, uint64_t>>& out);

// Cuenta las expulsiones de texturas del pack (cache.cpp). La textura nueva tiene
// la MISMA clave que la expulsada, asi que el backend D3D12, que reutiliza los
// descriptores de textura mientras las claves no cambien, seguiria apuntando a la
// vieja ya destruida (la GPU lee memoria liberada: DEVICE_HUNG al cargar zonas).
// Cuando cambia este numero hay que reescribir los descriptores.
uint64_t TexturePackEvictionGeneration();
const ReplacementImage* FindReplacementForKey(uint64_t key_lo, uint64_t key_hi);

// El juego invalida la memoria de muchas texturas cada poco y el cache las
// vuelve a cargar. Con la textura original eso es barato; con un PNG de
// 4096x4096 es rehacer los mips y subir ~85 MB cada vez. El PNG no cambia, así
// que basta con subirlo UNA vez por textura creada: la primera llamada devuelve
// true (hay que subir) y las siguientes false, hasta que la textura se vuelve a
// crear (SetReplacementForKey) o se recarga el pack.
bool ReplacementNeedsUpload(uint64_t key_lo, uint64_t key_hi);

// Textura vigilada (cvar odisea_watch_texture = hash hex del pack): se
// identifica al crearse (SetReplacementForKey) y el cache marca cada dibujo que
// la usa. El exe lo lee con odisea_WatchedTextureLastSeenMs (exportada).
bool TextureWatchActive();
// Pagina fisica de 4 KB del nivel base (TextureKey::base_page) de la textura vigilada.
uint32_t WatchedTextureBasePage();
bool IsWatchedTextureKey(uint64_t key_lo, uint64_t key_hi);
void MarkWatchedTextureSeen();
int64_t WatchedTextureLastSeenMs();

// Recarga en caliente del pack (boton del menu F2 -> cvar
// odisea_texture_pack_reload). Va en dos fases porque las texturas ya
// creadas siguen enlazadas con el formato y el tamano del PNG: primero se pide
// el vaciado de cachés, y solo cuando esas texturas han desaparecido se tiran
// el indice y las imagenes.
enum class TexturePackReloadStep { kNone, kRequestCacheClear, kFinish };
TexturePackReloadStep TexturePackPollReload();

// Los 128 bits de una TextureKey como dos uint64, para identificar sin
// ambigüedad las texturas sustituidas por el pack.
template <typename KeyT>
inline void SplitTextureKey(const KeyT& key, uint64_t& lo, uint64_t& hi) {
  uint64_t bits[2] = {0, 0};
  std::memcpy(bits, &key, sizeof(key) < sizeof(bits) ? sizeof(key) : sizeof(bits));
  lo = bits[0];
  hi = bits[1];
}

// Cuántos niveles de mip da una imagen de ese tamaño, bajando hasta 1x1.
inline uint32_t MipLevelsAvailable(uint32_t width, uint32_t height) {
  uint32_t levels = 1;
  for (uint32_t w = width, h = height; w > 1 || h > 1; ++levels) {
    w = w > 1 ? (w >> 1) : 1;
    h = h > 1 ? (h >> 1) : 1;
  }
  return levels;
}

// Cadena de mips del PNG con un filtro de caja 2x2. El juego pide todos los
// niveles y sin ellos el muestreo minificado leería basura. Devuelve
// `levels - 1` imágenes (el nivel 0 es la propia imagen del pack).
inline std::vector<std::vector<uint8_t>> BuildMipChain(const ReplacementImage& image,
                                                       uint32_t levels) {
  std::vector<std::vector<uint8_t>> mips;
  mips.reserve(levels ? levels - 1 : 0);
  const uint8_t* prev = image.rgba.data();
  uint32_t pw = image.width, ph = image.height;
  for (uint32_t level = 1; level < levels; ++level) {
    uint32_t nw = pw > 1 ? (pw >> 1) : 1;
    uint32_t nh = ph > 1 ? (ph >> 1) : 1;
    std::vector<uint8_t> cur(size_t(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y) {
      uint32_t y0 = std::min(y * 2, ph - 1), y1 = std::min(y * 2 + 1, ph - 1);
      const uint8_t* r0 = prev + size_t(y0) * pw * 4;
      const uint8_t* r1 = prev + size_t(y1) * pw * 4;
      uint8_t* dst = cur.data() + size_t(y) * nw * 4;
      for (uint32_t x = 0; x < nw; ++x) {
        uint32_t x0 = std::min(x * 2, pw - 1) * 4, x1 = std::min(x * 2 + 1, pw - 1) * 4;
        for (uint32_t c = 0; c < 4; ++c) {
          dst[x * 4 + c] =
              uint8_t((uint32_t(r0[x0 + c]) + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) >> 2);
        }
      }
    }
    mips.push_back(std::move(cur));
    prev = mips.back().data();
    pw = nw;
    ph = nh;
  }
  return mips;
}

// Niveles que puede dar la imagen: los del fichero en un DDS, todos los que
// salgan de reducir a la mitad en un PNG.
inline uint32_t ReplacementLevelsAvailable(const ReplacementImage& image) {
  if (image.format != ReplacementFormat::kRGBA8) {
    return uint32_t(image.level_offsets.size());
  }
  return MipLevelsAvailable(image.width, image.height);
}

// Punteros a los datos de los `levels` primeros niveles. En un PNG los mips se
// generan en `storage` (que debe vivir mientras se usen los punteros); en un
// DDS apuntan directamente al fichero ya cargado.
inline std::vector<const uint8_t*> ReplacementLevelData(
    const ReplacementImage& image, uint32_t levels, std::vector<std::vector<uint8_t>>& storage) {
  std::vector<const uint8_t*> data(levels);
  if (image.format != ReplacementFormat::kRGBA8) {
    for (uint32_t level = 0; level < levels; ++level) {
      data[level] = image.rgba.data() + image.level_offsets[level];
    }
    return data;
  }
  storage = BuildMipChain(image, levels);
  for (uint32_t level = 0; level < levels; ++level) {
    data[level] = level ? storage[level - 1].data() : image.rgba.data();
  }
  return data;
}

}  // namespace rex::graphics::odisea

#endif  // REX_GRAPHICS_XENOS1080_TEXTURE_PACK_H_
