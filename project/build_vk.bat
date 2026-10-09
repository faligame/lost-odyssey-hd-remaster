@echo off
setlocal
rem Igual que build.bat, pero contra el SDK compilado desde fuentes con Vulkan
rem (..\build_sdk_vk.bat). Hace falta para que el exe y el plugin compartan el
rem mismo rexruntime.dll: el plugin con backend Vulkan solo enlaza contra ese.
rem
rem Escribe en el mismo out\build\win-amd64-release para no duplicar data\,
rem savedata\ ni el pack de texturas. Copia de seguridad de los binarios que
rem funcionaban: out\build\win-amd64-release\backup-sdk-binario\.
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

set "CC=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang.exe"
set "CXX=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang++.exe"

rem SDK de ReXGlue ya instalado (ver build_sdk_vk.bat); por defecto ..\rexglue-sdk-src\out\install\win-amd64.
rem Se puede fijar con la variable de entorno REXGLUE_SDK.
if not defined REXGLUE_SDK set "REXGLUE_SDK=%~dp0..\rexglue-sdk-src\out\install\win-amd64"
set "SDK=%REXGLUE_SDK:\=/%"

cd /d "%~dp0"

rem Version publica por defecto; "build_vk.bat dev" compila la version debug (LO_DEV: sondas, F3/F4/consola,
rem volcados, ajuste en vivo...). Cambiar de modo reconfigura y recompila. Ver Rexglue\docs\ARQUITECTURA_Y_FLUJO.md.
set "DEVFLAG=-DLO_DEV=OFF"
if /i "%~1"=="dev" set "DEVFLAG=-DLO_DEV=ON"

rem rexglue_DIR hay que forzarlo: CMake lo cachea la primera vez que encuentra el
rem paquete y luego ignora CMAKE_PREFIX_PATH, asi que sin esto se seguiria
rem compilando contra el SDK binario viejo y el post-build copiaria SU
rem rexruntime.dll (sin Vulkan) encima del bueno.
cmake --preset win-amd64-release %DEVFLAG% -DCMAKE_PREFIX_PATH="%SDK%" ^
  -Drexglue_DIR="%SDK%/lib/cmake/rexglue" || exit /b 1
cmake --build --preset win-amd64-release || exit /b 1
