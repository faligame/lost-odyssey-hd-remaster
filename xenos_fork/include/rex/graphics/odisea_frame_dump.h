// Fork (odisea): volcado de fotogramas completos a un fichero de texto.
//
// Para separar la escena 3D de la interfaz hace falta saber, en orden, cada
// draw (destinos de color y profundidad en la EDRAM, mezcla, texturas que lee),
// cada resolve (de qué base de la EDRAM a qué dirección) y cada swap. Con
// odisea_frame_dump_after_ms > 0, al primer swap pasado ese tiempo se vuelcan
// odisea_frame_dump_frames fotogramas completos a odisea_frame_dump.txt (junto
// al exe). Comun a D3D12 y Vulkan.
#ifndef REX_GRAPHICS_ODISEA_FRAME_DUMP_H_
#define REX_GRAPHICS_ODISEA_FRAME_DUMP_H_

#include <cstdint>

#include <rex/graphics/xenos.h>

namespace rex::graphics {
class RegisterFile;
class Shader;
}  // namespace rex::graphics

namespace rex::graphics::odisea {

// true mientras se esta volcando (para no calcular nada si no).
bool FrameDumpActive();
void FrameDumpDraw(const RegisterFile& regs, const Shader& vertex_shader,
                   const Shader* pixel_shader, xenos::PrimitiveType primitive_type,
                   uint32_t index_count);
void FrameDumpResolve(const RegisterFile& regs, bool ok, uint32_t written_address,
                      uint32_t written_length);
// Llamar al principio de IssueSwap: arranca y termina el volcado.
void FrameDumpSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                   uint32_t frontbuffer_height);

}  // namespace rex::graphics::odisea

#endif  // REX_GRAPHICS_ODISEA_FRAME_DUMP_H_
