"""Compile the production TLS allocator against bounded host heap stubs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import re

TOOLS = Path(__file__).resolve().parent
FW = TOOLS.parent


class TlsMemoryTests(unittest.TestCase):
    def test_actual_allocator_zeroing_limits_routing_failure_and_once(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ required")
        with tempfile.TemporaryDirectory(prefix="reader-tls-test-") as directory:
            root = Path(directory)
            (root / "mbedtls").mkdir()
            (root / "esp_heap_caps.h").write_text('''
#pragma once
#include <cstddef>
#include <cstdint>
constexpr uint32_t MALLOC_CAP_SPIRAM=1, MALLOC_CAP_INTERNAL=2, MALLOC_CAP_8BIT=4;
void *heap_caps_calloc(size_t, size_t, uint32_t);
void heap_caps_free(void *);
''', encoding="utf-8")
            (root / "mbedtls/platform.h").write_text('''
#pragma once
#include <cstddef>
int mbedtls_platform_set_calloc_free(void *(*)(size_t,size_t),void (*)(void *));
''', encoding="utf-8")
            (root / "test.cpp").write_text(r'''
#include "tls_memory.h"
#include "esp_heap_caps.h"
#include <cassert>
#include <cstdlib>
void *(*allocate)(size_t,size_t) = nullptr;
void (*release)(void *) = nullptr;
unsigned registrations=0, calls=0, lastCaps=0;
bool fail=false;
int mbedtls_platform_set_calloc_free(void *(*a)(size_t,size_t),void (*f)(void *)) {
    ++registrations; allocate=a; release=f; return 0;
}
void *heap_caps_calloc(size_t n, size_t s, uint32_t caps) {
    ++calls; lastCaps=caps; return fail ? nullptr : calloc(n,s);
}
void heap_caps_free(void *p) { free(p); }
int main() {
    assert(ReaderTlsMemory::initialize());
    assert(ReaderTlsMemory::initialize() && registrations==1);
    for (size_t size : {size_t(64),size_t(4095),size_t(4096),size_t(16384)}) {
        auto *p=static_cast<unsigned char *>(allocate(1,size));
        assert(p && lastCaps==((size>=4096 ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL)|MALLOC_CAP_8BIT));
        for(size_t i=0;i<size;++i) assert(p[i]==0);
        release(p);
    }
    void *p=allocate(2,2048); assert(p && (lastCaps&MALLOC_CAP_SPIRAM)); release(p);
    unsigned before=calls; assert(!allocate(SIZE_MAX,2) && calls==before);
    fail=true; assert(!allocate(1,16384)); // Allocation failure remains failure, no security bypass.
}
'''.replace('#include <cstdlib>', '#include <cstdlib>\n#include <initializer_list>'), encoding="utf-8")
            binary = root / "tls.exe"
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                     "-I", str(root), "-I", str(FW / "include"),
                                     str(root / "test.cpp"), str(FW / "src/tls_memory.cpp"),
                                     "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_allocator_installed_before_workers_and_trust_is_retained(self):
        main = (FW / "src/main.cpp").read_text(encoding="utf-8")
        setup = main.split("void setup()", 1)[1].split("void loop()", 1)[0]
        self.assertLess(setup.index("testPsram()"), setup.index("ReaderTlsMemory::initialize()"))
        self.assertLess(setup.index("ReaderTlsMemory::initialize()"), setup.index("AutomaticSync::start()"))
        network = (FW / "src/network_service.cpp").read_text(encoding="utf-8")
        self.assertNotIn("setInsecure", network)
        self.assertGreaterEqual(network.count("client.setCACert(kTrustedRoots)"), 4)

    def test_http_cleanup_cannot_leave_a_socket_after_radio_shutdown(self):
        network = (FW / "src/network_service.cpp").read_text(encoding="utf-8")
        self.assertEqual(network.count("HTTPClient http;"), 4)
        self.assertEqual(network.count("http.setReuse(false);"), 4)
        # HTTPClient::end() alone can retain its socket. All normal and failure
        # cleanup paths must explicitly stop TLS before NetworkService::disconnect.
        ends = list(re.finditer(r"http\.end\(\);", network))
        self.assertEqual(len(ends), 8)
        for end in ends:
            self.assertTrue(network[end.end():].lstrip().startswith("client.stop();"))
        begin_failure = network.split('setError(result.error, sizeof(result.error), "http-begin-failed");', 1)[1].split("return false;", 1)[0]
        self.assertLess(begin_failure.index("client.stop();"), begin_failure.index("disconnect();"))


if __name__ == "__main__":
    unittest.main()
