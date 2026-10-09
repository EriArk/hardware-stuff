"""Exclusive physical OK gesture recognition, including hold and timer rollover."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
class OkGestureTests(unittest.TestCase):
    def test_cpp_gesture_sequences(self):
        compiler = shutil.which('g++')
        self.assertIsNotNone(compiler, 'C++17 compiler required')
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / 'gestures.exe'
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'include'), str(ROOT / 'tools/host_ok_gestures.cpp'),
                            '-o', str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn('OK_GESTURES_OK', result.stdout)
