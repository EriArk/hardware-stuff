#include "network_service.h"
#include "automatic_sync.h"
#include "work_progress.h"
#include "download_writer.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>

#include "book_upload.h"
#include "cover_cache.h"
#include "provisioning_store.h"
#include "wifi_credentials.h"

namespace {
std::atomic<unsigned> wifiDisconnectReason{0};
std::atomic<unsigned> wifiNoApEvents{0};

constexpr time_t kMinimumTrustedEpoch = 1700000000;
constexpr size_t kMaximumOpdsResponseBytes = 192U * 1024U;
constexpr size_t kBookTransferBufferBytes = 4096;
// Write one FAT sector at a time: the Arduino 2.0.17 SD multi-block
// transaction intermittently fails with EIO on the connected card. Network
// reads remain 4 KiB; only the storage staging buffer/transaction is smaller.
constexpr size_t kSdWriteBlockBytes = 512;
constexpr size_t kFb2ValidationPrefixBytes = 16U * 1024U;
constexpr uint32_t kBookTransferIdleTimeoutMs = 30000;

struct SdDownloadSink {
    int fd = -1;
    int error = 0;
    ssize_t write(const uint8_t *data, size_t size) {
        const ssize_t count = ::write(fd, data, size);
        if (count < 0) error = errno;
        return count;
    }
    bool sync() {
        if (::fsync(fd) == 0) return true;
        error = errno;
        return false;
    }
};

class SdDownloadWriter {
public:
    SdDownloadWriter() : staging_(static_cast<uint8_t *>(heap_caps_malloc(
        kSdWriteBlockBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT))),
        buffer_(sink_, staging_, kSdWriteBlockBytes) {}
    ~SdDownloadWriter() { close(); heap_caps_free(staging_); }
    bool allocated() const { return staging_ != nullptr; }
    bool open(const char *path) {
        char mounted[128]{};
        if (!staging_ || snprintf(mounted, sizeof(mounted), "/sd%s", path) >= static_cast<int>(sizeof(mounted))) return false;
        sink_.fd = ::open(mounted, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (sink_.fd < 0) sink_.error = errno;
        return sink_.fd >= 0;
    }
    bool append(const uint8_t *data, size_t size) { return buffer_.append(data, size); }
    bool finish() { return buffer_.finish(); }
    bool close() {
        if (sink_.fd < 0) return true;
        const int fd = sink_.fd; sink_.fd = -1;
        if (::close(fd) == 0) return true;
        sink_.error = errno; return false;
    }
    int error() const { return buffer_.error() ? buffer_.error() : sink_.error; }
private:
    uint8_t *staging_;
    SdDownloadSink sink_;
    DownloadWriteBuffer<SdDownloadSink> buffer_;
};

// Google Trust Services Root R4 and its currently served GlobalSign Root R1
// cross-sign anchor, taken from the certificate authorities' official
// repositories. Roots, not the configured server's short-lived leaf, are
// trusted so ordinary leaf renewal does not require new firmware.
constexpr char kTrustedRoots[] PROGMEM = R"PEM(-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBggUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIDdTCCAl2gAwIBAgILBAAAAAABFUtaw5QwDQYJKoZIhvcNAQEFBQAwVzELMAkG
A1UEBhMCQkUxGTAXBgNVBAoTEEdsb2JhbFNpZ24gbnYtc2ExEDAOBgNVBAsTB1Jv
b3QgQ0ExGzAZBgNVBAMTEkdsb2JhbFNpZ24gUm9vdCBDQTAeFw05ODA5MDExMjAw
MDBaFw0yODAxMjgxMjAwMDBaMFcxCzAJBgNVBAYTAkJFMRkwFwYDVQQKExBHbG9i
YWxTaWduIG52LXNhMRAwDgYDVQQLEwdSb290IENBMRswGQYDVQQDExJHbG9iYWxT
aWduIFJvb3QgQ0EwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDaDuaZ
jc6j40+Kfvvxi4Mla+pIH/EqsLmVEQS98GPR4mdmzxzdzxtIK+6NiY6arymAZavp
xy0Sy6scTHAHoT0KMM0VjU/43dSMUBUc71DuxC73/OlS8pF94G3VNTCOXkNz8kHp
1Wrjsok6Vjk4bwY8iGlbKk3Fp1S4bInMm/k8yuX9ifUSPJJ4ltbcdG6TRGHRjcdG
snUOhugZitVtbNV4FpWi6cgKOOvyJBNPc1STE4U6G7weNLWLBYy5d4ux2x8gkasJ
U26Qzns3dLlwR5EiUWMWea6xrkEmCMgZK9FGqkjWZCrXgzT/LCrBbBlDSgeF59N8
9iFo7+ryUp9/k5DPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8E
BTADAQH/MB0GA1UdDgQWBBRge2YaRQ2XyolQL30EzTSo//z9SzANBgkqhkiG9w0B
AQUFAAOCAQEA1nPnfE920I2/7LqivjTFKDK1fPxsnCwrvQmeU79rXqoRSLblCKOz
yj1hTdNGCbM+w6DjY1Ub8rrvrTnhQ7k4o+YviiY776BQVvnGCv04zcQLcFGUl5gE
38NflNUVyRRBnMRddWQVDf9VMOyGj/8N7yy5Y0b2qvzfvGn9LhJIZJrglfCm7ymP
AbEVtQwdpf5pLGkkeB6zpxxxYu7KyJesF12KwvhHhm4qxFYxldBniYUr+WymXUad
DKqC5JlR3XC321Y9YeRq4VzW9v493kHMB65jUr9TU/Qr6cf9tveCX4XSQRjbgbME
HMUfpIBvFSDJ3gyICh3WZlXi/EjJKSZp4A==
-----END CERTIFICATE-----
)PEM";

void setError(char *target, size_t capacity, const char *value) {
    if (target != nullptr && capacity > 0) {
        snprintf(target, capacity, "%s", value == nullptr ? "" : value);
    }
}

size_t countOccurrences(const String &text, const char *needle) {
    size_t count = 0;
    int position = 0;
    while (true) {
        position = text.indexOf(needle, position);
        if (position < 0) {
            break;
        }
        ++count;
        position += strlen(needle);
    }
    return count;
}

void digestToHex(const uint8_t *digest, char *target, size_t capacity) {
    if (target == nullptr || capacity < 65) {
        return;
    }
    for (size_t index = 0; index < 32; ++index) {
        snprintf(target + index * 2, 3, "%02x", digest[index]);
    }
    target[64] = '\0';
}

bool hashFile(const char *path, uint8_t digest[32], size_t &bytes) {
    bytes = 0;
    File input = SD.open(path, FILE_READ);
    if (!input) {
        return false;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(
        kBookTransferBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        input.close();
        mbedtls_sha256_free(&sha);
        return false;
    }
    while (input.available()) {
        reportWorkProgress();
        const size_t count = workCancelled() ? 0 : input.read(buffer, kBookTransferBufferBytes);
        if (count == 0) {
            input.close();
            heap_caps_free(buffer);
            mbedtls_sha256_free(&sha);
            return false;
        }
        mbedtls_sha256_update_ret(&sha, buffer, count);
        bytes += count;
    }
    input.close();
    heap_caps_free(buffer);
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);
    return true;
}

bool acceptedFb2ContentType(const char *value) {
    String contentType(value == nullptr ? "" : value);
    contentType.toLowerCase();
    return contentType.indexOf("fictionbook") >= 0 ||
           contentType.indexOf("application/xml") >= 0 ||
           contentType.indexOf("text/xml") >= 0 ||
           contentType.indexOf("application/octet-stream") >= 0 ||
           contentType.indexOf("binary/octet-stream") >= 0;
}

bool acceptedCoverContentType(const char *value) {
    String contentType(value == nullptr ? "" : value);
    contentType.toLowerCase();
    return contentType.indexOf("image/jpeg") >= 0 ||
           contentType.indexOf("image/jpg") >= 0 ||
           contentType.indexOf("application/octet-stream") >= 0;
}

}  // namespace

bool NetworkService::loadConfiguration(char *error, size_t errorCapacity) {
    if (configurationLoaded_) {
        setError(error, errorCapacity, "");
        return true;
    }
    memset(&configuration_, 0, sizeof(configuration_));
    if (!ProvisioningStore::load(configuration_, error, errorCapacity)) {
        return false;
    }
    WifiCredential wifi{};
    if (!WifiCredentials::load(wifi)) {
        setError(error, errorCapacity, "wifi-not-configured");
        return false;
    }
    memcpy(configuration_.wifiSsid, wifi.ssid, sizeof(wifi.ssid));
    memcpy(configuration_.wifiPassword, wifi.password, sizeof(wifi.password));
    memset(&wifi, 0, sizeof(wifi));
    configurationLoaded_ = true;
    return true;
}

void NetworkService::invalidateConfiguration() {
    disconnect();
    configurationLoaded_ = false;
    memset(&configuration_, 0, sizeof(configuration_));
    lastStatus_ = NetworkStatus{};
}

bool NetworkService::synchronizeTime(char *error, size_t errorCapacity,
                                     uint32_t timeoutMs) {
    if (time(nullptr) >= kMinimumTrustedEpoch) {
        setError(error, errorCapacity, "");
        return true;
    }

    configTzTime("UTC0", "time.google.com", "pool.ntp.org",
                 "time.cloudflare.com");
    const uint32_t startedAt = millis();
    while (millis() - startedAt < timeoutMs) {
        if (AutomaticSync::cancelRequested()) break;
        reportWorkProgress();
        if (time(nullptr) >= kMinimumTrustedEpoch) {
            setError(error, errorCapacity, "");
            return true;
        }
        delay(100);
    }
    setError(error, errorCapacity, "time-sync-timeout");
    return false;
}

bool NetworkService::connect(NetworkStatus &status, uint32_t timeoutMs,
                             uint8_t requestedCredentialSlot) {
    status = NetworkStatus{};
    if (AutomaticSync::cancelRequested()) {
        setError(status.error, sizeof(status.error), "sync-paused");
        return false;
    }
    if (!AutomaticSync::ownsNetworkSession()) {
        setError(status.error, sizeof(status.error), "sync-only");
        return false;
    }
    char error[64]{};
    if (!loadConfiguration(error, sizeof(error))) {
        setError(status.error, sizeof(status.error), error);
        lastStatus_ = status;
        return false;
    }
    status.configured = true;

    if (WiFi.status() == WL_CONNECTED) {
        status.connected = true;
        status.rssi = WiFi.RSSI();
        status.timeValid = synchronizeTime(status.error, sizeof(status.error));
        if (!status.timeValid) {
            disconnect();
            status.connected = false;
        }
        lastStatus_ = status;
        return status.timeValid;
    }

    WifiCredential fallback{};
    char fallbackError[32]{};
    const bool hasFallback = ProvisioningStore::loadFallbackWifi(
        fallback, fallbackError, sizeof(fallbackError));
    const bool distinctFallback =
        hasFallback && strcmp(fallback.ssid, configuration_.wifiSsid) != 0;
    const bool fallbackOnly = requestedCredentialSlot == 2;
    if (fallbackOnly && !hasFallback) {
        setError(status.error, sizeof(status.error),
                 "fallback-wifi-not-configured");
        status.configured = true;
        lastStatus_ = status;
        return false;
    }

    const uint32_t startedAt = millis();
    const auto wifiDiagnostic = WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        wifiDisconnectReason = info.wifi_sta_disconnected.reason;
        if (info.wifi_sta_disconnected.reason == 201) ++wifiNoApEvents;
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    // The radio is only enabled for an explicit sync session. Keep it awake
    // during association and transfers; disconnect() powers it off afterwards.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(false);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setHostname("abyss-reader");
    const auto tryCredential = [](const char *ssid, const char *password,
                                  uint32_t durationMs) {
        wifiDisconnectReason = 0;
        wifiNoApEvents = 0;
        WiFi.begin(ssid, password);
        const uint32_t attemptStartedAt = millis();
        uint32_t retryAt = 0;
        while (millis() - attemptStartedAt < durationMs &&
               WiFi.status() != WL_CONNECTED) {
            if (AutomaticSync::cancelRequested()) return false;
            // A transient scan miss otherwise leaves STA idle for the entire
            // 20-second budget when automatic/background reconnect is disabled.
            if (wifiNoApEvents.exchange(0)) retryAt = millis() + 1000;
            if (retryAt && static_cast<int32_t>(millis() - retryAt) >= 0) {
                retryAt = 0;
                WiFi.reconnect();
            }
            reportWorkProgress();
            delay(100);
        }
        const bool connected = WiFi.status() == WL_CONNECTED;
        // Numeric diagnostics only; SSIDs, passwords and addresses stay private.
        Serial.printf("NETWORK WIFI_ATTEMPT connected=%s status=%d reason=%u duration_ms=%lu\n",
                      connected ? "true" : "false", int(WiFi.status()), wifiDisconnectReason.load(),
                      static_cast<unsigned long>(millis() - attemptStartedAt));
        return connected;
    };

    if (!fallbackOnly &&
        tryCredential(configuration_.wifiSsid, configuration_.wifiPassword,
                      timeoutMs)) {
        status.credentialSlot = 1;
    } else if (!AutomaticSync::cancelRequested() && (fallbackOnly || distinctFallback)) {
        WiFi.disconnect(false, false);
        delay(50);
        if (tryCredential(fallback.ssid, fallback.password, timeoutMs)) {
            status.credentialSlot = 2;
        }
    }
    memset(&fallback, 0, sizeof(fallback));
    WiFi.removeEvent(wifiDiagnostic);
    status.connectDurationMs = millis() - startedAt;
    status.connected = WiFi.status() == WL_CONNECTED;
    if (!status.connected) {
        setError(status.error, sizeof(status.error), "wifi-connect-timeout");
        disconnect();
        status.configured = true;
        lastStatus_ = status;
        return false;
    }

    status.rssi = WiFi.RSSI();
    status.timeValid = synchronizeTime(status.error, sizeof(status.error));
    if (!status.timeValid) {
        disconnect();
        status.connected = false;
        status.configured = true;
        lastStatus_ = status;
        return false;
    }
    setError(status.error, sizeof(status.error), "");
    lastStatus_ = status;
    return true;
}

void NetworkService::disconnect() {
    if (WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
        delay(10);
    }
    lastStatus_.connected = false;
    lastStatus_.rssi = 0;
}

NetworkStatus NetworkService::status() {
    NetworkStatus result = lastStatus_;
    char error[64]{};
    result.configured = loadConfiguration(error, sizeof(error));
    result.connected = WiFi.status() == WL_CONNECTED;
    result.rssi = result.connected ? WiFi.RSSI() : 0;
    result.timeValid = time(nullptr) >= kMinimumTrustedEpoch;
    if (!result.configured) {
        setError(result.error, sizeof(result.error), error);
    }
    return result;
}

bool NetworkService::copyOpdsRootUrl(char *target, size_t capacity,
                                     char *error, size_t errorCapacity) {
    if (target == nullptr || capacity == 0) {
        setError(error, errorCapacity, "invalid-target");
        return false;
    }
    target[0] = '\0';
    if (!loadConfiguration(error, errorCapacity)) {
        return false;
    }
    if (strnlen(configuration_.opdsUrl, sizeof(configuration_.opdsUrl)) >=
        capacity) {
        setError(error, errorCapacity, "opds-url-too-long");
        return false;
    }
    snprintf(target, capacity, "%s", configuration_.opdsUrl);
    setError(error, errorCapacity, "");
    return true;
}

bool NetworkService::isAllowedOpdsUrl(const char *url) const {
    if (url == nullptr || strncmp(url, "https://", 8) != 0 ||
        strncmp(configuration_.opdsUrl, "https://", 8) != 0) {
        return false;
    }
    const char *configuredPath = strchr(configuration_.opdsUrl + 8, '/');
    const char *candidatePath = strchr(url + 8, '/');
    const size_t configuredOriginLength = configuredPath == nullptr
                                              ? strlen(configuration_.opdsUrl)
                                              : configuredPath -
                                                    configuration_.opdsUrl;
    const size_t candidateOriginLength =
        candidatePath == nullptr ? strlen(url) : candidatePath - url;
    return configuredOriginLength == candidateOriginLength &&
           strncasecmp(configuration_.opdsUrl, url, configuredOriginLength) ==
               0;
}

bool NetworkService::syncRequest(const char *path, const char *json,
                                String &body, SyncRequestResult &result,
                                uint32_t timeoutMs) {
    body = String();
    result = SyncRequestResult{};
    char error[64]{};
    if (path == nullptr || strncmp(path, "/reader-api/device/", 19) != 0) {
        result.error = ReaderSyncPolicy::Error::Protocol;
        return false;
    }
    if (!loadConfiguration(error, sizeof(error))) {
        result.error = ReaderSyncPolicy::Error::Configuration;
        return false;
    }
    String target(configuration_.opdsUrl);
    const int slash = target.indexOf('/', 8);
    if (slash >= 0) target.remove(slash);
    target += path;
    if (!isAllowedOpdsUrl(target.c_str())) {
        result.error = ReaderSyncPolicy::Error::Configuration;
        return false;
    }
    NetworkStatus connection{};
    // Association/DHCP and HTTP have independent budgets.
    if (!connect(connection, ReaderSyncPolicy::kWifiAttemptMs)) {
        result.error = ReaderSyncPolicy::connectionError(connection.error);
        return false;
    }
    WiFiClientSecure client;
    client.setCACert(kTrustedRoots);
    client.setHandshakeTimeout(12);
    HTTPClient http;
    http.useHTTP10(true);
    http.setReuse(false);
    http.setUserAgent("AbyssTail-EInk/0.18");
    http.setConnectTimeout(timeoutMs);
    http.setTimeout(timeoutMs);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(client, target)) {
        result.error = ReaderSyncPolicy::Error::Transport;
        return false;
    }
    http.setAuthorization(configuration_.opdsUsername, configuration_.opdsPassword);
    http.addHeader("Accept", "text/plain, application/json");
    if (json != nullptr) {
        http.addHeader("Content-Type", "application/json");
        result.httpCode = http.POST(reinterpret_cast<uint8_t *>(const_cast<char *>(json)), strlen(json));
    } else {
        result.httpCode = http.GET();
    }
    if (result.httpCode < 0) {
        char tlsMessage[96]{};
        const int tlsError = client.lastError(tlsMessage, sizeof(tlsMessage));
        Serial.printf("NETWORK SYNC_HTTP code=%d tls=%d wifi=%d rssi=%d heap=%u largest=%u\n",
                      result.httpCode, tlsError, int(WiFi.status()), int(WiFi.RSSI()),
                      unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                      unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    }
    const int size = http.getSize();
    bool ok = result.httpCode == 204 || (result.httpCode == 200 && size >= 0 && size <= 2048);
    if (ok && result.httpCode == 200) {
        body = http.getString();
        ok = body.length() == static_cast<size_t>(size);
    }
    if (!ok) result.error = result.httpCode == 200 ? ReaderSyncPolicy::Error::Protocol
        : ReaderSyncPolicy::httpError(result.httpCode);
    http.end();
    client.stop();
    return ok;
}

bool NetworkService::verifiedLocalDigest(const char *bookId, char digest[65]) {
    char path[96]{};
    uint8_t bytesDigest[32]{};
    size_t bytes = 0;
    if (!BookUploadReceiver::bookPath(bookId, path, sizeof(path)) ||
        !hashFile(path, bytesDigest, bytes) || bytes < BookUploadReceiver::kMinimumBookBytes) {
        return false;
    }
    digestToHex(bytesDigest, digest, 65);
    return true;
}

bool NetworkService::fetchOpds(const char *url, String &body,
                               OpdsFetchResult &result,
                               uint32_t timeoutMs) {
    body = String();
    result = OpdsFetchResult{};
    const uint32_t startedAt = millis();
    char configurationError[64]{};
    if (!loadConfiguration(configurationError, sizeof(configurationError))) {
        setError(result.error, sizeof(result.error), configurationError);
        result.durationMs = millis() - startedAt;
        return false;
    }
    const char *target =
        url == nullptr || url[0] == '\0' ? configuration_.opdsUrl : url;
    if (!isAllowedOpdsUrl(target)) {
        setError(result.error, sizeof(result.error), "cross-origin-blocked");
        result.durationMs = millis() - startedAt;
        return false;
    }

    NetworkStatus connection{};
    if (!connect(connection, timeoutMs)) {
        setError(result.error, sizeof(result.error), connection.error);
        result.durationMs = millis() - startedAt;
        disconnect();
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(kTrustedRoots);
    client.setHandshakeTimeout(15);

    HTTPClient http;
    // http.end() may retain a keep-alive socket. Never leave one alive when
    // stopping the Wi-Fi driver; its pending RX buffers still belong to Wi-Fi.
    http.setReuse(false);
    http.setConnectTimeout(static_cast<int32_t>(timeoutMs));
    http.setTimeout(static_cast<uint16_t>(timeoutMs));
    http.setUserAgent("AbyssTail-EInk/0.9");
    // Authentication must never be replayed to a redirect target. The OPDS
    // facade emits absolute same-origin links, so redirects are unnecessary.
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const char *headerKeys[] = {"Content-Type"};
    http.collectHeaders(headerKeys, 1);

    if (!http.begin(client, target)) {
        setError(result.error, sizeof(result.error), "http-begin-failed");
        result.durationMs = millis() - startedAt;
        client.stop();
        disconnect();
        return false;
    }
    http.setAuthorization(configuration_.opdsUsername,
                          configuration_.opdsPassword);
    http.addHeader("Accept", "application/atom+xml;profile=opds-catalog, "
                             "application/atom+xml;q=0.9, application/xml;q=0.8");

    Serial.printf("NETWORK TLS_HEAP free=%u largest=%u\n",
                  static_cast<unsigned>(heap_caps_get_free_size(
                      MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(
                      MALLOC_CAP_INTERNAL)));
    result.httpCode = http.GET();
    if (result.httpCode < 0) {
        char tlsMessage[96]{};
        result.tlsError = client.lastError(tlsMessage, sizeof(tlsMessage));
        Serial.printf("NETWORK HTTP_GET code=%d detail=%s tls=%d message=%s\n",
                      result.httpCode,
                      HTTPClient::errorToString(result.httpCode).c_str(),
                      result.tlsError,
                      tlsMessage[0] == '\0' ? "none" : tlsMessage);
        memset(tlsMessage, 0, sizeof(tlsMessage));
    }
    if (result.httpCode > 0) {
        const String contentType = http.header("Content-Type");
        snprintf(result.contentType, sizeof(result.contentType), "%s",
                 contentType.c_str());
    }
    if (result.httpCode != HTTP_CODE_OK) {
        setError(result.error, sizeof(result.error),
                 result.httpCode < 0 ? "tls-or-network-failed"
                                     : "unexpected-http-status");
        http.end();
        client.stop();
        result.durationMs = millis() - startedAt;
        disconnect();
        return false;
    }

    const int announcedLength = http.getSize();
    if (announcedLength > static_cast<int>(kMaximumOpdsResponseBytes)) {
        setError(result.error, sizeof(result.error), "response-too-large");
        http.end();
        client.stop();
        result.durationMs = millis() - startedAt;
        disconnect();
        return false;
    }
    body = http.getString();
    result.responseBytes = body.length();
    result.tlsVerified = true;
    const bool xmlContent = strstr(result.contentType, "xml") != nullptr ||
                            strstr(result.contentType, "atom") != nullptr;
    result.ok = !body.isEmpty() && body.length() <= kMaximumOpdsResponseBytes &&
                xmlContent;
    if (!result.ok) {
        body = String();
        setError(result.error, sizeof(result.error),
                 xmlContent ? "invalid-opds-body" : "invalid-content-type");
    }
    http.end();
    client.stop();
    result.durationMs = millis() - startedAt;
    disconnect();
    return result.ok;
}

bool NetworkService::downloadFb2(const char *url, const char *bookId,
                                 BookDownloadResult &result,
                                 uint32_t timeoutMs) {
    result = BookDownloadResult{};
    const uint32_t startedAt = millis();
    char configurationError[64]{};
    if (!BookUploadReceiver::validBookId(bookId)) {
        setError(result.error, sizeof(result.error), "invalid-book-id");
        return false;
    }
    if (!loadConfiguration(configurationError, sizeof(configurationError))) {
        setError(result.error, sizeof(result.error), configurationError);
        return false;
    }
    if (!isAllowedOpdsUrl(url)) {
        setError(result.error, sizeof(result.error), "cross-origin-blocked");
        return false;
    }
    if (!BookUploadReceiver::ensureBookDirectory(bookId)) {
        setError(result.error, sizeof(result.error), "mkdir-failed");
        return false;
    }

    char partialPath[96]{};
    if (!BookUploadReceiver::partialBookPath(bookId, partialPath,
                                             sizeof(partialPath))) {
        setError(result.error, sizeof(result.error), "path-failed");
        return false;
    }
    SD.remove(partialPath);

    // Reserve the small disk buffer before TLS consumes internal memory.
    SdDownloadWriter output;
    if (!output.allocated()) {
        setError(result.error, sizeof(result.error), "transfer-allocation-failed");
        return false;
    }
    NetworkStatus connection{};
    if (!connect(connection, timeoutMs)) {
        setError(result.error, sizeof(result.error), connection.error);
        result.durationMs = millis() - startedAt;
        disconnect();
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(kTrustedRoots);
    client.setHandshakeTimeout(15);
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(static_cast<int32_t>(timeoutMs));
    http.setTimeout(static_cast<uint16_t>(timeoutMs));
    http.setUserAgent("AbyssTail-EInk/0.12");
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const char *headerKeys[] = {"Content-Type"};
    http.collectHeaders(headerKeys, 1);

    const auto fail = [&](const char *error) {
        setError(result.error, sizeof(result.error), error);
        Serial.printf("NETWORK BOOK_FAILED id=%s reason=%s http=%d tls=%d bytes=%lu\n",
                      bookId, error, result.httpCode, result.tlsError,
                      static_cast<unsigned long>(result.responseBytes));
        http.end();
        client.stop();
        SD.remove(partialPath);
        result.durationMs = millis() - startedAt;
        disconnect();
        return false;
    };

    if (!http.begin(client, url)) {
        return fail("http-begin-failed");
    }
    http.setAuthorization(configuration_.opdsUsername,
                          configuration_.opdsPassword);
    http.addHeader("Accept", "application/x-fictionbook+xml, "
                             "application/xml;q=0.9, "
                             "application/octet-stream;q=0.8");
    result.httpCode = http.GET();
    if (result.httpCode < 0) {
        char tlsMessage[96]{};
        result.tlsError = client.lastError(tlsMessage, sizeof(tlsMessage));
        memset(tlsMessage, 0, sizeof(tlsMessage));
    }
    if (result.httpCode > 0) {
        const String contentType = http.header("Content-Type");
        snprintf(result.contentType, sizeof(result.contentType), "%s",
                 contentType.c_str());
    }
    if (result.httpCode != HTTP_CODE_OK) {
        return fail(result.httpCode < 0 ? "tls-or-network-failed"
                                        : "unexpected-http-status");
    }
    if (!acceptedFb2ContentType(result.contentType)) {
        return fail("invalid-content-type");
    }

    const int announcedLength = http.getSize();
    if (AutomaticSync::ownsNetworkSession()) AutomaticSync::reportProgress(SyncProgress::Stage::Download,0,announcedLength>0?announcedLength:0);
    if (announcedLength >= 0 &&
        (announcedLength <
             static_cast<int>(BookUploadReceiver::kMinimumBookBytes) ||
         announcedLength >
             static_cast<int>(BookUploadReceiver::kMaximumBookBytes))) {
        return fail("invalid-size");
    }

    if (!output.open(partialPath)) {
        return fail("open-part-failed");
    }
    char *prefix = static_cast<char *>(heap_caps_malloc(
        kFb2ValidationPrefixBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(
        kBookTransferBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (prefix == nullptr || buffer == nullptr) {
        heap_caps_free(prefix);
        heap_caps_free(buffer);
        output.close();
        return fail("transfer-allocation-failed");
    }
    memset(prefix, 0, kFb2ValidationPrefixBytes + 1);

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    WiFiClient *stream = http.getStreamPtr();
    size_t prefixBytes = 0;
    size_t received = 0;
    uint32_t lastDataAt = millis();
    bool transferOk = true;
    const char *transferError = "stream-failed";

    while (http.connected() || stream->available() > 0) {
        reportWorkProgress();
        if (AutomaticSync::cancelRequested()) {
            transferOk = false;
            transferError = "sync-paused";
            break;
        }
        const int available = stream->available();
        if (available <= 0) {
            if (millis() - lastDataAt > kBookTransferIdleTimeoutMs) {
                transferOk = false;
                transferError = "transfer-timeout";
                break;
            }
            delay(2);
            continue;
        }
        const size_t requested =
            min(static_cast<size_t>(available), kBookTransferBufferBytes);
        const size_t count = stream->readBytes(buffer, requested);
        if (count == 0) {
            transferOk = false;
            break;
        }
        if (received + count > BookUploadReceiver::kMaximumBookBytes) {
            transferOk = false;
            transferError = "response-too-large";
            break;
        }
        if (!output.append(buffer, count)) {
            transferOk = false;
            transferError = "sd-write-failed";
            break;
        }
        mbedtls_sha256_update_ret(&sha, buffer, count);
        const size_t prefixCopy =
            min(count, kFb2ValidationPrefixBytes - prefixBytes);
        if (prefixCopy > 0) {
            memcpy(prefix + prefixBytes, buffer, prefixCopy);
            prefixBytes += prefixCopy;
            prefix[prefixBytes] = '\0';
        }
        received += count;
        if (AutomaticSync::ownsNetworkSession()) AutomaticSync::reportProgress(SyncProgress::Stage::Download,received,announcedLength>0?announcedLength:0);
        lastDataAt = millis();
        if (announcedLength >= 0 &&
            received == static_cast<size_t>(announcedLength)) {
            break;
        }
    }
    if (transferOk && !output.finish()) {
        transferOk = false;
        transferError = "sd-write-failed";
    }
    const bool closed = output.close();
    if (!closed && transferOk) { transferOk = false; transferError = "sd-write-failed"; }
    if (!transferOk && strcmp(transferError, "sd-write-failed") == 0)
        Serial.printf("NETWORK SD_WRITE errno=%d bytes=%lu internal_free=%lu internal_largest=%lu\n",
            output.error(), static_cast<unsigned long>(received),
            static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));

    uint8_t transportDigest[32]{};
    mbedtls_sha256_finish_ret(&sha, transportDigest);
    mbedtls_sha256_free(&sha);
    const bool fb2Marker = strstr(prefix, "<FictionBook") != nullptr;
    heap_caps_free(prefix);
    heap_caps_free(buffer);
    result.responseBytes = received;

    if (!transferOk) {
        return fail(transferError);
    }
    if (received < BookUploadReceiver::kMinimumBookBytes ||
        (announcedLength >= 0 &&
         received != static_cast<size_t>(announcedLength))) {
        return fail("incomplete-response");
    }
    if (!fb2Marker) {
        return fail("invalid-fb2-body");
    }

    uint8_t persistedDigest[32]{};
    size_t persistedBytes = 0;
    if (!hashFile(partialPath, persistedDigest, persistedBytes) ||
        persistedBytes != received ||
        memcmp(transportDigest, persistedDigest, sizeof(transportDigest)) !=
            0) {
        char transportHex[65]{}, persistedHex[65]{};
        digestToHex(transportDigest, transportHex, sizeof(transportHex));
        digestToHex(persistedDigest, persistedHex, sizeof(persistedHex));
        Serial.printf("NETWORK VERIFY_FAILED received=%lu persisted=%lu transport=%s persisted_sha=%s\n",
                      static_cast<unsigned long>(received), static_cast<unsigned long>(persistedBytes),
                      transportHex, persistedHex);
        return fail("sd-verification-failed");
    }
    digestToHex(persistedDigest, result.sha256, sizeof(result.sha256));
    if (!BookUploadReceiver::invalidateDerivedFiles(bookId)) {
        return fail("cache-invalidate-failed");
    }
    if (!BookUploadReceiver::publishPartial(bookId)) {
        return fail("publish-failed");
    }

    result.ok = true;
    result.tlsVerified = true;
    setError(result.error, sizeof(result.error), "");
    http.end();
    client.stop();
    result.durationMs = millis() - startedAt;
    disconnect();
    return true;
}

bool NetworkService::downloadCover(const char *url, const char *bookId,
                                   BookDownloadResult &result,
                                   uint32_t timeoutMs) {
    result = BookDownloadResult{};
    const uint32_t startedAt = millis();
    char configurationError[64]{};
    if (!BookUploadReceiver::validBookId(bookId)) {
        setError(result.error, sizeof(result.error), "invalid-book-id");
        return false;
    }
    if (!loadConfiguration(configurationError, sizeof(configurationError))) {
        setError(result.error, sizeof(result.error), configurationError);
        return false;
    }
    if (!isAllowedOpdsUrl(url)) {
        setError(result.error, sizeof(result.error), "cross-origin-blocked");
        return false;
    }
    if (!BookUploadReceiver::ensureBookDirectory(bookId)) {
        setError(result.error, sizeof(result.error), "mkdir-failed");
        return false;
    }
    char partialPath[96]{};
    if (!CoverCache::partialSourcePath(bookId, partialPath,
                                       sizeof(partialPath))) {
        setError(result.error, sizeof(result.error), "path-failed");
        return false;
    }
    SD.remove(partialPath);

    SdDownloadWriter output;
    if (!output.allocated()) {
        setError(result.error, sizeof(result.error), "transfer-allocation-failed");
        return false;
    }
    NetworkStatus connection{};
    if (!connect(connection, timeoutMs)) {
        setError(result.error, sizeof(result.error), connection.error);
        result.durationMs = millis() - startedAt;
        disconnect();
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(kTrustedRoots);
    client.setHandshakeTimeout(15);
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(static_cast<int32_t>(timeoutMs));
    http.setTimeout(static_cast<uint16_t>(timeoutMs));
    http.setUserAgent("AbyssTail-EInk/0.16");
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const char *headerKeys[] = {"Content-Type"};
    http.collectHeaders(headerKeys, 1);
    const auto fail = [&](const char *error) {
        setError(result.error, sizeof(result.error), error);
        http.end();
        client.stop();
        SD.remove(partialPath);
        result.durationMs = millis() - startedAt;
        disconnect();
        return false;
    };
    if (!http.begin(client, url)) {
        return fail("http-begin-failed");
    }
    http.setAuthorization(configuration_.opdsUsername,
                          configuration_.opdsPassword);
    http.addHeader("Accept", "image/jpeg, image/*;q=0.8");
    result.httpCode = http.GET();
    if (result.httpCode < 0) {
        char tlsMessage[96]{};
        result.tlsError = client.lastError(tlsMessage, sizeof(tlsMessage));
        memset(tlsMessage, 0, sizeof(tlsMessage));
    }
    if (result.httpCode > 0) {
        const String contentType = http.header("Content-Type");
        snprintf(result.contentType, sizeof(result.contentType), "%s",
                 contentType.c_str());
    }
    if (result.httpCode != HTTP_CODE_OK) {
        return fail(result.httpCode < 0 ? "tls-or-network-failed"
                                        : "unexpected-http-status");
    }
    if (!acceptedCoverContentType(result.contentType)) {
        return fail("invalid-content-type");
    }
    const int announcedLength = http.getSize();
    if (AutomaticSync::ownsNetworkSession()) AutomaticSync::reportProgress(SyncProgress::Stage::Cover,0,announcedLength>0?announcedLength:0);
    if (announcedLength >= 0 &&
        (announcedLength < 32 ||
         announcedLength >
             static_cast<int>(CoverCache::kMaximumSourceBytes))) {
        return fail("invalid-size");
    }
    const bool opened = output.open(partialPath);
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(
        kBookTransferBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!opened || buffer == nullptr) {
        output.close();
        heap_caps_free(buffer);
        return fail("transfer-allocation-failed");
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    WiFiClient *stream = http.getStreamPtr();
    size_t received = 0;
    uint8_t signature[2]{};
    uint32_t lastDataAt = millis();
    bool transferOk = true;
    const char *transferError = "stream-failed";
    while (http.connected() || stream->available() > 0) {
        reportWorkProgress();
        if (AutomaticSync::cancelRequested()) {
            transferOk = false;
            transferError = "sync-paused";
            break;
        }
        const int available = stream->available();
        if (available <= 0) {
            if (millis() - lastDataAt > kBookTransferIdleTimeoutMs) {
                transferOk = false;
                transferError = "transfer-timeout";
                break;
            }
            delay(2);
            continue;
        }
        const size_t requested =
            min(static_cast<size_t>(available), kBookTransferBufferBytes);
        const size_t count = stream->readBytes(buffer, requested);
        if (count == 0) {
            transferOk = false;
            break;
        }
        if (received + count > CoverCache::kMaximumSourceBytes) {
            transferOk = false;
            transferError = "response-too-large";
            break;
        }
        if (received < sizeof(signature)) {
            const size_t copy = min(count, sizeof(signature) - received);
            memcpy(signature + received, buffer, copy);
        }
        if (!output.append(buffer, count)) {
            transferOk = false;
            transferError = "sd-write-failed";
            break;
        }
        mbedtls_sha256_update_ret(&sha, buffer, count);
        received += count;
        if (AutomaticSync::ownsNetworkSession()) AutomaticSync::reportProgress(SyncProgress::Stage::Cover,received,announcedLength>0?announcedLength:0);
        lastDataAt = millis();
        if (announcedLength >= 0 &&
            received == static_cast<size_t>(announcedLength)) {
            break;
        }
    }
    if (transferOk && !output.finish()) { transferOk = false; transferError = "sd-write-failed"; }
    const bool closed = output.close();
    if (!closed && transferOk) { transferOk = false; transferError = "sd-write-failed"; }
    heap_caps_free(buffer);
    uint8_t transportDigest[32]{};
    mbedtls_sha256_finish_ret(&sha, transportDigest);
    mbedtls_sha256_free(&sha);
    result.responseBytes = received;
    if (!transferOk) {
        return fail(transferError);
    }
    if (received < 32 || signature[0] != 0xFF || signature[1] != 0xD8 ||
        (announcedLength >= 0 &&
         received != static_cast<size_t>(announcedLength))) {
        return fail("invalid-jpeg-body");
    }
    uint8_t persistedDigest[32]{};
    size_t persistedBytes = 0;
    if (!hashFile(partialPath, persistedDigest, persistedBytes) ||
        persistedBytes != received ||
        memcmp(transportDigest, persistedDigest, sizeof(transportDigest)) !=
            0) {
        return fail("sd-verification-failed");
    }
    digestToHex(persistedDigest, result.sha256, sizeof(result.sha256));
    if (!CoverCache::publishSourcePartial(bookId)) {
        return fail("publish-failed");
    }
    result.ok = true;
    result.tlsVerified = true;
    setError(result.error, sizeof(result.error), "");
    http.end();
    client.stop();
    result.durationMs = millis() - startedAt;
    disconnect();
    return true;
}

OpdsProbeResult NetworkService::probeRoot(uint32_t timeoutMs) {
    OpdsProbeResult result{};
    String body;
    OpdsFetchResult fetch{};
    fetchOpds(nullptr, body, fetch, timeoutMs);
    result.httpCode = fetch.httpCode;
    result.tlsError = fetch.tlsError;
    result.tlsVerified = fetch.tlsVerified;
    result.responseBytes = fetch.responseBytes;
    result.durationMs = fetch.durationMs;
    snprintf(result.contentType, sizeof(result.contentType), "%s",
             fetch.contentType);
    if (!fetch.ok) {
        setError(result.error, sizeof(result.error), fetch.error);
        return result;
    }
    result.responseBytes = body.length();
    result.entries = countOccurrences(body, "<entry");
    result.atomFeed = body.indexOf("<feed") >= 0;
    const bool xmlContent =
        strstr(result.contentType, "xml") != nullptr ||
        strstr(result.contentType, "atom") != nullptr;
    result.ok = result.atomFeed && xmlContent && result.responseBytes > 0;
    if (!result.ok) {
        setError(result.error, sizeof(result.error), "invalid-opds-feed");
    }
    return result;
}
