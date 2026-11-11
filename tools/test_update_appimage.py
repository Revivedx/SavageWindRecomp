#!/usr/bin/env python3
"""An AppImage updating itself, every way it can go wrong (Linux; make test-update-appimage).

Builds the update code with a throwaway key and a test feed, then for each scenario makes a stand-in AppImage (a shell
script) in a temporary folder and serves made-up releases from a local server, with the system's real curl.
"""
import hashlib
import http.server
import json
import os
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix='update_appimage_test_')
OLD = '0.5.1-alpha.5'
NEW = '0.5.2-alpha.5'
ARCH = os.uname().machine
NAME = f'Savage_Wind-{NEW}-{ARCH}.AppImage'


def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, **kw)
    if p.returncode:
        raise SystemExit(f'{cmd[0]} failed: {p.stderr.decode(errors="replace")[-1500:]}')
    return p


def build():
    prebuilt = os.environ.get('SW_TEST_PREBUILT')  # (a machine without a compiler: the three files from another)
    if prebuilt:
        return tuple(os.path.join(prebuilt, n) for n in ('key.pem', 'other.pem', 'tool'))
    key = os.path.join(TMP, 'key.pem')
    other = os.path.join(TMP, 'other.pem')
    for k in (key, other):
        run(['openssl', 'ecparam', '-genkey', '-name', 'prime256v1', '-noout', '-out', k])
    pub = run([sys.executable, 'tools/sign_update.py', '--public', key], cwd=ROOT).stdout.decode().strip()
    header = os.path.join(TMP, 'key.h')
    with open(header, 'w') as f:
        f.write(f'#define SW_UPDATE_TEST_PUBKEY "{pub}"\n')
    exe = os.path.join(TMP, 'tool')
    run(['clang', '-O1', '-Wall', '-DSW_UPDATE_TEST', '-include', header, '-Iruntime', '-o', exe, 'tools/test_update_appimage.c',
         'runtime/update_appimage.c', 'runtime/update_core.c', 'runtime/update_crypto.c'], cwd=ROOT)
    return key, other, exe


KEY, OTHER, TOOL = build()


def script(tag, health=73):
    return (f'#!/bin/sh\n# {tag}\nif [ "$1" = --update-health ]; then exit {health}; fi\n'
            'echo "$0 $*" >> "$(dirname "$0")/started.txt"\n').encode()


def sign(data, key=KEY):
    p = os.path.join(TMP, 'to-sign')
    with open(p, 'wb') as f:
        f.write(data)
    run([sys.executable, 'tools/sign_update.py', p, key], cwd=ROOT)
    with open(p + '.sig', 'rb') as f:
        return f.read()


class Server:
    def __init__(self):
        owner = self
        self.routes = {}
        self.hits = []

        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *a):
                pass

            def do_GET(self):
                owner.hits.append((self.path, self.headers.get('Range')))
                r = owner.routes.get(self.path)
                if callable(r):
                    return r(self)
                status, body, headers = r if r else (404, b'nope', {})
                rng = self.headers.get('Range')
                if rng and status == 200 and self.path.endswith('.AppImage'):
                    start = int(rng.split('=')[1].rstrip('-'))
                    if start >= len(body):
                        self.send_response(416)
                        self.send_header('Content-Length', '0')
                        self.end_headers()
                        return
                    body, status = body[start:], 206
                self.send_response(status)
                for k, v in headers.items():
                    self.send_header(k, v)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def url(self, p):
        return f'http://127.0.0.1:{self.port}{p}'

    def publish(self, data, sig=None, digest=True, size=None):
        assets = [{'name': NAME, 'browser_download_url': self.url('/d/' + NAME), 'size': size or len(data),
                   'digest': 'sha256:' + hashlib.sha256(data).hexdigest() if digest else None}]
        if sig is not False:
            assets.append({'name': NAME + '.sig', 'browser_download_url': self.url('/d/s.sig'), 'size': 128, 'digest': None})
            self.routes['/d/s.sig'] = (200, sig if sig is not None else sign(data), {})
        self.routes['/feed'] = (200, json.dumps([{'tag_name': 'v' + NEW, 'draft': False, 'html_url': 'x', 'assets': assets}]).encode(), {})
        self.routes['/d/' + NAME] = (200, data, {})


S = Server()
NEW_FILE = script('new')


class App:
    def __init__(self, name):
        self.dir = os.path.join(TMP, name)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        self.path = os.path.join(self.dir, 'Savage_Wind-old.AppImage')
        self.old = script('old')
        with open(self.path, 'wb') as f:
            f.write(self.old)
        os.chmod(self.path, 0o755)
        with open(os.path.join(self.dir, 'mine.txt'), 'w') as f:
            f.write('mine')
        self.work = os.path.join(TMP, name + '-work')
        os.makedirs(self.work, exist_ok=True)

    def run(self, cmd, env=None, feed=None):
        e = dict(os.environ, APPIMAGE=self.path, SW_UPDATE_TEST_FEED=feed or S.url('/feed'))
        e.update(env or {})
        p = subprocess.run([TOOL, cmd, self.work, OLD], env=e, capture_output=True, timeout=120)
        self.result = dict(line.split('=', 1) for line in p.stdout.decode().splitlines() if '=' in line)
        return self.result

    def read(self, n=None):
        with open(n or self.path, 'rb') as f:
            return f.read()

    def has(self, suffix):
        return os.path.exists(self.path + suffix)

    def assert_old(self):
        assert self.read() == self.old, 'the AppImage changed'
        assert os.stat(self.path).st_mode & stat.S_IXUSR
        assert not self.has('.old')
        assert self.read(os.path.join(self.dir, 'mine.txt')) == b'mine'


def expect(r, code):
    assert r.get('status') == 'error' and r.get('code') == code, f'expected {code}, got {r}'
    assert r.get('title') and r.get('what'), r


SC = {}


def scenario(f):
    SC[f.__name__] = f
    return f


@scenario
def update_ok():
    a = App('ok')
    S.publish(NEW_FILE)
    a.run('apply')
    assert a.read() == NEW_FILE, 'not replaced'
    assert os.stat(a.path).st_mode & stat.S_IXUSR, 'not executable'
    assert a.read(a.path + '.old') == a.old, 'the old one must stay until set up'
    assert not a.has('.part')
    assert b'--config' in a.read(os.path.join(a.dir, 'started.txt')), 'not reopened'
    assert a.read(os.path.join(a.dir, 'mine.txt')) == b'mine'


@scenario
def go_back():
    a = App('back')
    S.publish(NEW_FILE)
    a.run('apply')
    assert a.run('back')['status'] == 'back'
    assert a.read() == a.old and not a.has('.old')


@scenario
def nothing_to_go_back_to():
    a = App('noback')
    expect(a.run('back'), 'no_backup')


@scenario
def check_only():
    a = App('check')
    S.publish(NEW_FILE)
    r = a.run('check')
    assert r['status'] == 'available' and r['version'] == NEW, r
    a.assert_old()


@scenario
def no_releases():
    a = App('none')
    S.routes['/feed'] = (200, b'[]', {})
    assert a.run('apply')['status'] == 'current'


@scenario
def unsigned_not_offered():
    a = App('unsigned')
    S.publish(NEW_FILE, sig=False)
    assert a.run('apply')['status'] == 'current'
    a.assert_old()


@scenario
def resumes():
    a = App('resume')
    data = NEW_FILE + b'#' * 200000
    S.publish(data)
    with open(a.path + '.part', 'wb') as f:
        f.write(data[:1000])
    S.hits.clear()
    a.run('apply')
    assert a.read() == data
    assert any(h[1] == 'bytes=1000-' for h in S.hits), S.hits


@scenario
def stale_part_downloaded_again():
    a = App('stale')
    S.publish(NEW_FILE)
    with open(a.path + '.part', 'wb') as f:
        f.write(b'x' * len(NEW_FILE))
    a.run('apply')
    assert a.read() == NEW_FILE


@scenario
def connection_refused():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    a = App('refused')
    expect(a.run('check', feed=f'http://127.0.0.1:{port}/f'), 'cannot_connect')


@scenario
def no_internet():
    a = App('dns')
    expect(a.run('check', feed='http://no-such-host.invalid/f'), 'no_internet')


@scenario
def curl_missing():
    a = App('nocurl')
    empty = os.path.join(TMP, 'empty')
    os.makedirs(empty, exist_ok=True)
    expect(a.run('check', env={'PATH': empty}), 'no_curl')


@scenario
def rate_limited():
    a = App('rate')
    S.routes['/feed'] = (403, b'{}', {'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': str(int(time.time()) + 600)})
    r = a.run('check')
    expect(r, 'rate_limited')
    assert 'minute' in r['what']


@scenario
def github_down():
    a = App('down')
    S.routes['/feed'] = (503, b'x', {})
    expect(a.run('check'), 'github_down')


@scenario
def repo_gone():
    a = App('gone')
    S.routes['/feed'] = (404, b'x', {})
    expect(a.run('check'), 'not_found')


@scenario
def captive_portal():
    a = App('portal')
    S.routes['/feed'] = (200, b'<html>log in</html>', {})
    expect(a.run('check'), 'bad_response')


@scenario
def download_gone():
    a = App('dlgone')
    S.publish(NEW_FILE)
    S.routes['/d/' + NAME] = (404, b'', {})
    expect(a.run('apply'), 'not_found')
    a.assert_old()


@scenario
def damaged():
    a = App('damaged')
    S.publish(NEW_FILE)
    S.routes['/d/' + NAME] = (200, b'X' * len(NEW_FILE), {})
    expect(a.run('apply'), 'corrupt_download')
    a.assert_old()
    assert not a.has('.part')


@scenario
def forged_signature():
    a = App('forged')
    S.publish(NEW_FILE, sig=sign(NEW_FILE, OTHER))
    expect(a.run('apply'), 'bad_signature')
    a.assert_old()
    assert not a.has('.part')


@scenario
def garbage_signature():
    a = App('garbage')
    S.publish(NEW_FILE, sig=b'nonsense')
    expect(a.run('apply'), 'bad_signature')
    a.assert_old()


@scenario
def not_enough_disk():
    a = App('disk')
    S.publish(NEW_FILE)
    expect(a.run('apply', env={'SW_UPDATE_TEST_FREE_MB': '10'}), 'disk_full')
    a.assert_old()


@scenario
def read_only_folder():
    if os.geteuid() == 0:
        return
    a = App('ro')
    S.publish(NEW_FILE)
    os.chmod(a.dir, 0o555)
    try:
        expect(a.run('apply'), 'read_only')
    finally:
        os.chmod(a.dir, 0o755)
    a.assert_old()


@scenario
def new_version_does_not_start():
    a = App('health')
    bad = script('new', health=5)
    S.publish(bad)
    expect(a.run('apply'), 'health_failed')
    a.assert_old()
    assert not a.has('.part')


@scenario
def not_an_appimage():
    a = App('notai')
    S.publish(NEW_FILE)
    expect(a.run('apply', env={'APPIMAGE': ''}), 'not_appimage')


def main():
    want = sys.argv[1] if len(sys.argv) > 1 else ''
    names = [n for n in SC if want in n]
    failed = []
    for n in names:
        try:
            SC[n]()
            print(f'  ok    {n}', flush=True)
        except Exception as e:  # noqa: BLE001  (every scenario runs; the failures are listed at the end)
            failed.append(n)
            print(f'  FAIL  {n}: {type(e).__name__}: {e}', flush=True)
    print(f'{len(names) - len(failed)} of {len(names)} scenarios passed')
    if failed:
        print('failed:', ', '.join(failed), '| fixtures in', TMP)
        return 1
    shutil.rmtree(TMP, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
