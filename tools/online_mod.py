"""Builds online play's RomFS overlay (docs/ONLINE.md) from the player's own game files: an actor for the other
players, OnlinePlayer - Link's model and animations on a DemoNPC (an NPC the game draws in the world and doesn't
steer), resident (in Pack/TitleBG.pack) so the runtime can create it at any time (runtime/online_actors.c).

    python tools/online_mod.py <romfs> <out>          e.g.  "E:/extracted hom/romfs"  online_mod
                                                     (needs oead: pip install oead)

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
# the armour pieces the other players can wear (runtime/online_actors.c): each an actor of its own, Online<piece>,
# with that piece's model and Link's animations, kept in step with the Link it dresses. Every head, upper and lower
# piece of the game (Actor/Pack/Armor_*); --armor-only=<piece>,... for a few (tests); --no-armor for none
ARMOR_PREFIX = 'Online'
# the armour models online_mod carries: each is a resident actor, and the game's resident list holds only about
# ten more than its own 48 (the 59th crashes it at start: 0x7000d49080's fixed arrays), Link's included. The pieces
# most seen (the head is Link's own hair in his model): Hylian tunic and trousers, the Old Shirt and Well-Worn
# Trousers, the Champion's Tunic, Climbing and Stealth. --armor-models=Armor_001_Upper,... for others
ARMOR_MODELS = ['Armor_001_Upper', 'Armor_001_Lower', 'Armor_043_Upper', 'Armor_043_Lower', 'Armor_116_Upper',
                'Armor_014_Upper', 'Armor_014_Lower', 'Armor_012_Upper', 'Armor_012_Lower']
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
    global ARMOR
    romfs, out = sys.argv[1], sys.argv[2]
    only = [a.split('=', 1)[1] for a in sys.argv if a.startswith('--armor-only=')]
    if '--no-armor' in sys.argv:
        ARMOR = []
    elif only:
        ARMOR = only[0].split(',')
    else:
        ARMOR = sorted(n[:-len('.sbactorpack')] for n in os.listdir(os.path.join(romfs, 'Actor', 'Pack'))
                       if n.startswith('Armor_') and not n.startswith('Armor_Default')
                       and n.endswith(('_Head.sbactorpack', '_Upper.sbactorpack', '_Lower.sbactorpack')))
    title_bg_path = os.path.join(romfs, 'Pack', 'TitleBG.pack')
    title_bg = oead.Sarc(open(title_bg_path, 'rb').read())
    npc = actor_pack(romfs, title_bg, BASE_NPC)
    look = actor_pack(romfs, title_bg, BASE_LOOK)
    rstb = Rstb(oead.yaz0.decompress(open(os.path.join(romfs, 'System', 'Resource',
                                                       'ResourceSizeTable.product.srsizetable'), 'rb').read()))

    sizes = {}  # RSTB sizes of the new files

    def build(name, modellist, model_files=(), with_as=True):
        """an actor pack: the DemoNPC's actor link and an AI that only stands, the model list given, and the AS list
        (Link's animations and the runtime's AS); returns the packed (yaz0) actor pack"""
        files = {}
        link = pio(npc.get_file('Actor/ActorLink/%s.bxml' % BASE_NPC).data)
        lt = link.objects['LinkTarget']
        for key, value in (('ModelUser', name), ('ASUser', NAME), ('ElinkUser', 'Dummy'), ('SlinkUser', 'Dummy')):
            lt.params[key] = oead.aamp.Parameter(value)
        # the AI program: a DemoNPC that only stands there (root DummyAction: a demo heron's), not the spirit's (its
        # waiting state hides the model)
        for f in idle.get_files():
            if f.name.startswith('Actor/AIProgram/'):
                files[f.name] = bytes(f.data)
                lt.params['AIProgramUser'] = oead.aamp.Parameter(f.name.split('/')[-1].rsplit('.', 1)[0])
                sizes[f.name] = rstb.get(f.name)
        files['Actor/ActorLink/%s.bxml' % name] = link.to_binary()
        sizes['Actor/ActorLink/%s.bxml' % name] = rstb.get('Actor/ActorLink/%s.bxml' % BASE_NPC)
        files['Actor/ModelList/%s.bmodellist' % name] = modellist
        sizes['Actor/ModelList/%s.bmodellist' % name] = max(rstb.get('Actor/ModelList/%s.bmodellist' % BASE_LOOK), 0x4000)
        if with_as:  # (every online actor carries the AS list and AS: an actor finds them in its own pack)
            files.update(as_files)
        for p2, d in model_files:
            files[p2] = d
        w = oead.SarcWriter(oead.Endianness.Little)
        for p2, d in files.items():
            w.files[p2] = d
        pack = w.write()[1]
        sizes['Actor/Pack/%s.bactorpack' % name] = int(len(pack) * 1.5) + 0x1000
        return oead.yaz0.compress(pack), len(files)

    # the AS list, shared by every online actor: PauseMenuPlayer's (Player_Animation), "GiftStart", and the runtime's
    idle = actor_pack(romfs, title_bg, BASE_AI)
    as_files = {}
    asl = pio(look.get_file('Actor/ASList/%s.baslist' % BASE_LOOK).data)
    defs = asl.lists['ASDefines']
    o = oead.aamp.ParameterObject()
    o.params['Name'] = oead.aamp.Parameter(oead.FixedSafeString64('GiftStart'))
    o.params['Filename'] = oead.aamp.Parameter(oead.FixedSafeString64('Player_PauseMenuWait'))
    defs.objects['ASDefine_%d' % len(defs.objects)] = o
    # the animations the runtime plays (runtime/online_actors.c k_anims): the player's own AS (GameROMPlayer's)
    player = actor_pack(romfs, title_bg, 'GameROMPlayer')
    for as_name, bas in ANIMS:
        o = oead.aamp.ParameterObject()
        o.params['Name'] = oead.aamp.Parameter(oead.FixedSafeString64(as_name))
        o.params['Filename'] = oead.aamp.Parameter(oead.FixedSafeString64(bas))
        defs.objects['ASDefine_%d' % len(defs.objects)] = o
        f = player.get_file('Actor/AS/%s.bas' % bas) or look.get_file('Actor/AS/%s.bas' % bas)
        as_files['Actor/AS/%s.bas' % bas] = bytes(f.data)
    # the game finds an AS by binary search on its name: the defines stay sorted (as the game's own lists are)
    objs = sorted((o for o in defs.objects.values()), key=lambda o: str(o.params['Name'].v))
    sorted_defs = oead.aamp.ParameterList()
    for i, o in enumerate(objs):
        sorted_defs.objects['ASDefine_%d' % i] = o
    asl.lists['ASDefines'] = sorted_defs
    as_files['Actor/ASList/%s.baslist' % NAME] = asl.to_binary()
    # (its RSTB size grows with the defines: the player's 399 take 135704, ~340 each)
    sizes['Actor/ASList/%s.baslist' % NAME] = 512 * len(sorted_defs.objects) + 0x2000
    for f in look.get_files():
        if f.name.startswith('Actor/AS/'):
            as_files[f.name] = bytes(f.data)

    # the actors: Link (PauseMenuPlayer's model list) and each armour piece (its own model list), resident in TitleBG
    link_ml = pio(look.get_file('Actor/ModelList/%s.bmodellist' % BASE_LOOK).data)
    if '--armor-in-model' in sys.argv:  # (experiment: the armour as more model data of Link's own model list)
        mds = link_ml.lists['ModelData']
        for piece in ARMOR:
            folder = piece.rsplit('_', 1)[0]
            md = oead.aamp.ParameterList()
            base = oead.aamp.ParameterObject()
            base.params['Folder'] = oead.aamp.Parameter(oead.FixedSafeString64(folder))
            md.objects['Base'] = base
            unit = oead.aamp.ParameterList()
            u = oead.aamp.ParameterObject()
            u.params['UnitName'] = oead.aamp.Parameter(oead.FixedSafeString64(piece))
            u.params['BindBone'] = oead.aamp.Parameter(oead.FixedSafeString64(''))
            unit.objects['Unit_0'] = u
            md.lists['Unit'] = unit
            mds.lists['ModelData_%d' % len(mds.lists)] = md
    packs = {NAME: build(NAME, link_ml.to_binary())}
    model_of = {}  # online armour actor -> the piece whose model and ActorInfo bounds it has
    models_arg = [a.split('=', 1)[1] for a in sys.argv if a.startswith('--armor-models=')]
    models = set(models_arg[0].split(',') if models_arg else ARMOR_MODELS)
    have = {}  # piece -> its online actor: one per model list (upgrades and dyes share their piece's)
    made = set()
    for piece in ARMOR:
        a = actor_pack(romfs, title_bg, piece)
        # its model list: its actor link's ModelUser (pieces share them)
        al = [f for f in a.get_files() if f.name.startswith('Actor/ActorLink/')]
        user = str(pio(al[0].data).objects['LinkTarget'].params['ModelUser'].v) if al else piece
        ml = a.get_file('Actor/ModelList/%s.bmodellist' % user)
        if not ml:
            print('online_mod: %s has no model list (%s), left out' % (piece, user))
            continue
        if user not in models:
            continue
        have[piece] = ARMOR_PREFIX + user
        if user in made:
            continue
        made.add(user)
        # (with its support bones and cloth: its model refers to them)
        extra = [(f.name, bytes(f.data)) for f in a.get_files() if f.name.startswith('Physics/')]
        packs[ARMOR_PREFIX + user] = build(ARMOR_PREFIX + user, bytes(ml.data), extra)
        model_of[ARMOR_PREFIX + user] = user
    pieces = have
    tw = oead.SarcWriter.from_sarc(title_bg)
    for n, (z, _) in packs.items():
        tw.files['Actor/Pack/%s.sbactorpack' % n] = z
    title_bg_new = tw.write()[1]

    # ActorInfo: PauseMenuPlayer's entry as OnlinePlayer, a DemoNPC
    ai_path = os.path.join(romfs, 'Actor', 'ActorInfo.product.sbyml')
    info = oead.byml.from_binary(oead.yaz0.decompress(open(ai_path, 'rb').read()))
    actors, hashes = info['Actors'], info['Hashes']
    npc_info = next(a for a in actors if a['name'] == BASE_NPC)

    def add_info(name, model_from):
        # PauseMenuPlayer's entry (a model and its bounds, nothing of an item), with the model's own model and bounds
        src = next(a for a in actors if a['name'] == model_from)
        entry = oead.byml.Hash()
        for k2, v in next(a for a in actors if a['name'] == BASE_LOOK).items():
            entry[k2] = v
        for k2 in ('bfres', 'mainModel', 'aabbMin', 'aabbMax', 'boundingForTraverse'):
            if k2 in src:
                entry[k2] = src[k2]
        entry['name'] = name
        entry['profile'] = 'DemoNPC'
        entry['elink'] = 'Dummy'
        entry['slink'] = 'Dummy'
        entry['instSize'] = npc_info['instSize']
        if 'tags' in npc_info:
            entry['tags'] = npc_info['tags']
        h = crc(name)
        keys = [int(x) & 0xffffffff for x in hashes]
        if h not in keys:
            i = 0
            while i < len(keys) and keys[i] < h:
                i += 1
            hashes.insert(i, oead.S32(h) if h < 0x80000000 else oead.U32(h))
            actors.insert(i, entry)

    add_info(NAME, BASE_LOOK)
    for online, piece in model_of.items():
        add_info(online, piece)
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
    for n in packs:
        if not any(r['name'] == n for r in res):
            r = oead.byml.Hash()
            r['name'] = n
            r['only_res'] = True
            res.append(r)
    res_bin = oead.byml.to_binary(res, big_endian=False, version=2)
    bw = oead.SarcWriter.from_sarc(bootup)
    bw.files['Actor/ResidentActors.byml'] = res_bin
    # (its RSTB size: the list grew; too small and the game reads no list at all and crashes at start, 0x7000d49080)
    rstb.set('Actor/ResidentActors.byml', max(rstb.get('Actor/ResidentActors.byml') or 0, len(res_bin) * 4 + 0x1000))
    put('Pack/Bootup.pack', bw.write()[1])

    put('Pack/TitleBG.pack', title_bg_new)
    put('Actor/ActorInfo.product.sbyml', oead.yaz0.compress(info_bin))
    put('System/Resource/ResourceSizeTable.product.srsizetable', oead.yaz0.compress(rstb.binary()))
    open(os.path.join(out, 'online_mod.txt'), 'w').write('Savage Wind online play: OnlinePlayer (tools/online_mod.py)\n')
    # piece=its online actor, one a line (runtime/online_actors.c armor_actor)
    open(os.path.join(out, 'armor.txt'), 'w').write(''.join('%s=%s\n' % kv for kv in sorted(pieces.items())))
    print('online_mod: %d actors in %s (%s)' % (len(packs), out, ', '.join(packs)))


if __name__ == '__main__':
    main()
