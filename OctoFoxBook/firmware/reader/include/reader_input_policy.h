#pragma once
#include <stdint.h>

// Unsigned timestamps survive millis() rollover. Navigation never waits for
// a long-press classification; CENTER/POWER retain short-vs-long semantics.
struct NavigationRepeat {
    uint32_t next = 0;
    void pressed(uint32_t now) { next = now + 650; }
    bool held(uint32_t now) {
        if (static_cast<int32_t>(now - next) < 0) return false;
        next = now + 180; // no catch-up burst after an interrupted task
        return true;
    }
};

// Debounced OK states. A completed gesture emits exactly one action. A second
// press reserves the first click, even if it stays down past the double window.
class OkGesture {
public:
    enum class Event { None, Short, Double, Long };
    Event update(bool pressed, uint32_t now, bool allowDouble = true) {
        Event result = Event::None;
        if (pending_ && !down_ && now - releasedAt_ > 350) {
            pending_ = false;
            result = Event::Short;
        }
        if (pressed && !down_) {
            doubleForPress_ = allowDouble;
            second_ = pending_;
            pending_ = false;
            pressedAt_ = now;
            longSent_ = false;
        }
        if (pressed && now - pressedAt_ >= 800 && !longSent_) {
            longSent_ = true;
            second_ = false;
            result = Event::Long;
        }
        if (!pressed && down_ && !longSent_) {
            if (!doubleForPress_) {
                result = Event::Short;
                pending_ = second_ = false;
            } else if (second_) {
                result = Event::Double;
                second_ = false;
            } else {
                pending_ = true;
                releasedAt_ = now;
            }
        }
        down_ = pressed;
        return result;
    }
private:
    bool down_ = false, second_ = false, pending_ = false, longSent_ = false;
    bool doubleForPress_ = true;
    uint32_t pressedAt_ = 0, releasedAt_ = 0;
};
