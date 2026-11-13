# Online play

Savage Wind's own online play, after the idea of [Milk Bar Launcher](https://github.com/MilkBarModding/MilkBarLauncher)
(Breath of the Wild Multiplayer for Cemu). Milk Bar's code reads and writes the Wii U game's memory inside Cemu and
spawns its players with Wii U actor packs; none of that carries over to the Switch game, so this is written anew for
Savage Wind: the same kind of session (a host, up to 8 players), its own protocol and its own way into the game.

## Playing

- **Create a session:** Settings (F1) > **Online** (or the game's Options > Online) > Name, Port (52520), Password
  (optional) > **Create session**. The row's description lists this PC's addresses to give the others.
- **Join:** the same tab > Host address (IP or name), the same port and password > **Join session**.
- Players outside the host's network need the port (TCP) forwarded on the host's router, or a virtual network
  (Tailscale, ZeroTier, Radmin VPN).
- **Dedicated server**, without the game: `savagewind.exe --server [port] [--password p]`.
- `SAVAGEWIND_ONLINE=host` or `SAVAGEWIND_ONLINE=<address>` creates or joins a session at start (shortcuts, tests).
- The settings are in `online.ini` in the data folder.

Each player plays their own game (their save, their world: enemies, chests and quests are not shared); the others are
in it as Links: each other player is a Link in the world where he is, posed as his Link is in his game (his skeleton,
bone by bone: walking, climbing, every swing of a sword, a shield's guard, drawing a bow, throwing...), in the armour he wears
and with the weapon, shield, bow, sheath and quiver he carries, in his hands or on his back as in his game, with his
name over him. His Remote Bomb bombs (in his hands, thrown, rolling) and his arrows in flight are there too, and his
bombs go off where they do (their effects and sound only: what hurts stays in his game). The overlay adds the session's players (left, under the hearts), join / leave notices, the chat's last
lines and an arrow at the screen's edge for a player out of view. Chat from the Online tab.

### The players' Links (online_mod)

The other players' Links (OnlinePlayer) and their gear (OnlineGear_*) are actors the game doesn't have, made from the
player's own game files by `tools/online_mod.py` into `online_mod/` in the data folder (git-ignored: it is derived from
the game). A source checkout makes it at the first start, and again over an older one (`tools/venv` with `oead`:
`tools/setup_windows.ps1` installs it), or by hand:

    tools/venv/Scripts/python.exe tools/online_mod.py "<game files>/romfs" online_mod

With `online_mod/` there the runtime serves it as the RomFS overlay (`SAVAGEWIND_ROMFS_OVERLAY`). Without it the
other players are the overlay's figures only.

## How it works

| Part | Where |
|---|---|
| Protocol, server, client, interpolation, settings | `runtime/online.c`, `runtime/online.h` |
| Online tab and overlay | `runtime/ui/online_ui.mm` (tab 10 of `runtime/ui/menu.mm`; Options > Online in `game_options.mm`) |
| Scene camera for the overlay | `runtime/nvn/vulkan/exec.c` (frame generation's camera capture, also while online) |
| The other players as Links | `runtime/online_actors.c`, called each frame from `tools/patches.py` `_vfr_ratio` |
| OnlinePlayer and OnlineGear_*, from the game files | `tools/online_mod.py` |
| Test player without a game | `tools/online_bot.c` |

The wire (protocol 9): TCP, little-endian, `u16 length, u8 type, payload`. HELLO (magic `SWOL`, protocol version,
name, password) -> WELCOME (slot) or REJECT (full, version, password, name); JOIN / LEAVE; STATE 20 times a second
(Link's feet position, heading, paused / loading flags, the sender's round trip, the pose: his AS and his gear's
bones); SKEL 20 times a second (his skeleton and his bow's, its string: each bone's local rotation, the smallest
three of its quaternion in 6 bytes, and translation, 3 x s16 of 1/8192 m: 121 bones and the bow's, ~1.6 KB, ~30 KB/s
a player); EQUIP (what he wears and
carries: when it changes and every 5 s); CHAT; PING / PONG. The server passes each STATE, SKEL, EQUIP and CHAT on
with the sender's slot, and the last STATE and EQUIP of everyone to whoever joins. Clients draw the others 100 ms
behind what they last heard, interpolated (the skeleton's rotations by nlerp); the pose is the newer snapshot's.

What the game gives: `uking::act::PlayerInfo` [`player_info`] +0x88 Link's position, +0x60 the player actor; the
pause state [`pause_state`]. The pose (`sample_pose` in `online_actors.c`, on the frame loop): the AS the player
actor plays on four channels (slot 0 bank 0, the body; slot 1 banks 0 and 1, the upper body; slot 2, drawing a
weapon: `Wait`, `Move`, `CutS1`, `CutChargeLswordSpin`, `BowCharge`, `WeaponThrow`, ...), his heading from his
matrix (actor +0x398), and for each piece of gear the bone of his model it is nearest to (`Weapon_R`, `Weapon_L`, the
back's `Pod_A/B/C`, ...) and its matrix in that bone's space. Without a pose (an older client) the others go by
`ONLINE_A_*`, from how he moves (`motion_anim` in `online.c`).

The skeleton: the gsys model unit with Link's bones (the one `Skl_Root` is in, `0x7000f77260`) has its skeleton at
+0x118 (+0x18 the bones' local matrices, 0x60 each: +0x10 the scale, +0x20 the 3x4 as columns; +0x20 the world
matrices; +0x50 the bone count, 121); `0x7000f960f8`(unit, `Matrix34f *` out, `Vec3 *` scale out, bone) reads a
bone's local matrix and `0x7000f95fa8`(unit, `Matrix34f *`, `Vec3 *` scale, bone) writes it (both gsys ModelUnit
vfuncs: the game's bone controls write so after the animation). The runtime reads the local Link's after his
animation (the frame loop runs before the actors' update: last frame's), and writes the remote's, whose Link plays no
AS: nothing writes over it before the game makes the bones' world matrices from it. Copied so, his Link moves exactly
as the player's (the player's AS, played by name on another actor, pick their clips by the player's state: weapon
type, shield, bow... and went wrong). His armour, worn on that skeleton, and his gear on its bones follow.

### OnlinePlayer and OnlineGear

`tools/online_mod.py`, from the game's own files:

- the actor link of a DemoNPC (`Dm_Npc_Rito_HeroSoul_Kago`: an NPC the game draws in the world and doesn't steer),
  without its effects and sounds; the AI program of a DemoNPC that only stands (`Dm_Animal_Heron_A`, root
  `DummyAction`; the spirit's own AI hides its model while it waits);
- OnlinePlayer: Link's model list and physics (`PauseMenuPlayer`'s: Folder Link; support bones and cloth, without its
  sensor), the player's own AS list and AS (`GameROMPlayer`'s `Player.baslist`: 399 AS, its partials) plus the
  runtime's one-clip AS (`OnlineWait`, ...). **The defines must stay sorted by name**: the game finds an AS by binary
  search. Resident: in `Pack/TitleBG.pack` and `Actor/ResidentActors.byml` (`Pack/Bootup.pack`, `only_res`; the list
  holds only ten more than the game's 48: the 59th crashes it at start, 0x7000d49080's fixed arrays);
- OnlineGear_<model>: one per model list of the game's weapons, shields, bows, sheaths and quivers (263), the model
  alone; not resident (`Actor/Pack/`), asked for when a player carries one (`gear.txt`: actor = its online actor);
- an entry for each in `Actor/ActorInfo.product.sbyml` (sorted by crc32 of the name) and sizes in the
  ResourceSizeTable.

### The game's functions (1.9.0)

Read from the code (capstone, and Ghidra: `tools/ghidra/README.md`; no symbols, no RTTI in `main`):

- `0x7001535900`: creates an actor now, by name. x0 the creator (`**0x7001d66550`), x1 the name, x2 a heap
  (`*(**0x7001d66558 + 0x28)`), x3 0, w4, w5 0; returns the actor, or 0. The Remote Bomb rune calls it
  (`0x7000b3bb40`). The actor is drawn only when its resources (its model) are loaded: resident, or in use.
- `0x7001535b10`: asks for an actor, its resources loaded first on the game's threads: x0 the creator, x1 the name,
  x2 the heap, x3 a BaseProcHandle (16 bytes: the unit, a flag), x4 its params or 0, x5 0, w6 1. The unit's state
  (int at +0) turns 2 when it is made; `0x7001516b74`(handle) then gives the actor (and wakes it); 3 or 5: it failed,
  `0x70015168a8` releases the handle. The player's armour code (`0x70011750c8`) works so.
- `0x700152261c`: sets an actor's matrix (x0 actor, x1 `Matrix34f *` row-major, x2 scale or 0): actor +0x398.
- `0x7001514bac`: deletes an actor (x0, w1 0).
- Actor +0x64: the BaseProc state (0 init, 1 calc, 2 sleep, 3 delete). An actor's AS can play from state 1 on.
- `0x700151e42c`: an AI action's "play this AS": x0 the action (its actor at +8), x1 the AS name, w2 not again if
  playing, w3 the slot, w4 the bank, s0 the blend (-1: its own). `ForkASPlay`'s enter (`0x7000759e74`) calls it. The AS manager is
  actor +0x560 (+0xb8 slots, +0xc0 the slots, 0x50 each, +0x20 a slot's banks; +0xd8 the actor);
  `0x70014a0040` (manager, slot, bank) gives the AS playing (+8 its name; `0x7001dad0b8` for none).
- `0x7000f77260`(model, SafeString) finds a bone: the unit in the low 16 bits (-1 none), the bone in the high 16;
  `unit->vtable[0x70]`(unit, `Matrix34f *`, bone) its matrix in the world (`0x7000741454` reads `Weapon_L` so). The AS list is `*(actor +0x570) +0x98` (+0x2b8 count, +0x2c0 entries of 0x88).

### Tried, and what came of it

- `RemoteBombCube` as a player: drawn, but its AI and physics keep it at Link's hands.
- `PauseMenuPlayer`: never drawn in the world (the pause menu's own scene).
- `GameROMPlayer` (a second player): the game crashes right after (access at 0x4e0).
- `Dm_Npc_Rito_HeroSoul_Kago`: created, state 2 (sleep), its model hidden by its AI.
- `Item_Material_01`: created, not drawn (its model isn't loaded).

### Armour

Each player sends what his Link wears and carries (EQUIP), read on the frame loop from the actor links the game keeps
(a BaseProcLink: the actor's unit and id; the unit holds the id at +0x40 and the actor at +0x48 while it lives): his
armour's manager is player +0x2500 (its +0x128 the player) and links head, upper and lower at +0x10, 0x10 each; the
player links his weapon at +0xb98, his shield at +0xbb0, his bow at +0xbc8 (0x18 each); a weapon links its sheath
and a bow its quiver at +0x958 (their PodName, QuiverName). (`*(player +0xb98)` read as an array of 0x58 slots looked
like the equipment at first: it is the weapon link's unit, in a pool of units that only start out in that order.)
An actor of ours the game deletes (a load) is seen gone by its link and made again.

Each piece is made once for a player and kept for the session (his pool, `g_pool`): a second actor of a piece made
for him (after the first was deleted, or made and never taken) is never drawn, whatever was tried (made at once with
0x7001535900 or asked for, with or without the armour code's init params "@P" / "@DC", unworn before deletion): its
model unit never gets what drawing needs (unit +0xa0, +0x100, which a drawn one has). So a piece he takes off is
unworn (`0x700117363c`(armour, 0)) and its materials hidden (unworn it still goes with the skeleton it was on:
each unit's `vtable[0xb8]` material count, `vtable[0x1b0]`(unit, material, visible)), and put back on it is the
same actor, worn again and shown. A
request under way can't be stopped (`0x70015168a8` releases only a failed one; the unit keeps the handle's address
at +8, so each request has a handle of its own, never moved): it goes to the pool as it is and is taken when it comes.
The pool goes when he leaves (deleted) or at a load.

**Now (model-only armour):** the game's own Armor_* actors, worn on a remote skeleton, crashed some games (mixed
sets, fast changes) and a second one made for a player was never drawn. So tools/online_mod.py makes an actor per
armour model list (`OnlineArmor_<ModelUser>`: the model with its physics, cloth and support bones; `armor.txt` maps
each Armor_* piece to it, Link's default hair and shorts too), and `bones.txt` gives, for each unit of it, each
bone's index in Link's skeleton (255: a bone Link hasn't). The runtime puts the piece on his Link's matrix and each
frame writes Link's local bones into it (`0x7000f95fa8`): the skeleton he sent, or, without one (an older game, the
test bot), his remote Link's own bones as its AS left them (`0x7000f960f8`). Dyed pieces share their model list,
so they look undyed.

What follows is how it was before (kept for what was learnt). The others dress his Link in the game's own pieces, any of them: each is asked for (`0x7001535b10`, its model loaded
then), and once made worn on his Link as the player's armour code does (`0x70011750c8`): `0x700117363c`(armour,
wearer) puts it on the wearer's model's skeleton (+0x840), then `0x7001173550`(armour). It moves with his skeleton,
its cloth and support bones its own. No helmet: `Armor_Default_Head` (Link's hair: his model has none).

What covers what is in the models: a bfres material's `HideMaterialGroup` user data names its groups (Link's
`Mt_Upper_Skin`: `G_Upper`, `G_UpperBeltSet`, ...; `Mt_Lower_Skin`: `G_Lower`), and a model's the groups it covers
(`Armor_116_Upper`: `G_Upper`; `Armor_014_Lower`: `G_Lower`). The player's armour code (0x7001175324) calls
`0x700126ba4c`(x1 a model, x2 another or 0, w3 visible) for each piece's model (actor +0x4e0): all shown, then hidden
where another piece covers it. The runtime does the same for each remote Link's parts once all run, and again when
one changes.

Learnt on the way: an actor of its own per armour piece, playing Link's AS on Link's matrix, goes out of step (the
player's AS pick clips at random per actor) and needs its own physics (cloth, support bones), and only nine fit in
the resident list; a DemoNPC's pack needs the AS list and AS it names (without them its load never ends).

### Gear

A bow's own bones (its string, drawn) travel with the skeleton and are written into the remote bow's model; while his
upper body plays `BowReload` / `BowCharge` with the bow in a hand, an arrow (`NormalArrow`'s model: +z its point) is
in his right hand pointing at his left.

Each piece (weapon, shield, bow, sheath, quiver) is an OnlineGear actor, asked for when the player carries it; each
frame its matrix is the bone's matrix on the remote Link times the piece's matrix in that bone's space as his game
has it (STATE's pose: a bone is kept while the piece stays near it, so two bones as near don't flicker). A piece on
no bone (thrown, dropped) goes under the world, out of sight (farther than 0.6 m from every bone; a shield 1.5 m, so
the one he surfs on, under his feet, stays).

The paraglider is a sixth piece (protocol 8): the player links his `PlayerStole2` (or `_Vagrant`) actor, at player
+0x2c28 in 1.9.0 (found once by looking through the player for a link to it); it is sent only while his body plays
a `Para*` AS (`ParaEquipOn`, `ParashawlGlide`, ...; not `ParaEquipOff`), on the bone it is nearest (3 m reach).

The online actors have `traverseDist` 1000 in their ActorInfo: without it the game deleted the gear farther than
about 50 m and the remote Link farther than about 80 to 140 m from the camera, and they were made again and again
(a piece left standing where it was deleted). Requests still under way when everything is forgotten (a load, a
shrine) are kept (`g_orphan`) and what they make is deleted when it comes, so no stray piece stays in the world.

The Master Sword's online actor has the sword's own ELink, SLink and XLink (`GLOWS` in tools/online_mod.py).

Daruk's Protection (protocol 9: STATE's `effects`): the player shows it with effects of his own ELink, each kept as a
handle (the effect, its id; alive while the effect's +0x20 is that id): "Kago_Shield" (a hit taken on it) at player
+0x2350 and "Kago_ShieldCount_0<n>" (its ring while he guards, n charges left at player +0x1cd4) at +0x2360 (1.9.0
0x7100cd39fc). His game sends which are on and n; the others play the same effects on his Link with `0x7001100bd0`
(x8 the handle out, x0 the actor, x1 the effect's name) and end them with `0x700056d010`(handle). For that the
OnlinePlayer has the player's ElinkUser, `Player_GameRom`.

The Master Sword's beam is a `PlayerBeam` actor (a Bullet; the sword's `MagicName`), which the player doesn't link
and which the game doesn't make through 0x7001535900 / 0x7001535b10 (those only preload it). Its AI action,
`PlayerBeamMove` (crc 0xc38e3702 in the action factories: 0x7000b6d698, vtable 0x7001c1df30), has a method the AI
asks while it flies (0x7000710498, x0 the action, its actor at +8): the patch `beam_move` puts the beam in the arrows'
ring, and it goes as a shot (`ONLINE_K_BEAM`). The others show `OnlineBeam`: PlayerBeam's ELink and SLink with no
model, AI or attack, on the matrices that come.

### Throws and shots

What a player throws or shoots goes with his state (OnlineShots: up to 8, each by an id of his, as it is in the world;
the bombs that went off lately, each by a number that only grows, sent for two seconds). His game finds them where
they are made, through two patches (tools/patches.py; sites in tools/game_sites.py, an optional group each):

- `remote_bomb_made`: the Remote Bomb rune (1.9.0 0x7000b3ba04) makes its bombs with 0x7001535900 ("RemoteBomb",
  "RemoteBombCube", "...2"); right after, the bomb (x22) is noted (`rt_online_bomb`). It is followed until it goes;
  it went off when it starts its AS "Explode" (it goes some frames later), or when it is gone without it.
- `arrow_move`: each update of an arrow's flight action (0x70005ee5dc; x0 the action, its actor at +8; the straight
  part and the fall) notes the arrow (`rt_online_arrow`, in a ring: the actors update on several threads); the frame
  loop takes those first seen within 4 m of the local Link (his, not an enemy's) and follows them.

(The player links at +0xea8 what he holds or could pick up: a bomb in his hands, an arrow on the ground before him;
not his arrows in flight, and a bomb only while it is in front of him.)

The others show them with model-only actors (OnlineGear_* for the arrows; OnlineProj_<bomb>: the bomb's model, AS
list and AS, its ELink "Bomb" and SLink, without its AI and physics, from tools/online_mod.py) on the matrices they
come with (moved between two snapshots). A bomb gone off: the actor that showed it (a second actor of it would not
be drawn, nor its effects), its materials hidden, at that place, plays its AS "Explode" (Bomb_Explode: the
explosion's effects and sound), without the Explode actor that hurts.

### Magnesis

What a player holds with the Magnesis rune goes with his state (protocol 19, STATE's `grab`: the actor's name, where
it was when he caught it, its matrix now; nothing held: no name). His game reads it from the rune's object
(0x7001e15f18: its +0x318 the magnetic part of the actor held, as 0x7000b993e8 checks; on while its +0x10 bit 0; the
actor at its +8). The others' games take the actor of that name nearest that place (within 4 m; looked for again each
second while it isn't found), and each frame while he holds it:

- its rigid body (the actor's +0x190) gets the velocities that take it to his matrix in a step (0x70012d4e18, as the
  game does for keyframed motion): it moves there with its physics, pushes what it meets, and falls from where he lets
  it go (setting the actor's matrix alone, 0x700152261c, is undone by its body);
- it glows as held: its +0x710 bit 2 (0x700152a08c, as the rune does at 0x7000b98c40) and its materials'
  `uk_special_effect_B` at 1 (0x700124d220: s0 the value, x1 the model); both back to 0 when he lets go, as the rune
  does (0x7000b98de0);
- the rune's "MagneCatchMoving" sparkles where it is, played on the actor the rune plays them on
  (`**0x7001d66ab8 + 0x30`, ReactionHit; 0x7001395354, the position at the effect's +0xbc with +0x98 / +0x9a bit 8).

`tools/online_bot.c` BOT_GRAB=<actor>,x,y,z holds that actor 10 s of every 20, going round a circle over it.

### Stasis and Cryonis

Stasis (protocol 22, STATE's `stop`, as `grab`): an actor near the player drawn as stopped (its +0x710 bit 3, as the
rune sets it; bit 4 while he aims; a model and a body, not a tag, and none shown for another) goes to the others, whose
same object is held where it is in his game (its body, 0x70012d4e18, each frame) and drawn as stopped: +0x710 bit 3 and
its materials' `uk_special_effect_B` at 1 (its glow; `uk_special_effect_A`, 0x700124ca7c per model unit, is the red
pulse before it goes, 0 meanwhile). Its chains (`BitaLocked_Start`) aren't an effect of the object's own ELink.

Cryonis (STATE's `ice`: up to 3 pillars, their matrices): the Obj_IceMakerBlock actors near the player. The others
show each with OnlineIce (tools/online_mod.py: the pillar's model, AS "Appear_Linear", effects, sounds and physics, one
can stand on it; without its AI: the game's own pillar made by anyone but the rune deletes itself at once), gone when
his goes.

### Players on other maps

Each STATE carries the map its player is on (protocol 22, `map`: the game's own, 0x7001208304 its type and
0x70012082f8 its name, e.g. `MainField/`, `CDungeon/Dungeon000`, `MainFieldDungeon/RemainsWind`). A player on another
map (a shrine, a Divine Beast) has no Link in the others' world and has none of its enemies (`can_own`, and the host's
enemies near him aren't made everyone's).

### Bosses (Ganon)

Calamity Ganon, Dark Beast Ganon and the Blights (`is_boss`) aren't the host's shared enemies: each game keeps its own,
fought through the game's own events and phases (the sanctum's EventTag HyruleCastle<BossRoom0>, Demo142_0, the
champions' lasers, Demo717_0 for the second half, ...). Their damage is everyone's (protocol 23, STATE's `boss`): each
game sends, for the bosses near its Link (their map HashId, actor +0x640), the damage its own player has done so far
(what the life at actor +0x848 lost, but for what the others' damage took); what another player's grows by is taken
from that boss in every game on the same map. Every player's blows add up, the phases follow in each game, and a new
phase's life (or a boss made again) takes nothing back. GameClear and the castle's Blights' flags are shared with the
quests'.

Tested with two games in the sanctum: both made Calamity Ganon and played its cutscenes; the host's game's blows
(`SAVAGEWIND_ONLINE_BOSS_HIT` / `_KILL`, test hooks) took the other's Ganon down with them, into its second half.

### Quests

The quests' progress is everyone's (protocol 17): online_mod/quests.txt lists the quests' flags (Quest/QuestProduct.
sbquestpack: the 20 Type Main ones first, then the 60 Sub, the shrine quests, and the 90 Mini, the side quests; each
step's NextFlag, and the bool flags their steps depend on, Clear_Remains* and the like; 666, all bool flags; up to
1024). Every 2 s each game sends a bit per flag it has (QUEST, 128 bytes) to the host's game, which ORs
them with its own and sends the union (QUESTS); each game sets the ones it lacks, as an event's Demo_FlagON does (EventFlagONAction, 0x7001187758: the handle from
0x7001143950(the pending buffer, the name's crc32) with the manager's +0xc28 in its top byte, then 0x7000730148(the
manager, 1, handle)), which tells the rest of the game (a value written in memory alone is seen by nothing: the
slate's HUD didn't show). A bool flag's value in memory:
GameDataMgr (`test_warp_gdt_mgr`: the singleton TimeMgr::calc loads) +0xbd8, its pending-write buffer: +8 a count
(42031 in 1.9.0) and +0x10 an array of proxies, 0x18 each (+8 the flag's definition, its name's crc32 at +0xc; +0x10
the value), sorted by that hash, found by a binary search. What a flag does in the world beyond the quest (a Divine
Beast calmed, a door open) may only show after a load. With them go the Great Plateau's start (STORY_FLAGS in
tools/online_mod.py): the Sheikah Slate's pedestal (Demo700_0: IsGet_PortableUnit, PortableUnit_PowerOn, OP_DRC,
Open_FirstDoor) and its first door (Demo701_0: Open_Door), their cutscenes seen (IsPlayed_Demo700_0 / 701_0); the slate's flags are set on
the others once the one who took it is through its cutscene (IsPlayed_Demo700_0, set at its end, has come): no
cutscene of theirs, the slate and its HUD at once, the slate itself into their pouch (Obj_DRStone_Get, with the pouch's
own can-get 0x700120af80 and get 0x700120af98, as EventIncreasePorchItemAction does: the flag alone shows it only after
a reload), then the game's own voice line and tutorial (Demo318_0). (Its
pedestal's event can be started on them instead, as an EventTag starts its own (0x70011662e0: metadata from
0x7001112508, named with 0x7001112794(meta, flow, entry, ""), 0x700110d590(the event manager, request: +0 a
matrix, +0x30 0x101, +0x38 the metadata, +0x48 the actor), freed with 0x70011128b4): SAVAGEWIND_ONLINE_SLATE_EVENT=1;
SAVAGEWIND_ONLINE_SLATE_TEST=<s> starts it that many seconds in.) And
the towers (Demo004_0, a tower's map downloaded: MapTower_01..15, MapTower_DemoFirst, MapTower_07_Demo; the first
one rising, IsPlayed_Demo164_0 / 166_0); and the slate's runes and upgrades (IsGet_Obj_RemoteBomb / Cube / Lv2,
Magnetglove, StopTimer / Lv2, IceMaker, Camera, PictureBook, SheikSensor / Lv2, SheikPadLv2, Motorcycle; Hateno's
power-up, Demo032_0).

Sheikah pedestals (protocol 18): a game in the slate's pedestal event or a tower's (`framepace_event_name`: Obj_DRStone_A_01, Demo700_0,
Demo701_0, Demo004_0, Demo164_0; not a chest, a talk or any other) says so in its state (effects ONLINE_E_DEMO).
While another player within 30 m is at one (and this game is in no event), this game's A is held back (`g_online_block_a`, runtime/hle/input.c), so its player can't start the same thing
meanwhile (BOT_DEMO=1: the test bot is in one 20 s of every 40; SAVAGEWIND_ONLINE_DEMO_LOG=1 logs the check).

Also shared (protocol 20+, 4096 flags): the runes and the slate's upgrades (the flag, and the item through the pouch,
0x700120af98: the flag alone leaves the slate's runes as they were); every Korok found (MainField_Npc_HiddenKorok*:
the seed through the pouch; with that Korok within 60 m, its HashId at actor +0x640, its own cutscene instead,
Demo017_0<HiddenKorok_Ground / _Air> started on it with 0x700110d500 as 0x70008f682c does, which gives the seed and
sets the flag; retried while another event plays); the shrines activated, entered and cleared (Clear_Dungeon*: a
Spirit Orb, Obj_DungeonClearSeal); the Divine Beasts (Clear_Remains*, IsGet_Obj_HeroSoul_*: the champion's ability
through the pouch; Get_HeartUtuwa_*: a heart container through 0x7001211520, as EventIncreasePorchItemAction does: the
pouch alone doesn't raise the hearts). A flag's word has other bits: only bit 0 is its value.

Enemies dead everywhere: the host sends each enemy's map HashId (`OnlineEnemy.hash`); one that died in any game gets
its dead flag (`<map>_<actor>_<HashId>`) set in every game: the game doesn't make it again until a Blood Moon, and a
camp's chest (its SyncLink group) opens. A dead player's game has no enemies (`can_own`: they go to another player at
once, not frozen at his game over); one gone from its owner's game still alive (he tries again, his world reloads) is
the host's again instead of gone for everyone.

### Loot

What enemies drop is everyone's (protocol 15). When an enemy a game has for real dies there (life 0, or gone), that
game watches where it died for 10 s: the items that come within 6 m (Item_*, Weapon_*, PutRupee*, those not there
when it died) and the weapon and shield it held are its loot (those kept while it lives: as it dies it drops
them, and they are unlinked by then). The host's game keeps the list (LOOT_ADD from the
others, each with the game it came from and its actor's id there; up to 32, the oldest goes), and sends it (LOOTS)
when it changes and every 2 s. Every other game within 150 m makes each one (the item itself, asked for by name)
where it was, a real item its player can pick up. One gone from a game with that game's Link within 4 m of it was
picked up: a weapon or shield is taken off the list (LOOT_TAKE), and every game deletes its own of it, the one it
came from too; food and materials each player picks up his own. The list holds 64 (more stay only where they fell:
taking one off would have it go everywhere).

Weapons and shields lying in the world (protocol 18): each game watches the Weapon_* actors within 4 m of its Link
that he doesn't carry; one gone with him within 2.5 m was picked up (PICK: its name, where it was), and the other
games delete the one of that name within 2 m of it.

### Enemies

The host's enemies are everyone's (protocol 11+, ENEMIES, 10 times a second from the host's game, through his server
to the others). Every actor the game has is reached through its proc units: 0x58 bytes each, all in one list (+0x10
/ +0x18 the previous / next one's +0x10, the list's head no unit; +0x40 the actor's id, +0x48 the actor), from any
actor's (+0x68). The host sends up to 32 `Enemy_*` within 150 m of any player, the nearest: each one's id, name,
matrix, the AS it plays (slot 0) and the weapon it holds (a link at enemy +0xb98, found once by looking through one)
with its matrix. The others delete their own enemies within 170 m of the host's Link (each frame: those the game makes
there go as they come) and show his with `OnlineEnemy_<name without Enemy_>` actors (tools/online_mod.py,
`enemies.txt`): the enemy's own pack and ActorInfo entry (its profile, physics, damage params and life: they are hit as
it is), with the AI that only stands, no AI schedule, no drops, and its AS each as one clip. Its physics is its own copy
with no character controller (with one it stays where it stands, whatever matrix it is given, and its weapon, which
follows the host's, floats away from it) and its attack bodies on SensorNoHit (its attacks land through the runtime);
the bodies it is hit on are kept. (Its actor link written
again is bigger than the enemy's: its RSTB size is the enemy's plus twice the growth, else the Tags parse runs out of
the resource's memory and the game crashes. `SAVAGEWIND_ONLINE_ENEMY_TEST=1` makes each one in turn next to Link.) An enemy's AS choose
their clip by asking its AI (its weapon, its state: TypeIndex 98, 47, ...), which a puppet has none of, so each one is
made `OE_<its file>`: its element tree followed from the first through each choice's "default" (or first) child to the
first clip (TypeIndex 67), in an AS list of the puppet's own that keeps the enemy's animations (AddReses). His weapon
is an OnlineGear actor. When his feed stops (he left, the connection went), they stay as they were, and the own ones
there stay gone, until it comes again.

Owners (protocol 13): each of the host's enemies is had for real by one game, its owner's, the player nearest it
(another one only when the owner is 40 m from it or more, or another has been 15 m nearer for 5 s, and not again for
10 s: each change makes it anew in one game and deletes it in another, so players fighting it together keep it with
one of them). The host's game keeps
the list (its own enemies found near a player, their ids everyone's) and decides; each entry of ENEMIES says its
owner's slot. The owner's game runs it (its AI sees him, chases him, hits him as any enemy) and sends its state to the
host's game (ESTATE: id, name, matrix, AS, life, weapon; life -1: gone there, died), which sends it on; the others show
it. A game that comes to own one makes it as it was (where it was, its life) with its weapon (an InstParamPack, as
0x7000b1711c makes one: EquipItem1 the weapon's name; the pack's vtable the GOT's 0x7001d664b0 + 0x10, then s16 count,
s16 bytes used, entries of key char *, type 5, the string); the one that no longer owns it deletes it with what it
holds (its weapon, shield and bow at +0xb98 / +0xbb0 / +0xbc8 as the player's: else they stay on the ground, one more
each time it changes hands). Hits go to the owner's game. A copy whose enemy died in its owner's game (life 0: its
body there a ragdoll, its root going anywhere) stays where it was, plays its own Die, drops its weapon and shield, and a second later vanishes as the game's dying
enemies do (its XLink keyword "Disappear": 0x700159f00c(actor, "Disappear", 2, 0x20 bytes of 0), as 0x70015302c0
does it), hidden then deleted (`SAVAGEWIND_ONLINE_KILL_TEST=<s>` on the host's game has its own enemies die that many
seconds in). What an enemy holds (its weapon, linked at +0xb98, its shield at +0xbb0) goes in the space of the hand
bone it is in (Weapon_R / Weapon_L, within 0.6 m), else where it is in the world: a copy plays its own AS, so its hands
are elsewhere than the owner's enemy's, and each piece goes in its own hand. It is held at the enemy's size for it
(online_mod/wscale.txt: the gparam's WeaponScaleSmallSword / LargeSword / Spear / Bow / Shield, a Moblin's or a Lynel's
bigger than in Link's hands; the weapon's own matrix has none of it, and a scale put in the matrix isn't drawn: its
model's root bone is scaled, 0x7000f95fa8 with the scale). A copy made anew (the game removed one, or it came back)
takes no hit for the life it starts with. A new owner makes
its enemy with both (EquipItem1, EquipItem2). Protocol 14. After a long jump taken for a load, the owned ones still
there are deleted.

Hits (protocol 12, HIT): an actor's life is the s32 at +0x848 (its General Life to start with; seen going down as it
is hit). The others set each puppet's life to the host's enemy's every frame (at least 1: his dies in his game); what
it lost since goes to the host's game (HIT: its id, the life it took) through his server, and his game takes it off
his enemy's life (at 0 the enemy's own AI has it die, drops and all; it doesn't flinch).

Their attacks on the others: a player's own life is the same s32 (player +0x848, quarter hearts). When one of the
host's enemies starts an AS with Attack or Kick in its name, a third of a second later (STRIKE_FRAMES) the local
Link is hit if he is within its reach (2.2 m, 3.5 m for the big ones, more with a weapon, a spear or a big sword
most) in front of it and not guarding (an AS of his with Guard): his life goes down by its weapon's attack power, its
own when it holds none (online_mod/power.txt: the enemies' Attack Power, the weapons' ActorInfo attackPower). At 0 the
game has him die as from anything else (Mipha's Grace included). A heavy blow (a kick, a Large or Jump attack, any
of a big enemy's) shakes him: `OnlineShock` (the enemies' shock wave, EnemyShockWave, its ELink, SLink and XLink Dummy:
nothing seen) a metre from him on the enemy's side; he flinches (DamageS), no life taken. Knocking him down takes a
blast: `OnlineKnock` (a Remote Bomb's, ELink / SLink / XLink Dummy, BombExplode's UseDefaultEffect false) does it
(DamageL / SlippingDownLand / DamageLStand), but its attack is an explosion's: the game shows the explosion on him and
takes a quarter heart whatever its params (SAVAGEWIND_ONLINE_KNOCK=OnlineKnock to use it;
SAVAGEWIND_ONLINE_KNOCK_TEST=<s> makes one next to the local Link every that many seconds). The bosses' shock waves
(SiteBossSpearShockWave, GanonShockWave, PriestBossShockWave) only make him flinch too. No defence from his armour yet.
`SAVAGEWIND_ONLINE_ENEMY_AS_LOG=1` logs the host's enemies' AS as they come, `SAVAGEWIND_ONLINE_LIFE_LOG=1` the local
Link's life.

The time of day is the host's (protocol 10): his game's frame loop publishes its hour (`test_warp_hour`), his
server sends it to everyone every 2 s (TIME), and the others set theirs to it (`test_warp_set_hour`) when it is off
by more than 3 game minutes (both clocks run at the same speed in between).

Players going: a player who sends no state for 3 s isn't shown (his actors to his pool, `STALE_NS`), and the server
drops a client it hears nothing from for 10 s (a game that hung, a network gone without closing the connection) and
tells the others he left. A game out of the session (it left, or lost the connection) deletes the others' actors at
once instead of forgetting them (they would stay where they stood); a slot taken by another player before the first
one's leaving was seen deletes the first one's actors.

When the game deletes a remote Link (the local Link died: game over; a load), the world's actors are going: all are
forgotten and made again two seconds later, and nothing is made while the local Link is dead (his AS Dead...);
making them again during it crashed the game. The arrow on
a drawn bow's string is no actor in his game: the others put a NormalArrow from his right hand toward his left while
his upper body plays BowReload / BowCharge.

### Not done yet

- Less to send: only the bones that moved, or fewer times a second (a session of 8 sends each player's skeleton to 7).

- A dye's colour (the armour's `ArmorDyeColor` param when it is made, as `0x70011759a4` passes it).
- The beam is not seen working yet: in the test runs (full hearts, the Master Sword) the game fired none, with or
  without online play, so `beam_move` and OnlineBeam are untested.
- Magnesis' blue beam from the slate to what it holds (not a named effect of the player's, ReactionHit's or
  Item_Magnetglove's ELink: 0x7001395354 / 0x7001100bd0 find none of MagneHand, MagneConnect, Item_Magne(_Main,
  _Tail); the Item_Magnetglove actor's ELink plays it from its AI's state), Stasis' chains.
- A Divine Beast seen moving by the others while one is inside: each world's beast is as its own flags are (the
  quests' flags are shared), its AI isn't followed.
- Mipha's Grace, Revali's Gale, Urbosa's Fury (the same way as Daruk's: "Kago_Heal", "Kago_Wind", "Kago_Lightning"),
  a thrown weapon's flight, other effects (a sword's trail, a spin attack's light).
- The world shared: enemies, items, time of day, quests (Milk Bar's quest and enemy sync); damage between players.
