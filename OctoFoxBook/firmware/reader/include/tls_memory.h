#pragma once

namespace ReaderTlsMemory {
// Call once during setup, after PSRAM validation and before any TLS session.
bool initialize();
}
