"""Builds online play's RomFS overlay (docs/ONLINE.md) from the player's own game files: the actors the other players
are made of (runtime/online_actors.c).

    python tools/online_mod.py <romfs> <out>          e.g.  "E:/extracted hom/romfs"  online_mod
                                                     (needs oead: pip install oead)

The output is derived from the game's files: it stays on the player's machine (the folder is git-ignored) and the
runtime serves it as the RomFS overlay (runtime/main.c: <data root>/online_mod).

  - OnlinePlayer: Link's model (PauseMenuPlayer's model list, support bones and cloth) and the player's own AS list
    and AS (GameROMPlayer's Player.baslist: the others' Links play the AS their players' Links play) on a DemoNPC (an
    NPC the game draws in the world and doesn't steer); resident (in Pack/TitleBG.pack), so the runtime can create
    it at any time. His armour is the game's own (the real Armor_* actors, worn on his skeleton: no actor here).
  - OnlineGear_<model>: a weapon, shield, bow, sheath or quiver's model alone on a DemoNPC, one per model list of the
    game's Weapon_* actors (the real ones fall, can be picked up, hurt): not resident, the runtime asks for one when
    a player carries it and the game loads it meanwhile. gear.txt: "actor=its online actor" a line.
From the game, also:
  - Pack/TitleBG.pack Dm_Npc_Rito_HeroSoul_Kago.sbactorpack: a DemoNPC's actor link; Dm_Animal_Heron_A's AI program
  - Actor/ActorInfo.product.sbyml: the new actors' entries (PauseMenuPlayer's, with each model's own model and bounds)
  - System/Resource/ResourceSizeTable.product.srsizetable: sizes for the new files
"""
import os
import shutil
import struct
import sys
import zlib

import oead

NAME = 'OnlinePlayer'
BASE_NPC = 'Dm_Npc_Rito_HeroSoul_Kago'
BASE_LOOK = 'PauseMenuPlayer'
BASE_AI = 'Dm_Animal_Heron_A'
BASE_AS = 'GameROMPlayer'
GEAR_PREFIX = 'OnlineGear_'
GEAR_KINDS = ('Weapon_Sword_', 'Weapon_Lsword_', 'Weapon_Spear_', 'Weapon_Shield_', 'Weapon_Bow_', 'Weapon_Sheath_',
              'Weapon_Lsheath_', 'Weapon_SpearSheath_', 'Weapon_Quiver_')
# the arrow on a drawn bow's string (the game's own, resident in TitleBG: they fly, hurt)
ARROWS = ['NormalArrow', 'FireArrow', 'IceArrow', 'ElectricArrow', 'BombArrow_A', 'AncientArrow', 'BrightArrow']
# (AS name, one Player_Animation clip): the order is runtime/online_actors.c k_anims'. What a Link plays while his
# player's own AS hasn't come (an older game, or none yet)
ANIMS = [('OnlineWait', 'Nml_Wait'), ('OnlineWalk', 'Nml_Move_Walk'), ('OnlineRun', 'Nml_Move_Run'),
         ('OnlineDash', 'Nml_Move_Dash'), ('OnlineJump', 'Nml_Wait_Jump'), ('OnlineFall', 'Fall'),
         ('OnlineGlide', 'Float_Float'), ('OnlineSwimWait', 'Swim_Wait'), ('OnlineSwim', 'Swim_Move_Slow_F'),
         ('OnlineClimbWait', 'Climb_Wall_Wait_FootL'), ('OnlineClimb', 'Climb_Wall_Move_U'),
         ('OnlineSquatWait', 'Squat_Wait_FootL'), ('OnlineSquat', 'Squat_Move_Walk')]


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
    no_gear = '--no-gear' in sys.argv
    if os.path.exists(os.path.join(out, 'online_mod.txt')):  # (an older one: made anew, nothing of it left)
        shutil.rmtree(out)
    title_bg_path = os.path.join(romfs, 'Pack', 'TitleBG.pack')
    title_bg = oead.Sarc(open(title_bg_path, 'rb').read())
    npc = actor_pack(romfs, title_bg, BASE_NPC)
    look = actor_pack(romfs, title_bg, BASE_LOOK)
    idle = actor_pack(romfs, title_bg, BASE_AI)
    rstb = Rstb(oead.yaz0.decompress(open(os.path.join(romfs, 'System', 'Resource',
                                                       'ResourceSizeTable.product.srsizetable'), 'rb').read()))

    sizes = {}  # RSTB sizes of the new files

    def build(name, modellist, model_files=(), as_files=None, physics=None):
        """an actor pack: the DemoNPC's actor link and an AI that only stands, the model list given, the AS list and
        AS when given (an actor finds them in its own pack: without them, its load never ends), and physics = (its
        bphysics, RSTB size) when given; returns the packed (yaz0) actor pack"""
        files = {}
        link = pio(npc.get_file('Actor/ActorLink/%s.bxml' % BASE_NPC).data)
        lt = link.objects['LinkTarget']
        for key, value in (('ModelUser', name), ('ASUser', NAME if as_files else 'Dummy'), ('ElinkUser', 'Dummy'),
                           ('SlinkUser', 'Dummy')):
            lt.params[key] = oead.aamp.Parameter(value)
        # the AI program: a DemoNPC that only stands there (root DummyAction: a demo heron's), not the spirit's (its
        # waiting state hides the model)
        for f in idle.get_files():
            if f.name.startswith('Actor/AIProgram/'):
                files[f.name] = bytes(f.data)
                lt.params['AIProgramUser'] = oead.aamp.Parameter(f.name.split('/')[-1].rsplit('.', 1)[0])
                sizes[f.name] = rstb.get(f.name)
        if physics:  # (its cloth and support bones: without them Link's cloth and the bones that bend his clothes
            # with his legs stay in their rest pose while he moves)
            lt.params['PhysicsUser'] = oead.aamp.Parameter(name)
            files['Actor/Physics/%s.bphysics' % name] = physics[0]
            sizes['Actor/Physics/%s.bphysics' % name] = physics[1]
        files['Actor/ActorLink/%s.bxml' % name] = link.to_binary()
        sizes['Actor/ActorLink/%s.bxml' % name] = rstb.get('Actor/ActorLink/%s.bxml' % BASE_NPC)
        files['Actor/ModelList/%s.bmodellist' % name] = modellist
        sizes['Actor/ModelList/%s.bmodellist' % name] = max(rstb.get('Actor/ModelList/%s.bmodellist' % BASE_LOOK), 0x4000)
        files.update(as_files or {})
        for p2, d in model_files:
            files[p2] = d
        w = oead.SarcWriter(oead.Endianness.Little)
        for p2, d in files.items():
            w.files[p2] = d
        pack = w.write()[1]
        sizes['Actor/Pack/%s.bactorpack' % name] = int(len(pack) * 1.5) + 0x1000
        return oead.yaz0.compress(pack)

    # OnlinePlayer's AS list: the player's own (its AS, its partials: upper body, arms, ...) and the runtime's one-clip
    # ones; the game finds an AS by binary search on its name, so the defines stay sorted (as the game's own are)
    player = actor_pack(romfs, title_bg, BASE_AS)
    as_files = {}
    asl_file = next(f for f in player.get_files() if f.name.startswith('Actor/ASList/'))
    asl = pio(asl_file.data)
    defs = asl.lists['ASDefines']
    for as_name, clip in ANIMS:
        o = oead.aamp.ParameterObject()
        o.params['Name'] = oead.aamp.Parameter(oead.FixedSafeString64(as_name))
        o.params['Filename'] = oead.aamp.Parameter(oead.FixedSafeString64('Online_' + as_name))
        defs.objects['ASDefine_%d' % len(defs.objects)] = o
        par = oead.aamp.ParameterObject()  # (one element: TypeIndex 67 plays the clip FileName)
        par.params['TypeIndex'] = oead.aamp.Parameter(oead.S32(67))
        par.params['FileName'] = oead.aamp.Parameter(oead.FixedSafeString64(clip))
        e = oead.aamp.ParameterList()
        e.objects['Parameters'] = par
        els = oead.aamp.ParameterList()
        els.lists['Element0'] = e
        bas = oead.aamp.ParameterIO()
        bas.type = 'xml'
        bas.lists['Elements'] = els
        as_files['Actor/AS/Online_%s.bas' % as_name] = bas.to_binary()
        sizes['Actor/AS/Online_%s.bas' % as_name] = rstb.get('Actor/AS/Player_Wait.bas')
    objs = sorted((o for o in defs.objects.values()), key=lambda o: str(o.params['Name'].v))
    sorted_defs = oead.aamp.ParameterList()
    for i, o in enumerate(objs):
        sorted_defs.objects['ASDefine_%d' % i] = o
    asl.lists['ASDefines'] = sorted_defs
    as_files['Actor/ASList/%s.baslist' % NAME] = asl.to_binary()
    sizes['Actor/ASList/%s.baslist' % NAME] = max(rstb.get(asl_file.name) or 0, 512 * len(sorted_defs.objects)) + 0x4000
    for f in player.get_files():
        if f.name.startswith('Actor/AS/'):
            as_files[f.name] = bytes(f.data)

    # Link: PauseMenuPlayer's model list, support bones and cloth (its physics without its sensor, a rigid body)
    link_ml = pio(look.get_file('Actor/ModelList/%s.bmodellist' % BASE_LOOK).data)
    phys = pio(look.get_file('Actor/Physics/%s.bphysics' % BASE_LOOK).data)
    ps = phys.lists['ParamSet']
    for k in ('RigidContactInfo', 'RigidBodySet'):
        if k in ps.lists:
            del ps.lists[k]
    head = next(o for o in ps.objects.values() if 'use_cloth' in o.params)
    head.params['use_rigid_body_set_num'] = oead.aamp.Parameter(0)
    head.params['use_contact_info'] = oead.aamp.Parameter(False)
    link_phys = [(f.name, bytes(f.data)) for f in look.get_files() if f.name.startswith('Physics/')]
    resident = {NAME: build(NAME, link_ml.to_binary(), link_phys, as_files,
                            physics=(phys.to_binary(), rstb.get('Actor/Physics/%s.bphysics' % BASE_LOOK)))}

    # the gear: one actor per model list of the game's weapons, shields, bows, sheaths and quivers
    gear = {}      # the game's actor -> its online actor
    model_of = {}  # online actor -> the game's actor whose ActorInfo model and bounds it has
    loose = {}     # online actor -> its pack (not resident)
    names = [] if no_gear else sorted(n[:-len('.sbactorpack')] for n in os.listdir(os.path.join(romfs, 'Actor', 'Pack'))
                                      if n.startswith(GEAR_KINDS))
    names += [] if no_gear else ARROWS
    for piece in names:
        a = actor_pack(romfs, title_bg, piece)
        al = [f for f in a.get_files() if f.name.startswith('Actor/ActorLink/')]
        user = str(pio(al[0].data).objects['LinkTarget'].params['ModelUser'].v) if al else piece
        ml = a.get_file('Actor/ModelList/%s.bmodellist' % user)
        if not ml:
            continue
        gear[piece] = GEAR_PREFIX + user
        if GEAR_PREFIX + user in loose:
            continue
        loose[GEAR_PREFIX + user] = build(GEAR_PREFIX + user, bytes(ml.data))
        model_of[GEAR_PREFIX + user] = piece

    tw = oead.SarcWriter.from_sarc(title_bg)
    for n, z in resident.items():
        tw.files['Actor/Pack/%s.sbactorpack' % n] = z
    title_bg_new = tw.write()[1]

    # ActorInfo: PauseMenuPlayer's entry for each, a DemoNPC, with its model's own model and bounds
    ai_path = os.path.join(romfs, 'Actor', 'ActorInfo.product.sbyml')
    info = oead.byml.from_binary(oead.yaz0.decompress(open(ai_path, 'rb').read()))
    actors, hashes = info['Actors'], info['Hashes']
    npc_info = next(a for a in actors if a['name'] == BASE_NPC)
    by_name = {a['name']: a for a in actors}

    def add_info(name, model_from):
        src = by_name[model_from]
        entry = oead.byml.Hash()
        for k2, v in by_name[BASE_LOOK].items():
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
        if piece in by_name:
            add_info(online, piece)
    info_bin = oead.byml.to_binary(info, big_endian=False, version=2)
    sizes['Actor/ActorInfo.product.byml'] = max(rstb.get('Actor/ActorInfo.product.byml') or 0, len(info_bin) * 2)

    for p, s in sizes.items():
        rstb.set(p, int(s) if s else 0x10000)

    def put(rel, data):
        p = os.path.join(out, *rel.split('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(data)

    # OnlinePlayer's resources loaded at start and kept (only_res: no instance of its own), as the Remote Bomb's
    bootup_path = os.path.join(romfs, 'Pack', 'Bootup.pack')
    bootup = oead.Sarc(open(bootup_path, 'rb').read())
    res = oead.byml.from_binary(bytes(bootup.get_file('Actor/ResidentActors.byml').data))
    for n in resident:
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
    for n, z in loose.items():
        put('Actor/Pack/%s.sbactorpack' % n, z)
    put('Actor/ActorInfo.product.sbyml', oead.yaz0.compress(info_bin))
    put('System/Resource/ResourceSizeTable.product.srsizetable', oead.yaz0.compress(rstb.binary()))
    open(os.path.join(out, 'online_mod.txt'), 'w').write('Savage Wind online play: OnlinePlayer (tools/online_mod.py)\n')
    # the game's actor=its online actor, one a line (runtime/online_actors.c gear_actor)
    open(os.path.join(out, 'gear.txt'), 'w').write(''.join('%s=%s\n' % kv for kv in sorted(gear.items())))
    print('online_mod: %s and %d gear actors (%d pieces) in %s' % (NAME, len(loose), len(gear), out))


if __name__ == '__main__':
    main()
