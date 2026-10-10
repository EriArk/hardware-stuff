// H716 has no software-controlled main power latch and no VBUS wake input.
// This is protection DEEP SLEEP, never advertised as a hardware disconnect.
// Progress/bookmarks are committed as they change. Persist the resume context
// as well, so removing the battery does not lose the last reading screen.
bool readBatteryResume() {
    Preferences prefs;
    if(!prefs.begin("abyss-power",true))return false;
    SleepContext stored{};
    const bool ok=prefs.getBytesLength("low-context")==sizeof(stored) &&
        prefs.getBytes("low-context",&stored,sizeof(stored))==sizeof(stored) &&
        stored.magic==kSleepContextMagic && stored.version==kSleepContextVersion &&
        stored.checksum==sleepContextChecksum(stored);
    prefs.end();
    if(ok)sleepContext=stored;
    return ok;
}
void clearBatteryResume() {
    Preferences prefs;
    if(prefs.begin("abyss-power",false)) {prefs.remove("low-context");prefs.end();}
}
void enterBatteryProtection(bool preserveContext=false) {
    if(!preserveContext) {
        saveSleepContext();
        Preferences prefs;
        bool saved=prefs.begin("abyss-power",false);
        if(saved) {saved=prefs.putBytes("low-context",&sleepContext,sizeof(sleepContext))==sizeof(sleepContext);prefs.end();}
        Serial.printf("BATTERY CONTEXT saved=%s\n",saved?"true":"false");
    }
    batteryWarningVisible=false;batteryWarningPending=false;
    heap_caps_free(batteryUnderlay);batteryUnderlay=nullptr;
    const bool painted=paintBatteryNotice(true);
    networkService.disconnect();
    SD.end();
    displayRefresh.shutdown(); // Wait for the charge message to reach the panel.
    pinMode(kPowerPin,INPUT_PULLUP);
    // Arm the opposite CURRENT level even for a held momentary button. This
    // avoids rapid wake/sleep loops when charge protection interrupts a hold.
    const int wakeLevel=digitalRead(kPowerPin)==LOW?1:0;
    rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(kPowerPin));
    rtc_gpio_pullup_en(static_cast<gpio_num_t>(kPowerPin));
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(kPowerPin),wakeLevel);
    Serial.printf("BATTERY PROTECTED mode=deep-sleep percent=%u frame=%s wake=Sleep usb_auto_wake=false\n",
        diagnostics.batteryPercent,painted?"true":"false");
    Serial.flush();
    esp_deep_sleep_start();
}
bool processBatteryProtection() {
    if(!batteryProtectionRequested)return false;
    if(WifiSetup::active())closeWifiSettings();
    AutomaticSync::setPaused(true);AutomaticSync::cancel();
    if(!restoringSettings)BookPreparation::cancel();
    if(bookUpload.active())bookUpload.cancel(Serial);
    // Never unmount SD underneath a writer. The workers already have bounded
    // network timeouts and cooperative cancellation at file boundaries.
    if(AutomaticSync::busy() || BookPreparation::busy() || preparingSettings)return true;
    enterBatteryProtection();return true;
}
