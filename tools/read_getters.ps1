$p = Get-Process lostodyssey -ErrorAction SilentlyContinue
if (-not $p) { "juego cerrado"; exit 1 }
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public class RPMX {
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int size, out IntPtr read);
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint acc, bool inh, int pid);
}
'@
$h = [RPMX]::OpenProcess(0x410, $false, $p.Id)
function ReadBE32($guest) {
  $buf = New-Object byte[] 4; $r = [IntPtr]::Zero
  $ok = [RPMX]::ReadProcessMemory($h, [IntPtr]([int64]0x100000000 + $guest), $buf, 4, [ref]$r)
  if (-not $ok) { return $null }
  return ([uint32]$buf[0] -shl 24) -bor ([uint32]$buf[1] -shl 16) -bor ([uint32]$buf[2] -shl 8) -bor [uint32]$buf[3]
}
"master w = " + (ReadBE32 0x83235AE4) + "  h = " + (ReadBE32 0x83235AE8)
$a = ReadBE32 0x83315FB4
"global 0x83315FB4 -> 0x{0:X8}" -f $a
if ($a) {
  $b = ReadBE32 ($a + 708)
  "  +708 -> 0x{0:X8}" -f $b
  if ($b) {
    $obj = ReadBE32 ($b + 68)
    "  +68 obj -> 0x{0:X8}" -f $obj
    if ($obj) {
      $vt = ReadBE32 $obj
      "  vtable -> 0x{0:X8}" -f $vt
      if ($vt) {
        "  GetWidth  = 0x{0:X8}" -f (ReadBE32 $vt)
        "  GetHeight = 0x{0:X8}" -f (ReadBE32 ($vt + 4))
      }
    }
  }
}

