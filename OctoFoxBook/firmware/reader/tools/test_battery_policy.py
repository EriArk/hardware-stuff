from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class BatteryTests(unittest.TestCase):
    def test_shutdown_waits_for_writers_flushes_display_and_restores_context(self):
        with tempfile.TemporaryDirectory() as folder:
            exe=Path(folder)/'shutdown.exe'
            subprocess.run([shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror',
                '-I',str(ROOT/'tools/wifi_host_stubs'),str(ROOT/'tools/host_battery_shutdown.cpp'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

    def test_thresholds_sag_hysteresis_and_invalid_adc(self):
        with tempfile.TemporaryDirectory() as folder:
            exe=Path(folder)/'battery.exe'
            subprocess.run([shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror',
                '-I',str(ROOT/'include'),str(ROOT/'tools/host_battery_policy.cpp'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
