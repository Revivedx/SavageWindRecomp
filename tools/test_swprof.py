"""Tests of tools/swprof.py on a capture built by hand. `make test-prof` or `python3 -m unittest tools.test_swprof`."""
import contextlib
import csv
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(__file__))
import swprof  # noqa: E402

TPM = 1000  # ticks per ms: the header's two clocks run 1000 ticks per 1e6 ns


def build(sites, threads, records, modules=()):
    """sites {id: name}; threads [name]; records [(thread, t0, t1, val, site, kind)] in ticks;
    modules [(base, size, bias, kind, name)]."""
    body = b''
    for sid, name in sites.items():
        n = name.encode()
        body += struct.pack('<IH', sid, len(n)) + n
    for i, name in enumerate(threads):
        body += swprof.THREAD.pack(i, 0, 100 + i, name.encode())
    for base, size, bias, kind, name in modules:
        body += swprof.MODULE.pack(base, size, bias, kind, name.encode())
    by_thread = {}
    for r in records:
        by_thread.setdefault(r[0], []).append(r)
    chunks = b''
    for th, recs in by_thread.items():
        chunks += swprof.CHUNK.pack(th, len(recs))
        for _, a, b, val, site, kind in recs:
            chunks += swprof.REC.pack(a, b, val, site, kind)
    end_ticks = max(r[2] for r in records) + 10
    header = swprof.HEADER.pack(b'SWPROF01', 1, swprof.REC.size, 0, 0, end_ticks, end_ticks * 1000000 // TPM,
                                1700000000, len(sites), len(threads), len(by_thread), len(modules))
    return header + body + chunks


class SwprofTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        sites = {1: 'render', 2: 'draw', 3: 'exec', 4: 'exec/pre', 5: 'queue', 6: 'pass 1920x1080', 7: '(gpu idle)'}
        recs = []
        # 10 frames of 10 ms, one of them (frame 5) 60 ms because "draw" takes 50 ms in it
        t = 0
        for f in range(11):
            recs.append((0, t * TPM, 0, f, 0, swprof.FRAME))
            dur = 60 if f == 5 else 10
            if f < 10:
                recs.append((0, t * TPM, (t + dur) * TPM, 0, 1, swprof.ZONE))
                inner = 50 if f == 5 else 4
                recs.append((0, (t + 1) * TPM, (t + 1 + inner) * TPM, 0, 2, swprof.ZONE))
                recs.append((1, t * TPM, 7, 0, 5, swprof.COUNTER))
                # 100 calls of exec, 2 ms in total, 1.5 ms of it in exec/pre
                recs.append((1, 0, 2 * TPM, f, 3, swprof.ACC | (100 << 8)))
                recs.append((1, 0, int(1.5 * TPM), f, 4, swprof.ACC | (100 << 8)))
                # the GPU: a pass of 3 ms (20 draws) and 1 ms idle
                recs.append((1, (t + 2) * TPM, (t + 5) * TPM, 20 | (f << 32), 6, swprof.GPU))
                recs.append((1, (t + 5) * TPM, (t + 6) * TPM, f << 32, 7, swprof.GPU))
            t += dur
        self.path = os.path.join(self.dir.name, 'x.swprof')
        with open(self.path, 'wb') as fh:
            fh.write(build(sites, ['main', 'render'], recs))

    def test_reads_the_file(self):
        cap = swprof.Capture(self.path)
        self.assertEqual([n for n, _ in cap.threads], ['main', 'render', 'gpu'])
        self.assertEqual(len(cap.frame_marks), 11)
        self.assertAlmostEqual(cap.ticks_per_ms, TPM, delta=1)
        self.assertEqual(len(cap.zones[0]), 20)

    def test_frames_and_slow_frame_cause(self):
        res = swprof.analyse(swprof.Capture(self.path))
        f = res['frames']
        self.assertEqual(f['count'], 10)
        self.assertAlmostEqual(f['ms_max'], 60, delta=0.1)
        self.assertAlmostEqual(f['ms_p50'], 10, delta=0.1)
        self.assertEqual(f['over_50_ms'], 1)
        slow = res['slow_frames'][0]
        self.assertEqual(slow['frame'], 5)
        self.assertEqual(slow['top'][0]['name'], 'render/draw')
        self.assertAlmostEqual(slow['top'][0]['ms'], 50, delta=0.1)

    def test_slow_frame_excludes_waits_and_keeps_sample_split(self):
        sites = {1: 'worker', 2: 'guest sleep', 3: 'kernel wait', 4: 'real work'}
        recs = [(0, 0, 0, 0, 0, swprof.FRAME),
                (0, 100 * TPM, 0, 1, 0, swprof.FRAME),
                (0, 0, 100 * TPM, 0, 1, swprof.ZONE),
                (0, 0, 90 * TPM, 0, 2, swprof.ZONE),
                (0, 90 * TPM, 100 * TPM, 0, 4, swprof.ZONE),
                (1, 0, 100 * TPM, 0, 3, swprof.ZONE),
                (2, TPM, 100, 0, 0, swprof.SAMPLE | (1 << 8)),
                (2, 0, 0, 0, 0, swprof.STACK)]
        with open(self.path, 'wb') as fh:
            fh.write(build(sites, ['main', 'waiting worker', 'sampler'], recs))
        res = swprof.analyse(swprof.Capture(self.path))
        slow = res['slow_frames'][0]
        self.assertEqual(slow['top'][0]['name'], 'worker/real work')
        self.assertAlmostEqual(slow['top'][0]['ms'], 10)
        self.assertFalse({r['name'].rsplit('/', 1)[-1] for r in slow['top']} & swprof.WAIT_ZONES)
        report = swprof.text_report(res, 10, 33.4).split('Slowest frames', 1)[1]
        self.assertIn('worker/real work', report)
        self.assertNotIn('guest sleep', report)
        self.assertNotIn('kernel wait', report)
        self.assertIn('samples: waiting 1', report)

    def test_zone_self_time_and_acc(self):
        res = swprof.analyse(swprof.Capture(self.path))
        main = next(t for t in res['threads'] if t['name'] == 'main')
        render = next(z for z in main['zones'] if z['path'] == 'render')
        draw = next(z for z in main['zones'] if z['path'] == 'render/draw')
        self.assertAlmostEqual(render['incl_ms'], (9 * 10 + 60) / 10, delta=0.01)
        self.assertAlmostEqual(draw['incl_ms'], (9 * 4 + 50) / 10, delta=0.01)
        self.assertAlmostEqual(render['self_ms'], render['incl_ms'] - draw['incl_ms'], delta=0.01)
        rt = next(t for t in res['threads'] if t['name'] == 'render')
        ex = next(a for a in rt['acc'] if a['path'] == 'exec')
        pre = next(a for a in rt['acc'] if a['path'] == 'exec/pre')
        self.assertAlmostEqual(ex['incl_ms'], 2.0, delta=0.01)
        self.assertAlmostEqual(ex['self_ms'], 0.5, delta=0.01)
        self.assertAlmostEqual(pre['calls_per_frame'], 100, delta=0.1)
        top = {(t['thread'], t['name']): t['self_ms_per_frame'] for t in res['top_self']}
        self.assertAlmostEqual(top[('render', 'exec/pre')], 1.5, delta=0.01)

    def accumulator_capture(self):
        sites = {1: 'backend_execute', 2: 'exec', 3: 'exec/pre', 4: 'draw', 5: 'other thread'}
        recs = [(0, i * 10 * TPM, 0, 40 + i, 0, swprof.FRAME) for i in range(3)]
        recs += [(0, 0, 10 * TPM, 0, 1, swprof.ZONE),
                 (0, 10 * TPM, 20 * TPM, 0, 1, swprof.ZONE),
                 (1, 0, 20 * TPM, 0, 5, swprof.ZONE),
                 # Flush timestamps deliberately lie in a later frame: ACC.val owns attribution.
                 (0, 20 * TPM, 4 * TPM, 40, 2, swprof.ACC | (2 << 8)),
                 (0, 20 * TPM, 1 * TPM, 40, 3, swprof.ACC | (2 << 8)),
                 (0, 20 * TPM, 6 * TPM, 40, 4, swprof.ACC | (3 << 8)),
                 (0, 20 * TPM, 2 * TPM, 41, 2, swprof.ACC | (1 << 8)),
                 (0, 20 * TPM, 3 * TPM, 41, 4, swprof.ACC | (1 << 8))]
        with open(self.path, 'wb') as fh:
            fh.write(build(sites, ['render', 'worker'], recs))
        return swprof.Capture(self.path)

    def test_accumulator_work_is_removed_from_zone_self_time_per_thread_and_frame(self):
        cap = self.accumulator_capture()
        res = swprof.analyse(cap)
        render = res['threads'][0]
        self.assertAlmostEqual(render['zones'][0]['incl_ms'], 10)
        self.assertAlmostEqual(render['zones'][0]['self_ms'], 2.5)
        top = {(r['thread'], r['name']): r['self_ms_per_frame'] for r in res['top_self']}
        self.assertAlmostEqual(sum(v for (t, _), v in top.items() if t == 'render'), 10)
        self.assertAlmostEqual(top[('worker', 'other thread')], 10)
        self.assertAlmostEqual(res['_per_frame_self'][0][('render', 'backend_execute')], 0)
        self.assertAlmostEqual(res['_per_frame_self'][1][('render', 'backend_execute')], 5)
        selected = swprof.analyse(cap, 41, 41)
        self.assertAlmostEqual(selected['threads'][0]['zones'][0]['self_ms'], 5)

    def test_accumulator_deduction_is_capped_at_zone_self_time(self):
        cap = self.accumulator_capture()
        cap.acc[0].append((40, 4, 1, 20))
        res = swprof.analyse(cap, 40, 40)
        self.assertEqual(res['threads'][0]['zones'][0]['self_ms'], 0)
        self.assertTrue(all(r['self_ms_per_frame'] >= 0 for r in res['top_self']))

    def test_accumulator_totals_reach_frame_attribution_and_csv(self):
        cap = self.accumulator_capture()
        res = swprof.analyse(cap)
        per = res['_per_frame_self']
        for i, expected in enumerate(((3, 1, 6), (2, 0, 3))):
            for name, ms in zip(('exec', 'exec/pre', 'draw'), expected):
                self.assertAlmostEqual(per[i].get(('render', name), 0), ms)
            self.assertAlmostEqual(sum(ms for (t, _), ms in per[i].items() if t == 'render'), 10)
        slow = next(r for r in res['slow_frames'] if r['frame'] == 40)
        self.assertEqual(slow['top'][1], {'thread': 'render', 'name': 'draw', 'ms': 6})
        path = os.path.join(self.dir.name, 'acc.csv')
        swprof.write_csv(res, path)
        with open(path) as fh:
            rows = list(csv.DictReader(fh))
        self.assertEqual([r['render:draw'] for r in rows], ['6.000', '3.000'])
        self.assertEqual([r['render:exec/pre'] for r in rows], ['1.000', '0.000'])
        selected = swprof.analyse(cap, 41, 41)
        swprof.write_csv(selected, path)
        with open(path) as fh:
            rows = list(csv.DictReader(fh))
        self.assertEqual(len(rows), 1)
        self.assertEqual((rows[0]['frame'], rows[0]['render:draw']), ('41', '3.000'))

    def test_accumulator_without_zones_reaches_csv(self):
        res = swprof.analyse(swprof.Capture(self.path))
        path = os.path.join(self.dir.name, 'acc-only.csv')
        swprof.write_csv(res, path)
        with open(path) as fh:
            rows = list(csv.DictReader(fh))
        self.assertEqual([r['render:exec/pre'] for r in rows], ['1.500'] * 10)
        self.assertEqual([r['render:exec'] for r in rows], ['0.500'] * 10)

    def test_gpu(self):
        res = swprof.analyse(swprof.Capture(self.path))
        g = res['gpu']
        self.assertAlmostEqual(g['busy_ms_per_frame'], 3.0, delta=0.01)
        self.assertAlmostEqual(g['idle_ms_per_frame'], 1.0, delta=0.01)
        self.assertAlmostEqual(g['busy_percent'], 75.0, delta=0.1)
        self.assertEqual(g['rows'][0]['name'], 'pass 1920x1080')
        self.assertAlmostEqual(g['rows'][0]['draws_per_frame'], 20, delta=0.01)
        self.assertTrue(any(t['name'] == 'gpu' for t in res['threads']))

    def test_gpu_clock_metadata_reports_estimated_alignment(self):
        cap = self.crossing_capture()
        cap.counters['GPU clock calibrated'] = [(5, 0)]
        self.assertEqual(swprof.analyse(cap)['gpu']['alignment'], 'estimated')

    def test_gpu_clock_metadata_reports_calibration_uncertainty(self):
        cap = self.crossing_capture()
        cap.counters['GPU clock calibrated'] = [(5, 1)]
        cap.counters['GPU clock deviation ns'] = [(5, 1234)]
        gpu = swprof.analyse(cap)['gpu']
        self.assertEqual(gpu['alignment'], 'calibrated')
        self.assertEqual(gpu['max_deviation_ns'], 1234)

    def test_counters(self):
        res = swprof.analyse(swprof.Capture(self.path))
        self.assertEqual(res['counters']['queue']['max'], 7)

    def test_frame_range(self):
        res = swprof.analyse(swprof.Capture(self.path), 6, 9)
        self.assertEqual(res['frames']['count'], 4)
        self.assertAlmostEqual(res['frames']['ms_max'], 10, delta=0.1)

    def test_cli_outputs(self):
        js, csv = os.path.join(self.dir.name, 'o.json'), os.path.join(self.dir.name, 'o.csv')
        with contextlib.redirect_stdout(io.StringIO()):
            swprof.main([self.path, '--json', js, '--csv', csv, '--top', '3'])
        with open(js) as fh:
            data = json.load(fh)
        self.assertEqual(data['frames']['count'], 10)
        with open(csv) as fh:
            lines = fh.read().splitlines()
        self.assertEqual(len(lines), 11)
        self.assertTrue(lines[0].startswith('frame,ms,'))

    def test_rejects_other_files(self):
        bad = os.path.join(self.dir.name, 'bad')
        with open(bad, 'wb') as fh:
            fh.write(b'\0' * 200)
        with self.assertRaises(SystemExit):
            swprof.Capture(bad)

    def test_rejects_truncated_chunk_payload_at_record_and_byte_boundaries(self):
        with open(self.path, 'rb') as fh:
            data = fh.read()
        for missing in (swprof.REC.size, 1):
            with self.subTest(missing=missing):
                with open(self.path, 'wb') as fh:
                    fh.write(data[:-missing])
                with self.assertRaisesRegex(SystemExit, r'truncated chunk .*expected .*bytes.*found .*bytes'):
                    swprof.Capture(self.path)

    def test_rejects_truncated_chunk_header(self):
        data = build({}, ['main'], [(0, 0, 0, 0, 0, swprof.FRAME)])
        with open(self.path, 'wb') as fh:
            fh.write(data[:-swprof.REC.size - 1])
        with self.assertRaisesRegex(SystemExit, 'truncated chunk .*header'):
            swprof.Capture(self.path)

    def test_truncated_capture_cli_fails_without_reports(self):
        with open(self.path, 'rb') as fh:
            data = fh.read()
        with open(self.path, 'wb') as fh:
            fh.write(data[:-swprof.REC.size])
        js, csv_path = os.path.join(self.dir.name, 'bad.json'), os.path.join(self.dir.name, 'bad.csv')
        run = subprocess.run([sys.executable, swprof.__file__, self.path, '--json', js, '--csv', csv_path],
                             capture_output=True, text=True, timeout=10)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('truncated chunk', run.stderr)
        self.assertNotIn('Traceback', run.stderr)
        self.assertEqual(run.stdout, '')
        self.assertFalse(os.path.exists(js))
        self.assertFalse(os.path.exists(csv_path))

    def crossing_capture(self):
        recs = [(0, i * TPM, 0, i // 10, 0, swprof.FRAME) for i in range(0, 41, 10)]
        recs += [(0, 5 * TPM, 35 * TPM, 0, 1, swprof.ZONE),
                 (0, 12 * TPM, 18 * TPM, 0, 2, swprof.ZONE),
                 (0, 5 * TPM, 35 * TPM, 30, 3, swprof.GPU)]
        with open(self.path, 'wb') as f:
            f.write(build({1: 'outer', 2: 'inner', 3: 'pass'}, ['main'], recs))
        return swprof.Capture(self.path)

    def test_crossing_zone_is_clipped_to_selected_frames(self):
        res = swprof.analyse(self.crossing_capture(), 1, 2)
        zones = {z['path']: z for z in res['threads'][0]['zones']}
        self.assertAlmostEqual(zones['outer']['incl_ms'], 10)
        self.assertAlmostEqual(zones['outer']['self_ms'], 7)
        self.assertAlmostEqual(zones['outer/inner']['self_ms'], 3)

    def test_self_time_is_split_at_frame_and_child_boundaries(self):
        res = swprof.analyse(self.crossing_capture())
        rows = res['_per_frame_self']
        for i, expected in enumerate((5, 4, 10, 5)):
            self.assertAlmostEqual(rows[i][('main', 'outer')], expected)
        self.assertAlmostEqual(rows[1][('main', 'outer/inner')], 6)

    def test_crossing_gpu_interval_is_clipped(self):
        gpu = swprof.analyse(self.crossing_capture(), 1, 2)['gpu']
        self.assertAlmostEqual(gpu['busy_ms_per_frame'], 10)

    def test_selection_before_a_nested_child(self):
        res = swprof.analyse(self.crossing_capture(), 0, 0)
        zones = {z['path']: z for z in res['threads'][0]['zones']}
        self.assertAlmostEqual(zones['outer']['self_ms'], 5)

    def test_selection_after_a_nested_child(self):
        res = swprof.analyse(self.crossing_capture(), 3, 3)
        zones = {z['path']: z for z in res['threads'][0]['zones']}
        self.assertAlmostEqual(zones['outer']['self_ms'], 5)

    def test_empty_frame_selection_does_not_fall_back_to_whole_capture(self):
        res = swprof.analyse(self.crossing_capture(), 100, 200)
        self.assertFalse(res['top_self'])
        self.assertFalse(res['gpu'].get('rows'))


GAME, RUNTIME, OTHER = 0x10000000, 0x20000000, 0x30000000
MODULES = [(GAME, 0x1000, GAME, swprof.MOD_GAME, 'game.dll'), (RUNTIME, 0x1000, RUNTIME, swprof.MOD_RUNTIME, 'swr.exe'),
           (OTHER, 0x1000, OTHER, swprof.MOD_OTHER, 'driver.dll')]


class SamplerTest(unittest.TestCase):
    """100 ms of one thread ("main", os id 100), a sample every ms: 60 in game code, 10 in the runtime, 10 in the driver
    called from the runtime, and 20 inside a "kernel wait" zone (those would be in game code otherwise)."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        sites = {1: 'kernel wait'}
        recs = [(0, 0, 0, 0, 0, swprof.FRAME), (0, 100 * TPM, 0, 1, 0, swprof.FRAME),
                (0, 80 * TPM, 100 * TPM, 0, 1, swprof.ZONE)]
        for ms in range(100):
            if ms < 60 or ms >= 80:
                stack = [GAME + 0x10, GAME + 0x20, RUNTIME + 0x300]
            elif ms < 70:
                stack = [RUNTIME + 0x100, GAME + 0x20]
            else:
                stack = [OTHER + 0x5, RUNTIME + 0x200, GAME + 0x20]
            recs.append((1, ms * TPM, 100, 7, 0, swprof.SAMPLE | (len(stack) << 8)))
            stack += [0] * (-len(stack) % 3)
            for i in range(0, len(stack), 3):
                recs.append((1, stack[i], stack[i + 1], stack[i + 2], 0, swprof.STACK))
        self.path = os.path.join(self.dir.name, 's.swprof')
        with open(self.path, 'wb') as fh:
            fh.write(build(sites, ['main', 'sampler'], recs, MODULES))

    def test_reads_modules_and_samples(self):
        cap = swprof.Capture(self.path)
        self.assertEqual([m[3] for m in cap.modules], [swprof.MOD_GAME, swprof.MOD_RUNTIME, swprof.MOD_OTHER])
        self.assertEqual(len(cap.samples), 100)
        self.assertEqual(cap.samples[0][3], [GAME + 0x10, GAME + 0x20, RUNTIME + 0x300])
        self.assertEqual(cap.samples[65][3], [RUNTIME + 0x100, GAME + 0x20])  # (padding is cut off)

    def test_split_and_runtime_functions(self):
        res = swprof.analyse(swprof.Capture(self.path))
        sp = res['sampler']
        self.assertEqual(sp['samples'], 100)
        self.assertAlmostEqual(sp['interval_ms'], 1.0, delta=0.01)
        main = next(t for t in sp['threads'] if t['name'] == 'main')
        self.assertEqual((main['game'], main['runtime'], main['driver/system'], main['waiting']), (60.0, 10.0, 10.0, 20.0))
        self.assertEqual(sp['runtime_self'][0]['name'], 'runtime+0x100')
        self.assertEqual(sp['runtime_callers_of_driver'][0]['name'], 'runtime+0x200')
        incl = {r['name']: r['samples'] for r in sp['runtime_inclusive']}
        self.assertEqual(incl, {'runtime+0x100': 10, 'runtime+0x200': 10, 'runtime+0x300': 60})  # (waiting is not counted)
        self.assertIn('no runtime binary', sp['symbols_note'])

    def test_report_mentions_the_note_and_no_game_address(self):
        res = swprof.analyse(swprof.Capture(self.path))
        text = swprof.text_report(res, 10, 33.4)
        self.assertIn('Do not put game addresses', text)
        self.assertNotIn(hex(GAME)[2:], text)

    def test_wait_found_by_the_stack(self):
        # the driver samples sit under a runtime wait function with no zone around them: waiting, not driver/system
        exe = os.path.join(self.dir.name, 'swr')
        with open(exe, 'wb') as fh:
            fh.write(b'\x7fELF' + b'\0' * 100)
        done = mock.Mock(stdout='0000000000000100 T render_frame\n0000000000000200 t kwait\n')
        with mock.patch.object(swprof.shutil, 'which', return_value='/usr/bin/nm'), \
                mock.patch.object(swprof.subprocess, 'run', return_value=done):
            res = swprof.analyse(swprof.Capture(self.path), exe=exe)
        sp = res['sampler']
        main = next(t for t in sp['threads'] if t['name'] == 'main')
        self.assertEqual((main['game'], main['runtime'], main['driver/system'], main['waiting']), (60.0, 10.0, 0.0, 30.0))
        self.assertEqual([(r['name'], r['samples']) for r in sp['runtime_waits']], [('kwait', 10)])
        self.assertEqual(sp['runtime_callers_of_driver'], [])

    def test_vulkan_semaphore_wait_found_by_stack_without_a_completed_zone(self):
        exe = os.path.join(self.dir.name, 'swr')
        with open(exe, 'wb') as fh:
            fh.write(b'\x7fELF' + b'\0' * 100)
        done = mock.Mock(stdout='0000000000000100 T render_frame\n0000000000000200 T vk_wait_seq\n')
        with mock.patch.object(swprof.shutil, 'which', return_value='/usr/bin/nm'), \
                mock.patch.object(swprof.subprocess, 'run', return_value=done):
            res = swprof.analyse(swprof.Capture(self.path), exe=exe)
        main = next(t for t in res['sampler']['threads'] if t['name'] == 'main')
        self.assertEqual((main['driver/system'], main['waiting']), (0, 30))
        self.assertEqual(res['sampler']['runtime_callers_of_driver'], [])
        self.assertEqual(res['sampler']['runtime_waits'][0]['name'], 'vk_wait_seq')
        self.assertEqual(res['slow_frames'][0]['samples']['waiting'], 30)

    def test_gpu_wait_zone_classifies_single_pc_samples_without_symbols(self):
        cap = swprof.Capture(self.path)
        cap.sites[1] = 'gpu_wait'
        cap.samples = [(90, 100, 7, [OTHER + 5])]
        res = swprof.analyse(cap)
        main = res['sampler']['threads'][0]
        self.assertEqual((main['driver/system'], main['waiting']), (0, 100))
        self.assertFalse(res['top_self'])
        self.assertEqual(res['top_waits'][0]['name'], 'gpu_wait')

    def test_symbols(self):
        exe = os.path.join(self.dir.name, 'swr')
        with open(exe, 'wb') as fh:
            fh.write(b'\x7fELF' + b'\0' * 100)
        nm = '0000000000000100 T render_frame\n0000000000000200 t helper\n0000000000000300 T wait_thing\n0000000000000400 D data\n'
        done = mock.Mock(stdout=nm)
        with mock.patch.object(swprof.shutil, 'which', return_value='/usr/bin/nm'), \
                mock.patch.object(swprof.subprocess, 'run', return_value=done):
            sym = swprof.Symbols(exe, MODULES[1])
        self.assertEqual(sym.name(RUNTIME + 0x150), 'render_frame')
        self.assertEqual(sym.name(RUNTIME + 0x200), 'helper')
        self.assertEqual(sym.name(RUNTIME + 0x300 + (1 << 21)), 'runtime+0x%x' % (0x300 + (1 << 21)))


if __name__ == '__main__':
    unittest.main()
