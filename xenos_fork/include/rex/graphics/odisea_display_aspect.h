// Fork (odisea): aspecto con el que se presenta la salida del juego.
//
// El presentador del SDK encaja la imagen en la ventana con el aspecto del modo
// de video (1280x720 = 16:9). Con los parches de aspecto del exe (16:10 de Steam
// Deck, 21:9 ultrapanoramico) el juego dibuja un backbuffer de otro aspecto
// (1152x720, 1280x548) y hay que presentarlo con el suyo, no estirado a 16:9.
// El exe escribe odisea_display_aspect ("16:10", "7:3"...); vacio = modo de video.
#ifndef REX_GRAPHICS_ODISEA_DISPLAY_ASPECT_H_
#define REX_GRAPHICS_ODISEA_DISPLAY_ASPECT_H_

#include <cstdint>

namespace rex::graphics::odisea {

// Sustituye display_width/height (los del modo de video) por el aspecto pedido
// en odisea_display_aspect, si hay uno valido. guest_output_* (lo que se va a
// presentar) solo se usa para dejar en el registro cada cambio de tamano.
void ApplyDisplayAspect(uint32_t& display_width, uint32_t& display_height,
                        uint32_t guest_output_width, uint32_t guest_output_height);

}  // namespace rex::graphics::odisea

#endif  // REX_GRAPHICS_ODISEA_DISPLAY_ASPECT_H_
