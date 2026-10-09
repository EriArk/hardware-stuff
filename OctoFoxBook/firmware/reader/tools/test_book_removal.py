"""Actual production archive function with an in-memory SD, no connected reader."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

FW = Path(__file__).resolve().parents[1]


class BookRemovalTest(unittest.TestCase):
    def test_archive_is_idempotent_collision_safe_and_preserves_failed_source(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ required')
        source = (FW / 'src/book_upload.cpp').read_text(encoding='utf-8')
        functions = '\n'.join(re.search(r'bool BookUploadReceiver::'+name+r'\([^\n]*\) \{.*?\n\}', source, re.S)[0]
                              for name in ('validBookId', 'archiveBook'))
        stub = r'''
#include <set>
#include <string>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <cassert>
struct FakeSD {
    std::set<std::string> paths;
    bool failRename=false, failMkdir=false;
    int writes=0;
    bool exists(const char *p) { return paths.count(p); }
    bool mkdir(const char *p) { ++writes; return !failMkdir && paths.insert(p).second; }
    bool rename(const char *from,const char *to) {
        ++writes;
        if(failRename || !paths.count(from) || paths.count(to)) return false;
        const std::string prefix=std::string(from)+"/";
        auto copy=paths;
        for(const auto &path:copy) if(path==from || path.find(prefix)==0) {
            paths.erase(path); paths.insert(std::string(to)+path.substr(strlen(from)));
        }
        return true;
    }
} SD;
struct BookUploadReceiver {
    static bool validBookId(const char *);
    static bool archiveBook(const char *);
};
'''
        tests = r'''
int main() {
    assert(!BookUploadReceiver::archiveBook(nullptr));
    for(auto id:{"", "../other", "x/y", "../", "123456789012345678901234567890123"})
        assert(!BookUploadReceiver::archiveBook(id));
    assert(SD.writes==0);
    assert(BookUploadReceiver::archiveBook("opds-absent") && SD.writes==0);
    SD.paths={"/books/opds-42", "/books/opds-42/book.fb2", "/books/opds-42/reader-state.json", "/trash", "/trash/opds-42-0"};
    assert(BookUploadReceiver::archiveBook("opds-42"));
    assert(!SD.paths.count("/books/opds-42"));
    assert(SD.paths.count("/trash/opds-42-0"));
    assert(SD.paths.count("/trash/opds-42-1/book.fb2"));
    assert(SD.paths.count("/trash/opds-42-1/reader-state.json"));
    auto saved=SD.paths; assert(BookUploadReceiver::archiveBook("opds-42") && saved==SD.paths);
    SD.paths={"/books/42", "/books/42/book.fb2"}; SD.failRename=true;
    assert(!BookUploadReceiver::archiveBook("42"));
    assert(SD.paths.count("/books/42/book.fb2"));
    SD.paths.erase("/trash"); SD.failMkdir=true;
    assert(!BookUploadReceiver::archiveBook("42"));
    assert(SD.paths.count("/books/42/book.fb2"));
}
'''
        with tempfile.TemporaryDirectory(prefix='reader-removal-') as folder:
            root = Path(folder)
            (root/'test.cpp').write_text(stub+functions+tests, encoding='utf-8')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', str(root/'test.cpp'), '-o', str(root/'test.exe')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(root/'test.exe')], check=True, timeout=10)

    def test_removal_uses_v2_and_durable_receipt_before_changing_storage(self):
        source = (FW/'src/automatic_sync.cpp').read_text(encoding='utf-8')
        self.assertIn('&v=2', source)
        branch = source.split('if (removal) {',1)[1].split('BookDownloadResult result',1)[0]
        self.assertIn('BookUploadReceiver::archiveBook(id)', branch)
        self.assertIn('saveReceipt(receipt)', branch)
        self.assertIn('sendSavedReceipt(network)', branch)
        self.assertIn('ok ? "removed" : "queued"', branch)
        self.assertLess(source.index('if (!saveReceipt(receipt))'), source.index('if (removal)'))


if __name__ == '__main__':
    unittest.main()
