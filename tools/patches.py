"""Per-instruction replacements applied by gen.py (guest address -> function(gen, addr) returning C code).

Each entry replaces the single guest instruction at that address with hand-written C, for example
to redirect a hard-coded aspect-ratio load to the runtime. The addresses belong to one exact build
of the game's main module, so every patch must check the instruction word it expects before
replacing it (assert on the decoded word) and fail loudly on any other build.

The Link's Awakening patches this file held in LASRecomp are kept in the archived copy
(reference/LASRecomp/tools/patches.py) as more examples of the pattern.
"""
import struct


def _word(gen, a):
    return struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]


# Savage Wind's own pages in the game's Options screen (runtime/ui/game_options.mm), opened from the title screen
# menu (screen 1, buttons 0x69..0x6f, FUN_7100d4c5e0) or the pause menu's System page (buttons 0x4d..0x53,
# FUN_7100cbc918). The screen builds one row per entry of a static option table every time it opens (the table is
# read through the pointers at 0x7001cccd98..0x7001cccdb0), and every row goes through a handful of functions. The
# runtime points the table at a longer copy with its own entries (index >= the game's 14) and these hooks hand its
# rows to the runtime: values, texts and what A does. A page change closes the screen and opens it again with another
# row list, from whichever menu opened it. Quitting asks with the game's own two-button dialog (screen 0x39).

def _sub_sp(n):  # `sub sp, sp, #n`
    return 0xd10003ff | (n << 10)


def _quit_dialog(ret, owner):
    # C: open the game's dialog (screen 0x39) as a two-button question (type 7, vtable +0x4e0) over the screen `owner`
    # (a C expression; its input is off while the question is up); the runtime gives it its texts (_dialog_texts) and
    # reads the answer
    return ('{ extern void f_7000d6fd90(Cpu *); extern uint64_t rt_quit_dialog_begin(uint64_t); '
            f'uint64_t own = {owner}; c->x[0] = 0x39; c->x[1] = 0; c->x[30] = {ret:#x}ull; f_7000d6fd90(c); '
            'uint64_t dlg = rt_quit_dialog_begin(own); '
            f'if (dlg) {{ c->x[0] = dlg; c->x[1] = 7; c->x[2] = 0; c->x[3] = 0; c->x[30] = {ret:#x}ull; '
            'c->pc = LD64(LD64(dlg) + 0x4e0); rt_dispatch(c); } }')


def _reopen_options(ret):
    # C: open the Options screen as the menus' Options buttons do: open request (FUN_7100d6fd90 with 0x34), then
    # vtable +0x28 (open, FUN_71012d90e0) with 1
    return ('{ extern void f_7000d6fd90(Cpu *); extern void f_70012d90e0(Cpu *); extern uint64_t rt_opt_screen(void); '
            f'c->x[0] = 0x34; c->x[1] = 0; c->x[30] = {ret:#x}ull; f_7000d6fd90(c); uint64_t scr = rt_opt_screen(); '
            f'if (scr) {{ c->x[0] = scr; c->x[1] = 1; c->x[30] = {ret:#x}ull; f_70012d90e0(c); }} }}')


def _opt_rows(gen, a):
    # createRows (Options vtable +0x2f0): the runtime installs the row list of the page about to be shown
    assert _word(gen, a) == _sub_sp(0xc0), hex(_word(gen, a))
    return ('{ extern void rt_opt_rows(void); rt_opt_rows(); c->sp -= 0xc0; }'
            f'  /* {a:#x}: sub sp, sp, #0xc0 (Options: build rows) */')


def _opt_flag_type(gen, a):
    # row init: the game data flag type of the entry (x1); 0 bool, 1 s32
    assert _word(gen, a) >> 26 == 0x25, hex(_word(gen, a))  # bl 0x7001023038
    return ('{ extern int rt_opt_flag_type(uint64_t); int t = rt_opt_flag_type(c->x[1]); '
            'if (t >= 0) c->x[0] = (uint32_t)t; '
            'else { extern void f_7001023038(Cpu *); c->x[30] = 0x7000d1c20cull; f_7001023038(c); } }'
            f'  /* {a:#x}: bl 0x7001023038 (Options: flag type) */')


def _opt_read(gen, a):
    # row value read (x0 = row): fills +0x54 bool, +0x58 value, +0x5c max, +0x60 min, +0x64 button state
    assert _word(gen, a) == 0xa9bd7bfd, hex(_word(gen, a))  # stp x29, x30, [sp, #-0x30]!
    return ('{ extern int rt_opt_read(uint64_t); if (rt_opt_read(c->x[0])) return; '
            'uint64_t ea = c->sp - 0x30; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }'
            f'  /* {a:#x}: stp x29, x30, [sp, #-0x30]! (Options: read value) */')


def _opt_store(gen, a):
    # row value store (x0 = row)
    assert _word(gen, a) == 0xb0007d88, hex(_word(gen, a))  # adrp x8, 0x7001ccc000
    return ('{ extern int rt_opt_store(uint64_t); if (rt_opt_store(c->x[0])) return; c->x[8] = 0x7001ccc000ull; }'
            f'  /* {a:#x}: adrp x8, 0x7001ccc000 (Options: store value) */')


def _opt_reset(gen, a):
    # row reset to the default (x0 = row), from "reset to defaults" (X)
    assert _word(gen, a) == 0xb9406808, hex(_word(gen, a))  # ldr w8, [x0, #0x68]
    return ('{ extern int rt_opt_reset(uint64_t); if (rt_opt_reset(c->x[0])) return; c->x[8] = LD32(c->x[0] + 0x68); }'
            f'  /* {a:#x}: ldr w8, [x0, #0x68] (Options: reset value) */')


def _opt_msg(gen, a):
    # every option text: message w21 of LayoutMsg/OptionWindow_00 into the {char16_t *, u32 length} at x2
    assert _word(gen, a) >> 26 == 0x25, hex(_word(gen, a))  # bl 0x7000d71210
    return ('{ extern int rt_opt_msg(uint32_t, uint64_t); if (rt_opt_msg((uint32_t)c->x[21], c->x[2])) c->x[0] = 0; '
            'else { extern void f_7000d71210(Cpu *); c->x[30] = 0x7000d1dc44ull; f_7000d71210(c); } }'
            f'  /* {a:#x}: bl 0x7000d71210 (Options: message text) */')


def _opt_decide(gen, a):
    # A on a row (x0 = row). 1: close the screen (a page change; the menu that opened it opens it again), 2: done,
    # 3: ask whether to quit
    assert _word(gen, a) == _sub_sp(0x30), hex(_word(gen, a))
    return ('{ extern int rt_opt_decide(uint64_t); int r = rt_opt_decide(c->x[0]); '
            'if (r == 1) { extern void f_70012d9124(Cpu *); c->x[0] = LD64(c->x[0]); c->x[1] = 0xffffffffull; '
            'c->x[30] = 0x7000d1d3d4ull; f_70012d9124(c); return; } '
            f'if (r == 3) {_quit_dialog(a + 4, "LD64(c->x[0])")} '
            'if (r) return; c->sp -= 0x30; }'
            f'  /* {a:#x}: sub sp, sp, #0x30 (Options: decide) */')


def _opt_tick(gen, a):
    # Options update (x0 = screen): the runtime may ask for one row to be redrawn (a binding changed); 1: skip the
    # update (the quit question is up: the screen's own dialog handling would take its answer)
    assert _word(gen, a) == _sub_sp(0x40), hex(_word(gen, a))
    return ('{ extern uint64_t rt_opt_tick(uint64_t); uint64_t row = rt_opt_tick(c->x[0]); if (row == 1) return; '
            'if (row) { extern void f_7000d1bba8(Cpu *); uint64_t s0 = c->x[0], s30 = c->x[30]; '
            'c->x[0] = row; c->x[1] = 0; c->x[30] = 0x7000d1c238ull; f_7000d1bba8(c); c->x[0] = s0; c->x[30] = s30; } '
            'c->sp -= 0x40; }'
            f'  /* {a:#x}: sub sp, sp, #0x40 (Options: update) */')


def _opt_back(gen, a):
    # B on the screen: it closes (after the save); on one of Savage Wind's pages it opens again on the first page
    assert _word(gen, a) >> 26 == 0x25, hex(_word(gen, a))  # bl 0x70012d8928
    return ('{ extern void rt_opt_back(void); rt_opt_back(); extern void f_70012d8928(Cpu *); '
            'c->x[30] = 0x7000d1c508ull; f_70012d8928(c); }'
            f'  /* {a:#x}: bl 0x70012d8928 (Options: back) */')


def _title_tick(gen, a):
    # The title screen menu's update (screen 1, x0 = menu). While its Options button is active (+0x363c == 0x6a) it
    # waits for the Options screen (id 0x34) to go away; after a page change the runtime opens it again first. The
    # quit question's answer is read here too.
    assert _word(gen, a) == 0xa9bb7bfd, hex(_word(gen, a))  # stp x29, x30, [sp, #-0x50]!
    return ('{ extern int rt_opt_title_tick(uint64_t); '
            f'if (rt_opt_title_tick(c->x[0])) {{ uint64_t s0 = c->x[0], s30 = c->x[30]; {_reopen_options(a + 4)} '
            'c->x[0] = s0; c->x[30] = s30; } '
            'uint64_t ea = c->sp - 0x50; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }'
            f'  /* {a:#x}: stp x29, x30, [sp, #-0x50]! (title menu: update) */')


def _title_event(gen, a):
    # The title screen menu's button handler (x0 = menu, x1 = button, its id at +0x44): the amiibo settings button
    # (0x6c; Savage Wind has no amiibo support) is Quit, which asks where to
    assert _word(gen, a) == _sub_sp(0x60), hex(_word(gen, a))
    return ('{ extern int rt_title_event(uint32_t); '
            f'if (rt_title_event(LD32(c->x[1] + 0x44))) {{ {_quit_dialog(a + 4, "c->x[0]")} return; }} '
            'c->sp -= 0x60; }'
            f'  /* {a:#x}: sub sp, sp, #0x60 (title menu: button) */')


def _title_msg(gen, a):
    # Message lookup (x0 = message file, x1 = label, x2 = {char16_t *, u32 length}): the title menu's file
    # (LayoutMsg/Title_00 at 0x7001cd06d8) gives "0003" (amiibo settings) as Quit
    assert _word(gen, a) == _sub_sp(0x40), hex(_word(gen, a))
    return ('{ extern int rt_title_msg(uint64_t, uint64_t); '
            'if (c->x[0] == 0x7001cd06d8ull && rt_title_msg(c->x[1], c->x[2])) { c->x[0] = 0; return; } '
            'c->sp -= 0x40; }'
            f'  /* {a:#x}: sub sp, sp, #0x40 (message lookup) */')


def _pause_option_state(gen, a):
    # The pause menu System page's Options state update (x0 = page): it leaves the state once the screen reports
    # closed. During a page change it waits instead, then opens it again.
    assert _word(gen, a) == 0xa9be7bfd, hex(_word(gen, a))  # stp x29, x30, [sp, #-0x20]!
    return ('{ extern int rt_opt_pause_tick(void); int r = rt_opt_pause_tick(); '
            f'if (r == 1) {_reopen_options(a + 4)} '
            'if (r) return; '
            'uint64_t ea = c->sp - 0x20; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }'
            f'  /* {a:#x}: stp x29, x30, [sp, #-0x20]! (pause System page: Options state) */')


def _dialog_texts(gen, a):
    # The dialog's two-button layout (x0 = dialog, x1 = message, x2 / x3 = left / right button, {char16_t *, u32}
    # each): the quit question's texts
    assert _word(gen, a) == _sub_sp(0x50), hex(_word(gen, a))
    return ('{ extern void rt_quit_dialog_texts(uint64_t, uint64_t, uint64_t); '
            'rt_quit_dialog_texts(c->x[1], c->x[2], c->x[3]); c->sp -= 0x50; }'
            f'  /* {a:#x}: sub sp, sp, #0x50 (dialog: two-button texts) */')


def _dialog_back(gen, a):
    # B on the dialog presses its cancel button (one of the two); the quit question just closes instead (no answer)
    assert _word(gen, a) == 0xf959d900, hex(_word(gen, a))  # ldr x0, [x8, #0x33b0]
    return ('{ extern int rt_quit_dialog_back(void); c->x[0] = rt_quit_dialog_back() ? 0 : LD64(c->x[8] + 0x33b0); }'
            f'  /* {a:#x}: ldr x0, [x8, #0x33b0] (dialog: B) */')


# Frame rate (runtime/hle/framepace.c): the runtime paces the frames at the present and sets the game's clock. The
# frame loop's vsync wait (sead framework, 0x7000d811fc, x0 = its display object; only the first frames after start-up
# come here) waits for the display's vsync, then for more vsyncs until interval / 60 s have passed: skipped (the
# game's own runs with SAVAGEWIND_FRAMEPACE=0). ksys::VFR::updateInterval (x19 = VFR) has just set the interval ratio
# the game's time step comes from (interval / 2): the runtime puts the frame's real length there, before the time
# multipliers and the deltas are worked out from it.

def _frame_wait(gen, a):
    assert _word(gen, a) == 0xa9bd7bfd, hex(_word(gen, a))  # stp x29, x30, [sp, #-0x30]!
    return ('{ extern int rt_frame_wait(void); if (rt_frame_wait()) return; '
            'uint64_t ea = c->sp - 0x30; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }'
            f'  /* {a:#x}: stp x29, x30, [sp, #-0x30]! (frame wait) */')


def _vfr_ratio(gen, a):
    assert _word(gen, a) == 0xb9407a68, hex(_word(gen, a))  # ldr w8, [x19, #0x78]
    return ('{ extern void rt_vfr_update(uint64_t); rt_vfr_update(c->x[19]); c->x[8] = LD32(c->x[19] + 0x78); }'
            f'  /* {a:#x}: ldr w8, [x19, #0x78] (VFR: interval ratio) */')


# Cheats (F1 menu > Cheats; rt_cheat: runtime/settings.c). The sites are those of the Atmosphere cheats for this build
# (DE0B6AC4EFC1DDA7); each keeps the game's instruction, which runs as before while its cheat is off. `code` is C with
# {orig} where the game's instruction (lifted as usual) goes.
CHEAT_HEALTH, CHEAT_STAMINA, CHEAT_ARROWS, CHEAT_DURABILITY = 0, 1, 2, 3


def _cheat(word, cheat, what, code):
    def patch(gen, a):
        assert _word(gen, a) == word, hex(_word(gen, a))
        body = code.replace('{orig}', gen.lift(a).code).replace('{on}', f'rt_cheat({cheat})')
        return f'{{ extern int rt_cheat(int); {body} }}  /* {a:#x}: {what} (cheat) */'
    return patch


PATCHES_CHEATS = {
    # Player damage (FUN_7100bb17xx): w25 is the life after the hit, [x21] the life now, [sp, #0xc] a copy of w25.
    # Before the `cmp w25, #0` that takes the death path, a loss is undone (healing goes through).
    0x7000bb1988: _cheat(0x7100033f, CHEAT_HEALTH, 'cmp w25, #0',
                         'if ({on}) { uint32_t o = LD32(c->x[21]); '
                         'if ((int32_t)(uint32_t)c->x[25] < (int32_t)o) { c->x[25] = o; ST32(c->sp + 0xc, o); } } '
                         '{orig}'),
    # its one-quarter path: life - 1 stays life
    0x7000bb18b0: _cheat(0x51000509, CHEAT_HEALTH, 'sub w9, w8, #1',
                         'if ({on}) c->x[9] = (uint32_t)c->x[8]; else {orig}'),
    # life setter (FUN_7100bb3c70, x0: the life): a lower life isn't stored
    0x7000bb3c94: _cheat(0xb9000013, CHEAT_HEALTH, 'str w19, [x0]',
                         'if (!{on} || (int32_t)(uint32_t)c->x[19] >= (int32_t)LD32(c->x[0])) {orig}'),
    # stamina setter (FUN_7100bb3f30, a float at +0xa4): a lower stamina isn't stored
    0x7000bb3f50: _cheat(0xbd00a668, CHEAT_STAMINA, 'str s8, [x19, #0xa4]',
                         '{ uint32_t nv = VS32(8), ov = LD32(c->x[19] + 0xa4); float n, o; '
                         '__builtin_memcpy(&n, &nv, 4); __builtin_memcpy(&o, &ov, 4); '
                         'if (!{on} || !(n < o)) {orig} }'),
    # arrows shot (FUN_7100c80xxx: returns unless the item is an arrow): a stack keeps its count
    0x7000c806f0: _cheat(0x4b160128, CHEAT_ARROWS, 'sub w8, w9, w22',
                         'if ({on}) c->x[8] = (uint32_t)c->x[9]; else {orig}'),
    0x7000c806d0: _cheat(0xb900239f, CHEAT_ARROWS, 'str wzr, [x28, #0x20]',
                         'if (!{on}) {orig}'),
    # an item's life loss (FUN_7100a81da8: *x1 = max(0, *x1 - w9)): none for weapons ([x1 + 4] == -1, as the
    # Atmosphere code's cave tells them apart)
    0x7000a81f14: _cheat(0xb9401fa9, CHEAT_DURABILITY, 'ldr w9, [x29, #0x1c]',
                         '{orig} if ({on} && (int32_t)LD32(c->x[19] + 4) == -1) c->x[9] = 0;'),
}


PATCHES = {
    0x7000cbd0b4: _pause_option_state,
    0x7000d1bd78: _opt_rows,
    0x7000d1c208: _opt_flag_type,
    0x7000d1cab4: _opt_read,
    0x7000d1bb14: _opt_store,
    0x7000d1dde8: _opt_reset,
    0x7000d1dc40: _opt_msg,
    0x7000d1d3d0: _opt_decide,
    0x7000d1c234: _opt_tick,
    0x7000d1c504: _opt_back,
    0x7000d44748: _dialog_texts,
    0x7000d46364: _dialog_back,
    0x7000d4bca0: _title_tick,
    0x7000d4c5e0: _title_event,
    0x7000d71210: _title_msg,
    0x7000d811fc: _frame_wait,
    0x70014092f4: _vfr_ratio,
    **PATCHES_CHEATS,
}
