#!/usr/bin/env python3
"""Call graph of the translated game, built from the generated C++ (fh1/generated/default).

Each DEFINE_REX_FUNC(sub_XXXXXXXX) body carries the original PowerPC as comments; direct calls
are "// bl 0x...", tail calls "// b 0x..." to another function. Imports appear as __imp__Name(.

    python tools/callgraph.py callers 829EFB30         # who calls it
    python tools/callgraph.py callees 829EFB30         # what it calls (direct + imports)
    python tools/callgraph.py tree 829EFB30 [depth]    # callers, recursively
    python tools/callgraph.py import VdSwap            # functions calling an import
    python tools/callgraph.py grep "lis r11,-15872"    # functions containing an instruction

The index is cached in fh1/generated/.callgraph.json (rebuilt when the sources are newer).
For mapping the game's Direct3D (ROADMAP Stage 3, phase A). Reads only generated code: nothing
derived from the game is written into the repository.
"""
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "fh1", "generated", "default")
CACHE = os.path.join(ROOT, "fh1", "generated", ".callgraph.json")

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
CALL_RE = re.compile(r"^\s*// bl 0x([0-9a-f]{8})")
TAIL_RE = re.compile(r"^\s*// b 0x([0-9a-f]{8})")
IMPORT_RE = re.compile(r"__imp__([A-Za-z_][A-Za-z0-9_]*)\(ctx")


def build():
    files = sorted(glob.glob(os.path.join(GEN, "*.cpp")))
    newest = max(os.path.getmtime(f) for f in files)
    if os.path.exists(CACHE) and os.path.getmtime(CACHE) > newest:
        with open(CACHE) as f:
            return json.load(f)
    funcs = {}
    for path in files:
        cur = None
        with open(path, encoding="latin-1") as f:
            for line in f:
                m = FUNC_RE.match(line)
                if m:
                    cur = m.group(1)
                    funcs[cur] = {"calls": [], "tails": [], "imports": [], "file": os.path.basename(path)}
                    continue
                if cur is None:
                    continue
                m = CALL_RE.match(line)
                if m:
                    funcs[cur]["calls"].append(m.group(1).upper())
                    continue
                m = TAIL_RE.match(line)
                if m:
                    funcs[cur]["tails"].append(m.group(1).upper())
                    continue
                for m in IMPORT_RE.finditer(line):
                    name = m.group(1)
                    if not name.startswith("sub_"):
                        funcs[cur]["imports"].append(name)
    # A "b" only counts as a call when its target is another function's entry.
    for f in funcs.values():
        f["tails"] = [t for t in f["tails"] if t in funcs]
        f["calls"] = sorted(set(f["calls"]))
        f["imports"] = sorted(set(f["imports"]))
    with open(CACHE, "w") as f:
        json.dump(funcs, f)
    return funcs


def callers_of(funcs, target):
    return sorted(a for a, f in funcs.items() if target in f["calls"] or target in f["tails"])


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    cmd, arg = sys.argv[1], sys.argv[2]
    funcs = build()
    key = arg.upper().replace("SUB_", "").replace("0X", "")
    if cmd == "callers":
        for a in callers_of(funcs, key):
            print(f"sub_{a}  ({funcs[a]['file']})")
    elif cmd == "callees":
        f = funcs[key]
        for a in f["calls"]:
            print(f"bl  sub_{a}")
        for a in f["tails"]:
            print(f"b   sub_{a}")
        for n in f["imports"]:
            print(f"imp {n}")
    elif cmd == "tree":
        depth = int(sys.argv[3]) if len(sys.argv) > 3 else 3
        seen = set()

        def walk(a, d):
            for c in callers_of(funcs, a):
                print("  " * d + f"<- sub_{c}")
                if c not in seen and d + 1 < depth:
                    seen.add(c)
                    walk(c, d + 1)

        print(f"sub_{key}")
        walk(key, 1)
    elif cmd == "import":
        for a, f in sorted(funcs.items()):
            if arg in f["imports"]:
                print(f"sub_{a}  ({f['file']})")
    elif cmd == "grep":
        pat = re.compile(re.escape(arg))
        hits = set()
        for path in sorted(glob.glob(os.path.join(GEN, "*.cpp"))):
            cur = None
            with open(path, encoding="latin-1") as fh:
                for line in fh:
                    m = FUNC_RE.match(line)
                    if m:
                        cur = m.group(1)
                    elif cur and line.lstrip().startswith("//") and pat.search(line):
                        hits.add(cur)
        for a in sorted(hits):
            print(f"sub_{a}")
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
