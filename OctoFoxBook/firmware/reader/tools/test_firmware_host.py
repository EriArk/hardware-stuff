"""No-device regressions: actual C++ recovery and USB log assertions.

Run with Python unittest and a C++17 g++ on PATH. No firmware is flashed and
the fake SD stays in memory; all generated binaries live in a temporary folder.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import test_home_navigation
import test_local_library_sections
import test_opds_sections_search

TOOLS = Path(__file__).resolve().parent
FIRMWARE = TOOLS.parent


class FirmwareHostTests(unittest.TestCase):
    def test_actual_storage_recovery_and_progress_cpp(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 g++ required for the firmware host regression")
        with tempfile.TemporaryDirectory(prefix="abyss-host-tests-") as folder:
            binary = Path(folder) / "recovery-test.exe"
            subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I", str(TOOLS / "host_stubs"),
                "-I", str(FIRMWARE / "include"),
                str(TOOLS / "host_storage_recovery.cpp"),
                str(FIRMWARE / "src/storage_recovery.cpp"),
                str(FIRMWARE / "src/work_progress.cpp"), "-o", str(binary),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("recovery_scenarios=12", result.stdout)

    def test_cleanup_scheduling_is_not_an_extra_refresh(self):
        for module in (test_home_navigation, test_local_library_sections, test_opds_sections_search):
            with self.subTest(module=module.__name__):
                module.require_single_refresh([
                    "DISPLAY CLEAN SCHEDULED reason=screen-local-library",
                    "DISPLAY LIBRARY requested=quality-full applied=recovery-full ok=true",
                ], "clear-plus-draw")

    def test_duplicate_and_missing_draws_still_fail(self):
        draw = "DISPLAY LIBRARY requested=quality-full applied=recovery-full ok=true"
        for module in (test_home_navigation, test_local_library_sections, test_opds_sections_search):
            for lines in ([], [draw, draw], ["DISPLAY CLEAN SCHEDULED reason=test"]):
                with self.subTest(module=module.__name__, lines=lines):
                    with self.assertRaises(AssertionError):
                        module.require_single_refresh(lines, "invalid-draw-count")


if __name__ == "__main__":
    unittest.main()
