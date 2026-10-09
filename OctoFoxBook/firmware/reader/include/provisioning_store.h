#pragma once

#include <Arduino.h>

constexpr uint16_t kProvisioningSchemaVersion = 1;
constexpr uint8_t kProvisioningAllFieldsMask = 0x1F;

struct ProvisioningConfig {
    char wifiSsid[33]{};
    char wifiPassword[65]{};
    char opdsUrl[257]{};
    char opdsUsername[65]{};
    char opdsPassword[129]{};
};

struct WifiCredential {
    char ssid[33]{};
    char password[65]{};
};

class ProvisioningStore {
public:
    static bool load(ProvisioningConfig &config, char *error,
                     size_t errorCapacity);
    static bool save(const ProvisioningConfig &config, char *error,
                     size_t errorCapacity);
    static bool clear(char *error, size_t errorCapacity);
    static bool loadFallbackWifi(WifiCredential &credential, char *error,
                                 size_t errorCapacity);
    static bool saveFallbackWifi(const WifiCredential &credential,
                                 char *error, size_t errorCapacity);
    static bool clearFallbackWifi(char *error, size_t errorCapacity);
    static bool validate(const ProvisioningConfig &config, char *error,
                         size_t errorCapacity);
    static bool decodeBase64Field(const char *encoded, char *target,
                                  size_t targetCapacity, size_t &decodedBytes,
                                  char *error, size_t errorCapacity);
};
