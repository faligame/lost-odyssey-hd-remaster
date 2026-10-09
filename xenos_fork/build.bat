@echo off
setlocal
rem Compila rexgpu-odisea.dll (fork del plugin Xenos) contra el SDK binario.
rem Misma cadena de herramientas que project\build.bat (clang de VS 2022 + Ninja).
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

set "CC=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang.exe"
set "CXX=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang++.exe"

cd /d "%~dp0"
if not defined REXGLUE_SDK_BIN set "REXGLUE_SDK_BIN=%~dp0..\win-amd64"
set "SDKBIN=%REXGLUE_SDK_BIN:\=/%"

cmake -S . -B out\release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH="%SDKBIN%" || exit /b 1
cmake --build out\release || exit /b 1

rem Copia la DLL junto al ejecutable del juego para --gpu_plugin odisea.
copy /Y "out\release\rexgpu-odisea.dll" "..\project\out\build\win-amd64-release\rexgpu-odisea.dll" || exit /b 1
echo.
echo OK: rexgpu-odisea.dll copiada junto a lostodyssey.exe
