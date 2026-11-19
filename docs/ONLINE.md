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
his game keeps it as his weapon while it flies and until it has lain still for 3 s. It goes on the loot list (Loot,
below; protocol 34: a loot's rotation) as it leaves his hand: the others' games make the real weapon at once, and while
his flies (his piece sent where it is in the world) they move theirs there through its body (FN_BODY_TO, as Magnesis)
instead of showing his piece, so it can be picked up in the air; one picked up goes in every game, his too. One that
breaks on a blow is taken off the list wherever it is. One thrown as he draws another is followed too (not shown
flying: theirs falls from where it left his hand). A shield or bow let go of (dropped for another) goes on the list
half a second after. Tested: the host's sword thrown 18 m: the client's real one was there at once, 8 m out, then
18 m, and lay where the host's did (-1056.0 249.3 1800.4 in both), 60 fps. Tested: a traveller's sword thrown 18 m was placed
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
player's memory before and while charging, 1.9.0), and the others keep "Kago_Wind" on his Link while it is alive. That
handle is the Champions' ready effect's (Kago_Ready), alive while he guards with Daruk's Protection too: the Gale goes
only when the last Champion's effect his Link played was Kago_Wind (else Daruk's shield drew Revali's wind on him in
the others' games; tested with two games: the client's guard sent Kago_ShieldCount alone, his Gale Kago_Wind). The
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
- Not his continuous sounds (continuous_sound): the game plays them every frame or for as long as he moves, their
  volume his speed - his gear's rattle (EquipNoise_*), the wind of his fall (WindDirect, and FallFlap*, emitted once
  as a fall starts), his slide (ReactionSlide) - and any other his game emits again within 0.3 s, from then on. Played
  by their keyword on his puppet, each was played whole at full volume and they piled up: bursts of white noise, 2.5 s
  each, whenever he walked, jumped, fell or slid (found by recording the game's output, `SAVAGEWIND_AUDIO_WAV`, with
  his sounds let through one at a time: FallFlap alone gave the roar). Tested: walking, three jumps and a fall down a
  slope by the client, the host's recording without a burst. The paraglider's wind (ParashawlGlideMove /
  ParashawlGlideAscendant) is in the known list too: only seen again it was held back, so its first one went, and
  played whole on his puppet it was 4 s of loud hiss as he rose on Revali's Gale (each time, as the 64 sounds
  remembered rotated). Tested with two games (slot 0, the champions' save): the client used Revali's Gale, the host's
  recording had the hiss before, none after.
- One of his effects sent again while the last of it still plays (a Champion's ability used again and again:
  Kago_Ready) ends that one first: they piled up one over the other.
- `SAVAGEWIND_EFF_ERR_LOG=1` logs the effects' error lines ("EFF_ERR[...]", made with
  sead::FormatFixedSafeString<128> at 0x7000579614 and never shown: `fmt128` in tools/patches.py).

Not his: footprints, grass and water as he walks or swims ("FootEffect", "Footprint", "GrassTrack", "WaterTrack",
"Splash") are played by `ReactionField`, an actor of the game's that plays the ground's reactions for everyone (its
user instances at its +0x48 / +0x50), not by the player's ELink: sent their own way (ReactionField below). Grass cut by a blade is the grass's own, still to
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

`tools/online_bot.c` BOT_GRAB=<actor>,x,y,z holds that actor 10 s of every 20, going round a circle over it. (Its
messages are built in a 16 KiB buffer, the runtime's own limit: in 1100 bytes, with STATE grown to 2969, every
message of it was cut, so the host saw it loading for good and never made its Link.) Tested with the bot holding a
crate at the Bokoblin camp: the host's crate was lifted, went round with it and glowed, the bot's Link beside it.

The beam, the magnet and the chains, the game's own (online_mod v41): `OnlineRuneFx` (ReactionHit's ELink and SLink, the slate's
model hidden: an actor with none has its effects made, never drawn) and `OnlineSlate` (Item_Conductor, the slate, in
his puppet's right hand while he holds). The rune's beam is MagneConnect, played on ReactionHit
(`[[0x7001d66ab8]] +0x30`, 0x7001100bd0 at 0x7000d40b94) and placed each frame by 0x7000d40bb8(holder, matrix, s0 the
length): the holder keeps the handle at +0x48 and its id at +0x50, its +0x13c (0 while it holds) goes to the
particles' +0x190 +0x18; the handle takes the matrix at +0xc0, the length at +0x110, bits 8 and 0x400 at +0x180. The
matrix is the world's, whatever actor plays it: at the slate, its z toward what is held, its y as near up as it can
be, its x = y x z; the beam is drawn from there along z for the length, blue at the slate (the magnet between Link's
hands), pink, yellow at what is held (placed the other way, from the object toward the hands, the blue end is drawn at
the object: the first video after this change). For another's grab, grab_update plays MagneConnect on his
OnlineRuneFx (kept at his slate, unturned) and places it so, from between his puppet's hands (Weapon_R and Weapon_L)
to the object here. The blue magnet over the hands is another effect: the key "Hand" of Item_Magnetglove's ELink
(the particles MagneHand), which the rune plays from its action trigger - not by name, so it never shows among the
emits (`SAVAGEWIND_ONLINE_FX_ALL`). Its keys, read from ELink2DB (header 0x60 bytes, its positions 64-bit; 0x5f6 users,
their name hashes at 0x60, their offsets 64-bit after them; a user's header 0x40 bytes, its sort table, then its keys,
0x30 bytes each, the name's offset first, from the names at 0x3a28d8): Chemical_*, Connect, Hand, MagneBall,
MagneHit, ReactionEffect, Default, SlowMotion, FPSOK / FPSNG. `OnlineMagnetFx` (that ELink, the slate's model hidden)
goes between his hands, its y toward what he holds (the U opens along its y), and plays "Hand" while he holds.
Tested with the bot: the U and its glow at its hands, the beam from there to the box.

The beam starts at the magnet's tips (MAGNET_TIP, 0.8 m from his hands toward what he holds; at most half the way),
as in the holder's own game. His slate and these effect actors are made once and kept with his puppet, models
hidden (the slate's drawn while he holds): pooled under the world between his grabs, the game removed them ("the game
removed his OnlineRuneFx, made again") and a piece made again for him is never drawn, so his beam and magnet were gone
for the rest of the session - "sometimes it doesn't show". And what he holds is looked for near where he caught it
or where it is now in his game (15 m; let go, it fell its own way in each world); not found here, his magnet and
beam still point where his is. Tested with the bot over three grabs in a row: the magnet and the beam each time. The Stasis
chains: BitaLocked_Start on another OnlineRuneFx put where what he stopped is (stop_update).

The beam needs the rune's aim: the game keeps two Vector3f at `[0x7001e58720] +0x192cc` (P_MAGNE_AIM,
MAGNE_AIM_OFF), the camera's forward as the local rune was last raised, and MagneConnect's strands leave along it and
bend to what is held. Zero until the local Link first raises the rune this boot: before that every MagneConnect (ours,
the local rune's) is made ("on") and never drawn; after, they stay at his last aim (a remote beam bent along it,
looping round him). grab_update writes his beam's own way there each frame his beam is drawn, unless the local Link
holds something with Magnesis (his own beam then; raising the rune writes its own). Found by raising the rune with
nothing to catch (the bot's beam then drawn, and after cancelling), then a diff of that object before and after: the
two vectors written by hand, the rune never raised, drew the bot's beam. Not it (tried before): the ELinks'
properties, the rune object's values and its handle, MagneMarker* / MagneBall / MagneHit, a real Item_Magnetglove
made, the camera's AI action, the xlink system's words, the particles' resource records. Tested with the bot (the
local rune never raised): its beam from its magnet to the box it held, straight; then a local hold with its beam on:
both beams drawn, the local one from Link to his box.

Weapons and Magnesis: a weapon he dropped or threw is his loot, its copy here (`g_theirs`). One he holds with
Magnesis isn't sent as thrown (gear on no bone, ONLINE_GEAR_WORLD: in the video it took the copy to where the
dropped weapon's old matrix was, in the sky, and the grab never found it, `holds ... not here`), the thrown path
leaves alone the copy his grab moves, and grab_update looks for his loot of that name first (30 m), then the world's
objects (15 m).

### Stasis and Cryonis

Stasis (protocol 22, STATE's `stop`, as `grab`): an actor near the player drawn as stopped (its +0x710 bit 3, as the
rune sets it; bit 4 while he aims; a model and a body, not a tag, and none shown for another) goes to the others, whose
same object is held where it is in his game (its body, 0x70012d4e18, each frame) and drawn as stopped: +0x710 bit 3 and
its materials' `uk_special_effect_B` at 1 (its glow; `uk_special_effect_A`, 0x700124ca7c per model unit, is the red
pulse before it goes, 0 meanwhile). Its chains (`BitaLocked_Start`, ReactionHit's) are played once on his
OnlineRuneFx and placed as the rune places them (chains_place, from 1.9.0 0x70011f5970): the effect's matrix (+0xc0,
flags +0x180 | 8, +0xb4..+0xbc 1) scaled by the stopped object's model's largest extent (the model unit's vtable
+0x208, its box) at the object. Made but never placed, they were never seen. Tested with the bot (BOT_STOP on an iron
box): the chains round the box from the sky, as the rune's own, then the box's glow.

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
picked up: it is taken off the list (LOOT_TAKE), and every game deletes its own of it, the one it came from too -
a weapon, a shield, food, a material, a rupee, as in one world (before, food and materials each player picked up
his own). A weapon picked up into a free hand isn't gone: the
game keeps the same actor, in Link's hand now (+0xb98, +0xbb0, +0xbc8, their sheaths at +0x958: `link_carried`), so
one that comes to be there was picked up too; before, those pickups, the most common ones, went unseen and the
weapon stayed in the other games. No game deletes one this Link carries. The list holds 64 (more stay only where
they fell: taking one off would have it go everywhere). `SAVAGEWIND_ONLINE_WEAPONS_LOG=1` logs the Weapon_* actors
within 10 m of Link every 5 s.

What a player holds from his pouch over his head (up to 5 materials or food, Item_* and Animal_Insect_*) is in his
puppet's hands for the others (sample_held, OnlinePose.held, protocol 38): each material's model alone
(OnlineGear_<its model>, tools/online_mod.py, online_mod v42), heaped on the puppet's hands (between Weapon_R
and Weapon_L) as his game heaps them: three side by side, two over them, as far apart as they are big (0.22 m times
the size). Each at the size his game holds it: the item's own +0x418, a scale on each axis (an apple 0.55, a mushroom
0.372; 1 on the ground), sent in m[0] and written on the piece's own +0x418 each frame. (At 1 a mushroom was a column
over his head; heaped 5 cm apart the apples went into each other, a small red lump.)
One he holds is linked to him (+0x4d4 non-zero) and is sent as "in his hands"; as he throws it or lets it fall it
flies from there: once it has left where it was while he held it, it is sent where it is, until it has lain 1.5 s (by then it is on the loot
list and the others have the real one there). Before, the others saw nothing in his hands, and what he threw only
once it lay (2-3 s: the ground was looked at every 30 frames, 1 s still). Tested with two games: the client held 3
mushrooms, the host saw them over its puppet's hands (OnlineGear_Item_Mushroom_E, 3 made); let fall, the host saw
them fall at once and had the 3 real ones where they lay 2.2 s after (falling and coming to rest included). Not
tested: a throw ("Apuntar lejos": not found how to press it from a script), insects.

What falls to the ground is everyone's too (drops_update, in loot_update): a new item near the local Link (Item_*,
Weapon_*, PutRupee*, within 15 m) that the map didn't place (no HashId) - an ore's, a crate's or a pot's insides, a
tree's fruit, what he drops from his pouch - not ours already nor one of the others' made here, goes on the loot list
once it has lain still there 0.4 s (moved less than 5 cm between looks, every 9 frames: DROP_SETTLE_NS; an enemy's loot and a thrown weapon go on it
their own way first). Not one in his hands: linked to him (+0x4d4 non-zero: 0x72 while he holds it, as the slate on
his hip; 0 on the ground), or within 1.5 m of Link and 0.5 m over his feet (before, what he held went on the list
and the others saw it lying where he had stood: the apples). Tested with two games: the client held 3
mushrooms from its pouch and walked about 10 s, none sent; let fall, the 3 came in the host's game where they lay
(-1037.9 250.0 1817.9 and the others in both), and one the host picked up went from the client's. Not one still falling (a weapon dropped from
his pouch went on the list in the air, made there for the others: "the bow wasn't where it should be"). What
is near when the game starts looking (and after a jump of 30 m: a load, a teleport) isn't new. Where our game breaks
something for another (break_here: his ore, his crate, his tree), what ours drops of it is taken away for 5 s round
it (g_drop_hush): his comes as his loot. One of the others' made here and gone within 1.5 s (made where our Link
stood, it was lost at once) isn't picked up: it is made again (up to 5 times); before, that took it from everyone.
`SAVAGEWIND_ONLINE_DROP_TEST=<s>[,<actor>]` (test): an item (Item_Fruit_A unless named) made 1.5 m before Link that
many seconds in. Tested with two games: a sword the host's test dropped came in the client's game where it lay
(-1041.5 250.2 1813.4 in both), the client standing on it and 8 m off; picked up by the client, gone in both; an
apple that rolled to -1042.4 250.1 1813.6 there in both, picked up by the client, the host's left (each his own).
Untested: an ore or a crate broken by one of them (the drops hushed in the other's game). A bow dropped 1 m over the
ground came where it came to rest (-1041.5 250.2 1813.4 in both); an apple the client picked up went from the host's.

An item the map placed (a HashId: a mushroom, a fruit lying there, a rupee; map_item) picked up by one goes from
every world as a broken object does: gone from the local Link's world with him within 3.5 m, it goes on `broken`
and the session's world list, and the others' games take it out (take_out: its actor deleted, its map object marked
gone) and set its flag `<field>_<actor>_<HashId>`, so it stays gone (world_enforce, once a second, as a broken one).
Not the Sheikah Slate on Link's hip nor the rune's glove (Item_Conductor, Item_Magnetglove: players_own_item, the
drops' and the map items' both), not weapons (PICKs). Tested with two games: the client picked up two mushrooms
(Item_Mushroom_E) by the start; the same two went from the host's world, the third left in both.

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
skeleton. Its saddle and reins (protocol 31: GameRomHorseSaddle_* / Reins_*, actors of their own, the nearest to his
horse) go with it (`tack`, `tack_m`) and are shown as their model alone (OnlineHorse_<their actor>), where his are; not
tested ridden (riding can't be driven from the control port). `SAVAGEWIND_ONLINE_HORSE_TEST=<horse>` (test): that horse
beside the host. Tested: GameRomHorse01 and the Master Cycle shown beside the host's Link on the client.

ReactionField (footprints, grass, water): its ELink plays them with its own matrix at the origin (0 0 0, each effect
put in place through its handle after: a point at +0xa8 and the ground's normal at +0xb4 with +0x180 bit 2, or a
matrix at +0xc0 with bit 3), so who and where aren't known as it plays them. Those it plays while the local Link moves
(more than 1 cm a frame; one of a name a quarter of a second: it plays every actor's about) go as his effects (who 4,
protocol 33 with its property values at the last, `ground_props`: the ground's kind and the like, which its containers
choose by), and the others' games play them on their own ReactionField (the keyword emit, then the name) at his
puppet, with his values. Tested: standing, none sent; walking, 13 sent in 4 s, 4 of them made (the others had nothing
to play there, as the local game's own), his blows' effects and sounds still go. Footprint isn't made by its name in
the others' games (no handle: not sent). `SAVAGEWIND_ONLINE_RF_LOG=1` logs its effects with its place, the local
Link's and its values.

Players bump into each other: the puppets have no body, so each game pushes its own Link out of the others' (on
foot, at the same level: 0.7 m between their centres, 1.7 m of height), all of the overlap at once, written to his real
position (control_cell_player_pos, what `teleport` writes); none on a horse or in an event.
`SAVAGEWIND_ONLINE_NO_COLLIDE=1` turns it off. Tested: two Links made at the same spot stood 0.70 m apart; one running
head-on into the other slid around him (0.89 m at the nearest sample) instead of through.

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
they load that place (one in view stays until then). Tested both ways (an item's flag set on the host, an ore's on
the client), 60 fps kept.

A Blood Moon (protocol 32: QUEST carries whose it is): the host's game keeps each game's flags as they came last (not
all those ever sent), and the union it sends is of those and its own, so a flag every game has cleared leaves it. A
game that sees 64 or more of these flags clear at once has had a Blood Moon: those the union still has then (the
others' from before theirs) aren't set in it again, until the union drops them; one taken after is. So what was
taken comes back in each game at its own Blood Moon. Tested with `bloodmoon` (developer mode: clears this game's
world flags, as one does): 70 items set on the host, cleared on the client, stayed clear there while the host still
had them; cleared on the host too, clear in both; one set again on the host, set again on the client.

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
`give`; and for online play `bloodmoon` (this game's world flags cleared, as a Blood Moon does), `actors <text>` (live actors: where, how far, their address), `flag` / `setflag` (read
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

### The session's rules, revival, the Blood Moon, blows and the world's objects (protocol 35, mod v36)

The host's rules (Settings > Online > Combat; `collide` and `revive` in online.ini, both on by default): his state
carries them (`collide`, `revive`) and the others follow his; in another's session the toggles can't be changed.

A player who dies is revived by a fairy where he fell (`revive`): the game asks whether Link has a fairy as he dies
(1.9.0 0x7000cec510: x0 the player, its pause menu data asked for Animal_Insect_F; false at once in modes without
one); the patch `has_fairy` (tools/patches.py, on its `and w0, w0, #1` before it returns) asks
rt_online_has_fairy, which says yes while connected and the session's rule is on. The fairy's own revival plays
(its effect, his hearts back) and his world isn't loaded again; a fairy of his own is used first, as always.
Tested: the client's life written to 0: "revived by a fairy", hearts back to 12 quarters, no game over, the fairy's
light on him.

The Blood Moon: the time manager (ksys::world::Manager +0x5c8, its first) counts it at +0xc8 in degrees of the day
(360 a day; past 2520, 7 days, at midnight there is one: 1.9.0 0x7001425fa8, and it starts again at 0). It writes
it each frame to its game data value WM_BloodyMoonTimer (its handle at +0x120) and takes it back from there, so a
write to the copy alone was undone the same frame; the game data manager's f32 setter (1.9.0 0x7000b36c80: x0 the
manager, w1 the handle, s0 the value) sets it. Each player's state carries his (`bloody_moon`); the others take the
host's when theirs is off by more than 30 (two game hours), so the Blood Moon comes for everyone the same night.
`SAVAGEWIND_ONLINE_BM_TEST=<v>` (test): the host's set to v once in the world. Tested: the host's set to 2400, the
client's went from 560.3 to 2401.4, both 2406 a few seconds later.

The weather: of the players within 500 m, the first in the session's (the host when he is there) is everyone's, so
players together anywhere on the map share one sky (before, only near the host).

Sneak strikes: a copy of an enemy another's game has has no senses here, so the game takes it as unaware and a blow
from behind does eight times its damage. A blow on one whose AS in its owner's game is a fighting one (not Wait*,
*Sleep*, Squat*, *Talk*, Walk*, *Sit*, *Relax*, *Eat*, *Dance*, Look*) sends no more than the striker's weapon's
attack power (power.txt) when it is more than twice that ("a blow of ... as without a sneak strike"). And an enemy
just handed to this game (own_ready: struck by us, or come nearer) is made unaware, so our next blow would be a real
sneak strike on it: for 8 s its life is kept GUARD_LIFE (1000) above its own (the real one apart, the one sent), and
a blow takes off it no more than our weapon's power when it is more than twice that (its first half second, the game
setting its life up, not counted). Tested: a Bokoblin handed to the client (12, 1012 to the game), a blow of 60: 3
taken; its life 9 once the guard was over. (In a session, two Bokoblins were killed so: the copy's blow went as 4,
handing them over, and the next blow on the new real one was a sneak strike.)

Another player's bomb gone off within 4 m of the local Link, with damage between players on: two hearts less his
armour's defence, and the blast knocks him down (OnlineKnock). Tested: the host's Remote Bomb set off 1.7 m from the
client: "Host's bomb hit us: 6 after our defence 2", 8 -> 2.
Sneak strikes tested: a blow of 12 on a copy of the host's Bokoblin fighting him (DoubleAttack) went as 3 (the
client's weapon's power), and the host's game took 3.

Blows: a player who hits one of the host's enemies another's game has (OnlinePose.aggro_id, aggro_seq: the enemy
and the hit's number) gets it: the host's game hands it to him (within OWN_SAFE of it, not again for 6 s), so it
turns on him, as an enemy turns on whoever strikes it; it stays his while he is within 45 m of it, whoever is
nearer. `SAVAGEWIND_ONLINE_AGGRO_LOG=1` logs each blow and why. Tested: the host by a Bokoblin camp, the client 20 m
off with `SAVAGEWIND_ONLINE_ELEM_HIT_TEST` (now on the nearest copy): both Bokoblin it hit went to the client
("struck by it") and stayed his ("we have ... for real now", none given back).

The world's objects: the map's objects with a body (a HashId; crates, barrels, boulders, a shrine's balls...) moving
within 20 m of a player go with his state (`objs`: up to 8, the nearest, their matrices); the others' games move
theirs, the same map object, there through its body each frame (FN_BODY_TO, as Magnesis). One moving in two
players' worlds goes as the first one's in the session. Not enemies, players, items, animals nor what a level moves
itself (lifts, gears, doors, water). `SAVAGEWIND_ONLINE_OBJ_TEST=<s>` (test): the nearest crate or barrel lifted 3 m
for 3 s (1000 + <s>: that long after the break test's cut, SAVAGEWIND_ONLINE_BREAK_TEST, which waits for a tree
near). Tested: a crate lifted on the host rose in the client's world too (253.3 while the host's was at 254.2) and
both lay at -1043.0 251.2 1994.9 after. (Moved every 3 frames, the client's crate overshot and broke.)

A cut tree's trunk (protocol 37, `objs_trunk`: bit k, objs[k] a trunk) has no HashId of its own: each game makes its
own as the tree is cut (ours, followed in g_our_cut; the others', made here, in g_their_trunk), so it goes by the
tree's HashId, and moves in every game as the others' objects do - pushed, rolled, launched out of Stasis. Before,
each player's trunk lay where it fell in his own game: pushed away in one, still there in the other. Tested with two
games: the host's test cut, both trunks at rest at -1052.2 249.8 1811.3; lifted 3 m on the host, the client's rose
after it; let go, both again at -1052.2 249.8 1811.3.

The others' objects are taken to his place through their bodies, which get there over a few steps: each is taken
there every frame and for OBJ_HOLD frames (1.5 s) after his last (he stops sending one a second after it rests), and
woken first when asleep (at rest a while, the game updates it no more and its body took nothing). Before, his trunk
pushed stopped short here (0.4 m of a 3 m lift while his was at 2.4 m) or, at rest a while, didn't move at all.
Tested with two games: a trunk at rest 40 s lifted 3 m on the host rose the whole 3 m in the client's world (249.8 to
252.8) and lay again at 249.8 after. Still more than 0.5 m off his at the last of it (caught on a stump its body
went round), it is put there (FN_SET_MTX). One another player holds with Magnesis or stopped with Stasis, moved here
to his, is his too: sent back as ours, each game pulled the other's to where its own was (his Stasis, struck and let
go, launched his trunk in his world alone). One followed while it moves goes to OBJ_FAR (100 m; 40 m before, and a
launched one stopped in the air here). His Stasis's object is looked for as his grab's: the one of that name nearest
where he stopped it or where it is now, 15 m (4 m before: two trunks fallen their own ways, his was "not here"; tested
with the bot stopping a box it said was 8 m off: found, chained).

Whose an object is, as in one world: the game that set it moving - a push, a blow, a bomb, a throw, however far its
player - sends it until it rests (a second still), and the others follow it; one we are sending ourselves is ours
and his is ignored, unless his began first (both within OBJ_TIE_NS, 0.3 s: the lower player number's). One a player
just let go of (Magnesis, or Stasis struck and released: g_act.released, 4 s) is his as it flies. A trunk made here
for another's cut sends nothing for 8 s (TRUNK_QUIET_NS: its fall is the cutter's), and ours, of our cut, sends where
it fell for a second as it is first followed (the others' put there). (Tried before: the nearer player's - a far
push, a bomb's, was lost; the first to send it kept it - a trunk rolling on the slope in the other's world took the
host's own lift away.) An object another moves is followed here however far from our Link (obj_find: a trunk by its
tree's HashId, else the map object of that HashId); before, only within 20 m, and with him 25 m off as he cut a tree,
its fall and his pushes went unheard and it lay as it fell here, another way. One just followed isn't ours to send
for 0.7 s (OBJ_QUIET_NS: it settling as it was taken to his), and one farther than 20 m starts no move of ours. Tested
with two games: the client 60 m off as the host cut a tree and lifted its trunk, the client's rose and lay with it;
the client cut and lifted one with the host 60 m off, the host's rose after it; both near, the host's lift, the two
at 252.8, then 249.9 1811.4 each. (The test hooks: SAVAGEWIND_ONLINE_BREAK_TEST waits for a tree near and marks it
seen, else world_enforce cut it again, a second trunk; the lift wakes what it lifts, asleep 15 s after it fell.)

The menus don't pause (the host's rule, `nopause` in online.ini, on by default): the pause menu and the map set the
UI's pause state (0x7001e1611c) to 2, and the game stops the world through the actor system's pause bits
(ksys::act::BaseProcMgr at 0x7001e937f8, its u16 at +0x168, bit n "paused for n": 0x70015195c0 asks one,
0x70015195d0 / 0x70015195e0 set and clear them; the world manager's update is skipped while bit 0 is set, 1.9.0
0x700127f810; the pause reasons at 0x7001e16114, requestPause 0x7000d465a8, aren't used for menus). Opening either
sets 0x7f; with others in the session they are cleared each frame while the state is 2, so the world, the enemies
and the time of day go on while a player looks at his things. The quick menus (state 4) too: they stop it the same
way (scene state 1, the bits 0x7f).
Found by counting the game's function calls (SAVAGEWIND_FNPROF) in play and with the menu open: 0x700127f7d0 ran in
both, the world manager's update (0x7001435618) only in play. Clearing those bits alone ran the time of day but
left the actors still: the frame also leaves the world's jobs out (ksys::PhysicsEntitryWorldPre and the actors')
while the game scene's state (0x7001e44ef0, set by 0x7001281100) is not 0, and it is 1 while a menu is up; the
patches `scene_state_get` (its getter 0x700127f3c0), `scene_state_phys` (0x70012800bc), `scene_state_b`
(0x70012810b8), `scene_state_c` (0x700128322c) and `scene_state_d` (0x7001280164: the physics' paused branch)
read it through rt_scene_state, 0 then. The menu's own resume
(0x7000d462d0) is done for the world alone each time it pauses: the physics system (0x700156f61c(its system, 0,
0)), the bits (0x70015195e0(manager, 0x7f)), the manager's procs (0x7001519720), the scene's paused byte (+0x8b8).
His pose is sampled with the menu up too, so the others see him as he is. Found the physics' share by counting
calls again: with the first four sites the actors' jobs ran, the physics step (0x70012531a8, 0x7001459d08) didn't.
Tested: with the host's menu up next to a Bokoblin camp, the Bokoblins moved metres every 2 s, the time of day went
on, 57-60 fps. The local Link's own hits aren't taken while the menu is his (next to them for 14 s: 12 -> 4
quarter hearts in play, 12 -> 12 with the menu up, his damage handling 0x7000cc4c3c running in both), so our own
enemies' blows land through the runtime then, as the host's enemies' do on the others (own_strikes_in_menu: one
starting an Attack / Kick AS, within its reach in front of him, his shield not up, STRIKE_FRAMES later).
An archer's arrow (ArrowShoot, facing him within 40 m) lands
as the arrow would (at 40 m/s), its bow's attack power less his defence, his shield up taking it. `SAVAGEWIND_ONLINE_MENU_LOG=1`
logs their AS. Tested: two archers on their towers hit the host 17 m off with his menu up (2 each). Tested: with the host's menu up among the Bokoblins they jumped at him
and struck (JumpAttack, Attack), 12 -> 10 -> 8 -> 6 -> 4 -> 2 -> 0, a fairy revived him (the menu up). Each kind tested the same
way: the pause menu, the map, the weapons', shields' and runes' quick menus: the bits 0, the Bokoblins moving and
striking (12 -> 6, 12 -> 8, 8 -> 2, 10 -> 4, 10 -> 6 in 6 s).

Any of the UI's pauses does the same (the windows that come up in play, as a new item's, uiPickUpWin_00Screen), each
new value logged once ("the UI's pause"), but not a load's (its bit 0x200: the world waits for the map then).

But not once he presses A or X in the pouch (X "Coger", or A on a material and "Coger" in its list: a material taken
out to hold over his head; A for anything else there too, the world stopping sooner than it must) until the menu goes:
what he takes out is made then, and made with the actors' pause bits cleared the game never put it in his hands - it
hung in the air where he took it out as he walked off, his hands empty ("las manzanas se congelan en el aire").
The world is stopped again then as the menu stopped it as it opened: the physics world (0x700156f61c(its system, 1,
0), as 1.9.0 0x7000c40458), the bits (0x70015195d0(manager, 0x7f)) and the manager's procs, the scene's paused
byte; the scene's state is read as it is; the menu's own resume undoes it as it goes. A and X are the buttons the
game is given (runtime/hle/input.c g_game_buttons: the keyboard's, the mouse's and the controller's alike). X alone
wasn't enough: a player took his apples with A on them and "Coger" in their list, and they hung in the air. Found by turning the no-pause's parts off one at a time, two games, the
client holding 3 mushrooms and walking 20 m: with the bits cleared they stayed 14-20 m behind him (the world jobs'
scene state, the physics and the getter alone didn't matter; clearing bit 0 alone did it too, and stopping the
world again after the first one was made was too late); stopped at X, 1 m from him in two runs, in his hands.

The world behind the menus is drawn live, not the still picture the game takes as one opens (`SAVAGEWIND_ONLINE_MENU_STILL`:
the picture, as without online play). Three things kept it still, found by following the world's draw from the frame
down (SAVAGEWIND_FNPROF in play and with a menu up, then the bytes each skipped function tests):
- the menus' frame (1.9.0 0x7000d45d70) asks the world's drawing paused every frame, 0x70015089b0(the graphics
  manager, 1): its +0x96b0 set and the world's render layers off. The patch `menu_draw` (tools/patches.py, at its
  `mov w1, #1`, 0x7000d45d84) asks with rt_menu_draw_pause, 0 while the menus don't pause the world;
- the pause menu turns the world's camera off as it opens (0x7000e6840c: 0x7000e89490(1, false), camera 1 of the
  game's list, its +0x90 bit 0; the views of a layer are drawn only with their camera on: 0x7000ff52dc). The frame hook
  turns it on again (0x7000e89490(1, true));
- the menu shows a small blurred picture of the scene made once as it opens (the pause menu and the map 320x180, its
  tab icons cut from it by its alpha; the quick menus 1280x720): with the world drawn, it still showed the moment it
  opened. The Vulkan backend (runtime/nvn/vulkan/exec.c menu_capture_refresh) makes it again each frame, before the
  first draw that samples it, from the frame drawn so far: halved step by step, then a 13x13 Gaussian blur at its
  size with alpha 1 (menu_blur.frag), then copied into it. A 16:9 target two thirds of the screen or less that no
  pass drew since the frame before, sampled into a target at least 1280 wide; not in the menu's first 4 frames (the
  camera comes back a frame later: else a dark flash).
Tested with two games: the host's menu up among Bokoblins, two screenshots 1.5 s apart differ (the Bokoblins move and
strike behind the blurred background, 450000-540000 draws every 2 s as in play, 104000 before), a teleport with the
menu up shows the new place, and the weapons' quick menu the same.

The map's objects broken (`broken`, 8 HashIds; `trunks_gone`): a map object (its HashId; not enemies, players,
items, animals) within 15 m of the local Link that goes while he stays (he moved less than 10 m) was broken by him: a
crate, a barrel, a rock a bomb broke, an ore, a pot, a bush his blade cut, or a tree he cut, which the game replaces
with `<tree>_Stump` where it stood and `<tree>_Trunk`, the log, falling (a tree that went with no stump there isn't
counted). The others' games break the same object (break_here): a tree as the game cuts it (its stump and its trunk
made where it stood, the trunk tilted 8 degrees away from the cutter so it falls that way), anything else by the game
itself (its life set to 0: its own pieces, effects and drops; and its flag `<field>_<actor>_<HashId>` set as a blow
sets it, else the game made it again a few seconds later). His trunk chopped up goes from their worlds too.

An object far from anything that moves it is asleep (its state +0x64 is 0: the game never updates it), and a tree
asleep is drawn by its map placement's own model, its actor drawing nothing. So the object is woken first
(0x7001515988: state 1, the process manager told): a crate's life 0 then breaks it as a blow does (asleep, it
broke nothing and was only taken away), and a tree is taken out 0.4 s later (cut_later): woken, its first updates hand
the drawing to its actor. Taken out at once, or deleted, hidden or sunk while asleep, the placement's tree stayed,
drawn standing beside its stump, whatever the actor became (its state 3, its model 0). Taken out as the game takes out
what a blow broke (take_out): its map object marked gone (0x7000bdc1f0: the actor's map object, +0x430 of its vtable,
its flags +0xc | 0x80, as a tree's own cut does at 0x7000951ae8), then 0x7001524598 (mode 0: FN_DELETE and its
placement told, 0x7001526bd0). Found by watching the game's own cut: `SAVAGEWIND_FNPROF_ARGS=7001524598,7001514bac`
logged its removal of the tree, from 0x7000951b7c.

The session's world (g_world, 2048 HashIds): every map object broken in the session, by anyone, is kept while it lasts
(cleared when the session ends), and stays broken in every game:
- one that comes again - near again, made again by a load or a game over (the game grows a cut tree again when its
  area loads), or one a player's game didn't have yet (a tree far from his Link is no actor until he comes near) - is
  broken again by world_enforce, about once a second, each actor of it once (its trunk only for a cut of the last
  20 s);
- while a player breaks nothing new, his game sends the whole list round in his last broken slot, one every 1.5 s
  (world_rotate): a player who joins later gets everything broken before him;
- one of the list that goes isn't told as ours (else each game sent the other's cut back to it, and it made the tree's
  stump and trunk again: a pile of them).

`SAVAGEWIND_ONLINE_BREAK_TEST=<s>`: the nearest tree cut that many seconds in, as the others' are, and sent as ours.
Tested with two games, each seen on screen (screenshots from the same place and camera):
- the host cut a tree with his axe and the client, 9 m away, saw it go: its stump where it stood, its trunk fallen,
  as in the host's own world (before the wake, the client's tree stayed standing beside its stump);
- the same with the client 270 m away: "not here yet"; come back, its game cut the tree (the world's list);
- the host went 300 m away and back: his own cut tree had grown again, and was cut again, with a bush;
- a crate the host broke broke in the client's world 60 m away (before the wake: taken away, unbroken);
- a client that joined after the host had cut a tree and broken a crate got both (the crate as it came near).

The level's mechanisms (protocol 36, STATE's `movers`): what a level moves itself - lifts and elevators, doors and
shutters, gears, bridges, turning and sliding floors, propellers, gates, levers, a Divine Beast's parts (Remains*) -
each game moved by its own logic, so one a player set going (a switch, a terminal, an orb) moved in his world alone.
One within 80 m of the local Link that moves or changes the AS it plays in slot 0 while no other player's game moves
it goes with his state (the nearest 6: its HashId, its matrix, its AS); the others' games wake theirs (0x7001515988)
and put it there - through its body when it has one (FN_BODY_TO), and its matrix (FN_SET_MTX) - and play its AS
(play_as), so it moves in every world as in his. One both move goes as the lower player number's; one theirs isn't
sent back for 0.7 s after. Not the rocks to lift, the tags nor the far models (objs_update, nothing to move). A map
actor the game makes again (woken, or moved while asleep: its state 3, another actor of the same HashId) is looked for
again at once by its HashId; a woken one goes to another proc unit, so its watch is taken again (alive_rewatch).
`SAVAGEWIND_ONLINE_MOVER_TEST=<s>`: the nearest mechanism lowered 2 m in 2 s that many seconds in (and
`SAVAGEWIND_ONLINE_MOVER_TEST_AS=<name>` played on it); `SAVAGEWIND_ONLINE_MOVER_LOG=1` logs each one taken or sent.
Tested with two games at a shrine's entrance (DgnObj_EntranceElevatorSP): its matrix moved on the host went to the
client's, which moved its own there (187.70 -> 187.45). That shrine's elevator, closed in the test save, didn't run
by itself, and an asleep one moved by hand is made again by the game; a lift ridden for real and a Divine Beast's
parts (their AS) aren't tested yet.

The shops: their wares sold out (every `*_SoldOut` flag) and the jeweller's opening (GemShop_Open) are shared flags
(mod v37); the wares on their shelves are items, whose revival flags already are.

One world's state (mod v37: 25767 shared flags): with the quests', chests' and items' flags, the Divine Beasts'
(Remains*) and the Trial of the Sword's (FinalTrial*) progress, the people's talks and errands (Npc*, NPC*, the mini
games, the towns' own), the field's switches (LinkTag*: the gates and doors they open), the walls broken
(WallCrack, IceWall), the pots, beehives, ores and dig spots used and the field's, towns' and dungeons' objects
broken (FldObj, TwnObj, DgnObj). Not each player's own (the Compendium, items got, cutscenes seen). Those a Blood
Moon clears come back as the items do. A shrine's puzzle isn't in flags (each entry starts it anew): players in
the same shrine see each other, and its moving balls and boxes go as the world's objects.

### Not done yet

- Less to send: only the bones that moved, or fewer times a second (a session of 8 sends each player's skeleton to 7).

- A dye's colour on the others' screens: not seen yet. The player's armour code (0x7000cdc43c) makes each piece with
  0x70011759a4(mgr, slot, name, dye, ...), the dye from player +0x1114 (head), +0x1118 (upper), +0x111c (lower; -1
  none), put in the piece's InstParamPack as `ArmorDyeColor` (s32) when the piece has the tag 0x216a3a63 (dyeable).
  Each player's state carries his three (`dye`, protocol 27) and his puppet's pieces are made with the same param
  (made again when it changes; tested: a dye written on the host made the client's piece again). Whether an
  OnlineArmor (a DemoNPC) shows it is still to see; if not, the colour has to go on its materials.
- Bodies for the players' puppets (standing on one, the world's blows on them): they are DemoNPCs; the bumping is
  the runtime's push (players_collide), the blows between players the runtime's (pvp_update).
- The beam is not seen working yet: in the test runs (full hearts, the Master Sword) the game fired none, with or
  without online play, so `beam_move` and OnlineBeam are untested.
- A Divine Beast seen moving by the others while one is inside: its parts (Remains*) are followed as the level's
  mechanisms (their matrix and AS), not tested inside a beast yet.
- Footprints left on the ground (Footprint: not made by its name), grass cut by a blade; the water's and grass's
  reactions are sent with the dust (ReactionField's), not seen yet in water.
- The world shared: enemies, items, time of day, quests (Milk Bar's quest and enemy sync); damage between players.
