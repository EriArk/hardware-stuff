// Executes the production UI adapter and records Canvas calls for visual review.
#include "bookish_ui.h"
#include "text_keyboard.h"
#include "wifi_credentials.h"
#include "wifi_setup.h"
#include "reader_settings.h"
#include <WiFi.h>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <iomanip>
enum class UiScreen { Home, LocalLibrary, DeviceSettings, Wifi };
enum class TopLevelTab { OnDevice };
enum class LocalLibraryPhase { Sections };
struct Library {bool loaded=false;TopLevelTab owner=TopLevelTab::OnDevice;LocalLibraryPhase phase=LocalLibraryPhase::Sections;} localLibrarySession;
struct Loaded {bool loaded=false;} homeSession,favoritesSession;
TopLevelTab activeTopLevelTab=TopLevelTab::OnDevice;
ReaderSettings activeReaderSettings;
std::atomic<bool> sleepSwitchLatching{false};
bool batteryWarningVisible=false;
bool settingsSaveOk=true;
bool ReaderSettingsStore::save(const ReaderSettings &) {return settingsSaveOk;}
void ReaderSettingsStore::apply(const ReaderSettings &s) {I18n::language=s.language;}
const char *kFirmwareVersion="test";
struct Diagnostics {unsigned batteryPercent=75;} diagnostics;
void scheduleGhostCleanup(const char *) {}
enum class DisplayRefreshMode { QualityFull, FastUi };
struct Refresh { bool ok=true; } lastDisplayRefresh;
struct Display { Refresh refresh(uint8_t *,DisplayRefreshMode) { return {}; } } displayRefresh;
bool hasDisplayRefresh=false, provisioningActive=false;
struct Upload { bool active()const{return false;} } bookUpload;
struct NetworkService { void invalidateConfiguration(){} } networkService;
namespace AutomaticSync { bool busy(){return false;} }
namespace BookPreparation { bool busy(){return false;} }
UiScreen uiScreen=UiScreen::Home;
uint8_t buffer[1], *framebuffer=buffer;
constexpr size_t kFramebufferBytes=1;
constexpr int MALLOC_CAP_SPIRAM=0, MALLOC_CAP_8BIT=0;
void *heap_caps_malloc(size_t n,int){return malloc(n);}
void heap_caps_free(void *p){free(p);}
void scheduleScreenTransitionCleanup(UiScreen,const char *){}
void noteUiNavigationClick(){}
struct Logger { template<typename... T> void printf(const char *,T...){} } Serial;
bool displayHome(bool,const char *){uiScreen=UiScreen::Home;return true;}
bool displayLocalLibrary(bool,const char *){uiScreen=UiScreen::LocalLibrary;return true;}
bool ProvisioningStore::load(ProvisioningConfig &,char *,size_t){return false;}
std::ostringstream drawing;
class ReaderBookishCanvas : public BookishUI::Canvas {
public:
    explicit ReaderBookishCanvas(uint8_t *){drawing.str("");drawing.clear();}
    void box(int x,int y,int w,int h,int r,uint8_t border,uint8_t fill,int t=1) override {
        drawing<<"[\"box\","<<x<<','<<y<<','<<w<<','<<h<<','<<r<<','<<int(border)<<','<<int(fill)<<','<<t<<"]\n";
    }
    void text(BookishUI::Font font,const char *s,int x,int y,int w,uint8_t ink=0,uint8_t paper=15) override {
        drawing<<"[\"text\","<<int(font)<<','<<std::quoted(s)<<','<<x<<','<<y<<','<<w<<','<<int(ink)<<','<<int(paper)<<"]\n";
    }
    void title(const char *,int,int,int)override{}
    void cover(const char *,int,int,int,int)override{}
    void logo(int,int)override{}
};
#include "../src/wifi_settings_ui.inc"
void snapshot(const char *name) { std::cout<<"SCENE "<<name<<'\n'<<drawing.str(); }
void input(const char *button,const char *gesture="SHORT") { inputWifiSettings(button,gesture); }
int main() {
    openWifiSettings();assert(wifiPage==WifiPage::Settings);snapshot("settings");
    input("DOWN");input("CENTER");assert(wifiPage==WifiPage::Reading);snapshot("reading-settings");
    input("DOWN");input("CENTER");assert(activeReaderSettings.textSize==ReaderTextSize::Large);
    settingsSaveOk=false;input("CENTER");assert(activeReaderSettings.textSize==ReaderTextSize::Large);settingsSaveOk=true;
    input("CENTER","DOUBLE");
    for(int i=0;i<4;++i)input("DOWN");input("CENTER");assert(wifiPage==WifiPage::Language);snapshot("language");
    input("DOWN");input("DOWN");input("CENTER");assert(I18n::language==I18n::Language::Russian);snapshot("language-ru");
    input("UP");input("CENTER");assert(I18n::language==I18n::Language::English);
    input("CENTER","DOUBLE");
    for(int i=0;i<5;++i)input("DOWN");
    input("CENTER");assert(WiFi.radio==WIFI_STA);snapshot("scan");
    WiFi.results={{"Home Wi-Fi",-45,WIFI_AUTH_WPA2_PSK},{"Guest Wi-Fi",-67,WIFI_AUTH_OPEN}};
    WiFi.scanResult=2;pollWifiSettings();assert(WiFi.radio==WIFI_OFF);snapshot("networks");
    input("DOWN");input("CENTER");assert(wifiPage==WifiPage::Password && wifiTextEntry);snapshot("keyboard");
    input("CENTER");input("CENTER");input("CENTER");
    assert(!strcmp(wifiKeyboard.value(),"qq"));snapshot("typing");
    input("CENTER","DOUBLE");assert(!wifiKeyboard.inside);
    input("CENTER","DOUBLE");assert(!wifiKeyboard.length() && wifiPage==WifiPage::Networks);
    input("UP");input("UP");input("CENTER");assert(wifiPage==WifiPage::Name);snapshot("hidden");
    input("CENTER");input("CENTER");input("CENTER","DOUBLE");input("UP");input("CENTER");
    assert(wifiPage==WifiPage::Password && !strcmp(wifiDraft.ssid,"q"));
    // Deterministic test draft, never a real password.
    wifiKeyboard.reset("sample123",64);wifiKeyboard.row=5;input("CENTER");
    assert(wifiPage==WifiPage::Connecting);snapshot("connecting");
    fakeMillis+=18001;pollWifiSettings();assert(wifiPage==WifiPage::Result);snapshot("failure");
    input("CENTER");assert(wifiPage==WifiPage::Password && !strcmp(wifiKeyboard.value(),"sample123"));
    input("CENTER");WiFi.connection=WL_CONNECTED;pollWifiSettings();
    assert(wifiPage==WifiPage::Result && !wifiKeyboard.length() && !wifiDraft.password[0]);snapshot("success");
    input("CENTER");assert(wifiPage==WifiPage::Settings);input("CENTER","DOUBLE");
    assert(uiScreen==UiScreen::LocalLibrary && !WifiSetup::active() && WiFi.radio==WIFI_OFF);
    openWifiSettings();
    wifiSetPage(WifiPage::Power);wifiSelection=2;
    input("CENTER");assert(activeReaderSettings.sleepLatching && sleepSwitchLatching);
    input("CENTER");assert(!activeReaderSettings.sleepLatching && !sleepSwitchLatching);
    wifiSetPage(WifiPage::Keyboard);wifiSelection=3;
    input("CENTER");assert(activeReaderSettings.keyboardLayouts==7);snapshot("keyboard-languages");
    input("CENTER");assert(activeReaderSettings.keyboardLayouts==3);
    wifiSelection=1;input("CENTER");assert(activeReaderSettings.keyboardLayouts==3);
    wifiSetPage(WifiPage::Password);wifiKeyboard.configure(3);wifiKeyboard.layout=TextKeyboard::Russian;
    wifiKeyboard.reset();paintWifiSettings();snapshot("keyboard-russian");
    drawing.str("");drawing.clear();ReaderBookishCanvas canvas(nullptr);
    BookishUI::batteryNotice(canvas,15,false);snapshot("battery-warning");
    drawing.str("");drawing.clear();BookishUI::batteryNotice(canvas,8,true);snapshot("charge-required");
    std::cout<<"WIFI_UI_OK\n";
}
