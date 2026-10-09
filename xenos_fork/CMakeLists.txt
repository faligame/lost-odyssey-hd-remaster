# rexgpu-odisea: fork del plugin GPU Xenos del SDK ReXGlue 0.10.0
# (fuentes del nightly 20260826-f5337cdc) compilado fuera del árbol del SDK
# contra el SDK binario (win-amd64). Cambios respecto al original:
#   - EDRAM emulada de 4096 tiles (20 MB) en vez de 2048 (10 MB) para que
#     Lost Odyssey pueda renderizar a 1920x1080 nativo (color+depth = 3264 tiles).
#   - Bases de render target de 12 bits (RB_COLOR_INFO/RB_DEPTH_INFO).
# Se carga con --gpu_plugin odisea (el runtime busca rexgpu-<nombre>.dll
# junto al ejecutable), asi que convive con el rexgpu-xenos.dll original.

cmake_minimum_required(VERSION 3.25)
project(xenos_fork LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# SDK binario: -DCMAKE_PREFIX_PATH=<...>/Rexglue/win-amd64
find_package(rexglue CONFIG REQUIRED)

set(FORK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(GFX "${FORK_ROOT}/src/graphics")

# El backend Vulkan solo se puede compilar contra un SDK con
# REXGLUE_USE_VULKAN=ON: el presentador vive en el runtime (src/ui/vulkan), no
# aqui, y el paquete win-amd64 que distribuyen viene sin el. Se construye con
# build_sdk_vk.bat, que instala en rexglue-sdk-src/out/install/win-amd64.
option(FORK_USE_VULKAN "Compilar tambien el backend Vulkan" OFF)

# Misma lista que src/graphics/CMakeLists.txt del SDK.
set(XENOS_SOURCES
    ${GFX}/plugin_main.cpp

    ${GFX}/flags.cpp
    # Fork: volcado y pack de texturas, tambien comunes a los dos backends.
    ${GFX}/odisea_texture_dump.cpp
    ${GFX}/odisea_texture_replace.cpp
    ${GFX}/odisea_lopack.cpp
    ${GFX}/odisea_watched_draw.cpp
    # Fork: perfilador de tiempo de GPU por categorias (D3D12 y Vulkan).
    ${GFX}/odisea_gpu_profiler.cpp
    # Fork: SMAA en el swap (cvar y texturas de consulta comunes).
    ${GFX}/odisea_smaa.cpp
    ${GFX}/odisea_display_aspect.cpp
    ${GFX}/odisea_frame_dump.cpp
    ${GFX}/odisea_dlss.cpp
    ${GFX}/odisea_prewarm.cpp
    ${GFX}/register_file.cpp
    ${GFX}/registers.cpp
    ${GFX}/pipeline/shader/shader.cpp
    ${GFX}/format/ucode.cpp
    ${GFX}/sampler_info.cpp

    ${GFX}/graphics_system.cpp
    ${GFX}/command_processor.cpp
    ${GFX}/util/draw_extent_estimator.cpp
    ${GFX}/util/draw.cpp
    ${GFX}/packet_disassembler.cpp
    ${GFX}/primitive_processor.cpp
    ${GFX}/pipeline/render_target/cache.cpp
    ${GFX}/shared_memory.cpp
    ${GFX}/pipeline/texture/cache.cpp
    ${GFX}/pipeline/shader/interpreter.cpp
    ${GFX}/pipeline/shader/translator.cpp
    ${GFX}/pipeline/shader/translator_disasm.cpp

    ${FORK_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp

    ${GFX}/pipeline/shader/dxbc.cpp
    ${GFX}/pipeline/shader/dxbc_translator.cpp
    ${GFX}/pipeline/shader/dxbc_translator_alu.cpp
    ${GFX}/pipeline/shader/dxbc_translator_fetch.cpp
    ${GFX}/pipeline/shader/dxbc_translator_memexport.cpp
    ${GFX}/pipeline/shader/dxbc_translator_om.cpp

    ${GFX}/d3d12/graphics_system.cpp
    ${GFX}/d3d12/command_processor.cpp
    ${GFX}/d3d12/primitive_processor.cpp
    ${GFX}/d3d12/render_target_cache.cpp
    ${GFX}/d3d12/shader.cpp
    ${GFX}/d3d12/shared_memory.cpp
    ${GFX}/d3d12/texture_cache.cpp
    ${GFX}/d3d12/odisea_smaa_d3d12.cpp
    ${GFX}/d3d12/odisea_hud_d3d12.cpp
    ${GFX}/d3d12/odisea_dlss_d3d12.cpp
    ${GFX}/d3d12/deferred_command_list.cpp
    ${GFX}/d3d12/pipeline_cache.cpp
)

if(FORK_USE_VULKAN)
    list(APPEND XENOS_SOURCES
        # Traduccion de shaders a SPIR-V (el equivalente de la rama DXBC).
        ${GFX}/pipeline/shader/spirv_builder.cpp
        ${GFX}/pipeline/shader/spirv.cpp
        ${GFX}/pipeline/shader/spirv_translator.cpp
        ${GFX}/pipeline/shader/spirv_translator_alu.cpp
        ${GFX}/pipeline/shader/spirv_translator_fetch.cpp
        ${GFX}/pipeline/shader/spirv_translator_memexport.cpp
        ${GFX}/pipeline/shader/spirv_translator_rb.cpp

        ${GFX}/vulkan/graphics_system.cpp
        ${GFX}/vulkan/deferred_command_buffer.cpp
        ${GFX}/vulkan/command_processor.cpp
        ${GFX}/vulkan/pipeline_cache.cpp
        ${GFX}/vulkan/primitive_processor.cpp
        ${GFX}/vulkan/render_target_cache.cpp
        ${GFX}/vulkan/shader.cpp
        ${GFX}/vulkan/shared_memory.cpp
        ${GFX}/vulkan/texture_cache.cpp
        ${GFX}/vulkan/odisea_smaa_vulkan.cpp
        ${GFX}/vulkan/odisea_hud_vulkan.cpp
        ${GFX}/vulkan/odisea_dlss_vulkan.cpp
    )
endif()

# zstd (solo descompresion, BSD): imagenes del .lopack.
add_library(lo_zstd STATIC
    thirdparty/zstd/common/debug.c
    thirdparty/zstd/common/entropy_common.c
    thirdparty/zstd/common/error_private.c
    thirdparty/zstd/common/fse_decompress.c
    thirdparty/zstd/common/xxhash.c
    thirdparty/zstd/common/zstd_common.c
    thirdparty/zstd/decompress/huf_decompress.c
    thirdparty/zstd/decompress/zstd_ddict.c
    thirdparty/zstd/decompress/zstd_decompress.c
    thirdparty/zstd/decompress/zstd_decompress_block.c)
target_include_directories(lo_zstd PUBLIC thirdparty/zstd)
target_include_directories(lo_zstd PRIVATE thirdparty/zstd/common thirdparty/zstd/decompress)
target_compile_definitions(lo_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0 XXH_NAMESPACE=ZSTD_)
set_target_properties(lo_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(rexgpu-odisea SHARED ${XENOS_SOURCES})
# Sin la ruta del equipo de compilacion dentro de la DLL (__FILE__, informes de error).
target_compile_options(rexgpu-odisea PRIVATE "-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=")

set_target_properties(rexgpu-odisea PROPERTIES
    OUTPUT_NAME "rexgpu-odisea"
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Las cabeceras modificadas del fork (include/rex/graphics/...) van ANTES que
# las del SDK para que sustituyan a las originales.
#
# CUIDADO al AÑADIR una cabecera nueva aqui: los ficheros .d de los objetos ya
# compilados apuntan a la cabecera del SDK, que no ha cambiado, asi que Ninja
# los da por buenos y solo recompila lo que edites. Si la cabecera nueva cambia
# el tamano o el orden de los miembros de una clase, unos objetos usan el
# diseno viejo y otros el nuevo: corrupcion de memoria y caidas en codigo que
# no has tocado. Tras anadir una cabecera al fork hay que BORRAR out/ y
# recompilar entero. Editarla despues ya se propaga bien.
target_include_directories(rexgpu-odisea BEFORE PRIVATE
    ${FORK_ROOT}/include
    ${FORK_ROOT}   # thirdparty/dxbc/DXBCChecksum.h
)

# Flags globales del CMakeLists raiz del SDK + preset win-amd64.
target_compile_options(rexgpu-odisea PRIVATE
    -march=x86-64-v2
    -fno-strict-aliasing
    -ffp-model=strict
    -fno-char8_t
    -O3
    -Wall
    -Wextra
    -Wno-unused-variable
)
target_compile_definitions(rexgpu-odisea PRIVATE
    NDEBUG
    REX_PLATFORM_WINDOWS=1
    REXGLUE_BUILD_CONFIG="Release"
)

set_source_files_properties(${FORK_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# rex_target_pch del SDK (cmake/rex_pch.cmake) + extras de src/graphics.
target_precompile_headers(rexgpu-odisea PRIVATE
    <algorithm>
    <array>
    <cstddef>
    <cstdint>
    <cstring>
    <functional>
    <memory>
    <optional>
    <string>
    <string_view>
    <utility>
    <vector>
    <rex/platform.h>
    <rex/types.h>
    <rex/assert.h>
    <rex/cvar.h>
    <rex/logging.h>
    <mutex>
    <unordered_map>
    <rex/math.h>
    <rex/memory.h>
    <rex/thread/mutex.h>
    <rex/system/xmemory.h>
    <rex/graphics/xenos.h>
    <rex/graphics/registers.h>
    <rex/graphics/format/ucode.h>
    <rex/graphics/pipeline/shader/shader.h>
)

# rex::runtime aporta include/, NOMINMAX, WIN32_LEAN_AND_MEAN, REX_HAS_D3D12=1,
# fmt/spdlog/SDL3/dxc-headers. Igual que el original: todo PRIVATE.
target_link_libraries(rexgpu-odisea PRIVATE
    rex::runtime
    rex::xxhash
    rex::renderdoc
    d3d12
    dxgi
    dxguid
    bcrypt
    lo_zstd
)

# Fork (odisea): NVIDIA DLSS (SDK NGX, git clone de github.com/NVIDIA/DLSS en
# ../sdk_externos/DLSS). Si no esta, el plugin se compila sin DLSS. Licencia
# NVIDIA RTX SDK: la dll nvngx_dlss.dll puede ir con el juego; el SDK no se
# publica.
set(FORK_DLSS_SDK "${FORK_ROOT}/../sdk_externos/DLSS" CACHE PATH "SDK de NVIDIA DLSS (NGX)")
if(EXISTS "${FORK_DLSS_SDK}/include/nvsdk_ngx.h")
    target_include_directories(rexgpu-odisea PRIVATE "${FORK_DLSS_SDK}/include")
    target_compile_definitions(rexgpu-odisea PRIVATE ODISEA_DLSS=1)
    target_link_libraries(rexgpu-odisea PRIVATE
        "${FORK_DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    message(STATUS "odisea: DLSS activado (${FORK_DLSS_SDK})")
else()
    message(STATUS "odisea: sin SDK de DLSS en ${FORK_DLSS_SDK}")
endif()

if(FORK_USE_VULKAN)
    # El SDK instala las cabeceras de Vulkan y vk_mem_alloc.h en su include/
    # (lo aporta rex::runtime), pero NO las de glslang: SPIRV/SpvBuilder.h y
    # SPIRV/GLSL.std.450.h solo existen en el arbol de fuentes, y build_info.h
    # lo genera el propio build. Hay que apuntar a los dos sitios.
    set(FORK_SDK_SRC "${FORK_ROOT}/../rexglue-sdk-src" CACHE PATH
        "Arbol de fuentes del SDK (para las cabeceras de glslang)")
    foreach(dir "${FORK_SDK_SRC}/thirdparty/glslang"
                "${FORK_SDK_SRC}/out/build/win-amd64/include")
        if(NOT EXISTS "${dir}")
            message(FATAL_ERROR "FORK_USE_VULKAN: no existe ${dir}. Compila antes el SDK "
                                "con build_sdk_vk.bat.")
        endif()
    endforeach()
    target_include_directories(rexgpu-odisea PRIVATE
        "${FORK_SDK_SRC}/thirdparty/glslang"
        "${FORK_SDK_SRC}/out/build/win-amd64/include"
    )

    target_link_libraries(rexgpu-odisea PRIVATE
        rex::SPIRV
        rex::glslang
        rex::MachineIndependent
        rex::GenericCodeGen
        rex::OSDependent
        rex::OGLCompiler
    )
endif()
