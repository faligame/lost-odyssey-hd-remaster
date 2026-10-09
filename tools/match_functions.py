#!/usr/bin/env python3
"""Match named functions from one ReXGlue-generated project (the *source*, e.g.
Blue Dragon generated with re:Blue's functions.toml) against the unnamed
functions of another (the *target*, Lost Odyssey), by instruction *shape*.

ReXGlue's generated `*_recomp.N.cpp` files carry the original PPC instruction
as a `// mnemonic operands` comment above every translated line, so both
binaries can be compared on already-decrypted, already-disassembled code
without touching the XEX encryption.

Shape normalization (what is masked so two builds of the same XDK code still
compare equal even though they were linked at different addresses):
  * every branch target                        -> T
  * lis/addis immediates (address high halves)  -> #
  * the displacement of any D-form instruction whose base register was loaded
    by a lis within the last few instructions (address low halves)  -> #
Everything else (li/ori constants, stack/struct offsets, registers, cr fields)
is kept, since same-source code keeps those across links.

Usage:
  match_functions.py SRC_GEN_DIR TGT_GEN_DIR [--names src_functions.toml]
                     [--prefix D3D] [--min-insns 8] [--out matches.toml]
                     [--report report.tsv]
"""
import argparse
import collections
import glob
import os
import re
import sys

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
ASM_RE = re.compile(r"^\t// (\S+)\s*(.*)$")
BRANCH_RE = re.compile(r"^b[a-z]*[+-]?$")  # b, bl, beq, bne, bdnz, blr, bctr...
HEX_RE = re.compile(r"0x[0-9a-fA-F]+")
DFORM_RE = re.compile(r"^(-?\d+)\((r\d+)\)$")
LIS_WINDOW = 6


def parse_generated(gen_dir):
    """Return {func_name: [(mnemonic, operands), ...]} for a generated dir."""
    funcs = {}
    files = sorted(glob.glob(os.path.join(gen_dir, "*_recomp.*.cpp")))
    if not files:
        sys.exit(f"no *_recomp.*.cpp in {gen_dir}")
    for path in files:
        cur = None
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = FUNC_RE.match(line)
                if m:
                    cur = m.group(1)
                    funcs[cur] = []
                    continue
                if cur is None:
                    continue
                m = ASM_RE.match(line)
                if m:
                    funcs[cur].append((m.group(1), m.group(2).strip()))
    return funcs


def normalize(insns):
    """Turn [(mnemonic, operands)] into a list of shape strings."""
    out = []
    lis_regs = {}  # reg -> index of the lis that set it
    for i, (mn, ops) in enumerate(insns):
        parts = [p.strip() for p in ops.split(",")] if ops else []
        if BRANCH_RE.match(mn) and parts and HEX_RE.fullmatch(parts[-1] or ""):
            parts[-1] = "T"
        elif mn in ("lis", "addis") and len(parts) >= 2:
            lis_regs[parts[0]] = i
            parts[-1] = "#"
        elif parts:
            dm = DFORM_RE.match(parts[-1])
            if dm:
                base = dm.group(2)
                if base in lis_regs and i - lis_regs[base] <= LIS_WINDOW:
                    parts[-1] = f"#({base})"
            elif mn in ("addi", "ori", "subi") and len(parts) == 3:
                src = parts[1]
                if src in lis_regs and i - lis_regs[src] <= LIS_WINDOW:
                    parts[-1] = "#"
        # a register written by anything else stops being "a lis result"
        if parts and mn not in ("lis", "addis") and parts[0] in lis_regs \
                and not mn.startswith(("st", "cmp", "b")):
            lis_regs.pop(parts[0], None)
        out.append(mn + " " + ",".join(parts))
    return out


def load_names(toml_path):
    """{addr_int: name} from a re:Blue style [functions] / [entrypoint.functions] file."""
    names = {}
    rx = re.compile(r'^\s*(0x[0-9A-Fa-f]+)\s*=\s*\{\s*name\s*=\s*"([^"]+)"')
    with open(toml_path, encoding="utf-8") as f:
        for line in f:
            m = rx.match(line)
            if m:
                names[int(m.group(1), 16)] = m.group(2)
    return names


def similarity(a, b):
    import difflib
    return difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src_gen")
    ap.add_argument("tgt_gen")
    ap.add_argument("--names", help="functions.toml naming the source functions")
    ap.add_argument("--prefix", default="", help="only source names starting with this")
    ap.add_argument("--min-insns", type=int, default=8)
    ap.add_argument("--fuzzy", type=float, default=0.92)
    ap.add_argument("--out", default="matches.toml")
    ap.add_argument("--report", default="report.tsv")
    args = ap.parse_args()

    src = parse_generated(args.src_gen)
    tgt = parse_generated(args.tgt_gen)
    print(f"source: {len(src)} functions, target: {len(tgt)} functions")

    # Which source functions carry a real name? Generated names are the manifest
    # names, so anything not sub_XXXXXXXX is named. Optionally restrict by prefix.
    named = {n: v for n, v in src.items() if not n.startswith("sub_")}
    if args.names:
        wanted = set(load_names(args.names).values())
        named = {n: v for n, v in named.items() if n in wanted}
    if args.prefix:
        named = {n: v for n, v in named.items() if n.startswith(args.prefix)}
    print(f"named source functions to match: {len(named)}")

    # Index target by exact shape and by a prefix key for fuzzy candidates.
    tgt_shape = {n: normalize(v) for n, v in tgt.items() if len(v) >= args.min_insns}
    by_full = collections.defaultdict(list)
    by_prefix = collections.defaultdict(list)
    for n, sh in tgt_shape.items():
        by_full["\n".join(sh)].append(n)
        by_prefix["\n".join(sh[:10])].append(n)

    exact, fuzzy, ambiguous, missing = [], [], [], []
    for name, insns in sorted(named.items()):
        if len(insns) < args.min_insns:
            missing.append((name, "too-short"))
            continue
        sh = normalize(insns)
        key = "\n".join(sh)
        hits = by_full.get(key, [])
        if len(hits) == 1:
            exact.append((name, hits[0], len(sh), 1.0))
            continue
        if len(hits) > 1:
            ambiguous.append((name, hits, len(sh)))
            continue
        # fuzzy: same 10-insn prefix, then similarity over the whole body
        cands = by_prefix.get("\n".join(sh[:10]), [])
        best = None
        for c in cands:
            csh = tgt_shape[c]
            if abs(len(csh) - len(sh)) > max(4, len(sh) // 5):
                continue
            r = similarity(sh, csh)
            if best is None or r > best[1]:
                best = (c, r)
        if best and best[1] >= args.fuzzy:
            fuzzy.append((name, best[0], len(sh), best[1]))
        else:
            missing.append((name, f"best={best[1]:.2f}" if best else "no-candidate"))

    # A target function claimed by two different names is suspicious; drop both.
    claimed = collections.Counter(t for _, t, _, _ in exact + fuzzy)
    dup = {t for t, c in claimed.items() if c > 1}

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("# Generated by tools/match_functions.py -- XDK/CRT functions in the\n")
        f.write("# target named after their re:Blue (Blue Dragon) counterparts.\n")
        f.write("# exact = identical instruction shape; fuzzy = similarity >= "
                f"{args.fuzzy}\n\n")
        for tag, rows in (("exact", exact), ("fuzzy", fuzzy)):
            f.write(f"# --- {tag} ({len([r for r in rows if r[1] not in dup])}) ---\n")
            for name, t, n, r in sorted(rows, key=lambda x: x[1]):
                if t in dup:
                    continue
                addr = t.split("_")[-1]
                f.write(f'0x{addr} = {{ name = "{name}" }}  # {tag} {n} insns'
                        f'{"" if r == 1.0 else f" sim={r:.3f}"}\n')
    with open(args.report, "w", encoding="utf-8") as f:
        f.write("kind\tname\ttarget\tinsns\tsim\n")
        for name, t, n, r in exact:
            f.write(f"exact\t{name}\t{t}\t{n}\t{r:.3f}\n")
        for name, t, n, r in fuzzy:
            f.write(f"fuzzy\t{name}\t{t}\t{n}\t{r:.3f}\n")
        for name, hits, n in ambiguous:
            f.write(f"ambiguous\t{name}\t{' '.join(hits[:6])}\t{n}\t\n")
        for name, why in missing:
            f.write(f"missing\t{name}\t{why}\t\t\n")

    print(f"exact {len(exact)}  fuzzy {len(fuzzy)}  ambiguous {len(ambiguous)}  "
          f"missing {len(missing)}  (dropped {len(dup)} double-claimed targets)")
    print(f"wrote {args.out} and {args.report}")


if __name__ == "__main__":
    main()
