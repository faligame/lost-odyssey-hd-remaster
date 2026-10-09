"""Busca funciones recompiladas que LEEN un registro no volatil (r14-r31, f14-f31, v14-v31,
v64-v127) antes de asignarlo, con non_volatile_as_local activado en el codegen.

Con esa opcion cada funcion tiene sus propias variables locales para esos registros, que
empiezan a 0. Una funcion que sigue el ABI nunca lee un no volatil antes de escribirlo (salvo
para guardarlo en la pila en el prologo, que es inofensivo). Si lo lee, espera un valor del
llamador: convencion propia, tipica del ensamblador escrito a mano (codecs de video, memcpy...).
Esas funciones hay que marcarlas share_registers en el manifiesto.

Analisis lineal (sin flujo de control): da candidatos, no certezas.

Uso: python buscar_registros_heredados.py <carpeta generated/default> [max]
"""
import glob
import os
import re
import sys

FUNC_RE = re.compile(r'^DEFINE_REX_FUNC\((\w+)\)\s*\{')
DECL_RE = re.compile(r'^\s*PPC(?:Register|VRegister|FRegister)?\w*\s+([rfv]\d+)\{\};')
TOKEN_RE = re.compile(r'(?<![\w.])([rfv])(\d+)\b(?=\.)')
# Guardado del prologo: REX_STORE_xx(ctx.r1.u32 + N, rNN.xxx) o similares con r12/r11 de base.
SAVE_RE = re.compile(r'REX_STORE_\w+\(\s*(?:ctx\.)?r1[12]?\.u32\s*[+-]')
SAVE_VEC_RE = re.compile(r'(?:_mm_store|store_?v|REX_STORE_V)')


def non_volatile(kind, n):
    if kind in 'rf':
        return n >= 14
    return 14 <= n <= 31 or 64 <= n <= 127


def scan_file(path, results):
    name = None
    locals_ = set()
    written = set()
    flagged = {}
    with open(path, encoding='utf-8', errors='ignore') as f:
        for line in f:
            m = FUNC_RE.match(line)
            if m:
                if name and flagged:
                    results.append((name, os.path.basename(path), flagged))
                name, locals_, written, flagged = m.group(1), set(), set(), {}
                continue
            if name is None:
                continue
            d = DECL_RE.match(line)
            if d:
                locals_.add(d.group(1))
                continue
            stripped = line.strip()
            if stripped.startswith('//') or not stripped:
                continue
            # Asignacion: "rN.xxx = ..." al principio de la sentencia.
            lhs = re.match(r'^\s*([rfv]\d+)\.\w+(?:\[\w+\])?\s*=', line)
            is_save = bool(SAVE_RE.search(line)) or bool(SAVE_VEC_RE.search(line))
            body = line
            if lhs:
                body = line.split('=', 1)[1]
            for kind, num in TOKEN_RE.findall(body):
                reg = f'{kind}{num}'
                if reg not in locals_ or not non_volatile(kind, int(num)):
                    continue
                if reg in written or is_save:
                    continue
                flagged.setdefault(reg, stripped[:110])
            if lhs:
                written.add(lhs.group(1))
            if stripped == '}' and line.startswith('}'):
                if flagged:
                    results.append((name, os.path.basename(path), flagged))
                name = None
    if name and flagged:
        results.append((name, os.path.basename(path), flagged))


def main():
    root = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    results = []
    for path in sorted(glob.glob(os.path.join(root, 'lostodyssey_recomp.*.cpp'))):
        scan_file(path, results)
    print(f'{len(results)} funciones candidatas')
    for name, file, flagged in results[:limit]:
        regs = ', '.join(sorted(flagged))
        first = next(iter(flagged.values()))
        print(f'{name}  [{file}]  lee antes de escribir: {regs}   ej.: {first}')


if __name__ == '__main__':
    main()
