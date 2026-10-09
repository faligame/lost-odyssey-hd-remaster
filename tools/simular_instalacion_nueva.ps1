# Simula el primer arranque de un jugador que acaba de instalar el port:
# aparta (renombra, no borra) las cachés de sombreadores del juego y del
# controlador. Con -Restaurar las devuelve a su sitio.
#
#   powershell -ExecutionPolicy Bypass -File simular_instalacion_nueva.ps1
#   powershell -ExecutionPolicy Bypass -File simular_instalacion_nueva.ps1 -Restaurar
#
# Cierra antes el juego y todo lo que use la GPU (navegadores, Steam, Discord...):
# si algo tiene abierta la caché del controlador, no se podrá renombrar.
# La caché de NVIDIA es de TODOS los juegos: mientras esté apartada, cada juego
# recompila sus sombreadores la primera vez (luego se restaura tal cual).
param([switch]$Restaurar)

$juego = Join-Path (Split-Path -Parent $PSScriptRoot) "project\out\build\win-amd64-release"
$rutas = @(
    "$juego\cache\shaders\shareable",          # .xsh, .xpso, .pipelib, .pipecache del port
    "$env:LOCALAPPDATA\NVIDIA\DXCache",        # PSO D3D12 compilados por el controlador
    "$env:LOCALAPPDATA\NVIDIA\GLCache",        # idem Vulkan
    "$env:LOCALAPPDATA\D3DSCache"              # caché de sombreadores de Windows
)
$sufijo = ".apartado_prueba"

if (Get-Process lostodyssey -ErrorAction SilentlyContinue) {
    Write-Error "El juego está abierto: ciérralo primero."; exit 1
}
foreach ($r in $rutas) {
    $apartada = "$r$sufijo"
    if ($Restaurar) {
        if (Test-Path $apartada) {
            if (Test-Path $r) { Remove-Item $r -Recurse -Force }   # lo generado durante la prueba
            Rename-Item $apartada (Split-Path $r -Leaf)
            "restaurada: $r"
        }
    } elseif ((Test-Path $apartada) -and -not (Test-Path $r)) {
        "ya estaba apartada: $r"
    } elseif (Test-Path $r) {
        if (Test-Path $apartada) {
            # Ya hay una copia apartada y el juego ha vuelto a crear la carpeta:
            # lo nuevo es de la prueba en curso, no se toca.
            Write-Warning "ya hay una copia apartada de $r; se deja lo que hay ahora (¿falta -Restaurar?)"
            continue
        }
        try { Rename-Item $r (Split-Path $apartada -Leaf) -ErrorAction Stop; "apartada: $r" }
        catch { Write-Warning "no se pudo apartar $r (¿algo la está usando?): $_" }
    }
}
