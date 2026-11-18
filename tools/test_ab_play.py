#!/usr/bin/env python3
"""Check tools/swr_ab.py against a simulated game whose optimizations have known sizes.

The simulated game keeps the counters the control endpoint gives (`threads`, `cpuopts`, `frames`), runs on a simulated
clock, and has a drifting scene (a slow wave and a random walk in the cost of everything and the draw count), noise
per window, and per-bit savings on the main, render and recorder threads. The frame time is that of the slowest of them,
as in the game. The session then runs through swr_ab.run_session exactly as against the real thing. Checked:

  - the design is balanced and orthogonal (every switch on in half of the windows, every pair of columns uncorrelated),
  - the least squares fit is right (a closed-form case),
  - recovered effects lie within their stated error of the truth in most runs of many seeds, the large ones are called
    resolved, and switches with no effect are called resolved only rarely,
  - a limit on a different thread hides a gain on the frame time but not on its own thread's column,
  - skipped windows (a menu) are dropped without breaking the fit; a refused mask aborts; the mask is restored.

    tools/test_ab_play.py
"""
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swr_ab as ab


class FakeGame:
    """Savings: {bit: {'main': ms, 'render': ms, 'recorder': ms}} per frame when the bit is on."""

    def __init__(self, savings, seed, base=(6.5, 9.0, 4.5), noise=0.25, menu_at=None, refuse=None):
        self.rng = random.Random(seed)
        self.savings = savings
        self.base = base
        self.noise = noise
        self.mask = 0
        self.t = 0.0
        self.presents = 0.0
        self.cpu = {'guest.101_1234': 0.0, 'guest.150_99': 0.0, 'host.render': 0.0, 'host.recorder': 0.0}
        self.draws = 0.0
        self.hist = []  # (t_end, frames, seconds)
        self.scene = 1.0
        self.hidden = 1.0
        self.slow = self.rng.uniform(0, 6)
        self.menu_at = menu_at or []
        self.refuse = refuse
        self.log = []

    def costs(self):
        m, r, c = self.base
        for bit, s in self.savings.items():
            if self.mask & bit:
                m -= s.get('main', 0)
                r -= s.get('render', 0)
                c -= s.get('recorder', 0)
        k = self.scene * self.hidden
        return m * k, r * k, c * k

    def advance(self, seconds):
        step = 0.25
        left = seconds
        while left > 1e-9:
            dt = min(step, left)
            left -= dt
            self.hidden = max(0.7, min(1.3, self.hidden + self.rng.gauss(0, 0.02) - 0.02 * (self.hidden - 1)))
            self.scene = max(0.6, 1.0 + 0.12 * math.sin(self.t / 29.0 + self.slow))  # (what the draw count shows)
            self.scene += self.rng.gauss(0, self.noise / 6)
            m, r, c = self.costs()
            m *= 1 + self.rng.gauss(0, 0.01)
            r *= 1 + self.rng.gauss(0, 0.01)
            frame = max(m, r, c) + 0.3
            paused = any(a <= self.t < b for a, b in self.menu_at)
            frames = 0 if paused else dt * 1000.0 / frame
            self.presents += frames
            self.cpu['guest.101_1234'] += frames * m
            self.cpu['guest.150_99'] += frames * 0.8
            self.cpu['host.render'] += frames * r
            self.cpu['host.recorder'] += frames * c
            self.draws += frames * 4400 * self.scene * (0.85 if self.mask & 512 else 1.0)
            self.t += dt
            self.hist.append((self.t, frames, dt, frame))

    def cmd(self, *words, ok=True, timeout=10):
        name = words[0]
        if name == 'cpuopts':
            if len(words) == 2:
                if self.refuse is not None and int(words[1]) == self.refuse:
                    return {'mask': self.mask, 'draws': int(self.draws)}
                self.mask = int(words[1])
            return {'mask': self.mask, 'draws': int(self.draws)}
        if name == 'threads':
            d = {'now_ms': self.t * 1000, 'presents': int(self.presents)}
            d.update(self.cpu)
            return d
        if name == 'frames':
            secs = float(words[1])
            use = [h for h in self.hist if h[0] > self.t - secs - 1e-9]
            fr = sum(h[1] for h in use)
            tt = sum(h[2] for h in use)
            if fr < 1:
                reply = {'ok': False, 'error': 'No frames in this window'}
                if ok:
                    raise RuntimeError('frames: No frames in this window')
                return reply
            ms = [h[3] for h in use if h[1] > 0]
            return {'frame_ms_avg': tt * 1000.0 / fr, 'frame_ms_p99': max(ms), 'fps_avg': fr / tt}
        raise AssertionError(name)


def run(savings, seed, passes=8, confirm=6, **kw):
    factors = ab.FACTORS
    plan, n = ab.make_plan(factors, passes, confirm, random.Random(seed))
    game = FakeGame(savings, seed, **kw)
    windows, main_key, start = ab.run_session(game, plan, 3.0, 0.5, 15.0, game.advance, log=lambda *a: None)
    return ab.analyze(windows, factors, n - 1), windows, game


def test_design():
    for nf in (3, 7, 8, 15):
        n = 4
        while n - 1 < nf:
            n *= 2
        cols = ab.hadamard_columns(n)
        for i in range(len(cols)):
            assert sum(cols[i]) == 0, 'every column is on in half of the rows'
            for j in range(i + 1, len(cols)):
                assert sum(a * b for a, b in zip(cols[i], cols[j])) == 0, 'columns are orthogonal'
    plan, n = ab.make_plan(ab.FACTORS, 8, 6, random.Random(5))
    assert n == 16 and len(plan) == 8 * 16 + 12
    design = [w for w in plan if w['kind'] == 'design']
    for k in range(len(ab.FACTORS)):
        assert sum(w['codes'][k] for w in design) == 0
        for j in range(len(ab.FACTORS), n - 1):
            assert sum(w['codes'][k] * w['codes'][j] for w in design) == 0
    for w in plan:
        if w['kind'] == 'design':
            assert w['mask'] == sum(f[0] for k, f in enumerate(ab.FACTORS) if w['codes'][k] > 0)
    assert sorted(w['index'] for w in plan) == list(range(len(plan)))
    assert sum(1 for w in plan if w['kind'] == 'off') == 6 and sum(1 for w in plan if w['kind'] == 'on') == 6
    for b in range(8):  # (each run of the design is whole, and balanced on its own)
        blk = [w for w in design if w['block'] == b]
        assert len(blk) == 16 and all(sum(w['codes'][k] for w in blk) == 0 for k in range(15))
    # the same seed, the same order
    again, _ = ab.make_plan(ab.FACTORS, 8, 6, random.Random(5))
    assert [w['mask'] for w in again] == [w['mask'] for w in plan]


def test_lstsq():
    rng = random.Random(1)
    X = [[1.0, rng.random(), rng.random()] for _ in range(40)]
    true = [2.0, -3.0, 0.5]
    y = [sum(a * b for a, b in zip(r, true)) for r in X]
    beta, inv, rss = ab.solve_normal(X, y)
    assert all(abs(a - b) < 1e-9 for a, b in zip(beta, true)) and rss < 1e-15
    # standard error of a mean: the one-column case
    y2 = [rng.gauss(5, 2) for _ in range(200)]
    b, inv, rss = ab.solve_normal([[1.0] for _ in y2], y2)
    se = math.sqrt(rss / (len(y2) - 1) * inv[0][0])
    sd = math.sqrt(sum((v - sum(y2) / len(y2)) ** 2 for v in y2) / (len(y2) - 1))
    assert abs(se - sd / math.sqrt(len(y2))) < 1e-9
    try:
        ab.solve_normal([[1.0, 2.0], [2.0, 4.0], [3.0, 6.0]], [1, 2, 3])
        raise AssertionError('dependent regressors accepted')
    except ValueError:
        pass


def test_recovery():
    # the render thread is the limit (9.5 vs main 5.5): its savings show in the frame time one for one. The main thread's
    # savings show in its own column only. The recorder has headroom.
    savings = {
        16: {'render': 0.50}, 64: {'render': 0.30}, 128: {'render': 0.70},
        512: {'render': 0.90, 'recorder': 0.2},
        1024: {'main': 0.15}, 2048: {'main': 0.0}, 32: {},
    }
    truth_frame = {16: -0.50, 64: -0.30, 128: -0.70, 512: -0.90, 1024: 0.0, 2048: 0.0, 32: 0.0}
    runs, inside, flagged_zero, zero_n, big_called, big_n = 0, 0, 0, 0, 0, 0
    sigma_ok = 0
    for seed in range(24):
        res, windows, game = run(savings, seed, base=(5.5, 9.5, 4.5))
        fm = res['metrics']['frame_ms']
        runs += 1
        for bit, t in truth_frame.items():
            e = fm['effects'][bit]
            # the scene factor scales everything (mean 1.0), so the truth stays near the stated saving
            inside += abs(e['effect'] - t) <= 3 * e['se'] + 0.03
            if t == 0.0:
                zero_n += 1
                flagged_zero += e['verdict'] == 'resolved'
            elif abs(t) >= 0.5:
                big_n += 1
                big_called += e['verdict'] == 'resolved'
        # the main thread's own column sees the dispatch saving that the frame time cannot
        mm = res['metrics']['main_ms']['effects'][1024]
        sigma_ok += abs(mm['effect'] + 0.15) <= 3 * mm['se'] + 0.03
        c = res['confirm']['frame_ms']
        assert 4 <= c['n_on'] <= 6 and 4 <= c['n_off'] <= 6
    total = runs * len(truth_frame)
    assert inside >= 0.95 * total, 'only %d of %d recovered effects within 3 standard errors' % (inside, total)
    assert flagged_zero <= 0.03 * zero_n + 1, '%d of %d zero effects called resolved' % (flagged_zero, zero_n)
    assert big_called >= 0.75 * big_n, 'only %d of %d large effects called resolved' % (big_called, big_n)
    assert sigma_ok >= 0.9 * runs, 'the main thread column missed the dispatch gain in %d runs' % (runs - sigma_ok)
    print('recovery: %d/%d within 3 se, %d/%d zero effects wrongly resolved, %d/%d large effects resolved' % (
        inside, total, flagged_zero, zero_n, big_called, big_n))


def test_noise_floor_and_report():
    savings = {128: {'render': 0.7}}
    res, windows, game = run(savings, 3)
    fm = res['metrics']['frame_ms']
    assert len(fm['probes']) == 15 - len(ab.FACTORS)
    assert fm['probe_rms'] < 0.35, fm['probe_rms']  # the probes found nothing large
    text = ab.report(res, ab.FACTORS, 'test')
    assert 'bit 128' in text and 'resolved' in text and 'noise floor' in text
    assert '\u2014' not in text, 'no em dashes in the output'
    assert game.mask == 0, 'the mask is back where it started'


def test_skipped_windows():
    savings = {128: {'render': 0.7}, 16: {'render': 0.5}}
    res, windows, game = run(savings, 4, base=(5.5, 9.5, 4.5), menu_at=[(200, 230), (400, 420)])
    skipped = [w for w in windows if not w['ok']]
    assert 8 <= len(skipped) <= 18, len(skipped)
    assert res['usable'] == len(windows) - len(skipped)
    fm = res['metrics']['frame_ms']
    assert fm['dropped'] >= 1, 'the windows that straddle a pause (a long frame in them) are dropped as outliers'
    e = fm['effects'][128]
    assert e['verdict'] != 'within noise' and abs(e['effect'] + 0.7) <= 3 * e['se'] + 0.03, e


def test_refused_mask():
    plan, n = ab.make_plan(ab.FACTORS, 4, 0, random.Random(1))
    start_mode = 777 & 3  # (the material cache mode the game starts with is kept in every window)
    game = FakeGame({}, 1, refuse=plan[2]['mask'] | start_mode)
    start = game.mask = 777
    try:
        ab.run_session(game, plan, 3.0, 0.5, 0.0, game.advance, log=lambda *a: None)
        raise AssertionError('a refused mask went unnoticed')
    except RuntimeError as e:
        assert 'refused' in str(e)
    assert game.mask == start, 'the mask is restored after an abort'


def test_limited_by_another_thread():
    # render saves 0.7 ms but the main thread is the limit (13.0): the frame time cannot move, the render column does
    res, windows, game = run({128: {'render': 0.7}}, 7, base=(13.0, 9.0, 4.5))
    fm = res['metrics']['frame_ms']['effects'][128]
    rm = res['metrics']['render_ms']['effects'][128]
    assert abs(fm['effect']) < 3 * fm['se'] + 0.05, fm
    assert rm['verdict'] == 'resolved' and abs(rm['effect'] + 0.7) < 0.2, rm


def test_over_the_wire():
    # the same session through swr_ab.Game and swctl against a socket that speaks the control protocol
    import json
    import socket
    import threading
    fake = FakeGame({128: {'render': 0.7}}, 11, base=(5.5, 9.5, 4.5))
    srv = socket.socket()
    srv.bind(('127.0.0.1', 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    seen = []

    def serve():
        conn, _ = srv.accept()
        buf = b''
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                return
            buf += chunk
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                words = line.decode().split()
                seen.append(words)
                try:
                    reply = {'ok': True, 'data': fake.cmd(*words)}
                except Exception as e:
                    reply = {'ok': False, 'error': str(e)}
                conn.sendall(json.dumps(reply).encode() + b'\n')

    threading.Thread(target=serve, daemon=True).start()
    game = ab.Game('127.0.0.1', port)
    plan, n = ab.make_plan(ab.FACTORS, 2, 1, random.Random(2))
    windows, main_key, start = ab.run_session(game, plan, 3.0, 0.5, 5.0, fake.advance, log=lambda *a: None)
    assert main_key == 'guest.101_1234' and start == 0
    assert all(w['ok'] for w in windows) and len(windows) == len(plan)
    assert ['frames', '3'] in seen and fake.mask == 0
    res = ab.analyze(windows, ab.FACTORS, n - 1)
    assert 'frame_ms' in res['metrics'] and 'effects' in res['metrics']['frame_ms']
    srv.close()


def test_cli_plan():
    assert ab.main(['--dry-run', '--seed', '3']) == 0
    assert ab.main(['--dry-run', '--seed', '3', '--bits', '16,128', '--no-visual', '--confirm', '1']) == 0
    args = ab.parse_args(['--bits', '16,128', '--no-visual'])
    assert [f[0] for f in ab.pick_factors(args)] == [16, 128]


if __name__ == '__main__':
    import io
    import contextlib
    for name, fn in sorted(globals().items()):
        if name.startswith('test_') and callable(fn):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf) if name == 'test_cli_plan' else contextlib.nullcontext():
                fn()
            print('ok', name)
    print('swr_ab: all checks passed')
