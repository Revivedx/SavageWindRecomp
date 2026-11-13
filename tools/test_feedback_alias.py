#!/usr/bin/env python3
"""Exercise the actual feedback slot helper with synthetic framebuffer aliases."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "runtime/nvn/vulkan/exec.c").read_text()
start = source.index("static int feedback_color_slot(")
end = source.index("// The draw writes neither depth nor stencil:", start)
helper = source[start:end]
fixture = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define LOG printf
typedef struct { int image; } NvnTexture;
typedef struct { unsigned out_mask; } ShaderInfo;
typedef struct { ShaderInfo *fi; } Prog;
static struct { struct { int ncolor; NvnTexture *color[8]; } rt; unsigned channel_mask; } st;
static int same_image(NvnTexture *a, NvnTexture *b) { return a && b && a->image == b->image; }
static int draw_writes_color(Prog *pr, int i) {
    return ((pr->fi->out_mask >> (i * 4)) & 15) && ((st.channel_mask >> (i * 4)) & 15);
}
"""
checks = r"""
int main(void) {
    const char *e = getenv("SAVAGEWIND_VK_FEEDBACK_ALIAS_FIX");
    int enabled = !e || atoi(e);  // (on by default since 0.5.1: =0 turns it off)
    NvnTexture a = {1}, alias = {1}, b = {2}, alias_b = {2}, unique = {3}, missing = {99};
    st.rt.ncolor = 6;
    st.rt.color[1] = &a; st.rt.color[2] = &alias;
    st.rt.color[3] = &b; st.rt.color[4] = &alias_b; st.rt.color[5] = &unique;
    ShaderInfo info = {0x00f0f0}; Prog pr = {&info}; st.channel_mask = 0x00f0f0;
    assert(feedback_color_slot(&pr, &a) == (enabled ? 1 : 2));
    assert(feedback_color_slot(&pr, &b) == (enabled ? 3 : 4));
    assert(feedback_color_slot(&pr, &unique) == 5);
    assert(feedback_color_slot(&pr, &missing) == -1);
    st.channel_mask = 0;
    assert(feedback_color_slot(&pr, &a) == 2);
    st.channel_mask = 0x0000f0; info.out_mask = 0;
    assert(feedback_color_slot(&pr, &a) == 2);
    info.out_mask = 0x000f00; st.channel_mask = 0x000f00;
    assert(feedback_color_slot(&pr, &a) == 2);
    puts("feedback alias selection: passed");
    return 0;
}
"""
compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("clang")
if not compiler:
    raise SystemExit("A C compiler is required (CC, cc or clang)")
with tempfile.TemporaryDirectory(prefix="feedback-alias-") as tmp:
    cfile, binary = Path(tmp) / "test.c", Path(tmp) / "test"
    cfile.write_text(fixture + helper + checks)
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", str(cfile), "-o", str(binary)], check=True)
    for mode in (None, "0", "1"):
        env = os.environ.copy()
        env.pop("SAVAGEWIND_VK_FEEDBACK_ALIAS_FIX", None)
        if mode is not None:
            env["SAVAGEWIND_VK_FEEDBACK_ALIAS_FIX"] = mode
        subprocess.run([str(binary)], env=env, check=True)
