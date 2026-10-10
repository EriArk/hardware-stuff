// UI-task modal: underlying input/rendering pauses, workers can finish safely.
// The saved framebuffer restores exactly the editor/selection underneath it.
bool paintBatteryNotice(bool critical) {
    auto *scratch=static_cast<uint8_t*>(heap_caps_malloc(kFramebufferBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!scratch || !framebuffer) { heap_caps_free(scratch); return false; }
    ReaderBookishCanvas c(scratch);
    BookishUI::batteryNotice(c,diagnostics.batteryPercent,critical);
    heap_caps_free(scratch);
    lastDisplayRefresh=displayRefresh.refresh(framebuffer,DisplayRefreshMode::RecoveryFull);
    return lastDisplayRefresh.ok;
}
void dismissBatteryWarning() {
    if(!batteryWarningVisible) return;
    batteryWarningVisible=false;
    if(batteryUnderlay) {
        memcpy(framebuffer,batteryUnderlay,kFramebufferBytes);
        heap_caps_free(batteryUnderlay);batteryUnderlay=nullptr;
        lastDisplayRefresh=displayRefresh.refresh(framebuffer,DisplayRefreshMode::RecoveryFull);
    }
    lastActivityAt=millis();
    Serial.println("BATTERY WARNING dismissed=true");
}
void updateBatteryPolicy() {
    switch(batteryPolicy.sample(diagnostics.batteryMillivolts,diagnostics.batteryPercent)) {
        case BatteryPolicy::Action::Warn: batteryWarningPending=true; break;
        case BatteryPolicy::Action::Protect: batteryProtectionRequested=true; pendingPowerSleep=true; break;
        default: break;
    }
}
bool showPendingBatteryWarning() {
    if(batteryWarningPending && !batteryWarningVisible && framebuffer) {
        batteryUnderlay=static_cast<uint8_t*>(heap_caps_malloc(kFramebufferBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(batteryUnderlay) {
            memcpy(batteryUnderlay,framebuffer,kFramebufferBytes);
            batteryWarningVisible=paintBatteryNotice(false);
            if(batteryWarningVisible) {
                batteryWarningPending=false;
                Serial.printf("BATTERY WARNING percent=%u screen=%u\n",diagnostics.batteryPercent,static_cast<unsigned>(uiScreen));
            } else {heap_caps_free(batteryUnderlay);batteryUnderlay=nullptr;}
        }
    }
    return batteryWarningVisible;
}
