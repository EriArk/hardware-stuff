#include "tls_memory.h"

#include <stdint.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

namespace {
// The stock Arduino SDK forces mbedTLS into internal RAM. Its two 16 KiB
// record buffers do not fit beside the reader/display/Wi-Fi tasks. Keep small
// crypto allocations internal; use the board's PSRAM for large TLS buffers.
// ESP-IDF supports this allocator hook; no certificate/hostname checks change.
void *tlsCalloc(size_t count, size_t size) {
    if (size != 0 && count > SIZE_MAX / size) return nullptr;
    const uint32_t caps = count * size >= 4096 ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
    return heap_caps_calloc(count, size, caps | MALLOC_CAP_8BIT);
}
}

namespace ReaderTlsMemory {
bool initialize() {
    static bool initialized = false;
    if (!initialized) {
        initialized = mbedtls_platform_set_calloc_free(tlsCalloc, heap_caps_free) == 0;
    }
    return initialized;
}
}
