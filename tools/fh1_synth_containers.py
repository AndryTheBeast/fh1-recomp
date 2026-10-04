#!/usr/bin/env python3
# Copied from nfsc-recomp (GoatHonks, GPL-3.0) for FH1: rebuilds 2008 containers from the native renderer's --fh1_dump_ring_shaders files.
"""nfsc: builds Xbox 360 shader containers (2008 layout) from microcode alone.

The native renderer writes the microcode of every shader its library does not know (--fh1_dump_ring_shaders): a file
p_/v_<hash>.mc = type byte, big-endian u32 word count, big-endian microcode words. Direct3D releases the container
(header and constant table) once the GPU has the microcode, so everything the translator needs is rebuilt here:
  * vertex fetches   -> vertex elements (usages are only labels: each gets a distinct input location from a fixed pool),
  * texture fetches  -> sampler entries s<N> (type from the fetch dimension),
  * exports (ALU)    -> the pixel shader output mask,
  * constants        -> one float4 array covering every register (names are positional: the translator reads c<N>).
Interpolants are named by position by the translator itself (see shaders/XenosRecomp/shader_recompiler.cpp, NFSMW_RECOMP).

Usage: fh1_synth_containers.py <folder with .mc files> <new output folder>      (output: p_/v_<hash>.bin containers)
Never commit the output (derived from the game).
"""
import os
import struct
import sys

USAGE_POOL = [(0, 0), (3, 0), (6, 0), (7, 0), (5, 0), (5, 1), (5, 2), (5, 3), (10, 0), (1, 0), (10, 1),
              (5, 4), (5, 5), (5, 6), (5, 7)]  # Position0, Normal0, Tangent0, Binormal0, TexCoord0-3, Color0, BlendWeight0, ...
SAMPLER_TYPE = {0: 11, 1: 12, 2: 13, 3: 14}  # Sampler1D, 2D, 3D, Cube (D3DXPARAMETER_TYPE)


def walk(words):
    """Returns (fetch instructions [(index, kind, w0, w1, w2)], alu instruction indices) from the control flow."""
    n = len(words)
    instr_size = n * 4
    pos = 0
    execs = []
    while pos < instr_size and pos + 12 <= n * 4:
        w0, w1, w2 = words[pos // 4], words[pos // 4 + 1], words[pos // 4 + 2]
        code0, code1 = w0, w1 & 0xFFFF
        code2, code3 = (w1 >> 16) | ((w2 << 16) & 0xFFFFFFFF), w2 >> 16
        for lo, hi in ((code0, code1), (code2, code3)):
            op = (hi >> 12) & 0xF
            if op in (1, 2, 3, 4, 5, 6, 13, 14):
                address, count, sequence = lo & 0xFFF, (lo >> 12) & 7, (lo >> 16) & 0xFFF
                execs.append((address, count, sequence))
                if address:
                    instr_size = min(instr_size, address * 12)
        pos += 12
    fetches, alus = [], []
    for address, count, sequence in execs:
        for i in range(count):
            idx = address + i
            if (idx + 1) * 3 > n:
                raise ValueError('EXEC beyond the microcode')
            w = words[idx * 3: idx * 3 + 3]
            if (sequence >> (2 * i)) & 1:
                fetches.append((idx, w[0] & 0x1F, w[0], w[1], w[2]))
            else:
                alus.append((idx, w[0], w[1], w[2]))
    return fetches, alus


def build(kind, words):
    pixel = kind == 'p'
    fetches, alus = walk(words)
    vfetch = sorted(f for f in fetches if f[1] == 0)
    tfetch = [f for f in fetches if f[1] == 1]
    if not pixel and len(vfetch) > len(USAGE_POOL):
        raise ValueError('too many vertex fetches (%d)' % len(vfetch))
    # pixel shader outputs from the export instructions
    outputs = 0
    if pixel:
        for _, w0, _, _ in alus:
            if not (w0 >> 15) & 1:
                continue
            for dest, mask in ((w0 & 0x3F, (w0 >> 16) & 0xF), ((w0 >> 8) & 0x3F, (w0 >> 20) & 0xF)):
                if mask:
                    if dest <= 3:
                        outputs |= 1 << dest
                    elif dest == 61:
                        outputs |= 0x10
    samplers = {}
    for _, _, w0, w1, w2 in tfetch:
        samplers.setdefault((w0 >> 20) & 0x1F, (w2 >> 14) & 3)

    # ---- constant table
    names = []
    strings = bytearray()

    def add_string(t):
        off = len(strings)
        strings.extend(t.encode() + b'\0')
        return off

    consts = [('g_c', 2, 0, 256)]  # name, register set (2 = Float4, 3 = Sampler), index, count
    for reg in sorted(samplers):
        consts.append(('s%d' % reg, 3, reg, 1))
    ntypes = 1 + len(samplers)
    # layout inside the table (offsets relative to the start of the ConstantTable struct, 28 bytes long)
    info_off = 28
    types_off = info_off + 20 * len(consts)
    str_base = types_off + 16 * ntypes
    entries = []
    types = bytearray()
    types += struct.pack('>HHHHHHI', 1, 3, 1, 4, 1, 0, 0)  # Vector, Float, array of float4
    for reg in sorted(samplers):
        types += struct.pack('>HHHHHHI', 4, SAMPLER_TYPE[samplers[reg]], 1, 1, 1, 0, 0)  # Object, Sampler<N>D
    ti = 0
    for name, rs, idx, cnt in consts:
        so = add_string(name)
        entries.append(struct.pack('>IHHHHII', str_base + so, rs, idx, cnt, 0, types_off + 16 * ti, 0))
        ti += 1
    table_size = str_base + len(strings)
    table = struct.pack('>7I', table_size, 0, 0, len(consts), info_off, 0, 0) + b''.join(entries) + bytes(types) + bytes(strings)
    table += b'\0' * (-len(table) % 4)
    ctab = struct.pack('>I', len(table) + 4) + table  # ConstantTableContainer: size + table

    # ---- shader struct
    if pixel:
        shader = struct.pack('>6I', 0, len(words) * 4, 0, 0xFF00, 0, 16 << 5)  # fieldC: no position register
        shader += struct.pack('>2I', 0, outputs)
        for i in range(16):
            shader += struct.pack('>I', 5 << 4 | i << 8 | 0)  # TexCoord i in register i (positional)
    else:
        shader = struct.pack('>6I', 0, len(words) * 4, 0, 0, 0, 0)
        shader += struct.pack('>3I', 0, len(vfetch), 0)
        for k, f in enumerate(vfetch):
            usage, uidx = USAGE_POOL[k]
            shader += struct.pack('>I', f[0] | usage << 12 | uidx << 16)

    header_len = 36
    shader_off = header_len
    ct_off = shader_off + len(shader)
    virtual = ct_off + len(ctab)
    pad = -virtual % 4
    virtual += pad
    flags = 0x102A1100 | (0 if pixel else 1)
    head = struct.pack('>9I', flags, virtual, len(words) * 4, 0, ct_off, 0, shader_off, 0, 0)
    body = head + shader + ctab + b'\0' * pad
    micro = b''.join(struct.pack('>I', w) for w in words)
    return body + micro


def main():
    src, dst = sys.argv[1], sys.argv[2]
    if os.path.exists(dst):
        sys.exit('output folder exists')
    os.makedirs(dst)
    ok = bad = 0
    for name in sorted(os.listdir(src)):
        if not name.endswith('.mc'):
            continue
        b = open(os.path.join(src, name), 'rb').read()
        kind = chr(b[0])
        n = struct.unpack_from('>I', b, 1)[0]
        words = list(struct.unpack_from('>%dI' % n, b, 5))
        try:
            data = build(kind, words)
        except Exception as e:  # noqa: BLE001
            print('%s: %s' % (name, e))
            bad += 1
            continue
        open(os.path.join(dst, name[:-3] + '.bin'), 'wb').write(data)
        ok += 1
    print('built %d containers, %d rejected' % (ok, bad))


if __name__ == '__main__':
    main()
