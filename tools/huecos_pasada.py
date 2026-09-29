#!/usr/bin/env python3
"""One clean-up pass over a gap file (huecos.toml) after a codegen.

    python tools/huecos_pasada.py fh1 --gen generated/default --huecos huecos.toml [--excluir fh1/huecos_excluir.txt]

1. Data: removes entries the codegen reported as "function 0x... not in any code region"
   (data embedded in .text) and appends them to the exclusion list, if one is given.
2. Thunk runs: a gap declared as one function whose emitted body is just
   "addi r3,r3,N; b target" but whose gap is longer is a run of 8-byte this-adjusting
   thunks (C++ multiple inheritance). Declares every remaining 8-byte slot.

Other partial gaps are left alone: their rest is usually the tail of the previous
function, which tools/fusionar_continuaciones.py merges after the next codegen.
"""
import argparse, glob, json, os, re

ap = argparse.ArgumentParser()
ap.add_argument('app')
ap.add_argument('--gen', default='generated/default')
ap.add_argument('--huecos', default='huecos.toml')
ap.add_argument('--excluir', default='')
args = ap.parse_args()

gen = os.path.join(args.app, args.gen)
path = os.path.join(args.app, args.huecos)
log = open(os.path.join(args.app, 'codegen.log')).read()
lines = open(path).read().split('\n')

starts = sorted(int(k, 16) for k in json.load(open(os.path.join(gen, 'codegen.partition.json')))['assignments'])
lo_mod, hi_mod = starts[0], starts[-1] + 0x100000

# 1. Data
datos = {int(a, 16) for a in re.findall(r'function 0x([0-9A-Fa-f]+) not in any code region', log)}
datos = {a for a in datos if lo_mod <= a < hi_mod}
kept, removed = [], 0
for l in lines:
    m = re.match(r'"0x([0-9A-Fa-f]+)" = \{ \}', l)
    if m and int(m.group(1), 16) in datos:
        removed += 1
        continue
    kept.append(l)
lines = kept
if args.excluir and datos:
    have = set()
    if os.path.exists(args.excluir):
        have = {l.strip().upper() for l in open(args.excluir) if l.strip() and not l.startswith('#')}
    with open(args.excluir, 'a') as o:
        for a in sorted(datos):
            if ('0X%08X' % a) not in have:
                o.write('0x%08X\n' % a)

# 2. Thunk runs
declared = {}
for l in lines:
    m = re.match(r'"0x([0-9A-Fa-f]+)" = \{ \}\s+#\s+(\d+) bytes', l)
    if m:
        declared[int(m.group(1), 16)] = int(m.group(2))
already = {int(m.group(1), 16) for l in lines for m in [re.match(r'"0x([0-9A-Fa-f]+)"', l)] if m}
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
            new += [a + o for o in range(8, gsize, 8) if a + o not in already]
if new:
    lines += ['', '# Thunk runs split by tools/huecos_pasada.py'] + ['"0x%08X" = { }   # thunk' % a for a in sorted(new)]
while lines and lines[-1] == '':
    lines.pop()
open(path, 'w').write('\n'.join(lines) + '\n')
print('data entries removed: %d, thunk slots added: %d' % (removed, len(new)))
