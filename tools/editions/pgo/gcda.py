# -*- coding: utf-8 -*-
# Reads and writes GCC 16 .gcda files (the PGO profile): header, function records and counters.
import struct

TAG_FUNCTION = 0x01000000
TAG_SUMMARY = 0xA1000000


def read(path):
    b = open(path, 'rb').read()
    magic, version, stamp, sum = struct.unpack_from('<4I', b, 0)
    assert magic == 0x67636461, 'not a .gcda'
    o = 16
    register_values = []
    while o + 8 <= len(b):
        tag, is_long = struct.unpack_from('<Ii', b, o)
        o += 8
        if tag == 0:
            break
        data = b[o:o + max(is_long, 0)]
        o += max(is_long, 0)
        register_values.append([tag, is_long, data])
    return (magic, version, stamp, sum), register_values


def write(path, header, register_values):
    partes = [struct.pack('<4I', *header)]
    for tag, is_long, data in register_values:
        partes.append(struct.pack('<Ii', tag, is_long) + data)
    partes.append(struct.pack('<I', 0))
    open(path, 'wb').write(b''.join(partes))


def functions(register_values):
    # [(function record index, ident, lineno, cfg, [counter records])]
    out = []
    for i, (tag, is_long, data) in enumerate(register_values):
        if tag == TAG_FUNCTION and is_long == 12:
            ident, lineno, cfg = struct.unpack('<3I', data)
            out.append([i, ident, lineno, cfg, []])
        elif out and tag not in (TAG_FUNCTION, TAG_SUMMARY):
            out[-1][4].append(i)
    return out


def crc32_gcc(chksum, text):
    # GCC's crc32_string: CRC-32 0x04C11DB7 without reflection, byte by byte, including the final 0
    for c in text.encode() + b'\0':
        input_value = c << 24
        for _ in range(8):
            feedback = 0x04C11DB7 if (input_value ^ chksum) & 0x80000000 else 0
            chksum = ((chksum << 1) & 0xFFFFFFFF) ^ feedback
            input_value = (input_value << 1) & 0xFFFFFFFF
    return chksum


def ident_public(name_assembler):
    # GCC's coverage_compute_profile_id for a visible symbol: crc32 of the name, 31 bits and never 0
    c = crc32_gcc(0, name_assembler) & 0x7FFFFFFF
    return c + (not c)
