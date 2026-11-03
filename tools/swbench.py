#!/usr/bin/env python3
"""Repeatable control endpoint benchmarks, using only Python 3.8's standard library."""
import argparse
import datetime
import json
import math
import os
import sys
import time

try:
    from . import swctl
except ImportError:
    import swctl


class CommandError(Exception):
    pass


def command(conn, words, optional=False):
    reply = swctl.parse_reply(swctl.exchange(conn, swctl.request_line(words), 5), words[0])
    if not reply['ok']:
        if optional and 'unknown command' in reply['error'].lower():
            return None
        hint = '; use a game build with the set command, or omit --set' if (
            words[0] == 'set' and 'unknown command' in reply['error'].lower()) else ''
        raise CommandError(reply['error'] + hint)
    return reply['data']


def number(value):
    return type(value) in (int, float) and math.isfinite(value)


def state(conn):
    data = command(conn, ['state'])
    if not (type(data.get('frame')) is int and data['frame'] >= 0 and
            number(data.get('fps')) and data['fps'] >= 0 and number(data.get('hour'))):
        raise ValueError('state needs frame, fps and hour; load a world and retry')
    return data


def stats(values):
    return {'min': min(values), 'avg': sum(values) / len(values), 'max': max(values)}


def run(args):
    # Retry only the initial connection, never a command that might have taken effect.
    with swctl.connect('127.0.0.1', args.port, 5, args.wait) as conn:
        if args.hour is not None:
            command(conn, ['hour', str(args.hour)])
        for setting in args.settings:
            command(conn, ['set', *setting.split('=', 1)])
        time.sleep(args.warmup)
        baseline = state(conn)
        start = previous_time = time.monotonic()
        previous = baseline
        samples = []
        # Whole one-second intervals plus a final shorter interval for fractional durations.
        deadline = 0.0
        while deadline < args.duration:
            deadline = min(deadline + 1, args.duration)
            time.sleep(max(0, start + deadline - time.monotonic()))
            data = state(conn)
            now = time.monotonic()
            elapsed = now - previous_time
            if data['frame'] < previous['frame'] or elapsed <= 0:
                raise ValueError('frame counter or clock went backwards; rerun the benchmark')
            samples.append(dict(data, elapsed_s=now - start, interval_s=elapsed,
                                frames_per_second=(data['frame'] - previous['frame']) / elapsed))
            previous, previous_time = data, now
        frames = command(conn, ['frames', str(args.duration)], optional=True)
        shot = command(conn, ['shot', args.shot]) if args.shot else None
    summary = {'fps': stats([s['fps'] for s in samples]),
               'frames_per_second': stats([s['frames_per_second'] for s in samples]),
               'frame_times': frames}
    result = {'label': args.label, 'time': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'args': vars(args), 'hour': baseline['hour'], 'baseline': baseline,
              'samples': samples, 'summary': summary, 'shot': shot}
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as stream:
            json.dump(result, stream, indent=2, allow_nan=False)
            stream.write('\n')
    swctl.print_data({'label': args.label, 'hour': result['hour'], **summary}, 'run')
    for i, sample in enumerate(samples):
        print('sample[{}].frames_per_second: {:.3f}'.format(i, sample['frames_per_second']))
    if frames is None:
        print('frames: unavailable (unknown command; use a game build with frames)')
    return 0


def load(path):
    with open(path, encoding='utf-8') as stream:
        result = json.load(stream, parse_constant=swctl.invalid_constant)
    try:
        for group in ('fps', 'frames_per_second'):
            values = result['summary'][group]
            if not all(number(values[k]) and values[k] >= 0 for k in ('min', 'avg', 'max')):
                raise ValueError('invalid summary numbers')
            if not values['min'] <= values['avg'] <= values['max']:
                raise ValueError('invalid summary range')
        if not number(result['hour']):
            raise ValueError('invalid hour')
    except (KeyError, TypeError) as exc:
        raise ValueError('expected a swbench result: ' + path) from exc
    return result


def compare(args):
    a, b = load(args.a), load(args.b)
    print('runs: {} | {}'.format(a.get('label', args.a), b.get('label', args.b)))
    if a['hour'] != b['hour']:
        print('warning: different hours ({} | {}); lighting can change fps'.format(a['hour'], b['hour']))
    for group in ('fps', 'frames_per_second'):
        for key in ('min', 'avg', 'max'):
            x, y = a['summary'][group][key], b['summary'][group][key]
            delta = '{:+.2f}%'.format((y - x) / x * 100) if x else 'n/a (baseline zero)'
            print('{}.{}: {:.3f} | {:.3f} | {}'.format(group, key, x, y, delta))
    for key, x in swctl.fields(a['summary'].get('frame_times') or {}):
        other = dict(swctl.fields(b['summary'].get('frame_times') or {}))
        if key in other:
            try:
                xv, yv = float(x), float(other[key])
                delta = '{:+.2f}%'.format((yv - xv) / xv * 100) if xv else 'n/a (baseline zero)'
            except ValueError:
                delta = 'n/a'
            print('frame_times.{}: {} | {} | {}'.format(key, x, other[key], delta))
    x, y = (r['summary']['frames_per_second'] for r in (a, b))
    spread = max(x['max'] - x['min'], y['max'] - y['min'])
    within = abs(y['avg'] - x['avg']) <= spread
    print('noise: {} (mean difference vs largest sample range; heuristic, not a significance test)'.format(
        'within sample noise' if within else 'larger than sample noise'))
    return 0


def setting(value):
    if '=' not in value or not all(value.split('=', 1)):
        raise argparse.ArgumentTypeError('use key=value with a nonempty key and value')
    try:
        swctl.request_line(value.split('=', 1))
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    return value


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    sub = ap.add_subparsers(dest='action', required=True)
    rp = sub.add_parser('run', allow_abbrev=False)
    env = os.environ.get('SAVAGEWIND_CONTROL', '')
    rp.add_argument('--port', type=swctl.port_number, default=env if env.isascii() and env.isdigit() else '7777')
    rp.add_argument('--wait', type=swctl.seconds, default=30.0, metavar='S', help='startup wait (default: 30)')
    rp.add_argument('--hour', type=swctl.seconds)
    rp.add_argument('--set', dest='settings', action='extend', nargs='+', type=setting, default=[])
    rp.add_argument('--warmup', type=swctl.seconds, default=5.0)
    rp.add_argument('--duration', type=swctl.seconds, default=30.0)
    rp.add_argument('--shot', metavar='FILE')
    rp.add_argument('--label', default='')
    rp.add_argument('--out', default='result.json', metavar='FILE', help='JSON output (default: result.json)')
    cp = sub.add_parser('compare', allow_abbrev=False)
    cp.add_argument('a')
    cp.add_argument('b')
    args = ap.parse_args(argv)
    if args.action == 'run':
        if args.duration <= 0:
            rp.error('--duration must be greater than zero')
        if args.hour is not None and args.hour > 24:
            rp.error('--hour must be from 0 to 24')
    try:
        return run(args) if args.action == 'run' else compare(args)
    except CommandError as exc:
        print('error: ' + str(exc), file=sys.stderr)
        return 1
    except (OSError, ValueError, TypeError) as exc:
        hint = '; start the game with SAVAGEWIND_CONTROL={} and load a world'.format(args.port) if args.action == 'run' else '; use JSON files written by swbench run'
        print('error: ' + str(exc) + hint, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
