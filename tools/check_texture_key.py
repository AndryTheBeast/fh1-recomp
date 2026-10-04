# -*- coding: utf-8 -*-
# Check of the already built ELF, without running anything. It looks in the machine code for the texture
# key of PrepareTexture (keys[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, f[5] >> 9} and
# its XXH3) and checks whether any read of those 20 stack bytes comes before the store of what it reads.
# That is the bug that broke texture keys:
#     ldr x3, [sp+548]      <- reads keys[3] and keys[4]
#     str w2, [sp, #552]    <- writes keys[4] afterwards
# Usage:  py check_texture_key.py <elf> [<elf> ...]     (the .stripped.elf works; no symbols needed)
# Output: one line per site found and a verdict per ELF: OK, MAL or NO FOUND. Exit code 1 if any
# ELF is MAL or NO FOUND.
#
# How it finds the site: the only instruction "and wD, wN, #0xffc003ff" followed by "str wD, [sp, #K]"
# and by a 20-byte XXH3, either inlined (constant 20 * PRIME64_1 = 0x5c5581de766bd28c) or called out
# of line with x1 = 20.
# Limits: it follows program order in a straight window (it does not follow branches); that is why it
# also lists the instructions it used, so they can be checked by hand.

import os
import re
import struct
import subprocess
import sys

OBJDUMP = os.environ.get('OBJDUMP') or os.path.join(os.environ.get('DEVKITPRO', '/opt/devkitpro'), 'devkitA64',
                                                     'bin', 'aarch64-none-elf-objdump')
if not os.path.exists(OBJDUMP):
    OBJDUMP = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objdump'

BEFORE = 64      # instructions before the AND that are examined (some key words are written earlier)
AFTER = 200   # instructions after the AND


def section_text(path):
    with open(path, 'rb') as f:
        data = f.read()
    assert data[:4] == b'\x7fELF' and data[4] == 2 and data[5] == 1, path + ': no es un ELF64 little-endian'
    e_shoff, = struct.unpack_from('<Q', data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x3A)
    def header(i):
        return struct.unpack_from('<IIQQQQIIQQ', data, e_shoff + i * e_shentsize)
    names = header(e_shstrndx)
    for i in range(e_shnum):
        c = header(i)
        ini = names[4] + c[0]
        name = data[ini:data.index(b'\x00', ini)].decode()
        if name == '.text':
            return data, c[3], c[4], c[5]  # address, file offset, size
    raise SystemExit(path + ': no .text section')


def ands_of_the_mask(data, address, displacement, size):
    # and wD, wN, #0xffc003ff = 0x120a4c00 | (N << 5) | D: bytes LE xx [4c-4f] 0a 12
    chunk = data[displacement:displacement + size]
    for m in re.finditer(rb'[\x00-\xff][\x4c-\x4f]\x0a\x12', chunk, re.S):
        if m.start() % 4 == 0:
            word, = struct.unpack_from('<I', chunk, m.start())
            if word & 0xFFFFFC00 == 0x120A4C00:
                yield address + m.start()


LINE = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')


def disassemble(path, since, until):
    output = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', '--start-address=' + hex(since),
                             '--stop-address=' + hex(until), path], capture_output=True, text=True).stdout
    instr = []
    for l in output.splitlines():
        m = LINE.match(l)
        if m:
            ops = m.group(3).split('//')[0].strip()
            instr.append((int(m.group(1), 16), m.group(2), ops))
    return instr


TAM = {'x': 8, 'w': 4, 'q': 16, 'd': 8, 's': 4, 'h': 2, 'b': 1}
MEM = re.compile(r'\[(sp|x\d+)(?:,\s*#(-?(?:0x[0-9a-f]+|\d+)))?\](!?)')


def access(mnem, ops, bases):
    """(kind, start relative to sp, bytes) for loads and stores based on sp (or on a register = sp + imm)."""
    is_load = mnem.startswith('ld')
    es_alm = mnem.startswith('st')
    if not (is_load or es_alm) or mnem.startswith(('ldax', 'stlx', 'ldxr', 'stxr', 'ldar', 'stlr')):
        return None
    m = MEM.search(ops)
    if not m:
        return None
    base = m.group(1)
    imm = int(m.group(2), 0) if m.group(2) else 0
    if ', #' in ops[m.end():]:
        imm = 0  # post-indexed: accesses at the base
    if base == 'sp':
        start = imm
    elif base in bases:
        start = bases[base] + imm
    else:
        return None
    regs = [r.strip() for r in ops[:m.start()].rstrip(', ').split(',')]
    par = mnem in ('ldp', 'stp', 'ldnp', 'stnp')
    if mnem in ('ldrb', 'strb', 'ldrsb', 'ldurb', 'sturb'):
        tam = 1
    elif mnem in ('ldrh', 'strh', 'ldrsh', 'ldurh', 'sturh'):
        tam = 2
    elif mnem == 'ldrsw':
        tam = 4
    else:
        tam = TAM.get(regs[0][0], 8) if regs and regs[0] else 8
        if regs[0] in ('xzr',):
            tam = 8
        if regs[0] in ('wzr',):
            tam = 4
    if par:
        tam *= 2
    return ('load' if is_load else 'write', start, tam)


def target(ops):
    return ops.split(',')[0].strip() if ops else ''


def analyze(path, address_and):
    instr = disassemble(path, address_and - 4 * BEFORE, address_and + 4 * AFTER)
    i_and = next((i for i, x in enumerate(instr) if x[0] == address_and), None)
    if i_and is None:
        return None
    rd = target(instr[i_and][2])
    # K: the first store of wD to the stack after the AND (keys[0])
    k = None
    for a, mnem, ops in instr[i_and + 1:]:
        if mnem == 'str' and ops.startswith(rd + ',') and '[sp' in ops:
            m = MEM.search(ops)
            k = int(m.group(2), 0) if m.group(2) else 0
            break
        if target(ops) in (rd, 'x' + rd[1:]) and not mnem.startswith('st'):
            break
    if k is None:
        return None
    zone = range(k, k + 20)
    bases = {}
    first_write = {}
    bad = []
    used = []
    signature = None
    call_after_writes = None
    for idx, (a, mnem, ops) in enumerate(instr):
        if mnem == 'mov' and re.match(r'x\d+, #0xd28c$', ops):
            signature = 'XXH3 integrado (0x...d28c = 20 * PRIME64_1)'
        if mnem == 'mov' and ops == 'x1, #0x14' and idx > i_and:
            signature = signature or 'XXH3 out of line (x1 = 20)'
        acc = access(mnem, ops, bases)
        if acc:
            type, ini, tam = acc
            due = [b for b in range(ini, ini + tam) if b in zone]
            if due:
                used.append((a, mnem, ops, type))
                if type == 'write':
                    for b in due:
                        first_write.setdefault(b, idx)
                elif idx > i_and and call_after_writes is None:
                    missing = [b for b in due if first_write.get(b, 10 ** 9) > idx]
                    if missing:
                        bad.append((a, mnem, ops, min(missing), max(missing)))
        # registers that point into the stack: add xT, sp, #imm
        m = re.match(r'(x\d+), sp, #(0x[0-9a-f]+|\d+)$', ops) if mnem == 'add' else None
        d = target(ops)
        if m:
            bases[m.group(1)] = int(m.group(2), 0)
        elif d in bases and not mnem.startswith(('st', 'cmp', 'tst', 'cb', 'tb', 'b.')):
            bases.pop(d, None)
        if idx > i_and and mnem in ('bl', 'blr') and all(b in first_write for b in zone):
            if call_after_writes is None:
                call_after_writes = a
        if idx > i_and and mnem in ('ret', 'b', 'br'):
            break
    if signature is None:
        return None
    return {'and': address_and, 'k': k, 'signature': signature, 'bad': bad, 'used': used,
            'written': sorted(set(b - k for b in first_write)), 'call': call_after_writes}


def main():
    if len(sys.argv) < 2:
        print('use: py check_texture_key.py <elf> [<elf> ...]')
        return 2
    miss = False
    for path in sys.argv[1:]:
        data, address, displacement, size = section_text(path)
        sites = []
        for a in ands_of_the_mask(data, address, displacement, size):
            r = analyze(path, a)
            if r:
                sites.append(r)
        print('== ' + path)
        if not sites:
            print('   NOT FOUND: no "and wD, wN, #0xffc003ff" with the key on the stack and a 20-byte XXH3')
            miss = True
            continue
        for s in sites:
            print('   site: AND at {:#x}; keys at [sp+{}, sp+{}); {}'.format(s['and'], s['k'], s['k'] + 20, s['signature']))
            for a, mnem, ops, type in s['used']:
                print('      {:#x}  {:8} {:30} {}'.format(a, mnem, ops, type))
            if s['bad']:
                miss = True
                for a, mnem, ops, b0, b1 in s['bad']:
                    print('   BAD: {:#x} {} {} reads sp+{}..sp+{} BEFORE writing it (keys[{}])'.format(
                        a, mnem, ops, b0, b1, (b0 - s['k']) // 4))
            elif len(s['written']) != 20:
                miss = True
                print('   BAD: only {} of the 20 key bytes are seen written in the window'.format(len(s['written'])))
            else:
                extra = ' (XXH3 called at {:#x}, after the 5 writes)'.format(s['call']) if s['call'] else ''
                print('   OK: every read of the key comes after its write' + extra)
    return 1 if miss else 0


if __name__ == '__main__':
    sys.exit(main())
