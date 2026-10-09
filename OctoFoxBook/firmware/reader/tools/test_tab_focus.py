"""Execute the production tab handler and redraw logic without hardware."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class TabFocusTests(unittest.TestCase):
    def test_tab_focus_is_a_separate_top_level(self):
        source = (ROOT/'src/main.cpp').read_text(encoding='utf-8')
        enums = source.split('enum class UiScreen',1)[1].split('struct Diagnostics',1)[0]
        production = source.split('void displaySections() {',1)[1].split('void processTopLevelActions()',1)[0]
        harness = r'''
#include <cassert>
#include <cstring>
#include <cstddef>
#include <cstdint>
''' + 'enum class UiScreen' + enums + r'''
UiScreen uiScreen=UiScreen::Home;
TopLevelTab activeTopLevelTab=TopLevelTab::Home;
size_t sectionSelection=0;
struct {bool loaded=true;} homeSession;
bool loadHomeSession(){homeSession.loaded=true;return true;}
int painted=-1,opened=-1,draws=0;
bool renderBookishHome(int focus){painted=focus;return true;}
void scheduleScreenTransitionCleanup(UiScreen,const char*){}
void noteUiNavigationClick(){}
enum class DisplayRefreshMode { QualityFull, FastUi };
struct Result { bool ok=true; } lastDisplayRefresh;
bool hasDisplayRefresh=false;
uint8_t *framebuffer=nullptr;
struct Display { Result refresh(uint8_t *,DisplayRefreshMode){++draws;return {};} } displayRefresh;
struct Log {template<class...T> void printf(const char *,T...){} } Serial;
bool displayTopLevelTab(TopLevelTab tab,bool,const char*) {
    opened=int(tab);activeTopLevelTab=tab;uiScreen=UiScreen::Home;return true;
}
void displaySections() {
''' + production + r'''
int main() {
    for(auto screen:{UiScreen::Home,UiScreen::LocalLibrary,UiScreen::Search,
        UiScreen::Favorites,UiScreen::Reader,UiScreen::BookCard,UiScreen::Annotation}) {
        uiScreen=screen;activeTopLevelTab=TopLevelTab::OnDevice;
        assert(handleTabNavigation("CENTER","LONG"));
        assert(uiScreen==UiScreen::Sections && sectionSelection==1 && painted==1);
        assert(handleTabNavigation("DOWN","SHORT"));
        assert(sectionSelection==2 && painted==2); // Redraw cannot reset focus.
        assert(handleTabNavigation("UP","SHORT"));assert(sectionSelection==1);
        const int previous=opened;
        assert(handleTabNavigation("CENTER","LONG"));
        assert(uiScreen==UiScreen::Sections && opened==previous);
        assert(handleTabNavigation("CENTER","SHORT"));
        assert(opened==int(TopLevelTab::OnDevice) && uiScreen!=UiScreen::Sections);
    }
    activeTopLevelTab=TopLevelTab::Home;uiScreen=UiScreen::Home;displaySections();
    handleTabNavigation("UP","SHORT");assert(sectionSelection==kVisibleTabCount-1);
    handleTabNavigation("DOWN","SHORT");assert(sectionSelection==0);
    for(unsigned i=0;i<kVisibleTabCount;++i) {
        handleTabNavigation("CENTER","SHORT");assert(opened==int(kVisibleTabs[i]));
        displaySections();assert(sectionSelection==i);
        handleTabNavigation("DOWN","SHORT");
    }
    for(auto screen:{UiScreen::Wifi,UiScreen::OrderedSelector,UiScreen::ReaderMenu}) {
        uiScreen=screen;assert(!handleTabNavigation("CENTER","LONG"));
    }
    assert(draws>10);
}
'''
        harness = '#include <initializer_list>\n' + harness
        with tempfile.TemporaryDirectory() as folder:
            cpp=Path(folder)/'tabs.cpp'; exe=Path(folder)/'tabs.exe'
            cpp.write_text(harness,encoding='utf-8')
            build=subprocess.run([shutil.which('g++'),'-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            run=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)
        self.assertIn('if (handleTabNavigation(button, gesture)) return;', source)
        self.assertIn('if (target < 0) { displaySections(); return; }', source)
        self.assertNotIn('HomeLayout::Action::Sections', source)

if __name__ == '__main__':
    unittest.main()
