#include <atomic>
// lostodyssey - ReXGlue Recompiled Project
//
// The Xenia Canary community patches for Lost Odyssey (EU/US default.xex,
// Media ID 368DE6DD, hash C0A4515E25B5D74B, author "boma") translated to
// ReXGlue midasm hooks. A Xenia patch overwrites instruction bytes in guest
// memory; here the code is already recompiled to C++, so each patch becomes a
// hook at the same guest address that produces the same effect: a register
// override where the patch changed an immediate, a forced jump where it
// flipped or inserted a branch, an early return where it stubbed a function.
// Every site was verified against this build's generated instruction stream
// (tools/, address -> DEFINE_REX_FUNC + instruction index, label-aligned).
//
// Each patch is a cvar so it is an option (lostodyssey.toml or --lo_xxx).

#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/system/kernel_state.h>
#include "multidisc.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>

namespace {
constexpr const char* kGroup = "LostOdyssey/Patches";
}

REXCVAR_DEFINE_BOOL(lo_60fps, true, kGroup,
                    "60 FPS: present every vblank instead of every second one. Xenia note: "
                    "going above 60 (no vsync) causes gameplay issues.");
REXCVAR_DEFINE_BOOL(lo_flicker_fix, false, kGroup,
                    "Flickering characters fix: force CPU skinning (avoids flickering dark "
                    "spots on characters, mostly in cutscenes). Costs CPU.");
// Occlusion queries: el parche de Xenia ("Disable Occlusion Queries") fuerza el
// resultado de las consultas de visibilidad. CONGELA a los enemigos (y, con skinning
// por GPU, dispara los vertices): UE3 deja de animar los esqueletos que cree no
// visibles. Confirmado en caliente el 2-oct-2026 (al pasar a 2 los enemigos de la
// batalla del inicio echaron a andar). Por defecto NO se aplica; los cuelgues que
// evitaba en Xenia (carcel del disco 1, callejon del disco 3) hay que tratarlos aparte.
// lo_occlusion_queries_mode: 0 = automatico por disco (desactivadas salvo disco 3),
// 1 = siempre desactivadas, 2 = nunca (dejar las del juego).
REXCVAR_DEFINE_INT32(lo_occlusion_queries_mode, 2, kGroup,
                     "Occlusion queries: 2 sin parche (por defecto), 0 parche automatico por "
                     "disco (salvo disco 3), 1 parche siempre. El parche congela enemigos.");
REXCVAR_DEFINE_BOOL(lo_disable_occlusion_queries, false, kGroup,
                    "Disable occlusion queries (stability).");
REXCVAR_DEFINE_BOOL(lo_postfx_upscale_fix, true, kGroup,
                    "Post-processing upscaling fix: zero the half-texel offsets the post "
                    "effects assume for 1280x720, so they stay aligned at other sizes.");
REXCVAR_DEFINE_BOOL(lo_disable_dof, false, kGroup, "Disable depth of field.");
REXCVAR_DEFINE_BOOL(lo_disable_motion_blur, false, kGroup, "Disable motion blur.");
REXCVAR_DEFINE_BOOL(lo_aniso_16x_unused, true, kGroup,
                    "16x anisotropic filtering (the game asks for 4x).");
REXCVAR_DEFINE_BOOL(lo_disable_dynamic_shadows, false, kGroup,
                    "Disable dynamic shadows (performance).");

// --- 60 FPS -----------------------------------------------------------------
// sub_827B4918 maps the requested presentation interval to a vblank count:
// cases 0/1/2/4 -> li r10,3 / 2 / 1 at 0x827B4A04 / 0x827B4A0C / 0x827B4A14.
// The game takes the "2" case (30 fps). Xenia: be8 0x827B4A0F = 0x01.
// after_instruction=true, registers=["r10"]
void LoFps60Hook(PPCRegister& r10) {
  if (REXCVAR_GET(lo_60fps)) r10.u32 = 1;
}

// --- Flickering characters fix ---------------------------------------------
// sub_82558000 decides GPU vs CPU skinning; Xenia replaces its first two
// instructions with "li r3,1 ; blr" (always CPU). Same thing as an early
// return with r3 = 1.
// after_instruction=false, registers=["r3"], return_on_true=true
bool LoFlickerFixHook(PPCRegister& r3) {
  if (!REXCVAR_GET(lo_flicker_fix)) return false;
  r3.u32 = 1;
  return true;
}

// --- Disable occlusion queries ---------------------------------------------
// 0x823BAD3C in sub_823BAB50: "lwz r11,-30916(r15)" -> "li r11,1".
// after_instruction=true, registers=["r11"]
namespace {
bool OcclusionQueriesDisabled() {
  switch (REXCVAR_GET(lo_occlusion_queries_mode)) {
    case 1: return true;
    case 2: return false;
    default: return lo::MultiDiscMountedNumber() != 3;
  }
}
}  // namespace

void LoOcclusionQueriesAHook(PPCRegister& r11) {
  if (OcclusionQueriesDisabled()) r11.u32 = 1;
}
// 0x823BD618 in sub_823BC220: "cmplwi cr6,r31,0" -> "li r10,1" (the compare
// is dropped, exactly like the byte patch; the beq that follows keeps the
// previous cr6).
// after_instruction=false, registers=["r10"], jump_address_on_true=0x823BD61C
bool LoOcclusionQueriesBHook(PPCRegister& r10) {
  if (!OcclusionQueriesDisabled()) return false;
  r10.u32 = 1;
  return true;
}

// --- Post-processing upscaling fix -----------------------------------------
// Three "lfs" of a float parameter are replaced by loads of 0.0 (Xenia loads
// from guest address 0, which reads as zero):
//   0x8295B62C sub_8295AE68: lfs f0,40(r18)  -> f0 = 0
//   0x82698F50 sub_82698918: lfs f1,12(r28)  -> f1 = 0  (arg to sub_825CEB68)
//   0x8271C49C sub_8271BE60: lfs f1,12(r23)  -> Xenia writes "lfs f0,0(r31)"
//     there, which leaves f1 as it was; the call is the same sub_825CEB68 with
//     the same argument layout as the second site, so f1 = 0 is taken as the
//     intent. Revisit if this site misbehaves.
// Necesario siempre que la escala de render no sea x1 (todos los presets menos
// 720p); lo_options lo pone en lo_postfx_upscale_fix a partir del preset.
// after_instruction=true
void LoPostFxUpscaleAHook(PPCRegister& f0) {
  if (REXCVAR_GET(lo_postfx_upscale_fix)) f0.f64 = 0.0;
}
void LoPostFxUpscaleBHook(PPCRegister& f1) {
  if (REXCVAR_GET(lo_postfx_upscale_fix)) f1.f64 = 0.0;
}
void LoPostFxUpscaleCHook(PPCRegister& f1) {
  if (REXCVAR_GET(lo_postfx_upscale_fix)) f1.f64 = 0.0;
}

// --- Disable depth of field / motion blur ----------------------------------
// Xenia flips a "beq" (0x41..) into "bne" (0x40..) so the effect block is
// skipped whenever its object exists. Forcing the jump skips it always.
//   0x82305D74 sub_82305A80: beq cr6,0x82305DAC   (DoF)
//   0x826E1884 sub_826E1860: beq cr6,0x826E1EDC   (motion blur)
// after_instruction=false, jump_address_on_true=<branch target>
bool LoDisableDofHook() { return REXCVAR_GET(lo_disable_dof); }
bool LoDisableMotionBlurHook() { return REXCVAR_GET(lo_disable_motion_blur); }

// --- 16x anisotropic filtering ---------------------------------------------
// 0x823B93CC in sub_823B9318: "li r5,4" (max anisotropy passed to the sampler
// setup) -> 16.
// after_instruction=true, registers=["r5"]
// Siempre activo: en PC no cuesta nada y solo mejora.
void LoAniso16Hook(PPCRegister& r5) { r5.u32 = 16; }

// --- Disable dynamic shadows ----------------------------------------------
// Xenia: 0x823D5A98 "beq cr6,0x823D5AAC" -> unconditional b (skip the shadow
// pass setup), and 0x823DE090 "bl sub_823D5E50" -> nop (never render the
// shadow map). Its third write (0x823D5FCC, "bl 0x823D63E0" -> be16 0x4800)
// leaves that instruction unchanged in this build, so it is not mirrored.
// after_instruction=false, jump_address_on_true=0x823D5AAC / 0x823DE094
bool LoDisableShadowsBHook() { return REXCVAR_GET(lo_disable_dynamic_shadows); }
bool LoDisableShadowsCHook() { return REXCVAR_GET(lo_disable_dynamic_shadows); }
