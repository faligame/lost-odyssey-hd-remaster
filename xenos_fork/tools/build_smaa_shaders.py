"""Compila los compute shaders del SMAA del fork para D3D12 y Vulkan.

Fuente: src/graphics/shaders/xenos1080_smaa.cs.hlsl (tres pasadas, XE_SMAA_PASS).
Salida, en el mismo formato que las cabeceras de shaders del SDK:
  src/graphics/shaders/bytecode/d3d12_5_1/xenos1080_smaa_<pasada>_cs.h   (fxc, DXBC)
  src/graphics/shaders/vulkan_spirv/xenos1080_smaa_<pasada>_cs.h         (dxc, SPIR-V)

Cada SPIR-V se valida con spirv-val antes de escribir la cabecera. Solo hace
falta volver a ejecutarlo si cambia el .hlsl o el SMAA de thirdparty.
"""
import glob
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "src", "graphics", "shaders", "xenos1080_smaa.cs.hlsl")
OUT_DXBC = os.path.join(ROOT, "src", "graphics", "shaders", "bytecode", "d3d12_5_1")
OUT_SPIRV = os.path.join(ROOT, "src", "graphics", "shaders", "vulkan_spirv")

VULKAN_SDK = os.environ.get("VULKAN_SDK", r"C:\VulkanSDK\1.4.350.0")
DXC = os.path.join(VULKAN_SDK, "Bin", "dxc.exe")
SPIRV_VAL = os.path.join(VULKAN_SDK, "Bin", "spirv-val.exe")

PASSES = (("edges", 0), ("weights", 1), ("blend", 2))


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
        sys.exit("Fallo: " + " ".join(os.path.basename(a) if i == 0 else a
                                      for i, a in enumerate(args)))
    return result


def write_spirv_header(spv_path, header_path, name, pass_index):
    data = open(spv_path, "rb").read()
    if len(data) % 4:
        sys.exit(name + ": el SPIR-V no mide un multiplo de 4 bytes")
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    if words[0] != 0x07230203:
        sys.exit(name + ": numero magico de SPIR-V incorrecto")
    lines = [
        "// Generado por tools/build_smaa_shaders.py a partir de",
        "// xenos1080_smaa.cs.hlsl (XE_SMAA_PASS=%d). No editar a mano." % pass_index,
        "const uint32_t %s[] = {" % name,
    ]
    for i in range(0, len(words), 6):
        lines.append("    " + ", ".join("0x%08X" % w for w in words[i:i + 6]) + ",")
    lines.append("};")
    with open(header_path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    fxc = find_fxc()
    for tool in (DXC, SPIRV_VAL):
        if not os.path.isfile(tool):
            sys.exit("No encuentro " + tool)
    os.makedirs(OUT_DXBC, exist_ok=True)
    os.makedirs(OUT_SPIRV, exist_ok=True)

    for pass_name, pass_index in PASSES:
        name = "xenos1080_smaa_%s_cs" % pass_name

        dxbc_header = os.path.join(OUT_DXBC, name + ".h")
        run([fxc, "/nologo", "/T", "cs_5_0", "/E", "main", "/O3",
             "/D", "XE_SMAA_PASS=%d" % pass_index,
             "/Vn", name, "/Fh", dxbc_header, SOURCE])
        print("DXBC   ", os.path.relpath(dxbc_header, ROOT))

        with tempfile.TemporaryDirectory() as tmp:
            spv = os.path.join(tmp, name + ".spv")
            run([DXC, "-spirv", "-T", "cs_6_0", "-E", "main", "-O3",
                 "-fspv-target-env=vulkan1.0",
                 "-D", "XE_SMAA_SPIRV=1", "-D", "XE_SMAA_PASS=%d" % pass_index,
                 "-Fo", spv, SOURCE])
            run([SPIRV_VAL, "--target-env", "vulkan1.0", spv])
            spirv_header = os.path.join(OUT_SPIRV, name + ".h")
            write_spirv_header(spv, spirv_header, name, pass_index)
            print("SPIR-V ", os.path.relpath(spirv_header, ROOT), "(spirv-val OK)")


if __name__ == "__main__":
    main()
