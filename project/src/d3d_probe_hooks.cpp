// lostodyssey - ReXGlue Recompiled Project
//
// Sonda de creacion de recursos de Direct3D (Xbox 360): anota en el registro
// cada superficie y textura que crea el juego, con sus argumentos y quien la
// pide (LR). Sirve para encontrar de donde sale el tamano del mapa de sombras
// (pasada con pitch 880 en la EDRAM, 2-oct-2026). Cvar lo_d3d_create_log.
//
// Firma de la Xbox 360: D3DDevice_CreateSurface(device, width, height, format,
// multisample, params, out) y D3DDevice_CreateTexture(device, width, height,
// depth, levels, usage, format, pool, type, out). Se anotan r4..r10 tal cual
// para no depender de la firma exacta.

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(lo_d3d_create_log, false, "LostOdyssey/Diagnostico",
                    "Anota cada superficie y textura que crea el juego (argumentos y llamador)");
REXCVAR_DEFINE_BOOL(lo_d3d_resolve_log, false, "LostOdyssey/Diagnostico",
                    "Anota los resolves del juego (copias de la EDRAM a texturas) por pila de llamadas: "
                    "la primera vez y cada vez que una pila vuelve tras 2 s sin usarse");

namespace {

// Pila de llamadas del guest: la cadena de marcos empieza en r1 ([r1] = r1 del
// llamador) y cada funcion guarda su LR en [r1 del llamador - 8].
std::string GuestBacktrace(const PPCContext& ctx, const uint8_t* base, int depth = 5) {
  std::string out;
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < depth; ++i) {
    // Las pilas de los hilos del juego estan en 0x7xxxxxxx.
    if (sp < 0x1000 || sp >= 0xF0000000u) break;
    const uint32_t old = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + sp));
    if (old <= sp || old >= 0xF0000000u || old - sp > 0x100000) break;
    const uint32_t lr = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + old - 8));
    out += fmt::format(" {:08X}", lr);
    sp = old;
  }
  return out;
}

}  // namespace

REX_EXTERN(__imp__D3DDevice_CreateSurface);
REX_EXTERN(__imp__D3DDevice_CreateTexture);

REX_EXTERN(D3DDevice_CreateSurface) {
  if (REXCVAR_GET(lo_d3d_create_log)) {
    // Firma real de la libreria de la 360 (sin puntero a dispositivo): r3 = ancho.
    REXLOG_INFO("lo_d3d: CreateSurface w={} h={} fmt=0x{:08X} msaa={} params=0x{:08X} lr=0x{:08X} pila:{}",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, uint32_t(ctx.lr),
                GuestBacktrace(ctx, base));
  }
  __imp__D3DDevice_CreateSurface(ctx, base);
}

REX_EXTERN(D3DDevice_CreateTexture) {
  if (REXCVAR_GET(lo_d3d_create_log)) {
    REXLOG_INFO(
        "lo_d3d: CreateTexture w={} h={} depth={} levels={} usage=0x{:X} fmt=0x{:08X} lr=0x{:08X} pila:{}",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, uint32_t(ctx.lr),
        GuestBacktrace(ctx, base));
  }
  __imp__D3DDevice_CreateTexture(ctx, base);
}

REX_EXTERN(__imp__D3DDevice_Resolve);

// D3DDevice_Resolve(device, flags, source_rect, dest_texture, dest_point, level, face, clear_color,
// clear_z (f1), ...). Para encontrar la copia de pantalla de los fundidos y los menus (7-oct-2026).
REX_EXTERN(D3DDevice_Resolve) {
  if (REXCVAR_GET(lo_d3d_resolve_log)) {
    const std::string stack = GuestBacktrace(ctx, base, 4);
    const auto now = std::chrono::steady_clock::now();
    static std::mutex mutex;
    struct Seen {
      std::chrono::steady_clock::time_point last;
      uint64_t count = 0;
    };
    static std::map<std::string, Seen> seen;
    bool log = false;
    uint64_t count = 0;
    {
      std::lock_guard lock(mutex);
      Seen& e = seen[stack + fmt::format(" lr{:08X}", uint32_t(ctx.lr))];
      log = e.count == 0 || now - e.last > std::chrono::seconds(2);
      e.last = now;
      count = ++e.count;
    }
    if (log) {
      auto be = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a)); };
      uint32_t tex_base = 0, tex_size = 0;
      if (ctx.r6.u32 >= 0x1000 && ctx.r6.u32 < 0x40000000) {
        tex_base = be(ctx.r6.u32 + 0x1C + 4) & 0xFFFFF000u;
        tex_size = be(ctx.r6.u32 + 0x1C + 8);
      }
      uint32_t rect[4] = {};
      if (ctx.r5.u32 >= 0x1000 && ctx.r5.u32 < 0x40000000) {
        for (int i = 0; i < 4; ++i) rect[i] = be(ctx.r5.u32 + 4 * i);
      }
      REXLOG_INFO("lo_d3d: Resolve #{} flags=0x{:X} rect=({},{},{},{}) dest=0x{:08X} base=0x{:08X} size=0x{:08X} "
                  "lr=0x{:08X} pila:{}",
                  count, ctx.r4.u32, rect[0], rect[1], rect[2], rect[3], ctx.r6.u32, tex_base, tex_size,
                  uint32_t(ctx.lr), stack);
    }
  }
  __imp__D3DDevice_Resolve(ctx, base);
}
