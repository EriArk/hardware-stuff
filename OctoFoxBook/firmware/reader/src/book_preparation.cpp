#include "book_preparation.h"
#include <Arduino.h>
#include <atomic>
#include <esp_heap_caps.h>
#include "book_upload.h"
#include "fb2_cache.h"
#include "reader_pagination.h"
#include "work_progress.h"

namespace {
std::atomic<bool> active{false}, done{false}, cancelled{false};
bool succeeded = false;
char target[33]{}, failure[96]{};
bool cancellation() { return cancelled.load(); }
void prepare(void *) {
    setWorkCancelCallback(cancellation);
    auto *cache = static_cast<Fb2CacheInfo *>(heap_caps_calloc(
        1, sizeof(Fb2CacheInfo), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ReaderPaginationInfo pages{};
    const uint32_t started = millis();
    bool ok = cache && (Fb2Cache::load(target, *cache) || Fb2Cache::build(target, *cache));
    if (!ok) snprintf(failure, sizeof(failure), "%s", cache ? cache->error : "allocation-failed");
    if (ok && !cancelled.load()) {
        ok = ReaderPagination::load(target, pages) || ReaderPagination::build(target, pages);
        if (!ok) snprintf(failure, sizeof(failure), "%s", pages.error);
    }
    heap_caps_free(cache);
    succeeded = ok && !cancelled.load();
    setWorkCancelCallback(nullptr);
    Serial.printf("BOOK WORKER id=%s ok=%s cancelled=%s duration_ms=%lu\n", target,
        succeeded ? "true" : "false", cancelled.load() ? "true" : "false",
        static_cast<unsigned long>(millis() - started));
    active = false;
    done = true; // release publishes result strings and success to the UI
    vTaskDelete(nullptr);
}
}
namespace BookPreparation {
bool start(const char *bookId) {
    if (active.load() || done.load() || !BookUploadReceiver::validBookId(bookId)) return false;
    snprintf(target, sizeof(target), "%s", bookId);
    failure[0] = '\0';
    cancelled = false;
    active = true;
    if (xTaskCreatePinnedToCore(prepare, "book_prepare", 16384, nullptr, 1, nullptr, 0) != pdPASS) {
        active = false;
        return false;
    }
    return true;
}
bool busy() { return active.load() || done.load(); }
void cancel() { cancelled = true; }
bool takeResult(bool &ok, bool &wasCancelled) {
    if (!done.exchange(false)) return false;
    ok = succeeded;
    wasCancelled = cancelled.load();
    return true;
}
const char *error() { return failure; }
}
