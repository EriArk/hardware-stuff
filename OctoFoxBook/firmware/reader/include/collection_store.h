#pragma once
#include <stddef.h>
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
// Upload before reconciling book delivery; fetch the snapshot after delivery.
bool sync(NetworkService &network, const char *device, bool uploadOnly = false);
const char *error();
}
