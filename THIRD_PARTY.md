# Third-party components

| Component | Where | Licence |
|---|---|---|
| ReXGlue SDK (static recompilation SDK, kernel/GPU/VFS runtime) | not included; build against tag `f5337cd` plus [`sdk-patches/`](sdk-patches/) | MIT |
| Xenia (GPU research, shader sources used by the plugin) | `xenos_fork/src/graphics/shaders/` | BSD-3-Clause |
| SMAA | `xenos_fork/thirdparty/smaa/` | MIT |
| zstd | `xenos_fork/thirdparty/zstd/` | BSD-3-Clause |
| FXAA (Timothy Lottes) | `xenos_fork/src/graphics/shaders/xesl/third_party/fxaa/` | see its `LICENSE` |
| lzokay | `project/thirdparty/lzokay/` | MIT |
| stb | `project/thirdparty/stb/` | public domain |
| Dear ImGui, SDL3, spdlog, fmt, xxHash, toml++, glslang, SPIRV-Tools, Vulkan Memory Allocator | pulled in by the ReXGlue SDK | MIT / zlib / BSD / Apache-2 |
| FFmpeg (audio/video decoding) | linked into the SDK runtime in the binaries | LGPL v2.1 |
| NVIDIA DLSS | **not included**: bring the DLSS SDK from NVIDIA | NVIDIA RTX SDK licence |
| Microsoft DirectX Shader Compiler (`dxcompiler.dll`) | **not included** in the source; shipped in binary releases | LLVM licence with exceptions |

Binary releases carry every licence text in their `LICENSES/` folder.
