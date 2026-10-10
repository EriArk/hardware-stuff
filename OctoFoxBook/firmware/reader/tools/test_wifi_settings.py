"""Real editor/input C++ and radio/credential lifecycle with fake ESP services."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class WifiSettingsTests(unittest.TestCase):
    def test_production_ui_navigation_and_retry(self):
        compiler = shutil.which('g++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / 'wifi-ui.exe'
            build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra',
                '-I', str(ROOT/'tools/wifi_host_stubs'), '-I', str(ROOT/'include'),
                str(ROOT/'tools/host_wifi_ui.cpp'), str(ROOT/'src/wifi_setup.cpp'),
                str(ROOT/'src/wifi_credentials.cpp'), str(ROOT/'src/bookish_ui.cpp'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, encoding='utf-8')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('WIFI_UI_OK', result.stdout)

    def test_radio_and_atomic_credential_lifecycle(self):
        compiler = shutil.which('g++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / 'wifi.exe'
            build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT/'tools/wifi_host_stubs'), '-I', str(ROOT/'include'),
                str(ROOT/'tools/host_wifi_setup.cpp'), str(ROOT/'src/wifi_setup.cpp'),
                str(ROOT/'src/wifi_credentials.cpp'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('WIFI_LIFECYCLE_OK', result.stdout)

    def test_keyboard_reachability_limits_and_editor_gestures(self):
        compiler = shutil.which('g++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / 'keyboard.exe'
            build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT/'include'), str(ROOT/'tools/host_text_keyboard.cpp'),
                '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('KEYBOARD_OK', result.stdout)

if __name__ == '__main__':
    unittest.main()
