#!/usr/bin/env python3
"""Find short-lived bright spots (twinkles, flicker) across build/frames/*.png: prints 24px cells by how often
they're much brighter than their per-pixel median."""
import collections, glob, sys
import numpy as np
from PIL import Image
fs = sorted(glob.glob('build/frames/*.png'))
A = np.stack([np.asarray(Image.open(f).convert('L'), dtype=np.int16) for f in fs])
D = A - np.median(A, axis=0)
mx = D.max(axis=0)
thr = int(sys.argv[1]) if len(sys.argv) > 1 else 100
ys, xs = np.where(mx > thr)
cl = collections.Counter((y // 24 * 24, x // 24 * 24) for y, x in zip(ys, xs))
print(len(fs), 'frames', A.shape[1:])
for (y, x), v in cl.most_common(12):
    n = sum((D[i, y:y + 24, x:x + 24] > thr).any() for i in range(len(fs)))
    print(f'  y {y:4} x {x:4}  pixels {v:4}  in {n} frames')
