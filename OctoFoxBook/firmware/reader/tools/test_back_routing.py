"""Execute the actual parent routing function across every nested screen."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
class BackRoutingTests(unittest.TestCase):
    def test_double_click_returns_one_level(self):
        s=(ROOT/'src/main.cpp').read_text('utf8')
        enums='enum class UiScreen'+s.split('enum class UiScreen',1)[1].split('struct Diagnostics',1)[0]
        function='void queueBackAction()'+s.split('void queueBackAction()',1)[1].split('#include "battery_ui.inc"',1)[0]
        flags=sorted(set(re.findall(r'\b(pending\w+)\s*=true',function)))
        expected={'ReaderMenu':'pendingReaderMenuBack','Contents':'pendingContentsBack','Bookmarks':'pendingReaderBookmarksBack',
            'ReadingSettings':'pendingReadingSettingsBack','OrderedSelector':'pendingOrderedSelectorBack','LocalLibrary':'pendingLocalLibraryBack',
            'Catalog':'pendingCatalogBack','Search':'pendingSearchBack','Favorites':'pendingFavoritesBack','Download':'pendingDownloadBack',
            'BulkDownloadConfirm':'pendingBulkDownloadBack','BookCard':'pendingBookCardBack','Annotation':'pendingAnnotationBack'}
        harness='#include <cassert>\n#include <cstdint>\n#include <cstddef>\n'+enums+'\nUiScreen uiScreen;\n'
        harness+='TopLevelTab pendingTopLevelTarget=TopLevelTab::Home,readerReturnTab=TopLevelTab::Favorites;\n'
        harness+='\n'.join('bool '+f+'=false;' for f in flags)+'\n'+function+'\nint main(){\n'
        for screen,flag in expected.items():
            harness+='uiScreen=UiScreen::'+screen+';queueBackAction();assert('+flag+');\n'
            harness+='assert(('+ '+'.join('int('+f+')' for f in flags)+')==1);'+flag+'=false;\n'
        harness+='uiScreen=UiScreen::Reader;queueBackAction();assert(pendingTopLevelOpen && pendingTopLevelTarget==readerReturnTab);pendingTopLevelOpen=false;\n'
        harness+='uiScreen=UiScreen::Home;queueBackAction();assert(!pendingTopLevelOpen);}\n'
        with tempfile.TemporaryDirectory() as folder:
            cpp=Path(folder)/'back.cpp';exe=Path(folder)/'back.exe';cpp.write_text(harness,encoding='utf8')
            subprocess.run([shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
