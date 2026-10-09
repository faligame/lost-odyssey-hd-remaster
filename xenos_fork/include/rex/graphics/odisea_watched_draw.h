// Fork (odisea): dibujos que usan la textura vigilada por el exe
// (odisea_watch_texture). Comun a D3D12 y Vulkan.
#pragma once

#include <cstdint>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/system/xmemory.h>

namespace rex::graphics::odisea {

// Si el sombreador de pixeles lee la textura vigilada.
bool DrawUsesWatchedTexture(const RegisterFile& regs, const Shader* pixel_shader);

// Bufer de vertices modificado en la memoria del guest: el llamador debe
// invalidarlo en la memoria compartida (la escritura no pasa por la vigilancia
// de paginas) y marcar su fetch constant como no residente.
struct PatchedVertexRange {
  uint32_t address;
  uint32_t length;
  uint32_t fetch_constant;
};

// Diagnostico de vertices y constantes invalidos (cvar odisea_vertex_nan_check) y,
// con odisea_vertex_nan_fix, limpieza de los vertices de la CPU (devuelve lo tocado).
std::vector<PatchedVertexRange> CheckDrawForInvalidFloats(const RegisterFile& regs, const Shader& vertex_shader,
                                                          memory::Memory& memory);

// Diagnostico (cvar odisea_pass_check): vertices que cambian entre una pasada y la
// siguiente con test de profundidad 'igual'.
void CheckPassConsistency(const RegisterFile& regs, const Shader& vertex_shader, const Shader* pixel_shader,
                          memory::Memory& memory, uint64_t frame);

// Volcado de buferes de vertices de un vs (cvar odisea_dump_vs, en caliente).
struct IndexDumpInfo {
  uint32_t guest_base;
  uint32_t length;
  uint32_t count;
  uint32_t endian;
  bool is_32bit;
};
void DumpVertexBuffers(const RegisterFile& regs, const Shader& vertex_shader, const Shader* pixel_shader,
                       memory::Memory& memory, uint64_t frame, const void* index_info /* IndexDumpInfo* o null */);

// Interruptor de sombreadores (cvar odisea_skip_shader, en caliente).
// Dibujo de la interfaz: proyeccion 2D de diseno 1280x720, color y sin profundidad.
bool IsDesignHudDraw(const RegisterFile& regs);
// Alto de la salida con la interfaz aparte (odisea_hud_resolution), 0 = desactivado.
uint32_t HudOutputHeight();
// Ancho de esa salida (odisea_hud_output_width, o 16:9 si es 0).
uint32_t HudOutputWidth();
// Dibujo sobre la pantalla principal (backbuffer, viewport entero, destino 0 en 8_8_8_8).
bool IsHudMainSurfaceDraw(const RegisterFile& regs);
bool ShouldSkipDraw(const RegisterFile& regs, const Shader& vertex_shader, const Shader* pixel_shader);
// Direccion de la imagen que se presenta (IssueSwap): la textura del fundido entre escenas.
void NoteFrontbuffer(uint32_t frontbuffer_ptr);
// Dibujo con la proyeccion de la interfaz que pega en la pantalla principal una textura del tamano
// de la pantalla: una copia de la escena (fondo de menus, fundidos), no interfaz. Tiene que quedarse
// en la EDRAM: el juego copia luego la pantalla (resolve) para el fondo del menu y del orbe.
bool IsScreenCopyDraw(const RegisterFile& regs, const Shader* pixel_shader);

// Diagnostico: fuerza la funcion de profundidad (odisea_force_zfunc) en los dibujos
// de los sombreadores de odisea_force_zfunc_shader mientras dura el objeto.
class DepthFuncOverride {
 public:
  DepthFuncOverride(RegisterFile& regs, const Shader& vertex_shader, const Shader* pixel_shader);
  ~DepthFuncOverride();
  DepthFuncOverride(const DepthFuncOverride&) = delete;
  DepthFuncOverride& operator=(const DepthFuncOverride&) = delete;

 private:
  RegisterFile* regs_ = nullptr;
  uint32_t old_ = 0;
};

// Llamar en cada dibujo justo antes de asegurar los buferes de vertices.
// Sombras mas suaves (cvar odisea_shadow_softness): factor para este dibujo (1 = no
// es el proyector de sombras o no hay que tocarlo) y ajuste de una constante float4
// del sombreador de pixeles (solo cambia los desplazamientos del filtrado).
float ShadowSoftenFactor(const RegisterFile& regs, const Shader* pixel_shader);

// Progreso de la preparacion de sombreadores del arranque (lo lee el exe).
void ShaderPrepBegin(int phase, uint32_t total);
// Estadistica de pipelines nuevos en partida y dibujos saltados por compilarse.
void NotePipelineCreatedInGame();
void NoteDraw();
// Dibujo sin ningun efecto observable: sin memexport, sin escribir color (mascara 0 o modo de solo
// profundidad), sin escribir Z ni stencil (estado normalizado). El llamador comprueba ademas que no
// haya una occlusion query del host activa (su cuenta si depende del dibujo). Cvar
// odisea_skip_noeffect_draws (en caliente). Ejemplo: las ~700 cajas de occlusion query de UE3.
bool IsNoEffectDraw(const RegisterFile& regs, xenos::EdramMode edram_mode, bool memexport_used);
// Contadores de occlusion queries para el registro de rendimiento: 0 = consulta del host iniciada,
// 1 = consulta del host leida, 2 = resultado falso, 3 = sin consultas del host (ruta base).
void NoteOcclusionQuery(int kind);
// --- Efectos transparentes en FSI/ROV (EDRAM emulada en el sombreador) ---
// Bits libres de las banderas del sistema de los traductores (30 y 31): el pixel
// no cambia el destino 0 si su alfa es 0 (bit 30) y/o su rgb es 0 (bit 31), segun
// la mezcla de RB_BLENDCONTROL0. Con los dos bits hacen falta las dos cosas.
constexpr uint32_t kFsiSysFlag_NoOpIfAlphaZero = UINT32_C(1) << 30;
constexpr uint32_t kFsiSysFlag_NoOpIfColorZero = UINT32_C(1) << 31;
// Devuelve los bits anteriores para el destino 0 (0 = no se puede saltar nada).
// rt0_write_mask: bits rgba del destino 0 (mascara de escritura normalizada).
uint32_t FsiBlendNoOpFlags(uint32_t rb_blendcontrol0, uint32_t rt0_write_mask);
// Dibujos sin escritura de profundidad ni stencil: hacer la prueba de profundidad
// despues del sombreador (la seccion critica solo cubre la EDRAM, no el sombreado).
bool FsiLateDepthStencilAllowed();
// --- Borrado rapido de profundidad ---
// El Clear del juego dibuja un rectangulo en coordenadas de pantalla con
// zfunc "siempre", escritura de profundidad y stencil "reemplazar": en ROV/FSI
// eso pasa pixel a pixel por el sombreador con interlock. Si el dibujo es
// exactamente eso (y el rectangulo va alineado a 8 pixeles), se puede hacer
// con el sombreador de borrado de la EDRAM de los resolves.
struct FastDepthClear {
  uint32_t x0, y0, x1, y1;  // pixeles del guest, multiplos de 8
  uint32_t depth_clear;     // valor de 32 bits de la EDRAM (profundidad << 8 | stencil)
};
bool GetFastDepthClear(const RegisterFile& regs, const Shader& vertex_shader,
                       const Shader* pixel_shader, const memory::Memory& memory,
                       xenos::PrimitiveType primitive_type, uint32_t index_count,
                       FastDepthClear& out);
// Informacion de resolve equivalente (solo borrado de profundidad).
void FillDepthClearResolveInfo(const RegisterFile& regs, const FastDepthClear& clear,
                               uint32_t draw_resolution_scale_x,
                               uint32_t draw_resolution_scale_y, draw_util::ResolveInfo& info);
// Captura de RenderDoc de un fotograma del juego (llamar al principio de IssueSwap).
void RenderDocFrameBoundary();
void NoteFrame();
void NoteDrawSkippedForPipeline();
void ShaderPrepSetTotal(uint32_t total);
void ShaderPrepStep(int phase);
void ShaderPrepEnd();

// Filtrado bilineal del mapa de sombras del juego (cvar odisea_shadow_bilinear).
bool ShadowMapBilinear(const xenos::xe_gpu_texture_fetch_t& fetch);

// Parches del microcodigo de sombreadores concretos del juego (penumbra lineal del
// proyector de sombras). Si se parchea, deja la copia en patched y devuelve la
// huella nueva; si no, devuelve la misma huella y deja patched vacio.
uint64_t PatchShaderUcode(const uint32_t* guest_dwords, uint32_t dword_count, uint64_t hash,
                          std::vector<uint32_t>& patched);
// Huella con la que quedara guardado el sombreador (barata: sin copiar el microcodigo).
uint64_t PatchedShaderHash(uint64_t hash);
void SoftenShadowConstant(uint32_t constant_index, float* v, float factor);

// Escritura de registro redundante (mismo valor que ya hay) y sin efectos secundarios: el juego
// reestablece casi todo el estado en cada dibujo y WriteRegister era ~15-20 % del hilo de comandos.
// Excluye los registros con efecto al escribirlos (scratch, COHER, tabla de gamma).
bool IsRedundantRegisterWrite(const uint32_t* values, uint32_t index, uint32_t value);
// Tablas por registro (1 = escribir redundante ignorable / 1 = registro conocido), RegisterFile::kRegisterCount bytes.
const uint8_t* RegisterWriteSkipTable();
const uint8_t* KnownRegisterTable();
// 1 = registro cuya escritura solo guarda el valor (sin efectos ni invalidaciones).
const uint8_t* RegisterPlainWriteTable();

std::vector<PatchedVertexRange> ProcessWatchedDraw(const RegisterFile& regs, const Shader& vertex_shader,
                                                   const Shader* pixel_shader, memory::Memory& memory,
                                                   xenos::PrimitiveType primitive_type, uint32_t index_count);

}  // namespace rex::graphics::odisea
