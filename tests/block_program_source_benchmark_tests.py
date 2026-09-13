import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from block_program_source_benchmark import timed_process


class TimedProcessTests(unittest.TestCase):
    def test_blocking_wait_and_output(self):
        original = subprocess.Popen.wait
        waits = []

        def wait(process, *args, **kwargs):
            waits.append((args, kwargs))
            return original(process, *args, **kwargs)

        with tempfile.TemporaryFile(mode="w+") as output, mock.patch.object(subprocess.Popen, "wait", wait):
            elapsed = timed_process([sys.executable, "-c", "print('complete')"], env=os.environ, stdout=output)
            output.seek(0)
            self.assertEqual(output.read().strip(), "complete")
        self.assertGreater(elapsed, 0)
        self.assertTrue(waits)
        self.assertTrue(all(not args and not kwargs for args, kwargs in waits))

    def test_nonzero_exit(self):
        with self.assertRaises(subprocess.CalledProcessError) as error:
            timed_process([sys.executable, "-c", "raise SystemExit(7)"], env=os.environ,
                          stdout=subprocess.DEVNULL)
        self.assertEqual(error.exception.returncode, 7)

    def test_timeout_kills_and_reaps(self):
        processes = []
        original = subprocess.Popen

        def create(*args, **kwargs):
            process = original(*args, **kwargs)
            processes.append(process)
            return process

        with mock.patch.object(subprocess, "Popen", create), self.assertRaises(subprocess.TimeoutExpired):
            timed_process([sys.executable, "-c", "import time; time.sleep(60)"], env=os.environ,
                          stdout=subprocess.DEVNULL, timeout=0.05)
        self.assertEqual(len(processes), 1)
        self.assertIsNotNone(processes[0].returncode)

    def test_interruption_kills_and_reaps(self):
        original = subprocess.Popen.wait
        interrupted = []

        def wait(process, *args, **kwargs):
            if not interrupted:
                interrupted.append(process)
                raise KeyboardInterrupt()
            return original(process, *args, **kwargs)

        with mock.patch.object(subprocess.Popen, "wait", wait), self.assertRaises(KeyboardInterrupt):
            timed_process([sys.executable, "-c", "import time; time.sleep(60)"], env=os.environ,
                          stdout=subprocess.DEVNULL)
        self.assertIsNotNone(interrupted[0].returncode)


if __name__ == "__main__":
    unittest.main()
