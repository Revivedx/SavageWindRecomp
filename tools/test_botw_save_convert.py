#!/usr/bin/env python3
"""Self-contained tests for botw_save_convert.py. No game files needed: the flag table is built by hand.

    python tools/test_botw_save_convert.py
"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import botw_save_convert as c  # noqa: E402

_read = c._read


def make_format(revision=0x4730):
    fmt = c.Format.__new__(c.Format)
    fmt.revision = revision
    specs = [  # name, kind, element count, InitValue
        ('Rupees', 's32_data', 1, 0),
        ('SavePos', 'vector3f_data', 1, [[1.0, 2.0, 3.0]]),
        ('Name', 'string64_data', 1, 'Link'),
        ('Open', 'bool_data', 1, 1),
        ('Respawn', 'bool_data', 1, 132),        # InitValue is not the state for these
        ('Weather', 's32_array_data', 3, [{'Values': [0, 0, 0]}]),
        ('Brand', 's32_data', 1, 7),             # only in the target revision
    ]
    fmt.flags = {}
    for name, kind, count, init in specs:
        h = c.crc32_name(name)
        base = kind.replace('_array_data', '').replace('_data', '')
        fmt.flags[h] = c.Flag(name, h, kind, c._WORDS[base] * count, init)
    fmt.files = {'game_data.sav': sorted(fmt.flags)}
    return fmt


def build(fmt, e, values, skip=(), revision=None):
    """A save file: header, entries sorted by hash, footer. `values` maps name -> list of raw 4-byte words."""
    out = struct.pack(e + 'III', revision or fmt.revision, 0xFFFFFFFF, 1)
    for h in sorted(fmt.flags):
        if fmt.flags[h].name in skip:
            continue
        for w in values[fmt.flags[h].name]:
            out += struct.pack(e + 'I', h) + w
    return out + c.FOOTER


def i32(e, v):
    return struct.pack(e + 'i', v)


def f32(e, v):
    return struct.pack(e + 'f', v)


def text(s, words):
    return s.encode().ljust(words * 4, b'\0')


def sample(e):
    return {
        'Rupees': [i32(e, 1234)],
        'SavePos': [f32(e, 10.5), f32(e, -2.0), f32(e, 300.25)],
        'Name': [text('Epona', 16)[i:i + 4] for i in range(0, 64, 4)],
        'Open': [i32(e, 1)],
        'Respawn': [i32(e, 0)],
        'Weather': [i32(e, 1), i32(e, 2), i32(e, 3)],
        'Brand': [i32(e, 99)],
    }


class ConvertTests(unittest.TestCase):
    def setUp(self):
        self.fmt = make_format()

    def run_convert(self, data, to):
        return c.convert_flags(data, self.fmt, 'game_data.sav', to, [])

    def test_detects_byte_order(self):
        self.assertEqual(c.detect_endian(build(self.fmt, '<', sample('<'))), '<')
        self.assertEqual(c.detect_endian(build(self.fmt, '>', sample('>'), revision=0x471B)), '>')

    def test_same_format_is_identity(self):
        data = build(self.fmt, '<', sample('<'))
        self.assertEqual(self.run_convert(data, '<'), data)

    def test_big_to_little(self):
        be = build(self.fmt, '>', sample('>'), revision=0x471B)
        out = self.run_convert(be, '<')
        self.assertEqual(out, build(self.fmt, '<', sample('<')))

    def test_strings_are_not_byte_swapped(self):
        be = build(self.fmt, '>', sample('>'), revision=0x471B)
        _, _, runs = c.read_entries(self.run_convert(be, '<'))
        raw = b''.join(runs[c.crc32_name('Name')])
        self.assertTrue(raw.startswith(b'Epona\0'))

    def test_floats_survive(self):
        be = build(self.fmt, '>', sample('>'))
        _, _, runs = c.read_entries(self.run_convert(be, '<'))
        self.assertEqual([struct.unpack('<f', w)[0] for w in runs[c.crc32_name('SavePos')]], [10.5, -2.0, 300.25])

    def test_missing_flag_gets_default_and_header_uses_target_revision(self):
        be = build(self.fmt, '>', sample('>'), skip=('Brand',), revision=0x471B)
        out = self.run_convert(be, '<')
        self.assertEqual(struct.unpack_from('<I', out, 0)[0], 0x4730)
        _, _, runs = c.read_entries(out)
        self.assertEqual(struct.unpack('<i', runs[c.crc32_name('Brand')][0])[0], 7)

    def test_bool_default_ignores_respawn_numbers(self):
        be = build(self.fmt, '>', sample('>'), skip=('Open', 'Respawn'))
        _, _, runs = c.read_entries(self.run_convert(be, '<'))
        self.assertEqual(struct.unpack('<I', runs[c.crc32_name('Open')][0])[0], 1)
        self.assertEqual(struct.unpack('<I', runs[c.crc32_name('Respawn')][0])[0], 0)

    def test_unknown_flags_are_dropped(self):
        other = make_format()
        extra = c.Flag('Gone', c.crc32_name('Gone'), 's32_data', 1, 0)
        other.flags[extra.hash] = extra
        src = build(other, '>', dict(sample('>'), Gone=[i32('>', 5)]))
        _, _, runs = c.read_entries(self.run_convert(src, '<'))
        self.assertNotIn(extra.hash, runs)

    def test_array_length_change_is_padded_with_defaults(self):
        longer = make_format()
        flag = next(f for f in longer.flags.values() if f.name == 'Weather')
        flag.words = 2
        src = build(longer, '>', sample('>') | {'Weather': [i32('>', 1), i32('>', 2)]})
        _, _, runs = c.read_entries(self.run_convert(src, '<'))
        self.assertEqual([struct.unpack('<i', w)[0] for w in runs[c.crc32_name('Weather')]], [1, 2, 0])

    def test_round_trip_through_the_other_console(self):
        le = build(self.fmt, '<', sample('<'))
        self.assertEqual(self.run_convert(self.run_convert(le, '>'), '<'), le)

    def test_tracker_header_and_body(self):
        be = bytes([0, 0x20, 0x2F, 0, 0x01, 0x2C, 0, 0, 0, 0, 0, 0x11]) + bytes([1, 2, 3, 4])
        le = bytes([0, 0x20, 0x2F, 0, 0x2C, 0x01, 0, 0, 0x11, 0, 0, 0]) + bytes([4, 3, 2, 1])
        self.assertEqual(c.convert_tracker(be), le)
        self.assertEqual(c.convert_tracker(le), be)

    def test_tracker_only_swaps_when_the_byte_order_changes(self):
        be = bytes([0, 0x20, 0x2F, 0, 0x01, 0x2C, 0, 0, 0, 0, 0, 0x11]) + bytes([1, 2, 3, 4])
        le = c.convert_tracker(be)
        for src_e, to_e, track, want in (('>', '<', be, le), ('<', '>', le, be), ('<', '<', le, le), ('>', '>', be, be)):
            with tempfile.TemporaryDirectory() as tmp:
                os.makedirs(os.path.join(tmp, 'src', '0'))
                os.makedirs(os.path.join(tmp, 'src', 'tracker'))
                with open(os.path.join(tmp, 'src', '0', 'game_data.sav'), 'wb') as f:
                    f.write(build(self.fmt, src_e, sample(src_e)))
                with open(os.path.join(tmp, 'src', 'tracker', 'trackblock00.sav'), 'wb') as f:
                    f.write(track)
                _, out = c.convert_profile(os.path.join(tmp, 'src'), os.path.join(tmp, 'dst'), self.fmt, to_e,
                                           None, 'convert', False, dry_run=True)
                self.assertEqual(out['tracker/trackblock00.sav'], want, (src_e, to_e))

    def test_rejects_garbage(self):
        with self.assertRaises(ValueError):
            c.read_entries(b'\x00' * 40)
        with self.assertRaises(ValueError):
            c.detect_endian(b'\x01\x02')

    def test_folder_conversion_never_overwrites(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, 'src', '0')
            os.makedirs(src)
            with open(os.path.join(src, 'game_data.sav'), 'wb') as f:
                f.write(build(self.fmt, '>', sample('>')))
            dst = os.path.join(tmp, 'dst')
            c.convert_profile(os.path.join(tmp, 'src'), dst, self.fmt, '<', None, 'skip', False)
            path = os.path.join(dst, '0', 'game_data.sav')
            first = _read(path)
            with open(path, 'wb') as f:
                f.write(b'mine')
            report, _ = c.convert_profile(os.path.join(tmp, 'src'), dst, self.fmt, '<', None, 'skip', False)
            self.assertEqual(_read(path), b'mine')
            self.assertTrue(any('skipped' in line for line in report))
            c.convert_profile(os.path.join(tmp, 'src'), dst, self.fmt, '<', None, 'skip', True)
            self.assertEqual(_read(path), first)
            self.assertTrue(any(n.startswith('game_data.sav.bak-') for n in os.listdir(os.path.dirname(path))))


if __name__ == '__main__':
    unittest.main()
