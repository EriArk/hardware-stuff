"""Exercise production list normalization at both page edges and after shrink."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = source.index('void ' + name + '() {')
    end = start
    depth = 0
    for end in range(source.index('{', start), len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end + 1]
    raise AssertionError(name)


class ListPagingTests(unittest.TestCase):
    def test_stationary_pages_forward_backward_and_restored_selection(self):
        source = (ROOT / 'src/main.cpp').read_text('utf8')
        production = '\n'.join(function(source, name) for name in (
            'normalizeSearchSelection', 'normalizeCatalogSelection',
            'normalizeFavoritesSelection'))
        harness = r'''
#include <algorithm>
#include <cassert>
#include <cstddef>
#include "list_paging.h"
using std::min;
size_t count;
constexpr size_t kSearchRowsPerScreen=4, kCatalogRowsPerScreen=8;
namespace BookishUI { constexpr size_t kListRows=4; }
enum class FavoritesPhase { Folders, Books };
struct Session {
    size_t selected=0, firstVisible=0, folder=0;
    FavoritesPhase phase=FavoritesPhase::Books;
} searchSession, catalogSession, favoritesSession;
size_t searchRowCount(){return count;}
size_t catalogRowCount(){return count;}
size_t favoriteFolderCount(size_t){return count;}
namespace Collections { size_t count(){return ::count;} }
''' + production + r'''
void check(Session &s, void (*normalize)(), size_t rows) {
    for(count=0;count<35;++count) {
        s.selected=100; s.firstVisible=99; normalize();
        assert(s.selected==(count ? count-1 : 0));
        assert(s.firstVisible==(count ? ((count-1)/rows)*rows : 0));
        size_t changes=0;
        s.selected=0; normalize();
        for(size_t i=0;i<count;++i) {
            const size_t previous=s.firstVisible;
            s.selected=i; normalize();
            assert(s.firstVisible<=i && i<s.firstVisible+rows);
            if(s.firstVisible!=previous) {
                ++changes;
                assert(i%rows==0 && s.firstVisible==previous+rows);
            }
        }
        assert(changes==(count ? (count-1)/rows : 0));
        for(size_t i=count;i>0;--i) {
            const size_t previous=s.firstVisible;
            s.selected=i-1; normalize();
            if(s.firstVisible!=previous)
                assert(s.selected==previous-1 && s.firstVisible+rows==previous);
        }
    }
}
int main() {
    check(searchSession,normalizeSearchSelection,4);
    check(catalogSession,normalizeCatalogSelection,8);
    check(favoritesSession,normalizeFavoritesSelection,4);
    count=8;favoritesSession.phase=FavoritesPhase::Folders;
    favoritesSession.selected=8;normalizeFavoritesSelection();
    assert(favoritesSession.firstVisible==8); // New collection on a partial page.
    assert(listPageStart(5,4)==4 && listPageStart(4,4)==4);
    assert(listPageStart(3,4)==0 && listPageStart(8,8)==8);
    assert(listPageStart(100,0)==0);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            cpp = Path(folder) / 'paging.cpp'
            exe = Path(folder) / 'paging.exe'
            cpp.write_text(harness, encoding='utf8')
            subprocess.run([shutil.which('g++'), '-std=c++17', '-Wall', '-Wextra',
                            '-Werror', '-I', str(ROOT / 'include'), str(cpp),
                            '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_every_former_sliding_list_uses_page_boundaries(self):
        source = (ROOT / 'src/main.cpp').read_text('utf8')
        for session in ('searchSession', 'catalogSession', 'localLibrarySession',
                        'favoritesSession', 'contentsSession', 'readerBookmarksSession'):
            self.assertRegex(source, rf'{session}\.firstVisible\s*=\s*listPageStart\({session}\.selected,')
        self.assertNotRegex(source, r'\.selected\s*-\s*k\w*Rows\w*\s*\+\s*1')
        # Contents and bookmarks also clean on a page turn, not only screen entry.
        for session in ('contentsSession', 'readerBookmarksSession', 'catalogSession'):
            self.assertIn(f'if (renderedFirst != {session}.firstVisible) scheduleGhostCleanup("list-page");', source)


if __name__ == '__main__':
    unittest.main()
