#!/usr/bin/env python3
"""Control client checks with a local mock server; no game files needed.

Usage: python3 -m unittest tools/test_swctl.py
"""
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time
import unittest
from unittest import mock

SCRIPT = Path(__file__).with_name('swctl.py')


class Server:
    """Serve one canned reply and record the exact request, using an OS-selected port."""

    def __init__(self, reply=b'{"ok":true,"data":{}}\n', delay=0, pause=0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind(('127.0.0.1', 0))
        self.port = self.sock.getsockname()[1]
        self.chunks = reply if isinstance(reply, list) else [reply]
        self.delay = delay
        self.pause = pause
        self.request = None
        self.error = None
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def __enter__(self):
        if not self.delay:
            self.sock.listen(1)
        self.thread.start()
        return self

    def serve(self):
        try:
            if self.delay:
                if self.stop.wait(self.delay):
                    return
                self.sock.listen(1)
            self.sock.settimeout(0.1)
            while not self.stop.is_set():
                try:
                    conn, _ = self.sock.accept()
                    break
                except socket.timeout:
                    continue
            else:
                return
            with conn:
                conn.settimeout(5)
                with conn.makefile('rb') as stream:
                    self.request = stream.readline()
                if self.stop.wait(self.pause):
                    return
                for i, chunk in enumerate(self.chunks):
                    if i and self.stop.wait(0.03):
                        return
                    conn.sendall(chunk)
        except (OSError, ValueError) as exc:
            self.error = exc
        finally:
            self.sock.close()

    def __exit__(self, *exc):
        self.stop.set()
        self.thread.join(6)
        self.sock.close()
        if self.thread.is_alive():
            raise AssertionError('mock server did not stop')
        if self.error and exc[0] is None:
            raise self.error


class SwctlTests(unittest.TestCase):
    def run_client(self, *args, env=None):
        child_env = os.environ.copy()
        child_env.pop('SAVAGEWIND_CONTROL', None)
        child_env['PYTHONIOENCODING'] = 'utf-8'
        if env:
            child_env.update(env)
        return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)],
                              capture_output=True, encoding='utf-8', env=child_env, timeout=8)

    def talk(self, reply, *args):
        with Server(reply) as server:
            result = self.run_client('--port', server.port, *args)
        return result, server.request

    def assert_client_error(self, result, port=None):
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(result.stdout, '')
        self.assertIn('SAVAGEWIND_CONTROL=' + (str(port) if port else ''), result.stderr)
        self.assertNotIn('Traceback', result.stderr)

    def test_quoting(self):
        result, request = self.talk(b'{"ok":true,"data":{}}\n',
                                    'shot', 'out 1.png', 'say "hi"', r'C:\me\out.png', '', 'plain')
        self.assertEqual(request, b'shot "out 1.png" "say \\"hi\\"" "C:\\\\me\\\\out.png" "" plain\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, '')

    def test_unicode_and_options_after_command(self):
        result, request = self.talk(b'{"ok":true,"data":{}}\n', 'echo', 'Hyrule é', '--json', '-1')
        self.assertEqual(request, 'echo "Hyrule é" --json -1\n'.encode('utf-8'))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_plain_ok(self):
        result, request = self.talk(b'{"ok":true,"data":{"frame":42,"fps":59.75,"hour":18.0,"paused":false,"pong":true}}\n', 'state')
        self.assertEqual(request, b'state\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'frame: 42\nfps: 59.75\nhour: 18\npaused: false\npong: true\n')
        self.assertEqual(result.stderr, '')

    def test_nested_data_and_object_lists(self):
        reply = {'ok': True, 'data': {'player': {'position': {'x': 1.25, 'y': 2}, 'name': 'Link'},
                                    'actors': [{'name': 'Link', 'hp': 6}, {'name': 'Zelda', 'hp': 8}],
                                    'flags': [True, False], 'nothing': None, 'empty': {}}}
        result, _ = self.talk((json.dumps(reply) + '\n').encode(), 'state')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'player.position.x: 1.25\nplayer.position.y: 2\nplayer.name: Link\n'
                         'actors[0]: name: Link, hp: 6\nactors[1]: name: Zelda, hp: 8\n'
                         'flags: [true, false]\nnothing: null\nempty: {}\n')

    def test_error(self):
        result, _ = self.talk(b'{"ok":false,"error":"time: bad value"}\n', 'time', 'oops')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, '')
        self.assertEqual(result.stderr, 'error: time: bad value\n')

    def test_json_passthrough(self):
        reply = b' { "ok" : true, "data": {"hour": 18.000, "name": "\\u00e9"} }  \n'
        result, _ = self.talk(reply, '--json', 'state')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.encode(), reply)
        self.assertEqual(result.stderr, '')

    def test_json_error_passthrough(self):
        reply = b'{"ok":false, "error":"no such command"}\n'
        result, _ = self.talk(reply, '--json', 'unknown')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout.encode(), reply)
        self.assertEqual(result.stderr, '')

    def test_refused(self):
        with socket.socket() as blocker:
            blocker.bind(('127.0.0.1', 0))
            port = blocker.getsockname()[1]
            result = self.run_client('--port', port, 'ping')
        self.assert_client_error(result, port)
        self.assertIn('connect', result.stderr.lower())

    def test_closed_without_reply(self):
        result, _ = self.talk(b'', 'ping')
        self.assert_client_error(result)
        self.assertIn('closed', result.stderr)

    def test_unterminated_reply(self):
        result, _ = self.talk(b'{"ok":true,"data":{}}', 'ping')
        self.assert_client_error(result)

    def test_exit_reply_before_close(self):
        reply = b'{"ok":true,"data":{}}\n'
        result, request = self.talk(reply, '--json', 'exit')
        self.assertEqual(request, b'exit\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.encode(), reply)

    def test_reply_in_two_chunks(self):
        result, _ = self.talk([b'{"ok":true,"data":{"name":"\xc3', b'\xa9","hour":18.0}}\n'], 'state')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'name: é\nhour: 18\n')

    def test_reads_only_one_reply(self):
        result, _ = self.talk(b'{"ok":true,"data":{"pong":true}}\nnot JSON\n', 'ping')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'pong: true\n')

    def test_invalid_replies(self):
        for reply in (b'not JSON\n', b'[]\n', b'{"ok":1,"data":{}}\n', b'{"data":{}}\n',
                      b'{"ok":true,"data":[]}\n', b'{"ok":false}\n', b'{"ok":false,"error":3}\n',
                      b'{"ok":true,"data":{"x":NaN}}\n', b'\xff\n'):
            with self.subTest(reply=reply):
                result, _ = self.talk(reply, '--json', 'state')
                self.assert_client_error(result)
                self.assertIn('invalid reply', result.stderr)

    def test_timeout(self):
        with Server(pause=0.5) as server:
            result = self.run_client('--port', server.port, '--timeout', 0.05, 'ping')
        self.assert_client_error(result, server.port)
        self.assertIn('timed out', result.stderr)

    def test_wait_for_startup(self):
        with Server(b'{"ok":true,"data":{"pong":true}}\n', delay=0.3) as server:
            result = self.run_client('--host', '127.0.0.1', '--port', server.port, '--wait', 2, 'ping')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(server.request, b'ping\n')
        self.assertEqual(result.stdout, 'pong: true\n')

    def test_wait_retries_connection_failure(self):
        from tools import swctl
        real_connect = socket.create_connection
        attempts = []

        def refuse_once(address, timeout):
            attempts.append((address, timeout))
            if len(attempts) == 1:
                raise ConnectionRefusedError('game is still starting')
            return real_connect(address, timeout)

        with Server() as server:
            with mock.patch.object(swctl.socket, 'create_connection', side_effect=refuse_once):
                with mock.patch('sys.stdout', new_callable=io.StringIO):
                    code = swctl.main(['--port', str(server.port), '--timeout', '5', '--wait', '1', 'ping'])
        self.assertEqual(code, 0)
        self.assertEqual(len(attempts), 2)
        self.assertTrue(all(0 < timeout <= 1 for _, timeout in attempts))
        self.assertEqual(server.request, b'ping\n')

    def test_wait_does_not_retry_a_sent_command(self):
        from tools import swctl
        with Server(pause=0.5) as server:
            with mock.patch.object(swctl.socket, 'create_connection', wraps=socket.create_connection) as connect:
                with mock.patch('sys.stderr', new_callable=io.StringIO) as err:
                    code = swctl.main(['--port', str(server.port), '--timeout', '0.05', '--wait', '2', 'exit'])
        self.assertEqual(code, 2)
        self.assertEqual(connect.call_count, 1)
        self.assertEqual(server.request, b'exit\n')
        self.assertIn('timed out', err.getvalue())

    def test_wait_expires(self):
        with socket.socket() as blocker:
            blocker.bind(('127.0.0.1', 0))
            start = time.monotonic()
            result = self.run_client('--port', blocker.getsockname()[1], '--wait', 0.15, 'ping')
            elapsed = time.monotonic() - start
        self.assert_client_error(result)
        self.assertGreaterEqual(elapsed, 0.15)
        self.assertLess(elapsed, 3)

    def test_time_forms(self):
        for args, held in (((), True), (('freeze',), True), (('unfreeze',), False),
                           (('18',), False), (('18',), True), (('set', '18'), False), (('set', '18'), True),
                           (('free',), False)):
            with self.subTest(args=args):
                reply = ('{"ok":true,"data":{"hour":18.0,"hour_held":'
                         + str(held).lower() + '}}\n').encode()
                result, request = self.talk(reply, 'time', *args)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(request, (' '.join(('time',) + args) + '\n').encode())
                self.assertEqual(result.stdout, 'hour: 18\nhour_held: ' + str(held).lower() + '\n')

    def test_help_table(self):
        reply = b'{"ok":true,"data":{"commands":[{"name":"ping","args":"","help":"check the game"},{"name":"time","args":"[freeze | unfreeze | <0-24>]","help":"set time"}]}}\n'
        result, _ = self.talk(reply, 'help')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'ping                                check the game\ntime  [freeze | unfreeze | <0-24>]  set time\n')

    def test_invalid_help_table(self):
        for commands in (None, {}, [None], [{'name': 'ping', 'args': '', 'help': 3}]):
            with self.subTest(commands=commands):
                reply = json.dumps({'ok': True, 'data': {'commands': commands}}).encode() + b'\n'
                result, _ = self.talk(reply, 'help')
                self.assert_client_error(result)
                self.assertIn('invalid reply', result.stderr)

    def test_environment_port(self):
        with Server(b'{"ok":true,"data":{"pong":true}}\n') as server:
            result = self.run_client('ping', env={'SAVAGEWIND_CONTROL': str(server.port)})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(server.request, b'ping\n')

    def test_explicit_port_overrides_environment(self):
        with Server() as server:
            result = self.run_client('--port', server.port, 'ping', env={'SAVAGEWIND_CONTROL': '1'})
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_default_and_nonnumeric_environment_port(self):
        from tools import swctl
        for value in (None, '', 'disabled'):
            with self.subTest(value=value), mock.patch.dict(os.environ, {}, clear=True):
                if value is not None:
                    os.environ['SAVAGEWIND_CONTROL'] = value
                with mock.patch.object(swctl.socket, 'create_connection', side_effect=OSError('refused')) as connect:
                    with mock.patch('sys.stderr'):
                        self.assertEqual(swctl.main(['ping']), 2)
                self.assertEqual(connect.call_args.args[0], ('127.0.0.1', 7777))

    def test_no_command(self):
        result = self.run_client()
        self.assertEqual(result.returncode, 2)
        self.assertIn('usage:', result.stderr)
        self.assertIn('help', result.stderr)

    def test_bad_options(self):
        for option, value in (('--port', '0'), ('--port', '65536'), ('--timeout', '0'),
                              ('--timeout', 'nan'), ('--wait', '-1'), ('--wait', 'inf')):
            with self.subTest(option=option, value=value):
                result = self.run_client(option, value, 'ping')
                self.assertEqual(result.returncode, 2)
                self.assertNotIn('Traceback', result.stderr)

    def test_newline_argument_rejected(self):
        result = self.run_client('echo', 'one\ntwo')
        self.assertEqual(result.returncode, 2)
        self.assertIn('newline', result.stderr)
        self.assertNotIn('Traceback', result.stderr)


if __name__ == '__main__':
    unittest.main()
