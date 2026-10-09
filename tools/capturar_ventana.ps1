param([int]$ProcessId, [string]$Salida)
# Captura la ventana de un proceso aunque este tapada (PrintWindow con
# PW_RENDERFULLCONTENT). Uso: capturar_ventana.ps1 -ProcessId <pid> -Salida <png>
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Cap {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$p = Get-Process -Id $ProcessId
$h = $p.MainWindowHandle
$r = New-Object Cap+RECT
[Cap]::GetClientRect($h, [ref]$r) | Out-Null
$w = [Math]::Max(1, $r.R - $r.L); $hh = [Math]::Max(1, $r.B - $r.T)
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Cap]::PrintWindow($h, $hdc, 3) | Out-Null
$g.ReleaseHdc($hdc); $g.Dispose()
$bmp.Save($Salida); $bmp.Dispose()
"$w x $hh"
