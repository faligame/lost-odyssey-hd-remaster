// lostodyssey - ReXGlue Recompiled Project
//
// Idiomas. Los discos de Lost Odyssey traen el juego en seis idiomas (carpetas
// bin\xenon\loc|scr|event|snd\<idioma>): textos en ingles, frances, aleman,
// italiano, espanol y japones; voces en ingles, frances, aleman, italiano y
// japones (el espanol usa las voces inglesas). El juego elige segun el idioma
// de la consola emulada, el cvar user_language del SDK (XC_LANGUAGE), que se
// lee al arrancar: cambiarlo pide reiniciar.
//
// Los textos propios del port (pestanas de ajustes, avisos) se escriben en el
// codigo en espanol y Tr() los devuelve en el idioma del juego. Una cadena que
// no esta en la tabla se devuelve tal cual.
#pragma once

#include <cstdint>

namespace lo {

enum class Lang : int { kEnglish = 0, kFrench, kGerman, kItalian, kSpanish, kJapanese };
inline constexpr int kLangCount = 6;

// Idioma con el que arranco el juego (user_language).
Lang GameLanguage();
// Conversion con XC_LANGUAGE (1 ingles, 2 japones, 3 aleman, 4 frances,
// 5 espanol, 6 italiano). Cualquier otro valor se trata como ingles, que es lo
// que hace el juego.
Lang LangFromXLanguage(uint32_t x);
uint32_t XLanguageOf(Lang lang);
// Carpeta del idioma en los discos: int, fra, deu, ita, spa, jpn.
const char* LangFolder(Lang lang);
// Nombre del idioma en su propio idioma ("English", "Deutsch", "日本語"...).
const char* LangNativeName(Lang lang);

// Texto del port en el idioma del juego. es = el texto en espanol del codigo.
const char* Tr(const char* es);
// El mismo texto en un idioma concreto (para caer al ingles si la fuente del
// juego no tiene algun glifo).
const char* TrIn(Lang lang, const char* es);

}  // namespace lo
