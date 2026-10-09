# Puente entre nuestras partidas (carpeta con save.bin) y el editor con interfaz
# grafica, que solo abre paquetes CON de Xbox 360.
#
#   empaquetar     <ranura> <paquete.con>   mete el save.bin en un paquete
#   desempaquetar  <paquete.con> <ranura>   devuelve el save.bin a la ranura
#   editar         <ranura>                 empaqueta, abre el editor, espera y devuelve
#
# El empaquetado usa X360.dll, la libreria que ya viene con el editor. El cifrado
# lo sigue haciendo el propio editor; para trabajar sin el esta Rexglue/tools/lo_save.py.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet("empaquetar", "desempaquetar", "editar")]
    [string]$Accion,
    [Parameter(Mandatory = $true)][string]$Ruta,
    [string]$Destino,
    [string]$CarpetaEditor = "$PSScriptRoot\..\..\save_editor"
)

$ErrorActionPreference = "Stop"

function Cargar-Libreria {
    foreach ($dll in @("PackageIO.dll", "X360.dll")) {
        $ruta = Join-Path $CarpetaEditor $dll
        if (-not (Test-Path $ruta)) { throw "falta $dll en $CarpetaEditor" }
        [Reflection.Assembly]::LoadFrom($ruta) | Out-Null
    }
}

function Empaquetar([string]$ranura, [string]$paquete) {
    $save = Join-Path $ranura "save.bin"
    if (-not (Test-Path $save)) { throw "no hay save.bin en $ranura" }
    Cargar-Libreria
    $crear = New-Object X360.STFS.CreateSTFS
    $crear.HeaderData.TitleID = 0x4D5307FA
    $crear.HeaderData.ThisType = [X360.STFS.PackageType]::SavedGame
    $crear.HeaderData.Title_Display = "Lost Odyssey"
    $crear.HeaderData.Title_Package = "Lost Odyssey"
    $crear.HeaderData.Description = (Split-Path $ranura -Leaf)
    [void]$crear.AddFile($save, "save.bin")
    $miniatura = Join-Path $ranura "__thumbnail.png"
    if (Test-Path $miniatura) { [void]$crear.AddFile($miniatura, "__thumbnail.png") }

    if (Test-Path $paquete) { Remove-Item $paquete -Force }
    $log = New-Object X360.Other.LogRecord
    $rsa = New-Object X360.STFS.RSAParams -ArgumentList ([X360.STFS.StrongSigned]::LIVE)
    $pkg = New-Object X360.STFS.STFSPackage -ArgumentList $crear, $rsa, $paquete, $log
    [void]$pkg.CloseIO()
    if (-not (Test-Path $paquete)) { throw "no se pudo crear el paquete" }
    "paquete creado: $paquete ({0:N0} bytes)" -f (Get-Item $paquete).Length
}

function Desempaquetar([string]$paquete, [string]$ranura) {
    if (-not (Test-Path $paquete)) { throw "no existe $paquete" }
    Cargar-Libreria
    $log = New-Object X360.Other.LogRecord
    $pkg = New-Object X360.STFS.STFSPackage -ArgumentList $paquete, $log
    try {
        $fichero = $pkg.GetFile("save.bin")
        if (-not $fichero) { throw "el paquete no contiene save.bin" }
        $destino = Join-Path $ranura "save.bin"
        $copia = "$destino.previo"
        if (Test-Path $destino) { Copy-Item $destino $copia -Force }
        if (-not $fichero.Extract($destino)) { throw "no se pudo extraer save.bin" }
        "save.bin devuelto a $ranura (copia de seguridad en save.bin.previo)"
    }
    finally { [void]$pkg.CloseIO() }
}

switch ($Accion) {
    "empaquetar" { Empaquetar $Ruta $Destino }
    "desempaquetar" { Desempaquetar $Ruta $Destino }
    "editar" {
        $ranura = $Ruta
        $paquete = Join-Path $env:TEMP ("lo_" + (Split-Path $ranura -Leaf) + ".con")
        Empaquetar $ranura $paquete
        $editor = Join-Path $CarpetaEditor "LostOdysseySaveEditor.exe"
        if (-not (Test-Path $editor)) { throw "no se encuentra el editor en $CarpetaEditor" }
        ""
        "Se abre el editor. Dentro:"
        "   1. Abre el paquete:  $paquete"
        "   2. Cambia lo que quieras y guarda."
        "   3. Cierra el editor: al cerrarlo, la partida vuelve sola a su sitio."
        ""
        Start-Process explorer.exe -ArgumentList "/select,`"$paquete`""
        $proceso = Start-Process $editor -PassThru
        $proceso.WaitForExit()
        Desempaquetar $paquete $ranura
        $comprobador = Join-Path $PSScriptRoot "lo_save.py"
        if (Test-Path $comprobador) { python $comprobador info (Join-Path $ranura "save.bin") }
    }
}
