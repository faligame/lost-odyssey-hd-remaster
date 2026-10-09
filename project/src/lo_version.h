// lostodyssey - version publica. Unico sitio donde se cambia: el instalador, el
// menu, el pack de texturas y los informes de cierre la leen de aqui.
#pragma once

#define LO_VERSION_MAJOR 0
#define LO_VERSION_MINOR 0
#define LO_VERSION_PATCH 1
#define LO_VERSION_STRING "0.0.1"
#define LO_VERSION_WSTRING L"0.0.1"

// Manifiesto del pack de texturas HD (texture-pack.json en la raiz del repositorio, rama main).
#define LO_TEXTURE_MANIFEST_URL "https://raw.githubusercontent.com/faligame/lost-odyssey-hd-remaster/main/texture-pack.json"

// Donde se abren los informes de cierre (GitHub issues).
#define LO_ISSUES_URL "https://github.com/faligame/lost-odyssey-hd-remaster/issues/new"

// Carpeta del repositorio (rama main) donde estan latest.json y latest.json.sig (ver tools/publicar/publicar_version.py).
#define LO_UPDATE_BASE_URL "https://raw.githubusercontent.com/faligame/lost-odyssey-hd-remaster/main/"
