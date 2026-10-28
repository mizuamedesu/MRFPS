"""Portable checks for macOS memory reports; no compiler or app is launched."""
import pathlib
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import mac_resource_guard as guard


class MemoryReportTests(unittest.TestCase):
    def test_apple_silicon_and_intel_page_sizes(self):
        for page in (4096, 16384):
            report = f"""Mach Virtual Memory Statistics: (page size of {page} bytes)
Pages free:                               100.
Pages active:                             999.
Pages inactive:                           200.
Pages speculative:                         50.
Pages wired down:                         300.
Pages occupied by compressor:             400.
"""
            with self.subTest(page=page), patch.object(guard.subprocess, "check_output", return_value=report):
                self.assertEqual(guard.available_memory(), 350*page)

    def test_missing_measurements_fail_closed(self):
        for report in ("", "Mach Virtual Memory Statistics: (page size of 16384 bytes)\nPages free: 100."):
            with self.subTest(report=report), patch.object(guard.subprocess, "check_output", return_value=report):
                with self.assertRaises(RuntimeError):
                    guard.available_memory()

    def test_invalid_reserve_never_starts_a_child(self):
        for value in ("nan", "inf", "0", "-1", "16"):
            with self.subTest(value=value), patch.object(guard.sys, "platform", "darwin"), \
                 patch.object(guard.sys, "argv", ["guard", "--reserve-gb", value, "--", "compiler"]), \
                 patch.object(guard.subprocess, "check_output", return_value=str(16*2**30)), \
                 patch.object(guard, "available_memory", return_value=12*2**30), \
                 patch.object(guard.subprocess, "Popen") as launch:
                with self.assertRaises(RuntimeError):
                    guard.main()
                launch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
