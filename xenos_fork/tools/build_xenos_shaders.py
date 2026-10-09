"""Compila los shaders del Xenos (fuentes XeSL de Xenia) igual que `xb buildshaders`.

Fuentes: src/graphics/shaders/xesl/ reproduce el arbol de Xenia (src/xenia/gpu/shaders, src/xenia/ui/shaders
y third_party/fxaa, por las rutas relativas de los #include) -- copia de
src/xenia/gpu/shaders y src/xenia/ui/shaders de Xenia Canary
en el commit cbbaae8ea (4-ene-2026; licencia BSD, ver LICENSE-xenia.txt). Es el commit
cuyos compilados coinciden byte a byte con los que trae el SDK de ReXGlue 0.10.0 (109 de
109; resolve_downscale es propio de ReXGlue y su fuente ya estaba en shaders/).

Mismas opciones que Xenia (xenia-build, buildshaders):
  DXBC:  fxc /D SHADING_LANGUAGE_HLSL_XE=1 /Fh <out>.h /T <etapa>_5_1 /Vn <id> /nologo <src>
  SPIR-V: glslangValidator --stdin -DSHADING_LANGUAGE_GLSL_XE=1 -S <etapa> -V -I<dir>, con el
          envoltorio "#version 460 ... #include", y despues spirv-opt -O --canonicalize-ids.
          La cabecera .h lleva el desensamblado (spirv-dis) y las palabras en hex.

Por defecto define XE_EDRAM_TILE_COUNT = xenos::kEdramTileCount del fork (EDRAM ampliada,
el equivalente en fuente de lo que hacian patch_shaders.py / patch_shaders_spirv.py sobre el
binario); --xenia compila los originales de Xenia (2048 tiles).

Uso:
  python build_xenos_shaders.py --instalar          (compila y deja los .h donde los usa CMake)
  python build_xenos_shaders.py --salida <dir> [--solo patron] [--dxbc] [--spirv] [-D NOMBRE=VALOR ...]
  python build_xenos_shaders.py --comparar <dir compilado> <dir de referencia>   (bytes, no texto)

Sin --dxbc/--spirv compila los dos.
"""

import argparse
import fnmatch
import glob
import os
import re
import subprocess
import sys
import tempfile

AQUI = os.path.dirname(os.path.abspath(__file__))
FUENTES = os.path.join(os.path.dirname(AQUI), "src", "graphics", "shaders", "xesl", "src", "xenia", "gpu", "shaders")
ETAPAS_DXBC = ["vs", "hs", "ds", "gs", "ps", "cs"]
ETAPAS_SPIRV = {"vs": "vert", "hs": "tesc", "ds": "tese", "gs": "geom", "ps": "frag", "cs": "comp"}
ENVOLTORIO_SPIRV = ("#version 460\n"
                    "#extension GL_EXT_control_flow_attributes : require\n"
                    "#extension GL_EXT_samplerless_texture_functions : require\n"
                    "#extension GL_GOOGLE_include_directive : require\n"
                    "#include \"%s\"\n")


def buscar_fxc():
    base = os.environ.get("ProgramFiles(x86)") or os.environ.get("ProgramFiles")
    rutas = sorted(glob.glob(os.path.join(base, "Windows Kits/10/bin/*", "x64", "fxc.exe")))
    if not rutas:
        sys.exit("no se encuentra fxc.exe (Windows SDK)")
    return rutas[-1]


def vulkan_bin(nombre):
    sdk = os.environ.get("VULKAN_SDK")
    if not sdk:
        sys.exit("falta VULKAN_SDK")
    for carpeta in ("Bin", "bin"):
        p = os.path.join(sdk, carpeta, nombre + (".exe" if os.name == "nt" else ""))
        if os.path.exists(p):
            return p
    sys.exit("no se encuentra " + nombre)


def fuentes(extension_ok, solo):
    for nombre in sorted(os.listdir(FUENTES)):
        if not any(nombre.endswith(e) for e in extension_ok):
            continue
        if len(nombre) <= 8 or nombre[-8] != ".":
            continue
        ident = nombre[:-5].replace(".", "_")
        if solo and not any(fnmatch.fnmatch(ident, s) for s in solo):
            continue
        yield nombre, ident


RAIZ = os.path.dirname(AQUI)
INSTALAR_DXBC = os.path.join(RAIZ, "src", "graphics", "shaders", "bytecode", "d3d12_5_1")
INSTALAR_SPIRV = os.path.join(RAIZ, "src", "graphics", "shaders", "vulkan_spirv")


def compilar_dxbc(dest, solo, defines):
    fxc = buscar_fxc()
    os.makedirs(dest, exist_ok=True)
    n = 0
    for nombre, ident in fuentes((".hlsl", ".xesl"), solo):
        etapa = ident[-2:]
        if etapa not in ETAPAS_DXBC:
            continue
        args = [fxc, "/D", "SHADING_LANGUAGE_HLSL_XE=1"]
        for d in defines:
            args += ["/D", d]
        args += ["/Fh", os.path.join(dest, ident + ".h"), "/T", etapa + "_5_1", "/Vn", ident, "/nologo",
                 os.path.join(FUENTES, nombre)]
        if subprocess.call(args, stdout=subprocess.DEVNULL) != 0:
            sys.exit("fallo al compilar (DXBC) " + nombre)
        n += 1
    print(f"DXBC: {n} shaders en {dest}")


def compilar_spirv(dest, solo, defines):
    glslang = vulkan_bin("glslangValidator")
    spirv_opt = vulkan_bin("spirv-opt")
    spirv_dis = vulkan_bin("spirv-dis")
    os.makedirs(dest, exist_ok=True)
    n = 0
    with tempfile.TemporaryDirectory() as tmp:
        for nombre, ident in fuentes((".glsl", ".xesl"), solo):
            etapa = ETAPAS_SPIRV.get(ident[-2:])
            if etapa is None:
                continue
            es_xesl = nombre.endswith(".xesl")
            spv_bruto = os.path.join(tmp, ident + ".glslang.spv")
            args = [glslang, "--stdin" if es_xesl else os.path.join(FUENTES, nombre),
                    "-DSHADING_LANGUAGE_GLSL_XE=1"] + ["-D" + d for d in defines] + \
                   ["-S", etapa, "-o", spv_bruto, "-V"]
            if es_xesl:
                args.append("-I" + FUENTES)
            r = subprocess.run(args, input=(ENVOLTORIO_SPIRV % nombre) if es_xesl else None,
                               universal_newlines=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if r.returncode != 0:
                print(r.stdout)
                sys.exit("fallo al compilar (SPIR-V) " + nombre)
            spv = os.path.join(tmp, ident + ".spv")
            # --strip-debug: los SPIR-V del SDK de ReXGlue no llevan nombres de depuracion
            # (Xenia si los deja). Con la version de spirv-opt del Vulkan SDK 1.4.350 el
            # resultado difiere del del SDK solo en unos pocos operandos que apuntan a otro
            # identificador con el mismo valor (equivalente).
            if subprocess.call([spirv_opt, "-O", "--canonicalize-ids", "--strip-debug", spv_bruto, "-o", spv]) != 0:
                sys.exit("fallo en spirv-opt con " + nombre)
            txt = os.path.join(tmp, ident + ".txt")
            if subprocess.call([spirv_dis, "-o", txt, spv]) != 0:
                sys.exit("fallo en spirv-dis con " + nombre)
            with open(os.path.join(dest, ident + ".h"), "w", newline="\n") as out:
                out.write("// Generated with `xb buildshaders`.\n#if 0\n")
                dis = open(txt).read()
                out.write(dis if dis.endswith("\n") else dis + "\n")
                out.write("#endif\n\nconst uint32_t %s[] = {" % ident)
                datos = open(spv, "rb").read()
                for i in range(0, len(datos), 4):
                    out.write("\n    " if (i // 4) % 6 == 0 else " ")
                    out.write("0x%08X," % int.from_bytes(datos[i:i + 4], sys.byteorder))
                out.write("\n};\n")
            n += 1
    print(f"SPIR-V: {n} shaders en {dest}")


def bytes_de_cabecera(ruta):
    """Los datos del array de una cabecera generada (DXBC en bytes, SPIR-V en palabras)."""
    t = open(ruta, encoding="latin-1").read()
    cuerpo = t[t.index("{", t.index("#endif")) + 1:]
    cuerpo = cuerpo[:cuerpo.index("}")]
    valores = [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", cuerpo)]
    if "uint32_t" in t[t.index("#endif"):t.index("{", t.index("#endif"))]:
        return b"".join(v.to_bytes(4, "little") for v in valores)
    return bytes(valores)


def comparar(compilado, referencia):
    iguales, distintos, faltan = [], [], []
    for sub in ("d3d12_5_1", "vulkan_spirv"):
        dc, dr = os.path.join(compilado, sub), os.path.join(referencia, sub)
        if not os.path.isdir(dc) or not os.path.isdir(dr):
            continue
        for f in sorted(os.listdir(dc)):
            if not f.endswith(".h"):
                continue
            r = os.path.join(dr, f)
            if not os.path.exists(r):
                faltan.append(f"{sub}/{f}")
            elif bytes_de_cabecera(os.path.join(dc, f)) == bytes_de_cabecera(r):
                iguales.append(f"{sub}/{f}")
            else:
                distintos.append(f"{sub}/{f}")
    print(f"iguales: {len(iguales)}  distintos: {len(distintos)}  sin referencia: {len(faltan)}")
    for d in distintos:
        print("  DISTINTO", d)
    for d in faltan:
        print("  sin referencia", d)
    return not distintos


def tiles_del_fork():
    """xenos::kEdramTileCount de include/rex/graphics/xenos.h (EDRAM ampliada)."""
    xenos_h = os.path.join(os.path.dirname(AQUI), "include", "rex", "graphics", "xenos.h")
    m = re.search(r"constexpr uint32_t kEdramTileCount = (\d+);", open(xenos_h, encoding="utf-8").read())
    if not m:
        sys.exit("no se encuentra kEdramTileCount en xenos.h")
    return int(m.group(1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--salida")
    ap.add_argument("--instalar", action="store_true",
                    help="escribe en las carpetas de compilados del fork (las que usa CMake)")
    ap.add_argument("--solo", action="append", default=[], help="patron del identificador (fnmatch)")
    ap.add_argument("--dxbc", action="store_true")
    ap.add_argument("--spirv", action="store_true")
    ap.add_argument("-D", dest="defines", action="append", default=[])
    ap.add_argument("--xenia", action="store_true",
                    help="EDRAM de Xenia (2048 tiles) en vez de la del fork (kEdramTileCount)")
    ap.add_argument("--comparar", nargs=2, metavar=("COMPILADO", "REFERENCIA"))
    a = ap.parse_args()
    if a.comparar:
        sys.exit(0 if comparar(*a.comparar) else 1)
    if not a.salida and not a.instalar:
        ap.error("falta --salida o --instalar")
    if not a.xenia and not any(d.startswith("XE_EDRAM_TILE_COUNT") for d in a.defines):
        a.defines.append("XE_EDRAM_TILE_COUNT=%du" % tiles_del_fork())
    print("defines:", " ".join(a.defines) or "(ninguno)")
    todos = not a.dxbc and not a.spirv
    if todos or a.dxbc:
        compilar_dxbc(INSTALAR_DXBC if a.instalar else os.path.join(a.salida, "d3d12_5_1"), a.solo, a.defines)
    if todos or a.spirv:
        compilar_spirv(INSTALAR_SPIRV if a.instalar else os.path.join(a.salida, "vulkan_spirv"), a.solo,
                       a.defines)


if __name__ == "__main__":
    main()
