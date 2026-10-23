#!/bin/bash
# Montage of build/frame_*.png -> build/montage.png (4 columns, 320px thumbnails)
cd "$(dirname "$0")/../.."
rm -f build/s_*.png
for f in build/frame_*.png; do sips -Z 320 "$f" --out "${f/frame_/s_}" >/dev/null; done
tools/venv/bin/python -c "
from PIL import Image
import glob
fs=sorted(glob.glob('build/s_*.png'))
w,h=Image.open(fs[0]).size
cols=4; rows=(len(fs)+cols-1)//cols
m=Image.new('RGB',(w*cols,h*rows))
for i,f in enumerate(fs): m.paste(Image.open(f),((i%cols)*w,(i//cols)*h))
m.save('build/montage.png')"
