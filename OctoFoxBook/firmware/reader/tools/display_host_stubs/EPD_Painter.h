#pragma once
#include "Arduino.h"
#include "epd_driver.h"
#include <cassert>
namespace FakePanel {
inline std::atomic<unsigned> paints{0}, clears{0};
inline std::atomic<unsigned> regionClears{0};
inline int clearX=0, clearY=0, clearW=0, clearH=0;
inline std::atomic<bool> finish{false};
inline std::atomic<bool> blockNext{false};
inline uint8_t frame[EPD_WIDTH * EPD_HEIGHT]{};
}
class EPD_Painter {
public:
    enum class Quality { QUALITY_FAST, QUALITY_NORMAL, QUALITY_HIGH };
    enum class ClearMode { HARD, SOFT };
    struct Rect { int16_t x, y, w, h; };
    EPD_Painter(int, bool) {}
    void setAutoShutdown(bool) {}
    bool begin() { return true; }
    void setIdleTimeout(int) {}
    void setGreyLevels(int) {}
    void setQuality(Quality) {}
    void setDirectTransitions(bool) {}
    bool paintIdle() { return FakePanel::paints == 0 || FakePanel::finish.load(); }
    void paint(uint8_t *frame) {
        assert(paintIdle());
        memcpy(FakePanel::frame, frame, sizeof(FakePanel::frame));
        if (FakePanel::blockNext.exchange(false)) FakePanel::finish = false;
        ++FakePanel::paints;
    }
    void clear(const Rect *regions, int count, ClearMode) {
        assert(paintIdle()); ++FakePanel::clears;
        if (regions && count) {
            FakePanel::clearX=regions[0].x; FakePanel::clearY=regions[0].y;
            FakePanel::clearW=regions[0].w; FakePanel::clearH=regions[0].h;
            ++FakePanel::regionClears;
        }
    }
    void end() { assert(paintIdle()); }
};
