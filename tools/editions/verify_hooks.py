# -*- coding: utf-8 -*-
# Checks that the functions we hook or reimplement natively are identical in another edition, from start
# to end and without normalizing structure offsets.
#
# match_functions.py only compares 16 instructions and removes every 16-bit immediate, including the one in
# "lwz r3,0x854(r31)", which is a structure field. A native function depends on those fields. Here whole
# functions are compared (up to the next function in the codegen partition) with these rules:
#   - identical words: fine;
#   - branches (b/bl/bc): the translated target must match the other edition's;
#   - lis: same register (the high part of an address);
#   - addi/ori/loads/stores with a different immediate: only if the base register comes from a lis in
#     the same function (the low part of an absolute address);
#   - data words inside the function (jump tables): if they are .text addresses, translated.
# Everything else is reported.
#
# Usage: verify_hooks.py <PAL image> <other image> <table.tsv of all addresses> <PAL partition>
#        <addresses>
import bisect, json, struct, sys
import numpy as np
from match_functions import sections, BASE


def main():
    img_a = open(sys.argv[1], 'rb').read()
    img_b = open(sys.argv[2], 'rb').read()
    map = {}
    for line in open(sys.argv[3], encoding='utf-8').read().splitlines()[1:]:
        pal, sec, other, est = line.split('\t')
        if other != '-':
            map[int(pal, 16)] = int(other, 16)
    starts = sorted(int(a, 16) for a in json.load(open(sys.argv[4]))['assignments'])
    requested = sorted({int(l.split()[0], 16) for l in open(sys.argv[5]) if l.strip()})
    ta = next(s for s in sections(img_a) if s[0] == '.text')
    tb = next(s for s in sections(img_b) if s[0] == '.text')

    # translation of any .text address with the nearest anchor (the one of the function that contains it)
    keys = sorted(k for k in map if ta[1] <= k < ta[2])

    def translate(d):
        k = bisect.bisect_right(keys, d) - 1
        return map[keys[k]] + (d - keys[k]) if k >= 0 else None

    def word(img, d):
        return struct.unpack_from('>I', img, d - BASE)[0]

    def target(w, d):
        op = w >> 26
        if op == 18:
            li = w & 0x03FFFFFC
            li = li - 0x04000000 if li & 0x02000000 else li
            return (li if w & 2 else d + li) & 0xFFFFFFFF
        bd = w & 0xFFFC
        bd = bd - 0x10000 if bd & 0x8000 else bd
        return (bd if w & 2 else d + bd) & 0xFFFFFFFF

    bad = 0
    for d in requested:
        if not (ta[1] <= d < ta[2]) or d not in map:
            continue
        i = bisect.bisect_right(starts, d)
        fin = starts[i] if i < len(starts) else ta[2]
        # the function that contains the address (a hook can be in the middle of a function)
        ini = starts[i - 1] if i else d
        e = map[d] - (d - ini)
        lis = set()
        problems = []
        for k in range(0, fin - ini, 4):
            a, b = word(img_a, ini + k), word(img_b, e + k)
            op = a >> 26
            if a == b:
                if op == 15 and (a >> 16) & 31 == 0:
                    lis.add((a >> 21) & 31)
                continue
            if op != b >> 26:
                # data word: a translated .text address
                if ta[1] <= a < ta[2] and translate(a) == b:
                    continue
                problems.append('+%X: %08X / %08X' % (k, a, b))
                continue
            if op in (16, 18):
                if (a & ~0x03FFFFFC & 0xFFFFFFFF) == (b & ~0x03FFFFFC & 0xFFFFFFFF) if op == 18 else (a & 0xFFFF0003) == (b & 0xFFFF0003):
                    if translate(target(a, ini + k)) == target(b, e + k):
                        continue
                problems.append('+%X: jump %08X / %08X' % (k, a, b))
                continue
            if (a & 0xFFFF0000) == (b & 0xFFFF0000):
                if op == 15 and (a >> 16) & 31 == 0:
                    lis.add((a >> 21) & 31)
                    continue
                base = (a >> 16) & 31
                if op in (14, 24) or 32 <= op <= 55:
                    if base in lis:
                        continue
                problems.append('+%X: immediate %08X / %08X' % (k, a, b))
                continue
            problems.append('+%X: %08X / %08X' % (k, a, b))
        if problems:
            bad += 1
            print('%08X (function %08X, %d bytes) -> %08X: %d differences; %s' % (
                d, ini, fin - ini, map[d], len(problems), '; '.join(problems[:6])))
    print('functions checked: %d, with differences: %d' % (len(requested), bad))


if __name__ == '__main__':
    main()
