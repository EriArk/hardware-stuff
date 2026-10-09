#include "wifi_setup.h"
#include "wifi_credentials.h"
#include "automatic_sync.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <atomic>
#include <string.h>

namespace WifiSetup {
namespace {
std::atomic<bool> entered{false};
State current = State::Idle;
Network networks[kCapacity]; unsigned found = 0;
WifiCredential candidate{};
uint32_t started = 0;
const char *failure = "";
void radioOff() {
    if (WiFi.getMode() != WIFI_OFF) {
        esp_wifi_scan_stop(); WiFi.scanDelete();
        WiFi.disconnect(true, false); WiFi.mode(WIFI_OFF);
    }
}
void finish(State target, const char *error = "") {
    radioOff(); WifiCredentials::wipe(&candidate, sizeof(candidate)); current = target; failure = error;
}
void radioOn() {
    WiFi.persistent(false); WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_STA); WiFi.setSleep(true);
}
}
bool enter() {
    if (active() || AutomaticSync::busy()) return false;
    entered = true; current = State::Idle; found = 0; failure = ""; return true;
}
bool active() { return entered.load(); }
void leave() { if (!active()) return; finish(State::Idle); found = 0; entered = false; }
void cancel() { if (active()) finish(State::Networks); }
State state() { return current; }
const char *error() { return failure; }
unsigned count() { return found; }
const Network &network(unsigned index) { static const Network empty{}; return index < found ? networks[index] : empty; }
void scan() {
    if (!active() || AutomaticSync::busy()) return;
    radioOff(); found = 0; failure = ""; radioOn(); started = millis(); current = State::Scanning;
    if (WiFi.scanNetworks(true, false) == WIFI_SCAN_FAILED) finish(State::Failed, "scan-failed");
}
bool connect(const WifiCredential &credential, bool open) {
    if (!active() || AutomaticSync::busy() || !credential.ssid[0] ||
        !memchr(credential.ssid, 0, sizeof(credential.ssid)) ||
        !WifiCredentials::validPassword(credential.password, open)) return false;
    radioOff(); candidate = credential; radioOn();
    started = millis(); failure = ""; current = State::Connecting;
    WiFi.begin(candidate.ssid, open ? nullptr : candidate.password);
    return true;
}
bool poll() {
    if (!active()) return false;
    if (current == State::Scanning) {
        const int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            if (millis() - started < 18000) return false;
            finish(State::Failed, "scan-timeout"); return true;
        }
        if (n < 0) { finish(State::Failed, "scan-failed"); return true; }
        for (int i = 0; i < n; ++i) {
            Network item{};
            const String ssid = WiFi.SSID(i);
            if (!ssid.length() || ssid.length() > 32) continue;
            snprintf(item.ssid, sizeof(item.ssid), "%s", ssid.c_str());
            item.rssi = WiFi.RSSI(i);
            const auto auth = WiFi.encryptionType(i);
            item.open = auth == WIFI_AUTH_OPEN;
            item.supported = item.open || auth == WIFI_AUTH_WPA_PSK || auth == WIFI_AUTH_WPA2_PSK ||
                auth == WIFI_AUTH_WPA_WPA2_PSK || auth == WIFI_AUTH_WPA3_PSK || auth == WIFI_AUTH_WPA2_WPA3_PSK;
            bool duplicate = false;
            for (unsigned j = 0; j < found; ++j)
                if (!strcmp(networks[j].ssid, item.ssid) && networks[j].open == item.open && networks[j].supported == item.supported) {
                    duplicate = true; if (item.rssi > networks[j].rssi) networks[j].rssi = item.rssi;
                }
            if (duplicate) continue;
            unsigned at = 0; while (at < found && networks[at].rssi >= item.rssi) ++at;
            if (at >= kCapacity) continue;
            if (found < kCapacity) ++found;
            for (unsigned j = found-1; j > at; --j) networks[j] = networks[j-1];
            networks[at] = item;
        }
        for (unsigned i = 1; i < found; ++i)
            for (unsigned j = i; j > 0 && networks[j].rssi > networks[j-1].rssi; --j) {
                const Network swap = networks[j]; networks[j] = networks[j-1]; networks[j-1] = swap;
            }
        finish(State::Networks); return true;
    }
    if (current == State::Connecting) {
        if (WiFi.status() == WL_CONNECTED) {
            const bool saved = WifiCredentials::save(candidate);
            finish(saved ? State::Success : State::Failed, saved ? "" : "save-failed");
            return true;
        }
        if (millis() - started >= 18000) { finish(State::Failed, "connect-failed"); return true; }
    }
    return false;
}
}
