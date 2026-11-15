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

Each player plays their own game (their save, their world; shared with the others: the host's enemies and what they
drop, the quests' progress, the treasure chests opened, the time of day and, near the host, his weather, weapons
picked up); the others are
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
no bone (farther than 0.6 m from every bone; a shield 1.5 m, so the one he surfs on, under his feet, stays) but within
60 m of him is sent where it is in the world (`gear_bone` ONLINE_GEAR_WORLD, `gear_rel` its world matrix; an older
game reads it as no bone and hides it, as before). A thrown weapon leaves his equipment as it leaves his hand, so
his game keeps it as his weapon while it flies and until it has lain still for 2 s; then it is an item in his world
only, which the others couldn't take, and goes out of their sight. Tested: a traveller's sword thrown 18 m was placed
along its flight in the other game and came to rest where it lay in the thrower's (-974.1 231.6 1875.4 in both).

The paraglider is a sixth piece (protocol 8): the player links his `PlayerStole2` (or `_Vagrant`) actor, at player
+0x2c28 in 1.9.0 (found once by looking through the player for a link to it); it is sent only while his body plays
a `Para*` AS (`ParaEquipOn`, `ParashawlGlide`, ...; not `ParaEquipOff`), on the bone it is nearest (3 m reach).

The online actors have `traverseDist` 1000 in their ActorInfo: without it the game deleted the gear farther than
about 50 m and the remote Link farther than about 80 to 140 m from the camera, and they were made again and again
(a piece left standing where it was deleted). The `OnlineEnemy_*` actors too: with their enemy's own (150 m for
most) the game deleted those of the host's enemies farther than that from the camera, which the host sends from 150 m
of any player, and they were made again and again (about twice a second each, Great Plateau test 2026-11-13). Requests still under way when everything is forgotten (a load, a
shrine) are kept (`g_orphan`) and what they make is deleted when it comes, so no stray piece stays in the world.

The Master Sword's online actor has the sword's own ELink, SLink and XLink (`GLOWS` in tools/online_mod.py).

Daruk's Protection (protocol 9: STATE's `effects`): the player shows it with effects of his own ELink, each kept as a
handle (the effect, its id; alive while the effect's +0x20 is that id): "Kago_Shield" (a hit taken on it) at player
+0x2350 and "Kago_ShieldCount_0<n>" (its ring while he guards, n charges left at player +0x1cd4) at +0x2360 (1.9.0
0x7100cd39fc). His game sends which are on and n; the others play the same effects on his Link with `0x7001100bd0`
(x8 the handle out, x0 the actor, x1 the effect's name) and end them with `0x700056d010`(handle). For that the
OnlinePlayer has the player's ElinkUser, `Player_GameRom`.

Revali's Gale the same way (`effects` ONLINE_E_GALE): its charge's handle is at player +0x23c0 (found by diffing the
player's memory before and while charging, 1.9.0), and the others keep "Kago_Wind" on his Link while it is alive. The
handle at +0x2380, new each time the Gale is let go, is the updraft's sound ("Kago_WindRiseUp" is only in SLink2, not
ELink2), so it isn't sent; his rise and the paraglider he opens come with his position and gear. Tested with two
games (the client given the Gale with `setflag IsGet_Obj_HeroSoul_Rito` on the host, through the shared flag):
"Client: Kago_Wind" on the host as he charged. An older game ignores the bit.

The effects a player and what he holds play (protocol 25): every effect and sound any actor plays goes through
xlink2::UserInstance::emit (1.9.0 0x7000f51054, its "emit(%s)" log line; `xlink_emit` in tools/patches.py): x0 the
user instance, x1 the locator (+8 its asset call table, +0 of that the asset's key name). An actor's ELink user
instance is its +0x568 +0x48 (0x7001100bd0 plays an effect by name through it), its SLink's +0x50. The runtime notes
the emits of the local Link's ELink and of his weapon's, shield's and bow's (`SAVAGEWIND_ONLINE_FX_LOG=1` logs them;
`SAVAGEWIND_ONLINE_FX_ALL=1` logs every actor's, with its user instance, to find which one plays an effect).

- His other effects (a spin attack's charge, "ChargeLvUp" at each level, and its spin, "Kaitengiri_<level>"; what a
  heavy weapon's blow plays...) go as events (`fxev`: the last four, each by its number), and the others play each one
  by its name on his puppet (it has the player's ELink, `Player_GameRom`) or on the weapon in its hand, ended after
  2 s. Not sent: his trail (below), the Champions' abilities (`Kago_*`, above), the environment's (`MaterialEnv`,
  `FieldEnvEffect_*`). The player's ELink containers choose what to play by its property values: his ELink user
  instance's +0x70, 24 words (6, 0x11, a float that changes...), all 0 on a puppet, so nothing played there; they go
  in his state (`fx_props`) every frame and onto his puppet's. Tested with two games and seen on screen.
- His weapon's swing trail ("Blur_Default", the weapon's own ELink's, as each swing starts) goes in his state (`fx`)
  for 0.3 s and is played on the weapon in his puppet's hand (the swung gear, `SWUNG` in tools/online_mod.py, has its
  weapon's ELink). It is SwordBlur's, drawn by the game's own code from three things a puppet lacks:
  - its actor's AnimInfo: the trail data (+0x570 +0x148 +0x2c0, 0x700152ff4c; the AnimInfo resource, the class made
    where ActorParam registers `baniminfo`) has, for each attack animation, its frames with a trail (114, from
    `GameROMPlayer.baniminfo`). The puppet has the player's (`AnimationInfo` in tools/online_mod.py), and the effect's
    user data (+0x190, its actor at +8) is set to the puppet instead of the weapon;
  - the animation its model plays in its slots 0-2 (+0x4e0 the model, +0xd0 +0x48 the slots, 0xa8 each: the
    animation's index << 16 at +0, its frame's float at +0x20): the emitter callback (0x7001510a90) finds the trail
    entry by them, and the draw callback (0x7001511150) checks the slot again. A puppet is posed from his skeleton
    and plays no AS (an AS played on it poses its bones over his skeleton: his head and arms went their own way), so
    his slots go in his state while the trail plays (`fx_anim`, `fx_frame`) and the four reads of a puppet's slots
    get his (`blur_anim`, `blur_frame_a`, `blur_frame_b`, `blur_anim_draw` in tools/patches.py).
  Tested with two games and seen on screen.
- His sounds the same way (events `who` 2 and 3): his Link's SLink (user instance at +0x568 +0x50: his voice, PV*;
  his equipment's rustle, EquipNoise_*; a charge's ChargeLv*) and his weapon's (PlayerAttackNormal*,
  PlayerAttackCharged...), played on his puppet (SlinkUser `GameROMPlayer`, tools/online_mod.py) or its weapon (the
  swung gear has its weapon's SLink) through 0x700159f00c, the game's own keyword emit (w2 1: the SLink alone; x3 two
  handles, the SLink's at +0x10). Its generic emit, 0x7000f3fc10, on an SLink user instance crashed the game (a write
  at 1). Mipha's Grace and Urbosa's Fury go as events too (Kago_Heal, Kago_Lightning...: only Daruk's Kago_Shield* and
  Revali's Kago_Wind, sent by their handles, are left out). Tested with two games: the host's voice, swings, charge
  and steps' rustle played on the client's puppet of him, each with a handle.
- `SAVAGEWIND_EFF_ERR_LOG=1` logs the effects' error lines ("EFF_ERR[...]", made with
  sead::FormatFixedSafeString<128> at 0x7000579614 and never shown: `fmt128` in tools/patches.py).

Not his: footprints, grass and water as he walks or swims ("FootEffect", "Footprint", "GrassTrack", "WaterTrack",
"Splash") are played by `ReactionField`, an actor of the game's that plays the ground's reactions for everyone (its
user instances at its +0x48 / +0x50), not by the player's ELink. Grass cut by a blade is the grass's own, still to
look at.

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
  loop takes those first seen within 4 m of the local Link while he uses his bow (one of his AS channels in a
  `*Bow*` state: BowCharge, BowShoot, BowReload, SquatBow*, HorseBow*, BowBoard*, now or in the last second) and
  follows them. An arrow failing either when first seen is remembered as not his (its actor and id), since the ring
  notes every arrow again on each update. Without both checks an enemy's arrows were sent as his shots (and an
  enemy's fire arrow landing by him gave his next hit on a puppet its element): 16 in one test with no check, 7 and 4
  with the distance alone (an archer on a tower right above him is within 4 m).

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
Only the map's bosses, those with a HashId, are tracked: a Blight's drones (`Enemy_SiteBoss_*_ChildDev`) have none,
and nearer than the Blight they took both of the two places, so its damage wasn't shared while they flew (and the
test hooks hit a drone). To get there from any save: `SAVAGEWIND_TEST_WARP=-254,195,-1040` (the sanctum's floor),
then `swctl teleport -254 297 -1058` into the Area at its top that starts HyruleCastle<BossRoom0> (again if it says
the position failed to verify). With no Divine Beast freed the castle's Blights come first: the host's game's
Windblight halved to 0 took the other's to 0 with it, and both went on to Waterblight (2026-11-13).

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

Tested 2026-11-14 with two games on the Great Plateau, the client doing everything and the host only receiving:
the tower (MapTower_07, MapTower_07_Demo, MapTower_DemoFirst, IsPlayed_Demo164_0), the Old Man's talk on it
(FindDungeon_Activated), the Mah Ounu shrine (CDungeon/Dungeon038) activated (Open_Dungeon038: in the host's world
the shrine stood open and lit), entered (Enter_Dungeon038), its rune (IsGet_Obj_Magnetglove, Magnesis in the host's
slate) and its clear (Clear_Dungeon038; the Spirit Orb was in the host's pouch, x1). Koroks, both ways a game
learns of one: the host 160 m from it got the seed through the pouch (MainField_Npc_HiddenKorokGround_20346760);
15 m from the next, its own Demo017_0 played on that Korok ("its Korok is here, its cutscene"). Each game then had
2 Korok seeds, none twice. All four plateau shrines (Dungeon038, 041, 009, 065) went the same way, driven
through the control port (teleport to each pedestal, guidance stone and monk from the map data): each rune (Magnesis, Remote Bombs, Stasis, Cryonis) and Spirit Orb in the host's
pouch. A Goddess Statue's trade is each player's own (the orbs are shared, the heart or stamina each one picks).

The paraglider: with FindDungeon_Finish from another player (the plateau done, the king's gift) a game puts
PlayerStole2 in the pouch; before, the host had the quest done and no way off the plateau. IsGet_PlayerStole2 says
nothing about it (set long before in both games, read live with the `flag` control command) and isn't shared.
Tested: the host, from a save at the tower, got the paraglider when the client past the king joined, and glided
down from the tower with it.

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
came from too; food and materials each player picks up his own. A weapon picked up into a free hand isn't gone: the
game keeps the same actor, in Link's hand now (+0xb98, +0xbb0, +0xbc8, their sheaths at +0x958: `link_carried`), so
one that comes to be there was picked up too; before, those pickups, the most common ones, went unseen and the
weapon stayed in the other games. No game deletes one this Link carries. The list holds 64 (more stay only where
they fell: taking one off would have it go everywhere). `SAVAGEWIND_ONLINE_WEAPONS_LOG=1` logs the Weapon_* actors
within 10 m of Link every 5 s.

Weapons and shields lying in the world (protocol 18): each game watches the Weapon_* actors within 4 m of its Link
that he doesn't carry; one gone with him within 2.5 m, or in his hand now, was picked up (PICK: its name, where it
was), and the other games delete the one of that name within 2 m of it (not one of the loot's: the loot list has
those, and a PICK of one deleted another loot weapon next to it). And those within 6 m of the enemies it has for real: one that
comes to be in such an enemy's hand (a camp's weapons lie on the ground until its enemies are alarmed) is a PICK
too, where it lay. Without it the others' games still had the camp's weapons lying there, and each one came to be
there twice once it was the dead enemy's loot (the Great Plateau's cliff camp: 3 Bokoblin clubs, 2 shields).

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
(another one only when the owner is 40 m from it or more and the other 10 m nearer, or another has been 15 m nearer
for 5 s, and not again for 10 s; with the owner far and no one clearly nearer, two players both about 40 m from a
guard had it go back and forth every few seconds; each change is logged with why and both distances: each change makes it anew in one game and deletes it in another, so players fighting it together keep it with
one of them). Never to a player more than 80 m from it (`OWN_SAFE`), unless its owner left: a game removes an enemy
it makes too far from its camera, short of its traverseDist (a Bokoblin made 136 m off went at once), and the one
deleted for it in the other game was gone with it, for everyone. One no player is near enough to have (its owner's
game lost it alive, far from the camera, or its owner left) is no one's (`OWNER_NONE`): kept as it was, shown so,
and the next player within 80 m of it has it; it leaves the list only when no player is within 150 m. Dropped at
once, it was gone for everyone (the map's one in the host's game was deleted when it went to another), and its camp
could never be cleared. The map's own ones the host's game removes or kills leave the list as before. An owned one
gone with life left isn't a death for its loot. The host's game sets the dead flag of one that dies there itself,
by the map HashId the list keeps: the game sets it only for the map's own, and a camp whose enemy was made again
wasn't cleared in the host's game (its chest shut, Demo040_0 not played) while it was in the others'.
`SAVAGEWIND_ONLINE_KILL_TEST=<s>` counts seconds now (it counted calls, whose rate varies). The host's game keeps
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
his enemy's life (at 0 the enemy's own AI has it die, drops and all; it doesn't flinch). The puppets have no chemical
(ActorLink ChemicalUser Dummy): a fire both games have, the camp's, burns the enemy in its owner's game already, and
a puppet burning in it too sent that again as a player's hits (`SAVAGEWIND_ONLINE_FIRE_TEST=<s>`: a lit FireWood under
the first puppet that many seconds after it is made, in this game only; before, its owner's enemy died of it, with
nobody near). A player's element arrow on a puppet sends its element with the hit instead (protocol 24, HIT's
element, ONLINE_ELEM_*): his game keeps where its own fire, ice and shock arrows were in the last half second, and a
puppet hit within 2.5 m of one was hit by that element. The owner's game makes a source of the element a metre over
its enemy, deleted 4 s later: fire `FireRodLv1Fire` (burning, no damage of its own), electric `ThunderRodLv1Thunder`
(shocked, none of its own), ice `ExpandIceField` (frozen; its own 10 damage is taken back for its first second, the
arrow's damage being the hit's). Found with `SAVAGEWIND_ONLINE_ELEMENT_TEST=<actor>[|<actor>...],<s>` on the
Great Plateau's Bokoblins: `ExpandFireField` burns too but with its own damage, `ExpandElectricField` killed one of 13
life, the rods' ice froze nothing, and the arrows (FireArrow, IceArrow, ElectricArrow) made in place do nothing without
a bow's shot. `SAVAGEWIND_ONLINE_ELEM_HIT_TEST=<s>` sends a hit of 1 with fire, ice and electric to three puppets: in
the host's game each was set burning, frozen and shocked, and each was left at 12 of 13 life. And with a real shot:
the other game's Link, with a bow and fire arrows, shot the host's guard on the Great Plateau's cliff camp tower
(40 m, at night): his game sent 5 (fire), the host's took 5 of its 13 and set it burning, and its puppet in the
shooter's game played Burn and BurnEnd.

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

The weather: each player's state has the weather where he is (`weather`, ksys::world::Manager +0x7d1, 255 unknown);
a game within 500 m of the host takes his (written as runtime/weather.c holds one: +0x7d1, its timer +0x794 at 4),
not in a cutscene (+0x7d6) nor while the player holds one himself (Cheats > Weather). The game's weather is a
region's, each game draws its own: far from the host each keeps his. Tested: the host's rain on the client.

The armour's defence: a blow of the host's enemies on a player takes his armour's defence off (the pieces he wears,
online_mod/defense.txt: each one's ActorInfo armorDefenceAddLevel, the defence the game shows: the Hylian Hood 3,
the Old Shirt 1), a quarter heart at least.

Horses (protocol 30): while a player's AS is a horse's (Horse*), the player's horse nearest him (GameRomHorse*, looked
for among the actors a few times a second) goes in his state (`horse`, `horse_m`, `horse_as`: its actor, its matrix,
the AS it plays); the others show it with `OnlineHorse_<its actor>` (tools/online_mod.py: its model and its AS each as
one clip, as the enemies', no AI nor physics; gear.txt; the Master Cycle the same, GameROMMotorcycle, while his AS
is a Motorcycle one), where his is, playing his AS; his Link rides it posed by his
skeleton. Not its saddle and reins (actors of their own). `SAVAGEWIND_ONLINE_HORSE_TEST=<horse>` (test): that horse
beside the host. Tested: GameRomHorse01 and the Master Cycle shown beside the host's Link on the client.

ReactionField (footprints, grass, water): its ELink plays them with its own matrix at the origin (0 0 0, each effect
put in place through its handle after), so who and where aren't known as it plays them; not shared.
`SAVAGEWIND_ONLINE_RF_LOG=1` logs its effects with its place and the local Link's.

Damage between players (protocol 29; Settings > Online > Combat, `pvp` in online.ini, off by default, only between two
players who both have it on; `SAVAGEWIND_ONLINE_PVP` for tests): the puppets have no bodies a blow lands on, so as a
player's swing starts (his weapon's trail) the nearest other player within its reach in front of him (2.2 m and his
weapon's, as the host's enemies' reach) is hit: the blow goes in his state (`pvp_seq`, `pvp_target`, `pvp_damage`: his
weapon's attack power, power.txt) and that player's game takes it as the host's enemies' blows: his shield up blocks
it, his armour's defence comes off, he flinches. Tested: the host's three blows (4) on the client took 2 each (his
defence 2).

Treasure chests: their flags (`<map>_TBox_<kind>_<HashId>`, 1615, most in revival_bool_data) are in quests.txt with
the quests': one opened in a game is opened in the others (what it held is the one's who opened it). Tested: a
chest's flag set on the host was set on the client.

The world's items (protocol 28: QUEST / QUESTS of 32768 flags, 4 KB every 2 s): the revival flag of each item lying
about (`<map>_Item_*`), ore and object broken (`_Obj_*`), weapon lying about, barrel and crate (`_Barrel_*`,
`_Kibako_*`) is in quests.txt too (21940 flags in all): taken or broken in one game, gone in the others the next time
they load that place (one in view stays until then). A Blood Moon clears each game's own, and the others' bring them
back: what was taken comes back only once every game has had its Blood Moon. Tested both ways (an item's flag set on
the host, an ore's on the client), 60 fps kept.

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

### Testing with two games on one PC

Two headless games, each with its own save folder: `SAVAGEWIND_SAVE_DIR=<folder>` (a copy of a save, never one in
use), `SAVAGEWIND_HIDDEN=1`, `SAVAGEWIND_AUDIO_GAIN=0`, a free port for the session (`SAVAGEWIND_ONLINE_PORT`, not the
one a real session uses), `SAVAGEWIND_ONLINE=host` for one and `=127.0.0.1` for the other, `SAVAGEWIND_ONLINE_NAME`,
and a control port each (`SAVAGEWIND_CONTROL=7777` / `7778`, docs/CONTROL.md). Two games hold about 9.5 GB of RAM.
A game can relaunch itself once at start (a child savagewind.exe): a script that watches the game by its PID should
look for that child.

Driving them (tools/swctl.py --port <p> ...): `teleport` / `goto <place>` (one map only: a shrine's or a Divine
Beast's needs walking in), `look`, `press` / `hold` / `release` / `stick` for input, `set god_mode true`, `time`,
`give`; and for online play `actors <text>` (live actors: where, how far, their address), `flag` / `setflag` (read
a game data flag; set one, which the other game then takes like any flag this player gets: `setflag
IsGet_Obj_HeroSoul_Rito` on one game gave the other Revali's Gale) and `event <flow> [entry]` (an event on Link; one
out of its context can hang). Positions to stand at come from the map data: an object's front is P + d*(sin r,
cos r) for its rotation r, facing atan2(-sin r, -cos r) (a pedestal 1 m, a guidance stone 1.9 m, a shrine's monk
about 4 m: his "examine" shows from 3.6 m, on his stairs).

Logs for it: `SAVAGEWIND_ONLINE_ENEMY_AS_LOG`, `_SHOT_LOG` (each arrow where it was first seen, whether its player
was using his bow), `_GEAR_LOG` (where each piece is put, a thrown weapon followed), `_POSE_LOG` (the local Link's
AS as they change), `_LIFE_LOG`. Always on: each handover of an enemy with why and both distances, each player's
"can own enemies" with why, "our pose" when it stops being sampled.

Recipes that found the bugs above:
- Shots: the host alone under the cliff camp's tower at night (-1203 198 1641, its archers shoot at him), the other
  ~55 m away; count "Host's shot" on the other game (0 expected), then the other under the tower too (0 "Client's
  shot" while he doesn't shoot), then three of his own shots (as many sent as his shot log says he made).
- A shrine: pedestal, elevator, guidance stone, monk, each step waiting until the other game logs the flag set by the
  other player; the Great Plateau's shrines need its tower first (Demo004_0 at its ground pedestal).
- An ability's effects: diff the player's memory (+0x2000..0x2800) before and while it is used; a handle that
  appears is the effect's (Daruk's layout), and its name must be in ELink2 (a name only in SLink2 is a sound).

### Not done yet

- Less to send: only the bones that moved, or fewer times a second (a session of 8 sends each player's skeleton to 7).

- A dye's colour on the others' screens: not seen yet. The player's armour code (0x7000cdc43c) makes each piece with
  0x70011759a4(mgr, slot, name, dye, ...), the dye from player +0x1114 (head), +0x1118 (upper), +0x111c (lower; -1
  none), put in the piece's InstParamPack as `ArmorDyeColor` (s32) when the piece has the tag 0x216a3a63 (dyeable).
  Each player's state carries his three (`dye`, protocol 27) and his puppet's pieces are made with the same param
  (made again when it changes; tested: a dye written on the host made the client's piece again). Whether an
  OnlineArmor (a DemoNPC) shows it is still to see; if not, the colour has to go on its materials.
- A horse's saddle and reins; the world's objects moved (not taken), players in the same shrine
  or Divine Beast is fine (the same map: shown), a Blood Moon at the same time.
- The beam is not seen working yet: in the test runs (full hearts, the Master Sword) the game fired none, with or
  without online play, so `beam_move` and OnlineBeam are untested.
- Magnesis' blue beam from the slate to what it holds (not a named effect of the player's, ReactionHit's or
  Item_Magnetglove's ELink: 0x7001395354 / 0x7001100bd0 find none of MagneHand, MagneConnect, Item_Magne(_Main,
  _Tail); the Item_Magnetglove actor's ELink plays it from its AI's state), Stasis' chains.
- A Divine Beast seen moving by the others while one is inside: each world's beast is as its own flags are (the
  quests' flags are shared), its AI isn't followed.
- Footprints, grass and water as a player walks (ReactionField's), grass cut by a blade.
- The world shared: enemies, items, time of day, quests (Milk Bar's quest and enemy sync); damage between players.
