r"""Numbers for comparing two screenshots of the same moment (native against emulated).

    python tools/fh1_pic_stats.py A.png B.png [--box x0,y0,x1,y1] [--crop out.png] [--zoom 3]

For each picture: brightness percentiles 5/25/50/75/95 of the picture area (the black bars are left out), mean
color, mean horizontal gradient (sharpness) and the share of near-white pixels. With --box the same numbers for
that rectangle (window pixels), and --crop saves the rectangles side by side (enlarged with NEAREST).
"""
import sys
import numpy as np
from PIL import Image


def area(im):
    a = np.asarray(im.convert("RGB")).astype(np.float32)
    lum = a.mean(axis=2)
    rows = np.where(lum.mean(axis=1) > 3)[0]
    cols = np.where(lum.mean(axis=0) > 3)[0]
    if len(rows) == 0 or len(cols) == 0:
        return a
    return a[rows[0]:rows[-1] + 1, cols[0]:cols[-1] + 1]


def stats(a):
    lum = a.mean(axis=2)
    p = np.percentile(lum, [5, 25, 50, 75, 95])
    grad = np.abs(np.diff(lum, axis=1)).mean()
    white = (a.min(axis=2) > 200).mean() * 100
    m = a.reshape(-1, 3).mean(axis=0)
    return "p5/25/50/75/95 %s  mean RGB %3.0f %3.0f %3.0f  gradient %.2f  near-white %.2f%%" % (
        "/".join("%.0f" % v for v in p), m[0], m[1], m[2], grad, white)


args = sys.argv[1:]
box = crop = None
zoom = 3
files = []
i = 0
while i < len(args):
    if args[i] == "--box":
        box = [int(v) for v in args[i + 1].split(",")]; i += 2
    elif args[i] == "--crop":
        crop = args[i + 1]; i += 2
    elif args[i] == "--zoom":
        zoom = int(args[i + 1]); i += 2
    else:
        files.append(args[i]); i += 1
cuts = []
for f in files:
    im = Image.open(f)
    print(f.replace("\\", "/").split("/")[-1])
    print("   whole:", stats(area(im)))
    if box:
        c = im.convert("RGB").crop(box)
        print("   box:  ", stats(np.asarray(c).astype(np.float32)))
        cuts.append(c.resize((c.width * zoom, c.height * zoom), Image.NEAREST))
if crop and cuts:
    out = Image.new("RGB", (sum(c.width for c in cuts) + 8 * (len(cuts) - 1), cuts[0].height), (255, 0, 255))
    x = 0
    for c in cuts:
        out.paste(c, (x, 0)); x += c.width + 8
    out.save(crop)
    print("saved", crop)
