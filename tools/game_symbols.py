"""The game-owned addresses the runtime uses, per main build: the one place that decides them.

The runtime pokes the game's memory in a handful of places (the pause state and the frame rate's inputs,
runtime/hle/framepace.c; the Options screen's tables, runtime/ui/game_options.mm; the frame loop's return address,
runtime/nvn/objects.c; the title screen's version line, runtime/services/fs.c). Those are addresses inside `main`, so
they differ between game versions: each build's values are here, `gen.py` writes them into the game code's layout.c
(GameLibSymbol game_symbols[]) and the runtime reads them through rt_game_sym() (runtime/gamelib.c). Nothing in
runtime/ knows about a version.

Adding a version: its main build id (tools/sw_probe.py prints the first 16 hex digits), a display name, and every
name from the first build's table. Where the values come from is in decompilation.md ("Comparing BotW 1.8.2 and
1.9.0"): the ADRP+ADD references of each object, and for code addresses the function that holds them.

`ADDR` values must lie inside main's image (checked()); `VAL` ones are values the runtime puts in its own code (a
struct offset), not addresses.
"""
ADDR, VAL = "addr", "val"

VERSIONS = {
    "de0b6ac4efc1dda7": {
        "display": "1.8.2",
        "symbols": {
            # (u32: 0 playing, 2 in the pause menu or the map)
            "pause_state": (ADDR, 0x7001cb48a4),
            # [the game's UI screen manager]: +0x28 screen count, +0x30 screen pointers by id
            "screen_manager": (ADDR, 0x7001c05d88),
            # [the ScreenMgr the Options screen is found in]: same fields
            "screen_mgr": (ADDR, 0x7001cd5c38),
            # [ksys::evt::Manager]: its active event at +0x1d2b8 (hasActiveEvent 0x7000ffa208 reads it)
            "event_manager": (ADDR, 0x7001cd8db8),
            # [uking::act::PlayerInfo]: Link's position at +0x88, the stamina at +0xa4
            "player_info": (ADDR, 0x7001cb2658),
            # [ksys::world::Manager]: +0x5c0 manager count, +0x5c8 the managers, [0] TimeMgr (the time at +0xb8)
            "world_manager": (ADDR, 0x7001cf5d00),
            # the controllers' held-button step (a float the game keeps at 1, read only there)
            "controller_step": (ADDR, 0x7001c4ba40),
            # the static Options table: entry count, entries, row order and its count
            "option_count": (ADDR, 0x7001cccd98),
            "option_entries": (ADDR, 0x7001cccda0),
            "option_rows": (ADDR, 0x7001cccda8),
            "option_order": (ADDR, 0x7001cccdb0),
            # the dialog's answer (FUN_7100d46068 takes it)
            "dialog_answer": (ADDR, 0x7001c4b308),
            # the title menu's message file (LayoutMsg/Title_00)
            "title_message_file": (ADDR, 0x7001cd06d8),
            # the title screen's "Ver.%s", the format of System/Version.txt (main 0x70014b854e, rodata)
            "title_version_fmt": (ADDR, 0x70014b854e),
            # the frame loop's return address (nvnWindowSetPresentInterval's caller, main 0x700101287c)
            "frame_loop_lr": (ADDR, 0x7001012a34),
            # main's .data start and its image end (the debug data dump)
            "main_data_start": (ADDR, 0x7001a1f000),
            "main_image_end": (ADDR, 0x7001d37000),
            # the dialog's cancel button (a pointer in the screen struct the B handler reads: `ldr x0, [x8, #0x33b0]`)
            "dialog_cancel_off": (VAL, 0x33b0),
            # the title menu's active button id (0x69..0x6f: 0x6a is Options, whose screen is up while the Options state
            # runs) and a screen's row count and row pointers (the Options screen's, to redraw a row)
            "menu_button_off": (VAL, 0x363c),
            "screen_rows_count_off": (VAL, 0x3698),
            "screen_rows_list_off": (VAL, 0x36a0),
        },
    },
    "cd57b23fa4bbad65": {
        "display": "1.9.0",
        "symbols": {
            # Measured by mapping each 1.8.2 reference site to its function's 1.9.0 counterpart (identical normalized
            # instruction sequence, same instruction) and the address it builds there: unanimous per object.
            # (decompilation.md, "Relocating the constants")
            "pause_state": (ADDR, 0x7001e1611c),       # 7/7
            "screen_manager": (ADDR, 0x7001d66d58),    # 209/209
            "screen_mgr": (ADDR, 0x7001e374b8),        # 313/313
            "event_manager": (ADDR, 0x7001e3a638),     # 134/134
            "player_info": (ADDR, 0x7001e13ed0),       # 121/121
            "world_manager": (ADDR, 0x7001e57580),     # 197/197
            # (read as *[a pointer slot]: the 1.9.0 slot 0x7001d6e9e0's relocation points at it; the object it sits in
            # is 0x7001dad0b8, mapped 124/124)
            "controller_step": (ADDR, 0x7001dad0e0),
            "option_count": (ADDR, 0x7001e2e610),      # 17/17
            "option_entries": (ADDR, 0x7001e2e618),    # (count, rows and order measured: the same 8-byte slots)
            "option_rows": (ADDR, 0x7001e2e620),       # 4/4
            "option_order": (ADDR, 0x7001e2e628),      # 2/2
            "dialog_answer": (ADDR, 0x7001dac9a8),     # 5/5
            "title_message_file": (ADDR, 0x7001e31f50),  # 11/11
            # (rodata is not a constant shift: found by its "Ver.%s" content)
            "title_version_fmt": (ADDR, 0x70015cd092),
            # (the frame loop's region; byte-identical code around the call, decompilation.md)
            "frame_loop_lr": (ADDR, 0x70011267c4),
            # (the NSO's data segment start and image end: 190's .data moves past 1.8.2's whole image)
            "main_data_start": (ADDR, 0x7001b7a000),
            "main_image_end": (ADDR, 0x7001e98000),
            "dialog_cancel_off": (VAL, 0x33c8),  # (that struct grew 0x18)
            # (the screen/menu structs grew 0x18 too: the same instructions with `ldr w8, [x19, #0x3654]` and
            # `[x19, #0x36b0]` / `[x19, #0x36b8]`)
            "menu_button_off": (VAL, 0x3654),
            "screen_rows_count_off": (VAL, 0x36b0),
            "screen_rows_list_off": (VAL, 0x36b8),
        },
    },
}


def symbols(build_id):
    """[(name, value)] of a main build, in this file's order."""
    try:
        return list(VERSIONS[build_id[:16]]["symbols"].items())
    except KeyError:
        raise KeyError(f'unsupported main build id {build_id[:16]}') from None


def display(build_id):
    return VERSIONS[build_id[:16]]["display"]


def values(build_id):
    """{name: value}: the symbols alone, for code that builds its own addresses on top of them."""
    return {name: value for name, (kind, value) in symbols(build_id)}


def checked(layout):
    """symbols() of the layout's main module, or ValueError if one of them is outside main's image.

    A generated address the loader never maps would be a crash far from here, so the layout is checked against the
    game code's own module sizes (tools/layout.py) instead.
    """
    main = [m for m in layout if m[0] == 'main']
    if not main:
        raise ValueError('the layout has no main module')
    _, base, size, build_id = main[0]
    try:
        syms = symbols(build_id)
    except KeyError as e:
        raise ValueError(e.args[0]) from None
    for name, (kind, value) in syms:
        if kind == ADDR and not base <= value <= base + size:
            raise ValueError(f'{name} (0x{value:x}) is outside main ({base:#x}-{base + size:#x}, '
                             f'build {build_id[:16]})')
    return syms
