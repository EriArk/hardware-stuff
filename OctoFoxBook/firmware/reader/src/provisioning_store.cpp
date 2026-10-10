#include "provisioning_store.h"
#include "wifi_credentials.h"

#include <Preferences.h>
#include <esp_crc.h>
#include <mbedtls/base64.h>

#include <stddef.h>
#include <string.h>

namespace {

constexpr char kNamespace[] = "abyss-reader";
constexpr char kKey[] = "provision";
constexpr char kFallbackWifiKey[] = "wifi2";
constexpr uint32_t kMagic = 0x31565250U;  // PRV1
constexpr uint32_t kFallbackWifiMagic = 0x31464957U;  // WIF1

#pragma pack(push, 1)
struct ProvisioningBlob {
    uint32_t magic = kMagic;
    uint16_t version = kProvisioningSchemaVersion;
    uint16_t bytes = 0;
    ProvisioningConfig config{};
    uint32_t crc32 = 0;
};

struct FallbackWifiBlob {
    uint32_t magic = kFallbackWifiMagic;
    uint16_t version = 1;
    uint16_t bytes = 0;
    WifiCredential credential{};
    uint32_t crc32 = 0;
};
#pragma pack(pop)

static_assert(sizeof(ProvisioningBlob) < 1024,
              "Provisioning blob must remain a small atomic NVS value");

void setError(char *error, size_t capacity, const char *message) {
    if (error != nullptr && capacity > 0) {
        snprintf(error, capacity, "%s", message);
    }
}

bool terminated(const char *value, size_t capacity) {
    return value != nullptr && memchr(value, '\0', capacity) != nullptr;
}

bool printableValue(const char *value, size_t capacity, bool allowEmpty) {
    if (!terminated(value, capacity)) {
        return false;
    }
    const size_t length = strnlen(value, capacity);
    if ((!allowEmpty && length == 0) || length >= capacity) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const uint8_t byte = static_cast<uint8_t>(value[index]);
        if (byte < 0x20U || byte == 0x7FU) {
            return false;
        }
    }
    return true;
}

uint32_t blobCrc(const ProvisioningBlob &blob) {
    return esp_crc32_le(0U, reinterpret_cast<const uint8_t *>(&blob),
                        offsetof(ProvisioningBlob, crc32));
}

uint32_t fallbackWifiCrc(const FallbackWifiBlob &blob) {
    return esp_crc32_le(0U, reinterpret_cast<const uint8_t *>(&blob),
                        offsetof(FallbackWifiBlob, crc32));
}

bool validWifiCredential(const WifiCredential &credential) {
    return printableValue(credential.ssid, sizeof(credential.ssid), false) &&
           printableValue(credential.password, sizeof(credential.password),
                          false);
}

}  // namespace

bool ProvisioningStore::validate(const ProvisioningConfig &config,
                                 char *error, size_t errorCapacity) {
    // Pairing and choosing Wi-Fi are independent. A reader can receive its
    // library key over USB before the user selects a network on the device.
    if (!printableValue(config.wifiSsid, sizeof(config.wifiSsid), true)) {
        setError(error, errorCapacity, "invalid-wifi-ssid");
        return false;
    }
    if (!printableValue(config.wifiPassword, sizeof(config.wifiPassword),
                        true) || (!config.wifiSsid[0] && config.wifiPassword[0])) {
        setError(error, errorCapacity, "invalid-wifi-password");
        return false;
    }
    if (!printableValue(config.opdsUrl, sizeof(config.opdsUrl), false) ||
        strncmp(config.opdsUrl, "https://", 8) != 0) {
        setError(error, errorCapacity, "invalid-opds-url");
        return false;
    }
    if (!printableValue(config.opdsUsername, sizeof(config.opdsUsername),
                        false)) {
        setError(error, errorCapacity, "invalid-opds-username");
        return false;
    }
    if (!printableValue(config.opdsPassword, sizeof(config.opdsPassword),
                        false)) {
        setError(error, errorCapacity, "invalid-opds-password");
        return false;
    }
    setError(error, errorCapacity, "");
    return true;
}

bool ProvisioningStore::load(ProvisioningConfig &config, char *error,
                             size_t errorCapacity) {
    config = ProvisioningConfig{};
    Preferences preferences;
    if (!preferences.begin(kNamespace, true)) {
        setError(error, errorCapacity, "nvs-open-failed");
        return false;
    }
    const size_t bytes = preferences.getBytesLength(kKey);
    if (bytes == 0) {
        preferences.end();
        setError(error, errorCapacity, "not-configured");
        return false;
    }
    if (bytes != sizeof(ProvisioningBlob)) {
        preferences.end();
        setError(error, errorCapacity, "invalid-nvs-size");
        return false;
    }
    ProvisioningBlob blob{};
    const size_t read = preferences.getBytes(kKey, &blob, sizeof(blob));
    preferences.end();
    if (read != sizeof(blob) || blob.magic != kMagic ||
        blob.version != kProvisioningSchemaVersion ||
        blob.bytes != sizeof(blob) || blob.crc32 != blobCrc(blob)) {
        setError(error, errorCapacity, "invalid-nvs-record");
        return false;
    }
    if (!validate(blob.config, error, errorCapacity)) {
        return false;
    }
    config = blob.config;
    return true;
}

bool ProvisioningStore::save(const ProvisioningConfig &config, char *error,
                             size_t errorCapacity) {
    if (!validate(config, error, errorCapacity)) {
        return false;
    }
    ProvisioningBlob blob{};
    blob.bytes = sizeof(blob);
    blob.config = config;
    blob.crc32 = blobCrc(blob);
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) {
        setError(error, errorCapacity, "nvs-open-failed");
        return false;
    }
    const size_t written = preferences.putBytes(kKey, &blob, sizeof(blob));
    preferences.end();
    memset(&blob, 0, sizeof(blob));
    if (written != sizeof(blob)) {
        setError(error, errorCapacity, "nvs-write-failed");
        return false;
    }
    setError(error, errorCapacity, "");
    return true;
}

bool ProvisioningStore::clear(char *error, size_t errorCapacity) {
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) {
        setError(error, errorCapacity, "nvs-open-failed");
        return false;
    }
    const bool primaryRemoved =
        !preferences.isKey(kKey) || preferences.remove(kKey);
    const bool fallbackRemoved = !preferences.isKey(kFallbackWifiKey) ||
                                 preferences.remove(kFallbackWifiKey);
    preferences.end();
    if (!primaryRemoved || !fallbackRemoved || !WifiCredentials::clear()) {
        setError(error, errorCapacity, "nvs-remove-failed");
        return false;
    }
    setError(error, errorCapacity, "");
    return true;
}

bool ProvisioningStore::loadFallbackWifi(WifiCredential &credential,
                                         char *error,
                                         size_t errorCapacity) {
    credential = WifiCredential{};
    Preferences preferences;
    if (!preferences.begin(kNamespace, true)) {
        setError(error, errorCapacity, "nvs-open-failed");
        return false;
    }
    const size_t bytes = preferences.getBytesLength(kFallbackWifiKey);
    if (bytes != sizeof(FallbackWifiBlob)) {
        preferences.end();
        setError(error, errorCapacity,
                 bytes == 0 ? "not-configured" : "invalid-nvs-size");
        return false;
    }
    FallbackWifiBlob blob{};
    const size_t read = preferences.getBytes(kFallbackWifiKey, &blob,
                                             sizeof(blob));
    preferences.end();
    if (read != sizeof(blob) || blob.magic != kFallbackWifiMagic ||
        blob.version != 1 || blob.bytes != sizeof(blob) ||
        blob.crc32 != fallbackWifiCrc(blob) ||
        !validWifiCredential(blob.credential)) {
        memset(&blob, 0, sizeof(blob));
        setError(error, errorCapacity, "invalid-nvs-record");
        return false;
    }
    credential = blob.credential;
    memset(&blob, 0, sizeof(blob));
    setError(error, errorCapacity, "");
    return true;
}

bool ProvisioningStore::saveFallbackWifi(
    const WifiCredential &credential, char *error, size_t errorCapacity) {
    if (!validWifiCredential(credential)) {
        setError(error, errorCapacity, "invalid-wifi-credential");
        return false;
    }
    FallbackWifiBlob blob{};
    blob.bytes = sizeof(blob);
    blob.credential = credential;
    blob.crc32 = fallbackWifiCrc(blob);
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) {
        memset(&blob, 0, sizeof(blob));
        setError(error, errorCapacity, "nvs-open-failed");
        return false;
    }
    const size_t written =
        preferences.putBytes(kFallbackWifiKey, &blob, sizeof(blob));
    preferences.end();
    memset(&blob, 0, sizeof(blob));
    if (written != sizeof(FallbackWifiBlob)) {
        setError(error, errorCapacity, "nvs-write-failed");
        return false;
    }
    setError(error, errorCapacity, "");
    return true;
}

bool ProvisioningStore::clearFallbackWifi(char *error,
                                          size_t errorCapacity) {
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) {
        setError(error, errorCapacity, "nvs-open-failed");
        return false;
    }
    const bool removed = !preferences.isKey(kFallbackWifiKey) ||
                         preferences.remove(kFallbackWifiKey);
    preferences.end();
    setError(error, errorCapacity, removed ? "" : "nvs-remove-failed");
    return removed;
}

bool ProvisioningStore::decodeBase64Field(
    const char *encoded, char *target, size_t targetCapacity,
    size_t &decodedBytes, char *error, size_t errorCapacity) {
    decodedBytes = 0;
    if (encoded == nullptr || target == nullptr || targetCapacity < 2) {
        setError(error, errorCapacity, "invalid-field-buffer");
        return false;
    }
    const size_t encodedLength = strlen(encoded);
    size_t outputLength = 0;
    const int result = mbedtls_base64_decode(
        reinterpret_cast<unsigned char *>(target), targetCapacity - 1,
        &outputLength, reinterpret_cast<const unsigned char *>(encoded),
        encodedLength);
    if (result != 0 || outputLength == 0 || outputLength >= targetCapacity) {
        memset(target, 0, targetCapacity);
        setError(error, errorCapacity, "invalid-base64-field");
        return false;
    }
    target[outputLength] = '\0';
    decodedBytes = outputLength;
    setError(error, errorCapacity, "");
    return true;
}
