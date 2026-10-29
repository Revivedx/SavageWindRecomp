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
name over him. The overlay adds the session's players (left, under the hearts), join / leave notices, the chat's last
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

The wire (protocol 6): TCP, little-endian, `u16 length, u8 type, payload`. HELLO (magic `SWOL`, protocol version,
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

The others dress his Link in the game's own pieces, any of them: each is asked for (`0x7001535b10`, its model loaded
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
no bone (thrown, dropped) goes under the world, out of sight.

### Not done yet

- Less to send: only the bones that moved, or fewer times a second (a session of 8 sends each player's skeleton to 7).

- A dye's colour (the armour's `ArmorDyeColor` param when it is made, as `0x70011759a4` passes it).
- The paraglider, arrows in flight, a thrown weapon's flight, effects (a sword's trail, a spin attack's light).
- The world shared: enemies, items, time of day, quests (Milk Bar's quest and enemy sync); damage between players.
