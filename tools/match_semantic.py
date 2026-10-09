#!/usr/bin/env python3
"""Second matching pass, for code that LTCG restructured between the two
binaries so instruction *shape* no longer lines up (see match_functions.py).

A function is fingerprinted by what it *does* rather than how it is laid out:
  * imports  : multiset of kernel imports it calls (__imp__VdSwap, ...)
  * consts   : multiset of "semantic" immediates it materializes -- li/ori/
               cmpwi/cmplwi values >= 0x100, which for the XDK D3D layer are
               Xenos GPU register indices (0x2000.., 0x4000..) and PM4 packet
               header halves. Those are hardware facts, identical across XDK
               versions.
  * callees  : multiset of callees already resolved to a name
  * callers  : same, for callers
  * size     : instruction count (loose)
Seeds come from the shape pass (matches.toml) plus explicit --seed pairs, then
names propagate over the call graph until nothing changes.

Usage:
  match_semantic.py SRC_GEN_DIR TGT_GEN_DIR --names functions.toml
      [--seeds matches.toml] [--seed Name=sub_XXXXXXXX ...]
      [--prefix D3D] [--out semantic.toml] [--report semantic.tsv]
"""
import argparse
import collections
import glob
import os
import re
import sys

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
ASM_RE = re.compile(r"^\t// (\S+)\s*(.*)$")
CALL_RE = re.compile(r"^\t(\w+)\(ctx, base\);")
IMM_INSNS = ("li", "ori", "oris", "cmpwi", "cmplwi", "andi.", "xori")
# Immediates below this are ignored as noise (loop counters, small offsets).
# The XDK writes GPU registers through PM4 SET_CONSTANT packets whose first
# dword is the register index minus 0x2000, so RB_COLOR_INFO shows up as 1 and
# PA_SC_WINDOW_SCISSOR_TL as 0x81: keep this low for the D3D layer.
MIN_CONST = 0x100
TGT_RANGES = []  # [(lo, hi)] address filter for target candidates


def in_ranges(name):
    if not TGT_RANGES:
        return True
    try:
        a = int(name.split("_")[-1], 16)
    except ValueError:
        return False
    return any(lo <= a < hi for lo, hi in TGT_RANGES)


def parse(gen_dir):
    funcs = {}
    for path in sorted(glob.glob(os.path.join(gen_dir, "*_recomp.*.cpp"))):
        cur = None
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = FUNC_RE.match(line)
                if m:
                    cur = m.group(1)
                    funcs[cur] = {"insns": [], "calls": []}
                    continue
                if cur is None:
                    continue
                m = ASM_RE.match(line)
                if m:
                    funcs[cur]["insns"].append((m.group(1), m.group(2).strip()))
                    continue
                m = CALL_RE.match(line)
                if m and m.group(1) not in ("REX_FUNC_PROLOGUE",):
                    funcs[cur]["calls"].append(m.group(1))
    if not funcs:
        sys.exit(f"no functions in {gen_dir}")
    return funcs


def consts_of(insns):
    out = collections.Counter()
    for mn, ops in insns:
        if mn not in IMM_INSNS:
            continue
        parts = [p.strip() for p in ops.split(",")]
        if not parts:
            continue
        try:
            v = int(parts[-1], 0)
        except ValueError:
            continue
        if v < 0:
            v &= 0xFFFF
        if v >= MIN_CONST:
            out[(mn if mn in ("ori", "oris") else "imm", v)] += 1
    return out


def imports_of(calls):
    return collections.Counter(c for c in calls if c.startswith("__imp__"))


def jaccard(a, b):
    if not a and not b:
        return 1.0
    inter = sum((a & b).values())
    union = sum((a | b).values())
    return inter / union if union else 0.0


def load_names(path):
    rx = re.compile(r'^\s*(0x[0-9A-Fa-f]+)\s*=\s*\{\s*name\s*=\s*"([^"]+)"')
    names = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = rx.match(line)
            if m:
                names[m.group(2)] = int(m.group(1), 16)
    return names


def load_seeds(path):
    """matches.toml lines: 0xADDR = { name = "X" }  ->  {X: sub_ADDR}"""
    rx = re.compile(r'^\s*0x([0-9A-Fa-f]+)\s*=\s*\{\s*name\s*=\s*"([^"]+)"')
    seeds = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = rx.match(line)
            if m:
                seeds[m.group(2)] = "sub_" + m.group(1).upper()
    return seeds


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src_gen")
    ap.add_argument("tgt_gen")
    ap.add_argument("--names", required=True)
    ap.add_argument("--seeds", action="append", default=[])
    ap.add_argument("--seed", action="append", default=[], help="Name=sub_X")
    ap.add_argument("--prefix", default="")
    ap.add_argument("--min-score", type=float, default=0.55)
    ap.add_argument("--margin", type=float, default=0.12,
                    help="best must beat runner-up by this much")
    ap.add_argument("--out", default="semantic.toml")
    ap.add_argument("--report", default="semantic.tsv")
    ap.add_argument("--min-const", type=lambda s: int(s, 0), default=0x100)
    ap.add_argument("--tgt-range", action="append", default=[],
                    help="lo-hi hex address range to restrict target candidates")
    args = ap.parse_args()
    global MIN_CONST, TGT_RANGES
    MIN_CONST = args.min_const
    TGT_RANGES = [tuple(int(x, 16) for x in r.split("-")) for r in args.tgt_range]

    src = parse(args.src_gen)
    tgt = parse(args.tgt_gen)
    wanted = load_names(args.names)
    named = [n for n in src if not n.startswith("sub_") and n in wanted
             and (not args.prefix or n.startswith(args.prefix))]
    print(f"src {len(src)} fns, tgt {len(tgt)} fns, named to resolve {len(named)}")

    # name -> tgt function (the growing answer)
    mapping = {}
    how = {}
    for p in args.seeds:
        for k, v in load_seeds(p).items():
            if v in tgt:
                mapping[k] = v
                how[k] = "shape"
    for s in args.seed:
        k, v = s.split("=", 1)
        if v in tgt:
            mapping[k] = v
            how[k] = "manual"
    print(f"seeds: {len(mapping)}")

    # Precompute features.
    def feats(fmap):
        F = {}
        for n, d in fmap.items():
            F[n] = {
                "consts": consts_of(d["insns"]),
                "imports": imports_of(d["calls"]),
                "calls": [c for c in d["calls"] if not c.startswith("__imp__")],
                "n": len(d["insns"]),
            }
        return F
    FS, FT = feats(src), feats(tgt)
    callers_t = collections.defaultdict(list)
    for n, d in tgt.items():
        for c in d["calls"]:
            callers_t[c].append(n)
    callers_s = collections.defaultdict(list)
    for n, d in src.items():
        for c in d["calls"]:
            callers_s[c].append(n)

    # Pass 0: unique kernel-import signatures.
    imp_index = collections.defaultdict(list)
    for n, f in FT.items():
        if f["imports"]:
            imp_index[tuple(sorted(f["imports"].items()))].append(n)
    for n in named:
        if n in mapping:
            continue
        key = tuple(sorted(FS[n]["imports"].items()))
        if key and len(imp_index.get(key, [])) == 1:
            mapping[n] = imp_index[key][0]
            how[n] = "imports"
    print(f"after import signatures: {len(mapping)}")

    # Iterative propagation.
    taken = set(mapping.values())
    inv = {v: k for k, v in mapping.items()}

    def score(n, g):
        fs, ft = FS[n], FT[g]
        s_c = jaccard(fs["consts"], ft["consts"])
        s_i = 1.0 if fs["imports"] == ft["imports"] else (0.0 if fs["imports"] or ft["imports"] else 0.5)
        # callees resolved through the mapping
        sc = collections.Counter(mapping.get(c, None) for c in fs["calls"] if c in mapping)
        tc = collections.Counter(c for c in ft["calls"] if c in inv)
        s_callee = jaccard(sc, tc) if (sc or tc) else 0.5
        sr = collections.Counter(mapping.get(c) for c in callers_s.get(n, []) if c in mapping)
        tr = collections.Counter(c for c in callers_t.get(g, []) if c in inv)
        s_caller = jaccard(sr, tr) if (sr or tr) else 0.5
        ratio = min(fs["n"], ft["n"]) / max(fs["n"], ft["n"], 1)
        s_n = 1.0 if ratio >= 0.5 else ratio * 2
        evidence = bool(fs["consts"]) + bool(fs["imports"]) + bool(sc) + bool(sr)
        return (0.45 * s_c + 0.15 * s_i + 0.2 * s_callee + 0.1 * s_caller + 0.1 * s_n), evidence

    changed = True
    rounds = 0
    while changed and rounds < 12:
        changed = False
        rounds += 1
        for n in named:
            if n in mapping:
                continue
            fs = FS[n]
            # candidate pool: share a resolved callee/caller, or share >=2 consts
            pool = set()
            for c in fs["calls"]:
                if c in mapping:
                    pool.update(callers_t.get(mapping[c], []))
            for c in callers_s.get(n, []):
                if c in mapping:
                    pool.update(tgt[mapping[c]]["calls"])
            if not pool or len(fs["consts"]) >= 3:
                keys = set(fs["consts"])
                if keys:
                    for g, ft in FT.items():
                        if len(keys & set(ft["consts"])) >= max(2, len(keys) // 2):
                            pool.add(g)
            pool = [g for g in pool if g in FT and g not in taken and in_ranges(g)]
            if not pool:
                continue
            scored = sorted(((score(n, g), g) for g in pool), reverse=True)
            (best, ev), g = scored[0]
            second = scored[1][0][0] if len(scored) > 1 else 0.0
            if best >= args.min_score and best - second >= args.margin and ev >= 2:
                mapping[n] = g
                inv[g] = n
                taken.add(g)
                how[n] = f"sem r{rounds} {best:.2f}/{second:.2f}"
                changed = True
        print(f"round {rounds}: {len(mapping)} resolved")

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("# Generated by tools/match_semantic.py\n")
        for n in sorted(mapping, key=lambda k: mapping[k]):
            addr = mapping[n].split("_")[-1]
            f.write(f'0x{addr} = {{ name = "{n}" }}  # {how[n]}\n')
    with open(args.report, "w", encoding="utf-8") as f:
        f.write("name\ttarget\thow\tsrc_insns\ttgt_insns\n")
        for n in sorted(named):
            if n in mapping:
                f.write(f"{n}\t{mapping[n]}\t{how[n]}\t{FS[n]['n']}\t{FT[mapping[n]]['n']}\n")
            else:
                f.write(f"{n}\t-\tunresolved\t{FS[n]['n']}\t\n")
    resolved = sum(1 for n in named if n in mapping)
    print(f"resolved {resolved}/{len(named)}; wrote {args.out}, {args.report}")


if __name__ == "__main__":
    main()
