#!/usr/bin/env python3
"""What tools/mods.py decides from the mods/ folder: python tools/test_mods.py"""
import json
import tempfile
from pathlib import Path

import mods


def test_plan():
    installed = {'a.bnp': {'hash': '1', 'id': 'A'}, 'b.bnp': {'hash': '2', 'id': 'B'}}
    assert mods.plan({}, {}) == ([], [])
    assert mods.plan({'a.bnp': '1', 'b.bnp': '2'}, installed) == ([], [])  # (nothing changed)
    assert mods.plan({'a.bnp': '1', 'b.bnp': '2', 'c.bnp': '3'}, installed) == (['c.bnp'], [])  # added
    assert mods.plan({'a.bnp': '1'}, installed) == ([], ['b.bnp'])  # deleted
    assert mods.plan({'a.bnp': '9', 'b.bnp': '2'}, installed) == (['a.bnp'], ['a.bnp'])  # a new version: both


def test_default_selects():
    assert mods.default_selects({}) == []
    info = {'options': {'multi': [{'folder': 'on', 'default': True}, {'folder': 'off', 'default': False}], 'single': []}}
    assert mods.default_selects(info) == ['on']
    info = {'options': {'single': [{'required': True, 'options': [{'folder': 'x'}, {'folder': 'y'}]},
                                   {'required': False, 'options': [{'folder': 'z'}]}]}}
    assert mods.default_selects(info) == ['x']


def test_find_sources():
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        assert mods.find_sources(d) == []
        (d / 'rules.txt').write_text('a Cemu graphic pack')
        assert mods.find_sources(d) == []
        # A bare romfs, as a Switch mod's folder; the folder around it is the mod
        (d / 'Cool Mod' / 'romfs' / 'Model').mkdir(parents=True)
        assert mods.find_sources(d) == [d / 'Cool Mod']
        mods.as_mod_folder(d / 'Cool Mod', 'Cool Mod')
        assert (d / 'Cool Mod' / mods.GAME_ID / 'romfs' / 'Model').is_dir()
        assert (d / 'Cool Mod' / 'info.json').exists()
        # What the game then installs from it is that folder, by its info.json
        assert mods.find_sources(d) == [d / 'Cool Mod']
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        # Atmosphere's layout: the title's folder holds the romfs
        (d / 'atmosphere' / 'contents' / mods.GAME_ID / 'romfs').mkdir(parents=True)
        assert mods.find_sources(d) == [d / 'atmosphere' / 'contents']
        # A BNP inside comes first
        (d / 'b.bnp').write_bytes(b'')
        (d / 'sub').mkdir()
        (d / 'sub' / 'a.bnp').write_bytes(b'')
        assert mods.find_sources(d) == [d / 'b.bnp', d / 'sub' / 'a.bnp']


def test_loose_romfs():
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        (d / 'Hair' / 'Actor' / 'Pack').mkdir(parents=True)
        (d / 'Hair' / 'Pack').mkdir()
        (d / 'Hair' / 'readme').mkdir()
        assert mods.find_sources(d) == [d / 'Hair']
        mods.as_mod_folder(d / 'Hair', 'Hair')
        assert (d / 'Hair' / mods.GAME_ID / 'romfs' / 'Actor' / 'Pack').is_dir()
        assert (d / 'Hair' / mods.GAME_ID / 'romfs' / 'Pack').is_dir()
        assert (d / 'Hair' / 'readme').is_dir()
        assert json.loads((d / 'Hair' / 'info.json').read_text())['name'] == 'Hair'


def test_text_hashing():
    import xxhash
    expected = xxhash.xxh64_hexdigest(b'abc')
    mods.text_hashing()
    assert xxhash.xxh64_hexdigest('abc') == expected
    assert xxhash.xxh64_hexdigest(b'abc') == expected
    assert xxhash.xxh64_intdigest('abc') == xxhash.xxh64_intdigest(b'abc')


def test_keep_game_file():
    try:
        import oead
    except ImportError:  # (only in BCML's environment)
        return

    def pack(files):
        w = oead.SarcWriter()
        for name, data in files.items():
            w.files[name] = data
        return bytes(w.write()[1])
    game = pack({'Layout/Common.sblarc': b'1.9', 'GameData/gamedata.ssarc': b'game'})
    mod = pack({'Layout/Common.sblarc': b'1.6', 'GameData/gamedata.ssarc': b'merged'})
    kept = oead.Sarc(mods.keep_game_file(mod, game, 'Layout/Common.sblarc'))
    assert bytes(kept.get_file('Layout/Common.sblarc').data) == b'1.9'
    assert bytes(kept.get_file('GameData/gamedata.ssarc').data) == b'merged'  # (the rest of the mods stays)
    assert mods.keep_game_file(game, game, 'Layout/Common.sblarc') is None


def test_ids_of():
    assert mods.ids_of({'id': 'A'}) == ['A']  # (as older runs wrote it)
    assert mods.ids_of({'ids': ['A', 'B']}) == ['A', 'B']


if __name__ == '__main__':
    test_plan()
    test_find_sources()
    test_loose_romfs()
    test_ids_of()
    test_default_selects()
    test_text_hashing()
    test_keep_game_file()
    print('PASS: mods')
