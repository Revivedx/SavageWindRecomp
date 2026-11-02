"""Compile the generated entry hook against a synthetic CPU to check its return and stack behavior."""
import os
import shlex
import struct
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace

import patches

gen = SimpleNamespace(base=0x1000, nso=SimpleNamespace(image=struct.pack('<I', 0xd103c3ff)))
hook = patches._amiibo_daily_count(gen, gen.base)
source = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef struct { uint64_t x[31], sp; } Cpu;
#define LD64(p) (*(const uint64_t *)(uintptr_t)(p))
static int bypass;
int rt_amiibo_ignore_daily(const char *name, uint32_t kind) {
    assert(!strcmp(name, "synthetic history"));
    assert(kind == 42);
    return bypass;
}
static int continued;
static void entry(Cpu *c) {
''' + hook + r'''
    continued++;
}
int main(void) {
    struct { uint64_t vtable; const char *text; } name = {0, "synthetic history"};
    Cpu c = {.x = {99, (uintptr_t)&name, 42}, .sp = 0x10000};
    c.x[30] = 0x1234;
    bypass = 1; entry(&c);
    assert(c.x[0] == 0 && c.sp == 0x10000 && c.x[30] == 0x1234 && !continued);
    c.x[0] = 99;
    bypass = 0; entry(&c);
    assert(c.x[0] == 99 && c.sp == 0xff10 && c.x[30] == 0x1234 && continued == 1);
}
'''
with tempfile.TemporaryDirectory(prefix='sw-amiibo-hook-') as tmp:
    path = Path(tmp)
    (path / 'hook.c').write_text(source)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=c11', '-Wall', '-Werror', str(path / 'hook.c'), '-o', str(path / 'hook')], check=True)
    subprocess.run([str(path / 'hook')], check=True)
print('PASS: amiibo generated hook preserves the stack and return address on both paths')
