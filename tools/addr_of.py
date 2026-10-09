# Dirección de instrucciones dentro de una función del código generado.
#   python addr_of.py sub_8239BC30 "mr r29,r3"          -> todas las coincidencias
#   python addr_of.py sub_8239BC30 "mr r29,r3" 2        -> la 2ª coincidencia
#   python addr_of.py sub_8239BC30 --dump 0 40          -> instrucciones [0,40) con dirección
# La dirección se deduce contando los comentarios de instrucción ("// op ...")
# desde DEFINE_REX_FUNC; se valida contra las etiquetas loc_XXXXXXXX.
import glob, os, re, sys

GEN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "project", "generated", "default")
INSTR_RE = re.compile(r"^\s*//\s*([a-z][a-z0-9_.+-]*)(\s.*)?$")
LOC_RE = re.compile(r"^loc_([0-9A-Fa-f]{8}):")


def load_function(name):
    for path in glob.glob(os.path.join(GEN, "lostodyssey_recomp.*.cpp")):
        text = open(path, encoding="utf-8", errors="ignore").read()
        m = re.search(r"DEFINE_REX_FUNC\(%s\) \{\n" % re.escape(name), text)
        if not m:
            continue
        body = text[m.end():]
        end = body.find("\n}\n")
        return path, body[:end].split("\n")
    raise SystemExit("no se encontro " + name)


def main():
    name = sys.argv[1]
    start = int(name.split("_")[1], 16)
    path, lines = load_function(name)
    instrs = []  # (addr, text, line)
    addr = start
    errors = 0
    for ln in lines:
        lm = LOC_RE.match(ln)
        if lm:
            label = int(lm.group(1), 16)
            if label != addr:
                # Datos en linea (tablas de salto tras un bctr) sin comentario de
                # instruccion: la etiqueta manda, resincronizar.
                errors += 1
                print("AVISO: etiqueta loc_%08X en la direccion calculada %08X (resincronizado, +%d bytes)"
                      % (label, addr, label - addr))
                addr = label
            continue
        im = INSTR_RE.match(ln)
        if im and not ln.strip().startswith("// ppc") and not ln.strip().startswith("// 0x"):
            instrs.append((addr, ln.strip()[3:]))
            addr += 4
    print("%s en %s: %d instrucciones, %d desalineaciones" % (name, os.path.basename(path), len(instrs), errors))
    if sys.argv[2] == "--dump":
        a, b = int(sys.argv[3]), int(sys.argv[4])
        for addr, txt in instrs[a:b]:
            print("  0x%08X  %s" % (addr, txt))
        return
    pat = re.compile(sys.argv[2])
    want = int(sys.argv[3]) if len(sys.argv) > 3 else None
    hits = [(i, addr, txt) for i, (addr, txt) in enumerate(instrs) if pat.search(txt)]
    for k, (i, addr, txt) in enumerate(hits, 1):
        if want is None or want == k:
            print("  #%d  0x%08X  [%d]  %s" % (k, addr, i, txt))


if __name__ == "__main__":
    main()
