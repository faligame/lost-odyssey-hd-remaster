"""Compila los compute shaders propios del fork para la interfaz y DLSS.

Fuentes: src/graphics/shaders/<nombre>.cs.hlsl (odisea_hud_compose, odisea_dlss_inputs).
Salida, en el mismo formato que las cabeceras de shaders del SDK:
  src/graphics/shaders/bytecode/d3d12_5_1/<nombre>_cs.h   (fxc, DXBC)
  src/graphics/shaders/vulkan_spirv/<nombre>_cs.h         (dxc, SPIR-V)

Solo hace falta volver a ejecutarlo si cambia el .hlsl.
"""
import glob
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DXBC = os.path.join(ROOT, "src", "graphics", "shaders", "bytecode", "d3d12_5_1")
OUT_SPIRV = os.path.join(ROOT, "src", "graphics", "shaders", "vulkan_spirv")
SHADERS = (("odisea_hud_compose", "cs"), ("odisea_dlss_inputs", "cs"), ("odisea_dlss_mask", "cs"),
           ("odisea_dlss_reactive", "cs"), ("odisea_dlss_object_motion", "vs"),
           ("odisea_dlss_object_motion", "ps"))

VULKAN_SDK = os.environ.get("VULKAN_SDK", r"C:\VulkanSDK\1.4.350.0")
DXC = os.path.join(VULKAN_SDK, "Bin", "dxc.exe")
SPIRV_VAL = os.path.join(VULKAN_SDK, "Bin", "spirv-val.exe")


def find_fxc():
    candidates = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\fxc.exe"))
    if not candidates:
        sys.exit("No encuentro fxc.exe (Windows SDK)")
    return candidates[-1]


def run(args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        sys.exit("Fallo: " + os.path.basename(args[0]))
    return result


def build(shader, stage):
    name = shader + "_" + stage
    source = os.path.join(ROOT, "src", "graphics", "shaders", shader + "." + stage + ".hlsl")
    run([find_fxc(), "/nologo", "/T", stage + "_5_0", "/E", "main", "/O3", "/Vn", name,
         "/Fh", os.path.join(OUT_DXBC, name + ".h"), source])
    print("DXBC   ", name)
    if not (os.path.isfile(DXC) and os.path.isfile(SPIRV_VAL)):
        print("Sin SDK de Vulkan: no genero SPIR-V")
        return
    with tempfile.TemporaryDirectory() as tmp:
        spv = os.path.join(tmp, name + ".spv")
        run([DXC, "-nologo", "-T", stage + "_6_0", "-E", "main", "-O3", "-spirv", "-D", "XE_SPIRV=1",
             "-fspv-target-env=vulkan1.1", "-fvk-use-dx-layout", "-Fo", spv, source])
        run([SPIRV_VAL, "--target-env", "vulkan1.1", spv])
        data = open(spv, "rb").read()
        words = struct.unpack("<%dI" % (len(data) // 4), data)
        lines = ["// Generado por tools/build_hud_shaders.py a partir de",
                 "// %s.%s.hlsl. No editar a mano." % (shader, stage),
                 "const uint32_t %s[] = {" % name]
        for i in range(0, len(words), 6):
            lines.append("    " + ", ".join("0x%08X" % w for w in words[i:i + 6]) + ",")
        lines.append("};")
        with open(os.path.join(OUT_SPIRV, name + ".h"), "w", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
    print("SPIR-V ", name)


def main():
    for shader, stage in SHADERS:
        build(shader, stage)


if __name__ == "__main__":
    main()
