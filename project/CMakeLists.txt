# lostodyssey - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(lostodyssey LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(LOSTODYSSEY_SOURCES
    src/main.cpp
    src/xenia_patch_hooks.cpp
    src/aspect_ratio_hooks.cpp
    src/shader_prep.cpp
    src/live_tuning_hooks.cpp
    src/lo_options.cpp
    src/lo_i18n.cpp
    src/title_logo.cpp
    src/turbo_hooks.cpp
    src/save_anywhere_hooks.cpp
    src/settings_page.cpp
    src/menu_assets.cpp
    src/multidisc_hooks.cpp
    src/disc_sources.cpp
    src/crash_handler.cpp
    src/save_headers.cpp
    src/guest_guards.cpp
    src/no_random_battles_hooks.cpp
    src/shader_prewarm.cpp
    src/texture_pack_secret.cpp
    src/installer.cpp
    src/texture_download.cpp
    src/crash_reporter.cpp
    src/lo_update.cpp
    # LZO1X para las texturas del menu del juego (MIT, sin modificar).
    thirdparty/lzokay/lzokay.cpp
)

# Version publica: sin sondas, volcados ni herramientas de depuracion. La build de
# desarrollo (cmake -DLO_DEV=ON) las incluye. Ver LO_VERSION en src/lo_version.h.
option(LO_DEV "Incluir las herramientas de depuracion (no para publicar)" OFF)
if(LO_DEV)
    list(APPEND LOSTODYSSEY_SOURCES
        src/d3d_probe_hooks.cpp
        src/heap_watch.cpp
        src/encounter_hooks.cpp
        src/disc_texture_dump.cpp)
endif()

if(WIN32)
    # Icono de la aplicacion (project/icon.ico via src/lostodyssey.rc).
    list(APPEND LOSTODYSSEY_SOURCES src/lostodyssey.rc)
    add_executable(lostodyssey WIN32 ${LOSTODYSSEY_SOURCES})
else()
    add_executable(lostodyssey ${LOSTODYSSEY_SOURCES})
endif()

target_include_directories(lostodyssey PRIVATE thirdparty/lzokay)
# Sin la ruta del equipo de compilacion dentro del ejecutable (__FILE__, informes de error).
target_compile_options(lostodyssey PRIVATE "-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=")
target_compile_options(lostodyssey PRIVATE "-ffile-prefix-map=${CMAKE_SOURCE_DIR}/../=")
if(LO_DEV)
    target_compile_definitions(lostodyssey PRIVATE LO_DEV=1)
endif()
if(WIN32)
    # GetOpenFileNameW: selector del disco 1 si no se encuentran los datos.
    target_link_libraries(lostodyssey PRIVATE comdlg32 comctl32 bcrypt ole32 shell32 uuid winhttp dxgi)
endif()
# Mapa del enlazador: permite traducir a funcion la direccion de un cierre.
target_link_options(lostodyssey PRIVATE "LINKER:/MAP:${CMAKE_BINARY_DIR}/lostodyssey.map")

rexglue_setup_target(lostodyssey GPU_PLUGINS xenos)

# Aplicador de actualizaciones (ver src/updater.cpp): ejecutable aparte, sin dependencias del SDK.
if(WIN32)
    add_executable(updater WIN32 src/updater.cpp)
    target_link_libraries(updater PRIVATE shell32 user32)
    set_target_properties(updater PROPERTIES OUTPUT_NAME updater)
endif()
