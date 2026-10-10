from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class CollectionsTests(unittest.TestCase):
    def test_migration_membership_atomic_retry_and_corrupt_file_preservation(self):
        with tempfile.TemporaryDirectory() as folder:
            exe=Path(folder)/'collections.exe'
            built=subprocess.run([shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror',
                '-I',str(ROOT/'tools/host_stubs'),'-I',str(ROOT/'include'),
                '-I',str(ROOT/'.pio/libdeps/t5_epaper_s3_fast/ArduinoJson/src'),
                str(ROOT/'tools/host_collections.cpp'),str(ROOT/'src/collection_store.cpp'),
                str(ROOT/'src/storage_recovery.cpp'),str(ROOT/'src/work_progress.cpp'),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(built.returncode,0,built.stderr)
            for arguments in ([],['corrupt']):
                result=subprocess.run([str(exe),*arguments],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
