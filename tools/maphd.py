#!/usr/bin/env python3
"""A sharper world map, made from your own game files: every map tile cleaned with Real-ESRGAN, as a Savage Wind mod.

    python3 tools/maphd.py <game files folder> <mod folder> --esrgan <realesrgan-ncnn-vulkan> [--scale 1|2]
                           [--work <folder>] [--jobs <n>]

    <game files folder>  the folder with romfs/: the one Savage Wind uses
    <mod folder>         where the mod is made, for example mods/MapHD in Savage Wind's data folder; it gets
                         romfs/UI/MapTex/MainField/ with the map's tiles
    --esrgan             realesrgan-ncnn-vulkan (https://github.com/xinntao/Real-ESRGAN/releases, the "ncnn
                         Vulkan" download), with its models/ folder next to it: realesr-animevideov3-x2 is used
    --scale 1            (default) each tile cleaned and kept at its own size: the map's blocky compression and
                         soft edges cleaned up, the same files the game reads
    --scale 2            the two closest zoom levels stored at twice the size, for windows above 1080p (see below)
    --work               where the in-between images go (default: <mod folder>-work, about 20 GB). A stopped run
                         goes on where it was; delete the folder afterwards
    --jobs               processes for decoding and encoding (default: half the CPU threads)

The map's tiles (UI/MapTex/MainField/MapTex{,1,2,3}_*.sbmaptex: four zoom levels, 586 tiles each, BC1 textures)
are decoded with a border taken from their neighbours, so the seams match; Real-ESRGAN (realesr-animevideov3, 2x)
cleans each level separately (the coarser levels are drawn separately, with fewer contour lines, not reduced copies);
each tile is then scaled back, cropped and compressed to BC1 again.

--scale 2: the game draws the two closest zoom levels at one tile texel per pixel of a 1080-line picture, so on a
4K window each texel covers 2x2 pixels. Stored at twice the size, the closest level (3000 -> 6000 pixels a tile) is
Real-ESRGAN's image itself and the medium level (840 -> 1680) is reduced from it (the detailed drawing, not the
game's simpler one at that zoom); the two coarsest levels are as with --scale 1. This only shows more detail if the
map is also drawn at the window's resolution (Savage Wind #1137); at 1080 lines the bigger tiles are reduced back.
The bigger tiles take about 11 GB more on disk; their ResourceSizeTable entries are grown by Savage Wind itself.

Needs numpy and Pillow (python3 -m pip install numpy pillow) and a GPU Real-ESRGAN can use; the whole map takes
hours (the 2x images of the closest level are the most). The mod is made from your game files: it holds the map's
textures, changed, and must not be shared. Nothing is downloaded and nothing outside the mod and work folders is
written.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from functools import lru_cache

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None  # (a padded closest-level tile at 2x is 6256 x 6256)

TILES = 'UI/MapTex/MainField'
COLS = 'ZABCDEFGHIJK'  # (the grid: Z, A..K left to right, 0..9 top to bottom)
PAD = {0: 64, 1: 24, 2: 12, 3: 8}  # border from the neighbours, per level (pixels at the tile's own size)
NAME = re.compile(r'^MapTex(\d?)_([A-Z])-(\d+)([-_].*)?\.sbmaptex$')
MODEL = 'realesr-animevideov3'
BC1_SRGB, BC1_UNORM = 0x1a06, 0x1a01


def level_of(f):
    m = NAME.match(f)
    return int(m.group(1) or 0), m.group(2), int(m.group(3))


# ---- Yaz0 -------------------------------------------------------------------------------------------------------

def yaz0_decompress(d):
    if d[:4] != b'Yaz0':
        return d
    size, = struct.unpack_from('>I', d, 4)
    out = bytearray(size)
    i, o = 16, 0
    while o < size:
        code = d[i]
        i += 1
        for bit in range(8):
            if o >= size:
                break
            if code & (0x80 >> bit):
                out[o] = d[i]
                i += 1
                o += 1
            else:
                b1, b2 = d[i], d[i + 1]
                i += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = d[i] + 0x12
                    i += 1
                else:
                    n += 2
                s = o - dist
                if dist >= n:
                    out[o:o + n] = out[s:s + n]
                else:  # (overlapping: a run)
                    for k in range(n):
                        out[o + k] = out[s + k]
                o += n
    return bytes(out)


def yaz0_store(d):
    """Yaz0 without compression (every byte a literal): the game reads it like any other."""
    out = bytearray(b'Yaz0' + struct.pack('>I', len(d)) + bytes(8))
    for i in range(0, len(d), 8):
        chunk = d[i:i + 8]
        out.append((0xFF00 >> len(chunk)) & 0xFF)
        out += chunk
    return bytes(out)


# ---- a tile: BFRES with one embedded BNTX holding one BC1 texture -------------------------------------------------

def tile_read(path):
    with open(path, 'rb') as f:
        d = yaz0_decompress(f.read())
    return d, tile_parse(d)


def tile_parse(d):
    b = d.find(b'BNTX')
    if b < 0 or struct.unpack_from('<I', d, b + 0x24)[0] != 1:
        raise ValueError('not a map tile (one BNTX texture expected)')
    r = d.find(b'BRTI', b)
    mips, = struct.unpack_from('<H', d, r + 0x16)
    fmt, = struct.unpack_from('<I', d, r + 0x1c)
    w, h = struct.unpack_from('<II', d, r + 0x24)
    th, = struct.unpack_from('<I', d, r + 0x34)
    size, = struct.unpack_from('<I', d, r + 0x50)
    brtd, = struct.unpack_from('<Q', d, b + 0x30)
    if mips != 1 or fmt not in (BC1_SRGB, BC1_UNORM) or d[b + brtd:b + brtd + 4] != b'BRTD':
        raise ValueError(f'not a map tile (mips {mips}, format {fmt:#x})')
    if _geom(w, h, th)[-1] != size:
        raise ValueError(f'map tile: {w}x{h} block height {th} does not take {size} bytes')
    return dict(w=w, h=h, th=th, size=size, data=b + brtd + 0x10, fmt=fmt)


# Block-linear layout (GOB = 64 bytes x 8 rows; blocks of 1 << th GOBs high) of rows of 8-byte BC1 blocks
def _geom(w, h, th):
    row_bytes, rows = (w + 3) // 4 * 8, (h + 3) // 4
    gobs_x, block = (row_bytes + 63) // 64, 512 << th
    blocks_y = (rows + (8 << th) - 1) // (8 << th)
    return row_bytes, rows, gobs_x, block, gobs_x * blocks_y * block


def _offsets(w, h, th):
    """The offset in the block-linear data of every 16-byte piece of each linear row (rows x pieces)."""
    row_bytes, rows, gobs_x, block, _ = _geom(w, h, th)
    y = np.arange(rows)[:, None]
    x = np.arange(0, (row_bytes + 15) // 16 * 16, 16)[None, :]
    bh = 1 << th
    g = (y // (8 * bh)) * gobs_x * block + ((y % (8 * bh)) // 8) * 512 + (y % 8 // 2) * 64 + (y % 2) * 16
    return g + (x // 64) * block + np.array([0, 32, 256, 288])[(x % 64) // 16]


def unswizzle(raw, w, h, th):
    row_bytes, rows, *_ = _geom(w, h, th)
    off = _offsets(w, h, th)
    src = np.frombuffer(raw, np.uint8)
    pieces = src[(off[..., None] + np.arange(16)).clip(max=len(src) - 1)]
    return pieces.reshape(rows, -1)[:, :row_bytes].copy()


def swizzle(lin, w, h, th):
    row_bytes, rows, _, _, size = _geom(w, h, th)
    off = _offsets(w, h, th)
    pad = np.zeros((rows, off.shape[1] * 16), np.uint8)
    pad[:, :row_bytes] = lin
    out = np.zeros(size + 16, np.uint8)
    idx = (off[..., None] + np.arange(16)).reshape(rows, -1)
    keep = np.arange(pad.shape[1]) < row_bytes  # (pieces past the row are padding)
    out[idx[:, keep]] = pad[:, keep]
    return out[:size].tobytes()


def tile_rgb(d, info):
    raw = d[info['data']:info['data'] + info['size']]
    return bc1_decode(unswizzle(raw, info['w'], info['h'], info['th']), info['w'], info['h'])[..., :3]


def tile_with(d, info, rgb):
    """The tile with its texture replaced by rgb (H x W x 3 uint8). The same size keeps the file's length; a bigger
    one grows the BNTX and, with it, the BFRES around it (its embedded-file entry, the relocation table's section
    over the BNTX, its file size)."""
    h, w = rgb.shape[:2]
    rgba = np.concatenate([rgb, np.full((h, w, 1), 255, np.uint8)], -1)
    data = swizzle(bc1_encode(rgba), w, h, info['th'])
    if (w, h) == (info['w'], info['h']):
        return d[:info['data']] + data + d[info['data'] + info['size']:]
    b = d.find(b'BNTX')
    bntx_size, = struct.unpack_from('<I', d, b + 0x1c)
    x = bytearray(d[b:b + bntx_size])
    brti = x.find(b'BRTI')
    brtd, = struct.unpack_from('<Q', x, 0x30)
    rlt_old, = struct.unpack_from('<I', x, 0x18)
    if rlt_old != brtd + 0x10 + info['size']:
        raise ValueError('map tile: the BNTX is not laid out as expected')
    rlt = bytearray(x[rlt_old:])
    head = x[:brtd + 0x10]
    struct.pack_into('<II', head, brti + 0x24, w, h)
    struct.pack_into('<I', head, brti + 0x50, len(data))
    struct.pack_into('<Q', head, brtd + 8, len(data) + 0x10)
    new_rlt = brtd + 0x10 + len(data)
    struct.pack_into('<II', head, 0x18, new_rlt, new_rlt + len(rlt))
    struct.pack_into('<I', rlt, 4, new_rlt)
    if struct.unpack_from('<I', rlt, 0x34)[0] != info['size'] + 0x10:
        raise ValueError('map tile: the BNTX relocation table is not laid out as expected')
    struct.pack_into('<I', rlt, 0x34, len(data) + 0x10)
    nb = bytes(head) + data + bytes(rlt)
    f_rlt, = struct.unpack_from('<I', d, 0x18)
    if struct.unpack_from('<QQ', d, 0xf0) != (b, bntx_size) or f_rlt != (b + bntx_size + 0xfff) & ~0xfff:
        raise ValueError('map tile: the BFRES is not laid out as expected')
    frlt = bytearray(d[f_rlt:])
    new_f_rlt = (b + len(nb) + 0xfff) & ~0xfff
    n_sec, = struct.unpack_from('<I', frlt, 8)
    for k in range(n_sec):
        o = 0x10 + k * 0x18
        off, size = struct.unpack_from('<II', frlt, o + 8)
        if off == b and off + size == f_rlt:
            struct.pack_into('<I', frlt, o + 12, new_f_rlt - b)
    struct.pack_into('<I', frlt, 4, new_f_rlt)
    out = bytearray(d[:b]) + nb + bytes(new_f_rlt - b - len(nb)) + frlt
    struct.pack_into('<II', out, 0x18, new_f_rlt, len(out))
    struct.pack_into('<Q', out, 0xf8, len(nb))
    out = bytes(out)
    p = tile_parse(out)
    if (p['w'], p['h'], p['size']) != (w, h, len(data)):
        raise ValueError('map tile: the bigger tile does not read back')
    return out


# ---- BC1 --------------------------------------------------------------------------------------------------------

def _565(c):
    c = c.astype(np.uint32)
    r, g, b = c >> 11 & 31, c >> 5 & 63, c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)


def _palette(c0, c1):
    """N -> N x 4 x 4 (RGBA) palettes, both BC1 modes."""
    a, b = _565(c0), _565(c1)
    four = (c0 > c1)[:, None]
    p2 = np.where(four, (2 * a + b) / 3, (a + b) / 2)
    p3 = np.where(four, (a + 2 * b) / 3, 0)
    pal = np.stack([a, b, p2, p3], 1)
    alpha = np.full(pal.shape[:2] + (1,), 255, np.float32)
    alpha[:, 3, 0] = np.where(four[:, 0], 255, 0)
    return np.concatenate([pal, alpha], -1)


def bc1_decode(lin, w, h):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    blk = lin.reshape(bh * bw, 8)
    c0 = blk[:, 0].astype(np.uint16) | blk[:, 1].astype(np.uint16) << 8
    c1 = blk[:, 2].astype(np.uint16) | blk[:, 3].astype(np.uint16) << 8
    bits = blk[:, 4:8].copy().view('<u4')[:, 0]
    idx = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    px = np.take_along_axis(_palette(c0, c1), idx[..., None].astype(np.int64), 1)  # N x 16 x 4
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return np.rint(img[:h, :w]).astype(np.uint8)


_W = np.array([0.299, 0.587, 0.114], np.float32) * 3  # (perceptual error weights)


def _to565(c):
    c = np.clip(np.rint(c), 0, 255)
    r = np.rint(c[..., 0] * 31 / 255).astype(np.uint16)
    g = np.rint(c[..., 1] * 63 / 255).astype(np.uint16)
    b = np.rint(c[..., 2] * 31 / 255).astype(np.uint16)
    return r << 11 | g << 5 | b


def _fit(px, c0, c1):
    """Indices and weighted error of N x 16 x 3 opaque pixels against endpoints c0 / c1 (565)."""
    pal = _palette(c0, c1)
    d = (((px[:, :, None, :] - pal[:, None, :, :3]) ** 2) * _W).sum(-1)  # N x 16 x 4
    d[..., 3] = np.where((c0 > c1)[:, None], d[..., 3], np.inf)  # (index 3 is transparent in 3-colour mode)
    idx = d.argmin(-1)
    return idx, np.take_along_axis(d, idx[..., None], -1)[..., 0].sum(-1)


def _refine(px, idx, four):
    """Least-squares endpoints for fixed indices (the weights along c0..c1 each index stands for)."""
    w4 = np.array([[1, 0], [0, 1], [2 / 3, 1 / 3], [1 / 3, 2 / 3]], np.float32)
    w3 = np.array([[1, 0], [0, 1], [.5, .5], [0, 0]], np.float32)
    wt = np.where(four[:, None, None], w4[idx], w3[idx])  # N x 16 x 2
    ata = np.einsum('nki,nkj->nij', wt, wt) + np.eye(2, dtype=np.float32) * 1e-4
    sol = np.linalg.solve(ata, np.einsum('nki,nkc->nic', wt, px))  # N x 2 x 3
    return sol[:, 0], sol[:, 1]


def _single_tables():
    """Per channel (5 and 6 bits): for each value 0..255 the endpoints whose 2/3 mix comes closest, as close
    together as possible (a flat colour stays flat however the GPU rounds)."""
    out = {}
    for bits in (5, 6):
        e = np.arange(1 << bits)
        x = ((e << (8 - bits)) | (e >> (2 * bits - 8))).astype(np.float32)
        mix = (2 * x[:, None] + x[None, :]) / 3
        cost = np.abs(mix[None] - np.arange(256, dtype=np.float32)[:, None, None]) * 1000 + \
            np.abs(x[:, None] - x[None, :])[None]
        flat = cost.reshape(256, -1).argmin(1)
        out[bits] = np.stack([flat // (1 << bits), flat % (1 << bits)], 1).astype(np.uint16)
    return out


_SINGLE = _single_tables()


def _single_colour(mean):
    v = np.clip(np.rint(mean), 0, 255).astype(np.int64)
    r, g, b = _SINGLE[5][v[:, 0]], _SINGLE[6][v[:, 1]], _SINGLE[5][v[:, 2]]
    c0 = r[:, 0] << 11 | g[:, 0] << 5 | b[:, 0]
    c1 = r[:, 1] << 11 | g[:, 1] << 5 | b[:, 1]
    return np.maximum(c0, c1), np.minimum(c0, c1)


def bc1_encode(rgba, refine=2, chunk=32768):
    """H x W x 4 uint8 (opaque) -> rows of BC1 blocks (bh x bw * 8), in chunks of blocks to bound memory."""
    h, w = rgba.shape[:2]
    bw, bh = (w + 3) // 4, (h + 3) // 4
    img = np.zeros((bh * 4, bw * 4, 3), np.float32)
    img[:h, :w] = rgba[..., :3]
    if h % 4:
        img[h:, :w] = img[h - 1:h, :w]  # (the edge repeated into the padding)
    if w % 4:
        img[:, w:] = img[:, w - 1:w]
    blk = img.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3)
    out = np.concatenate([_bc1_blocks(blk[i:i + chunk], refine) for i in range(0, len(blk), chunk)])
    return out.reshape(bh, bw * 8)


def _bc1_blocks(px, refine):
    # the principal axis (power iteration on the covariance), endpoints at the extreme projections, inset a little
    mean = px.mean(1, keepdims=True)
    cen = px - mean
    cov = np.einsum('nki,nkj->nij', cen, cen)
    v = np.ones((len(px), 3), np.float32)
    for _ in range(8):
        v = np.einsum('nij,nj->ni', cov, v)
        v /= np.linalg.norm(v, axis=-1, keepdims=True) + 1e-9
    t = np.einsum('nki,ni->nk', cen, v)
    lo, hi = t.min(1), t.max(1)
    inset = (hi - lo) / 32
    c0 = _to565(mean[:, 0] + v * (hi - inset)[:, None])
    c1 = _to565(mean[:, 0] + v * (lo + inset)[:, None])
    best_c0, best_c1 = np.maximum(c0, c1), np.minimum(c0, c1)
    best_idx, best_err = _fit(px, best_c0, best_c1)

    def keep(c0, c1):
        nonlocal best_c0, best_c1, best_idx, best_err
        idx, err = _fit(px, c0, c1)
        better = err < best_err
        best_c0, best_c1 = np.where(better, c0, best_c0), np.where(better, c1, best_c1)
        best_idx = np.where(better[:, None], idx, best_idx)
        best_err = np.where(better, err, best_err)

    keep(*_single_colour(mean[:, 0]))  # (flat and nearly flat blocks keep their exact hue)
    for _ in range(refine):
        for four_mode in (True, False):
            four = np.full(len(px), four_mode)
            a, b = _refine(px, best_idx, four)
            c0, c1 = _to565(a), _to565(b)
            hi_, lo_ = np.maximum(c0, c1), np.minimum(c0, c1)
            keep(np.where(four, hi_, lo_), np.where(four, lo_, hi_))
    bits = (best_idx.astype(np.uint32) << (2 * np.arange(16, dtype=np.uint32))).sum(-1).astype(np.uint32)
    out = np.zeros((len(px), 8), np.uint8)
    out[:, 0], out[:, 1] = best_c0 & 0xff, best_c0 >> 8
    out[:, 2], out[:, 3] = best_c1 & 0xff, best_c1 >> 8
    out[:, 4:8] = bits.view(np.uint8).reshape(-1, 4)
    return out


# ---- the steps ---------------------------------------------------------------------------------------------------

SRC = WORK = OUT = None
SCALE = 1


def _init(src, work, out, scale):
    global SRC, WORK, OUT, SCALE
    SRC, WORK, OUT, SCALE = src, work, out, scale


@lru_cache(maxsize=24)
def _base_tile(level, col, row):
    """The base (no variant) tile at a grid position, RGB, or None off the grid."""
    if col not in COLS or row < 0:
        return None
    p = os.path.join(SRC, f"MapTex{level or ''}_{col}-{row}.sbmaptex")
    if not os.path.exists(p):
        return None
    return tile_rgb(*tile_read(p))


def pad_one(f):
    """The tile decoded with a border from its neighbours (the base tiles of the same level) -> L<n>/in/<name>.png"""
    level, col, row = level_of(f)
    dst = os.path.join(WORK, f'L{level}', 'in', f[:-9] + '.png')
    if os.path.exists(dst):
        return
    img = tile_rgb(*tile_read(os.path.join(SRC, f)))
    p, h, w = PAD[level], img.shape[0], img.shape[1]
    big = np.pad(img, ((p, p), (p, p), (0, 0)), mode='edge')  # (edge copies where nothing is next to it)
    ci = COLS.index(col)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dx == dy == 0 or not 0 <= ci + dx < len(COLS):
                continue
            n = _base_tile(level, COLS[ci + dx], row + dy)
            if n is None or n.shape != img.shape:
                continue
            ys = slice(0, p) if dy < 0 else slice(p + h, p + h + p) if dy > 0 else slice(p, p + h)
            xs = slice(0, p) if dx < 0 else slice(p + w, p + w + p) if dx > 0 else slice(p, p + w)
            ny = slice(h - p, h) if dy < 0 else slice(0, p) if dy > 0 else slice(0, h)
            nx = slice(w - p, w) if dx < 0 else slice(0, p) if dx > 0 else slice(0, w)
            big[ys, xs] = n[ny, nx]
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    Image.fromarray(big).save(dst + '.tmp.png', compress_level=1)
    os.replace(dst + '.tmp.png', dst)


def esrgan(exe, level):
    """One Real-ESRGAN run over the level's tiles not cleaned yet: L<n>/in -> L<n>/x2"""
    src, dst = os.path.join(WORK, f'L{level}', 'in'), os.path.join(WORK, f'L{level}', 'x2')
    if not os.path.isdir(src):
        return
    os.makedirs(dst, exist_ok=True)
    todo = [f for f in os.listdir(src) if f.endswith('.png') and not os.path.exists(os.path.join(dst, f))]
    if not todo:
        return
    left = os.path.join(WORK, f'L{level}', 'todo')  # (Real-ESRGAN takes a folder: one with only what is left)
    shutil.rmtree(left, ignore_errors=True)
    os.makedirs(left)
    for f in todo:
        try:
            os.link(os.path.join(src, f), os.path.join(left, f))
        except OSError:
            shutil.copyfile(os.path.join(src, f), os.path.join(left, f))
    t = time.time()
    subprocess.run([exe, '-i', left, '-o', dst, '-n', MODEL, '-s', '2', '-f', 'png'],
                   cwd=os.path.dirname(os.path.abspath(exe)), check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    shutil.rmtree(left, ignore_errors=True)
    print(f'  Real-ESRGAN, level {level}: {len(todo)} tiles, {time.time() - t:.0f} s', flush=True)


def _x2(level, name, h, w):
    """Real-ESRGAN's image of a tile, its border cropped: (2h x 2w x 3)"""
    p = PAD[level]
    x2 = Image.open(os.path.join(WORK, f'L{level}', 'x2', name + '.png')).convert('RGB')
    if x2.size != ((w + 2 * p) * 2, (h + 2 * p) * 2):
        raise ValueError(f'{name}: Real-ESRGAN image is {x2.size[0]}x{x2.size[1]}')
    return x2, p


def encode_one(f):
    """The cleaned tile, compressed to BC1 again -> <mod>/romfs/UI/MapTex/MainField/<name>"""
    level, _, _ = level_of(f)
    dst = os.path.join(OUT, f)
    if os.path.exists(dst):
        return
    d, info = tile_read(os.path.join(SRC, f))
    h, w = info['h'], info['w']
    rest = f[len('MapTex' + (str(level) if level else '')):-9]  # ('_A-1_0': the same tile at every level)
    closest = os.path.join(WORK, 'L0', 'x2', 'MapTex' + rest + '.png')
    if SCALE == 2 and level in (0, 1) and os.path.exists(closest):  # (else: as with --scale 1)
        x2, p = _x2(0, 'MapTex' + rest, *((info['h'], info['w']) if level == 0 else _closest_size()))
        img = x2.crop((2 * p, 2 * p, x2.size[0] - 2 * p, x2.size[1] - 2 * p))
        if level == 1:
            img = img.resize((w * 2, h * 2), Image.LANCZOS)
        rgb = np.asarray(img)
    else:
        x2, p = _x2(level, f[:-9], h, w)
        rgb = np.asarray(x2.resize((w + 2 * p, h + 2 * p), Image.LANCZOS))[p:p + h, p:p + w]
    out = tile_with(d, info, np.ascontiguousarray(rgb))
    with open(dst + '.tmp', 'wb') as fh:
        fh.write(yaz0_store(out))
    os.replace(dst + '.tmp', dst)


@lru_cache(maxsize=1)
def _closest_size():
    f = next(f for f in sorted(os.listdir(SRC)) if NAME.match(f) and level_of(f)[0] == 0)
    info = tile_read(os.path.join(SRC, f))[1]
    return info['h'], info['w']


def run(fn, files, jobs, what):
    t = time.time()
    with ProcessPoolExecutor(jobs, initializer=_init, initargs=(SRC, WORK, OUT, SCALE)) as ex:
        futures = {ex.submit(fn, f): f for f in files}
        for n, fut in enumerate(as_completed(futures), 1):
            fut.result()  # (an error stops the run; what is done stays done)
            if n % 100 == 0 or n == len(files):
                print(f'  {what}: {n}/{len(files)}, {time.time() - t:.0f} s', flush=True)


def main(argv):
    opts, args, i = {}, [], 0
    while i < len(argv):
        if argv[i] in ('--esrgan', '--scale', '--work', '--jobs') and i + 1 < len(argv):
            opts[argv[i]] = argv[i + 1]
            i += 2
        elif argv[i].startswith('--'):
            sys.exit(__doc__)
        else:
            args.append(argv[i])
            i += 1
    if len(args) != 2 or '--esrgan' not in opts or opts.get('--scale', '1') not in ('1', '2'):
        sys.exit(__doc__)
    game, mod = (os.path.abspath(a) for a in args)
    exe = opts['--esrgan']
    if not os.path.isfile(exe):
        sys.exit(f'{exe}: not found (realesrgan-ncnn-vulkan)')
    if not os.path.isfile(os.path.join(os.path.dirname(os.path.abspath(exe)), 'models', MODEL + '-x2.param')):
        sys.exit(f'{exe}: no models/{MODEL}-x2.param next to it')
    src = os.path.join(game, 'romfs', TILES)
    if not os.path.isdir(src):
        sys.exit(f'{game}: no romfs/{TILES} (the folder with your romfs/)')
    work = os.path.abspath(opts.get('--work', mod.rstrip('/\\') + '-work'))
    scale = int(opts.get('--scale', '1'))
    out = os.path.join(mod, 'romfs', TILES)
    os.makedirs(out, exist_ok=True)
    stamp = os.path.join(mod, 'maphd-scale.txt')  # (outside romfs/: the game doesn't see it)
    if os.path.exists(stamp) and open(stamp).read() != str(scale):
        sys.exit(f'{mod} was made with another --scale: delete its romfs/ first')
    with open(stamp, 'w') as fh:
        fh.write(str(scale))
    jobs = int(opts.get('--jobs', max(1, (os.cpu_count() or 2) // 2)))
    _init(src, work, out, scale)
    files = sorted((f for f in os.listdir(src) if NAME.match(f)), key=level_of)
    print(f'{len(files)} tiles; work folder {work}', flush=True)
    run(pad_one, files, jobs, 'decoded')
    for level in range(4):
        esrgan(exe, level)
    run(encode_one, files, jobs, 'encoded')
    print(f'made {mod}: turn it on in the Mods tab. '
          f'The work folder ({work}) can be deleted.')


if __name__ == '__main__':
    main(sys.argv[1:])
