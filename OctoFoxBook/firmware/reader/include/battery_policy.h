#pragma once
#include <stdint.h>

// Feed averaged, fresh ADC samples, never every UI loop iteration. Percentage
// is a voltage estimate, not a fuel-gauge reading. Reject disconnected ADCs.
class BatteryPolicy {
public:
    static constexpr uint8_t WarningPercent=15;
    static constexpr uint8_t ProtectionPercent=8;
    static constexpr uint8_t ResumePercent=12;
    static constexpr uint8_t WarningRearmPercent=20;
    enum class Action { None, Warn, Protect };
    Action sample(uint16_t millivolts, uint8_t percent) {
        if (millivolts < 2500 || millivolts > 4500) { criticalSamples_=0; return Action::None; }
        if (percent >= WarningRearmPercent) warned_=false;
        if (percent <= ProtectionPercent) {
            if (++criticalSamples_ >= 3) return Action::Protect;
        } else criticalSamples_=0;
        if (percent <= WarningPercent && !warned_) { warned_=true; return Action::Warn; }
        return Action::None;
    }
private:
    bool warned_=false;
    unsigned criticalSamples_=0;
};
