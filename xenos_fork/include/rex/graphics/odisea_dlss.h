// Fork (odisea): estado comun de DLSS (D3D12 ahora, Vulkan despues).
//
// DLSS reconstruye la escena 3D (hud_scene_texture_, a la escala 3D) a la
// resolucion de salida. Pide, ademas del color:
//   - jitter: cada fotograma la escena se desplaza una fraccion de pixel
//     (secuencia de Halton 2,3); se aplica en el desplazamiento NDC de los
//     dibujos de la escena (IsSceneDraw), no a la interfaz ni a las sombras;
//   - profundidad de la escena;
//   - vectores de movimiento: solo de camara, con la profundidad y la
//     ViewProjection de este fotograma y del anterior. En Lost Odyssey la
//     ViewProjection (mundo -> clip, vector fila) esta en las constantes de
//     vertices c233-c236 (filas x, y, z y traslacion), en combate y en el campo.
#ifndef REX_GRAPHICS_ODISEA_DLSS_H_
#define REX_GRAPHICS_ODISEA_DLSS_H_

#include <cstdint>

namespace rex::graphics {
class RegisterFile;
class Shader;
}  // namespace rex::graphics

namespace rex::graphics::odisea::dlss {

// odisea_dlss (el exe lo escribe con la opcion de reescalado del 3D).
bool Requested();

// Lo llama el backend en cada swap con DLSS funcionando (o con active = false
// si no): fija el jitter del fotograma siguiente para una escena de
// render_width x render_height pixeles del host reescalada a la salida.
void SetFrame(bool active, uint32_t render_width, uint32_t render_height, uint32_t output_width,
              uint32_t output_height);
bool JitterActive();
// Jitter del fotograma en curso, en pixeles de la escena (x a la derecha, y hacia
// abajo).
void GetJitter(float& x, float& y);

// Dibujo de la escena 3D principal: superficie del backbuffer sin MSAA, con
// prueba de profundidad y viewport que la cubre.
bool IsSceneDraw(const RegisterFile& regs);
// Efecto de la escena (IsSceneDraw con mezcla y sin escribir profundidad:
// particulas, humo...): se dibuja otra vez en la mascara de DLSS.
bool IsEffectDraw(const RegisterFile& regs);
// Dibujo de un objeto animado con huesos (escribe profundidad y lee las
// constantes de los huesos, c113-c202 en Lost Odyssey): sus vertices se capturan
// para darle vectores de movimiento propios.
bool IsDynamicDraw(const RegisterFile& regs, const Shader& vertex_shader);
// Toma la ViewProjection del fotograma (la primera valida) de las constantes de
// un dibujo de la escena.
void NoteSceneDraw(const RegisterFile& regs, const Shader& vertex_shader);
// Matrices fila (16 floats, fila i = constante c233 + i) del fotograma en curso y
// del anterior. false si falta alguna (primer fotograma, escena sin camara).
bool GetMatrices(float current[16], float previous[16]);
// Transformacion z del viewport de la escena (profundidad = z_ndc * escala +
// desplazamiento), tomada con la ViewProjection. Por defecto 1 y 0.
void GetDepthMapping(float& scale, float& offset);
// Fin del fotograma (en el swap, despues de usar GetMatrices): la actual pasa a
// ser la anterior.
void EndFrame();

// Sesgo de mip para las texturas de la escena con DLSS (guia de NVIDIA:
// log2(render / salida)): las texturas se piden con el detalle de la salida.
// Se suma al campo de sesgo (x32, 10 bits con signo) de cada constante de
// textura de las 32 (6 palabras cada una) en la copia que va a la GPU, solo en
// los dibujos de la escena. Devuelve false si no hay sesgo este fotograma.
bool MipBiasActive();
void ApplyMipBias(uint32_t* fetch_constants);
// Nitidez del enfocado de la escena tras DLSS (odisea_dlss_sharpness, 0..1).
float Sharpness();

}  // namespace rex::graphics::odisea::dlss

#endif  // REX_GRAPHICS_ODISEA_DLSS_H_
