from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class BookishSectionsTests(unittest.TestCase):
    def test_production_sections_geometry_and_nested_navigation(self):
        with tempfile.TemporaryDirectory() as folder:
            exe=Path(folder)/'sections.exe'
            subprocess.run([shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror',
                '-I',str(ROOT/'include'),str(ROOT/'tools/host_bookish_sections.cpp'),
                str(ROOT/'src/bookish_ui.cpp'),'-o',str(exe)],check=True,capture_output=True)
            result=subprocess.run([str(exe)],check=True,capture_output=True,text=True,encoding='utf-8')
            self.assertEqual(result.stdout.count('SCENE '),8)

if __name__=='__main__':unittest.main()
