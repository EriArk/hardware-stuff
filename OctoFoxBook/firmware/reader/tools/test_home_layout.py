"""Production ordering and Home action reachability, without a device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

FW = Path(__file__).resolve().parents[1]


class HomeLayoutTest(unittest.TestCase):
    def test_newest_selection_progress_independence_and_all_actions(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ required')
        cpp = r'''
#include "home_layout.h"
#include <cassert>
#include <vector>
#include <algorithm>
struct Entry {const char *id; unsigned addedAt; bool hasProgress;};
int main() {
    using namespace HomeLayout;
    std::vector<Entry> b={{"old",10,true},{"second",20,false},{"new",30,false}};
    uint16_t out[2]{};
    assert(newest(b.data(),b.size(),out,2)==2 && out[0]==2 && out[1]==1);
    b[2].hasProgress=true; // Opening/reading is NOT an addition.
    assert(newest(b.data(),b.size(),out,2)==2 && out[0]==2 && out[1]==1);
    b.erase(b.begin()+2); // Deleted newest falls out; next item fills its slot.
    assert(newest(b.data(),b.size(),out,2)==2 && out[0]==1 && out[1]==0);
    b.push_back({"uploaded",40,false});
    assert(newest(b.data(),b.size(),out,2)==2 && out[0]==2);
    b={{"z",0,false},{"a",0,true},{"b",0,false}};
    assert(newest(b.data(),b.size(),out,2)==2 && out[0]==1 && out[1]==2);
    std::reverse(b.begin(),b.end());
    newest(b.data(),b.size(),out,2);
    assert(strcmp(b[out[0]].id,"a")==0 && strcmp(b[out[1]].id,"b")==0);
    assert(newest(b.data(),0,out,2)==0);
    assert(newest(b.data(),1,out,2)==1);
    assert(newest(b.data(),3,out,0)==0);
    for(size_t reading=0;reading<=kReadingCapacity;++reading)
      for(size_t added=0;added<=kAddedCapacity;++added) {
        assert(action(0,reading,added)==(reading ? Action::Continue : added ? Action::AddedCard : Action::Sync));
        assert(action(syncSelection(reading,added),reading,added)==Action::Sync);
        const size_t total=actionCount(reading,added);
        assert(initialSelection(reading,added)<total);
        assert(syncSelection(reading,added)==total-2);
        assert(action(settingsSelection(reading,added),reading,added)==Action::Settings);
        for(size_t i=kFirstBook;i<total-2;++i) {
            const auto a=action(i,reading,added);
            if(i<kFirstBook+reading) assert(a==(i==kFirstBook ? Action::Continue : Action::ReadingCard));
            else assert(a==Action::AddedCard);
        }
        assert(action(total,reading,added)==Action::Invalid);
      }
}
'''
        with tempfile.TemporaryDirectory(prefix='home-layout-') as directory:
            root=Path(directory)
            (root/'test.cpp').write_text(cpp,encoding='utf-8')
            result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-I',str(FW/'include'),str(root/'test.cpp'),'-o',str(root/'test.exe')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(root/'test.exe')],check=True)

    def test_only_continue_bypasses_card_and_selection_does_not_rescan(self):
        source=(FW/'src/main.cpp').read_text(encoding='utf-8')
        opening=source.split('if (pendingHomeBookOpen) {',1)[1].split('void closeBulkDownloadSession',1)[0]
        self.assertIn('action == HomeLayout::Action::Continue',opening)
        self.assertIn('"home-added" : "home-recent"',opening)
        self.assertIn('HomeLayout::newest(allEntries',source)
        self.assertIn('HomeLayout::newer(localLibrarySession.entries[value]',source)
        render=(FW/'src/bookish_adapter.inc').read_text(encoding='utf-8')
        self.assertNotIn('LocalLibrary::scan',render)
        self.assertNotIn('SD.open',render)
        self.assertIn('displayHome(false, "selection")',source)
        self.assertIn('homeSession.addedCount',render)


if __name__ == '__main__':
    unittest.main()
