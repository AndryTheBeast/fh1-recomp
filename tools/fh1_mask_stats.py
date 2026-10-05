# Prints the four channels of the shadow / headlight mask (1CE2D000) from a --fh1_dump_resolved_at_s dump of the
# native renderer: minimum, maximum, mean, and the value at two pixels (ground at the left, centre of the screen).
# The emulated GPU at the festival at evening gives about R 255, G 124, B 138, A 191 at the centre (the car's
# brake-light cone) and 255 in every channel on the ground outside the cone.
#   python tools/fh1_mask_stats.py [dump folder, default fh1/out/win-release/dump_resolved]
import os
import sys

import numpy as np
from PIL import Image

folder = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "fh1", "out", "win-release", "dump_resolved")
name = "frame_01_dest1CE2D000_1280x720.png"
color = np.array(Image.open(os.path.join(folder, name)).convert("RGB"))[::-1]  # the dumps are upside down
alpha = np.array(Image.open(os.path.join(folder, "alpha_" + name)).convert("RGB"))[::-1, :, 0]
mask = np.dstack([color, alpha])
for c, channel in enumerate("RGBA"):
    v = mask[:, :, c]
    print("%s min %3d max %3d mean %5.1f  ground(300,600) %3d  centre(640,360) %3d" % (
        channel, v.min(), v.max(), v.mean(), v[600, 300], v[360, 640]))
