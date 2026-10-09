#include "automatic_sync.h"
#include "wifi_setup.h"

#include <atomic>
#include <errno.h>
#include <time.h>
#include <esp_heap_caps.h>
#include <SD.h>
#include <ArduinoJson.h>

#include "book_upload.h"
#include "fb2_cache.h"
#include "network_service.h"
#include "work_progress.h"

namespace {
std::atomic<bool> paused{false};
std::atomic<bool> running{false};
std::atomic<bool> changed{false};
std::atomic<bool> cancelled{false};
std::atomic<bool> finished{false};
std::atomic<unsigned> deliveredCount{0};
std::atomic<AutomaticSync::Status> syncStatus{AutomaticSync::Status::Idle};
std::atomic<ReaderSyncPolicy::Error> lastError{ReaderSyncPolicy::Error::None};
std::atomic<int> lastHttpCode{0};
TaskHandle_t syncTask = nullptr;
portMUX_TYPE syncGate = portMUX_INITIALIZER_UNLOCKED;
char identity[32]{};
constexpr char kReceiptPath[] = "/sync-receipt.json";
constexpr char kReceiptPartial[] = "/sync-receipt.json.part";

void recordFailure(ReaderSyncPolicy::Error error, int httpCode = 0) {
    lastError = error;
    lastHttpCode = httpCode;
    // Only fixed codes, never credentials, URLs or server response bodies.
    Serial.printf("SYNC ERROR reason=%s http=%d\n", ReaderSyncPolicy::errorCode(error), httpCode);
}

bool saveReceipt(const char *json) {
    File file = SD.open(kReceiptPartial, FILE_WRITE);
    const bool ok = file && file.print(json) == strlen(json);
    file.flush();
    file.close();
    if (!ok) return false;
    // A completed .part can be recovered if power is lost during the rename.
    if (SD.exists(kReceiptPath) && !SD.remove(kReceiptPath)) return false;
    return SD.rename(kReceiptPartial, kReceiptPath);
}

bool sendSavedReceipt(NetworkService &network) {
    const char *saved = SD.exists(kReceiptPath) ? kReceiptPath : kReceiptPartial;
    if (!SD.exists(saved)) return true;
    File file = SD.open(saved, FILE_READ);
    JsonDocument receipt;
    const bool valid = file && file.size() <= 320 &&
                       !deserializeJson(receipt, file) && receipt["job"].is<unsigned long>();
    file.close();
    if (!valid) {
        Serial.println("SYNC RECEIPT invalid=true awaiting-lease-retry=true");
        // Only our interrupted receipt is removed, never book data.
        SD.remove(saved);
        return true;
    }
    char json[321]{};
    serializeJson(receipt, json, sizeof(json));
    char path[128]{};
    snprintf(path, sizeof(path), "/reader-api/device/ack?device=%s", identity);
    String body;
    SyncRequestResult result{};
    const bool ok = network.syncRequest(path, json, body, result);
    if (!ok && result.httpCode != 404 && result.httpCode != 409) {
        recordFailure(result.error, result.httpCode);
        return false;
    }
    // 404/409 means this old lease no longer belongs to the configured account.
    SD.remove(kReceiptPath);
    SD.remove(kReceiptPartial);
    return true;
}

bool localReady(const char *id) {
    char path[96]{};
    if (!BookUploadReceiver::bookPath(id, path, sizeof(path))) return false;
    File file = SD.open(path, FILE_READ);
    const bool ok = file && file.size() >= BookUploadReceiver::kMinimumBookBytes;
    file.close();
    return ok;
}

void worker(void *) {
    NetworkService network;
    for (;;) {
        // No boot scan, timer, retry or wake-triggered Wi-Fi connection.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        network.invalidateConfiguration(); // Wi-Fi may have changed in Settings.
        setWorkCancelCallback(AutomaticSync::cancelRequested);
        // USB provisioning may have replaced NVS while this task was paused.
        network.invalidateConfiguration();
        unsigned delivered = 0;
        unsigned removed = 0, handled = 0;
        bool complete = false;
        bool hadFailures = false;
        unsigned long long after = 0;
        for (unsigned batch = 0; batch < 64 && !paused.load() && !cancelled.load(); ++batch) {
            if (!sendSavedReceipt(network)) break;
            char path[128]{};
            // Each job is attempted once per manual pass. Failed jobs stay queued
            // for the next press, without starving later downloads or removals.
            snprintf(path, sizeof(path), "/reader-api/device/next?device=%s&v=2&after=%llu", identity, after);
            String body;
            SyncRequestResult response{};
            if (!network.syncRequest(path, nullptr, body, response)) {
                recordFailure(response.error, response.httpCode);
                break;
            }
            if (response.httpCode == 204) { complete = true; break; }
            char wire[2049]{};
            body.toCharArray(wire, sizeof(wire));
            char *save = nullptr;
            char *version = strtok_r(wire, "\t\r\n", &save);
            char *job = strtok_r(nullptr, "\t\r\n", &save);
            char *id = strtok_r(nullptr, "\t\r\n", &save);
            char *url = strtok_r(nullptr, "\t\r\n", &save);
            char *cover = strtok_r(nullptr, "\t\r\n", &save);
            const bool removal = version && strcmp(version, "2") == 0 && url && strcmp(url, "remove") == 0;
            if (!version || (strcmp(version, "1") && !removal) || !job || !id || !url ||
                !BookUploadReceiver::validBookId(id) || strspn(job, "0123456789") != strlen(job)) {
                recordFailure(ReaderSyncPolicy::Error::Protocol);
                break;
            }
            errno = 0;
            const unsigned long long jobNumber = strtoull(job, nullptr, 10);
            if (errno == ERANGE || jobNumber <= after) {
                recordFailure(ReaderSyncPolicy::Error::Protocol);
                break; // Also prevents a loop against a server ignoring cursors.
            }
            after = jobNumber;
            char receipt[320]{};
            snprintf(receipt, sizeof(receipt),
                     "{\"job\":%s,\"state\":\"queued\",\"error\":\"interrupted-transfer\"}", job);
            if (!saveReceipt(receipt)) {
                recordFailure(ReaderSyncPolicy::Error::Storage);
                break;
            }
            if (removal) {
                const bool ok = BookUploadReceiver::archiveBook(id);
                snprintf(receipt, sizeof(receipt),
                         "{\"job\":%s,\"state\":\"%s\",\"error\":\"%s\"}", job,
                         ok ? "removed" : "queued", ok ? "" : "sd-remove-failed");
                if (!ok) recordFailure(ReaderSyncPolicy::Error::Storage);
                const bool ack = saveReceipt(receipt) && sendSavedReceipt(network);
                if (!ack && ok) recordFailure(ReaderSyncPolicy::Error::Storage);
                Serial.printf("SYNC REMOVE id=%s removed=%s ack=%s recoverable=trash\n", id,
                              ok ? "true" : "false", ack ? "true" : "false");
                if (ok) changed = true;
                if (!ack) break; // Preserve the durable receipt until acknowledged.
                ++handled;
                if (!ok) { hadFailures = true; continue; }
                ++removed;
                continue;
            }
            BookDownloadResult result{};
            bool ok = false;
            ReaderSyncPolicy::Error bookError = ReaderSyncPolicy::Error::Storage;
            char marker[96]{};
            snprintf(marker, sizeof(marker), "/books/%s/sync.pending", id);
            const bool existing = localReady(id) && !SD.exists(marker);
            if (existing) {
                ok = NetworkService::verifiedLocalDigest(id, result.sha256);
            } else {
                BookUploadReceiver::ensureBookDirectory(id);
                File pending = SD.open(marker, FILE_WRITE);
                const bool marked = pending && pending.print("1\n") == 2;
                pending.close();
                Serial.printf("SYNC STAGE id=%s stage=download\n", id);
                ok = marked && (localReady(id) || network.downloadFb2(url, id, result));
                if (!ok && marked && result.error[0]) {
                    bookError = ReaderSyncPolicy::downloadError(result.error, result.httpCode);
                }
                if (ok) {
                    Serial.printf("SYNC STAGE id=%s stage=prepare\n", id);
                    auto *info = static_cast<Fb2CacheInfo *>(heap_caps_calloc(
                        1, sizeof(Fb2CacheInfo), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                    ok = info != nullptr && (Fb2Cache::load(id, *info) || Fb2Cache::build(id, *info));
                    if (!ok) bookError = info ? ReaderSyncPolicy::Error::Preparation : ReaderSyncPolicy::Error::Memory;
                    if (!ok && info) Serial.printf("SYNC PREPARE_FAILED id=%s reason=%s\n", id, info->error);
                    heap_caps_free(info);
                }
                if (ok) {
                    if (cover && cover[0] && !paused.load()) {
                        Serial.printf("SYNC STAGE id=%s stage=cover\n", id);
                        BookDownloadResult ignored{};
                        network.downloadCover(cover, id, ignored);
                    }
                    char addedPath[96]{};
                    snprintf(addedPath, sizeof(addedPath), "/books/%s/added.txt", id);
                    if (!SD.exists(addedPath)) {
                        File added = SD.open(addedPath, FILE_WRITE);
                        if (added) added.printf("%lu\n", static_cast<unsigned long>(time(nullptr)));
                        added.close();
                    }
                    ok = SD.remove(marker) && NetworkService::verifiedLocalDigest(id, result.sha256);
                    if (ok) changed = true;
                }
            }
            if (!ok) recordFailure(bookError, result.httpCode);
            snprintf(receipt, sizeof(receipt),
                     "{\"job\":%s,\"state\":\"%s\",\"sha256\":\"%s\",\"error\":\"%s\"}",
                     job, ok ? "delivered" : "queued", ok ? result.sha256 : "",
                     ok ? "" : ReaderSyncPolicy::errorCode(bookError));
            const bool receiptSaved = saveReceipt(receipt);
            if (!receiptSaved) recordFailure(ReaderSyncPolicy::Error::Storage);
            const bool acknowledged = receiptSaved && sendSavedReceipt(network);
            Serial.printf("SYNC BOOK id=%s saved=%s ack=%s\n", id, ok ? "true" : "false", acknowledged ? "true" : "false");
            if (!acknowledged) break;
            ++handled;
            if (!ok) { hadFailures = true; continue; }
            ++delivered;
            deliveredCount = delivered;
        }
        network.disconnect();
        setWorkCancelCallback(nullptr);
        syncStatus = paused.load() || cancelled.load() ? AutomaticSync::Status::Cancelled
            : complete && !hadFailures ? AutomaticSync::Status::Complete
            : hadFailures ? AutomaticSync::Status::Failed
            : handled == 64 ? AutomaticSync::Status::More : AutomaticSync::Status::Failed;
        finished = true;
        running = false;
        Serial.printf("SYNC IDLE downloaded=%u removed=%u automatic=false status=%u reason=%s http=%d\n", delivered, removed,
                      static_cast<unsigned>(syncStatus.load()),
                      ReaderSyncPolicy::errorCode(lastError.load()), lastHttpCode.load());
    }
}
}

namespace AutomaticSync {
void start() {
    if (syncTask != nullptr) return;
    const uint64_t mac = ESP.getEfuseMac();
    snprintf(identity, sizeof(identity), "reader-%04x%08lx",
             static_cast<unsigned>(mac >> 32), static_cast<unsigned long>(mac));
    xTaskCreatePinnedToCore(worker, "book_sync", 16384, nullptr, 1, &syncTask, 0);
    Serial.printf("SYNC CONFIG device=%s automatic=false\n", identity);
}
bool request(bool exclusiveBusy) {
    exclusiveBusy = exclusiveBusy || WifiSetup::active();
    start();
    portENTER_CRITICAL(&syncGate);
    const bool allowed = ReaderSyncPolicy::canRequest(syncTask != nullptr, running.load(), exclusiveBusy);
    if (allowed) {
        paused = false;
        running = true;
        cancelled = false;
        finished = false;
        deliveredCount = 0;
        syncStatus = Status::Running;
        lastError = ReaderSyncPolicy::Error::None;
        lastHttpCode = 0;
    } else if (!running.load()) {
        lastError = exclusiveBusy ? ReaderSyncPolicy::Error::Busy : ReaderSyncPolicy::Error::Memory;
        lastHttpCode = 0;
        syncStatus = Status::Failed;
    }
    portEXIT_CRITICAL(&syncGate);
    if (allowed) xTaskNotifyGive(syncTask);
    return allowed;
}
void cancel() {
    cancelled = true;
    if (running.load()) syncStatus = Status::Cancelling;
}
Status status() { return syncStatus.load(); }
unsigned downloaded() { return deliveredCount.load(); }
bool takeFinished() { return finished.exchange(false); }
void setPaused(bool value) {
    portENTER_CRITICAL(&syncGate);
    paused = value;
    if (value && running.load()) syncStatus = Status::Cancelling;
    portEXIT_CRITICAL(&syncGate);
}
bool busy() { return running.load(); }
bool isPaused() { return paused.load(); }
ReaderSyncPolicy::Error error() { return lastError.load(); }
int httpCode() { return lastHttpCode.load(); }
bool cancelRequested() {
    return (paused.load() || cancelled.load()) && xTaskGetCurrentTaskHandle() == syncTask;
}
bool ownsNetworkSession() {
    return syncTask != nullptr && running.load() && xTaskGetCurrentTaskHandle() == syncTask;
}
bool takeLibraryChanged() { return changed.exchange(false); }
const char *deviceId() { return identity; }
}
