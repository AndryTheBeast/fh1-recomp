#!/usr/bin/env python3
"""One clean-up pass over a gap file (gaps.toml) after a codegen.

    python tools/gaps_pass.py fh1 --gen generated/default --gaps gaps.toml [--exclude fh1/gaps_exclude.txt]

1. Data: removes entries the codegen reported as "function 0x... not in any code region"
   (data embedded in .text) or "outranked by import" (the import thunk area) and appends them to the exclusion list, if one is given.
2. Thunk runs: a gap declared as one function whose emitted body is just
   "addi r3,r3,N; b target" but whose gap is longer is a run of 8-byte this-adjusting
   thunks (C++ multiple inheritance). Declares every remaining 8-byte slot.

3. With --rests, any other gap whose declared function ends before the gap does gets its
   rest declared as a function too (several small functions packed in one gap, e.g.
   16-byte vtable-dispatch stubs). A rest that is really the tail of the previous
   function shows up as unresolved branches in the next codegen, and
   tools/merge_continuations.py merges it back. Repeat until nothing is added.
"""
import argparse, glob, json, os, re

ap = argparse.ArgumentParser()
ap.add_argument('app')
ap.add_argument('--gen', default='generated/default')
ap.add_argument('--gaps', default='gaps.toml')
ap.add_argument('--exclude', default='')
ap.add_argument('--rests', action='store_true',
                help='also declare the rest of every gap whose declared function ends early')
args = ap.parse_args()

gen = os.path.join(args.app, args.gen)
path = os.path.join(args.app, args.gaps)
log = open(os.path.join(args.app, 'codegen.log')).read()
lines = open(path).read().split('\n')

starts = sorted(int(k, 16) for k in json.load(open(os.path.join(gen, 'codegen.partition.json')))['assignments'])
lo_mod, hi_mod = starts[0], starts[-1] + 0x100000

# 1. Data
data = {int(a, 16) for a in re.findall(r'function 0x([0-9A-Fa-f]+) not in any code region', log)}
# The import thunk area: registered but never emitted, so the link fails with "undefined symbol".
data |= {int(a, 16) for a in re.findall(r'\[functions\] 0x([0-9A-Fa-f]+) outranked by import', log)}
data = {a for a in data if lo_mod <= a < hi_mod}
kept, removed = [], 0
for l in lines:
    m = re.match(r'"0x([0-9A-Fa-f]+)" = \{ \}', l)
    if m and int(m.group(1), 16) in data:
        removed += 1
        continue
    kept.append(l)
lines = kept
if args.exclude and data:
    have = set()
    if os.path.exists(args.exclude):
        have = {l.strip().upper() for l in open(args.exclude) if l.strip() and not l.startswith('#')}
    with open(args.exclude, 'a') as o:
        for a in sorted(data):
            if ('0X%08X' % a) not in have:
                o.write('0x%08X\n' % a)

# 2. Thunk runs
declared = {}
for l in lines:
    m = re.match(r'"0x([0-9A-Fa-f]+)" = \{ \}\s+#\s+(\d+) bytes', l)
    if m:
        declared[int(m.group(1), 16)] = int(m.group(2))
already = {int(m.group(1), 16) for l in lines for m in [re.match(r'"0x([0-9A-Fa-f]+)"', l)] if m}
# Ranges already claimed by an extended function ({ end = ... }): never declare inside them.
ranges = [(int(m.group(1), 16), int(m.group(2), 16)) for l in lines
          for m in [re.match(r'"0x([0-9A-Fa-f]+)" = \{ end = 0x([0-9A-Fa-f]+) \}', l)] if m]
def in_range(a):
    return any(lo < a < end for lo, end in ranges)
new = []
for f in glob.glob(os.path.join(gen, '*.cpp')):
    src = open(f).read()
    for m in re.finditer(r'DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{(.*?)\n\}', src, re.S):
        a = int(m.group(1), 16)
        if a not in declared:
            continue
        asm = [x.strip()[3:] for x in m.group(2).split('\n') if x.strip().startswith('// ')]
        gsize = declared[a]
        if len(asm) == 2 and asm[0].startswith('addi r3,r3,') and asm[1].startswith('b ') \
                and gsize > 8 and gsize % 8 == 0:
            new += [a + o for o in range(8, gsize, 8) if a + o not in already and not in_range(a + o)]
# 3. Rests of gaps
rests = []
if args.rests:
    all_starts = starts
    import bisect
    emitted = {}
    for f in glob.glob(os.path.join(gen, '*.cpp')):
        src = open(f).read()
        for m in re.finditer(r'DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{(.*?)\n\}', src, re.S):
            a = int(m.group(1), 16)
            n = sum(1 for x in m.group(2).split('\n') if x.strip().startswith('// '))
            emitted[a] = a + 4 * n
    gap_entries = {int(m.group(1), 16) for l in lines
                   for m in [re.match(r'"0x([0-9A-Fa-f]+)" = \{ \}', l)] if m}
    excl = set()
    if args.exclude and os.path.exists(args.exclude):
        excl = {int(l.strip(), 16) for l in open(args.exclude) if l.strip() and not l.startswith('#')}
    for a in sorted(gap_entries):
        if a not in emitted:
            continue
        i = bisect.bisect_right(all_starts, a)
        nxt = all_starts[i] if i < len(all_starts) else None
        end = emitted[a]
        if nxt and end < nxt and end not in already and end not in excl and a + 4 <= end and not in_range(end):
            rests.append(end)
    rests = sorted(set(rests) - set(new))
if new:
    lines += ['', '# Thunk runs split by tools/gaps_pass.py'] + ['"0x%08X" = { }   # thunk' % a for a in sorted(new)]
if rests:
    lines += ['', '# Rests of gaps (tools/gaps_pass.py --rests)'] + ['"0x%08X" = { }   # rest' % a for a in rests]
while lines and lines[-1] == '':
    lines.pop()
open(path, 'w').write('\n'.join(lines) + '\n')
print('data entries removed: %d, thunk slots added: %d, gap rests added: %d' % (removed, len(new), len(rests)))
