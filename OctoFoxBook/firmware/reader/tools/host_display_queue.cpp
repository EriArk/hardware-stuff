#include <cassert>
#include <iostream>
#include "EPD_Painter.h"
#include "display_refresh_controller.h"

int main() {
    DisplayRefreshController display;
    assert(display.begin());
    uint8_t image[EPD_WIDTH * EPD_HEIGHT / 2]{};
    const auto first = display.refresh(image, DisplayRefreshMode::ReaderText);
    assert(first.ok && first.queued);
    // Hold the fake physical panel busy, while the real controller accepts UI.
    const uint32_t started = millis();
    while (!FakePanel::paints.load() && millis() - started < 1000) delay(1);
    assert(FakePanel::paints == 1);
    memset(image, 0xFF, sizeof(image));
    assert(FakePanel::frame[0] == 3); // snapshot, not a reference to mutable UI
    display.requestHardClearBeforeNextRefresh();
    assert(display.refresh(image, DisplayRefreshMode::FastUi).queued);
    memset(image, 0x55, sizeof(image));
    assert(display.refresh(image, DisplayRefreshMode::FastUi).queued);
    memset(image, 0xAA, sizeof(image));
    assert(display.refresh(image, DisplayRefreshMode::FastUi).queued);
    assert(display.busy());
    assert(!display.flush(5)); // sleep must not overtake an unfinished paint
    FakePanel::finish = true;
    assert(display.flush(2000));
    assert(FakePanel::paints == 2); // all three pending frames collapse to last
    assert(FakePanel::clears == 1); // cleanup survived the replacements
    for (auto pixel : FakePanel::frame) assert(pixel == 1);
    assert(!display.busy());
    // Regional clean requests also survive newer frames, and merge safely.
    FakePanel::blockNext = true;
    assert(display.refresh(image, DisplayRefreshMode::FastUi).queued);
    const uint32_t next = millis();
    while (FakePanel::paints < 3 && millis() - next < 1000) delay(1);
    assert(FakePanel::paints == 3);
    assert(display.refresh(image, DisplayRefreshMode::RecoveryRegion, {0,0,4,2}).queued);
    assert(display.refresh(image, DisplayRefreshMode::RecoveryRegion, {4,1,4,2}).queued);
    assert(display.refresh(image, DisplayRefreshMode::FastUi).queued);
    assert(!display.refresh(image, DisplayRefreshMode::RecoveryRegion, {-1,0,4,2}).ok);
    FakePanel::finish = true;
    assert(display.flush(2000));
    assert(FakePanel::paints == 4 && FakePanel::clears == 2 && FakePanel::regionClears == 1);
    assert(FakePanel::clearX == 0 && FakePanel::clearY == 0 && FakePanel::clearW == 8 && FakePanel::clearH == 3);
    assert(display.repairToWhite().ok);
    assert(FakePanel::clears == 3);
    display.shutdown();
    std::cout << "DISPLAY_QUEUE_OK\n";
}
