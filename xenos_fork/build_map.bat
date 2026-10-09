@echo off
rem Igual que build_vk.bat pero SIN copiar la DLL junto al juego, y generando tools\mapped.map
rem (simbolos para tools\muestrear_pila.py). Copiar a mano con el juego cerrado.
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
set "CC=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang.exe"
set "CXX=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang++.exe"
rem SDK de ReXGlue ya instalado (ver build_sdk_vk.bat); por defecto ..\rexglue-sdk-src\out\install\win-amd64.
rem Se puede fijar con la variable de entorno REXGLUE_SDK.
if not defined REXGLUE_SDK set "REXGLUE_SDK=%~dp0..\rexglue-sdk-src\out\install\win-amd64"
set "SDK=%REXGLUE_SDK:\=/%"
cd /d "%~dp0"
cmake -S . -B out\release-vk -G Ninja -DCMAKE_BUILD_TYPE=Release -DFORK_USE_VULKAN=ON ^
  -DCMAKE_PREFIX_PATH="%SDK%" "-DCMAKE_SHARED_LINKER_FLAGS=-Xlinker /MAP:..\..\..\tools\mapped.map" >nul || exit /b 1
cmake --build out\release-vk
