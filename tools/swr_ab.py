#!/usr/bin/env python3
"""One play session, every optimization measured: how much each cpuopts bit adds, from windows of normal play.

The game runs with SAVAGEWIND_CONTROL=<port> and SAVAGEWIND_VK_CPU_BENCH=1 (docs/PERF-CPU-CANDIDATES.md). While you
play, this tool switches the optimizations on and off live (`cpuopts <mask>`; the two low bits, the material cache mode
the game started with, stay as they are in every window), three seconds at a time, in a shuffled
order that is a balanced design (a Hadamard matrix of 16 windows, run eight times, every second run with every switch
inverted, the "foldover"), so that every switch is on in half of the windows and off in the other half, and the others
average out in each half. Short windows and many of them, because the scene drifts: the error of an effect falls with the
number of windows, and a drift is slow against a window. After the last window a least squares fit with a level for each
run of the design, a time trend and the scene's draw count gives each switch's effect on the frame time and on each
thread's CPU time per frame, with its standard error. Columns of the design that no switch uses are
fitted too: what they "find" is noise, so they show how small an effect can still be told from none.

    tools/swr_ab.py --port 7790                 (play; the default plan is about 8.5 minutes)
    tools/swr_ab.py --port 7790 --dry-run       (print the plan, connect to nothing)
    tools/swr_ab.py --analyze build/swr_ab_X.json   (fit a saved session again)

What it can and cannot say:
  - An effect is in ms per frame with an interval (2 standard errors, about 95%). Below that interval's width it says
    "within noise": the session cannot see it, which is not the same as "nothing". Effects of 0.1 ms and less need
    much longer sessions (the error shrinks with the square root of the windows).
  - The frame time is the truth: a gain in a thread that is not the limit does not move it. The thread columns say
    where a gain happened (CPU time per frame; a thread that spins while it waits reads high and moves with its limit).
  - The scene changes under you. Standing in a busy place, turning the camera now and then, keeps it steadier than
    running across the map; the fit corrects for the draw count and a drift in time, not for everything.
  - Switch 512 draws far shadow cascades every second frame: the picture changes a little during its windows.
    Leave it out with --no-visual.
  - Interactions (two switches that help less together than apart) are balanced out of each effect, not measured. The
    all-off and all-on windows show what the combination really gave next to the sum of the single effects.
"""
import argparse
import json
import math
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# (bit, short name, what it is)
FACTORS = [
    (16, 'set-hash', 'descriptor-set hash bound by throughput'),
    (32, 'tex-watch', 'texture change detection from the write watch'),
    (64, 'copy-line', 'cache-line guest copy table and prefetches'),
    (128, 'gw-bitmap', 'block bitmap for the storing-shader search, static scratch'),
    (512, 'shadow-budget', 'far shadow cascades every second frame (changes the picture)'),
    (1024, 'dispatch', 'per-thread dispatch table for indirect calls'),
    (2048, 'cb-unlink', 'command buffer leaves the registry in O(1)'),
]
METRICS = [  # (key, heading)
    ('frame_ms', 'frame'),
    ('main_ms', 'main'),
    ('render_ms', 'render'),
    ('recorder_ms', 'recorder'),
    ('guest_ms', 'all guest'),
]


# ---------------------------------------------------------------- the design

def hadamard_columns(n):
    """The n-1 non-constant columns of the Sylvester Hadamard matrix of order n (a power of two): orthogonal, +-1."""
    return [[1 - 2 * (bin(i & j).count('1') & 1) for i in range(n)] for j in range(1, n)]


def make_plan(factors, passes, confirm, rng):
    """Windows in a random order. factors: [(bit, ...)]. passes: runs of the design (odd ones inverted, the foldover), each in its own random order.
    confirm: all-off and all-on windows each, mixed in. Every window: mask, kind, codes (one +-1 per design column)."""
    nf = len(factors)
    n = 4
    while n - 1 < nf + 3:  # (at least three spare columns: the noise probes that give the noise floor)
        n *= 2
    cols = hadamard_columns(n)
    plan = []
    for p in range(passes):  # (a pass: the whole design, in its own random order; the scene drifts less within it)
        sign = -1 if p % 2 else 1
        rows = []
        for r in range(n):
            codes = [sign * cols[c][r] for c in range(n - 1)]
            mask = sum(f[0] for k, f in enumerate(factors) if codes[k] > 0)
            rows.append({'kind': 'design', 'mask': mask, 'codes': codes, 'block': p})
        rng.shuffle(rows)
        plan += rows
    allbits = sum(f[0] for f in factors)
    for _ in range(confirm):
        for kind, mask in (('off', 0), ('on', allbits)):
            plan.insert(rng.randrange(len(plan) + 1), {'kind': kind, 'mask': mask, 'codes': None, 'block': -1})
    for i, w in enumerate(plan):
        w['index'] = i
    return plan, n


# ---------------------------------------------------------------- least squares (no libraries)

# The fit's standard errors assume the leftover scatter of one window is independent of the next one's; a scene drifts, so
# it is not, and the plain errors are about 15% too small (against a simulated game with a drifting scene, tools/
# test_ab_play.py). Every error here is multiplied by this to make up for it.
DRIFT_ALLOWANCE = 1.25

def solve_normal(X, y):
    """Least squares by the normal equations: (beta, (X'X)^-1, rss). X: rows of regressors. Raises ValueError when the
    regressors are linearly dependent."""
    n, p = len(X), len(X[0])
    a = [[sum(X[r][i] * X[r][j] for r in range(n)) for j in range(p)] + [1.0 if i == j else 0.0 for j in range(p)]
         for i in range(p)]
    b = [sum(X[r][i] * y[r] for r in range(n)) for i in range(p)]
    for i in range(p):
        piv = max(range(i, p), key=lambda k: abs(a[k][i]))
        if abs(a[piv][i]) < 1e-9:
            raise ValueError('the regressors are linearly dependent (too few windows?)')
        a[i], a[piv] = a[piv], a[i]
        b[i], b[piv] = b[piv], b[i]
        d = a[i][i]
        a[i] = [v / d for v in a[i]]
        b[i] /= d
        for k in range(p):
            if k != i and a[k][i]:
                f = a[k][i]
                a[k] = [v - f * w for v, w in zip(a[k], a[i])]
                b[k] -= f * b[i]
    inv = [row[p:] for row in a]
    rss = sum((y[r] - sum(b[j] * X[r][j] for j in range(p))) ** 2 for r in range(n))
    return b, inv, rss


def median(v):
    v = sorted(v)
    n = len(v)
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2


def fit_effects(windows, factors, metric, nprobe_cols, adjust, use_blocks=True, trim=True):
    """Effect of each switch on metric (on minus off) with its standard error, from the design windows; and the same
    for each spare design column (noise probes). adjust: add the draws per frame as a regressor. trim: a window whose
    leftover is more than 4 robust standard deviations out (a hitch, a loading screen) is dropped and the fit repeated."""
    rows = [w for w in windows if w['kind'] == 'design' and w.get('ok', True) and metric in w['metrics']]
    if len(rows) < len(factors) + 4:
        raise ValueError('only %d usable windows for %d switches' % (len(rows), len(factors)))
    dropped = 0
    for _ in range(3):
        out, probes, info, resid = _fit_rows(rows, factors, metric, nprobe_cols, adjust, use_blocks)
        if not trim:
            break
        spread = 1.4826 * median([abs(r - median(resid)) for r in resid])
        keep = [w for w, r in zip(rows, resid) if spread <= 0 or abs(r - median(resid)) <= 4 * spread]
        if len(keep) == len(rows) or len(keep) < len(factors) + 4:
            break
        dropped += len(rows) - len(keep)
        rows = keep
    info['dropped'] = dropped
    return out, probes, info


def _fit_rows(rows, factors, metric, nprobe_cols, adjust, use_blocks):
    n = len(rows)
    idx = [w['index'] for w in rows]
    mid, half = sum(idx) / n, (max(idx) - min(idx)) / 2 or 1.0
    dr = [w['metrics']['draws'] for w in rows]
    if adjust:
        # The scene's draw count with what the switches themselves change taken out (bit 512 cuts draws; that saving is
        # part of its effect, so it must not be "corrected" away): the residual of the draws on the switches' columns.
        F = [[1.0] + [float(w['codes'][c]) for c in range(len(factors))] for w in rows]
        bd, _, _ = solve_normal(F, dr)
        dr = [dr[k] - sum(bd[j] * F[k][j] for j in range(len(bd))) for k in range(n)]
    dmean = sum(dr) / n
    dscale = (max(abs(v - dmean) for v in dr) or 1.0)
    y = [w['metrics'][metric] for w in rows]
    blocks = sorted({w.get('block', 0) for w in rows})[1:]  # (a level of its own for every pass but the first)

    def base(extra=()):
        X = []
        for k, w in enumerate(rows):
            r = [1.0] + [float(w['codes'][c]) for c in range(len(factors))] + [(w['index'] - mid) / half]
            r += [1.0 if w.get('block', 0) == b else 0.0 for b in blocks] if use_blocks else []
            if adjust:
                r.append((dr[k] - dmean) / dscale)
            r += [float(w['codes'][c]) for c in extra]
            X.append(r)
        return X

    out = {}
    X = base()
    beta, inv, rss = solve_normal(X, y)
    df = n - len(beta)
    s2 = rss / df if df > 0 else float('nan')
    for k, f in enumerate(factors):
        out[f[0]] = (2 * beta[1 + k], DRIFT_ALLOWANCE * 2 * math.sqrt(max(s2 * inv[1 + k][1 + k], 0.0)), df)
    probes = []
    for c in nprobe_cols:
        b2, inv2, rss2 = solve_normal(base((c,)), y)
        probes.append(2 * b2[-1])
    resid = [y[r] - sum(beta[j] * X[r][j] for j in range(len(beta))) for r in range(n)]
    return out, probes, {'mean': sum(y) / n, 'n': n, 'sigma': math.sqrt(s2) if s2 == s2 else float('nan')}, resid


def trim_values(v):
    """Without the values more than 4 robust standard deviations from the median (a window with a hitch in it)."""
    if len(v) < 4:
        return v
    m = median(v)
    spread = 1.4826 * median([abs(x - m) for x in v])
    return [x for x in v if spread <= 0 or abs(x - m) <= 4 * spread]


def welch(a, b):
    """(mean(a) - mean(b), standard error) of two samples."""
    if len(a) < 2 or len(b) < 2:
        return None
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    va = sum((v - ma) ** 2 for v in a) / (len(a) - 1)
    vb = sum((v - mb) ** 2 for v in b) / (len(b) - 1)
    return ma - mb, math.sqrt(va / len(a) + vb / len(b))


def verdict(effect, se):
    t = abs(effect) / se if se > 0 else 0.0
    return 'resolved' if t >= 3 else 'probable' if t >= 2 else 'within noise'


# ---------------------------------------------------------------- measuring

def snapshot(game):
    th = game.cmd('threads')
    co = game.cmd('cpuopts')
    return th, co


def window_metrics(a, b, frames, main_key):
    (tha, coa), (thb, cob) = a, b
    presents = thb['presents'] - tha['presents']
    if presents <= 0:
        return None
    per = lambda k: (thb[k] - tha[k]) / presents if k in thb and k in tha and tha[k] >= 0 else None
    m = {
        'frame_ms': float(frames['frame_ms_avg']),
        'p99_ms': float(frames['frame_ms_p99']),
        'presents': presents,
        'draws': (cob['draws'] - coa['draws']) / presents,
        'seconds': (thb['now_ms'] - tha['now_ms']) / 1000.0,
    }
    for key, name in (('host.render', 'render_ms'), ('host.recorder', 'recorder_ms'), (main_key, 'main_ms')):
        v = per(key)
        if v is not None:
            m[name] = v
    guest = [per(k) for k in thb if k.startswith('guest.')]
    m['guest_ms'] = sum(v for v in guest if v is not None)
    m['fps'] = 1000.0 / m['frame_ms'] if m['frame_ms'] > 0 else 0.0
    return m


def find_main_key(game, sleep):
    a = game.cmd('threads')
    sleep(2.0)
    b = game.cmd('threads')
    pref = [k for k in b if k.startswith('guest.101_')]
    if pref:
        return pref[0]
    guests = sorted((k for k in b if k.startswith('guest.') and k in a), key=lambda k: b[k] - a[k], reverse=True)
    if not guests:
        raise RuntimeError('the threads command lists no guest threads')
    return guests[0]


def run_session(game, plan, window, settle, warmup, sleep, log=print, min_fps=5.0):
    start_mask = game.cmd('cpuopts')['mask']
    base = start_mask & 3  # (the material cache mode the game was started with stays on in every window: it is not a factor)
    main_key = find_main_key(game, sleep)
    out = []
    try:
        if warmup > 0:
            game.cmd('cpuopts', base)
            log('warming up for %g s with every switch off' % warmup)
            sleep(warmup)
        for w in plan:
            want = w['mask'] | base
            got = game.cmd('cpuopts', want)['mask']
            if got != want:
                raise RuntimeError('the game refused mask %d (it reports %d)' % (want, got))
            sleep(settle)
            a = snapshot(game)
            sleep(window)
            b = snapshot(game)
            frames = game.cmd('frames', '%g' % max(0.5, min(60.0, window)), ok=False)
            rec = dict(w)
            m = window_metrics(a, b, frames, main_key) if isinstance(frames, dict) and 'frame_ms_avg' in frames else None
            rec['ok'] = bool(m) and m['fps'] >= min_fps
            rec['metrics'] = m or {}
            out.append(rec)
            log('window %2d/%d  mask %4d  %-6s %s' % (
                w['index'] + 1, len(plan), want, w['kind'],
                ('%.2f ms/frame (%.0f fps), %d draws/frame' % (m['frame_ms'], m['fps'], m['draws']))
                if rec['ok'] else 'SKIPPED (no frames: a menu or a pause?)'))
    finally:
        try:
            game.cmd('cpuopts', start_mask)
        except Exception:
            pass
    return out, main_key, start_mask


# ---------------------------------------------------------------- the report

def analyze(windows, factors, n_cols):
    """The numbers: per metric, per switch (effect, se, df) from the scene-adjusted fit, the noise probes, and the
    all-off against all-on windows."""
    res = {'metrics': {}, 'confirm': {}, 'windows': len(windows), 'usable': sum(1 for w in windows if w.get('ok', True))}
    probe_cols = list(range(len(factors), n_cols))
    for key, _ in METRICS:
        have = [w for w in windows if w.get('ok', True) and key in w['metrics']]
        if not have:
            continue
        try:
            adj, probes, info = fit_effects(windows, factors, key, probe_cols, True)
            raw, _, _ = fit_effects(windows, factors, key, [], False)
        except ValueError as e:
            res['metrics'][key] = {'error': str(e)}
            continue
        eff = {}
        for f in factors:
            e = adj[f[0]]
            eff[f[0]] = {'effect': e[0], 'se': e[1], 'df': e[2], 'verdict': verdict(e[0], e[1]),
                         'raw_effect': raw[f[0]][0], 'raw_se': raw[f[0]][1]}
        rms = math.sqrt(sum(p * p for p in probes) / len(probes)) if probes else None
        res['metrics'][key] = {'effects': eff, 'probes': probes, 'probe_rms': rms, 'mean': info['mean'],
                               'sigma': info['sigma'], 'n': info['n'], 'dropped': info['dropped']}
        off = trim_values([w['metrics'][key] for w in windows if w['kind'] == 'off' and w.get('ok', True) and key in w['metrics']])
        on = trim_values([w['metrics'][key] for w in windows if w['kind'] == 'on' and w.get('ok', True) and key in w['metrics']])
        d = welch(on, off)
        if d:
            res['confirm'][key] = {'on_mean': sum(on) / len(on), 'off_mean': sum(off) / len(off), 'diff': d[0],
                                   'se': d[1], 'n_on': len(on), 'n_off': len(off)}
    return res


def cell(e):
    mark = {'resolved': '*', 'probable': '+', 'within noise': ' '}[e['verdict']]
    return '%+6.2f +-%.2f%s' % (e['effect'], 2 * e['se'], mark)


def report(res, factors, header=''):
    L = []
    if header:
        L.append(header)
    fm = res['metrics'].get('frame_ms')
    L.append('windows: %d run, %d usable' % (res['windows'], res['usable']))
    if not fm or 'error' in fm:
        L.append('not enough usable windows to fit: %s' % (fm or {}).get('error', 'no frame data'))
        return '\n'.join(L)
    base_fps = 1000.0 / fm['mean'] if fm['mean'] > 0 else 0
    L.append('mean frame time over the session: %.2f ms (%.0f fps); window-to-window scatter after the fit: %.2f ms'
             % (fm['mean'], base_fps, fm['sigma']))
    if fm.get('dropped'):
        L.append('%d window(s) dropped from the frame time fit as outliers (a hitch, a loading screen).' % fm['dropped'])
    L.append('')
    L.append('Effect of switching each optimization ON, ms per frame (negative = faster), +-2 standard errors.')
    L.append('  * resolved (3+ standard errors)   + probable (2+)   blank: within noise')
    head = '%-5s %-14s' % ('bit', 'name') + ''.join('%-17s' % h for _, h in METRICS if _ in res['metrics'])
    L.append(head)
    for f in factors:
        row = '%-5d %-14s' % (f[0], f[1])
        for key, _ in METRICS:
            m = res['metrics'].get(key)
            if m is None:
                continue
            row += '%-17s' % (cell(m['effects'][f[0]]) if 'effects' in m else 'n/a')
        L.append(row)
    probe = fm.get('probe_rms')
    if probe is not None:
        L.append('noise floor: the %d unused design columns "found" %.2f ms rms on the frame time (a real effect should '
                 'stand well clear of that)' % (len(fm['probes']), probe))
    L.append('(Corrected for the scene\'s draw count and for drift in time, errors widened by %d%% for what that does not '
             'catch; what a switch itself does to the draw count, bit 512, stays in its effect.)'
             % round((DRIFT_ALLOWANCE - 1) * 100))
    L.append('')
    L.append('In words:')
    total = 0.0
    for f in factors:
        e = fm['effects'][f[0]]
        lo, hi = e['effect'] - 2 * e['se'], e['effect'] + 2 * e['se']
        dfps = -e['effect'] * base_fps * base_fps / 1000.0
        where = []
        for key, name in METRICS[1:]:
            m = res['metrics'].get(key)
            if m and 'effects' in m and m['effects'][f[0]]['verdict'] != 'within noise':
                where.append('%s thread %+.2f ms' % (name, m['effects'][f[0]]['effect']))
        if e['verdict'] == 'within noise':
            L.append('  bit %-4d %-13s no change this session could resolve: frame time %+.2f ms (interval %+.2f to %+.2f). '
                     'Smaller than about %.2f ms is invisible here.%s'
                     % (f[0], f[1], e['effect'], lo, hi, 2 * e['se'] * 1.0,
                        (' CPU: ' + ', '.join(where) + '.') if where else ''))
        else:
            total += e['effect']
            L.append('  bit %-4d %-13s %s: frame time %+.2f ms (interval %+.2f to %+.2f), about %+.1f fps.%s'
                     % (f[0], f[1], e['verdict'], e['effect'], lo, hi, dfps,
                        (' Shows in: ' + ', '.join(where) + '.') if where else ''))
    c = res['confirm'].get('frame_ms')
    if c:
        L.append('')
        L.append('All off against all on (%d and %d windows): %.2f ms against %.2f ms, %+.2f ms +-%.2f; '
                 'the sum of the resolved single effects is %+.2f ms.'
                 % (c['n_off'], c['n_on'], c['off_mean'], c['on_mean'], c['on_mean'] - c['off_mean'], 2 * c['se'], total))
        if c['off_mean'] > 0 and c['on_mean'] > 0:
            L.append('That is %.0f fps against %.0f fps at these windows\' average.' % (1000 / c['off_mean'], 1000 / c['on_mean']))
        L.append('(If the combination gives much less than the sum, the optimizations compete for the same time, or the next '
                 'thread in line became the limit. Compare the thread columns above.)')
    return '\n'.join(L)


# ---------------------------------------------------------------- command line

def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=7790, help='the control port the game was started with')
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--window', type=float, default=3.0, help='seconds measured per window (default 3)')
    ap.add_argument('--settle', type=float, default=0.5, help='seconds after a switch before measuring (default 0.5)')
    ap.add_argument('--warmup', type=float, default=15.0, help='seconds before the first window (default 15)')
    ap.add_argument('--passes', type=int, default=8, help='runs of the design; odd ones inverted (default 8)')
    ap.add_argument('--confirm', type=int, default=6, help='all-off and all-on windows each (default 6)')
    ap.add_argument('--bits', help='comma list of the bits to test (default: all of them)')
    ap.add_argument('--no-visual', action='store_true', help='leave out bit 512 (it changes the picture)')
    ap.add_argument('--seed', type=int, help='seed of the order (default: from the clock; it is printed and saved)')
    ap.add_argument('--out', help='where to save the raw windows (default build/swr_ab_<time>.json)')
    ap.add_argument('--dry-run', action='store_true', help='print the plan and stop')
    ap.add_argument('--analyze', metavar='FILE', help='fit a saved session again, connect to nothing')
    return ap.parse_args(argv)


def pick_factors(args):
    chosen = FACTORS
    if args.bits:
        want = [int(b) for b in args.bits.split(',') if b.strip()]
        known = {f[0] for f in FACTORS}
        bad = [b for b in want if b not in known]
        if bad:
            raise SystemExit('unknown bits %s; known: %s' % (bad, sorted(known)))
        chosen = [f for f in FACTORS if f[0] in want]
    if args.no_visual:
        chosen = [f for f in chosen if f[0] != 512]
    if not chosen:
        raise SystemExit('no bits to test')
    return chosen


def describe_plan(plan, factors, window, settle, warmup):
    secs = warmup + len(plan) * (window + settle)
    lines = ['%d windows of %g s (+%g s settle), %g s warmup: about %.1f minutes' % (len(plan), window, settle, warmup, secs / 60),
             'switches: ' + ', '.join('%d %s' % (f[0], f[1]) for f in factors)]
    for w in plan:
        lines.append('  %2d  %-6s mask %4d' % (w['index'] + 1, w['kind'], w['mask']))
    return '\n'.join(lines)


class Game:
    def __init__(self, host, port):
        import swctl
        self.swctl = swctl
        self.conn = swctl.connect(host, port, 5, 30)

    def cmd(self, *words, ok=True, timeout=10):
        reply = self.swctl.parse_reply(self.swctl.exchange(self.conn, self.swctl.request_line([str(w) for w in words]),
                                                           timeout), words[0])
        if ok and not reply['ok']:
            raise RuntimeError('%s: %s' % (' '.join(map(str, words)), reply['error']))
        return reply.get('data') if reply['ok'] else reply


def main(argv=None):
    args = parse_args(argv)
    if args.analyze:
        saved = json.load(open(args.analyze))
        factors = [tuple(f) for f in saved['factors']]
        res = analyze(saved['windows'], factors, saved['columns'])
        print(report(res, factors, 'session %s (seed %s)' % (args.analyze, saved.get('seed'))))
        return 0
    factors = pick_factors(args)
    seed = args.seed if args.seed is not None else int(time.time()) % 100000
    plan, ncols = make_plan(factors, args.passes, args.confirm, random.Random(seed))
    print('seed %d' % seed)
    print(describe_plan(plan, factors, args.window, args.settle, args.warmup))
    if args.dry_run:
        return 0
    game = Game(args.host, args.port)
    try:
        game.cmd('cpuopts')
    except RuntimeError as e:
        raise SystemExit('the game has no cpuopts command: start it with SAVAGEWIND_CONTROL=%d and '
                         'SAVAGEWIND_VK_CPU_BENCH=1 (%s)' % (args.port, e))
    print('\nPlay now: stand somewhere busy and move the camera now and then; do not open menus.\n')
    windows, main_key, start_mask = run_session(game, plan, args.window, args.settle, args.warmup, time.sleep)
    out = args.out or os.path.join('build' if os.path.isdir('build') else '.', 'swr_ab_%d.json' % int(time.time()))
    json.dump({'seed': seed, 'factors': factors, 'columns': ncols - 1, 'windows': windows, 'main_key': main_key,
               'start_mask': start_mask, 'window': args.window}, open(out, 'w'), indent=1)
    res = analyze(windows, factors, ncols - 1)
    print()
    print(report(res, factors, 'saved to %s (fit again with --analyze)' % out))
    print('\nThe game is back on mask %d.' % start_mask)
    return 0


if __name__ == '__main__':
    sys.exit(main())
