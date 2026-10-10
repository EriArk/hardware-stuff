"""Progress must distinguish unknown totals and current-step completion."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SyncProgressTests(unittest.TestCase):
    def test_finishing_overlay_cleans_before_returning_to_the_same_list(self):
        source = (ROOT / 'src/main.cpp').read_text('utf8')
        finish = source.split('if (AutomaticSync::takeFinished()) {', 1)[1].split('if (AutomaticSync::takeLibraryChanged())', 1)[0]
        self.assertLess(finish.index('scheduleGhostCleanup("sync-finished")'), finish.index('syncProgressVisible = false'))
        self.assertLess(finish.index('scheduleGhostCleanup("sync-finished")'), finish.index('displayLocalLibrary('))

    def test_unknown_total_steps_and_large_transfers(self):
        source = r'''
#include "sync_progress.h"
#include <cassert>
int main() {
    using namespace SyncProgress;
    Snapshot s;
    assert(percent(s)==-1);
    s.done=100;assert(percent(s)==-1); // Bytes alone are not a percentage.
    s.total=1000;assert(percent(s)==10);
    s.done=1001;assert(percent(s)==100);
    s.done=3000000000U;s.total=4000000000U;assert(percent(s)==75);
    s.stage=Stage::ReadingUpload;s.done=3;s.total=5;assert(percent(s)==60);
    auto next=s;assert(!changed(s,next));
    next.stage=Stage::ReadingDownload;assert(changed(s,next));
    next=s;next.downloaded=1;assert(changed(s,next));
    next=s;next.total=0;assert(changed(s,next) && percent(next)==-1);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            cpp = Path(folder) / 'progress.cpp'
            exe = Path(folder) / 'progress.exe'
            cpp.write_text(source, encoding='utf8')
            subprocess.run([shutil.which('g++'), '-std=c++17', '-Wall', '-Wextra',
                            '-Werror', '-I', str(ROOT / 'include'), str(cpp),
                            '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
