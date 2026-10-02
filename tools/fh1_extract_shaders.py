"""Cuts FH1's shader containers (2008 XDK layout: 102A1100 pixel, 102A1101 vertex) out of the
game's .fxobj files, for the native renderer's shader library (docs/native-renderer-fh1.md, N0).

    python tools/fh1_extract_shaders.py GAME_ROOT OUT_DIR

Writes OUT_DIR/p_<hash>.bin and v_<hash>.bin (one container each, duplicates dropped; the input
format of shaders/nfsmw_hlsl.cpp) and OUT_DIR/index.txt (container -> source file and offset).
Only loose files for now; the tracks' bin.zip archives use zip method 21 (LZX) - not read yet.
"""
import hashlib
import os
import struct
import sys

game_root, out_dir = sys.argv[1], sys.argv[2]
os.makedirs(out_dir, exist_ok=True)
SIGS = {b'\x10\x2a\x11\x00': 'p', b'\x10\x2a\x11\x01': 'v'}

seen = {}
index = []
rejected = 0


def scan(name, data):
    global rejected
    for sig, kind in SIGS.items():
        o = data.find(sig)
        while o >= 0:
            if o + 36 <= len(data):
                virtual, physical = struct.unpack_from('>II', data, o + 4)
                const_off, def_off, shader_off = struct.unpack_from('>III', data, o + 16)
                size = virtual + physical
                ok = (36 <= virtual < 0x40000 and 0 < physical < 0x100000 and physical % 4 == 0
                      and o + size <= len(data) and shader_off < virtual and const_off < virtual
                      and def_off < virtual)
                if ok:
                    blob = data[o:o + size]
                    h = hashlib.sha1(blob).hexdigest()[:16]
                    if h not in seen:
                        seen[h] = kind
                        with open(os.path.join(out_dir, f'{kind}_{h}.bin'), 'wb') as f:
                            f.write(blob)
                    index.append(f'{kind}_{h} {name} {o:#x} {size}')
                else:
                    rejected += 1
            o = data.find(sig, o + 1)


for dp, _, files in os.walk(game_root):
    for f in files:
        if f.lower().endswith('.fxobj'):
            path = os.path.join(dp, f)
            scan(os.path.relpath(path, game_root), open(path, 'rb').read())

with open(os.path.join(out_dir, 'index.txt'), 'w') as f:
    f.write('\n'.join(index) + '\n')
kinds = list(seen.values())
print(f"{len(index)} containers found, {len(seen)} distinct "
      f"({kinds.count('p')} pixel, {kinds.count('v')} vertex), {rejected} signature matches rejected")
