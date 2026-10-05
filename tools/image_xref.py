"""Finds the code of default.xex's image that builds an address with lis + addi / ori (strings, tables, globals).

    python tools\\image_xref.py ..\\build_logs\\fh1_image.bin 8223AFD0 82242670

The image is the file --fh1_dump_image writes (loaded at 82000000). Prints each place, and the start of the
function it is in when the generated sources are there (the nearest "mflr" before it otherwise).
"""
import struct
import sys

BASE = 0x82000000


def main():
    data = open(sys.argv[1], "rb").read()
    targets = [int(a, 16) for a in sys.argv[2:]]
    words = struct.unpack(">%dI" % (len(data) // 4), data[:len(data) // 4 * 4])
    for target in targets:
        low = target & 0xFFFF
        high_addi = ((target + 0x8000) >> 16) & 0xFFFF
        high_ori = (target >> 16) & 0xFFFF
        hits = 0
        for i, w in enumerate(words):
            op = w >> 26
            if (w & 0xFFFF) != low or op not in (14, 24, 32, 34, 36, 38, 40, 48, 50, 52, 54):
                continue
            # addi rD, rA, low (14) / ori rA, rS, low (24) / a load or store with that displacement
            reg = (w >> 16) & 31 if op != 24 else (w >> 21) & 31
            want = high_ori if op == 24 else high_addi
            for j in range(i - 1, max(i - 24, -1), -1):
                v = words[j]
                if (v >> 26) == 15 and ((v >> 16) & 31) == 0 and ((v >> 21) & 31) == reg:
                    if (v & 0xFFFF) == want:
                        start = j
                        while start > 0 and words[start] != 0x7D8802A6 and i - start < 4000:
                            start -= 1
                        print("%08X: used at %08X (lis at %08X), mflr before it at %08X" % (
                            target, BASE + i * 4, BASE + j * 4, BASE + start * 4))
                        hits += 1
                    break
        print("%08X: %d places" % (target, hits))


main()
