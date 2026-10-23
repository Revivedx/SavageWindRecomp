#!/usr/bin/env python3
"""Low-frequency banding energy of an image region: std of (blur(r1) - blur(r2)) over a crop.
bandmetric.py <png> x0 y0 x1 y1 (crop in fractions of the image)."""
import sys
import numpy as np
from PIL import Image, ImageFilter
im = Image.open(sys.argv[1]).convert('L')
w, h = im.size
x0, y0, x1, y1 = (float(v) for v in sys.argv[2:6])
im = im.crop((int(x0 * w), int(y0 * h), int(x1 * w), int(y1 * h))).resize((640, int(640 * (y1 - y0) * h / ((x1 - x0) * w))))
a = np.asarray(im.filter(ImageFilter.GaussianBlur(3)), dtype=np.float32)
b = np.asarray(im.filter(ImageFilter.GaussianBlur(20)), dtype=np.float32)
print(f'{np.std(a - b):.3f}')
