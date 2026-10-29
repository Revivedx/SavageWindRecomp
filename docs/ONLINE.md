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

In a session: the players (left, under the hearts), join / leave notices, the chat's last lines, and each other player
where they are in the world (a figure in their colour with their name and distance; an arrow at the screen's edge when
out of view). Chat from the Online tab.

## How it works

| Part | Where |
|---|---|
| Protocol, server, client, interpolation, settings | `runtime/online.c`, `runtime/online.h` |
| Online tab and overlay | `runtime/ui/online_ui.mm` (tab 10 of `runtime/ui/menu.mm`; Options > Online in `game_options.mm`) |
| Scene camera for the overlay | `runtime/nvn/vulkan/exec.c` (frame generation's camera capture, also while online) |
| Actors for the other players (experimental, off) | `runtime/online_actors.c`, called from `tools/patches.py` `_vfr_ratio` |
| Test player without a game | `tools/online_bot.c` |

The wire: TCP, little-endian, `u16 length, u8 type, payload`. HELLO (magic `SWOL`, protocol version, name, password) ->
WELCOME (slot) or REJECT (full, version, password, name); JOIN / LEAVE; STATE 20 times a second (Link's feet position,
heading, paused / loading flags, the sender's round trip); CHAT; PING / PONG. The server passes each STATE and CHAT on
with the sender's slot. Clients draw the others 100 ms behind what they last heard, interpolated.

What the game gives: `uking::act::PlayerInfo` [`player_info`] +0x88 Link's position; the pause state
[`pause_state`]. The heading comes from where Link has been moving.

## Players as actors: what was found (1.9.0)

Read from the code with capstone (no symbols, no RTTI in `main`):

- `0x7001535900`: creates an actor now, by name. x0 the creator (`**0x7001d66550`), x1 the name, x2 a heap
  (`*(**0x7001d66558 + 0x28)`), x3 0, w4, w5 0; returns the actor, or 0 when its resources aren't loaded. The Remote
  Bomb rune calls it (`0x7000b3bb40`) with `RemoteBomb` / `RemoteBombCube`.
- `0x700152261c`: sets an actor's matrix (x0 actor, x1 `Matrix34f *`, x2 scale or 0); the 3x4 is at actor +0x398.
- `0x7001514bac`: deletes an actor (x0, w1 0).

Tried with `SAVAGEWIND_ONLINE_ACTOR=<name>` (the bot of `tools/online_bot.c` walking around Link):

- `RemoteBombCube`: created (the call works), but the bomb stays at Link's hands: its own AI / physics overrides the
  matrix.
- `PauseMenuPlayer` (the only actor besides the player with Link's model, `bfres` `Link`): created, never drawn in the
  world (it belongs to the pause menu's own scene).
- `GameROMPlayer` (a second player): the game crashes right after (access at 0x4e0).

Milk Bar's players are `Jugador1..32`: Hylian NPCs with a UMii (`Actor/UMii/Jugador<n>.bumii`, its values written at
run time) and equipment given to them, spawned through `ActorCreator::requestCreateActor` (Wii U 0x037b6040) with
their resources loaded first. The same on the Switch needs, still to be found: the asynchronous creation (resources
loaded, then created), the UMii appearance and the equipment, and the animations (the AS of the remote player's
action). A Ghidra project of 1.9.0's `main` (`tools/ghidra/README.md`) is the way to find them.
