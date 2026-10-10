from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class ReadingSyncTests(unittest.TestCase):
    def test_retry_after_lost_ack_keeps_newer_offline_edits_and_account_boundary(self):
        with tempfile.TemporaryDirectory() as folder:
            exe=Path(folder)/'reading-sync.exe'
            command=[shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror',
                *[part for path in ['tools/sync_host_stubs','tools/host_stubs','include','.pio/libdeps/t5_epaper_s3_fast/ArduinoJson/src'] for part in ['-I',str(ROOT/path)]],
                *[str(ROOT/path) for path in ['tools/host_reading_sync.cpp','src/storage_recovery.cpp','src/work_progress.cpp']],'-o',str(exe)]
            built=subprocess.run(command,capture_output=True,text=True)
            self.assertEqual(built.returncode,0,built.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
