"""Benchmark checks with a local mock endpoint, without game files."""
import contextlib
import io
import json
from pathlib import Path
import socket
import tempfile
import threading
import unittest
from unittest import mock

from tools import swbench


class Server:
    def __init__(self, replies):
        self.replies = replies
        self.requests = []
        self.error = None
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.port = self.sock.getsockname()[1]
        self.sock.listen(1)
        self.sock.settimeout(3)
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def serve(self):
        try:
            conn, _ = self.sock.accept()
            with conn, conn.makefile('rb') as stream:
                conn.settimeout(3)
                for reply in self.replies:
                    line = stream.readline()
                    if not line:
                        break
                    self.requests.append(line.decode().strip())
                    conn.sendall(reply if isinstance(reply, bytes) else (json.dumps(reply) + '\n').encode())
        except OSError as exc:
            self.error = exc

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.thread.join(4)
        self.sock.close()
        if self.thread.is_alive():
            raise AssertionError('server did not stop')
        if self.error and exc[0] is None:
            raise self.error


def ok(**data):
    return {'ok': True, 'data': data}


def error(text):
    return {'ok': False, 'error': text}


class BenchTests(unittest.TestCase):
    def invoke(self, args):
        out, err = io.StringIO(), io.StringIO()
        with tempfile.TemporaryDirectory() as folder, contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            if args[0] == 'run' and '--out' not in args:
                args = args + ['--out', str(Path(folder) / 'result.json')]
            code = swbench.main(args)
        return code, out.getvalue(), err.getvalue()

    def run_server(self, replies, *extra):
        with Server(replies) as server:
            result = self.invoke(['run', '--port', str(server.port), '--warmup', '0',
                                  '--duration', '0.01', *extra])
        return result, server.requests

    def test_full_run(self):
        with tempfile.TemporaryDirectory() as folder:
            path = str(Path(folder) / 'result.json')
            replies = [ok(hour=18), ok(), ok(), ok(frame=100, fps=60, hour=18),
                       ok(frame=160, fps=59, hour=18), ok(avg_ms=16, p99_ms=20), ok(path='a b.png')]
            result, requests = self.run_server(replies, '--hour', '18', '--set', 'aa=off',
                                              'name=a b', '--shot', 'a b.png', '--label', 'test', '--out', path)
            self.assertEqual(result[0], 0, result)
            self.assertEqual(requests[:4], ['hour 18.0', 'set aa off', 'set name "a b"', 'state'])
            self.assertEqual(requests[-2:], ['frames 0.01', 'shot "a b.png"'])
            data = json.loads(Path(path).read_text())
            self.assertEqual(data['label'], 'test')
            self.assertEqual(data['hour'], 18)
            self.assertEqual(data['summary']['fps']['avg'], 59)
            self.assertGreater(data['samples'][0]['frames_per_second'], 0)
            self.assertIn('frame_times.avg_ms: 16', result[1])

    def test_optional_frames(self):
        result, requests = self.run_server([ok(frame=1, fps=60, hour=12),
                                           ok(frame=2, fps=60, hour=12), error("unknown command 'frames'; try help")])
        self.assertEqual(result[0], 0, result)
        self.assertIn('frames: unavailable', result[1])
        self.assertEqual(requests, ['state', 'state', 'frames 0.01'])

    def test_unknown_set_and_command_errors(self):
        for message in ("unknown command 'set'; try help", 'set: bad value'):
            result, requests = self.run_server([error(message)], '--set', 'aa=off')
            self.assertEqual(result[0], 1)
            self.assertEqual(requests, ['set aa off'])
            self.assertIn(message, result[2])
            if 'unknown' in message:
                self.assertIn('omit --set', result[2])

    def test_frames_real_error_is_not_ignored(self):
        result, _ = self.run_server([ok(frame=1, fps=1, hour=1), ok(frame=2, fps=1, hour=1),
                                     error('frames: invalid window')])
        self.assertEqual(result[0], 1)

    def test_invalid_state_and_wire(self):
        for reply in (ok(frame=1), ok(frame=True, fps=1, hour=1), b'not json\n',
                      b'{"ok":true,"data":{}}'):
            result, _ = self.run_server([reply])
            self.assertEqual(result[0], 2, result)
            self.assertNotIn('Traceback', result[2])

    def test_connect_failure(self):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            result = self.invoke(['run', '--port', str(sock.getsockname()[1]), '--wait', '0'])
        self.assertEqual(result[0], 2)
        self.assertIn('SAVAGEWIND_CONTROL=', result[2])

    def test_wait_uses_client(self):
        with mock.patch.object(swbench.swctl, 'connect', side_effect=OSError('refused')) as connect:
            self.assertEqual(self.invoke(['run', '--wait', '7'])[0], 2)
        self.assertEqual(connect.call_args.args[-1], 7)

    def test_one_second_schedule(self):
        replies = [ok(frame=i * 60, fps=60, hour=12) for i in range(4)] + [ok(avg_ms=16)]
        clock = [0.0]
        def sleep(seconds):
            clock[0] += seconds
        with mock.patch.object(swbench.time, 'monotonic', side_effect=lambda: clock[0]), \
                mock.patch.object(swbench.time, 'sleep', side_effect=sleep):
            result, requests = self.run_server(replies, '--duration', '2.5')
        self.assertEqual(result[0], 0, result)
        self.assertEqual(requests, ['state'] * 4 + ['frames 2.5'])
        self.assertEqual(clock[0], 2.5)
        self.assertIn('sample[2].frames_per_second: 120.000', result[1])

    def test_compare_noise_hours_and_percent(self):
        with tempfile.TemporaryDirectory() as folder:
            paths = [str(Path(folder) / name) for name in ('a.json', 'b.json')]
            for path, avg, hour in zip(paths, (60, 61), (12, 18)):
                summary = {key: {'min': 58, 'avg': avg, 'max': 63}
                           for key in ('fps', 'frames_per_second')}
                summary['frame_times'] = {'avg_ms': 16}
                Path(path).write_text(json.dumps({'hour': hour, 'summary': summary}))
            code, out, err = self.invoke(['compare', *paths])
            self.assertEqual(code, 0, err)
            self.assertIn('+1.67%', out)
            self.assertIn('different hours', out)
            self.assertIn('within sample noise', out)
            self.assertIn('frame_times.avg_ms:', out)
            Path(paths[0]).write_text('[]')
            self.assertEqual(self.invoke(['compare', *paths])[0], 2)

    def test_bad_options(self):
        for args in (['--duration', '0'], ['--hour', '25'], ['--warmup', 'nan'],
                     ['--set', 'bad'], ['--set', '=bad'], ['--set', 'a=x\ny']):
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as exc:
                    swbench.main(['run', *args])
                self.assertEqual(exc.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
