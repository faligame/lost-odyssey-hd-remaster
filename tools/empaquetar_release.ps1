# Empaqueta la version publica: Rexglue\release\LostOdysseyHD-<version>-win64\ y su .zip
#
#   powershell -ExecutionPolicy Bypass -File empaquetar_release.ps1 [-Version 0.0.1]
#
# Lleva SOLO lo que se puede publicar: el ejecutable y las DLL propias, DXC y DLSS (con sus licencias), la semilla
# y las listas de pipelines esenciales (solo hashes y estados) y los textos. NO lleva: el .map (simbolos, se
# guarda aparte), config/saves/cache/logs, discos, texturas sueltas ni el .lopack (se descarga), ni la DLL
# d3d12.dll "SCSKiller" ni los plugins antiguos de pruebas.
param([string]$Version = "0.0.1")

$root = Split-Path -Parent $PSScriptRoot
$bin  = "$root\project\out\build\win-amd64-release"
$out  = "$root\release\LostOdysseyHD-$Version-win64"
$simbolos = "$root\release\simbolos\$Version"

if (Get-Process lostodyssey -ErrorAction SilentlyContinue) { Write-Error "Cierra el juego primero."; exit 1 }
if (Test-Path $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Force $out, "$out\LICENSES", $simbolos | Out-Null

# Binarios
foreach ($f in "lostodyssey.exe", "updater.exe", "rexruntime.dll", "rexgpu-odisea.dll", "dxcompiler.dll", "nvngx_dlss.dll",
               "prewarm_seed.bin", "prewarm_essentials.rov.d3d12.xpso", "prewarm_essentials.fsi.vk.xpso") {
    if (-not (Test-Path "$bin\$f")) { Write-Warning "falta $f"; continue }
    Copy-Item "$bin\$f" "$out\$f"
}
# Sin la ruta del equipo de compilacion dentro de los binarios (se limpian las copias del paquete)
python (Join-Path $root "tools\limpiar_rutas_binarios.py") $out
# Simbolos para leer los informes de cierre de esta compilacion (NO se publican)
if (Test-Path "$bin\lostodyssey.map") { Copy-Item "$bin\lostodyssey.map" "$simbolos\lostodyssey.map" }

# Interfaz propia cifrada (logo, fuentes x4, botones DualSense): viaja con el juego, no se descarga.
if (Test-Path "$root\ui_assets\ui.lopack") { Copy-Item "$root\ui_assets\ui.lopack" "$out\ui.lopack" } else { Write-Warning "falta ui_assets\ui.lopack" }

# Textos
Copy-Item "$root\release_docs\LEEME.txt" "$out\LEEME.txt"

# Licencias
$lic = @{
  "ReXGlue-SDK-MIT.txt"        = "$root\rexglue-sdk-src\LICENSE"
  "Dear-ImGui-MIT.txt"         = "$root\rexglue-sdk-src\thirdparty\imgui\LICENSE.txt"
  "SDL3-zlib.txt"              = "$root\rexglue-sdk-src\thirdparty\sdl3\LICENSE.txt"
  "spdlog-MIT.txt"             = "$root\rexglue-sdk-src\thirdparty\spdlog\LICENSE"
  "fmt-MIT.txt"                = "$root\rexglue-sdk-src\thirdparty\fmt\LICENSE"
  "xxHash-BSD.txt"             = "$root\rexglue-sdk-src\thirdparty\xxHash\LICENSE"
  "tomlplusplus-MIT.txt"       = "$root\rexglue-sdk-src\thirdparty\tomlplusplus\LICENSE"
  "glslang.txt"                = "$root\rexglue-sdk-src\thirdparty\glslang\LICENSE.txt"
  "VulkanMemoryAllocator-MIT.txt" = "$root\rexglue-sdk-src\thirdparty\vulkan-memory-allocator\LICENSE.txt"
  "SPIRV-Tools-Apache2.txt"    = "$root\rexglue-sdk-src\thirdparty\spirv-tools\LICENSE"
  "FFmpeg-LGPLv2.1.txt"        = "$root\rexglue-sdk-src\thirdparty\FFmpeg\COPYING.LGPLv2.1"
  "SMAA-MIT.txt"               = "$root\xenos_fork\thirdparty\smaa\LICENSE.txt"
  "zstd-BSD.txt"               = "$root\xenos_fork\thirdparty\zstd\LICENSE"
  "lzokay-MIT.txt"             = "$root\project\thirdparty\lzokay\LICENSE"
  "NVIDIA-DLSS.txt"            = "$root\sdk_externos\DLSS\LICENSE.txt"
}
foreach ($k in $lic.Keys) {
  if (Test-Path $lic[$k]) { Copy-Item $lic[$k] "$out\LICENSES\$k" } else { Write-Warning "licencia no encontrada: $($lic[$k])" }
}
@"
Lost Odyssey HD Remaster $Version - componentes de terceros

- ReXGlue SDK (MIT) y codigo derivado de Xenia (BSD-3): base de la recompilacion estatica y de la emulacion de la GPU/kernel.
- Dear ImGui (MIT), SDL3 (zlib), spdlog y fmt (MIT), xxHash (BSD), toml++ (MIT), SMAA (MIT), zstd (BSD), lzokay (MIT),
  Vulkan Memory Allocator (MIT), glslang y SPIRV-Tools (Apache 2 / BSD).
- Microsoft DirectX Shader Compiler (dxcompiler.dll): licencia LLVM con excepciones.
- NVIDIA DLSS (nvngx_dlss.dll): se redistribuye bajo la licencia del SDK de NVIDIA (ver NVIDIA-DLSS.txt).
- FFmpeg (LGPL v2.1) enlazado en rexruntime.dll para la decodificacion de audio y video. El codigo fuente de FFmpeg
  y el script de compilacion usados estan disponibles bajo peticion y en https://ffmpeg.org/.
"@ | Set-Content -Encoding UTF8 "$out\LICENSES\LEEME-LICENCIAS.txt"

# Sumas de control
Get-ChildItem $out -Recurse -File | ForEach-Object {
  $h = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower()
  "{0}  {1}" -f $h, $_.FullName.Substring($out.Length + 1)
} | Set-Content -Encoding UTF8 "$out\SHA256SUMS.txt"

# Zip
$zip = "$out.zip"
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path "$out\*" -DestinationPath $zip -CompressionLevel Optimal
"{0}: {1:N1} MB" -f $zip, ((Get-Item $zip).Length / 1MB)
