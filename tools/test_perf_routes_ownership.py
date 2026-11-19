"""The benchmark must distinguish its own executable from a player's game."""
import unittest
from types import SimpleNamespace
from unittest.mock import patch
import perf_routes as routes


class OwnershipTests(unittest.TestCase):
    def setUp(self):
        self.previous = set(routes.MY_PROCESSES)
        routes.MY_PROCESSES.clear()
        routes.MY_PROCESSES.add(123)

    def tearDown(self):
        routes.MY_PROCESSES.clear()
        routes.MY_PROCESSES.update(self.previous)

    def test_ownership_uses_pid_even_with_an_external_executable(self):
        result = SimpleNamespace(returncode=0, stdout='123 X:\\other\\savagewind.exe\n456 X:\\test\\savagewind.exe\n')
        with patch.object(routes.os, 'name', 'nt'), patch.object(routes.subprocess, 'run', return_value=result) as run:
            self.assertEqual(routes.others('X:\\test'), ['456 X:\\test\\savagewind.exe'])
            self.assertEqual(run.call_args.args[0][0], routes.powershell())

    def test_reexec_requires_both_parent_and_exact_image(self):
        import json
        rows = [dict(ProcessId=124, ParentProcessId=123, ExecutablePath='/test/savagewind.exe'),
                dict(ProcessId=456, ParentProcessId=999, ExecutablePath='/test/savagewind.exe'),
                dict(ProcessId=789, ParentProcessId=123, ExecutablePath='/other/savagewind.exe')]
        with patch.object(routes.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout=json.dumps(rows))):
            self.assertEqual(routes.reexec_child(123, '/test/savagewind.exe'), 124)
            self.assertIsNone(routes.reexec_child(123, '/missing/savagewind.exe'))

    def test_failed_process_query_stops_the_benchmark(self):
        with patch.object(routes.os, 'name', 'nt'), patch.object(routes.subprocess, 'run', return_value=SimpleNamespace(returncode=1)):
            with self.assertRaises(RuntimeError):
                routes.others('X:\\test')


if __name__ == '__main__':
    unittest.main()
