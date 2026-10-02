#!/usr/bin/env python3
"""Find game variables from autoplay memscan samples (fh1/src/fh1_memscan.cpp).

    python tools/memscan_match.py <autoplay folder> s1=17 s2=24 s3=30 s4=10 s5=0 [--top 30]

The folder is build_logs/autoplay-<run>-<date> (memscan-addrs.bin + memscan-<sample>.bin); each
NAME=VALUE gives the known quantity at that sample (e.g. the speedometer in mph read from the
screenshot of the same name). Lists the addresses whose value is proportional to the known
quantity in every sample (zero where it is zero), with the ratio: for a speed in mph, ~0.447 means
the game stores m/s, ~1.609 km/h, ~1.0 mph.
"""
import array
import math
import os
import sys


def main():
    args = sys.argv[1:]
    top = 30
    if "--top" in args:
        i = args.index("--top")
        top = int(args[i + 1])
        del args[i:i + 2]
    folder = args[0]
    known = []
    for a in args[1:]:
        name, value = a.split("=")
        known.append((name, float(value)))
    addrs = array.array("I")
    with open(os.path.join(folder, "memscan-addrs.bin"), "rb") as f:
        n = array.array("I", f.read(4))[0]
        addrs.frombytes(f.read(4 * n))
    samples = []
    for name, value in known:
        vals = array.array("f")
        with open(os.path.join(folder, "memscan-%s.bin" % name), "rb") as f:
            vals.frombytes(f.read())
        samples.append((name, value, vals))
    print("%d candidates, %d samples" % (len(addrs), len(samples)))

    moving = [(v, vals) for _, v, vals in samples if v > 0.5]
    still = [vals for _, v, vals in samples if v <= 0.5]
    results = []
    for i in range(len(addrs)):
        ratios = []
        ok = True
        for v, vals in moving:
            x = abs(vals[i])
            if not math.isfinite(x) or x == 0:
                ok = False
                break
            ratios.append(x / v)
        if not ok:
            continue
        for vals in still:
            if not math.isfinite(vals[i]) or abs(vals[i]) > 0.3:
                ok = False
                break
        if not ok:
            continue
        mean = sum(ratios) / len(ratios)
        spread = (max(ratios) - min(ratios)) / mean
        results.append((spread, i, mean))
    results.sort()
    for spread, i, mean in results[:top]:
        values = " ".join("%s=%.3f" % (name, vals[i]) for name, _, vals in samples)
        print("%08X  ratio %.4f  spread %.3f  %s" % (addrs[i], mean, spread, values))


if __name__ == "__main__":
    main()
