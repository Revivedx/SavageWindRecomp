#!/usr/bin/env python3
"""SW Update, every way an update can go wrong (Windows; make test-sw-update).

Builds SW Update with a throwaway signing key and a test feed, then for each scenario makes a small fake install in a
temporary folder (a stand-in savagewind.exe, DLL, game files, saves), serves made-up releases from a local HTTP server
and checks what SW Update reports and, above all, what is left on disk. No network, no game files.

usage: tools/test_sw_update.py [substring of the scenario names]
"""
import hashlib
import http.server
import io
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix='sw_update_test_', dir=os.path.join(ROOT, 'build'))
os.environ['TMP'] = os.environ['TEMP'] = TMP
OLD = '0.5.1-alpha.5'
NEW = '0.5.2-alpha.5'

# ------------------------------------------------------------------ building
FAKE_GAME_C = r'''
#include <stdio.h>
#include <string.h>
#include <windows.h>
int main(int argc, char **argv) {
    char exe[1024]; GetModuleFileNameA(NULL, exe, sizeof exe); *strrchr(exe, '\\') = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--update-health")) return HEALTH;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--sleep")) { Sleep(30000); return 0; }
    char p[1200]; snprintf(p, sizeof p, "%s\\started.txt", exe);
    FILE *f = fopen(p, "ab"); if (f) { for (int i = 1; i < argc; i++) fprintf(f, "%s ", argv[i]); fprintf(f, "\n"); fclose(f); }
    return 0;
}
'''


def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, **kw)
    if p.returncode:
        raise SystemExit(f'{cmd[0]} failed: {p.stderr.decode(errors="replace")[-1500:]}')
    return p


def build():
    key = os.path.join(TMP, 'key.pem')
    run(['openssl', 'ecparam', '-genkey', '-name', 'prime256v1', '-noout', '-out', key])
    pub = run([sys.executable, 'tools/sign_update.py', '--public', key], cwd=ROOT).stdout.decode().strip()
    other = os.path.join(TMP, 'other.pem')
    run(['openssl', 'ecparam', '-genkey', '-name', 'prime256v1', '-noout', '-out', other])
    exe = os.path.join(TMP, 'sw-update-test.exe')
    header = os.path.join(TMP, 'key.h')
    with open(header, 'w') as f:
        f.write(f'#define SW_UPDATE_TEST_PUBKEY "{pub}"\n')
    objs = []
    for src in ('runtime/platform/windows_update.c', 'runtime/update_core.c'):
        obj = os.path.join(TMP, os.path.basename(src) + '.o')
        run(['clang', '-O1', '-DSW_UPDATE_TEST', '-include', header, '-Ithird_party/stb', '-c', src, '-o', obj], cwd=ROOT)
        objs.append(obj)
    run(['clang', '-o', exe, *objs, '-static', '-fuse-ld=lld', '-municode', '-Wl,--subsystem,windows',
         '-lwinhttp', '-lbcrypt', '-lcomctl32', '-lshell32', '-luser32', '-lgdi32'], cwd=ROOT)
    src = os.path.join(TMP, 'fake.c')
    with open(src, 'w') as f:
        f.write(FAKE_GAME_C)
    fakes = {}
    for name, health in (('good', 73), ('bad', 5)):
        out = os.path.join(TMP, f'fake-{name}.exe')
        obj = out + '.o'
        run(['clang', '-O1', f'-DHEALTH={health}', '-c', src, '-o', obj])
        run(['clang', '-o', out, obj, '-static'])
        with open(out, 'rb') as f:
            fakes[name] = f.read()
    return key, other, exe, fakes


KEY, OTHER_KEY, TOOL, FAKE = build()


# ------------------------------------------------------------------ fixtures
def sha(b):
    return hashlib.sha256(b).hexdigest()


def manifest(version, files):
    lines = ['savagewind-update-manifest 1', f'version {version}']
    lines += [f'f {sha(b)} {len(b)} {p}' for p, b in sorted(files.items())]
    return ('\n'.join(lines) + '\n').encode()


def old_files():
    return {'savagewind.exe': FAKE['good'], 'SDL3.dll': b'old sdl', 'zig/zig.exe': b'old zig', 'only-in-old.txt': b'old',
            'colour-luts/a.cube': b'old lut', 'SW Update.exe': b'placeholder'}


def new_files():
    return {'savagewind.exe': FAKE['good'], 'SDL3.dll': b'new sdl', 'zig/zig.exe': b'new zig', 'only-in-new.txt': b'new',
            'colour-luts/a.cube': b'new lut', 'SW Update.exe': b'new updater', 'Savage Wind Configuration.exe': b'new cfg'}


def make_zip(files, man=None, version=NEW, extra=None):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('Savage Wind/sw-manifest.txt', man if man is not None else manifest(version, files))
        for p, b in files.items():
            z.writestr(f'Savage Wind/{p}', b)
        for p, b in (extra or {}).items():
            z.writestr(p, b)
    return out.getvalue()


def signature(data, key=KEY):
    path = os.path.join(TMP, 'to-sign.zip')
    with open(path, 'wb') as f:
        f.write(data)
    run([sys.executable, 'tools/sign_update.py', path, key], cwd=ROOT)
    with open(path + '.sig', 'rb') as f:
        return f.read()


class Server:
    """A release list and its files; the behaviour of each URL comes from self.routes: path -> (status, body, headers)
    or a function(handler)."""

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
                if r is None:
                    r = (404, b'{"message":"Not Found"}', {})
                status, body, headers = r
                rng = self.headers.get('Range')
                if rng and status == 200 and self.path.endswith('.zip'):
                    start = int(rng.split('=')[1].rstrip('-'))
                    if start >= len(body):
                        self.send_response(416)
                        self.send_header('Content-Length', '0')
                        self.end_headers()
                        return
                    body = body[start:]
                    status = 206
                self.send_response(status)
                for k, v in headers.items():
                    self.send_header(k, v)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                except OSError:
                    pass

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def url(self, p):
        return f'http://127.0.0.1:{self.port}{p}'

    def publish(self, zip_bytes, sig=None, version=NEW, digest=True, size=None):
        name = f'SavageWind-{version}-windows.zip'
        z, s = f'/d/{name}', f'/d/{name}.sig'
        assets = [{'name': name, 'browser_download_url': self.url(z), 'size': size if size is not None else len(zip_bytes),
                   'digest': 'sha256:' + sha(zip_bytes) if digest else None}]
        if sig is not False:
            assets.append({'name': name + '.sig', 'browser_download_url': self.url(s), 'size': 128, 'digest': None})
        self.routes['/feed'] = (200, json.dumps([
            {'tag_name': f'v{version}', 'draft': False, 'prerelease': True, 'html_url': self.url('/page'), 'assets': assets},
            {'tag_name': f'v{OLD}', 'draft': False, 'prerelease': True, 'html_url': self.url('/old'), 'assets': []},
        ]).encode(), {})
        self.routes[z] = (200, zip_bytes, {})
        if sig is not False:
            self.routes[s] = (200, sig if sig is not None else signature(zip_bytes), {})


SERVER = Server()
USER_FILES = {'extracted/exefs/main': b'game', 'extracted/romfs/a.bin': b'romfs', 'save/save_0/data.sav': b'my save',
              'cache/xlate/x.xlz': b'shader', 'savagewind.ini': b'fov = 90\n', 'splash.png': b'mine', 'mods/my.mod': b'mod',
              'colour-luts/my-own.cube': b'my lut', 'SW Update.log': b''}


class Install:
    def __init__(self, name, files=None, with_manifest=True, version=OLD):
        self.dir = os.path.join(TMP, 'installs', name)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        self.files = dict(files if files is not None else old_files())
        with open(TOOL, 'rb') as f:
            self.files['SW Update.exe'] = f.read()
        for p, b in {**self.files, **USER_FILES}.items():
            self.write(p, b)
        if with_manifest:
            self.write('sw-manifest.txt', manifest(version, self.files))
        self.user_before = self.user_state()

    def write(self, p, b):
        q = os.path.join(self.dir, p)
        os.makedirs(os.path.dirname(q), exist_ok=True)
        with open(q, 'wb') as f:
            f.write(b)

    def read(self, p):
        with open(os.path.join(self.dir, p), 'rb') as f:
            return f.read()

    def has(self, p):
        return os.path.exists(os.path.join(self.dir, p))

    def user_state(self):
        return {p: self.read(p) for p in USER_FILES if p != 'SW Update.log'}

    def run(self, *args, env=None, timeout=60, feed=True):
        out = os.path.join(TMP, f'result-{os.path.basename(self.dir)}.txt')
        if os.path.exists(out):
            os.remove(out)
        e = dict(os.environ)
        if feed:
            e['SW_UPDATE_TEST_FEED'] = SERVER.url('/feed')
        e.update(env or {})
        p = subprocess.run([os.path.join(self.dir, 'SW Update.exe'), '--headless', '--out', out, *args], env=e, timeout=timeout,
                           capture_output=True)
        self.exit = p.returncode
        self.result = {}
        if os.path.exists(out):
            with open(out, encoding='utf-8') as f:
                for line in f:
                    k, _, v = line.rstrip('\n').partition('=')
                    self.result[k] = v
        return self.result

    def assert_user_intact(self):
        assert self.user_state() == self.user_before, 'the player\'s own files changed'

    def assert_old(self):
        """Exactly the old version, as if nothing had happened."""
        for p, b in self.files.items():
            if p == 'SW Update.exe':
                continue
            assert self.read(p) == b, f'{p} is not the old file'
        for p in ('only-in-new.txt', 'Savage Wind Configuration.exe'):
            if p not in self.files:
                assert not self.has(p), f'{p} is still there'
        self.assert_user_intact()

    def assert_new(self, files=None):
        for p, b in (files or new_files()).items():
            assert self.read(p) == b, f'{p} is not the new file'
        assert not self.has('only-in-old.txt'), 'a file the new version dropped is still there'
        assert NEW.encode() in self.read('sw-manifest.txt')
        assert b'--config' in self.read('started.txt'), 'Configuration was not reopened'
        self.assert_user_intact()


def expect(r, code):
    assert r.get('status') == 'error', f'status {r}'
    assert r.get('code') == code, f'expected {code}, got {r}'
    assert r.get('title') and r.get('what'), f'no explanation: {r}'


SCENARIOS = {}


def scenario(f):
    SCENARIOS[f.__name__] = f
    return f


def good_release():
    SERVER.publish(make_zip(new_files()))


# ------------------------------------------------------------------ it works
@scenario
def update_ok():
    i = Install('ok')
    good_release()
    r = i.run('--apply')
    assert r['status'] == 'installed', r
    i.assert_new()
    assert i.has('update/backup/SDL3.dll') and i.has('update/journal.txt'), 'the previous version must stay until set up'


@scenario
def check_finds_update():
    i = Install('check')
    good_release()
    r = i.run('--check')
    assert r['status'] == 'available' and r['version'] == NEW, r
    i.assert_old()


@scenario
def already_newest():
    i = Install('current', version=NEW)
    good_release()
    r = i.run('--apply')
    assert r['status'] == 'current', r


@scenario
def newer_than_published():
    i = Install('newer', version='0.9.0-alpha.9')
    good_release()
    assert i.run('--check')['status'] == 'current'


@scenario
def no_manifest_old_copy():
    i = Install('nomanifest', with_manifest=False)
    good_release()
    r = i.run('--apply')
    assert r['status'] == 'installed', r
    for p, b in new_files().items():
        assert i.read(p) == b
    assert i.read('only-in-old.txt') == b'old', 'without a list nothing may be removed'
    i.assert_user_intact()


@scenario
def resumes_download():
    i = Install('resume')
    z = make_zip(new_files())
    SERVER.publish(z)
    i.write(f'update/download/SavageWind-{NEW}-windows.zip', z[:1000])
    SERVER.hits.clear()
    r = i.run('--apply')
    assert r['status'] == 'installed', r
    assert any(h[1] == 'bytes=1000-' for h in SERVER.hits), SERVER.hits


@scenario
def path_with_accents_and_spaces():
    i = Install('carpeta con ñ y espacios áé')
    good_release()
    assert i.run('--apply')['status'] == 'installed'
    i.assert_new()


# ------------------------------------------------------------------ the network
def net_case(err, code):
    i = Install(f'net{err}')
    good_release()
    expect(i.run('--apply', env={'SW_UPDATE_TEST_NET_ERR': str(err)}), code)
    i.assert_old()


@scenario
def no_internet():
    net_case(12007, 'no_internet')


@scenario
def blocked_connection():
    net_case(12029, 'cannot_connect')


@scenario
def timed_out():
    net_case(12002, 'timeout')


@scenario
def wrong_clock_certificate():
    net_case(12037, 'tls')


@scenario
def broken_proxy():
    net_case(12166, 'proxy')


@scenario
def server_really_closed():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    i = Install('closed')
    expect(i.run('--check', env={'SW_UPDATE_TEST_FEED': f'http://127.0.0.1:{port}/feed'}), 'cannot_connect')


@scenario
def rate_limited():
    i = Install('rate')
    reset = str(int(time.time()) + 600)
    SERVER.routes['/feed'] = (403, b'{}', {'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': reset})
    r = i.run('--check')
    expect(r, 'rate_limited')
    assert 'minute' in r['what'], r


def status_case(status, code):
    i = Install(f'status{status}')
    SERVER.routes['/feed'] = (status, b'oops', {})
    expect(i.run('--check'), code)


@scenario
def repo_gone():
    status_case(404, 'not_found')


@scenario
def github_down():
    status_case(503, 'github_down')


@scenario
def captive_portal_html():
    i = Install('portal')
    SERVER.routes['/feed'] = (200, b'<html>Please log in to the hotel wifi</html>', {})
    expect(i.run('--check'), 'bad_response')


@scenario
def no_releases():
    i = Install('norel')
    SERVER.routes['/feed'] = (200, b'[]', {})
    assert i.run('--apply')['status'] == 'current'


@scenario
def release_without_signature_not_offered():
    i = Install('unsigned')
    SERVER.publish(make_zip(new_files()), sig=False)
    assert i.run('--apply')['status'] == 'current'
    i.assert_old()


@scenario
def download_gone():
    i = Install('gone')
    good_release()
    SERVER.routes[f'/d/SavageWind-{NEW}-windows.zip'] = (404, b'', {})
    expect(i.run('--apply'), 'not_found')
    i.assert_old()


@scenario
def connection_keeps_dropping():
    i = Install('dropping')
    z = make_zip(new_files())
    SERVER.publish(z, size=len(z) + 100000)

    def cut(h):
        h.send_response(200)
        h.send_header('Content-Length', str(len(z)))
        h.end_headers()
        h.wfile.write(z)
        h.close_connection = True
    SERVER.routes[f'/d/SavageWind-{NEW}-windows.zip'] = cut
    expect(i.run('--apply', timeout=120), 'download_interrupted')
    i.assert_old()


# ------------------------------------------------------------------ bad downloads
@scenario
def damaged_download():
    i = Install('damaged')
    z = bytearray(make_zip(new_files()))
    SERVER.publish(bytes(z))
    z[100] ^= 0xFF
    SERVER.routes[f'/d/SavageWind-{NEW}-windows.zip'] = (200, bytes(z), {})
    expect(i.run('--apply'), 'corrupt_download')
    i.assert_old()
    assert not i.has(f'update/download/SavageWind-{NEW}-windows.zip'), 'a bad download must not stay'


@scenario
def forged_signature():
    i = Install('forged')
    z = make_zip(new_files())
    SERVER.publish(z, sig=signature(z, OTHER_KEY))
    expect(i.run('--apply'), 'bad_signature')
    i.assert_old()


@scenario
def garbage_signature():
    i = Install('garbage')
    SERVER.publish(make_zip(new_files()), sig=b'not a signature')
    expect(i.run('--apply'), 'bad_signature')
    i.assert_old()


@scenario
def signature_download_missing():
    i = Install('nosig')
    good_release()
    SERVER.routes[f'/d/SavageWind-{NEW}-windows.zip.sig'] = (404, b'', {})
    expect(i.run('--apply'), 'not_found')
    i.assert_old()


def signed_zip_case(name, zip_bytes, code):
    i = Install(name)
    SERVER.publish(zip_bytes)
    expect(i.run('--apply'), code)
    i.assert_old()


@scenario
def not_a_zip():
    signed_zip_case('notzip', b'this is not a zip' * 100, 'corrupt_zip')


@scenario
def zip_without_list():
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w') as z:
        z.writestr('Savage Wind/savagewind.exe', b'x')
    signed_zip_case('nolist', out.getvalue(), 'corrupt_zip')


@scenario
def zip_slip_in_list():
    f = new_files()
    bad = ('savagewind-update-manifest 1\nversion ' + NEW + '\nf ' + sha(b'x') + ' 1 ../../evil.exe\n').encode()
    signed_zip_case('slip1', make_zip(f, man=bad), 'corrupt_zip')


@scenario
def file_outside_the_list():
    signed_zip_case('slip2', make_zip(new_files(), extra={'Savage Wind/../../evil.exe': b'x'}), 'corrupt_zip')
    assert not os.path.exists(os.path.join(TMP, 'evil.exe')), 'wrote outside the folder'


@scenario
def file_not_matching_list():
    f = new_files()
    man = manifest(NEW, {**f, 'SDL3.dll': b'something else'})
    signed_zip_case('mismatch', make_zip(f, man=man), 'corrupt_zip')


@scenario
def file_missing_from_zip():
    f = new_files()
    man = manifest(NEW, {**f, 'needed.dll': b'where is it'})
    signed_zip_case('missing', make_zip(f, man=man), 'corrupt_zip')


@scenario
def zip_says_other_version():
    signed_zip_case('otherver', make_zip(new_files(), version='0.5.3-alpha.5'), 'wrong_version')


# ------------------------------------------------------------------ the folder and the PC
@scenario
def game_is_running():
    i = Install('running')
    good_release()
    exe = os.path.join(i.dir, 'savagewind-running.exe')
    with open(exe, 'wb') as f:
        f.write(FAKE['good'])
    t = subprocess.Popen([exe, '--sleep'])
    try:
        time.sleep(0.5)
        r = i.run('--apply')
        expect(r, 'game_running')
        assert 'savagewind-running.exe' in r['what'], r
        assert t.poll() is None, 'the running game was closed'
    finally:
        t.kill()
        t.wait()
    os.remove(exe)
    i.assert_old()
    good_release()
    assert i.run('--apply')['status'] == 'installed', 'Retry after closing the game'


@scenario
def not_enough_disk():
    i = Install('disk')
    good_release()
    r = i.run('--apply', env={'SW_UPDATE_TEST_FREE_MB': '50'})
    expect(r, 'disk_full')
    assert 'GB' in r['what'], r
    i.assert_old()


@scenario
def not_in_game_folder():
    i = Install('wrongfolder')
    os.remove(os.path.join(i.dir, 'savagewind.exe'))
    good_release()
    elsewhere = os.environ.get('SystemRoot', r'C:\Windows') + r'\Temp'
    expect(i.run('--apply', env={'TMP': elsewhere, 'TEMP': elsewhere}), 'wrong_folder')


@scenario
def opened_inside_zip():
    i = Install(os.path.join('Downloads', 'SavageWind.zip', 'Savage Wind'))
    os.remove(os.path.join(i.dir, 'savagewind.exe'))
    good_release()
    expect(i.run('--apply'), 'inside_zip')


@scenario
def read_only_folder():
    i = Install('readonly')
    good_release()
    sid = subprocess.check_output(['whoami', '/user', '/fo', 'csv', '/nh']).decode().strip().split(',')[1].strip('"')
    run(['icacls', i.dir, '/deny', f'*{sid}:(OI)(CI)(WD,AD,DC)'])
    try:
        r = i.run('--apply')
    finally:
        run(['icacls', i.dir, '/remove:d', f'*{sid}'])
    expect(r, 'read_only')
    i.assert_old()


def locked(path):
    import msvcrt
    f = open(path, 'r+b')
    msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
    return f


@scenario
def file_locked_midway_is_rolled_back():
    i = Install('locked')
    good_release()
    # an open file without FILE_SHARE_DELETE can't be renamed: what an antivirus or another program does
    held = open(os.path.join(i.dir, 'zig', 'zig.exe'), 'rb')
    try:
        r = i.run('--apply', timeout=120)
    finally:
        held.close()
    expect(r, 'locked_file')
    assert 'zig/zig.exe' in r['what'] or 'zig' in r['tech'], r
    i.assert_old()
    good_release()
    r = i.run('--apply')
    assert r['status'] == 'installed', ('Retry when the file is free', r)
    i.assert_new()


@scenario
def new_version_fails_to_start_is_rolled_back():
    i = Install('badhealth')
    f = new_files()
    f['savagewind.exe'] = FAKE['bad']
    SERVER.publish(make_zip(f))
    r = i.run('--apply')
    expect(r, 'health_failed')
    i.assert_old()


@scenario
def power_cut_at_every_step_recovers():
    steps = 2 * len(new_files()) + 8
    for at in (0, 1, 2, 3, 5, 8, 11, steps // 2, steps - 3):
        i = Install('cut')
        good_release()
        i.run('--apply', env={'SW_UPDATE_TEST_CRASH_AT': str(at)})
        assert i.exit == 99 or i.result.get('status') == 'installed', (at, i.exit, i.result)
        if i.exit != 99:
            continue
        assert i.has('update/journal.txt')
        r = i.run('--recover')
        assert i.exit == 0, (at, i.exit, r)
        i.assert_old()
        assert not i.has('update/journal.txt')
    i = Install('cut-then-update')
    good_release()
    i.run('--apply', env={'SW_UPDATE_TEST_CRASH_AT': '6'})
    assert i.run('--apply')['status'] == 'installed', 'updating again after a cut must just work'
    i.assert_new()


@scenario
def go_back_after_update():
    i = Install('rollback')
    f = new_files()
    f['SW Update.exe'] = i.files['SW Update.exe']  # (a real program this time: it is what runs the going back)
    SERVER.publish(make_zip(f))
    assert i.run('--apply')['status'] == 'installed'
    r = i.run('--rollback')
    assert r['status'] == 'rolled_back', r
    i.assert_old()
    assert i.read('only-in-old.txt') == b'old'
    assert OLD.encode() in i.read('sw-manifest.txt')


@scenario
def nothing_to_go_back_to():
    i = Install('noback')
    expect(i.run('--rollback'), 'no_backup')
    i.assert_old()


@scenario
def two_at_once():
    i = Install('twice')
    z = make_zip(new_files())
    SERVER.publish(z)

    def slow(h):
        time.sleep(4)
        h.send_response(200)
        h.send_header('Content-Length', str(len(z)))
        h.end_headers()
        h.wfile.write(z)
    SERVER.routes[f'/d/SavageWind-{NEW}-windows.zip'] = slow
    e = dict(os.environ, SW_UPDATE_TEST_FEED=SERVER.url('/feed'))
    first = subprocess.Popen([os.path.join(i.dir, 'SW Update.exe'), '--headless', '--apply'], env=e)
    time.sleep(1.5)
    # (the other copy counts as "running from this folder" only if nothing_running comes first: the lock is earlier)
    expect(i.run('--apply'), 'already_running')
    first.wait(60)


def main():
    want = sys.argv[1] if len(sys.argv) > 1 else ''
    failed = []
    names = [n for n in SCENARIOS if want in n]
    for n in names:
        try:
            SCENARIOS[n]()
            print(f'  ok    {n}', flush=True)
        except Exception as e:  # noqa: BLE001  (every scenario runs; the failures are listed at the end)
            failed.append(n)
            print(f'  FAIL  {n}: {type(e).__name__}: {e}', flush=True)
    print(f'{len(names) - len(failed)} of {len(names)} scenarios passed')
    if failed:
        print('failed:', ', '.join(failed))
        print('fixtures left in', TMP)
        return 1
    shutil.rmtree(TMP, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
