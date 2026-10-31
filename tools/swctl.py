#!/usr/bin/env python3
"""Talk to a running Savage Wind control endpoint (docs/CONTROL.md).

Usage: swctl.py [--json] [--port N] [--host 127.0.0.1] [--timeout S] [--wait N] command [args...]

Needs only Python 3.8 or later. Start the game with SAVAGEWIND_CONTROL=7777 to enable the endpoint.
"""
import argparse
import json
import math
import os
import socket
import sys
import time


def port_number(value):
    port = int(value)
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError('port must be between 1 and 65535')
    return port


def seconds(value):
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError('seconds must be finite and nonnegative')
    return number


def request_line(args):
    """Quote for the control protocol, which is not a shell command line."""
    words = []
    for arg in args:
        if any(c in arg for c in '\r\n\0'):
            raise ValueError('arguments cannot contain a newline or NUL')
        if not arg or any(c.isspace() or c in '"\\' for c in arg):
            arg = '"' + arg.replace('\\', '\\\\').replace('"', '\\"') + '"'
        words.append(arg)
    return (' '.join(words) + '\n').encode('utf-8')


def connect(host, port, timeout, wait):
    """Only retry before a connection is made, so a command is never sent twice."""
    deadline = time.monotonic() + wait
    while True:
        remaining = deadline - time.monotonic()
        try:
            return socket.create_connection((host, port), timeout=min(timeout, max(remaining, 0.001))
                                            if wait else timeout)
        except OSError:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise
            time.sleep(min(0.1, remaining))
            if time.monotonic() >= deadline:
                raise


def exchange(conn, request, timeout):
    """Read through the first newline, including when TCP splits a UTF-8 character."""
    deadline = time.monotonic() + timeout
    conn.settimeout(timeout)
    conn.sendall(request)
    chunks = []
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise socket.timeout('timed out waiting for a reply')
        conn.settimeout(remaining)
        chunk = conn.recv(4096)
        if not chunk:
            raise ValueError('connection closed without a complete reply line')
        before, end, _ = chunk.partition(b'\n')
        chunks.append(before)
        if end:
            return b''.join(chunks) + b'\n'


def invalid_constant(value):
    raise ValueError('non-JSON number: ' + value)


def parse_reply(line, command):
    try:
        reply = json.loads(line.decode('utf-8'), parse_constant=invalid_constant)
        if not isinstance(reply, dict) or type(reply.get('ok')) is not bool:
            raise ValueError('expected an object with a boolean "ok"')
        if reply['ok']:
            if not isinstance(reply.get('data'), dict):
                raise ValueError('expected a "data" object')
            if command == 'help':
                commands = reply['data'].get('commands')
                if not isinstance(commands, list) or any(
                        not isinstance(row, dict) or any(not isinstance(row.get(k), str)
                                                        for k in ('name', 'args', 'help'))
                        for row in commands):
                    raise ValueError('expected help commands with name, args and help strings')
        elif not isinstance(reply.get('error'), str):
            raise ValueError('expected an "error" string')
        return reply
    except (ValueError, RecursionError) as exc:
        raise ValueError('invalid reply: ' + str(exc)) from exc


def value_text(value):
    if isinstance(value, str):
        return value
    if isinstance(value, float):
        text = str(value)
        return text[:-2] if text.endswith('.0') else text
    return json.dumps(value, ensure_ascii=False)


def fields(data, prefix=''):
    for key, value in data.items():
        name = prefix + '.' + key if prefix else key
        if isinstance(value, dict) and value:
            yield from fields(value, name)
        elif isinstance(value, list) and value and all(isinstance(row, dict) for row in value):
            for i, row in enumerate(value):
                text = ', '.join(k + ': ' + v for k, v in fields(row)) if row else '{}'
                yield f'{name}[{i}]', text
        else:
            yield name, value_text(value)


def print_data(data, command):
    if command == 'help':
        rows = data['commands']
        name_width = max((len(row['name']) for row in rows), default=0)
        args_width = max((len(row['args']) for row in rows), default=0)
        for row in rows:
            print(f'{row["name"]:<{name_width}}  {row["args"]:<{args_width}}  {row["help"]}')
    else:
        for key, value in fields(data):
            print(f'{key}: {value}')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], allow_abbrev=False)
    env_port = os.environ.get('SAVAGEWIND_CONTROL', '')
    default_port = env_port if env_port.isascii() and env_port.isdigit() else '7777'
    ap.add_argument('--json', action='store_true', help='print the raw JSON reply')
    ap.add_argument('--port', type=port_number, default=default_port,
                    help='control port (default: numeric SAVAGEWIND_CONTROL, else 7777)')
    ap.add_argument('--host', default='127.0.0.1', help='control host (default: 127.0.0.1)')
    ap.add_argument('--timeout', type=seconds, default=5.0, metavar='S',
                    help='connect and reply timeout in seconds (default: 5)')
    ap.add_argument('--wait', type=seconds, default=0.0, metavar='N',
                    help='retry connecting for up to N seconds (default: 0)')
    ap.add_argument('command', nargs=argparse.REMAINDER, help='command and its arguments; try help')
    args = ap.parse_args(argv)
    if not args.command:
        ap.print_usage(sys.stderr)
        print('try swctl.py help for the game command list', file=sys.stderr)
        return 2
    if args.timeout == 0:
        ap.error('--timeout must be greater than zero')
    try:
        request = request_line(args.command)
    except ValueError as exc:
        ap.error(str(exc))

    hint = f'set SAVAGEWIND_CONTROL={args.port} on the game before starting it'
    try:
        conn = connect(args.host, args.port, args.timeout, args.wait)
    except OSError as exc:
        print(f'error: could not connect to {args.host}:{args.port}: {exc}; {hint}', file=sys.stderr)
        return 2
    try:
        with conn:
            line = exchange(conn, request, args.timeout)
        reply = parse_reply(line, args.command[0])
    except (OSError, ValueError) as exc:
        print(f'error: {exc}; {hint}', file=sys.stderr)
        return 2

    if args.json:
        sys.stdout.buffer.write(line)
        sys.stdout.buffer.flush()
    elif reply['ok']:
        print_data(reply['data'], args.command[0])
    else:
        print('error: ' + reply['error'], file=sys.stderr)
    return 0 if reply['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
