"""Per-instruction replacements applied by gen.py to the main module (address -> function(gen, addr) returning C).

Each entry replaces the single guest instruction at that address with hand-written C, for example
to redirect a hard-coded aspect-ratio load to the runtime. The addresses differ between game versions:
tools/game_sites.py finds them (and the functions and data the C uses) for the version being generated,
and table() maps them to these functions; gen.sites holds them all. Every patch still checks the
instruction word it expects before replacing it (assert on the decoded word) and fails loudly otherwise.
The 1.8.2 addresses in the comments are for reading along in a disassembly.

The Link's Awakening patches this file held in LASRecomp are kept in the archived copy
(reference/LASRecomp/tools/patches.py) as more examples of the pattern.
"""
import struct

from game_sites import adrp_page, sext


def _word(gen, a):
    return struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]


def _fn(gen, site):  # the generated function at a site's address
    return f'f_{gen.sites[site]:x}'


# Savage Wind's own pages in the game's Options screen (runtime/ui/game_options.mm), opened from the title screen menu
# (screen 1, buttons 0x69..0x6f, FUN_7100d4c5e0) or the pause menu's System page (buttons 0x4d..0x53, FUN_7100cbc918).
# The screen builds one row per entry of a static option table every time it opens (the table is read through the
# pointers at opt_table, 1.8.2 0x7001cccd98..0x7001cccdb0), and every row goes through a handful of functions. The
# runtime points the table at a longer copy with its own entries (index >= the game's 14) and these hooks hand its rows
# to the runtime: values, texts and what A does. A page change closes the screen and opens it again with another row
# list, from whichever menu opened it. Quitting asks with the game's own two-button dialog (screen 0x39).

def _sub_sp(n):  # `sub sp, sp, #n`
    return 0xd10003ff | (n << 10)


def _quit_dialog(gen, ret, owner):
    # C: open the game's dialog (screen 0x39) as a two-button question (type 7, vtable +0x4e0) over the screen `owner`
    # (a C expression; its input is off while the question is up); the runtime gives it its texts (_dialog_texts) and
    # reads the answer
    req = _fn(gen, 'screen_open_request')
    return (f'{{ extern void {req}(Cpu *); extern uint64_t rt_quit_dialog_begin(uint64_t); '
            f'uint64_t own = {owner}; c->x[0] = 0x39; c->x[1] = 0; c->x[30] = {ret:#x}ull; {req}(c); '
            'uint64_t dlg = rt_quit_dialog_begin(own); '
            f'if (dlg) {{ c->x[0] = dlg; c->x[1] = 7; c->x[2] = 0; c->x[3] = 0; c->x[30] = {ret:#x}ull; '
            'c->pc = LD64(LD64(dlg) + 0x4e0); rt_dispatch(c); } }')


def _reopen_options(gen, ret):
    # C: open the Options screen as the menus' Options buttons do: open request (screen_open_request, 1.8.2
    # FUN_7100d6fd90, with 0x34), then vtable +0x28 (open: screen_open, FUN_71012d90e0) with 1
    req, op = _fn(gen, 'screen_open_request'), _fn(gen, 'screen_open')
    return (f'{{ extern void {req}(Cpu *); extern void {op}(Cpu *); extern uint64_t rt_opt_screen(void); '
            f'c->x[0] = 0x34; c->x[1] = 0; c->x[30] = {ret:#x}ull; {req}(c); uint64_t scr = rt_opt_screen(); '
            f'if (scr) {{ c->x[0] = scr; c->x[1] = 1; c->x[30] = {ret:#x}ull; {op}(c); }} }}')


def _opt_rows(gen, a):
    # createRows (Options vtable +0x2f0): the runtime installs the row list of the page about to be shown
    assert _word(gen, a) == _sub_sp(0xc0), hex(_word(gen, a))
    return ('{ extern void rt_opt_rows(void); rt_opt_rows(); c->sp -= 0xc0; }'
            f'  /* {a:#x}: sub sp, sp, #0xc0 (Options: build rows) */')


def _opt_flag_type(gen, a):
    # row init: the game data flag type of the entry (x1); 0 bool, 1 s32
    assert _word(gen, a) >> 26 == 0x25, hex(_word(gen, a))  # bl gdt_flag_type
    t, fn = gen.sites['gdt_flag_type'], _fn(gen, 'gdt_flag_type')
    return ('{ extern int rt_opt_flag_type(uint64_t); int t = rt_opt_flag_type(c->x[1]); '
            'if (t >= 0) c->x[0] = (uint32_t)t; '
            f'else {{ extern void {fn}(Cpu *); c->x[30] = {a + 4:#x}ull; {fn}(c); }} }}'
            f'  /* {a:#x}: bl {t:#x} (Options: flag type) */')


def _opt_read(gen, a):
    # row value read (x0 = row): fills +0x54 bool, +0x58 value, +0x5c max, +0x60 min, +0x64 button state
    assert _word(gen, a) == 0xa9bd7bfd, hex(_word(gen, a))  # stp x29, x30, [sp, #-0x30]!
    return ('{ extern int rt_opt_read(uint64_t); if (rt_opt_read(c->x[0])) return; '
            'uint64_t ea = c->sp - 0x30; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }'
            f'  /* {a:#x}: stp x29, x30, [sp, #-0x30]! (Options: read value) */')


def _opt_store(gen, a):
    # row value store (x0 = row)
    assert _word(gen, a) & 0x9f00001f == 0x90000008, hex(_word(gen, a))  # adrp x8, <the option table's page>
    page = adrp_page(_word(gen, a), a)
    return (f'{{ extern int rt_opt_store(uint64_t); if (rt_opt_store(c->x[0])) return; c->x[8] = {page:#x}ull; }}'
            f'  /* {a:#x}: adrp x8, {page:#x} (Options: store value) */')


def _opt_reset(gen, a):
    # row reset to the default (x0 = row), from "reset to defaults" (X)
    assert _word(gen, a) == 0xb9406808, hex(_word(gen, a))  # ldr w8, [x0, #0x68]
    return ('{ extern int rt_opt_reset(uint64_t); if (rt_opt_reset(c->x[0])) return; c->x[8] = LD32(c->x[0] + 0x68); }'
            f'  /* {a:#x}: ldr w8, [x0, #0x68] (Options: reset value) */')


def _opt_msg(gen, a):
    # every option text: message w21 of LayoutMsg/OptionWindow_00 into the {char16_t *, u32 length} at x2
    assert _word(gen, a) >> 26 == 0x25, hex(_word(gen, a))  # bl msg_lookup
    t, fn = gen.sites['msg_lookup'], _fn(gen, 'msg_lookup')
    return ('{ extern int rt_opt_msg(uint32_t, uint64_t); if (rt_opt_msg((uint32_t)c->x[21], c->x[2])) c->x[0] = 0; '
            f'else {{ extern void {fn}(Cpu *); c->x[30] = {a + 4:#x}ull; {fn}(c); }} }}'
            f'  /* {a:#x}: bl {t:#x} (Options: message text) */')


def _opt_decide(gen, a):
    # A on a row (x0 = row). 1: close the screen (a page change; the menu that opened it opens it again), 2: done,
    # 3: ask whether to quit
    assert _word(gen, a) == _sub_sp(0x30), hex(_word(gen, a))
    close = _fn(gen, 'screen_close')
    return ('{ extern int rt_opt_decide(uint64_t); int r = rt_opt_decide(c->x[0]); '
            f'if (r == 1) {{ extern void {close}(Cpu *); c->x[0] = LD64(c->x[0]); c->x[1] = 0xffffffffull; '
            f'c->x[30] = {a + 4:#x}ull; {close}(c); return; }} '
            f'if (r == 3) {_quit_dialog(gen, a + 4, "LD64(c->x[0])")} '
            'if (r) return; c->sp -= 0x30; }'
            f'  /* {a:#x}: sub sp, sp, #0x30 (Options: decide) */')


def _opt_tick(gen, a):
    # Options update (x0 = screen): the runtime may ask for one row to be redrawn (a binding changed); 1: skip the
    # update (the quit question is up: the screen's own dialog handling would take its answer)
    assert _word(gen, a) == _sub_sp(0x40), hex(_word(gen, a))
    redraw = _fn(gen, 'opt_row_redraw')
    return ('{ extern uint64_t rt_opt_tick(uint64_t); uint64_t row = rt_opt_tick(c->x[0]); if (row == 1) return; '
            f'if (row) {{ extern void {redraw}(Cpu *); uint64_t s0 = c->x[0], s30 = c->x[30]; '
            f'c->x[0] = row; c->x[1] = 0; c->x[30] = {a + 4:#x}ull; {redraw}(c); c->x[0] = s0; c->x[30] = s30; }} '
            'c->sp -= 0x40; }'
            f'  /* {a:#x}: sub sp, sp, #0x40 (Options: update) */')


def _opt_back(gen, a):
    # B on the screen: it closes (after the save); on one of Savage Wind's pages it opens again on the first page
    assert _word(gen, a) >> 26 == 0x25, hex(_word(gen, a))  # bl screen_back
    t, fn = gen.sites['screen_back'], _fn(gen, 'screen_back')
    return (f'{{ extern void rt_opt_back(void); rt_opt_back(); extern void {fn}(Cpu *); '
            f'c->x[30] = {a + 4:#x}ull; {fn}(c); }}'
            f'  /* {a:#x}: bl {t:#x} (Options: back) */')


def _title_tick(gen, a):
    # The title screen menu's update (screen 1, x0 = menu). While its Options button is active ([title_options_state]
    # == 0x6a; 1.8.2 +0x363c) it waits for the Options screen (id 0x34) to go away; after a page change the runtime
    # opens it again first. The quit question's answer is read here too.
    assert _word(gen, a) == 0xa9bb7bfd, hex(_word(gen, a))  # stp x29, x30, [sp, #-0x50]!
    return ('{ extern int rt_opt_title_tick(uint64_t); '
            f'if (rt_opt_title_tick(c->x[0])) {{ uint64_t s0 = c->x[0], s30 = c->x[30]; {_reopen_options(gen, a + 4)} '
            'c->x[0] = s0; c->x[30] = s30; } '
            'uint64_t ea = c->sp - 0x50; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }'
            f'  /* {a:#x}: stp x29, x30, [sp, #-0x50]! (title menu: update) */')


def _title_event(gen, a):
    # The title screen menu's button handler (x0 = menu, x1 = button, its id at +0x44): the amiibo settings button
    # (0x6c; Savage Wind has no amiibo support) is Quit, which asks where to
    assert _word(gen, a) == _sub_sp(0x60), hex(_word(gen, a))
    return ('{ extern int rt_title_event(uint32_t); '
            f'if (rt_title_event(LD32(c->x[1] + 0x44))) {{ {_quit_dialog(gen, a + 4, "c->x[0]")} return; }} '
            'c->sp -= 0x60; }'
            f'  /* {a:#x}: sub sp, sp, #0x60 (title menu: button) */')


def _title_msg(gen, a):
    # Message lookup (x0 = message file, x1 = label, x2 = {char16_t *, u32 length}): the title menu's file
    # (LayoutMsg/Title_00, title_msg_file) gives "0003" (amiibo settings) as Quit
    assert _word(gen, a) == _sub_sp(0x40), hex(_word(gen, a))
    title = gen.sites['title_msg_file']
    return ('{ extern int rt_title_msg(uint64_t, uint64_t); '
            f'if (c->x[0] == {title:#x}ull && rt_title_msg(c->x[1], c->x[2])) {{ c->x[0] = 0; return; }} '
            'c->sp -= 0x40; }'
            f'  /* {a:#x}: sub sp, sp, #0x40 (message lookup) */')


def _pause_option_state(gen, a):
    # The pause menu System page's Options state update (x0 = page): it leaves the state once the screen reports
    # closed. During a page change it waits instead, then opens it again.
    assert _word(gen, a) == 0xa9be7bfd, hex(_word(gen, a))  # stp x29, x30, [sp, #-0x20]!
    return ('{ extern int rt_opt_pause_tick(void); int r = rt_opt_pause_tick(); '
            f'if (r == 1) {_reopen_options(gen, a + 4)} '
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
    w = _word(gen, a)
    assert w & 0xffc003ff == 0xf9400100, hex(w)  # ldr x0, [x8, #off] (1.8.2 0x33b0, 1.9.0 0x33c8: the screen grew)
    off = ((w >> 10) & 0xfff) * 8
    return ('{ extern int rt_quit_dialog_back(void); '
            f'c->x[0] = rt_quit_dialog_back() ? 0 : LD64(c->x[8] + {off:#x}); }}'
            f'  /* {a:#x}: ldr x0, [x8, #{off:#x}] (dialog: B) */')


# Frame rate (runtime/hle/framepace.c): the runtime paces the frames at the present and sets the game's clock. The frame
# loop's vsync wait (sead framework, 1.8.2 0x7000d811fc, x0 = its display object; only the first frames after start-up
# come here) waits for the display's vsync, then for more vsyncs until interval / 60 s have passed: skipped (the game's
# own runs with SAVAGEWIND_FRAMEPACE=0). ksys::VFR::updateInterval (x19 = VFR) has just set the interval ratio the
# game's time step comes from (interval / 2): the runtime puts the frame's real length there, before the time
# multipliers and the deltas are worked out from it.

def _frame_wait(gen, a):
    assert _word(gen, a) == 0xa9bd7bfd, hex(_word(gen, a))  # stp x29, x30, [sp, #-0x30]!
    return ('{ extern int rt_frame_wait(void); if (rt_frame_wait()) return; '
            'uint64_t ea = c->sp - 0x30; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }'
            f'  /* {a:#x}: stp x29, x30, [sp, #-0x30]! (frame wait) */')


def _vfr_ratio(gen, a):
    assert _word(gen, a) == 0xb9407a68, hex(_word(gen, a))  # ldr w8, [x19, #0x78]
    # (and online play's actors: runtime/online_actors.c, on this thread before the actors' update)
    return ('{ extern void rt_vfr_update(uint64_t); extern void rt_online_frame(Cpu *); rt_vfr_update(c->x[19]); '
            'rt_online_frame(c); c->x[8] = LD32(c->x[19] + 0x78); }'
            f'  /* {a:#x}: ldr w8, [x19, #0x78] (VFR: interval ratio) */')


# Arrows (#144): once in their fall, an arrow's velocity is its last direction x its speed plus a gravity vector
# (0, ArrowGravity, 0), added once per update with no time step (1.9.0 0x70005ee668; the straight part before it scales
# by the delta frame). Above 30 fps it fell 2 or 4 times as fast per second and landed short. The load of its Y part
# (the only one the game sets) takes the frame's length in console frames as a factor (runtime/hle/framepace.c
# rt_arrow_gravity: 1 at 30 fps, slow motion left out as on the console).

def _arrow_gravity(gen, a):
    assert _word(gen, a) == 0xbd410a81, hex(_word(gen, a))  # ldr s1, [x20, #0x108]
    return ('{ extern float rt_arrow_gravity(uint64_t, float); union { uint32_t u; float f; } g = { .u = (uint32_t)LD32(c->x[20] + 0x108) }; '
            'g.f = rt_arrow_gravity(c->x[20], g.f); c->v[1] = g.u; }'
            f'  /* {a:#x}: ldr s1, [x20, #0x108] (arrow gravity) */')


# The physics step's solver microsteps (runtime/hle/framepace.c rt_solver_steps, #161): before each Havok step the game
# gives the solver round(delta frame x 6) microsteps (1.9.0 0x700156f930): 6 at 30 fps, 3 at 60, 2 at 120. With fewer
# the solver gives way more to what is in contact with Link: holding a cucco, its body over his head held his jump
# to a third of its height above 90 fps. The runtime gives 6 / sqrt(delta frame) on shorter steps (12 at 120).

def _solver_steps(gen, a):
    assert _word(gen, a) == 0x1e220822, hex(_word(gen, a))  # fmul s2, s1, s2
    return ('{ extern float rt_solver_steps(float, float); union { uint32_t u; float f; } d = { .u = (uint32_t)c->v[1] }, '
            'k = { .u = (uint32_t)c->v[2] }; k.f = rt_solver_steps(d.f, k.f); c->v[2] = k.u; }'
            f'  /* {a:#x}: fmul s2, s1, s2 (solver microsteps: delta frame x 6) */')


# The sound system's frame (runtime/hle/framepace.c rt_snd_frame_floor, #145): once a frame the game hands its sound
# system the delta frame, at least 0.5 (1.9.0 0x7001551004; 1.8.2 0x710143ce64 in the Ghidra project): in console
# frames, with the frame rate (30) it makes the seconds every sound timer and fade steps by. The floor is a 60 fps
# frame, so above 60 fps the sound's clock ran fast: at ~95 fps each field battle music segment started the next one
# when it was 63% through, and the segments played over each other. The floor becomes 0.5 x the frame's length in
# console frames: 0.5 at 30 fps as on the console, and the real frame at any rate (slow motion floored as there).

def _snd_frame_floor(gen, a):
    assert _word(gen, a) == 0x1e2c1001, hex(_word(gen, a))  # fmov s1, #0.5
    return ('{ extern float rt_snd_frame_floor(void); union { uint32_t u; float f; } m = { .f = rt_snd_frame_floor() }; '
            'c->v[1] = m.u; }'
            f'  /* {a:#x}: fmov s1, #0.5 (sound frame: floor of the delta frame) */')


# The menus' speed (runtime/hle/framepace.c rt_gui_speed): once a frame the game stores its integer interval x 0.5 as
# the GUI speed (the float at +0xb20 of a UI object; 1.8.2 0x700116d0e8): 1 at 30 fps, 0.5 at 60 and never lower, so
# the menus' cursor and animations ran faster above 60 fps. The runtime gives the frame's real length instead, as Cemu's
# FPS++ does on the Wii U (v208 0x03415afc).

def _gui_speed(gen, a):
    assert _word(gen, a) == 0xbd0b2100, hex(_word(gen, a))  # str s0, [x8, #0xb20]
    return ('{ extern float rt_gui_speed(float); union { uint32_t u; float f; } s = { .u = (uint32_t)c->v[0] }; '
            's.f = rt_gui_speed(s.f); ST32(c->x[8] + 0xb20, s.u); }'
            f'  /* {a:#x}: str s0, [x8, #0xb20] (GUI speed) */')


# MainShortCut's HoldShortCutBg starts P_Capture_00 transparent and has an alpha jump at frame 1. The screen
# corrects that boundary only at frame == 1 (1.9.0 0x7000e26a38). The wall-time GUI
# clock can present fractional opening frames below 1, or skip the exact value: the backdrop briefly turns black
# (#173). Apply the same alpha correction once the animation has started, leaving frame 0 and stopped animations
# alone. Keep the GUI clock unchanged so both the quick selectors and the other menus retain their normal speed.
def _quick_menu_capture_alpha(gen, a):
    w = _word(gen, a)
    assert w & 0xff00001f == 0x54000001, hex(w)  # b.ne <skip alpha correction>
    assert _word(gen, a - 4) == 0x1e212000 and _word(gen, a - 8) == 0x1e2e1001
    target = a + sext((w >> 5) & 0x7ffff, 19) * 4
    return ('{ union { uint32_t u; float f; } frame = { .u = (uint32_t)c->v[0] }; '
            f'if (!(frame.f > 0.0f)) goto L_{target:x}; }} '
            f'/* {a:#x}: b.ne (quick-menu capture alpha after animation starts) */')


# Perspective projections' matrix (sead's PerspectiveProjection, its vtable +0x58; 1.8.2 0x7000da13f4): it reads
# tan(fovy / 2) at +0xac and the aspect ratio at +0xb0 here. The runtime may change both before the matrix is built:
# a 16:9 view widened to the window's shape (ultrawide: more to the sides, not stretched) and the field of view
# (runtime/nvn/output_scale.c rt_persp).

def _persp_matrix(gen, a):
    assert _word(gen, a) == 0x2d558801, hex(_word(gen, a))  # ldp s1, s2, [x0, #0xac]
    return ('{ extern void rt_persp(uint64_t, uint32_t *, uint32_t *); uint32_t t = LD32(c->x[0] + 0xac), '
            'k = LD32(c->x[0] + 0xb0); rt_persp(c->x[0], &t, &k); c->v[1] = t; c->v[2] = k; }'
            f'  /* {a:#x}: ldp s1, s2, [x0, #0xac] (perspective matrix: tan(fovy / 2), aspect) */')


# Frustum projections' matrix (sead's FrustumProjection, its vtable +0x58; 1.8.2 0x7000da18d4): left / right at
# +0xa8 / +0xac and top / bottom at +0xa0 / +0xa4, each pair loaded twice. Some HUD layers are these (the menus'
# cursor): a 16:9 one is widened as the perspective ones are (runtime/nvn/output_scale.c rt_frustum).

def _frustum_load(word, regs, what):
    def patch(gen, a):
        assert _word(gen, a) == word, hex(_word(gen, a))
        r0, r1 = regs
        k = 0 if what == 'lr' else 2
        return ('{ extern void rt_frustum(uint64_t, float *); float f[4]; rt_frustum(c->x[0], f); '
                f'union {{ float f; uint32_t u; }} p0 = {{ f[{k}] }}, p1 = {{ f[{k + 1}] }}; '
                f'c->v[{r0}] = p0.u; c->v[{r1}] = p1.u; }}'
                f'  /* {a:#x}: ldp s{r0}, s{r1}, [x0, #{0xa8 if what == "lr" else 0xa0:#x}] (frustum matrix) */')
    return patch


# Projections' update (sead's Projection, 1.8.2 0x7000da0e18): every getter of the matrix, the device matrix and a
# point's projection makes the matrix again first when its dirty flag (+8) is set. It is taken as always set, so every
# matrix follows the window's shape and the field of view now (runtime/nvn/output_scale.c rt_persp): one made before
# the first frame, or before a change, stayed as it was (the menus' cursor is drawn by a HUD layer made at startup).

def _proj_update(gen, a):
    assert _word(gen, a) == 0x39402008, hex(_word(gen, a))  # ldrb w8, [x0, #8]
    return f'c->x[8] = 1;  /* {a:#x}: ldrb w8, [x0, #8] (projection update: dirty, always) */'


# ... and its load of the view's offset (+0xb4 x, +0xb8 y: a frustum moved by a fraction of its width and height; the
# inventory's Link is drawn to the right that way), which the widened view takes as a fraction of its 16:9 part
# (runtime/nvn/output_scale.c rt_persp_offset).

def _persp_matrix_offset(gen, a):
    assert _word(gen, a) == 0x2d569404, hex(_word(gen, a))  # ldp s4, s5, [x0, #0xb4]
    return ('{ extern void rt_persp_offset(uint64_t, uint32_t *, uint32_t *); uint32_t x = LD32(c->x[0] + 0xb4), '
            'y = LD32(c->x[0] + 0xb8); rt_persp_offset(c->x[0], &x, &y); c->v[4] = x; c->v[5] = y; }'
            f'  /* {a:#x}: ldp s4, s5, [x0, #0xb4] (perspective matrix: offset) */')


# Perspective projections' screen position to camera position (their vtable +0x68; 1.8.2 0x7000da14c0): the game
# places some things on screen with it (the menus' cursor corners), so it takes the same widened view as the matrix.

def _persp_unproject_y(gen, a):
    assert _word(gen, a) == 0xbd40ac01, hex(_word(gen, a))  # ldr s1, [x0, #0xac]
    return ('{ extern void rt_persp(uint64_t, uint32_t *, uint32_t *); uint32_t t = LD32(c->x[0] + 0xac), '
            'k = LD32(c->x[0] + 0xb0); rt_persp(c->x[0], &t, &k); c->v[1] = t; }'
            f'  /* {a:#x}: ldr s1, [x0, #0xac] (screen to camera: tan(fovy / 2)) */')


def _persp_unproject_x(gen, a):
    assert _word(gen, a) == 0x2d558c02, hex(_word(gen, a))  # ldp s2, s3, [x0, #0xac]
    return ('{ extern void rt_persp(uint64_t, uint32_t *, uint32_t *); uint32_t t = LD32(c->x[0] + 0xac), '
            'k = LD32(c->x[0] + 0xb0); rt_persp(c->x[0], &t, &k); c->v[2] = t; c->v[3] = k; }'
            f'  /* {a:#x}: ldp s2, s3, [x0, #0xac] (screen to camera: tan(fovy / 2), aspect) */')


# The mouse camera (runtime/hle/input.c). Every camera action (CameraChase, CameraAiming, CameraHorse...) reads the
# right stick through one function (1.9.0 0x7000d57fe0), which applies a dead zone, squares the tilt (v |v|) and
# keeps it in the unit circle; the chase-like ones (CameraChase, CameraShieldSurfing, CameraWakeboard) square the
# vertical part once more (y |y|). The camera then turns by that x its speeds x the frame's length (VFR). Fine for a
# stick; from the mouse it crept on slow moves, sped up on fast ones and stopped at the stick's top speed. While the
# mouse has the camera the runtime answers the stick input itself (rt_cam_stick: the frame's mouse motion over the
# frame's length, linear, not clamped) and the vertical part goes through unsquared (rt_cam_pitch), so a frame's turn
# is the mouse's motion in it times the sensitivity, at any frame rate. The cameras turn the horizontal part 1.78
# times as fast as the vertical one for a stick, and the bow's (CameraAiming) and a held item's (CameraAiming2) scale
# them apart again (lngStickScale 0.7 and latStickScale 1; 0.8 and 1.5): the runtime gives the vertical part 1.78
# times the mouse's motion, and those two cameras' vertical part the horizontal part's scale (rt_cam_pitch_scale), so
# the mouse turns both alike. Optional sites (tools/game_sites.py OPTIONAL_GROUPS): without them the mouse moves the
# stick, as before.

def _cam_stick(gen, a):
    assert _word(gen, a) == _sub_sp(0x30), hex(_word(gen, a))
    return ('{ extern int rt_cam_stick(uint64_t, uint64_t); if (rt_cam_stick(c->x[0], c->x[30])) return; '
            'c->sp -= 0x30; }'
            f'  /* {a:#x}: sub sp, sp, #0x30 (camera stick input) */')


def _cam_pitch(gen, a):
    # fmul s1, s0, s0 just above, fnmul s3, s0, s0 here: the vertical part kept its sign squared, s1 or s3 by s0's sign
    assert _word(gen, a) == 0x1e208803 and _word(gen, a - 4) == 0x1e200801, hex(_word(gen, a))
    return ('{ extern int rt_cam_pitch(float *); union { uint32_t u; float f; } y = { .u = (uint32_t)c->v[0] }; '
            'if (rt_cam_pitch(&y.f)) c->v[1] = y.u; else y.f = -(y.f * y.f); c->v[3] = y.u; }'
            f'  /* {a:#x}: fnmul s3, s0, s0 (camera: the vertical stick part squared) */')


def _cam_pitch_aim(gen, a):
    # ldr s1, [x8]: the camera's latStickScale, through the parameter pointer loaded just above (ldr x8, [x19, #off]);
    # its lngStickScale's pointer is the next one (the parameters load in that order)
    assert _word(gen, a) == 0xbd400101, hex(_word(gen, a))
    w = next(_word(gen, a - 4 * i) for i in range(1, 5) if _word(gen, a - 4 * i) & 0xffc003ff == 0xf9400268)
    lng = ((w >> 10) & 0xfff) * 8 + 8
    return ('{ extern uint32_t rt_cam_pitch_scale(uint32_t, uint32_t); '
            f'c->v[1] = rt_cam_pitch_scale(LD32(c->x[8]), LD32(LD64(c->x[19] + {lng:#x}))); }}'
            f'  /* {a:#x}: ldr s1, [x8] (camera: latStickScale) */')


# The game's Camera X-axis / Y-axis options (Inverted) are applied by the cameras' controller (the one the camera
# input reads, id 3 of the controller manager): every frame it reads the flags (CameraRLReverse, CameraUpDownReverse)
# and turns that axis of its right stick around. The mouse's camera input doesn't go through it, so the runtime is
# told the flags here and turns the mouse's motion around the same way (rt_cam_reverse).

def _cam_reverse(axis):
    def patch(gen, a):
        w = _word(gen, a)
        assert w >> 26 == 0x25, hex(w)  # bl <the flag's getter>
        t = a + sext(w & 0x3FFFFFF, 26) * 4
        return (f'{{ extern void f_{t:x}(Cpu *); extern void rt_cam_reverse(int, int); c->x[30] = {a + 4:#x}ull; '
                f'f_{t:x}(c); rt_cam_reverse({axis}, (int)(c->x[0] & 1)); }}'
                f'  /* {a:#x}: bl {t:#x} (camera controller: {"XY"[axis]}-axis inverted) */')
    return patch


# tools/game_sites.py site -> its patch
# BOTW's streaming Yaz0 decoder (1.9.0 0x700131d674; x0 its state, x1/w2 the input chunk): in native code
# (runtime/hle/yaz0.c rt_yaz0, which declines the first call of a stream, the one that reads its header). Its return
# instruction checks the native result against the game's code with SAVAGEWIND_YAZ0=2.
def _yaz0_stream(gen, a):
    assert _word(gen, a) == 0xa9bd7bfd, hex(_word(gen, a))  # stp x29, x30, [sp, #-0x30]!
    return ('{ extern int rt_yaz0(Cpu *); if (rt_yaz0(c)) return; '
            'uint64_t ea = c->sp - 0x30; ST64(ea, c->x[29]); ST64(ea + 8, c->x[30]); c->sp = ea; }'
            f'  /* {a:#x}: stp x29, x30, [sp, #-0x30]! (Yaz0 stream decoder) */')


def _yaz0_stream_ret(gen, a):
    assert _word(gen, a) == 0xd65f03c0, hex(_word(gen, a))  # ret
    return '{ extern void rt_yaz0_done(Cpu *); rt_yaz0_done(c); return; }' + f'  /* {a:#x}: ret (Yaz0 stream decoder) */'


PATCHES = {
    'pause_options_update': _pause_option_state,
    'opt_create_rows': _opt_rows,
    'opt_row_init_flag_type': _opt_flag_type,
    'opt_read': _opt_read,
    'opt_store': _opt_store,
    'opt_reset': _opt_reset,
    'opt_msg_call': _opt_msg,
    'opt_decide': _opt_decide,
    'opt_update': _opt_tick,
    'opt_back_call': _opt_back,
    'dialog_texts': _dialog_texts,
    'dialog_back': _dialog_back,
    'title_update': _title_tick,
    'title_button': _title_event,
    'msg_lookup': _title_msg,
    'frame_wait': _frame_wait,
    'yaz0_stream': _yaz0_stream,
    'yaz0_stream_ret': _yaz0_stream_ret,
    'vfr_ratio': _vfr_ratio,
    'gui_speed': _gui_speed,
    'quick_menu_capture_alpha': _quick_menu_capture_alpha,
    'arrow_fall_gravity': _arrow_gravity,
    'phys_solver_steps': _solver_steps,
    'snd_frame_floor': _snd_frame_floor,
    'persp_matrix': _persp_matrix,
    'persp_matrix_offset': _persp_matrix_offset,
    'persp_unproject_y': _persp_unproject_y,
    'persp_unproject_x': _persp_unproject_x,
    'proj_update': _proj_update,
    'frustum_lr0': _frustum_load(0x2d550001, (1, 0), 'lr'),
    'frustum_lr1': _frustum_load(0x2d550c02, (2, 3), 'lr'),
    'frustum_tb0': _frustum_load(0x2d540800, (0, 2), 'tb'),
    'frustum_tb1': _frustum_load(0x2d540c02, (2, 3), 'tb'),
    'cam_stick': _cam_stick,
    'cam_pitch_chase': _cam_pitch,
    'cam_pitch_surf': _cam_pitch,
    'cam_pitch_wakeboard': _cam_pitch,
    'cam_pitch_aim': _cam_pitch_aim,
    'cam_pitch_item': _cam_pitch_aim,
    'cam_reverse_x': _cam_reverse(0),
    'cam_reverse_y': _cam_reverse(1),
}


def table(sites):
    """address -> patch, for the main module whose sites these are (tools/game_sites.py; an optional site it lacks
    has no patch)"""
    return {sites[name]: fn for name, fn in PATCHES.items() if name in sites}
