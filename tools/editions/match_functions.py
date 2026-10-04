# -*- coding: utf-8 -*-
# Translates addresses of the Spanish PAL executable to those of another edition (first, the USA one).
#
# .text: PowerPC instructions are normalized by removing what depends on position (branch offsets, 16-bit
# immediates of lis/addi/ori and of loads and stores), runs of 12 instructions that appear only once in
# each edition (anchors) are searched for, those in the same order are kept, and each address is
# translated with the offset of its previous anchor. Each translation is verified by comparing 16
# normalized instructions.
# .rdata: the contents (32 bytes) are searched for in the other edition; they must appear exactly once.
# .data: same address if the surrounding bytes match (the section does not move between PAL and USA).
#
# Usage: match_functions.py <PAL image> <other image> <output.tsv> [addresses...]
import bisect, struct, sys
import numpy as np

BASE = 0x82000000
K = 12
VERIFY = 16


def sections(img):
    pe = struct.unpack_from('<I', img, 0x3C)[0]
    n = struct.unpack_from('<H', img, pe + 6)[0]
    opt = struct.unpack_from('<H', img, pe + 20)[0]
    out = []
    for i in range(n):
        at = pe + 24 + opt + i * 40
        name = img[at:at + 8].rstrip(b'\0').decode()
        vsize, vaddr = struct.unpack_from('<II', img, at + 8)
        out.append((name, BASE + vaddr, BASE + vaddr + vsize))
    return out


def normalize(words):
    op = words >> 26
    w = words.copy()
    w[op == 18] &= np.uint32(0xFC000003)
    w[op == 16] &= np.uint32(0xFFFF0003)
    imm = np.isin(op, [14, 15, 24, 25] + list(range(32, 56)))
    w[imm] &= np.uint32(0xFFFF0000)
    return w


def fingerprints(w):
    # fingerprint of K consecutive instructions, per position
    h = np.zeros(len(w) - K + 1, dtype=np.uint64)
    for j in range(K):
        h = h * np.uint64(1000003) ^ w[j:len(w) - K + 1 + j].astype(np.uint64)
    return h


def anchors(ha, hb):
    ua, ia, ca = np.unique(ha, return_index=True, return_counts=True)
    ub, ib, cb = np.unique(hb, return_index=True, return_counts=True)
    ua, ia = ua[ca == 1], ia[ca == 1]
    ub, ib = ub[cb == 1], ib[cb == 1]
    common, pa, pb = np.intersect1d(ua, ub, return_indices=True)
    pairs = sorted(zip(ia[pa].tolist(), ib[pb].tolist()))
    # the longest increasing subsequence in the other edition: anchors in the same order
    queues, prev, idx = [], [-1] * len(pairs), []
    for i, (_, b) in enumerate(pairs):
        k = bisect.bisect_left(queues, b)
        if k == len(queues):
            queues.append(b); idx.append(i)
        else:
            queues[k] = b; idx[k] = i
        prev[i] = idx[k - 1] if k else -1
    chain, i = [], idx[-1] if idx else -1
    while i >= 0:
        chain.append(pairs[i]); i = prev[i]
    return chain[::-1]


def main():
    img_a = open(sys.argv[1], 'rb').read()
    img_b = open(sys.argv[2], 'rb').read()
    output = sys.argv[3]
    requested = [int(x, 16) for x in sys.argv[4:]] if len(sys.argv) > 4 else [int(l.split()[0], 16) for l in sys.stdin if l.strip()]
    sa, sb = sections(img_a), sections(img_b)
    ta = next(s for s in sa if s[0] == '.text')
    tb = next(s for s in sb if s[0] == '.text')
    wa = normalize(np.frombuffer(img_a[ta[1] - BASE:ta[2] - BASE], dtype='>u4').astype(np.uint32))
    wb = normalize(np.frombuffer(img_b[tb[1] - BASE:tb[2] - BASE], dtype='>u4').astype(np.uint32))
    chain = anchors(fingerprints(wa), fingerprints(wb))
    pos_a = [a for a, _ in chain]
    print('anchors in .text: %d (of %d instructions)' % (len(chain), len(wa)))

    def en(secs, dir_):
        return next((s for s in secs if s[1] <= dir_ < s[2]), None)

    rows, count = [], {}
    for d in sorted(set(requested)):
        s = en(sa, d)
        name = s[0] if s else 'outside'
        target, state = None, 'not translated'
        if name == '.text':
            i = (d - ta[1]) // 4
            k = bisect.bisect_right(pos_a, i) - 1
            if k >= 0:
                j = i + (chain[k][1] - chain[k][0])
                if 0 <= j < len(wb):
                    target = tb[1] + 4 * j
                    n = min(VERIFY, len(wa) - i, len(wb) - j)
                    state = 'exact' if np.array_equal(wa[i:i + n], wb[j:j + n]) else 'REVIEW'
        elif name == '.rdata':
            # 32 bytes and, if they appear several times (similar shader headers), larger windows
            rb = next(s for s in sb if s[0] == '.rdata')
            zone = img_b[rb[1] - BASE:rb[2] - BASE]
            state = 'REVIEW (no aparece)'
            for window in (32, 64, 128, 256, 512, 1024):
                chunk = img_a[d - BASE:d - BASE + window]
                p = zone.find(chunk)
                if p < 0:
                    break
                if zone.find(chunk, p + 1) < 0:
                    target, state = rb[1] + p, 'exact'
                    break
                state = 'REVIEW (aparece varias times)'
        elif name in ('.embsec_', '.no_bbt'):
            # embedded code: the section with the same order and size in the other edition; it must be identical
            order = [x for x in sa if x[0] == name].index(s)
            t_b = [x for x in sb if x[0] == name][order]
            if t_b[2] - t_b[1] == s[2] - s[1]:
                target = t_b[1] + (d - s[1])
                na = normalize(np.frombuffer(img_a[d - BASE:d - BASE + 4 * VERIFY], dtype='>u4').astype(np.uint32))
                nb = normalize(np.frombuffer(img_b[target - BASE:target - BASE + 4 * VERIFY], dtype='>u4').astype(np.uint32))
                state = 'exact' if np.array_equal(na, nb) else 'REVIEW (instructions different_2)'
        elif d in (0x82000000, 0x82CD0000) or d >= 0x82D20000:
            target, state = d, 'constant (same in both)'
        elif name == '.data' or name.startswith('.tls') or name.startswith('.idata'):
            equal = img_a[d - BASE - 16:d - BASE + 16] == img_b[d - BASE - 16:d - BASE + 16]
            target, state = d, 'same address' + ('' if equal else ' (content different alrededor)')
        count[name + ' ' + state.split(' (')[0]] = count.get(name + ' ' + state.split(' (')[0], 0) + 1
        rows.append('%08X\t%s\t%s\t%s' % (d, name, '%08X' % target if target else '-', state))
    open(output, 'w', encoding='utf-8').write('pal\tseccion\totra\testado\n' + '\n'.join(rows) + '\n')
    for k, v in sorted(count.items()):
        print('  %-40s %d' % (k, v))


if __name__ == '__main__':
    main()
