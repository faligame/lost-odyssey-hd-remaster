// lostodyssey - ReXGlue Recompiled Project
//
// Ver title_logo.h.
#include "title_logo.h"

#include <rex/cvar.h>
#include <rex/logging.h>

namespace lo {

void TitleLogoWatch() {
  if (rex::cvar::SetFlagByName("odisea_watch_texture", kTitleTextureHash)) {
    REXLOG_INFO("[titulo] vigilando la textura del titulo {}", kTitleTextureHash);
  } else {
    REXLOG_WARN("[titulo] el plugin grafico no tiene odisea_watch_texture: titulo original");
  }
}

}  // namespace lo
