"""Compile the production sector buffer with a fault-injecting file sink."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class DownloadWriterTest(unittest.TestCase):
    def test_fragmented_network_short_disk_writes_and_errors(self):
        compiler = shutil.which('g++')
        if not compiler:
            self.skipTest('g++ required')
        header = Path(__file__).resolve().parents[1] / 'include'
        harness = r'''
#include "download_writer.h"
#include <vector>
#include <cassert>
#include <algorithm>
struct Sink {
    std::vector<uint8_t> data;
    std::vector<size_t> requests;
    size_t limit = 99999;
    int interrupts = 0, failure = 0, syncs = 0;
    bool syncOk = true;
    long write(const uint8_t *p, size_t n) {
        requests.push_back(n);
        if (interrupts) { --interrupts; errno=EINTR; return -1; }
        if (failure) { errno=failure; return -1; }
        n=std::min(n,limit); data.insert(data.end(),p,p+n); return n;
    }
    bool sync() { ++syncs; return syncOk; }
};
int main() {
    std::vector<uint8_t> source(16391);
    for(size_t i=0;i<source.size();++i) source[i]=i*31;
    for(size_t capacity: {512,4096}) for(size_t fragment: {1,127,512,1380,4096,8192}) {
        uint8_t storage[4096]; Sink sink;
        DownloadWriteBuffer<Sink> output(sink,storage,capacity);
        for(size_t i=0;i<source.size();i+=fragment)
            assert(output.append(source.data()+i,std::min(fragment,source.size()-i)));
        assert(output.finish() && sink.data==source && sink.syncs==1);
        std::vector<size_t> expected(source.size()/capacity,capacity);
        expected.push_back(source.size()%capacity);
        assert(sink.requests==expected);
    }
    uint8_t storage[4096]; Sink shortSink; shortSink.limit=133; shortSink.interrupts=2;
    DownloadWriteBuffer<Sink> shortOutput(shortSink,storage,sizeof(storage));
    assert(shortOutput.append(source.data(),source.size()) && shortOutput.finish());
    assert(shortSink.data==source);
    for(int failure: {EIO,ENOSPC}) {
        Sink bad; bad.failure=failure;
        DownloadWriteBuffer<Sink> output(bad,storage,sizeof(storage));
        assert(!output.append(source.data(),4096) && output.error()==failure);
        bad.failure=0;
        assert(!output.finish() && !output.append(source.data(),1) && bad.data.empty());
    }
    Sink zero; zero.limit=0;
    DownloadWriteBuffer<Sink> z(zero,storage,sizeof(storage));
    assert(!z.append(source.data(),4096) && z.error()==EIO);
    Sink intr; intr.interrupts=100;
    DownloadWriteBuffer<Sink> i(intr,storage,sizeof(storage));
    assert(!i.append(source.data(),4096) && intr.requests.size()==8);
    Sink syncFail; syncFail.syncOk=false;
    DownloadWriteBuffer<Sink> f(syncFail,storage,sizeof(storage));
    assert(f.append(source.data(),17) && !f.finish());
}
'''
        with tempfile.TemporaryDirectory(prefix='reader-write-test-') as directory:
            cpp = Path(directory) / 'test.cpp'
            binary = Path(directory) / 'test.exe'
            cpp.write_text(harness, encoding='utf-8')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-I', str(header),
                                    str(cpp), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            tested = subprocess.run([str(binary)], capture_output=True, timeout=10)
            self.assertEqual(tested.returncode, 0, tested.stderr)


if __name__ == '__main__':
    unittest.main()
