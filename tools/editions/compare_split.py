# -*- coding: utf-8 -*-
# Compares another edition's function partition with the Spanish one and proposes corrected pairs: every
# Spanish function whose seeded pair does not exist is paired, in order, with the most similar nearby new
# function (whole function, normalized). Usage: compare_split.py <edition> <PAL image> <other image>
import bisect, json, os, sys
import numpy as np
from match_functions import BASE, normalize

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))) + '/'
ed, img_pal, img_other = sys.argv[1:4]
pal = json.load(open(R + 'app/generated/default/codegen.partition.json'))['assignments']
other = json.load(open(R + 'app_%s/generated/default/codegen.partition.json' % ed))['assignments']
map = {}
for l in open(ed + '/all.tsv', encoding='utf-8').read().splitlines()[1:]:
    p, s, o, e = l.split('\t')
    if o != '-':
        map[p] = o
try:
    map.update(json.load(open(ed + '/corrected_pairs.json'))['pairs'])
except (OSError, KeyError):
    pass
seed = {}
source = {}
for a, f in pal.items():
    o = map.get(a)
    if o and o not in seed:
        seed[o] = f
        source[o] = a
new_ones = sorted(a for a in other if a not in seed)
lost = sorted(a for a in seed if a not in other)
ia, ib = open(img_pal, 'rb').read(), open(img_other, 'rb').read()
pi, oi = sorted(int(a, 16) for a in pal), sorted(int(a, 16) for a in other)


def is_long(starts, d):
    i = bisect.bisect_right(starts, d)
    return (starts[i] - d) // 4 if i < len(starts) else 64


def cod(img, d, n):
    return normalize(np.frombuffer(img[d - BASE:d - BASE + 4 * n], dtype='>u4').astype(np.uint32))


used = set()
pairs = {}
for L in lost:
    P = int(source[L], 16)
    n = is_long(pi, P)
    cp = cod(ia, P, n)
    best = None
    for N in new_ones:
        if N in used:
            continue
        d = int(N, 16)
        if abs(d - int(L, 16)) > 0x400:
            continue
        m = is_long(oi, d)
        cn = cod(ib, d, m)
        k = min(n, m)
        equal = int((cp[:k] == cn[:k]).sum()) / max(n, m)
        if not best or equal > best[1] + 1e-9:
            best = (N, equal)
    if best and best[1] >= 0.6:
        pairs[source[L]] = best[0]
        used.add(best[0])
    print('PAL %s (file %d) sembrada %s -> %s' % (source[L], pal[source[L]], L,
          '%s similarity %.2f' % best if best else 'no candidate'))
sin = sorted(set(new_ones) - used)
print('pairs: %d; new ones without a pair: %s; lost without a pair: %s' % (
    len(pairs), sin, sorted(source[L] for L in lost if source[L] not in pairs)))
json.dump({'pairs': pairs, 'new_without_pair': sin}, open(ed + '/pairs_proposed.json', 'w'), indent=1)
