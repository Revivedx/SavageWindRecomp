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
in it as Links: each other player is a Link in the world where he is, turned where he goes, walking, running,
jumping, falling, gliding, climbing or swimming as he does, with his name over him. The overlay adds the session's
players (left, under the hearts), join / leave notices, the chat's last lines and an arrow at the screen's edge for a
player out of view. Chat from the Online tab.

### The players' Links (online_mod)

The other players' Links are an actor the game doesn't have, OnlinePlayer, made from the player's own game files by
`tools/online_mod.py` into `online_mod/` in the data folder (git-ignored: it is derived from the game). A source
checkout makes it at the first start (`tools/venv` with `oead`: `tools/setup_windows.ps1` installs it), or by hand:

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
| OnlinePlayer, from the game files | `tools/online_mod.py` |
| Test player without a game | `tools/online_bot.c` |

The wire (protocol 2): TCP, little-endian, `u16 length, u8 type, payload`. HELLO (magic `SWOL`, protocol version,
name, password) -> WELCOME (slot) or REJECT (full, version, password, name); JOIN / LEAVE; STATE 20 times a second
(Link's feet position, heading, paused / loading flags, the sender's round trip, what Link does: `ONLINE_A_*`); CHAT;
PING / PONG. The server passes each STATE and CHAT on with the sender's slot. Clients draw the others 100 ms behind
what they last heard, interpolated.

What the game gives: `uking::act::PlayerInfo` [`player_info`] +0x88 Link's position; the pause state
[`pause_state`]. The heading and what Link does (`motion_anim` in `online.c`) come from how he moves: on the ground
by his speed, in the air by how fast he rises or falls.

### OnlinePlayer

`tools/online_mod.py`, from the game's own files:

- the actor link of a DemoNPC (`Dm_Npc_Rito_HeroSoul_Kago`: an NPC the game draws in the world and doesn't steer),
  without its effects and sounds;
- the AI program of a DemoNPC that only stands (`Dm_Animal_Heron_A`, root `DummyAction`; the spirit's own AI hides
  its model while it waits);
- Link's model list and AS list (`PauseMenuPlayer`: Folder Link, `Player_Animation`) plus the runtime's AS
  (`OnlineWait`, `OnlineWalk`, ... : the player's own `Player_Wait`, `Player_Move`, ...). **The defines must stay
  sorted by name**: the game finds an AS by binary search. Its RSTB size grows with them (~340 bytes each);
- resident: in `Pack/TitleBG.pack` and `Actor/ResidentActors.byml` (`Pack/Bootup.pack`, `only_res`), an entry in
  `Actor/ActorInfo.product.sbyml` (sorted by crc32 of the name) and sizes in the ResourceSizeTable.

### The game's functions (1.9.0)

Read from the code (capstone, and Ghidra: `tools/ghidra/README.md`; no symbols, no RTTI in `main`):

- `0x7001535900`: creates an actor now, by name. x0 the creator (`**0x7001d66550`), x1 the name, x2 a heap
  (`*(**0x7001d66558 + 0x28)`), x3 0, w4, w5 0; returns the actor, or 0. The Remote Bomb rune calls it
  (`0x7000b3bb40`). The actor is drawn only when its resources (its model) are loaded: resident, or in use.
  `0x7001535b10` is the asynchronous request (88 callers), which also wants the actor's resources there.
- `0x700152261c`: sets an actor's matrix (x0 actor, x1 `Matrix34f *` row-major, x2 scale or 0): actor +0x398.
- `0x7001514bac`: deletes an actor (x0, w1 0).
- Actor +0x64: the BaseProc state (0 init, 1 calc, 2 sleep, 3 delete). An actor's AS can play from state 1 on.
- `0x700151e42c`: an AI action's "play this AS": x0 the action (its actor at +8), x1 the AS name, w2 not again if
  playing, w3 w4 0, s0 the blend (-1: its own). `ForkASPlay`'s enter (`0x7000759e74`) calls it. The AS manager is
  actor +0x560 (+0xb8 slots, +0xd8 the actor); `0x70014a0040` (manager, slot) gives the AS playing (+8 its name;
  `0x7001dad0b8` for none). The AS list is `*(actor +0x570) +0x98` (+0x2b8 count, +0x2c0 entries of 0x88).

### Tried, and what came of it

- `RemoteBombCube` as a player: drawn, but its AI and physics keep it at Link's hands.
- `PauseMenuPlayer`: never drawn in the world (the pause menu's own scene).
- `GameROMPlayer` (a second player): the game crashes right after (access at 0x4e0).
- `Dm_Npc_Rito_HeroSoul_Kago`: created, state 2 (sleep), its model hidden by its AI.
- `Item_Material_01`: created, not drawn (its model isn't loaded).

### Armour

Each player sends what his Link wears (EQUIP: head, upper, lower; when it changes and every 5 s), read from the
player actor: `PlayerInfo +0x60` is the player actor; `*(player +0xb98)` its equipment, slots of 0x58 with the
slot's actor at +0x48 (0 weapon, 1 shield, 2 bow, 3 head, 4 upper, 5 lower, 6 sheath, 7 quiver); an actor's name is
at actor +0x10. The others dress his Link with an actor per piece, `OnlineArmor_XXX` (the piece's model list, Link's
AS list and AS, on the Link's matrix, playing his AS from the same frame), from `online_mod/armor.txt`
(`piece=actor`: upgrades and dyes share their piece's model, and its actor; a dye's colour is lost).

The limit: each armour model is a resident actor, and the game's resident list (`Actor/ResidentActors.byml`) holds
only ten more than its own 48 (the 59th crashes the game at start in its loader, 0x7000d49080, fixed arrays). So
online_mod carries nine models, the pieces most seen (`ARMOR_MODELS` in `tools/online_mod.py`): Hylian tunic and
trousers, the Old Shirt and Well-Worn Trousers, the Champion's Tunic, Climbing and Stealth. A piece without one
shows Link's base model there (the head: Link's own hair). Also learnt: every online actor carries the AS list and
AS in its own pack (without them its load never ends), and the ResidentActors list's RSTB size must grow with it.

### Not done yet

- More armour models than nine: loading an armour's resources on demand instead of keeping them resident (the
  resident list is full), or more room in the resident loader's arrays.
- Weapons, shield and bow on the players' Links (the slots are there: 0, 1, 2), and the paraglider.
- What Link does, from the game itself (the player actor's AS) instead of from how he moves.
- The world shared: enemies, items, time of day, quests (Milk Bar's quest and enemy sync); damage between players.
