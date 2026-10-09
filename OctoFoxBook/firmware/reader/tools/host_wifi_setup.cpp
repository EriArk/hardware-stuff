#include "wifi_setup.h"
#include "wifi_credentials.h"
#include <WiFi.h>
#include <Preferences.h>
#include <cassert>
#include <iostream>
bool syncBusy=false, legacyPresent=true;
namespace AutomaticSync { bool busy() { return syncBusy; } }
bool ProvisioningStore::load(ProvisioningConfig &c,char *,size_t) {
    if(!legacyPresent)return false;
    c={}; strcpy(c.wifiSsid,"legacy");strcpy(c.wifiPassword,"oldpassword");
    strcpy(c.opdsUsername,"account-preserved");return true;
}
WifiCredential credential(const char *ssid,const char *password) {
    WifiCredential c{};strcpy(c.ssid,ssid);strcpy(c.password,password);return c;
}
void assertSaved(const char *ssid) {
    WifiCredential got{};assert(WifiCredentials::load(got));assert(!strcmp(got.ssid,ssid));
}
int main() {
    using namespace WifiSetup;
    assertSaved("legacy");
    syncBusy=true;assert(!enter());syncBusy=false;assert(enter());assert(!enter());
    scan();assert(WiFi.radio==WIFI_STA && !WiFi.persistentValue && !WiFi.reconnect);
    WiFi.results={{"duplicate",-60,WIFI_AUTH_WPA2_PSK},{"duplicate",-50,WIFI_AUTH_WPA2_PSK},
                  {"duplicate",-40,WIFI_AUTH_OPEN},{"enterprise",-30,WIFI_AUTH_WPA2_ENTERPRISE}};
    WiFi.scanResult=4;assert(poll());assert(count()==3);assert(WiFi.radio==WIFI_OFF);
    assert(!network(0).supported);assert(network(1).open);assert(network(2).rssi==-50);
    auto c=credential("new","password1");
    assert(connect(c,false));cancel();assert(WiFi.radio==WIFI_OFF);assertSaved("legacy");
    assert(connect(c,false));fakeMillis+=18001;assert(poll());assert(state()==State::Failed);
    assertSaved("legacy");assert(WiFi.radio==WIFI_OFF);
    assert(connect(c,false));WiFi.connection=WL_CONNECTED;assert(poll());assert(state()==State::Success);
    assertSaved("new");assert(WiFi.radio==WIFI_OFF);
    legacyPresent=false;assertSaved("new"); // Wi-Fi works before server provisioning.
    FakeNvs::failWrite=true;c=credential("other","password2");
    assert(connect(c,false));WiFi.connection=WL_CONNECTED;assert(poll());
    assert(state()==State::Failed && !strcmp(error(),"save-failed"));assertSaved("new");
    FakeNvs::failWrite=false;
    auto record=FakeNvs::data.at("wifi1");FakeNvs::data["wifi1"][5]^=1;
    WifiCredential got{};legacyPresent=true;assert(!WifiCredentials::load(got));
    FakeNvs::data["wifi1"]=record;
    assert(!connect(credential("invalid","short"),false));assert(WiFi.radio==WIFI_OFF);
    assert(WifiCredentials::validPassword("",true));assert(!WifiCredentials::validPassword("x",true));
    std::string hex(64,'a');assert(WifiCredentials::validPassword(hex.c_str(),false));
    hex[63]='z';assert(!WifiCredentials::validPassword(hex.c_str(),false));
    assert(!WifiCredentials::validPassword("password\n",false));
    c=credential("open","");assert(connect(c,true));WiFi.connection=WL_CONNECTED;assert(poll());assertSaved("open");
    fakeMillis=0xfffff000;scan();fakeMillis+=18001;assert(poll());assert(state()==State::Failed && WiFi.radio==WIFI_OFF);
    scan();leave();assert(!active() && WiFi.radio==WIFI_OFF);assert(!connect(c,true));
    assert(enter());assert(connect(c,true));leave();assert(WiFi.radio==WIFI_OFF);
    std::cout<<"WIFI_LIFECYCLE_OK\n";
}
