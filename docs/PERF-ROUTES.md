# Performance routes

`tools/perf_routes.py` measures the same six views the same way every time, so a change can be judged in fps and
milliseconds against a baseline. It needs a game build with the control endpoint (`threads`, `gpuprof`, `memstats`,
`frametrace`; docs/CONTROL.md), Windows for the thread shares (the stack sampler; Linux uses CPU time per thread), and a
scratch copy of a save that is not the player's.

## The routes

Each route is its own boot. `SAVAGEWIND_TEST_WARP=<place>` (runtime/services/test_warp.c) rewrites where the save puts
Link, scripted input picks Continue, and the game loads straight into the place. A long `teleport` is not used: far from
the loaded area it leaves the game waiting on its loading spinner. Two routes then hop a short way, inside the loaded
area, and hover there with fly mode.

| route | place | view |
|---|---|---|
| plateau | Great Plateau, outside the Shrine of Resurrection | north-east over the grass to Hyrule Castle (yaw 150, pitch -3) |
| kakariko | Kakariko Village, horse gate | north into the village, NPCs (yaw 180, pitch -10) |
| field | Riverside Stable, Hyrule Field | north over the field (yaw 180, pitch -3) |
| rito | Rito Village, hovering 75 m south of the goddess statue | north at the village spire (yaw 180, pitch -8) |
| castle | above castle town, hovering 145 m south of the castle gate | north at Hyrule Castle (yaw 180, pitch 0) |
| hateno | Hateno Village, goddess statue | east over the village (yaw 90, pitch -8) |

Every route: hour 10:00, held (`time set 10`, `time freeze`), clear weather (`SAVAGEWIND_TEST_WEATHER=0`), camera set with
`look`, then 12 s to settle. Each run keeps a screenshot of its view next to its log, to check it is the same view.

## What is measured

1. 20 s with nothing else running: `frames` (average fps, frame time p99 and max; "1% low" is 1000 / p99), the
   `[vk] t=...` lines of `SAVAGEWIND_VK_PROF=2` (GPU time of the submissions per frame, compiles, `[hitch]` lines over
   34 ms) and `threads` (CPU time per thread), with `nvidia-smi` read once a second (GPU clock, load, power).
2. `gpuprof`: the GPU time per pass of one frame; the top three labels (same label summed) go in the CSV. Pass sizes in
   the labels are the game's own (1600x900 docked), before the runtime scales them.
3. 4 s of `sample` stack samples every 2 ms. Per thread they give the share of time working or waiting:
   - main thread (the game's first thread, handle 0x101): busy, waiting for the render thread (`queue_wait`), waiting
     for the GPU (`vk_wait_seq`, `renderer_sync`) or another wait (other game threads, sleeps, a free core);
   - render thread (the NVN queue thread, 0xf000: translation and command recording): busy, waiting for the GPU or idle.

4. Memory: `memstats` once a second through the timed window (a few tens of microseconds on the presenting thread):
   process RAM (working set / RSS: peak and median over the window; `ram_hwm_mb` is the process's high-water mark
   since boot), memory in use system-wide, GPU memory per heap (`VK_EXT_memory_budget`: the whole process's use of
   each heap; `gpu_local` the device-local heaps, `gpu_host` the others) and texture memory by kind at the end of the
   window: native ASTC, ASTC decoded to RGBA8 on the CPU (GPUs without ASTC; with the ASTC size it came from), BC,
   render targets (depth formats and the NVN compressible or display flag), other. `astc_ms` is the CPU time spent
   decoding ASTC in the window.
5. Hitches: `frametrace` records every frame of the window: its time and what happened in it (pipelines and shaders
   built, on any thread, and their ms; draws that waited for a pipeline and how long; upload and texture MB; ASTC
   decode ms; GPU waits; guest file reads; `vkAllocateMemory`; the present; page faults; CPU ms of the main, render and
   recorder threads). Each run keeps `<tag>_frames.csv`. A hitch is a frame over twice the window's median frame
   time; `<tag>_hitches.csv` lists them with a cause: the largest stall (pipeline wait, ASTC, GPU wait over the
   window's median wait, file reads, `vkAllocateMemory`, present over its median) if it explains a quarter of the
   frame's excess over the median or more; else `pipeline_build` if the frame's pipeline builds do (synchronous
   shaders, the default, build on the render thread inside the frame); else the thread whose CPU time rose most over
   its median (`main_cpu`: game logic; `render_cpu`: translation; `recorder_cpu`: Vulkan recording), by the same quarter
   rule; else `builds` (some built in that frame), `upload` (4 MB or more), `faults` (over 4x the median), or
   `unknown`. `table` prints a memory table and a hitch-cause table after the speed table; `rehitch <dir>` works the
   hitch columns out again from the kept traces.

`game_ms` and `render_ms` are those busy shares times the frame time. **limiter** is the largest of main busy, render
busy and GPU time as a share of the frame time ("none" under 80%). The stack samples slow the game a little, which is why
they come after the timed window. CPU time from `threads` is not used for the limiter: three game threads spin while
they wait (an SDK event thread, the audio render thread and one game thread), so they always show 100%.

## Running it

```
python tools/perf_routes.py run --save <scratch save folder with save_0> --ini <savagewind.ini to start from> \
    --out <dir> --size 1920x1080 2560x1440 --runs 3 [--config <label>] [--affinity 0xff] [--env VAR=value ...]
python tools/perf_routes.py table <dir> [<dir> ...]     markdown table: median and spread over the runs
python tools/perf_routes.py boot --save ... --out <dir> --place kakariko    start one place and leave it running
```

The settings are the given ini with the frame cap off, vsync off, frame generation off, the perf overlay off and the
window size of `--size`; the game runs hidden (`SAVAGEWIND_HIDDEN=1`) on port 7790. `--affinity` starts the game on
those logical processors only (the mask is set before any thread runs). Results append to `<dir>/results.csv`, one row
per run and route; each run's folder holds the log, the settings and the screenshot. A route whose game crashed or
lost its camera is tried once more, in a `<run>_attempt2` folder (the failed one keeps its log).

### On the Steam Deck (Linux)

The script runs on the Deck itself (its python3; it needs only `tools/perf_routes.py`, `swctl.py` and
`profile_samples.py`), started as a user systemd unit in the desktop session so it outlives the SSH session, under
`systemd-inhibit --what=sleep:idle`. It takes the game as an AppImage with a data root of its own, set up once from
the game files with `SAVAGEWIND_SETUP_FROM`:

```
python3 tools/perf_routes.py run --tree ~/sw-bench --exe ~/sw-bench/SavageWind-igpu.AppImage \
    --data-root ~/sw-bench/data --save ~/sw-bench/save-src --ini ~/sw-bench/deck-default.ini \
    --out ~/sw-bench/runs/deck --size 1280x800 --runs 3 --visible --fullscreen
```

There is no stack sampler on Linux: main and render busy ms are their CPU time per frame (`busy_from` = cpu; neither
thread spins while it waits). The GPU clock, load and power come from amdgpu's sysfs files. Before each boot the
script waits while any `savagewind` it did not start runs (the player's game). `--cache-reset <dir>` puts the data
root's `cache/` back to a copy of `<dir>/cache` before each boot and starts the driver's on-disk shader cache (Mesa)
from `<dir>/mesa`: a player's first boot after setup (copy both right after setup). `--visible` shows the window (a hidden window on a
Wayland desktop is not a fair measure); `--fullscreen` keeps fullscreen, as a Deck player has it.

Hidden runs get background GPU clocks while someone uses the desktop: compare interleaved runs, and look at the
`gpu_clock_mhz` column (about 1950 MHz on an RTX 3060 when nobody is at the PC). The `others` column counts other
`savagewind.exe` processes seen during the run; a run with any is not comparable.

## Steam Deck baseline (2026-11-01)

The iGPU reference: Steam Deck (RADV VANGOGH, 15 W APU, 16 GB shared), Desktop mode (KDE Plasma), plugged in, 0.4.1-alpha.4
plus the counters of this tool; fresh-install settings (upscaler and AA off, shadows console, FOV 100, synchronous
shaders), fullscreen 1280x800, uncapped; the recorder thread off (it does not start on Linux builds: glibc puts the
1.14 MB of static TLS, 1 MB of it `ms_guarded`'s buffer in `runtime/memscan_os.c`, inside its 1 MB stack, and
`pthread_create` returns EINVAL). Warm caches, 3 runs, median (min-max):

| route | fps | 1% low | p99 ms | main ms | render ms | GPU ms | GPU clock | limits |
|---|---|---|---|---|---|---|---|---|
| plateau | 51.4 (51.2-51.9) | 34.6 | 28.9 | 12.65 | 18.52 | 16.53 | 643 MHz | render 95%, GPU 85% |
| kakariko | 53.4 (52.8-53.4) | 43.4 | 23.1 | 13.97 | 17.99 | 16.79 | 672 MHz | render 96%, GPU 90% |
| field | 67.3 (67.0-68.4) | 52.8 | 18.9 | 12.12 | 13.88 | 13.28 | 792 MHz | render 93%, GPU 89% |
| rito | 39.6 (39.5-39.7) | 26.6 | 37.6 | 13.51 | 23.64 | 20.98 | 518 MHz | render 94%, GPU 83% |
| castle | 55.5 (55.5-55.6) | 44.1 | 22.7 | 12.98 | 17.50 | 15.82 | 681 MHz | render 97%, GPU 88% |
| hateno | 52.6 (52.4-52.9) | 41.5 | 24.1 | 12.96 | 18.36 | 16.81 | 660 MHz | render 97%, GPU 88% |

- The APU sits at its 15 W cap in every view; the GPU runs at 520-790 MHz (of 1600), 94-98% busy. CPU time saved is
  also GPU clock gained.
- Memory: RSS 1.0-1.2 GB, GPU device-local 777 MB, GPU host 3.14-3.19 GB (the 3120 MB guest heap as a dma-buf; the host
  heap is 3072 MB, so it is over budget), system 7.6-7.8 GB of 14.8 in use. Textures: BC 141-234 MB, render targets
  96-98 MB, other 56-58 MB, **ASTC 0 MB** (no ASTC texture in any view, so no CPU decoding).
- Hitches: none over twice the median in 24 warm windows. A first boot after setup (traced 45 s from the first world
  frame) has 4-16 per view: GPU waits after new pipelines (33 of 59, 1.5 s) and pipelines linked on the render thread
  (20, 1.4 s, up to 170 ms with 99-236 builds in one frame; 270-706 built in 45 s despite setup's precompile).

The full report (PC comparison, hitch lists, targets) is in the maintainers' Alpha 5 notes.
