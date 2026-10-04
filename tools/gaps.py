#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Finds code gaps with no function assigned in the codegen output.

The problem
-----------
The binary dies with:
    [FATAL] Call to invalid or unregistered function at guest address 0xXXXXXXXX

That happens when something makes an indirect call (through a pointer or vtable) to an
address the analysis did not mark as a function start. The Validate phase does not
detect them: it only checks direct branches (b / bl). Indirect ones only show up at
run time, one at a time.

How it finds them
-----------------
Every PowerPC instruction takes 4 bytes and the codegen emits exactly one comment line
"\\t// <asm>" per instruction. So:

    end_of_function = start + 4 * number_of_comments

With the start of each function (codegen.partition.json) and its computed end, every
stretch between the end of one and the start of the next is a gap with no owner.
Gaps that contain real code are the candidates for breaking startup.

Usage:
    python tools\\gaps.py
    python tools\\gaps.py --min 8
    python tools\\gaps.py --should_check 0x8215FEA8 0x826BE258
"""


# ============================================================================
#  Known limitation - a gap is not always one function
#
#  This tool emits one declaration per gap, at its start address. That
#  assumes each gap contains exactly one function, and it is false.
#
#  A gap is simply space that automatic discovery did not claim. It can
#  contain several small functions in a row, typically tables of 8- and
#  16-byte thunks. By declaring only the start, the Discover phase swallows
#  the whole gap as one single function and the other entry points become
#  unreachable. The symptom is:
#
#     [FATAL] Call to invalid or unregistered function at guest address 0x...
#
#  with an address that falls inside a gap that is already declared.
#
#  Verified: four different crashes fell inside declared gaps, at +8, +24,
#  +64 and +96 bytes from their start. That is why the crash seemed to
#  "move": each fix uncovered the next entry of the same gap.
#
#  Splitting every gap every 8 bytes is not the solution: that would be more
#  than 3000 declarations, and most gaps do contain a single function, so
#  entry points would be declared in the middle of functions.
#
#  What does work:
#    1. Split only the gaps proven to have several entries (where a crash
#       already happened inside). See the block at the end of app/gaps.toml.
#    2. A probing run of the release build, which finds them empirically in
#       one go.
# ============================================================================

import argparse
import bisect
import json
import os
import re
import sys

RE_FUNC = re.compile(r'^DEFINE_REX_FUNC\((?:sub_)?([0-9A-Fa-f]{8})\)')


def measure_functions(gen_dir):
    """Returns {start: instruction_count} by walking the generated C++."""
    sizes = {}
    files = sorted(f for f in os.listdir(gen_dir) if f.endswith('.cpp'))
    for idx, name in enumerate(files, 1):
        path = os.path.join(gen_dir, name)
        actual = None
        n = 0
        with open(path, 'r', encoding='utf-8', errors='replace') as fh:
            for line in fh:
                if line.startswith('DEFINE_REX_FUNC('):
                    m = RE_FUNC.match(line)
                    if m:
                        actual = int(m.group(1), 16)
                        n = 0
                elif actual is not None:
                    if line.startswith('\t// '):
                        n += 1
                    elif line.startswith('}'):
                        sizes[actual] = n
                        actual = None
        sys.stdout.write("\r  read %d/%d archivos" % (idx, len(files)))
        sys.stdout.flush()
    print()
    return sizes


def main():
    p = argparse.ArgumentParser(description="Finds code gaps with no function.")
    p.add_argument("--gen", default="app/generated/default",
                   help="folder with the generated code")
    p.add_argument("--min", type=int, default=4,
                   help="minimum gap size to list (default 4)")
    p.add_argument("--should_check", nargs="*", default=[],
                   help="addresses concretas a localizar, p.ej. 0x8215FEA8")
    p.add_argument("--output", default="docs/gaps.txt")
    p.add_argument("--toml", default="tools/gaps_functions.toml")
    args = p.parse_args()

    part = os.path.join(args.gen, "codegen.partition.json")
    if not os.path.exists(part):
        raise SystemExit("%s does not exist. Run the codegen first." % part)

    print("Reading the function partition...")
    assignments = json.load(open(part))["assignments"]
    starts = sorted(int(k, 16) for k in assignments)
    print("  %d functions" % len(starts))

    print("Measuring every function in the generated C++...")
    sizes = measure_functions(args.gen)
    print("  %d measurements" % len(sizes))

    missing = [a for a in starts if a not in sizes]
    if missing:
        print("  warning: %d functions not measured (ignored)" % len(missing))

    # Compute gaps
    gaps = []
    for i, a in enumerate(starts[:-1]):
        n = sizes.get(a)
        if n is None:
            continue
        fin = a + 4 * n
        next = starts[i + 1]
        if fin < next:
            gaps.append((fin, next - fin, a, next))

    large = [h for h in gaps if h[1] >= args.min]

    # Distribution by size
    dist = {}
    for _, tam, _, _ in gaps:
        dist[tam] = dist.get(tam, 0) + 1

    lines = []
    def w(s=""):
        print(s)
        lines.append(s)

    w()
    w("=" * 60)
    w("  CODE GAPS WITH NO FUNCTION ASSIGNED")
    w("=" * 60)
    w("Functions            : %d" % len(starts))
    w("Gaps totals       : %d" % len(gaps))
    w("Gaps >= %d bytes    : %d" % (args.min, len(large)))
    w("Bytes in gaps      : %d" % sum(h[1] for h in gaps))
    w()
    w("Distribution by gap size:")
    for tam in sorted(dist):
        w("   %5d bytes  x %d" % (tam, dist[tam]))
    w()

    if args.should_check:
        w("-" * 60)
        w("Addresses consultadas:")
        ends = sorted(h[0] for h in gaps)
        for s in args.should_check:
            t = int(s, 16)
            is_start = bisect.bisect_left(starts, t) < len(starts) and \
                        starts[bisect.bisect_left(starts, t)] == t
            inside = None
            for fin, tam, ini, sig in gaps:
                if fin <= t < fin + tam:
                    inside = (fin, tam, ini, sig)
                    break
            w("  0x%08X  function start: %s" % (t, "YES" if is_start else "NO"))
            if inside:
                w("      falls in a gap of %d bytes: 0x%08X - 0x%08X"
                  % (inside[1], inside[0], inside[0] + inside[1]))
                w("      (after function 0x%08X, before 0x%08X)"
                  % (inside[2], inside[3]))
            elif not is_start:
                w("      does not fall in any known gap")
        w()

    w("-" * 60)
    w("First 60 gaps of >= %d bytes:" % args.min)
    for fin, tam, ini, sig in large[:60]:
        w("  0x%08X  %5d bytes   (after 0x%08X, before 0x%08X)"
          % (fin, tam, ini, sig))
    if len(large) > 60:
        w("  ... y %d mas" % (len(large) - 60))

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
        fh.write("\n" + "-" * 60 + "\nALL the gaps >= %d bytes:\n" % args.min)
        for fin, tam, ini, sig in large:
            fh.write("  0x%08X  %5d bytes   (after 0x%08X, before 0x%08X)\n"
                     % (fin, tam, ini, sig))
    print()
    print("Full report in %s" % args.output)

    os.makedirs(os.path.dirname(args.toml) or ".", exist_ok=True)
    with open(args.toml, "w", encoding="utf-8") as fh:
        fh.write("# Generated by tools/gaps.py - do NOT include as is.\n")
        fh.write("# Declaring a function in a gap that is padding or data can\n")
        fh.write("# break the codegen. Copy only the entries that are needed.\n")
        fh.write("[functions]\n")
        for fin, tam, ini, sig in large:
            fh.write('"0x%08X" = { }   # %d bytes, after 0x%08X\n' % (fin, tam, ini))
    print("Reference TOML block in %s" % args.toml)


if __name__ == "__main__":
    main()
