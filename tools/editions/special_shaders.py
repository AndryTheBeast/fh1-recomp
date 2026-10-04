# -*- coding: utf-8 -*-
# Finds in another edition the three shaders the renderer recognizes by fingerprint (bright pass, sky and
# composition) by comparing the microcode, and prints their XXH3 fingerprints (computed by xxh3.exe).
# Usage: special_shaders.py <PAL containers> <other containers> <xxh3 PAL> <xxh3 other>
import os, sys


def be(b, o):
    return int.from_bytes(b[o:o + 4], 'big')


def fingerprints(file):
    out = {}
    for line in open(file, encoding='utf-8'):
        h, path = line.split(None, 1)
        out[os.path.basename(path.strip())] = h
    return out


pal_dir, other_dir, hx_pal, hx_other = sys.argv[1:5]
h_pal, h_other = fingerprints(hx_pal), fingerprints(hx_other)
other = {n: open(os.path.join(other_dir, n), 'rb').read() for n in os.listdir(other_dir) if n.endswith('.bin')}
for name, rol in [('p_000094.bin', 'glow'), ('p_000117.bin', 'sky'), ('p_000139.bin', 'composition')]:
    b = open(os.path.join(pal_dir, name), 'rb').read()
    v = be(b, 4)
    identical = sorted(n for n, u in other.items() if u == b)
    same = sorted(n for n, u in other.items() if be(u, 4) == v and u[v:] == b[v:])
    print('%-12s %s PAL %s | identical %s | same microcode %s' % (
        rol, name, h_pal[name], identical, ', '.join('%s=%s' % (n, h_other[n]) for n in same)))
