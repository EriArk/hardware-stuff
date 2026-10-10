#include "wifi_credentials.h"
#include <Preferences.h>
#include <esp_crc.h>
#include <stddef.h>
#include <string.h>

namespace WifiCredentials {
namespace {
#pragma pack(push, 1)
struct Record {
    uint32_t magic = 0x32464957; // WIF2, independently replaceable NVS blob
    WifiCredential credential{};
    uint32_t crc = 0;
};
#pragma pack(pop)
uint32_t checksum(const Record &r) {
    return esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&r), offsetof(Record, crc));
}
bool valid(const WifiCredential &c) {
    if (!c.ssid[0] || !memchr(c.ssid, 0, sizeof(c.ssid)) ||
        !memchr(c.password, 0, sizeof(c.password))) return false;
    return validPassword(c.password, !c.password[0]);
}
}
bool validPassword(const char *password, bool open) {
    const size_t n = strnlen(password, 65);
    if (open) return n == 0;
    if (n < 8 || n > 64) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = password[i];
        if (n == 64) { if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false; }
        else if (c < 32 || c > 126) return false;
    }
    return true;
}
bool load(WifiCredential &out) {
    out = WifiCredential{};
    Preferences prefs;
    if (prefs.begin("abyss-reader", true)) {
        const size_t size = prefs.getBytesLength("wifi1");
        if (size) {
            Record record{};
            const bool ok = size == sizeof(record) && prefs.getBytes("wifi1", &record, sizeof(record)) == sizeof(record) &&
                record.magic == 0x32464957 && record.crc == checksum(record) && valid(record.credential);
            prefs.end();
            if (ok) out = record.credential;
            wipe(&record, sizeof(record));
            return ok; // A corrupt override must never silently revive old credentials.
        }
        prefs.end();
    }
    ProvisioningConfig legacy{}; char error[48]{};
    const bool ok = ProvisioningStore::load(legacy, error, sizeof(error));
    if (ok) {
        memcpy(out.ssid, legacy.wifiSsid, sizeof(out.ssid));
        memcpy(out.password, legacy.wifiPassword, sizeof(out.password));
    }
    wipe(&legacy, sizeof(legacy));
    return ok && valid(out);
}
bool save(const WifiCredential &credential) {
    if (!valid(credential)) return false;
    Record record{}; record.credential = credential; record.crc = checksum(record);
    Preferences prefs;
    bool ok = prefs.begin("abyss-reader", false);
    if (ok) { ok = prefs.putBytes("wifi1", &record, sizeof(record)) == sizeof(record); prefs.end(); }
    wipe(&record, sizeof(record));
    return ok;
}
bool clear() {
    Preferences prefs;
    if (!prefs.begin("abyss-reader", false)) return false;
    const bool ok = !prefs.isKey("wifi1") || prefs.remove("wifi1");
    prefs.end(); return ok;
}
}
