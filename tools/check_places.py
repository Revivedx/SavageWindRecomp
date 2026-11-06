#!/usr/bin/env python3
"""Live check for `goto <place>` against a running game (developer mode: SAVAGEWIND_CONTROL=7777).

Goes to a spread of places in the order given (the default starts on the Great Plateau and works outwards), waits, reads `pos` and prints where Link ended up
compared with the table. It stops when Link falls out of the world. It does not decide for you whether a place is right:
look at the game too (inside a wall, on the tower terminal, under the floor).

  python3 tools/check_places.py                 the default spread, 6 seconds each
  python3 tools/check_places.py --settle 10     wait longer before reading pos
  python3 tools/check_places.py "shrine 5" "korok H23" "stable wetland"    your own list

Start with Link standing on the Great Plateau in the world (load a save first), so the first jump is short. Needs only Python 3.8 or later.
"""
import argparse
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swctl  # noqa: E402

TABLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'runtime', 'control_places_data.inc')
ROW = re.compile(r'PLACE\(PLACE_(\w+), (\d+), "([^"]*)", "((?:[^"\\]|\\.)*)", "[^"]*", (-?[\d.]+)f, (-?[\d.]+)f, (-?[\d.]+)f, (true|false)\)')


def load_targets():
    """name -> where goto sends Link (the table position plus the lift goto adds: 1 for places on a start position of the game, else 2 for Koroks and 3)."""
    targets = {}
    with open(TABLE, encoding='utf-8') as f:
        for line in f:
            m = ROW.match(line)
            if m:
                lift = 1.0 if m.group(8) == 'true' else 2.0 if m.group(1) == 'KOROK' else 3.0
                targets[m.group(4).replace('\\"', '"')] = (float(m.group(5)), float(m.group(6)) + lift, float(m.group(7)))
    return targets


# A spread over the map: towers, a high and a low shrine, a village, stables, a landmark, Koroks in several squares.
DEFAULT = ['tower 7', 'shrine 5', 'shrine 100', 'shrine 115', 'tower 2', 'hateno', 'kakariko', 'stable wetland', 'stable rito',
           'hyrule castle', 'korok 1', 'korok 450', 'korok 900', 'korok H23', 'nearest shrine', 'nearest korok']


class Client:
    def __init__(self, host, port, timeout):
        self.host, self.port, self.timeout = host, port, timeout

    def call(self, *words):
        conn = swctl.connect(self.host, self.port, self.timeout, 5)
        try:
            reply = swctl.parse_reply(swctl.exchange(conn, swctl.request_line(list(words)), self.timeout), words[0])
        finally:
            conn.close()
        return reply

    def pos(self):
        d = self.call('pos')['data']
        return d['x'], d['y'], d['z']


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('places', nargs='*', help='what to give goto, one string each (default: a spread over the map)')
    ap.add_argument('--port', type=int, default=int(os.environ.get('SAVAGEWIND_CONTROL', '') or 7777))
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--timeout', type=float, default=15)
    ap.add_argument('--settle', type=float, default=6, help='seconds to wait after each goto (default 6)')
    args = ap.parse_args()
    c = Client(args.host, args.port, args.timeout)

    try:
        start = c.pos()
    except (OSError, ValueError, KeyError) as exc:
        print('No answer from the game on %s:%d (%s). Start it with SAVAGEWIND_CONTROL=%d and load a save.' % (args.host, args.port, exc, args.port))
        return 2
    print('start: %.1f %.1f %.1f' % start)
    queries = args.places or DEFAULT
    targets = load_targets()
    print('%-18s %-26s %9s %9s %9s  %s' % ('goto', 'place', 'dx,dz', 'dy', 'drift', 'verdict'))
    bad = 0
    for q in queries:
        try:
            reply = c.call('goto', *q.split())
        except (OSError, ValueError) as exc:
            print('%-18s connection problem: %s' % (q, exc))
            return 2
        if not reply['ok']:
            print('%-18s refused: %s' % (q, reply.get('error')))
            bad += 1
            continue
        data = reply['data']
        want = targets.get(data.get('place'))
        time.sleep(1.5)
        early = c.pos()
        time.sleep(max(args.settle - 1.5, 0))
        got = c.pos()
        if want is None:
            print('%-18s no table entry for "%s"' % (q, data.get('place')))
            bad += 1
            continue
        dxz = math.hypot(got[0] - want[0], got[2] - want[2])
        dy = got[1] - want[1]
        drift = math.hypot(got[0] - early[0], got[2] - early[2]) + abs(got[1] - early[1])
        verdict = 'ok'
        if dxz > 6:
            verdict = 'DID NOT ARRIVE'
        elif dy < -15:
            verdict = 'FELL (%.0f below the target)' % -dy
        elif drift > 8:
            verdict = 'STILL MOVING (%.0f since 1.5 s)' % drift
        elif dy > 8:
            verdict = 'FLOATING? (%.0f above the target)' % dy
        if verdict != 'ok':
            bad += 1
        print('%-18s %-26s %4.0f,%-4.0f %+8.1f %8.1f  %s' % (q, str(data.get('place', ''))[:26], got[0] - want[0], got[2] - want[2], dy, drift, verdict))
        if verdict.startswith('FELL') or got[1] < -50:
            print('Link left the world; stopping. Reload the save and run again, starting after this place.')
            return 1
    print('done: %d of %d need a look' % (bad, len(queries)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
