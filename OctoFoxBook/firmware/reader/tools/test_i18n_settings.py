from pathlib import Path
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from generate_i18n import generate

ROOT = Path(__file__).resolve().parents[1]


class LanguageSettingsTests(unittest.TestCase):
    def test_catalog_covers_marked_labels_and_generated_header_is_current(self):
        catalog = json.loads((ROOT/'assets/locales/en.json').read_text('utf8'))
        self.assertEqual((ROOT/'include/i18n_catalog.h').read_text('utf8'), generate())
        for path in [*ROOT.glob('src/*'), *ROOT.glob('include/*.h')]:
            if path.is_file():
                for label in re.findall(r'I18n::tr\(("(?:[^"\\]|\\.)*")\)', path.read_text('utf8')):
                    self.assertIn(json.loads(label), catalog, str(path))
        self.assertFalse(any(re.search('[\u0400-\u04ff]', label) for label in catalog.values()))

    def test_settings_defaults_roundtrip_and_failed_save_preserves_previous(self):
        with tempfile.TemporaryDirectory() as folder:
            exe=Path(folder)/'settings.exe'
            command=[shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror',
                '-I',str(ROOT/'tools/host_stubs'),'-I',str(ROOT/'include'),
                '-I',str(ROOT/'.pio/libdeps/t5_epaper_s3_fast/ArduinoJson/src'),
                *[str(ROOT/p) for p in ['tools/host_settings.cpp','src/reader_settings.cpp','src/storage_recovery.cpp','src/work_progress.cpp']],'-o',str(exe)]
            build=subprocess.run(command,capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
