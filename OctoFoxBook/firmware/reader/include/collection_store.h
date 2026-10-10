#pragma once
#include <stddef.h>
#include "sync_policy.h"
class NetworkService;
namespace Collections {
constexpr size_t kCapacity = 100;
bool load();
size_t count();
const char *id(size_t shelf);
const char *name(size_t shelf);
size_t bookCount(size_t shelf);
const char *book(size_t shelf, size_t ordinal);
bool contains(size_t shelf, const char *bookId);
bool create(const char *name);
bool toggle(size_t shelf, const char *bookId);
// Explicit operator action only, after the old books have been archived.
bool archiveProfile();
// Upload before reconciling book delivery; fetch the snapshot after delivery.
bool sync(NetworkService &network, const char *device, bool uploadOnly = false);
const char *error();
ReaderSyncPolicy::Error policyError();
int httpCode();
}
