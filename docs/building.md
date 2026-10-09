# Building from source

You need **your own copy of the game**: the recompiled code (`generated/`) is produced on your machine from your disc 1 `default.xex` and is never published.

## What you need

- Windows 10/11, Visual Studio 2022 (C++ workload + LLVM/clang), CMake 3.25+, Ninja, Python 3.11+.
- The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) at commit `f5337cd`, with the local patches in [`sdk-patches/`](../sdk-patches/) applied (`git apply`), built with Vulkan (`build_sdk_vk.bat` in the SDK folder).
- Optionally the NVIDIA DLSS SDK (for DLSS) and the DirectX Shader Compiler (`dxcompiler.dll`).
- Your disc 1, extracted, with `default.xex`.

## Steps

1. Generate the recompiled code from your `default.xex` with the ReXGlue codegen, using [`project/lostodyssey_manifest.toml`](../project/lostodyssey_manifest.toml) (output goes to `project/generated/`, which is git-ignored).
2. Build the GPU plugin: `xenos_fork\build_vk.bat`.
3. Build the game: `project\build_vk.bat` (public build) or `project\build_vk.bat dev` (debug build with probes, console and dumps).
4. Package: `tools\empaquetar_release.ps1`.

The build scripts find the SDK relative to their own location (`..\rexglue-sdk-src\out\install\win-amd64`, or `..\win-amd64` for the prebuilt SDK). If yours is elsewhere, set the environment variable `REXGLUE_SDK` (source-built SDK) or `REXGLUE_SDK_BIN` (prebuilt SDK). Visual Studio's clang path is assumed to be the default install; edit the top of each `.bat` otherwise.

## How it fits together

See [`docs/dev/ARQUITECTURA_Y_FLUJO.md`](dev/ARQUITECTURA_Y_FLUJO.md) (in Spanish): startup flow, the installer, `data\common`, the texture pack (`.lopack`), shader pipelines, the updater and the crash reporter. [`docs/dev/PENDIENTES.md`](dev/PENDIENTES.md) lists what is still open.

## The texture pack

`tools/lopack/` builds the encrypted `.lopack` from your own extracted textures; the key derives from your disc 1, so the pack only opens with the discs it was made from. The pack itself is not in this repository.
