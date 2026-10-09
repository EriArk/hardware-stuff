#pragma once

#include <Arduino.h>

#include "provisioning_store.h"
#include "sync_policy.h"

struct SyncRequestResult {
    int httpCode = 0;
    ReaderSyncPolicy::Error error = ReaderSyncPolicy::Error::None;
};

struct NetworkStatus {
    bool configured = false;
    bool connected = false;
    bool timeValid = false;
    int32_t rssi = 0;
    uint32_t connectDurationMs = 0;
    uint8_t credentialSlot = 0;
    char error[64]{};
};

struct OpdsProbeResult {
    bool ok = false;
    bool tlsVerified = false;
    bool atomFeed = false;
    int httpCode = 0;
    int tlsError = 0;
    size_t responseBytes = 0;
    size_t entries = 0;
    uint32_t durationMs = 0;
    char contentType[64]{};
    char error[64]{};
};

struct OpdsFetchResult {
    bool ok = false;
    bool tlsVerified = false;
    int httpCode = 0;
    int tlsError = 0;
    size_t responseBytes = 0;
    uint32_t durationMs = 0;
    char contentType[64]{};
    char error[64]{};
};

struct BookDownloadResult {
    bool ok = false;
    bool tlsVerified = false;
    int httpCode = 0;
    int tlsError = 0;
    size_t responseBytes = 0;
    uint32_t durationMs = 0;
    char contentType[64]{};
    char sha256[65]{};
    char error[64]{};
};

class NetworkService {
public:
    void invalidateConfiguration();
    // timeoutMs is the budget per saved network, not shared between SSIDs.
    bool connect(NetworkStatus &status, uint32_t timeoutMs = ReaderSyncPolicy::kWifiAttemptMs,
                 uint8_t requestedCredentialSlot = 0);
    void disconnect();
    NetworkStatus status();
    OpdsProbeResult probeRoot(uint32_t timeoutMs = 20000);
    bool copyOpdsRootUrl(char *target, size_t capacity, char *error,
                         size_t errorCapacity);
    bool fetchOpds(const char *url, String &body, OpdsFetchResult &result,
                   uint32_t timeoutMs = 20000);
    bool downloadFb2(const char *url, const char *bookId,
                     BookDownloadResult &result,
                     uint32_t timeoutMs = 30000);
    bool downloadCover(const char *url, const char *bookId,
                       BookDownloadResult &result,
                       uint32_t timeoutMs = 30000);
    // Bounded account-authenticated sync protocol; no OPDS catalogue parsing.
    bool syncRequest(const char *path, const char *json, String &body,
                     SyncRequestResult &result, uint32_t timeoutMs = 12000);
    static bool verifiedLocalDigest(const char *bookId, char digest[65]);

private:
    bool loadConfiguration(char *error, size_t errorCapacity);
    bool synchronizeTime(char *error, size_t errorCapacity,
                         uint32_t timeoutMs = 15000);
    bool isAllowedOpdsUrl(const char *url) const;

    bool configurationLoaded_ = false;
    ProvisioningConfig configuration_{};
    NetworkStatus lastStatus_{};
};
