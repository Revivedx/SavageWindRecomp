"""Builds online play's RomFS overlay (docs/ONLINE.md) from the player's own game files: an actor for the other
players, OnlinePlayer - Link's model and animations on a DemoNPC (an NPC the game draws in the world and doesn't
steer), resident (in Pack/TitleBG.pack) so the runtime can create it at any time (runtime/online_actors.c).

    python tools/online_mod.py <romfs> <out>          e.g.  "E:/extracted hom/romfs"  online_mod

The output is derived from the game's files: it stays on the player's machine (the folder is git-ignored) and the
runtime serves it as the RomFS overlay (runtime/main.c: <data root>/online_mod).

From the game:
  - PauseMenuPlayer.sbactorpack: the model list (Folder Link, Unit Link), the AS list (Player_Animation) and its AS
  - Pack/TitleBG.pack Dm_Npc_Rito_HeroSoul_Kago.sbactorpack: a DemoNPC's actor link and AI program
  - Actor/ActorInfo.product.sbyml: OnlinePlayer's entry (PauseMenuPlayer's model and bounds, the DemoNPC's profile)
  - System/Resource/ResourceSizeTable.product.srsizetable: sizes for the new files
"""
import os
import struct
import sys
import zlib

import oead

NAME = 'OnlinePlayer'
BASE_NPC = 'Dm_Npc_Rito_HeroSoul_Kago'
BASE_LOOK = 'PauseMenuPlayer'
BASE_AI = 'Dm_Animal_Heron_A'
# (AS name, the player's own AS file): the order is runtime/online_actors.c k_anims'
ANIMS = [('OnlineWait', 'Player_Wait'), ('OnlineWalk', 'Player_Move'), ('OnlineRun', 'Player_Move'),
         ('OnlineDash', 'Player_Move'), ('OnlineJump', 'Player_Jump'), ('OnlineFall', 'Player_Fall'),
         ('OnlineGlide', 'Player_ParashawlGlide'), ('OnlineSwimWait', 'Player_SwimWait'), ('OnlineSwim', 'Player_SwimMove'),
         ('OnlineClimbWait', 'Player_ClimbWait'), ('OnlineClimb', 'Player_ClimbMove'),
         ('OnlineSquatWait', 'Player_SquatWait'), ('OnlineSquat', 'Player_SquatMove')]


def crc(s):
    return zlib.crc32(s.encode())


def pio(data):
    return oead.aamp.ParameterIO.from_binary(bytes(data))


def actor_pack(romfs, title_bg, name):
    p = os.path.join(romfs, 'Actor', 'Pack', name + '.sbactorpack')
    if os.path.exists(p):
        data = open(p, 'rb').read()
    else:
        data = bytes(title_bg.get_file('Actor/Pack/%s.sbactorpack' % name).data)
    return oead.Sarc(oead.yaz0.decompress(data))


class Rstb:
    """System/Resource/ResourceSizeTable (little-endian): crc32 -> size, and names whose crc32 collide"""

    def __init__(self, data):
        assert data[:4] == b'RSTB'
        ncrc, nname = struct.unpack_from('<II', data, 4)
        o = 12
        self.crc = {}
        for i in range(ncrc):
            c, s = struct.unpack_from('<II', data, o)
            self.crc[c] = s
            o += 8
        self.names = {}
        for i in range(nname):
            n = data[o:o + 128].split(b'\0')[0].decode()
            self.names[n] = struct.unpack_from('<I', data, o + 128)[0]
            o += 132

    def get(self, path):
        return self.names.get(path, self.crc.get(crc(path)))

    def set(self, path, size):
        if path in self.names:
            self.names[path] = size
        else:
            self.crc[crc(path)] = size

    def binary(self):
        out = bytearray(b'RSTB' + struct.pack('<II', len(self.crc), len(self.names)))
        for c in sorted(self.crc):
            out += struct.pack('<II', c, self.crc[c])
        for n in sorted(self.names):
            out += n.encode().ljust(128, b'\0') + struct.pack('<I', self.names[n])
        return bytes(out)


def main():
    romfs, out = sys.argv[1], sys.argv[2]
    title_bg_path = os.path.join(romfs, 'Pack', 'TitleBG.pack')
    title_bg = oead.Sarc(open(title_bg_path, 'rb').read())
    npc = actor_pack(romfs, title_bg, BASE_NPC)
    look = actor_pack(romfs, title_bg, BASE_LOOK)
    rstb = Rstb(oead.yaz0.decompress(open(os.path.join(romfs, 'System', 'Resource',
                                                       'ResourceSizeTable.product.srsizetable'), 'rb').read()))

    files = {}  # path in the actor pack -> bytes
    sizes = {}  # RSTB sizes of the new files
    # the actor link: the DemoNPC's, with Link's model and animations, without the spirit's effects and sounds
    link = pio(npc.get_file('Actor/ActorLink/%s.bxml' % BASE_NPC).data)
    lt = link.objects['LinkTarget']
    for key, value in (('ModelUser', NAME), ('ASUser', NAME), ('ElinkUser', 'Dummy'), ('SlinkUser', 'Dummy')):
        lt.params[key] = oead.aamp.Parameter(value)
    # the AI program: a DemoNPC that only stands there (root DummyAction: a demo heron's), not the spirit's (its
    # waiting state hides the model)
    idle = actor_pack(romfs, title_bg, BASE_AI)
    for f in idle.get_files():
        if f.name.startswith('Actor/AIProgram/'):
            files[f.name] = bytes(f.data)
            lt.params['AIProgramUser'] = oead.aamp.Parameter(f.name.split('/')[-1].rsplit('.', 1)[0])
            sizes[f.name] = rstb.get(f.name)
    files['Actor/ActorLink/%s.bxml' % NAME] = link.to_binary()
    sizes['Actor/ActorLink/%s.bxml' % NAME] = rstb.get('Actor/ActorLink/%s.bxml' % BASE_NPC)
    # the model list: Link's (PauseMenuPlayer's)
    files['Actor/ModelList/%s.bmodellist' % NAME] = bytes(look.get_file('Actor/ModelList/%s.bmodellist' % BASE_LOOK).data)
    sizes['Actor/ModelList/%s.bmodellist' % NAME] = rstb.get('Actor/ModelList/%s.bmodellist' % BASE_LOOK)
    # the AS list: PauseMenuPlayer's (Player_Animation), and "GiftStart" (what the AI plays) as Link's standing wait
    asl = pio(look.get_file('Actor/ASList/%s.baslist' % BASE_LOOK).data)
    defs = asl.lists['ASDefines']
    k = len(defs.objects)
    o = oead.aamp.ParameterObject()
    o.params['Name'] = oead.aamp.Parameter(oead.FixedSafeString64('GiftStart'))
    o.params['Filename'] = oead.aamp.Parameter(oead.FixedSafeString64('Player_PauseMenuWait'))
    defs.objects['ASDefine_%d' % k] = o
    # the animations the runtime plays (runtime/online_actors.c k_anims): the player's own AS (GameROMPlayer's)
    player = actor_pack(romfs, title_bg, 'GameROMPlayer')
    for as_name, bas in (ANIMS if '--no-anims' not in sys.argv else []):
        o = oead.aamp.ParameterObject()
        o.params['Name'] = oead.aamp.Parameter(oead.FixedSafeString64(as_name))
        o.params['Filename'] = oead.aamp.Parameter(oead.FixedSafeString64(bas))
        defs.objects['ASDefine_%d' % len(defs.objects)] = o
        f = player.get_file('Actor/AS/%s.bas' % bas) or look.get_file('Actor/AS/%s.bas' % bas)
        files['Actor/AS/%s.bas' % bas] = bytes(f.data)
    # the game finds an AS by binary search on its name: the defines stay sorted (as the game's own lists are)
    objs = sorted((o for o in defs.objects.values()), key=lambda o: str(o.params['Name'].v))
    sorted_defs = oead.aamp.ParameterList()
    for i, o in enumerate(objs):
        sorted_defs.objects['ASDefine_%d' % i] = o
    asl.lists['ASDefines'] = sorted_defs
    defs = sorted_defs
    files['Actor/ASList/%s.baslist' % NAME] = asl.to_binary()
    # (its RSTB size grows with the defines: the player's 399 take 135704, ~340 each)
    sizes['Actor/ASList/%s.baslist' % NAME] = 512 * len(defs.objects) + 0x2000
    for f in look.get_files():
        if f.name.startswith('Actor/AS/'):
            files[f.name] = bytes(f.data)

    w = oead.SarcWriter(oead.Endianness.Little)
    for p, d in files.items():
        w.files[p] = d
    pack = w.write()[1]
    pack_z = oead.yaz0.compress(pack)
    sizes['Actor/Pack/%s.bactorpack' % NAME] = int(len(pack) * 1.5) + 0x1000

    # resident: in TitleBG.pack
    tw = oead.SarcWriter.from_sarc(title_bg)
    tw.files['Actor/Pack/%s.sbactorpack' % NAME] = pack_z
    title_bg_new = tw.write()[1]

    # ActorInfo: PauseMenuPlayer's entry as OnlinePlayer, a DemoNPC
    ai_path = os.path.join(romfs, 'Actor', 'ActorInfo.product.sbyml')
    info = oead.byml.from_binary(oead.yaz0.decompress(open(ai_path, 'rb').read()))
    actors, hashes = info['Actors'], info['Hashes']
    src = next(a for a in actors if a['name'] == BASE_LOOK)
    npc_info = next(a for a in actors if a['name'] == BASE_NPC)
    entry = oead.byml.Hash()
    for k2, v in src.items():
        entry[k2] = v
    entry['name'] = NAME
    entry['profile'] = 'DemoNPC'
    entry['elink'] = 'Dummy'
    entry['slink'] = 'Dummy'
    entry['instSize'] = npc_info['instSize']
    if 'tags' in npc_info:
        entry['tags'] = npc_info['tags']
    h = crc(NAME)
    keys = [int(x) & 0xffffffff for x in hashes]
    if h not in keys:
        i = 0
        while i < len(keys) and keys[i] < h:
            i += 1
        hashes.insert(i, oead.S32(h) if h < 0x80000000 else oead.U32(h))
        actors.insert(i, entry)
    info_bin = oead.byml.to_binary(info, big_endian=False, version=2)
    sizes['Actor/ActorInfo.product.byml'] = max(rstb.get('Actor/ActorInfo.product.byml') or 0, len(info_bin) * 2)

    for p, s in sizes.items():
        rstb.set(p, int(s) if s else 0x10000)

    def put(rel, data):
        p = os.path.join(out, *rel.split('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(data)

    # its resources loaded at start and kept (only_res: no instance of its own), as the Remote Bomb's
    bootup_path = os.path.join(romfs, 'Pack', 'Bootup.pack')
    bootup = oead.Sarc(open(bootup_path, 'rb').read())
    res = oead.byml.from_binary(bytes(bootup.get_file('Actor/ResidentActors.byml').data))
    if not any(r['name'] == NAME for r in res):
        r = oead.byml.Hash()
        r['name'] = NAME
        r['only_res'] = True
        res.append(r)
    bw = oead.SarcWriter.from_sarc(bootup)
    bw.files['Actor/ResidentActors.byml'] = oead.byml.to_binary(res, big_endian=False, version=2)
    put('Pack/Bootup.pack', bw.write()[1])

    put('Pack/TitleBG.pack', title_bg_new)
    put('Actor/ActorInfo.product.sbyml', oead.yaz0.compress(info_bin))
    put('System/Resource/ResourceSizeTable.product.srsizetable', oead.yaz0.compress(rstb.binary()))
    open(os.path.join(out, 'online_mod.txt'), 'w').write('Savage Wind online play: OnlinePlayer (tools/online_mod.py)\n')
    print('online_mod: %s in %s (%d files in its pack)' % (NAME, out, len(files)))


if __name__ == '__main__':
    main()
