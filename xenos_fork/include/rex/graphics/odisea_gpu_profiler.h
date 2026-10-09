// Fork (odisea): perfilador de tiempo de GPU por categorias con marcas de tiempo
// (timestamps) de la propia tarjeta. Comun a D3D12 y Vulkan: cada backend
// reserva un rango de consultas por envio, escribe una marca donde el
// perfilador se lo pide y, cuando el envio termina, le entrega los valores.
// Se activa en caliente con el cvar odisea_gpu_profile (lo_live_tuning.txt) y
// escribe un informe cada odisea_gpu_profile_period segundos en el registro y
// en odisea_gpu_profile.txt junto al ejecutable.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

namespace rex::graphics::odisea {

enum class GpuProfileCat : uint8_t {
  kBase = 0,      // marca inicial del envio (no suma tiempo)
  kPrepIndex,     // conversion de indices (procesador de primitivas)
  kPrepEdram,     // cache de destinos de render (EDRAM, render pass)
  kPrepTextures,  // cargas de texturas (guest y pack)
  kPrepVertex,    // subidas de memoria del guest (vertices, memexport) con barreras
  kPrepOther,     // resto antes del dibujo (barreras, constantes, descriptores)
  kOpaque,        // dibujos con sombreador de pixeles del juego sin mezcla
  kBlend,      // dibujos con mezcla (efectos, transparencias)
  kDepthOnly,  // dibujos sin sombreador de pixeles (profundidad, sombras)
  kResolve,    // resolves (EDRAM a memoria del guest)
  kSwap,       // presentacion: gamma, FXAA/SMAA, copia al presentador
  kOther,      // resto de un envio que no es de presentacion
  kCount
};

// RB_BLENDCONTROL del destino 0 con mezcla desactivada de hecho (src One, dst
// Zero, suma, en color y alfa): el dibujo cuenta como opaco.
inline bool BlendControlIsOpaque(uint32_t rb_blendcontrol0) {
  return (rb_blendcontrol0 & 0x1FFF1FFFu) == 0x00010001u;
}

// Clave de una carga de textura para el informe (kPrepTextures): tamano, formato
// del guest, si es un resolve escalado o una imagen del pack, y pagina base.
inline uint64_t TextureProfileHash(uint32_t width, uint32_t height, uint32_t format, bool scaled,
                                   bool pack, uint32_t base_page) {
  return (uint64_t(width & 0xFFFF) << 48) | (uint64_t(height & 0xFFFF) << 32) |
         (uint64_t(format & 0x3F) << 26) | (uint64_t(scaled) << 25) | (uint64_t(pack) << 24) |
         uint64_t(base_page & 0xFFFFFF);
}

// Contadores acumulados de las subidas de memoria del guest (shared_memory.cpp):
// eventos de subida (cada uno con sus barreras), paginas pedidas por los dibujos
// y paginas subidas de mas por la agrupacion (odisea_upload_dirty_batch).
void SharedMemoryUploadStats(uint64_t& events, uint64_t& pages_requested,
                             uint64_t& pages_speculative, uint32_t& page_size_log2);

class GpuProfiler {
 public:
  // Tamano del anillo de consultas de cada backend.
  static constexpr uint32_t kQueryCount = 32768;
  // Sitio minimo que debe quedar libre para perfilar un envio.
  static constexpr uint32_t kMinSubmissionQueries = 1024;

  // Lector de marcas de un rango: copia count valores en out; false si no estan.
  using ReadFn = std::function<bool(uint32_t first, uint32_t count, uint64_t* out)>;

  GpuProfiler();

  // Si el cvar esta activo (barato, se consulta al abrir cada envio).
  static bool Enabled();

  // Al abrir un envio: true si hay que perfilarlo; entonces el backend debe
  // escribir la marca base en base_index y, en Vulkan, reiniciar
  // reserved_count consultas desde base_index.
  bool BeginSubmission(uint32_t& base_index, uint32_t& reserved_count);
  bool recording() const { return recording_; }
  // Pide un indice para una marca; false si no se graba o no queda sitio.
  bool Mark(GpuProfileCat cat, uint64_t shader_hash, uint32_t& index);
  // Al cerrar el envio (submission = su numero, el que luego se compara con el
  // de envios completados): rango escrito (count incluye la base; 0 si no se grabo).
  void EndSubmission(uint64_t submission, uint32_t& first, uint32_t& count);
  // Un fotograma presentado (para las medias por fotograma).
  void NoteFrame();
  // Envios terminados hasta completed_submission: lee sus marcas y acumula.
  // ms_per_tick: milisegundos por unidad de la marca; tick_mask: bits validos.
  void SubmissionsCompleted(uint64_t completed_submission, double ms_per_tick, uint64_t tick_mask,
                            const ReadFn& read, const char* backend);

 private:
  struct Entry {
    uint64_t hash;
    GpuProfileCat cat;
  };
  struct Range {
    uint64_t submission;
    uint32_t first;
    uint32_t count;
  };
  struct ShaderStat {
    double ms = 0.0;
    uint32_t draws = 0;
    GpuProfileCat cat = GpuProfileCat::kOther;
  };

  void MaybeReport(const char* backend);
  void ResetAccumulators();

  std::vector<Entry> entries_;
  std::vector<uint64_t> read_buffer_;
  std::deque<Range> pending_;
  uint32_t cursor_ = 0;
  uint32_t range_first_ = 0;
  uint32_t range_count_ = 0;
  uint32_t range_limit_ = 0;
  bool recording_ = false;

  double cat_ms_[size_t(GpuProfileCat::kCount)] = {};
  uint64_t cat_n_[size_t(GpuProfileCat::kCount)] = {};
  std::unordered_map<uint64_t, ShaderStat> shaders_;
  uint64_t frames_ = 0;
  uint64_t marks_dropped_ = 0;
  uint64_t submissions_read_ = 0;
  double period_start_s_ = 0.0;
  bool was_enabled_ = false;
  uint64_t upload_events_last_ = 0;
  uint64_t upload_pages_requested_last_ = 0;
  uint64_t upload_pages_speculative_last_ = 0;
};

}  // namespace rex::graphics::odisea
