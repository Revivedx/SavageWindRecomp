"""Recorder lifecycle regressions using the real C recorder, without game files or a GPU."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import swprof


class LifecycleTest(unittest.TestCase):
    def run_case(self, name):
        exe = Path(os.environ.get('SWPROF_TEST_BIN', 'build/test_prof_lifecycle')).resolve()
        with tempfile.TemporaryDirectory() as d:
            run = subprocess.run([str(exe), name], cwd=d, capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            paths = [line[5:] for line in run.stdout.splitlines() if line.startswith('FILE ')]
            if name == 'exit':
                paths = [str(p) for p in Path(d, 'profile').glob('*.swprof')]
            return swprof.Capture(str(Path(d) / paths[-1])) if paths else None

    def test_pending_signal_after_stop(self):
        self.run_case('pending')

    def test_sampler_skips_a_thread_reaped_after_snapshot(self):
        self.run_case('reaped-snapshot')

    def test_delayed_realtime_request_after_stop(self):
        self.run_case('pending-rt')

    def test_gpu_tail_is_recorded_after_cpu_stop(self):
        cap = self.run_case('gpu-tail')
        self.assertEqual(len(cap.gpu), 1)
        self.assertEqual(cap.counters['delayed GPU'][0][1], 1)

    def test_cancelled_environment_stop_cannot_stop_another_capture(self):
        self.assertTrue(self.run_case('cancel-env').frame_marks)

    def test_producers_racing_repeated_stop_and_start(self):
        cap = self.run_case('stress')
        self.assertTrue(cap.frame_marks)
        self.assertTrue(all(b >= a for zs in cap.zones for a, b, _ in zs))

    def test_normal_exit_finishes_the_file(self):
        self.assertTrue(self.run_case('exit').frame_marks)

    def test_old_scopes_cannot_enter_a_new_capture(self):
        cap = self.run_case('cross')
        old = {sid for sid, name in cap.sites.items() if name in ('old zone', 'old accumulator')}
        self.assertFalse([r for zs in cap.zones for r in zs if r[2] in old])
        self.assertFalse([r for acc in cap.acc for r in acc if r[1] in old])

    def test_single_frame_has_both_boundaries(self):
        cap = self.run_case('one')
        self.assertEqual([n for _, n in cap.frame_marks], [100, 101])
        self.assertEqual(swprof.analyse(cap)['frames']['count'], 1)

    def test_legacy_single_frame_alias_has_both_boundaries(self):
        self.assertEqual([n for _, n in self.run_case('alias').frame_marks], [100, 101])

    def test_final_totals_survive_worker_exit(self):
        cap = self.run_case('final')
        calls = sum(n for acc in cap.acc for _, sid, n, _ in acc if cap.sites[sid] == 'final total')
        self.assertEqual(calls, 1)

    def test_partial_accumulator_flush_preserves_unstored_totals_and_frame(self):
        cap = self.run_case('full-acc')
        totals = [(frame, cap.sites[sid], calls, round(ms * cap.ticks_per_ms))
                  for acc in cap.acc for frame, sid, calls, ms in acc]
        self.assertEqual(sorted(totals), [(10, 'pending tally', 1, 0), (10, 'pending total', 1, 222),
                                         (10, 'stored total', 1, 111)])

    def test_writer_failure_allows_a_later_capture(self):
        self.assertTrue(self.run_case('write-failure').frame_marks)
