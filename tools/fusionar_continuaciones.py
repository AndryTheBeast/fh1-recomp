#!/usr/bin/env python3
"""Merges gap functions that are really continuations of their neighbour.

After a codegen with huecos.toml, reads codegen.log for
    Unresolved conditional branch to T from S
    Unresolved b target T from S
    Jump target T unresolved at bctr S
When S and T fall in two different functions and every function in the span
between them (other than the first) was declared by huecos.toml, the gap is
the tail of the first function, cut off too early (kind 2 in
reference/nfsmw-app/huecos.toml). This removes those gap entries and writes
"0xFIRST" = { end = 0xNEXT } into huecos.toml's continuation block, so the
first function covers the whole span.

    python tools/fusionar_continuaciones.py fh1
    python tools/fusionar_continuaciones.py fh1 --gen generated/xmediafacade_default --huecos xmediafacade_huecos.toml
Only log lines whose addresses fall inside that module's code are used.
Anything it cannot merge safely is listed for a manual look.
"""
import bisect, json, re, sys, os

import argparse
ap = argparse.ArgumentParser()
ap.add_argument('app')
ap.add_argument('--gen', default='generated/default')
ap.add_argument('--huecos', default='huecos.toml')
args = ap.parse_args()
app = args.app
log = open(os.path.join(app, 'codegen.log')).read()
starts = sorted(int(k, 16) for k in json.load(open(os.path.join(app, args.gen, 'codegen.partition.json')))['assignments'])
huecos_path = os.path.join(app, args.huecos)
lines = open(huecos_path).read().split('\n')
declared = {}
for i, l in enumerate(lines):
    m = re.match(r'"0x([0-9A-Fa-f]+)" = \{ \}', l)
    if m:
        declared[int(m.group(1), 16)] = i

pairs = set()
for m in re.finditer(r'Unresolved conditional branch to 0x([0-9A-F]+) from 0x([0-9A-F]+)', log):
    pairs.add((int(m.group(2), 16), int(m.group(1), 16)))
for m in re.finditer(r'Unresolved b target 0x([0-9A-F]+) from 0x([0-9A-F]+)', log):
    pairs.add((int(m.group(2), 16), int(m.group(1), 16)))
for m in re.finditer(r'Jump target 0x([0-9A-F]+) unresolved at bctr 0x([0-9A-F]+)', log):
    pairs.add((int(m.group(2), 16), int(m.group(1), 16)))

lo_mod, hi_mod = starts[0], starts[-1] + 0x100000
pairs = {(s_, t_) for s_, t_ in pairs if lo_mod <= s_ < hi_mod}

def owner(a):
    return starts[bisect.bisect_right(starts, a) - 1]

merges = {}   # first function -> end
manual = []
for s, t in sorted(pairs):
    fs, ft = owner(s), owner(t)
    lo, hi = min(fs, ft), max(fs, ft)
    i, j = starts.index(lo), starts.index(hi)
    between = starts[i + 1:j + 1]
    if lo == hi:
        # Same function, target past its analysed end: it was cut short.
        # Extend it to the start of the next function.
        k = starts.index(lo)
        merges[lo] = max(merges.get(lo, 0), starts[k + 1])
        continue
    if not all(b in declared for b in between):
        manual.append((s, t, fs, ft))
        continue
    end = starts[j + 1]
    merges[lo] = max(merges.get(lo, 0), end)

# Nested/overlapping merges: fold a merge that starts inside another one.
for lo in sorted(merges):
    for lo2 in sorted(merges):
        if lo < lo2 < merges[lo]:
            merges[lo] = max(merges[lo], merges.pop(lo2, merges[lo]))

drop = set()
for lo, end in merges.items():
    for b in starts:
        if lo < b < end and b in declared:
            drop.add(declared[b])
    if lo in declared:
        drop.add(declared[lo])

out = [l for i, l in enumerate(lines) if i not in drop]
while out and out[-1] == '':
    out.pop()
marker = '# Continuations (tools/fusionar_continuaciones.py)'
existing = {}
if marker in out:
    k = out.index(marker)
    rest = []
    for l in out[k + 1:]:
        m = re.match(r'"0x([0-9A-F]+)" = \{ end = 0x([0-9A-F]+) \}', l)
        if m:
            existing[int(m.group(1), 16)] = int(m.group(2), 16)
        else:
            rest.append(l)  # anything else written after the block (e.g. thunk runs) stays
    out = out[:k] + rest
    while out and out[-1] == '':
        out.pop()
for lo, end in merges.items():
    existing[lo] = max(existing.get(lo, 0), end)
out += [marker]
out += ['"0x%08X" = { end = 0x%08X }' % (lo, end) for lo, end in sorted(existing.items())]
# Final pass: an entry that starts inside a merged range belongs to it now.
ranges = sorted(existing.items())
def inside(a):
    return any(lo < a < end for lo, end in ranges)
cleaned = []
for l in out:
    m = re.match(r'"0x([0-9A-Fa-f]+)" = \{', l)
    if m and inside(int(m.group(1), 16)):
        continue
    cleaned.append(l)
out = cleaned
open(huecos_path, 'w').write('\n'.join(out) + '\n')
print('merged %d spans, removed %d gap entries' % (len(merges), len(drop)))
for s, t, fs, ft in manual:
    print('  manual: 0x%X -> 0x%X (functions 0x%X, 0x%X)' % (s, t, fs, ft))
