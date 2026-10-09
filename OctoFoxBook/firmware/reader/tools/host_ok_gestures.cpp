#include <cassert>
#include <cstdint>
#include <iostream>
#include "reader_input_policy.h"
using Event = OkGesture::Event;
int main() {
    OkGesture g;
    assert(g.update(false, 0) == Event::None);
    assert(g.update(true, 100) == Event::None);
    assert(g.update(false, 180) == Event::None);
    assert(g.update(false, 530) == Event::None);
    assert(g.update(false, 531) == Event::Short);
    assert(g.update(false, 800) == Event::None);
    // Second press began inside the window, releases outside it: still double.
    assert(g.update(true, 1000) == Event::None);
    assert(g.update(false, 1100) == Event::None);
    assert(g.update(true, 1400) == Event::None);
    assert(g.update(true, 1500) == Event::None);
    assert(g.update(false, 1700) == Event::Double);
    assert(g.update(false, 2200) == Event::None);
    // Holding the second press emits one hold, never short+hold or double.
    assert(g.update(true, 3000) == Event::None);
    assert(g.update(false, 3100) == Event::None);
    assert(g.update(true, 3200) == Event::None);
    assert(g.update(true, 3999) == Event::None);
    assert(g.update(true, 4000) == Event::Long);
    assert(g.update(true, 5000) == Event::None);
    assert(g.update(false, 5100) == Event::None);
    // Independent long press fires without waiting for release.
    assert(g.update(true, 6000) == Event::None);
    assert(g.update(true, 6800) == Event::Long);
    assert(g.update(false, 7000) == Event::None);
    // Late second press keeps two independent single clicks.
    assert(g.update(true, 8000) == Event::None);
    assert(g.update(false, 8100) == Event::None);
    assert(g.update(true, 8501) == Event::Short);
    assert(g.update(false, 8600) == Event::None);
    assert(g.update(false, 8951) == Event::Short);
    OkGesture rollover;
    assert(rollover.update(true, UINT32_MAX - 200) == Event::None);
    assert(rollover.update(false, UINT32_MAX - 100) == Event::None);
    assert(rollover.update(false, 251) == Event::Short);
    std::cout << "OK_GESTURES_OK\n";
}
