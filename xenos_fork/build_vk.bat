@echo off
setlocal
rem Compila rexgpu-odisea.dll con LOS DOS backends (D3D12 + Vulkan) contra el
rem SDK compilado desde fuentes por ..\build_sdk_vk.bat. El paquete binario
rem win-amd64 no sirve aqui: viene con REXGLUE_USE_VULKAN=OFF y su rexruntime
rem no lleva el presentador Vulkan (src/ui/vulkan).
rem
rem El backend se elige en el menu F2 (cvar lo_gpu_backend, requiere reiniciar).
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

set "CC=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang.exe"
set "CXX=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang++.exe"

rem SDK de ReXGlue ya instalado (ver build_sdk_vk.bat); por defecto ..\rexglue-sdk-src\out\install\win-amd64.
rem Se puede fijar con la variable de entorno REXGLUE_SDK.
if not defined REXGLUE_SDK set "REXGLUE_SDK=%~dp0..\rexglue-sdk-src\out\install\win-amd64"
set "SDK=%REXGLUE_SDK:\=/%"

cd /d "%~dp0"

cmake -S . -B out\release-vk -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DFORK_USE_VULKAN=ON -DCMAKE_PREFIX_PATH="%SDK%" || exit /b 1
cmake --build out\release-vk || exit /b 1

copy /Y "out\release-vk\rexgpu-odisea.dll" "..\project\out\build\win-amd64-release\rexgpu-odisea.dll" || exit /b 1
echo.
echo OK: rexgpu-odisea.dll (D3D12 + Vulkan) copiada junto a lostodyssey.exe
