"""Far-scenery fog check for the festival start view (paint shop, parked car, 88 s shot).
Prints the festival dome's warmth (red minus blue, cream when right, grey when foggy) and the
contrast of the mountain band. Correct pictures: warmth well above 20.
    python tools/img_diff.py shot.png [shot2.png ...]"""
import sys
import numpy as np
from PIL import Image

for path in sys.argv[1:]:
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)
    h, w, _ = a.shape
    dome = a[int(h * 0.355):int(h * 0.395), int(w * 0.72):int(w * 0.82)]
    hills = a[int(h * 0.26):int(h * 0.32), int(w * 0.40):int(w * 0.62)]
    warmth = (dome[..., 0] - dome[..., 2]).mean()
    print(f"{path}: dome warmth {warmth:5.1f}  hills contrast {hills.mean(axis=2).std():5.1f}")
