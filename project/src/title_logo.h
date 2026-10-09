// lostodyssey - ReXGlue Recompiled Project
//
// Logo de "Lost Odyssey HD Remaster" en la pantalla de titulo.
//
// El juego dibuja su titulo con la textura LO_TITLE (0xD96E255284DE672B,
// 512x128, paquete sys\img\titleparts): una tira de letras de 512x32 en la
// maqueta de 1280x720 y, encima, su copia difuminada para el brillo. No hay
// sitio para un logo, asi que el plugin grafico agranda esos dos cuadrados en
// el propio dibujo del juego (odisea_watched_draw.cpp, cvars
// odisea_watch_quad_*) y el pack de texturas trae el logo en su lugar. Al
// ser el dibujo del juego, el logo hace sus mismos fundidos, queda detras de
// los cuadros de mensaje y desaparece con el titulo.
//
// El exe solo le dice al plugin que textura vigilar. Sin pack de texturas no se
// identifica la textura y se ve el titulo original.
#pragma once

namespace lo {

// Hash de la textura del titulo del juego.
inline constexpr const char* kTitleTextureHash = "D96E255284DE672B";

// Pide al plugin grafico que vigile la textura del titulo. Llamar justo despues
// de cargar el plugin (sus cvars ya existen) y antes de que el juego cree
// texturas.
void TitleLogoWatch();

}  // namespace lo
