#pragma once
#include "provisioning_store.h"
namespace WifiCredentials {
inline void wipe(void *data, size_t length) {
    volatile unsigned char *p = static_cast<volatile unsigned char *>(data);
    while (length--) *p++ = 0;
}
// Independent primary network, with read-only migration from legacy provisioning.
bool load(WifiCredential &credential);
bool save(const WifiCredential &credential);
bool clear();
bool validPassword(const char *password, bool open);
}
