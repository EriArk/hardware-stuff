#pragma once

#include <stddef.h>
#include <stdint.h>

#include "epd_driver.h"

enum class DisplayRefreshMode : uint8_t {
    QualityRegion = 1,
    QualityFull = 2,
    RecoveryFull = 3,
    PanelRepairWhite = 4,
    CalibrationGrayscale = 5,
    CalibrationTransitionFull = 6,
    // H716 uses QUALITY_FAST for monochrome focus movement, QUALITY_HIGH for book pages
    // and all hard-cleared screen transitions. The legacy backend
    // accepts both names and renders them with its proven full refresh path.
    FastUi = 7,
    ReaderText = 8,
    // HARD scrub of the supplied native region, then QUALITY_HIGH repaint.
    RecoveryRegion = 9,
};

struct DisplayRefreshResult {
    bool ok = false;
    // queued means accepted, not yet physically displayed. Completion is
    // reported separately as DISPLAY COMPLETE; flush() is a sleep barrier.
    bool queued = false;
    DisplayRefreshMode requested = DisplayRefreshMode::QualityFull;
    DisplayRefreshMode applied = DisplayRefreshMode::QualityFull;
    Rect_t area{0, 0, 0, 0};
    uint32_t durationMs = 0;
    uint32_t sequence = 0;
};

class DisplayRefreshController {
public:
    bool begin();

    DisplayRefreshResult refresh(const uint8_t *fullFramebuffer,
                                 DisplayRefreshMode mode,
                                 Rect_t region = {0, 0, EPD_WIDTH, EPD_HEIGHT});

    // Makes the next normal refresh perform one hard panel clear immediately
    // before drawing its framebuffer.  This keeps automatic ghost cleanup to
    // a single clear + draw transition while preserving the requested paint
    // quality (notably ReaderText).
    void requestHardClearBeforeNextRefresh();

    // Runs the exact long-form screen_repair sequence published by LILYGO for
    // this panel and intentionally leaves the display blank white. Keeping it
    // separate from normal rendering lets us verify panel neutralization
    // before testing another drawing waveform.
    DisplayRefreshResult repairToWhite();

    // Draws exactly one grayscale calibration target on a visually confirmed
    // white panel. A second call is rejected until repairToWhite() runs again.
    DisplayRefreshResult drawCalibrationOnWhite(const uint8_t *fullFramebuffer);

    // Tests the prospective normal page transition: standard full clear,
    // followed by one complete grayscale target. It is one-shot per boot.
    DisplayRefreshResult drawFullTransition(const uint8_t *fullFramebuffer);

    // Waits for an in-flight paint and releases the panel before deep sleep.
    // Keeping this behind the controller prevents the legacy and fast GPIO
    // power sequences from being mixed by application code.
    void shutdown();
    void setIdleTimeout(int seconds);
    bool flush(uint32_t timeoutMs = 10000);
    bool busy() const;

    uint32_t regionalRefreshesSinceFull() const;

    static const char *modeName(DisplayRefreshMode mode);

private:
    bool initialized_ = false;
    uint32_t regionalRefreshesSinceFull_ = 0;
    uint32_t sequence_ = 0;
    bool calibrationDrawnSinceRepair_ = false;
    bool fullTransitionDrawnSinceBoot_ = false;
    bool hardClearBeforeNextRefresh_ = false;
};
