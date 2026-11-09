"""Query the analyzed Ghidra project of the game's main module (read-only).

Usage: gq.py <command> [args...]

  summary                     program, language, memory blocks, counts
  decompile <addr|name>...    pseudocode of the functions containing these addresses
  xrefs <addr|name>...        references to an address (callers of a function)
  callees <addr|name>...      functions called from a function
  disasm <addr|name> [n]      n instructions from an address (default 40)
  strings <regex> [max]       defined strings matching a regex, with the functions using them
  symbols <regex> [max]       symbols whose qualified name matches a regex
  functions                   every function as TSV (entry, name, bytes, references, thunk)

Addresses can be given as Ghidra addresses (main loads at 0x7100000000 there) or as runtime
addresses of main (0x70xxxxxxxx, the base Savage Wind's generator and runtime use); runtime ones
are moved to Ghidra's base. Output goes to stdout, never into the repository: it is derived from
the game's code.

Paths come from the environment or from tools/ghidra/gq.local.json (git-ignored):
  GHIDRA_INSTALL_DIR   extracted Ghidra release (with the Switch loader extension installed)
  JAVA_HOME            JDK 21+
  SW_GHIDRA_PROJECT    directory holding <name>.gpr (default: ../ghidra next to the repository)
  SW_GHIDRA_NAME       project name (default botw_1.9.0, the supported version; botw_1.8.2 is kept)
  SW_GHIDRA_PROGRAM    program inside the project (default main)
"""
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
RUNTIME_MAIN = (0x7000000000, 0x7004000000)  # runtime/rt.h MAIN_BASE .. SDK_BASE
GHIDRA_SHIFT = 0x7100000000 - 0x7000000000   # the Switch loader's base for main


def setting(name, default=None):
    local = os.path.join(HERE, 'gq.local.json')
    cfg = json.load(open(local)) if os.path.exists(local) else {}
    return os.environ.get(name) or cfg.get(name) or default


def to_ghidra(arg):
    if re.fullmatch(r'(?i)0x[0-9a-f]+', arg):
        v = int(arg, 16)
        if RUNTIME_MAIN[0] <= v < RUNTIME_MAIN[1]:
            print(f'(runtime {arg} -> ghidra {v + GHIDRA_SHIFT:#x})', file=sys.stderr)
            return hex(v + GHIDRA_SHIFT)
    return arg


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ('-h', '--help'):
        print(__doc__)
        return 0
    ghidra = setting('GHIDRA_INSTALL_DIR')
    if not ghidra:
        sys.exit('set GHIDRA_INSTALL_DIR (or put it in tools/ghidra/gq.local.json)')
    project = setting('SW_GHIDRA_PROJECT', os.path.join(os.path.dirname(REPO), 'ghidra'))
    name = setting('SW_GHIDRA_NAME', 'botw_1.9.0')
    program = setting('SW_GHIDRA_PROGRAM', 'main')
    env = dict(os.environ)
    if setting('JAVA_HOME'):
        env['JAVA_HOME'] = setting('JAVA_HOME')
        env['PATH'] = os.path.join(env['JAVA_HOME'], 'bin') + os.pathsep + env.get('PATH', '')
    env.setdefault('GHIDRA_HEADLESS_MAXMEM', '8G')

    cmd, args = sys.argv[1], [to_ghidra(a) for a in sys.argv[2:]]
    with tempfile.TemporaryDirectory(prefix='swgq-') as tmp:
        result, request = os.path.join(tmp, 'result.txt'), os.path.join(tmp, 'request.txt')
        with open(request, 'w', encoding='utf-8') as f:
            f.write('\n'.join([result, cmd] + args) + '\n')
        launcher = os.path.join(ghidra, 'support', 'analyzeHeadless' + ('.bat' if os.name == 'nt' else ''))
        proc = subprocess.run([launcher, project, name, '-process', program, '-noanalysis', '-readOnly',
                               '-scriptPath', HERE, '-postScript', 'SWQuery.java', request],
                              env=env, capture_output=True, text=True, errors='replace')
        if os.path.exists(result):
            sys.stdout.write(open(result, encoding='utf-8').read())
            return 0
        log = (proc.stdout + proc.stderr).splitlines()
        errors = [l for l in log if re.search(r'ERROR|Exception|Unable to lock|LockException', l)]
        print('\n'.join((errors or log)[-25:]), file=sys.stderr)
        if any('lock' in l.lower() for l in errors):
            print('(the project is open elsewhere, for example while the analysis is still running)', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
