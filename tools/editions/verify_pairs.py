# -*- coding: utf-8 -*-
# Strict check of the hooked functions in another edition: every "lis rX,HIGH" + instruction pair with
# base rX (addi, ori, loads and stores) must compute, in the other edition, the exact translation of the
# Spanish address. verify_hooks.py accepts any value in those pairs (they are addresses); here it is
# checked that they are the right address:
#   .text  -> the translation by anchors;
#   .rdata -> the same 16 bytes in both editions;
#   .data  -> the translation by references (the edition's offsets_data.json) or the same
#             address if the edition does not have it (.data unmoved).
# Usage: verify_pairs.py <PAL image> <other image> <table.tsv> <PAL partition> <addresses>
#        [offsets.json]
import bisect, json, struct, sys
import numpy as np
from match_functions import BASE, anchors, fingerprints, normalize, sections


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
    data = {int(k, 16): v for k, v in json.load(open(sys.argv[6])).items()} if len(sys.argv) > 6 else {}
    keys_data = sorted(data)
    sa, sb = sections(img_a), sections(img_b)
    ta = next(s for s in sa if s[0] == '.text')
    tb = next(s for s in sb if s[0] == '.text')
    wa = np.frombuffer(img_a[ta[1] - BASE:ta[2] - BASE], dtype='>u4').astype(np.uint32)
    wb = np.frombuffer(img_b[tb[1] - BASE:tb[2] - BASE], dtype='>u4').astype(np.uint32)
    chain = anchors(fingerprints(normalize(wa)), fingerprints(normalize(wb)))
    pos = [a for a, _ in chain]

    def section(secs, d):
        return next((s[0] for s in secs if s[1] <= d < s[2]), 'outside')

    def translate(d):
        s = section(sa, d)
        if s == '.text':
            i = (d - ta[1]) // 4
            k = bisect.bisect_right(pos, i) - 1
            return tb[1] + 4 * (i + chain[k][1] - chain[k][0]) + (d & 3) if k >= 0 else None
        if s == '.data':
            if not data:
                return d
            if d in data:
                return d + data[d]
            k = bisect.bisect_right(keys_data, d)
            if 0 < k < len(keys_data) and data[keys_data[k - 1]] == data[keys_data[k]]:
                return d + data[keys_data[k - 1]]
            return None
        return None  # .rdata and the rest: by contents

    def word(img, d):
        return struct.unpack_from('>I', img, d - BASE)[0]

    def low(x):
        op = x >> 26
        if op == 24:
            return x & 0xFFFF
        if op in (58, 62):
            return struct.unpack('>h', struct.pack('>H', x & 0xFFFC))[0]
        return struct.unpack('>h', struct.pack('>H', x & 0xFFFF))[0]

    bad = pairs = 0
    for d in requested:
        if not (ta[1] <= d < ta[2]) or d not in map:
            continue
        i = bisect.bisect_right(starts, d)
        ini = starts[i - 1] if i else d
        fin = starts[i] if i < len(starts) else ta[2]
        e = map[d] - (d - ini)
        high_a, high_b = {}, {}
        problems = []
        for k in range(0, fin - ini, 4):
            a, b = word(img_a, ini + k), word(img_b, e + k)
            op = a >> 26
            if op == 15 and (a >> 16) & 31 == 0:
                high_a[(a >> 21) & 31] = (a & 0xFFFF) << 16
                if b >> 26 == 15 and (b >> 16) & 31 == 0:
                    high_b[(b >> 21) & 31] = (b & 0xFFFF) << 16
                continue
            if op in (14, 24, 58, 62) or 32 <= op <= 55:
                base = (a >> 21) & 31 if op == 24 else (a >> 16) & 31
                if base in high_a and base in high_b and base != 0:
                    da = (high_a[base] + low(a)) & 0xFFFFFFFF
                    db = (high_b[base] + low(b)) & 0xFFFFFFFF
                    pairs += 1
                    s = section(sa, da)
                    if s in ('.text', '.data'):
                        expected = translate(da)
                        if expected is None:
                            problems.append('+%X: %08X (%s) no safe translation; the other one uses %08X' % (k, da, s, db))
                        elif expected != db:
                            problems.append('+%X: %08X (%s) should be %08X and the other one uses %08X' % (k, da, s, expected, db))
                    elif s != 'outside':
                        # constants: the same contents
                        if img_a[da - BASE:da - BASE + 16] != img_b[db - BASE:db - BASE + 16]:
                            problems.append('+%X: %08X (%s) with other content at %08X' % (k, da, s, db))
            # any write to the base register ends the pair (approximate: destination in bits 21-25)
            if op not in (36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55, 62) and op != 24:
                rd = (a >> 21) & 31
                high_a.pop(rd, None)
                high_b.pop((b >> 21) & 31, None)
            elif op == 24:
                high_a.pop((a >> 16) & 31, None)
                high_b.pop((b >> 16) & 31, None)
        if problems:
            bad += 1
            print('%08X (function %08X -> %08X): %s' % (d, ini, e, '; '.join(problems[:5])))
    print('pairs checked: %d; functions with problems: %d of %d' % (pairs, bad, len(requested)))


if __name__ == '__main__':
    main()
