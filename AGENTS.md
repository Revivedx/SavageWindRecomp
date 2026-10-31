Savage Wind Recomp

Read CONTRIBUTING.md before making changes.

━━ Project map

Savage Wind statically recompiles Switch BOTW into a locally generated swgame
library loaded by the host runtime. Windows and Linux/Vulkan are the maintained
targets; macOS/Metal is retained but unmaintained.

• tools/: NSO loading, module layout, code generation, instruction lifting,
  and guest patches.
• runtime/: host OS replacement, HLE, platform code, settings, and UI.
• runtime/nvn/: graphics objects, Maxwell shader translation, and Vulkan
  rendering.
• tools/test_*, tests/, and Makefile test-* targets: subsystem tests.
• Build guides: docs/linux.md, docs/BUILD-windows.md. docs/SAVAGE-WIND.md
  contains debugging details; historical publication commands are not the normal
  PR workflow.

━━ Code and data rules

• Never commit or upload Nintendo files, keys, firmware, game assets/fonts,
  saves, extracted files, generated game code, or disassembly/decompilation
  output. Do not copy from decompilation projects. Use synthetic test fixtures.

• Preserve unrelated edits and the user's local game files, settings, and
  saves. If unsure whether an operation could overwrite, delete, or expose game
  data, keys, or extracted files, stop and ask before proceeding. Keep
  machine-specific paths and secrets out of commits.

• Follow the nearest function's indentation, naming, and error handling.
  Style examples to copy:

    settings_load in runtime/settings.c (C)
    _surf_contact in tools/patches.py (Python)

• Keep changes focused; fix generator/patch sources rather than editing
  generated/.

• Add version-aware patterns in tools/game_sites.py, assert replaced
  instructions in tools/patches.py, and use g_site for runtime addresses. Keep
  version tables and optional groups consistent. Primary target: 1.9.0.

• Preserve guest FP/SIMD semantics and calling conventions. Do not change
  optimization or floating-point flags incidentally.

• Reset per-boot state with RT_ON_RESET (runtime/hle.h); do not retain guest
  pointers across reboots.

• Timing fixes must use the appropriate time step. Distinguish game updates,
  presented frames, and generated frames; compare 30/60/90/120 FPS where
  relevant.

• For Vulkan changes, read the synchronization model in:

    runtime/nvn/vulkan/vk.h

  Deferred destruction is implemented in:

    runtime/nvn/vulkan/textures.c

  Use vk_defer_image/vk_defer_view for in-flight resources; do not destroy them
  while submissions can reference them.

• For player-facing text changes, update the translation tables and run the
  UI-text check listed in Build and verify.

━━ Build and verify

Use the existing Python environment when available (tools/venv needs lz4,
capstone, and unicorn). From the repository root:

    make -j8             # if OOM-killed, retry with make -j4
    make runtime-objs    # compile runtime/backend/UI objects without linking
    make gen             # regenerate from the user's own extracted files

After generator, lifter, patch, or site changes, regenerate and rebuild before
testing gameplay. Without generated sources, make builds only the app.

For stale builds, find the loaded path in the startup log's [gamelib] line and
read its adjacent <library>.id file. Compare it with the current Makefile ID,
using the same build overrides:

    make -s --eval='print-gamelib-id:;@echo $(GAMELIB_ID)' print-gamelib-id

If the log says target: compatible, print GAMELIB_ID_COMPAT instead. Regenerate
and rebuild on a mismatch; do not edit the ID file to bypass the check. Loading
and validation are implemented in:

    runtime/gamelib.c

Run checks relevant to the change; inspect the Makefile and
.github/workflows/ for recipes and dependencies:

    tools/venv/bin/python tools/test_game_sites.py      # sites/versions
    tools/venv/bin/python tools/test_layout.py          # generator/layout
    tools/venv/bin/python tools/test_simd.py --jobs 8   # FP/SIMD
    python3 tools/i18n_check.py                         # UI text

For other subsystems, use neighboring test scripts and Makefile test targets.
For Linux gameplay tests with scratch saves and settings, use:

    tools/linux/test_run.sh

Record the game version, FPS, GPU/driver, settings, and observed result.
Separate hypotheses, synthetic checks, builds, and verified gameplay.

For documentation-only changes, check paths and links, run the command below,
and skip the build:

    git diff --check

━━ Commits and PRs

• Base focused topic branches and PRs on current upstream test; main is for
  releases.

• Review the staged diff and branch changes before publishing. Keep unrelated
  work out and preserve attribution.

• Follow .github/pull_request_template.md: describe the change and actual
  validation; link resolved issues with Fixes #N.

• Before resolving conflicts, search upstream history and inspect the patch.
  Substitute the actual upstream remote:

    git log origin/test --grep='keyword'

  Do not reapply an integrated fix.

• When authorized, merge each PR separately through GitHub and verify its
  status and linked issue closure. Closing keywords targeting test may not close
  issues automatically.

• When authorized and applying manually, credit the contributor and close the
  PR with a link to the integration commit.

• For authorized history rewrites, use --force-with-lease against the expected
  remote SHA; stop if the lease fails.

• Opening a PR does not authorize merging it, closing PRs or issues, or
  publishing a release.
