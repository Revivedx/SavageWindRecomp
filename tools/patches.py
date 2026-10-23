"""Per-instruction replacements applied by gen.py (guest address -> function(gen, addr) returning C code).

Each entry replaces the single guest instruction at that address with hand-written C, for example
to redirect a hard-coded aspect-ratio load to the runtime. The addresses belong to one exact build
of the game's main module, so the sites and the addresses the replacement code embeds are per build
(PATCH_BUILDS, keyed by the main module's build id): gen.py checks the instruction word at each site
before replacing it and stops on any other build, so a wrong site can never produce code that runs.

The game addresses the runtime itself reads are not here: they are in tools/game_symbols.py, generated into the game
code's layout.c, and only the patches' own literals (a function they call, the option table's page) are in `vars`.

The Link's Awakening patches this file held in LASRecomp are kept in the archived copy
(reference/LASRecomp/tools/patches.py) as more examples of the pattern.
"""
from game_symbols import values as game_symbol_values


def _vars(build_id, **extra):
    """The addresses the replacement code embeds: tools/game_symbols.py's for this build, plus the patch-only ones."""
    v = game_symbol_values(build_id)
    v.update(extra)
    return v


# Savage Wind's own pages in the game's Options screen (runtime/ui/game_options.mm), opened from the title screen
# menu (screen 1, buttons 0x69..0x6f, FUN_7100d4c5e0) or the pause menu's System page (buttons 0x4d..0x53,
# FUN_7100cbc918). The screen builds one row per entry of a static option table every time it opens (the table is
# read through the pointers at 0x7001cccd98..0x7001cccdb0), and every row goes through a handful of functions. The
# runtime points the table at a longer copy with its own entries (index >= the game's 14) and these hooks hand its
# rows to the runtime: values, texts and what A does. A page change closes the screen and opens it again with another
# row list, from whichever menu opened it. Quitting asks with the game's own two-button dialog (screen 0x39).

def _quit_dialog(ret, owner, v):
    # C: open the game's dialog (screen 0x39) as a two-button question (type 7, vtable +0x4e0) over the screen `owner`
    # (a C expression; its input is off while the question is up); the runtime gives it its texts (_dialog_texts) and
    # reads the answer
    return (f'{{ extern void f_{v["open_screen"]:x}(Cpu *); extern uint64_t rt_quit_dialog_begin(uint64_t); '
            f'uint64_t own = {owner}; c->x[0] = 0x39; c->x[1] = 0; c->x[30] = {ret:#x}ull; f_{v["open_screen"]:x}(c); '
            'uint64_t dlg = rt_quit_dialog_begin(own); '
            f'if (dlg) {{ c->x[0] = dlg; c->x[1] = 7; c->x[2] = 0; c->x[3] = 0; c->x[30] = {ret:#x}ull; '
            'c->pc = LD64(LD64(dlg) + 0x4e0); rt_dispatch(c); } }')


def _reopen_options(ret, v):
    # C: open the Options screen as the menus' Options buttons do: open request (FUN_7100d6fd90 with 0x34), then
    # vtable +0x28 (open, FUN_71012d90e0) with 1
    return (f'{{ extern void f_{v["open_screen"]:x}(Cpu *); extern void f_{v["screen_open_vt"]:x}(Cpu *); '
            'extern uint64_t rt_opt_screen(void); '
            f'c->x[0] = 0x34; c->x[1] = 0; c->x[30] = {ret:#x}ull; f_{v["open_screen"]:x}(c); '
            f'uint64_t scr = rt_opt_screen(); '
            f'if (scr) {{ c->x[0] = scr; c->x[1] = 1; c->x[30] = {ret:#x}ull; f_{v["screen_open_vt"]:x}(c); }} }}')


def _opt_rows(gen, a):
    # createRows (Options vtable +0x2f0): the runtime installs the row list of the page about to be shown
    return ('{ extern void rt_opt_rows(void); rt_opt_rows(); c->sp -= 0xc0; }'
            f'  /* {a:#x}: sub sp, sp, #0xc0 (Options: build rows) */')


def _opt_flag_type(gen, a):
    # row init: the game data flag type of the entry (x1); 0 bool, 1 s32
    fn = gen.patch_vars['flag_type_fn']
    return ('{ extern int rt_opt_flag_type(uint64_t); int t = rt_opt_flag_type(c->x[1]); '
            'if (t >= 0) c->x[0] = (uint32_t)t; '
            f'else {{ extern void f_{fn:x}(Cpu *); c->x[30] = {a + 4:#x}ull; f_{fn:x}(c); }} }}'
            f'  /* {a:#x}: bl {fn:#x} (Options: flag type) */')


def _opt_read(gen, a):
    # row value read (x0 = row): fills +0x54 bool, +0x58 value, +0x5c max, +0x60 min, +0x64 button state
    return ('{ extern int rt_opt_read(uint64_t); if (rt_opt_read(c->x[0])) return; '
            'uint64_t ea = c->sp - 0x30; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }'
            f'  /* {a:#x}: stp x29, x30, [sp, #-0x30]! (Options: read value) */')


def _opt_store(gen, a):
    # row value store (x0 = row)
    page = gen.patch_vars['option_page']
    return ('{ extern int rt_opt_store(uint64_t); if (rt_opt_store(c->x[0])) return; '
            f'c->x[8] = {page:#x}ull; }}'
            f'  /* {a:#x}: adrp x8, {page:#x} (Options: store value) */')


def _opt_reset(gen, a):
    # row reset to the default (x0 = row), from "reset to defaults" (X)
    return ('{ extern int rt_opt_reset(uint64_t); if (rt_opt_reset(c->x[0])) return; c->x[8] = LD32(c->x[0] + 0x68); }'
            f'  /* {a:#x}: ldr w8, [x0, #0x68] (Options: reset value) */')


def _opt_msg(gen, a):
    # every option text: message w21 of LayoutMsg/OptionWindow_00 into the {char16_t *, u32 length} at x2
    fn = gen.patch_vars['message_lookup']
    return ('{ extern int rt_opt_msg(uint32_t, uint64_t); if (rt_opt_msg((uint32_t)c->x[21], c->x[2])) c->x[0] = 0; '
            f'else {{ extern void f_{fn:x}(Cpu *); c->x[30] = {a + 4:#x}ull; f_{fn:x}(c); }} }}'
            f'  /* {a:#x}: bl {fn:#x} (Options: message text) */')


def _opt_decide(gen, a):
    # A on a row (x0 = row). 1: close the screen (a page change; the menu that opened it opens it again), 2: done,
    # 3: ask whether to quit
    v = gen.patch_vars
    fn = v['screen_close']
    return ('{ extern int rt_opt_decide(uint64_t); int r = rt_opt_decide(c->x[0]); '
            f'if (r == 1) {{ extern void f_{fn:x}(Cpu *); c->x[0] = LD64(c->x[0]); c->x[1] = 0xffffffffull; '
            f'c->x[30] = {a + 4:#x}ull; f_{fn:x}(c); return; }} '
            f'if (r == 3) {_quit_dialog(a + 4, "LD64(c->x[0])", v)} '
            'if (r) return; c->sp -= 0x30; }'
            f'  /* {a:#x}: sub sp, sp, #0x30 (Options: decide) */')


def _opt_tick(gen, a):
    # Options update (x0 = screen): the runtime may ask for one row to be redrawn (a binding changed); 1: skip the
    # update (the quit question is up: the screen's own dialog handling would take its answer)
    fn = gen.patch_vars['row_redraw']
    return ('{ extern uint64_t rt_opt_tick(uint64_t); uint64_t row = rt_opt_tick(c->x[0]); if (row == 1) return; '
            f'if (row) {{ extern void f_{fn:x}(Cpu *); uint64_t s0 = c->x[0], s30 = c->x[30]; '
            f'c->x[0] = row; c->x[1] = 0; c->x[30] = {a + 4:#x}ull; f_{fn:x}(c); c->x[0] = s0; c->x[30] = s30; }} '
            'c->sp -= 0x40; }'
            f'  /* {a:#x}: sub sp, sp, #0x40 (Options: update) */')


def _opt_back(gen, a):
    # B on the screen: it closes (after the save); on one of Savage Wind's pages it opens again on the first page
    fn = gen.patch_vars['screen_back']
    return ('{ extern void rt_opt_back(void); rt_opt_back(); '
            f'extern void f_{fn:x}(Cpu *); c->x[30] = {a + 4:#x}ull; f_{fn:x}(c); }}'
            f'  /* {a:#x}: bl {fn:#x} (Options: back) */')


def _title_tick(gen, a):
    # The title screen menu's update (screen 1, x0 = menu). While its Options button is active (+0x363c == 0x6a) it
    # waits for the Options screen (id 0x34) to go away; after a page change the runtime opens it again first. The
    # quit question's answer is read here too.
    return ('{ extern int rt_opt_title_tick(uint64_t); '
            f'if (rt_opt_title_tick(c->x[0])) {{ uint64_t s0 = c->x[0], s30 = c->x[30]; '
            f'{_reopen_options(a + 4, gen.patch_vars)} '
            'c->x[0] = s0; c->x[30] = s30; } '
            'uint64_t ea = c->sp - 0x50; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }'
            f'  /* {a:#x}: stp x29, x30, [sp, #-0x50]! (title menu: update) */')


def _title_event(gen, a):
    # The title screen menu's button handler (x0 = menu, x1 = button, its id at +0x44): the amiibo settings button
    # (0x6c; Savage Wind has no amiibo support) is Quit, which asks where to
    return ('{ extern int rt_title_event(uint32_t); '
            f'if (rt_title_event(LD32(c->x[1] + 0x44))) {{ {_quit_dialog(a + 4, "c->x[0]", gen.patch_vars)} return; }} '
            'c->sp -= 0x60; }'
            f'  /* {a:#x}: sub sp, sp, #0x60 (title menu: button) */')


def _title_msg(gen, a):
    # Message lookup (x0 = message file, x1 = label, x2 = {char16_t *, u32 length}): the title menu's file
    # (LayoutMsg/Title_00) gives "0003" (amiibo settings) as Quit
    file = gen.patch_vars['title_message_file']
    return ('{ extern int rt_title_msg(uint64_t, uint64_t); '
            f'if (c->x[0] == {file:#x}ull && rt_title_msg(c->x[1], c->x[2])) {{ c->x[0] = 0; return; }} '
            'c->sp -= 0x40; }'
            f'  /* {a:#x}: sub sp, sp, #0x40 (message lookup) */')


def _pause_option_state(gen, a):
    # The pause menu System page's Options state update (x0 = page): it leaves the state once the screen reports
    # closed. During a page change it waits instead, then opens it again.
    return ('{ extern int rt_opt_pause_tick(void); int r = rt_opt_pause_tick(); '
            f'if (r == 1) {_reopen_options(a + 4, gen.patch_vars)} '
            'if (r) return; '
            'uint64_t ea = c->sp - 0x20; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }'
            f'  /* {a:#x}: stp x29, x30, [sp, #-0x20]! (pause System page: Options state) */')


def _dialog_texts(gen, a):
    # The dialog's two-button layout (x0 = dialog, x1 = message, x2 / x3 = left / right button, {char16_t *, u32}
    # each): the quit question's texts
    return ('{ extern void rt_quit_dialog_texts(uint64_t, uint64_t, uint64_t); '
            'rt_quit_dialog_texts(c->x[1], c->x[2], c->x[3]); c->sp -= 0x50; }'
            f'  /* {a:#x}: sub sp, sp, #0x50 (dialog: two-button texts) */')


def _dialog_back(gen, a):
    # B on the dialog presses its cancel button (one of the two); the quit question just closes instead (no answer)
    off = gen.patch_vars['dialog_cancel_off']
    return ('{ extern int rt_quit_dialog_back(void); '
            f'c->x[0] = rt_quit_dialog_back() ? 0 : LD64(c->x[8] + {off:#x}); }}'
            f'  /* {a:#x}: ldr x0, [x8, #{off:#x}] (dialog: B) */')


# Frame rate (runtime/hle/framepace.c): the runtime paces the frames at the present and sets the game's clock. The
# frame loop's vsync wait (sead framework, 0x7000d811fc, x0 = its display object; only the first frames after start-up
# come here) waits for the display's vsync, then for more vsyncs until interval / 60 s have passed: skipped (the
# game's own runs with SAVAGEWIND_FRAMEPACE=0). ksys::VFR::updateInterval (x19 = VFR) has just set the interval ratio
# the game's time step comes from (interval / 2): the runtime puts the frame's real length there, before the time
# multipliers and the deltas are worked out from it.

def _frame_wait(gen, a):
    return ('{ extern int rt_frame_wait(void); if (rt_frame_wait()) return; '
            'uint64_t ea = c->sp - 0x30; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }'
            f'  /* {a:#x}: stp x29, x30, [sp, #-0x30]! (frame wait) */')


def _vfr_ratio(gen, a):
    return ('{ extern void rt_vfr_update(uint64_t); rt_vfr_update(c->x[19]); c->x[8] = LD32(c->x[19] + 0x78); }'
            f'  /* {a:#x}: ldr w8, [x19, #0x78] (VFR: interval ratio) */')


# {site: (the instruction word the patch was written for, the patch)}: gen.py stops when a site holds another word.
PATCH_BUILDS = {
    "de0b6ac4efc1dda7": {
        "sites": {
            0x7000cbd0b4: (0xa9be7bfd, _pause_option_state),  # stp x29, x30, [sp, #-0x20]!
            0x7000d1bb14: (0xb0007d88, _opt_store),          # adrp x8, 0x7001ccc000
            0x7000d1bd78: (0xd10303ff, _opt_rows),           # sub sp, sp, #0xc0
            0x7000d1c208: (0x940c1b8c, _opt_flag_type),      # bl 0x7001023038
            0x7000d1c234: (0xd10103ff, _opt_tick),           # sub sp, sp, #0x40
            0x7000d1c504: (0x9416f109, _opt_back),           # bl 0x70012d8928
            0x7000d1cab4: (0xa9bd7bfd, _opt_read),           # stp x29, x30, [sp, #-0x30]!
            0x7000d1d3d0: (0xd100c3ff, _opt_decide),         # sub sp, sp, #0x30
            0x7000d1dc40: (0x94014d74, _opt_msg),            # bl 0x7000d71210
            0x7000d1dde8: (0xb9406808, _opt_reset),          # ldr w8, [x0, #0x68]
            0x7000d44748: (0xd10143ff, _dialog_texts),       # sub sp, sp, #0x50
            0x7000d46364: (0xf959d900, _dialog_back),        # ldr x0, [x8, #0x33b0]
            0x7000d4bca0: (0xa9bb7bfd, _title_tick),         # stp x29, x30, [sp, #-0x50]!
            0x7000d4c5e0: (0xd10183ff, _title_event),        # sub sp, sp, #0x60
            0x7000d71210: (0xd10103ff, _title_msg),          # sub sp, sp, #0x40
            0x7000d811fc: (0xa9bd7bfd, _frame_wait),         # stp x29, x30, [sp, #-0x30]!
            0x70014092f4: (0xb9407a68, _vfr_ratio),          # ldr w8, [x19, #0x78]
        },
        "vars": _vars("de0b6ac4efc1dda7",
                      open_screen=0x7000d6fd90,    # FUN_7100d6fd90: the menus' screen open request
                      screen_open_vt=0x70012d90e0,  # FUN_71012d90e0: the Options screen's open (vtable +0x28)
                      flag_type_fn=0x7001023038,    # the game data flag's type
                      screen_close=0x70012d9124,    # FUN_71012d9124: the screen's close (vtable +0x30)
                      row_redraw=0x7000d1bba8,      # FUN_7100d1bba8: one row's redraw
                      screen_back=0x70012d8928,     # FUN_71012d8928: B (the screens' back)
                      option_page=0x7001ccc000,     # the page the option table's pointers live on (!)
                      message_lookup=0x7000d71210),  # the game's message lookup
    },
    "cd57b23fa4bbad65": {
        "sites": {
            0x7000dd0384: (0xa9be7bfd, _pause_option_state),
            0x7000e2ee44: (0x90008008, _opt_store),          # adrp x8, 0x7001e2e000
            0x7000e2f0a8: (0xd10303ff, _opt_rows),
            0x7000e2f538: (0x940c1e24, _opt_flag_type),      # bl 0x7001136dc8
            0x7000e2f564: (0xd10103ff, _opt_tick),
            0x7000e2f834: (0x9416f48d, _opt_back),           # bl 0x70013eca68
            0x7000e2fde4: (0xa9bd7bfd, _opt_read),
            0x7000e30700: (0xd100c3ff, _opt_decide),
            0x7000e30f70: (0x94014e40, _opt_msg),            # bl 0x7000e84870
            0x7000e31118: (0xb9406808, _opt_reset),
            0x7000e57af8: (0xd10143ff, _dialog_texts),
            0x7000e59714: (0xf959e500, _dialog_back),        # ldr x0, [x8, #0x33c8]
            0x7000e5f070: (0xa9bb7bfd, _title_tick),
            0x7000e5f9b0: (0xd10183ff, _title_event),
            0x7000e84870: (0xd10103ff, _title_msg),
            0x7000e9487c: (0xa9bd7bfd, _frame_wait),
            0x700151d494: (0xb9407a68, _vfr_ratio),
        },
        "vars": _vars("cd57b23fa4bbad65",
                      open_screen=0x7000e833f0,
                      screen_open_vt=0x70013ed220,
                      flag_type_fn=0x7001136dc8,
                      screen_close=0x70013ed264,
                      row_redraw=0x7000e2eed8,
                      screen_back=0x70013eca68,
                      option_page=0x7001e2e000,
                      message_lookup=0x7000e84870),
    },
}
