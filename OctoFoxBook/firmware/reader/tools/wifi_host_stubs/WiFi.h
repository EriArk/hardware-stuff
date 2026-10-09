#pragma once
#include "Arduino.h"
#include <vector>
enum { WIFI_OFF, WIFI_STA };
enum { WIFI_SCAN_RUNNING=-1, WIFI_SCAN_FAILED=-2 };
enum { WL_IDLE_STATUS, WL_CONNECTED, WL_CONNECT_FAILED };
enum { WIFI_AUTH_OPEN, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK,
       WIFI_AUTH_WPA3_PSK, WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_WEP, WIFI_AUTH_WPA2_ENTERPRISE };
struct FakeAccessPoint { String ssid; int rssi; int auth; };
class FakeWiFi {
public:
    int radio=WIFI_OFF, connection=WL_IDLE_STATUS, scanResult=WIFI_SCAN_RUNNING;
    unsigned begins=0;
    bool persistentValue=true, reconnect=true;
    std::vector<FakeAccessPoint> results;
    int getMode()const { return radio; }
    void mode(int m) { radio=m; }
    void persistent(bool x) { persistentValue=x; }
    void setAutoReconnect(bool x) { reconnect=x; }
    void setSleep(bool) {}
    void scanDelete() { results.clear(); }
    void disconnect(bool,bool) { connection=WL_IDLE_STATUS; }
    int scanNetworks(bool,bool) { scanResult=WIFI_SCAN_RUNNING; return scanResult; }
    int scanComplete()const { return scanResult; }
    String SSID(int i)const { return results[i].ssid; }
    int RSSI(int i)const { return results[i].rssi; }
    int encryptionType(int i)const { return results[i].auth; }
    void begin(const char *,const char *) { ++begins; }
    int status()const { return connection; }
};
inline FakeWiFi WiFi;
