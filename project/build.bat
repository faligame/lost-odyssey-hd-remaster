@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

set "CC=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang.exe"
set "CXX=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang++.exe"

cd /d "%~dp0"
if not defined REXGLUE_SDK_BIN set "REXGLUE_SDK_BIN=%~dp0..\win-amd64"
set "SDKBIN=%REXGLUE_SDK_BIN:\=/%"

cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH="%SDKBIN%" || exit /b 1
cmake --build --preset win-amd64-release || exit /b 1
