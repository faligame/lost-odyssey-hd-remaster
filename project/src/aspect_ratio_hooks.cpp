// lostodyssey - ReXGlue Recompiled Project
//
// Relacion de aspecto 16:10 (Steam Deck) y 21:9 (ultrapanoramica): los parches
// "Steam Deck / 16:10 Aspect Ratio Support" y "21:9 Ultrawide Aspect Ratio
// Support" de Xenia Canary (autor boma) traducidos a midasm hooks, igual que
// xenia_patch_hooks.cpp. Lo que hacen en el juego:
//   - El aspecto del backbuffer: sub_824862C0 (al crear el dispositivo) carga
//     el f32 de datos 0x82218598 (16/9) y saca el alto = ancho / aspecto. En
//     16:10 el ancho baja a 1152 (1152x720); en 21:9 queda 1280x548.
//   - El mismo f32 lo lee sub_82730000 (constructor, guarda el aspecto en +1944).
//     Xenia escribe el dato; aqui esta en una seccion de solo lectura, asi que
//     se cambia el registro donde se usa (las dos rutas convergen en 0x827300D8).
//   - Camara (sub_82300158): se salta la rama ConstrainAspectRatio (si no, las
//     cinematicas salen con bandas) y se ensancha el campo de vision horizontal.
//     Xenia multiplica la constante pi/360 por 1,3333 (21:9) o 0,9 (16:10); aqui
//     se usa la formula exacta Hor+ (mismo campo vertical que en 16:9).
//   - Solo 21:9: el alto del viewport de la interfaz se fuerza a 550.
// El presentador del plugin muestra la imagen con su aspecto real gracias al
// cvar odisea_display_aspect, que lo_options escribe junto a lo_aspect_ratio.
// Todo se decide al arrancar: cambiar el aspecto pide reiniciar.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_STRING(lo_aspect_ratio, "16:9", "LostOdyssey/Video",
                      "Relacion de aspecto ancho:alto: 16:9 (original), 16:10 (Steam Deck), "
                      "21:9 (el 2,3333 de Xenia) o la exacta de la pantalla (64:27 = 2560x1080, "
                      "43:18 = 3440x1440). Parches de Xenia Canary; requiere reiniciar.");

namespace {

// Lo que se decide al arrancar (el backbuffer se crea una vez y no cambia).
struct AspectConfig {
  enum Kind { k16x9, kNarrow, kWide } kind = k16x9;
  // Aspecto del backbuffer (ancho / alto), el que ven la camara y la escena.
  double value = 16.0 / 9.0;
  uint32_t width = 1280, height = 720;
};

const AspectConfig& CurrentAspect() {
  static const AspectConfig config = [] {
    AspectConfig c;
    const std::string v = REXCVAR_GET(lo_aspect_ratio);
    double a = 16.0 / 9.0;
    unsigned w = 0, h = 0;
    if (v == "21:9") {
      a = 2.3333;  // el valor del parche de Xenia (1280x548)
    } else if (std::sscanf(v.c_str(), "%u:%u", &w, &h) == 2 && w && h) {
      a = double(w) / double(h);
    }
    a = std::clamp(a, 1.25, 4.0);
    if (a > 16.0 / 9.0 + 0.01) {
      // Mas ancho: 1280 de ancho y menos alto (multiplo de 4).
      c.kind = AspectConfig::kWide;
      c.height = std::max(256u, uint32_t(std::lround(1280.0 / a / 4.0)) * 4);
    } else if (a < 16.0 / 9.0 - 0.01) {
      // Mas estrecho: 720 de alto y menos ancho (multiplo de 16).
      c.kind = AspectConfig::kNarrow;
      c.width = std::max(640u, uint32_t(std::lround(720.0 * a / 16.0)) * 16);
    }
    c.value = double(c.width) / double(c.height);
    REXLOG_INFO("lostodyssey: relacion de aspecto {} -> backbuffer {}x{} ({:.4f})", v, c.width,
                c.height, c.value);
    return c;
  }();
  return config;
}

}  // namespace

// --- Backbuffer --------------------------------------------------------------
// sub_824862C0, 0x82486354 "cmpwi cr6,r30,1280" (r30 = ancho de XGetVideoMode,
// f0 = aspecto; aqui convergen la rama panoramica y la de ancho/alto). Despues:
// si r30 > 1280 -> "li r30,1280" (0x8248635C, el que Xenia cambia a 1152) y salta
// a 0x82486374, donde alto = r30 / f0.
// after_instruction=false, registers=["r30","f0"], jump_address_on_true=0x82486374
bool LoAspectBackbufferHook(PPCRegister& r30, PPCRegister& f0) {
  const AspectConfig& a = CurrentAspect();
  const uint32_t width_in = r30.u32;
  const double aspect_in = f0.f64;
  bool jump = false;
  if (a.kind != AspectConfig::k16x9) {
    // alto = ancho / f0 se trunca: un cuarto de pixel de margen para que salga
    // justo el alto elegido aunque el float redondee hacia abajo.
    f0.f64 = double(float(double(a.width) / (double(a.height) + 0.25)));
    if (a.kind == AspectConfig::kNarrow) {
      // Xenia solo pisa el "li r30,1280" de la rama ancho > 1280; aqui se fuerza
      // siempre para que el alto quede en 720 (1152 / 1,6) con cualquier modo.
      r30.u32 = a.width;
      jump = true;
    } else if (int32_t(r30.u32) > 1280) {
      r30.u32 = 1280;
      jump = true;
    }
  }
  const uint32_t width = (!jump && int32_t(r30.u32) > 1280) ? 1280 : r30.u32;
  REXLOG_INFO("lostodyssey: backbuffer: modo {} aspecto {:.4f} -> {}x{} (aspecto {:.4f})", width_in,
              aspect_in, width, int(float(width) / float(f0.f64)), f0.f64);
  return jump;
}

// sub_82730000, 0x827300D8 "lwz r11,136(r31)": f0 = aspecto (de 0x82218598 o de
// los globales maestros 1280/720), que se guarda en +1944.
// after_instruction=false, registers=["f0"]
void LoAspectSceneHook(PPCRegister& f0) {
  const AspectConfig& a = CurrentAspect();
  if (a.kind != AspectConfig::k16x9) f0.f64 = double(float(a.value));
}

// --- Camara ------------------------------------------------------------------
// sub_82300158, 0x8230065C "beq cr6,0x82300708": Xenia la cambia por un salto
// incondicional (b 0x82300708) para no restringir el aspecto en cinematicas.
// after_instruction=false, jump_address_on_true=0x82300708
bool LoAspectNoConstrainHook() { return CurrentAspect().kind != AspectConfig::k16x9; }

// sub_82300158, 0x823007B0 "fmuls f1,f27,f0": f1 = campo de vision horizontal
// en grados (f27) * pi/360 = semiangulo en radianes, que va a sub_82300E50.
// Hor+: se conserva el campo vertical, tan(h') = tan(h) * aspecto / (16/9).
// after_instruction=true, registers=["f1"]
void LoAspectFovHook(PPCRegister& f1) {
  const AspectConfig& a = CurrentAspect();
  if (a.kind == AspectConfig::k16x9) return;
  const double half = f1.f64;
  if (!(half > 0.0 && half < 1.55)) return;
  const double k = a.value / (16.0 / 9.0);
  f1.f64 = double(float(std::atan(std::tan(half) * k)));
}

// --- Interfaz (solo mas ancho que 16:9) -----------------------------------------
// sub_8264F4F8, 0x8264F558 "lwz r11,4(r11)" + mtctr + bctrl: el alto del viewport
// de la interfaz se pide al objeto. Xenia: "li r3,550 ; b +8" (sin llamada) para
// su backbuffer de 548: aqui, el alto del backbuffer + 2. Con la interfaz
// dibujada aparte (odisea_hud_resolution) el plugin la recoloca en un
// rectangulo 16:9 centrado.
// after_instruction=false, registers=["r3"], jump_address_on_true=0x8264F564
bool LoAspectUiViewportHook(PPCRegister& r3) {
  const AspectConfig& a = CurrentAspect();
  if (a.kind != AspectConfig::kWide) return false;
  r3.u64 = a.height + 2;
  return true;
}
