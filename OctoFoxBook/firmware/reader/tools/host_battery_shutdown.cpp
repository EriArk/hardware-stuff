#include <Preferences.h>
#include <cstdint>
#include <cassert>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
std::vector<std::string> calls;
struct SleepContext {uint32_t magic=77,version=1,screen=4,checksum=82;} sleepContext;
constexpr unsigned kSleepContextMagic=77,kSleepContextVersion=1,kPowerPin=10;
uint32_t sleepContextChecksum(const SleepContext &c){return c.magic+c.version+c.screen;}
void saveSleepContext(){calls.push_back("save");sleepContext.checksum=sleepContextChecksum(sleepContext);}
bool batteryWarningVisible=true,batteryWarningPending=true,batteryProtectionRequested=false,preparingSettings=false;
bool restoringSettings=false;
uint8_t *batteryUnderlay=nullptr;
void heap_caps_free(void *p){free(p);}
bool paintBatteryNotice(bool critical){assert(critical);calls.push_back("paint");return true;}
struct Net {void disconnect(){calls.push_back("network-off");}} networkService;
struct Sd {void end(){calls.push_back("sd-end");}} SD;
struct Display {void shutdown(){calls.push_back("display-flush-off");}} displayRefresh;
struct Diagnostics {unsigned batteryPercent=3;} diagnostics;
struct Log {template<class...T> void printf(const char *,T...){} void flush(){}} Serial;
namespace WifiSetup {bool running=false;bool active(){return running;}}
void closeWifiSettings(){WifiSetup::running=false;calls.push_back("wifi-close");}
namespace AutomaticSync {bool running=false;bool busy(){return running;}void setPaused(bool b){assert(b);calls.push_back("paused");}void cancel(){calls.push_back("sync-cancel");}}
namespace BookPreparation {bool running=false;bool busy(){return running;}void cancel(){calls.push_back("prepare-cancel");}}
struct Upload {bool running=false;bool active(){return running;}void cancel(Log &){running=false;calls.push_back("upload-cancel");}} bookUpload;
constexpr int INPUT_PULLUP=1,LOW=0,ESP_SLEEP_WAKEUP_ALL=0;
using gpio_num_t=int;
int switchLevel=1,wakeLevel=-1;
void pinMode(unsigned,int){}
int digitalRead(unsigned){return switchLevel;}
void rtc_gpio_pulldown_dis(int){}
void rtc_gpio_pullup_en(int){}
void esp_sleep_disable_wakeup_source(int){calls.push_back("disable-wakes");}
void esp_sleep_enable_ext0_wakeup(int,int level){wakeLevel=level;calls.push_back("arm-sleep");}
void esp_deep_sleep_start(){calls.push_back("deep-sleep");throw 1;}
#include "../src/battery_shutdown.inc"
int main() {
    assert(!readBatteryResume());assert(!processBatteryProtection());assert(calls.empty());
    batteryProtectionRequested=true;AutomaticSync::running=true;WifiSetup::running=true;bookUpload.running=true;
    assert(processBatteryProtection());assert(!WifiSetup::running && !bookUpload.running);
    assert(std::find(calls.begin(),calls.end(),"sd-end")==calls.end());
    AutomaticSync::running=false;BookPreparation::running=true;assert(processBatteryProtection());
    BookPreparation::running=false;preparingSettings=true;assert(processBatteryProtection());
    assert(std::find(calls.begin(),calls.end(),"save")==calls.end());
    preparingSettings=false;calls.clear();
    try {processBatteryProtection();assert(false);}catch(int){}
    assert(wakeLevel==0);
    auto pos=[](const char *s){return std::find(calls.begin(),calls.end(),s)-calls.begin();};
    assert(pos("save")<pos("paint") && pos("paint")<pos("sd-end") && pos("sd-end")<pos("display-flush-off"));
    assert(pos("display-flush-off")<pos("arm-sleep") && pos("arm-sleep")<pos("deep-sleep"));
    sleepContext.screen=99;assert(readBatteryResume() && sleepContext.screen==4);
    calls.clear();switchLevel=0;
    try {enterBatteryProtection(true);assert(false);}catch(int){}
    assert(wakeLevel==1 && std::find(calls.begin(),calls.end(),"save")==calls.end());
    FakeNvs::data["low-context"][0]^=1;assert(!readBatteryResume());
    clearBatteryResume();assert(!readBatteryResume());
}
