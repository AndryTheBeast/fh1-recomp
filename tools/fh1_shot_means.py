#!/usr/bin/env python3
"""Mean brightness of each screenshot of a test run, in the order of their seconds.

    python tools/fh1_shot_means.py build_logs\\test-ovN-20261005-121216

prints one line per build_logs\\test-ovN-...-<second>s.png: the second, the mean brightness (0-255) of the picture
area (the middle 90 % of the window, without its title bar and the black bars) and the mean color. Used for the
brightness overshoot after a loading screen: a shot every second, and the step shows in the numbers.
"""
import glob
import re
import sys

from PIL import Image, ImageStat


def main():
    prefix = sys.argv[1]
    shots = []
    for path in glob.glob(prefix + "-*s.png"):
        m = re.search(r"-(\d+)s\.png$", path)
        if m:
            shots.append((int(m.group(1)), path))
    for second, path in sorted(shots):
        im = Image.open(path).convert("RGB")
        w, h = im.size
        box = im.crop((int(w * 0.05), int(h * 0.12), int(w * 0.95), int(h * 0.90)))
        r, g, b = ImageStat.Stat(box).mean
        print("%4d s  mean %5.1f   rgb %5.1f %5.1f %5.1f" % (second, (r + g + b) / 3, r, g, b))


if __name__ == "__main__":
    main()
