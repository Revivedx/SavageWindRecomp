#!/usr/bin/env python3
"""Tests tools/maphd.py on synthetic map tiles (no game files and no Real-ESRGAN needed): small BFRES / BNTX files
made here the way the game's tiles are laid out, and stand-in "cleaned" images in place of Real-ESRGAN's.

    python3 tools/test_maphd.py

One PASS / FAIL line per check; the exit status is 1 when anything failed. Needs numpy and Pillow, like the tool.
"""
import os
import struct
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import maphd as m  # noqa: E402

failed = 0


def check(name, ok):
    global failed
    print(('PASS ' if ok else 'FAIL ') + name)
    failed += not ok


def make_tile(rgb, th=4):
    """A map tile (BFRES with one BNTX holding one BC1 texture) of an H x W x 3 image, laid out like the game's:
    the BNTX at 0x1000, its data after a BRTD header, each part's relocation table after it."""
    h, w = rgb.shape[:2]
    data = m.swizzle(m.bc1_encode(np.concatenate([rgb, np.full((h, w, 1), 255, np.uint8)], -1)), w, h, th)
    size = len(data)
    brti, brtd = 0x200, 0xff0
    head = bytearray(brtd + 0x10)
    head[0:4] = b'BNTX'
    struct.pack_into('<I', head, 0x24, 1)
    struct.pack_into('<Q', head, 0x30, brtd)
    head[brti:brti + 4] = b'BRTI'
    struct.pack_into('<H', head, brti + 0x16, 1)
    struct.pack_into('<I', head, brti + 0x1c, m.BC1_SRGB)
    struct.pack_into('<II', head, brti + 0x24, w, h)
    struct.pack_into('<I', head, brti + 0x34, th)
    struct.pack_into('<I', head, brti + 0x50, size)
    head[brtd:brtd + 4] = b'BRTD'
    struct.pack_into('<Q', head, brtd + 8, size + 0x10)
    rlt = bytearray(0x68)
    rlt[0:4] = b'_RLT'
    struct.pack_into('<I', rlt, 4, brtd + 0x10 + size)
    struct.pack_into('<I', rlt, 0x34, size + 0x10)
    struct.pack_into('<II', head, 0x18, brtd + 0x10 + size, brtd + 0x10 + size + len(rlt))
    bntx = bytes(head) + data + bytes(rlt)
    b = 0x1000
    f_rlt = (b + len(bntx) + 0xfff) & ~0xfff
    frlt = bytearray(0x10 + 0x18)
    frlt[0:4] = b'_RLT'
    struct.pack_into('<II', frlt, 4, f_rlt, 1)
    struct.pack_into('<QIIII', frlt, 0x10, 0, b, f_rlt - b, 0, 1)
    out = bytearray(b) + bntx + bytes(f_rlt - b - len(bntx)) + frlt
    out[0:4] = b'FRES'
    struct.pack_into('<II', out, 0x18, f_rlt, len(out))
    struct.pack_into('<QQ', out, 0xf0, b, len(bntx))
    return bytes(out)


def noise(h, w, seed):
    r = np.random.default_rng(seed)
    base = np.repeat(np.repeat(r.integers(0, 256, (h // 4 + 1, w // 4 + 1, 3)), 4, 0), 4, 1)[:h, :w]
    return base.astype(np.uint8)


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def main():
    for d in (b'', b'abc', bytes(range(256)) * 9):
        check(f'Yaz0 store / decompress round trip ({len(d)} bytes)', m.yaz0_decompress(m.yaz0_store(d)) == d)

    for w, h in ((40, 40), (132, 132), (300, 300)):
        lin = np.random.default_rng(w).integers(0, 256, (((h + 3) // 4), (w + 3) // 4 * 8)).astype(np.uint8)
        check(f'block-linear swizzle round trip {w}x{h}', (m.unswizzle(m.swizzle(lin, w, h, 4), w, h, 4) == lin).all())

    flat = np.zeros((8, 8, 4), np.uint8)
    flat[...] = (90, 140, 60, 255)
    dec = m.bc1_decode(m.bc1_encode(flat), 8, 8)
    check('BC1: a flat colour stays flat and close', len(np.unique(dec[..., :3].reshape(-1, 3), axis=0)) == 1 and
          np.abs(dec[..., :3].astype(int) - flat[..., :3]).max() <= 4)
    grad = np.zeros((16, 16, 4), np.uint8)
    grad[..., 0] = np.arange(16)[None, :] * 16
    grad[..., 3] = 255
    dec = m.bc1_decode(m.bc1_encode(grad), 16, 16)
    check('BC1: a gradient within a few levels', np.abs(dec[..., 0].astype(int) - grad[..., 0]).mean() < 6)

    img = noise(40, 40, 1)
    tile = make_tile(img)
    info = m.tile_parse(tile)
    check('tile: reads back', (info['w'], info['h'], info['th']) == (40, 40, 4))
    same = m.tile_with(tile, info, img[::-1].copy())
    check('tile: same size keeps the file\'s length', len(same) == len(tile) and
          np.abs(m.tile_rgb(same, m.tile_parse(same)).astype(int) - img[::-1]).mean() < 8)
    big_img = np.repeat(np.repeat(img, 2, 0), 2, 1)
    big = m.tile_with(tile, info, big_img)
    bi = m.tile_parse(big)
    check('tile: a bigger texture', (bi['w'], bi['h']) == (80, 80) and
          np.abs(m.tile_rgb(big, bi).astype(int) - big_img).mean() < 8)
    f_rlt, fsize = struct.unpack_from('<II', big, 0x18)
    b_size, = struct.unpack_from('<Q', big, 0xf8)
    check('tile: the BFRES around it grows', fsize == len(big) and big[f_rlt:f_rlt + 4] == b'_RLT' and
          struct.unpack_from('<I', big, f_rlt + 4)[0] == f_rlt and f_rlt % 0x1000 == 0 and
          struct.unpack_from('<I', big, f_rlt + 0x10 + 12)[0] == f_rlt - 0x1000 and
          big[0x1000 + b_size - 0x68:0x1000 + b_size - 0x64] == b'_RLT')

    with tempfile.TemporaryDirectory() as t:
        src, work, out = (os.path.join(t, n) for n in ('src', 'work', 'out'))
        # a 2 x 2 corner of the grid at every level, and a variant of one tile, sizes as 3000 / 840 / 300 / 135 / 20
        sizes = {0: 40, 1: 24, 2: 16, 3: 12}
        imgs = {}
        for lv, s in sizes.items():
            for k, (c, r) in enumerate((('Z', 0), ('A', 0), ('Z', 1), ('A', 1))):
                n = f"MapTex{lv or ''}_{c}-{r}.sbmaptex"
                imgs[n] = noise(s, s, lv * 10 + k)
                put(os.path.join(src, n), m.yaz0_store(make_tile(imgs[n])))
            n = f"MapTex{lv or ''}_A-0_4.sbmaptex"
            imgs[n] = noise(s, s, 99 + lv)
            put(os.path.join(src, n), m.yaz0_store(make_tile(imgs[n])))
        old_pad = dict(m.PAD)
        m.PAD.update({0: 8, 1: 4, 2: 4, 3: 2})
        m._init(src, work, out, 1)
        m._base_tile.cache_clear()
        m.pad_one('MapTex_A-0.sbmaptex')
        p = m.PAD[0]
        padded = np.asarray(Image.open(os.path.join(work, 'L0', 'in', 'MapTex_A-0.png')))
        dec = {n: m.tile_rgb(*m.tile_read(os.path.join(src, n))) for n in ('MapTex_A-0.sbmaptex', 'MapTex_Z-0.sbmaptex',
                                                                              'MapTex_A-1.sbmaptex')}
        check('pad: the tile in the middle', (padded[p:-p, p:-p] == dec['MapTex_A-0.sbmaptex']).all())
        check('pad: the left border from the neighbour', (padded[p:-p, :p] == dec['MapTex_Z-0.sbmaptex'][:, -p:]).all())
        check('pad: the bottom border from the neighbour', (padded[-p:, p:-p] == dec['MapTex_A-1.sbmaptex'][:p]).all())
        check('pad: edge copies off the grid (row -1)', (padded[:p, p:-p] == dec['MapTex_A-0.sbmaptex'][:1]).all())

        # stand-ins for Real-ESRGAN's images: the padded image enlarged 2x
        names = sorted(imgs)
        for n in names:
            m.pad_one(n)
            lv = m.level_of(n)[0]
            a = Image.open(os.path.join(work, f'L{lv}', 'in', n[:-9] + '.png'))
            os.makedirs(os.path.join(work, f'L{lv}', 'x2'), exist_ok=True)
            a.resize((a.size[0] * 2, a.size[1] * 2), Image.NEAREST).save(os.path.join(work, f'L{lv}', 'x2', n[:-9] + '.png'))
        for scale in (1, 2):
            o = os.path.join(out, str(scale))
            os.makedirs(o)
            m._init(src, work, o, scale)
            m._closest_size.cache_clear()
            for n in names:
                m.encode_one(n)
            got = {n: m.tile_parse(m.yaz0_decompress(open(os.path.join(o, n), 'rb').read())) for n in names}
            want = {n: sizes[m.level_of(n)[0]] * (2 if scale == 2 and m.level_of(n)[0] < 2 else 1) for n in names}
            check(f'--scale {scale}: tile sizes', all(got[n]['w'] == got[n]['h'] == want[n] for n in names))
            n = 'MapTex2_A-1.sbmaptex'
            res = m.tile_rgb(m.yaz0_decompress(open(os.path.join(o, n), 'rb').read()), got[n])
            check(f'--scale {scale}: a tile comes back as the cleaned image', np.abs(res.astype(int) - imgs[n]).mean() < 8)
        n = 'MapTex1_A-0_4.sbmaptex'
        res = m.tile_rgb(m.yaz0_decompress(open(os.path.join(out, '2', n), 'rb').read()),
                         m.tile_parse(m.yaz0_decompress(open(os.path.join(out, '2', n), 'rb').read())))
        closest = np.asarray(Image.fromarray(imgs['MapTex_A-0_4.sbmaptex']).resize((48, 48), Image.LANCZOS)).astype(int)
        check('--scale 2: the medium level is reduced from the closest one (variants too)',
              np.abs(res.astype(int) - closest).mean() < np.abs(res.astype(int) - np.repeat(np.repeat(
                  imgs[n], 2, 0), 2, 1)).mean())
        os.remove(os.path.join(work, 'L0', 'x2', 'MapTex_Z-1.png'))
        o = os.path.join(out, 'fallback')
        os.makedirs(o)
        m._init(src, work, o, 2)
        m.encode_one('MapTex1_Z-1.sbmaptex')
        fb = m.tile_parse(m.yaz0_decompress(open(os.path.join(o, 'MapTex1_Z-1.sbmaptex'), 'rb').read()))
        check('--scale 2: without the closest level\'s image, as with --scale 1', fb['w'] == sizes[1])
        m.PAD.clear()
        m.PAD.update(old_pad)

    print('ok' if not failed else f'{failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
