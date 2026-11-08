#!/usr/bin/env python3
"""Read the game's own start positions (StartPos entries of Map/MainField/Static.smubin in Bootup.pack) for `goto <place>`.

A start position is where the game itself puts Link, on the ground and in the open. The map markers of the ZeldaMods data
(tools/gen_places.py) are not: they are the middle of a village or a floating icon, and Link landed in a wall, under the
terrain or high in the air at some of them. tools/gen_places.py uses the positions from here for every place that has one.

This needs the game files (your own dump, extracted with the setup scripts); only the few coordinates it needs are written to
tools/places_startpos.json, which is committed. Run it only when gen_places.py wants more names:
  python3 tools/dump_startpos.py [--pack extracted/romfs/Pack/Bootup.pack] [--out tools/places_startpos.json]
"""
import argparse
import json
import struct
import sys

from gen_places import START_NAMES


def yaz0(d):
    assert d[:4] == b"Yaz0"
    n = struct.unpack(">I", d[4:8])[0]
    out = bytearray()
    i = 16
    while len(out) < n:
        code = d[i]; i += 1
        for b in range(8):
            if len(out) >= n:
                break
            if code & (0x80 >> b):
                out.append(d[i]); i += 1
            else:
                b1, b2 = d[i], d[i + 1]; i += 2
                dist = ((b1 & 15) << 8 | b2) + 1
                ln = b1 >> 4
                if ln == 0:
                    ln = d[i] + 18; i += 1
                else:
                    ln += 2
                for _ in range(ln):
                    out.append(out[-dist])
    return bytes(out)


def sarc(d):
    bom = d[6:8]
    e = "<" if bom == b"\xff\xfe" else ">"
    hdr, _, fsize, doff = struct.unpack(e + "HHII", d[4:16])
    assert d[0x14:0x18] == b"SFAT"
    cnt = struct.unpack(e + "H", d[0x1a:0x1c])[0]
    nodes = []
    for k in range(cnt):
        h, attr, s, en = struct.unpack(e + "IIII", d[0x20 + 16 * k:0x30 + 16 * k])
        nodes.append((attr, s, en))
    sfnt = 0x20 + 16 * cnt
    assert d[sfnt:sfnt + 4] == b"SFNT"
    nbase = sfnt + 8
    files = {}
    for attr, s, en in nodes:
        o = nbase + (attr & 0xFFFFFF) * 4
        name = d[o:d.index(b"\0", o)].decode()
        files[name] = d[doff + s:doff + en]
    return files


def byml(d):
    e = "<" if d[:2] == b"YB" else ">"
    _, nt, st, root = struct.unpack(e + "HIII", d[2:16])

    def table(off):
        assert d[off] == 0xC2
        n = int.from_bytes(d[off + 1:off + 4], "little" if e == "<" else "big")
        offs = struct.unpack(e + "%dI" % (n + 1), d[off + 4:off + 8 + 4 * n])
        return [d[off + offs[i]:d.index(b"\0", off + offs[i])].decode() for i in range(n)]

    names = table(nt) if nt else []
    strs = table(st) if st else []

    def val(t, v, o_for_inline=None):
        if t == 0xA0: return strs[v]
        if t == 0xC1 or t == 0xC0: return node(v)
        if t == 0xD0: return bool(v)
        if t == 0xD1: return struct.unpack(e + "i", struct.pack(e + "I", v))[0]
        if t == 0xD2: return struct.unpack(e + "f", struct.pack(e + "I", v))[0]
        if t == 0xD3: return v
        if t == 0xD4: return struct.unpack(e + "q", d[v:v + 8])[0]
        if t == 0xD5: return struct.unpack(e + "Q", d[v:v + 8])[0]
        if t == 0xD6: return struct.unpack(e + "d", d[v:v + 8])[0]
        if t == 0xFF: return None
        raise ValueError(hex(t))

    def node(off):
        t = d[off]
        n = int.from_bytes(d[off + 1:off + 4], "little" if e == "<" else "big")
        if t == 0xC0:
            tp = d[off + 4:off + 4 + n]
            vo = off + 4 + ((n + 3) & ~3)
            return [val(tp[i], struct.unpack(e + "I", d[vo + 4 * i:vo + 4 * i + 4])[0]) for i in range(n)]
        if t == 0xC1:
            r = {}
            for i in range(n):
                p = off + 4 + 8 * i
                ni = int.from_bytes(d[p:p + 3], "little" if e == "<" else "big")
                tt = d[p + 3]
                v = struct.unpack(e + "I", d[p + 4:p + 8])[0]
                r[names[ni]] = val(tt, v)
            return r
        raise ValueError(hex(t))

    return node(root)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", default="extracted/romfs/Pack/Bootup.pack")
    ap.add_argument("--out", default="tools/places_startpos.json")
    args = ap.parse_args()
    files = sarc(open(args.pack, "rb").read())
    data = files["Map/MainField/Static.smubin"]
    if data[:4] == b"Yaz0":
        data = yaz0(data)
    found = {}
    for e in byml(data)["StartPos"]:
        n, t = e.get("PosName"), e["Translate"]
        if n in START_NAMES and n not in found:
            found[n] = [round(t["X"], 2), round(t["Y"], 2), round(t["Z"], 2)]
    missing = sorted(START_NAMES - set(found))
    if missing:
        print("not in the game files:", ", ".join(missing), file=sys.stderr)
    with open(args.out, "w") as f:
        json.dump(dict(sorted(found.items())), f, indent=0)
        f.write("\n")
    print("wrote %d start positions to %s" % (len(found), args.out))


if __name__ == "__main__":
    main()
