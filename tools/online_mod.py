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
import re
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
ARMOR_PREFIX = 'OnlineArmor_'
GLOWS = ('Weapon_Sword_070', 'PlayerBeam')  # (gear with its own effects: the Master Sword, its beam)
BEAM = 'OnlineBeam'
PLAYER_ELINK = 'Player_GameRom'  # (GameROMPlayer's ElinkUser)
VERSION = 5  # (runtime/main.c ONLINE_MOD_VERSION: armour actors, traverseDist, the paraglider)
TRAVERSE = 1000.0  # (the online actors' traverseDist)
GEAR_KINDS = ('Weapon_Sword_', 'Weapon_Lsword_', 'Weapon_Spear_', 'Weapon_Shield_', 'Weapon_Bow_', 'Weapon_Sheath_',
              'Weapon_Lsheath_', 'Weapon_SpearSheath_', 'Weapon_Quiver_')
# the Remote Bomb rune's bombs (resident in TitleBG), as the others see them thrown and going off
BOMBS = ['RemoteBomb', 'RemoteBombCube', 'RemoteBomb2', 'RemoteBombCube2']
BOMB_PREFIX = 'OnlineProj_'
# and as they go off for the others: the bomb's own actor (its AI goes off on the Remote Bomb rune's message, with its
# blast, effects and sound), without what hurts: its AtkExplode body on a layer that hits nothing (removed, the
# explosion stops short of its blast); no TgtBody (nothing aims at it)
BOOM_PREFIX = 'OnlineBoom_'
BOOM_LAYER = next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--boom-layer=')), 'SensorAttackCommon')
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


def _bfres_string(d, o):
    if o <= 0 or o + 2 > len(d):
        return None
    n = struct.unpack_from('<H', d, o)[0]
    return d[o + 2:o + 2 + n].decode('latin1') if 0 < n <= 64 and o + 2 + n <= len(d) else None


def bone_names(data):
    """a Switch bfres' skeletons: {model name: [bone names, in index order]} (FMDL: +0x10 its name, +0x20 its FSKL;
    FSKL +0x18 its bones, 0x50 each, +0 the name)"""
    d = bytes(oead.yaz0.decompress(data)) if data[:4] == b'Yaz0' else bytes(data)
    out = {}
    i = 0
    while True:
        i = d.find(b'FMDL', i)
        if i < 0:
            return out
        name = _bfres_string(d, struct.unpack_from('<Q', d, i + 0x10)[0])
        sk = struct.unpack_from('<Q', d, i + 0x20)[0]
        if name and 0 < sk < len(d) - 0x20 and d[sk:sk + 4] == b'FSKL':
            arr = struct.unpack_from('<Q', d, sk + 0x18)[0]
            bones = []
            while arr and arr + len(bones) * 0x50 + 8 <= len(d):
                b = _bfres_string(d, struct.unpack_from('<Q', d, arr + len(bones) * 0x50)[0])
                if not b:
                    break
                bones.append(b)
            out[name] = bones
        i += 4


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

    def build(name, modellist, model_files=(), as_files=None, physics=None, users=None):
        """an actor pack: the DemoNPC's actor link and an AI that only stands, the model list given, the AS list and
        AS when given (an actor finds them in its own pack: without them, its load never ends), physics = (its
        bphysics, RSTB size) when given, users: actor link users to set (ASUser, ElinkUser, ...); returns the packed
        (yaz0) actor pack"""
        files = {}
        link = pio(npc.get_file('Actor/ActorLink/%s.bxml' % BASE_NPC).data)
        lt = link.objects['LinkTarget']
        for key, value in (('ModelUser', name), ('ASUser', NAME if as_files else 'Dummy'), ('ElinkUser', 'Dummy'),
                           ('SlinkUser', 'Dummy')):
            lt.params[key] = oead.aamp.Parameter(value)
        for key, value in (users or {}).items():
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
    head.params['use_rigid_body_set_num'] = oead.aamp.Parameter(oead.S32(0))
    head.params['use_contact_info'] = oead.aamp.Parameter(False)
    link_phys = [(f.name, bytes(f.data)) for f in look.get_files() if f.name.startswith('Physics/')]
    resident = {NAME: build(NAME, link_ml.to_binary(), link_phys, as_files,
                            physics=(phys.to_binary(), rstb.get('Actor/Physics/%s.bphysics' % BASE_LOOK)),
                            users={'ElinkUser': PLAYER_ELINK})}  # (the player's effects: the runtime plays Daruk's
    # Protection, "Kago_Shield", on it)

    model_of = {}  # online actor -> the game's actor whose ActorInfo model and bounds it has
    # the armour: one actor per model list of the game's armour pieces (Armor_*, Link's default ones too), its model
    # alone with its physics (cloth, support bones); posed by the runtime bone by bone from the player's skeleton
    # (bones.txt: each actor's units, each bone's index in Link's skeleton, 255 for one Link hasn't: its cloth's)
    link_bones = bone_names(title_bg.get_file('Model/Link.sbfres').data)['Link']
    link_index = {b: k for k, b in enumerate(link_bones)}
    armor = {}      # the game's piece -> its online actor
    bone_map = {}   # online actor -> [per unit: [Link bone index or 255]]
    bfres_cache = {}
    armor_loose = {}
    pieces = [] if no_gear else sorted(n[:-len('.sbactorpack')] for n in os.listdir(os.path.join(romfs, 'Actor', 'Pack'))
                                       if re.match(r'Armor_\w+_(Head|Upper|Lower)(_\w+)?\.sbactorpack$', n))
    for piece in pieces:
        a = actor_pack(romfs, title_bg, piece)
        al = [f for f in a.get_files() if f.name.startswith('Actor/ActorLink/')]
        if not al:
            continue
        lt = pio(al[0].data).objects['LinkTarget']
        user = str(lt.params['ModelUser'].v)
        ml = a.get_file('Actor/ModelList/%s.bmodellist' % user)
        if not ml:
            continue
        online = ARMOR_PREFIX + user
        armor[piece] = online
        if online in armor_loose:
            continue
        mlp = pio(ml.data)
        units = []
        for md in mlp.lists['ModelData'].lists.values():
            folder = str(md.objects['Base'].params['Folder'].v)
            if folder not in bfres_cache:
                fp = os.path.join(romfs, 'Model', folder + '.sbfres')
                if os.path.exists(fp):
                    bfres_cache[folder] = bone_names(open(fp, 'rb').read())
                else:
                    tf = title_bg.get_file('Model/%s.sbfres' % folder)
                    bfres_cache[folder] = bone_names(tf.data) if tf else {}
            for u in md.lists['Unit'].objects.values():
                bones = bfres_cache[folder].get(str(u.params['UnitName'].v), [])
                units.append([link_index.get(b, 255) for b in bones])
        if not units or not units[0]:
            continue
        bone_map[online] = units
        pu = str(lt.params['PhysicsUser'].v)
        pf = a.get_file('Actor/Physics/%s.bphysics' % pu)
        extra = [(f.name, bytes(f.data)) for f in a.get_files() if f.name.startswith('Physics/')]
        armor_loose[online] = build(online, bytes(ml.data), extra,
                                    physics=(bytes(pf.data), rstb.get('Actor/Physics/%s.bphysics' % pu)) if pf else None)
        model_of_armor = model_of.setdefault('_armor', {})
        model_of_armor[online] = piece

    # the gear: one actor per model list of the game's weapons, shields, bows, sheaths and quivers
    gear = {}      # the game's actor -> its online actor
    loose = dict(armor_loose)  # online actor -> its pack (not resident)
    for online, piece in model_of.pop('_armor', {}).items():
        model_of[online] = piece
    names = [] if no_gear else sorted(n[:-len('.sbactorpack')] for n in os.listdir(os.path.join(romfs, 'Actor', 'Pack'))
                                      if n.startswith(GEAR_KINDS))
    names += [] if no_gear else ARROWS
    names += [] if no_gear else ['PlayerStole2', 'PlayerStole2_Vagrant']  # (the paraglider, open)
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
    if not no_gear:  # the Master Sword's beam: its effects and sounds alone (no AI, no attack), moved by the runtime
        bp = actor_pack(romfs, title_bg, 'PlayerBeam')
        loose[BEAM] = build(BEAM, bytes(bp.get_file('Actor/ModelList/None.bmodellist').data),
                            users={'ElinkUser': 'PlayerBeam', 'SlinkUser': 'PlayerBeam'})
        gear['PlayerBeam'] = BEAM
        model_of[BEAM] = 'PlayerBeam'

    # the Remote Bomb's bombs as they look and go off (their model, AS list and AS, effects and sounds: the AS
    # "Explode" sets off their explosion's), without what makes them hurt (their AI, physics, the Explode actor)
    for piece in [] if no_gear else BOMBS:
        a = actor_pack(romfs, title_bg, piece)
        lt = pio(next(f for f in a.get_files() if f.name.startswith('Actor/ActorLink/')).data).objects['LinkTarget']
        users = {k: str(lt.params[k].v) for k in ('ASUser', 'ElinkUser', 'SlinkUser', 'XlinkUser') if k in lt.params}
        user = str(lt.params['ModelUser'].v)
        bomb_as = {f.name: bytes(f.data) for f in a.get_files() if f.name.startswith(('Actor/AS/', 'Actor/ASList/'))}
        for f in bomb_as:
            sizes[f] = rstb.get(f)
        online = BOMB_PREFIX + piece
        loose[online] = build(online, bytes(a.get_file('Actor/ModelList/%s.bmodellist' % user).data), as_files=bomb_as,
                              users=users)
        gear[piece] = online
        model_of[online] = piece

    booms = {}  # online actor -> its pack (not resident)
    for piece in [] if no_gear else BOMBS:
        a = actor_pack(romfs, title_bg, piece)
        files = {f.name: bytes(f.data) for f in a.get_files()}
        lt_name = next(n for n in files if n.startswith('Actor/ActorLink/'))
        link = pio(files[lt_name])
        lt = link.objects['LinkTarget']
        pu = str(lt.params['PhysicsUser'].v)
        phys = pio(files['Actor/Physics/%s.bphysics' % pu])
        ps = phys.lists['ParamSet']
        sets = ps.lists['RigidBodySet']
        keep = [l for k, l in sets.lists.items()
                ]
        for l in keep:  # (AtkExplode kept: without it the explosion stops short of its blast; on a layer that hits
            # nothing, SensorNoHit)
            for body in l.lists.values():
                for o in body.objects.values():
                    if 'layer' in o.params and str(o.params['layer'].v) == 'SensorAttackCommon':
                        o.params['layer'] = oead.aamp.Parameter(oead.FixedSafeString32(BOOM_LAYER))
        new_sets = oead.aamp.ParameterList()
        for o in sets.objects.values():
            new_sets.objects[str(len(new_sets.objects))] = o
        new_sets.objects.clear()
        for k, l in enumerate(keep):
            new_sets.lists['RigidBodySet_%d' % k] = l
        ps.lists['RigidBodySet'] = new_sets
        head = next(o for o in ps.objects.values() if 'use_rigid_body_set_num' in o.params)
        head.params['use_rigid_body_set_num'] = oead.aamp.Parameter(oead.S32(len(keep)))
        online = BOOM_PREFIX + piece
        # its attack without damage, its push and shock wave kept (it knocks the player down, as the bomb's): its own
        # GeneralParamList and AI program, by its own names (the bomb's are loaded already: the game would take those)
        gpu = str(lt.params['GParamUser'].v)
        gp = pio(files.pop('Actor/GeneralParamList/%s.bgparamlist' % gpu))
        atk = gp.objects['Attack']
        for k2 in ('Power', 'PowerForPlayer'):  # (no damage; its push kept: it knocks the player down)
            atk.params[k2] = oead.aamp.Parameter(oead.S32(0))
        files['Actor/GeneralParamList/%s.bgparamlist' % online] = gp.to_binary()
        sizes['Actor/GeneralParamList/%s.bgparamlist' % online] = rstb.get('Actor/GeneralParamList/%s.bgparamlist' % gpu)
        lt.params['GParamUser'] = oead.aamp.Parameter(online)
        aip = str(lt.params['AIProgramUser'].v)
        ai = pio(files.pop('Actor/AIProgram/%s.baiprog' % aip))

        def harmless(l):
            for o in l.objects.values():
                if 'ShockPower' in o.params and '--boom-no-shock' in sys.argv:
                    o.params['ShockPower'] = oead.aamp.Parameter(0.0)
            for sub in l.lists.values():
                harmless(sub)
        harmless(ai)
        files['Actor/AIProgram/%s.baiprog' % online] = ai.to_binary()
        sizes['Actor/AIProgram/%s.baiprog' % online] = rstb.get('Actor/AIProgram/%s.baiprog' % aip)
        lt.params['AIProgramUser'] = oead.aamp.Parameter(online)
        lt.params['PhysicsUser'] = oead.aamp.Parameter(online)
        del files['Actor/Physics/%s.bphysics' % pu]
        files['Actor/Physics/%s.bphysics' % online] = phys.to_binary()
        sizes['Actor/Physics/%s.bphysics' % online] = rstb.get('Actor/Physics/%s.bphysics' % pu)
        del files[lt_name]
        files['Actor/ActorLink/%s.bxml' % online] = link.to_binary()
        sizes['Actor/ActorLink/%s.bxml' % online] = rstb.get(lt_name)
        w = oead.SarcWriter(oead.Endianness.Little)
        for p2, d in files.items():
            w.files[p2] = d
        pack = w.write()[1]
        sizes['Actor/Pack/%s.bactorpack' % online] = int(len(pack) * 1.5) + 0x1000
        booms[online] = oead.yaz0.compress(pack)
        gear['Boom_' + piece] = online

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
            elif k2 in ('bfres', 'mainModel') and k2 in entry:  # (no model: the beam)
                del entry[k2]
        entry['name'] = name
        entry['profile'] = 'DemoNPC'
        entry['elink'] = 'Dummy'
        entry['slink'] = 'Dummy'
        if model_from in BOMBS or model_from in GLOWS:  # (a bomb's effects and sounds, its explosion's; the Master Sword's glow)
            for k2 in ('elink', 'slink', 'xlink'):
                if k2 in src:
                    entry[k2] = src[k2]
        entry['instSize'] = npc_info['instSize']
        # (as far as the world is around: the game removes an actor farther than this from the camera)
        entry['traverseDist'] = oead.F32(TRAVERSE)
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
    next(a for a in actors if a['name'] == NAME)['elink'] = PLAYER_ELINK
    for online in booms:  # (the bomb's own entry, its profile and all)
        piece = online[len(BOOM_PREFIX):]
        entry = oead.byml.Hash()
        for k2, v in by_name[piece].items():
            entry[k2] = v
        entry['name'] = online
        h = crc(online)
        keys = [int(x) & 0xffffffff for x in hashes]
        if h not in keys:
            i = 0
            while i < len(keys) and keys[i] < h:
                i += 1
            hashes.insert(i, oead.S32(h) if h < 0x80000000 else oead.U32(h))
            actors.insert(i, entry)
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
    for n, z in list(loose.items()) + list(booms.items()):
        put('Actor/Pack/%s.sbactorpack' % n, z)
    put('Actor/ActorInfo.product.sbyml', oead.yaz0.compress(info_bin))
    put('System/Resource/ResourceSizeTable.product.srsizetable', oead.yaz0.compress(rstb.binary()))
    open(os.path.join(out, 'v%d' % VERSION), 'w').write('')  # (the runtime makes it anew when its own version isn't there)
    open(os.path.join(out, 'online_mod.txt'), 'w').write('Savage Wind online play: OnlinePlayer (tools/online_mod.py)\n')
    # the game's actor=its online actor, one a line (runtime/online_actors.c gear_actor)
    open(os.path.join(out, 'gear.txt'), 'w').write(''.join('%s=%s\n' % kv for kv in sorted(gear.items())))
    # the armour pieces' online actors (piece=actor), and each one's bone map (actor=unit|unit, each "i,i,...")
    open(os.path.join(out, 'armor.txt'), 'w').write(''.join('%s=%s\n' % kv for kv in sorted(armor.items())))
    open(os.path.join(out, 'bones.txt'), 'w').write(''.join(
        '%s=%s\n' % (k, '|'.join(','.join(str(i) for i in u) for u in v)) for k, v in sorted(bone_map.items())))
    print('online_mod: %s and %d gear actors (%d pieces) in %s' % (NAME, len(loose), len(gear), out))


if __name__ == '__main__':
    main()
