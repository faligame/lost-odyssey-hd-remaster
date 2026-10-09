# Reaplica el parche manual de ctx.lr en los hooks de los getters del objeto
# de salida (el codegen del SDK 0.10.0 NO soporta "lr" en registers: lo acepta
# en silencio y emite C++ roto). EJECUTAR DESPUES DE CADA CODEGEN que
# reescriba recomp.139.cpp / recomp.38.cpp. Idempotente.
import os as _os
ROOT = _os.path.abspath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))
import io

PATCHES = [
    (
        _os.path.join(ROOT, 'project', 'generated', 'default', 'lostodyssey_recomp.139.cpp'),
        "extern void LoOutputGetWidthHook(PPCRegister& r3);",
        "extern void LoOutputGetWidthHook(PPCRegister& r3, uint64_t lr);",
        "\tLoOutputGetWidthHook(ctx.r3);",
        "\tLoOutputGetWidthHook(ctx.r3, ctx.lr);",
    ),
    (
        _os.path.join(ROOT, 'project', 'generated', 'default', 'lostodyssey_recomp.38.cpp'),
        "extern void LoOutputGetHeightHook(PPCRegister& r3);",
        "extern void LoOutputGetHeightHook(PPCRegister& r3, uint64_t lr);",
        "\tLoOutputGetHeightHook(ctx.r3);",
        "\tLoOutputGetHeightHook(ctx.r3, ctx.lr);",
    ),
]

# Inyecciones del hook de viewports por item en sub_82300158 (recomp.193.cpp):
# tras cada REX_CALL_INDIRECT_FUNC de los 6 sitios GetWidth/GetHeight se llama
# LoItemViewportHook con el item (r28) y el proveedor (r31).
R193 = _os.path.join(ROOT, 'project', 'generated', 'default', 'lostodyssey_recomp.193.cpp')
ITEM_SITES = [
    ("0x823001C0", 0), ("0x823001FC", 1), ("0x82300238", 2),
    ("0x82300274", 3), ("0x8230071C", 4), ("0x82300748", 5),
]
with io.open(R193, "r", encoding="utf-8") as f:
    src = f.read()
changed = False
decl = "extern void LoItemViewportHook(PPCRegister& r3, uint32_t item, uint32_t provider, int comp);"
if decl not in src:
    src = src.replace("DEFINE_REX_FUNC(sub_82300158) {", decl + "\n\nDEFINE_REX_FUNC(sub_82300158) {")
    changed = True
for lr, comp in ITEM_SITES:
    call = f"\tLoItemViewportHook(ctx.r3, ctx.r28.u32, ctx.r31.u32, {comp});"
    anchor = f"\tctx.lr = {lr};\n\tREX_CALL_INDIRECT_FUNC(ctx.ctr.u32);"
    if call in src:
        continue
    if anchor not in src:
        print(f"recomp.193.cpp: ¡ANCLA {lr} NO ENCONTRADA! revisar a mano")
        continue
    src = src.replace(anchor, anchor + "\n" + call)
    changed = True
if changed:
    with io.open(R193, "w", encoding="utf-8", newline="") as f:
        f.write(src)
    print("recomp.193.cpp: parches de item-viewport aplicados")
else:
    print("recomp.193.cpp: ya parcheado")

for path, old_ext, new_ext, old_call, new_call in PATCHES:
    with io.open(path, "r", encoding="utf-8") as f:
        src = f.read()
    if new_ext in src and new_call in src:
        print(f"{path}: ya parcheado")
        continue
    if old_ext not in src or old_call not in src:
        print(f"{path}: ¡PATRON NO ENCONTRADO! (¿cambio el codegen? revisar a mano)")
        continue
    src = src.replace(old_ext, new_ext).replace(old_call, new_call)
    with io.open(path, "w", encoding="utf-8", newline="") as f:
        f.write(src)
    print(f"{path}: parcheado")
