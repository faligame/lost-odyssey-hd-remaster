# Todas las lecturas de los globales maestros (lwz 23268/23272 tras lis -31965)
# con su funcion y direccion de instruccion, para el censo completo.
import os as _os
ROOT = _os.path.abspath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))
import glob, os, re

GEN = _os.path.join(ROOT, 'project', 'generated', 'default')
FUNC_RE = re.compile(r"DEFINE_REX_FUNC\((\w+)\) \{")
INSTR_RE = re.compile(r"^\s*//\s*([a-z][a-z0-9_.+-]*)(\s.*)?$")
LOC_RE = re.compile(r"^loc_([0-9A-Fa-f]{8}):")
MASTER_RE = re.compile(r"//\s*lwz r\d+,(23268|23272)\(r\d+\)")

for path in glob.glob(os.path.join(GEN, "lostodyssey_recomp.*.cpp")):
    cur, addr = None, 0
    for ln in open(path, encoding="utf-8", errors="ignore"):
        fm = FUNC_RE.search(ln)
        if fm:
            cur = fm.group(1)
            try:
                addr = int(cur.split("_")[1], 16)
            except (IndexError, ValueError):
                cur = None
            continue
        if not cur:
            continue
        lm = LOC_RE.match(ln.strip())
        if lm:
            addr = int(lm.group(1), 16)
            continue
        im = INSTR_RE.match(ln)
        if im and not ln.strip().startswith("// ppc") and not ln.strip().startswith("// 0x"):
            mm = MASTER_RE.search(ln)
            if mm:
                print("%s @ 0x%08X (%s)" % (cur, addr, mm.group(1)))
            addr += 4
