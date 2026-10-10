#include "display_refresh_controller.h"

#include <Arduino.h>
#include <atomic>

#include <esp_timer.h>

#ifdef USE_EPD_PAINTER
#include <esp_heap_caps.h>

#include "EPD_Painter.h"
#include "EPD_Painter_presets.h"
#endif

namespace {

constexpr uint32_t kPowerOnSettleMs = 20;
constexpr uint32_t kPostDrawSettleMs = 10;
constexpr uint32_t kPowerOffSettleMs = 50;
constexpr int16_t kPanelRepairPulseTime = 50;
constexpr uint32_t kPanelRepairDwellMs = 500;
constexpr int32_t kPanelRepairDarkPasses = 20;
constexpr int32_t kPanelRepairWhitePasses = 40;

#ifdef USE_EPD_PAINTER
constexpr uint32_t kFastPaintTimeoutMs = 3000;
constexpr size_t kPainterFramebufferBytes =
    static_cast<size_t>(EPD_WIDTH) * EPD_HEIGHT;

// The UI already draws into a native 960x540 LilyGo framebuffer. Portrait
// rotation is performed by the existing drawing helpers, so the painter must
// remain in native landscape orientation here.
EPD_Painter fastPainter(EPD_LILYGO_EPD47_H716_PRESET, false);
uint8_t *painterFramebuffer = nullptr;
EPD_Painter::Quality painterQuality = EPD_Painter::Quality::QUALITY_FAST;
SemaphoreHandle_t frameMutex = nullptr;
TaskHandle_t frameTask = nullptr;
uint8_t *queuedFramebuffer = nullptr;
std::atomic<bool> framePending{false};
std::atomic<bool> frameActive{false};
std::atomic<bool> frameFailed{false};
DisplayRefreshMode queuedMode = DisplayRefreshMode::QualityFull;
bool queuedClear = false;
Rect_t queuedClearRegion{0, 0, 0, 0};
uint32_t queuedSequence = 0;
uint32_t queuedAt = 0;

bool waitForFastPainter(uint32_t timeoutMs = kFastPaintTimeoutMs) {
    const uint32_t started = millis();
    while (!fastPainter.paintIdle() && millis() - started < timeoutMs) {
        delay(1);
    }
    return fastPainter.paintIdle();
}

void convertLilyGo4BppToPainter4Level(const uint8_t *source,
                                      uint8_t *destination) {
    // LilyGo: 0=black..15=white, even pixel in the low nibble.
    // Painter: 0=white..3=black, one level code per byte.
    for (size_t pixel = 0; pixel < kPainterFramebufferBytes; ++pixel) {
        const uint8_t packed = source[pixel >> 1];
        const uint8_t shade =
            (pixel & 1U) == 0 ? packed & 0x0F : packed >> 4;
        destination[pixel] =
            static_cast<uint8_t>((15U - shade + 2U) / 5U);
    }
}

bool selectFastPainterQuality(DisplayRefreshMode mode) {
    const EPD_Painter::Quality wanted =
        (mode == DisplayRefreshMode::FastUi || mode == DisplayRefreshMode::QualityRegion)
            ? EPD_Painter::Quality::QUALITY_FAST
            : EPD_Painter::Quality::QUALITY_HIGH;
    if (wanted == painterQuality) {
        return true;
    }
    if (!waitForFastPainter()) {
        return false;
    }
    fastPainter.setQuality(wanted);
    painterQuality = wanted;
    return true;
}

// Only this task submits normal paints. The mailbox owns its snapshot: UI
// rendering and the loading indicator may immediately reuse their framebuffer.
void paintFrames(void *) {
    unsigned uiPaintsSinceClear = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            xSemaphoreTake(frameMutex, portMAX_DELAY);
            if (!framePending.load()) {
                xSemaphoreGive(frameMutex);
                break;
            }
            frameActive = true;
            const auto mode = queuedMode;
            // Antialiased UI/cover frames need HIGH and a shorter cleanup
            // cadence than continuous reading. Count completed paints, not keys.
            const bool clear = queuedClear || (queuedMode == DisplayRefreshMode::QualityFull && uiPaintsSinceClear >= 3);
            const Rect_t clearRegion = queuedClearRegion;
            const uint32_t sequence = queuedSequence;
            const uint32_t submitted = queuedAt;
            queuedClear = false;
            queuedClearRegion = {0, 0, 0, 0};
            convertLilyGo4BppToPainter4Level(queuedFramebuffer, painterFramebuffer);
            framePending = false;
            xSemaphoreGive(frameMutex);
            const uint32_t started = millis();
            bool ok = waitForFastPainter() && selectFastPainterQuality(mode);
            if (ok && clear) fastPainter.clear(nullptr, 0, EPD_Painter::ClearMode::HARD);
            else if (ok && clearRegion.width > 0) {
                const EPD_Painter::Rect region{
                    static_cast<int16_t>(clearRegion.x), static_cast<int16_t>(clearRegion.y),
                    static_cast<int16_t>(clearRegion.width), static_cast<int16_t>(clearRegion.height)};
                fastPainter.clear(&region, 1, EPD_Painter::ClearMode::HARD);
            }
            if (ok) {
                fastPainter.paint(painterFramebuffer);
                ok = waitForFastPainter();
            }
            if (ok && clear) uiPaintsSinceClear = 0;
            else if (ok && mode == DisplayRefreshMode::QualityFull) ++uiPaintsSinceClear;
            frameFailed = !ok;
            Serial.printf("DISPLAY COMPLETE seq=%lu ok=%s mode=%s clear=%s region_clear=%s wait_ms=%lu paint_ms=%lu total_ms=%lu\n",
                static_cast<unsigned long>(sequence), ok ? "true" : "false",
                DisplayRefreshController::modeName(mode), clear ? "true" : "false",
                !clear && clearRegion.width > 0 ? "true" : "false",
                static_cast<unsigned long>(started - submitted),
                static_cast<unsigned long>(millis() - started),
                static_cast<unsigned long>(millis() - submitted));
            frameActive = false;
        }
    }
}
#endif

}  // namespace

bool DisplayRefreshController::begin() {
    if (!initialized_) {
#ifdef USE_EPD_PAINTER
        // The no-touch H716 exposes USB directly through the ESP32-S3 and has
        // no detectable I2C charger. Disable the library's charger-based boot
        // shutdown heuristic before begin(), otherwise it can deep-sleep a
        // perfectly healthy USB-powered reader.
        fastPainter.setAutoShutdown(false);
        if (!fastPainter.begin()) {
            return false;
        }
        fastPainter.setIdleTimeout(5);
        fastPainter.setGreyLevels(4);
        fastPainter.setQuality(EPD_Painter::Quality::QUALITY_FAST);
        painterQuality = EPD_Painter::Quality::QUALITY_FAST;
        // The direct grey-to-grey engine reserves a sizeable sweep table in
        // scarce internal RAM.  It is useful for animation-heavy demos, but
        // selected-row inversion is faster with the regular 4-level decision
        // path and leaving the memory free keeps FB2 preparation reliable.
        fastPainter.setDirectTransitions(false);

        painterFramebuffer = static_cast<uint8_t *>(heap_caps_aligned_alloc(
            16, kPainterFramebufferBytes,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (painterFramebuffer == nullptr) {
            fastPainter.end();
            return false;
        }
        memset(painterFramebuffer, 0, kPainterFramebufferBytes);
        queuedFramebuffer = static_cast<uint8_t *>(heap_caps_malloc(
            kPainterFramebufferBytes / 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        frameMutex = xSemaphoreCreateMutex();
        if (!queuedFramebuffer || !frameMutex ||
            xTaskCreatePinnedToCore(paintFrames, "reader_display", 4096, nullptr,
                                   1, &frameTask, 0) != pdPASS) {
            if (frameMutex) vSemaphoreDelete(frameMutex);
            heap_caps_free(queuedFramebuffer);
            heap_caps_free(painterFramebuffer);
            queuedFramebuffer = nullptr;
            painterFramebuffer = nullptr;
            fastPainter.end();
            return false;
        }
#else
        epd_init();
#endif
        initialized_ = true;
    }
    return initialized_;
}

void DisplayRefreshController::requestHardClearBeforeNextRefresh() {
    hardClearBeforeNextRefresh_ = true;
}

DisplayRefreshResult DisplayRefreshController::refresh(
    const uint8_t *fullFramebuffer, DisplayRefreshMode mode, Rect_t region) {
    if (mode == DisplayRefreshMode::PanelRepairWhite) {
        return repairToWhite();
    }

    DisplayRefreshResult result{};
    result.requested = mode;
    result.applied = mode;
    result.sequence = ++sequence_;

    if (!initialized_ || fullFramebuffer == nullptr) {
        return result;
    }

#ifdef USE_EPD_PAINTER
    {
    result.area = epd_full_screen();
    if (result.area.width <= 0 || result.area.height <= 0 ||
        painterFramebuffer == nullptr || frameTask == nullptr) {
        return result;
    }

    const bool hardClear =
        mode == DisplayRefreshMode::RecoveryFull ||
        hardClearBeforeNextRefresh_ || frameFailed.load();
    const bool regionClear = mode == DisplayRefreshMode::RecoveryRegion;
    if (regionClear && (region.x < 0 || region.y < 0 || region.width <= 0 || region.height <= 0 ||
        region.x >= EPD_WIDTH || region.y >= EPD_HEIGHT ||
        region.width > EPD_WIDTH - region.x || region.height > EPD_HEIGHT - region.y)) return result;
    const int64_t startedAt = esp_timer_get_time();
    xSemaphoreTake(frameMutex, portMAX_DELAY);
    // Never discard a requested cleanup when replacing an intermediate frame.
    queuedClear = queuedClear || hardClear;
    // Coalescing may replace the frame, but must not lose a local scrub.
    if (regionClear) {
        if (queuedClearRegion.width == 0) queuedClearRegion = region;
        else {
            const int32_t right = max(queuedClearRegion.x + queuedClearRegion.width, region.x + region.width);
            const int32_t bottom = max(queuedClearRegion.y + queuedClearRegion.height, region.y + region.height);
            queuedClearRegion.x = min(queuedClearRegion.x, region.x);
            queuedClearRegion.y = min(queuedClearRegion.y, region.y);
            queuedClearRegion.width = right - queuedClearRegion.x;
            queuedClearRegion.height = bottom - queuedClearRegion.y;
        }
    }
    const bool preserveQuality = framePending.load() &&
        queuedMode != DisplayRefreshMode::FastUi && queuedMode != DisplayRefreshMode::QualityRegion;
    queuedMode = queuedClear ? DisplayRefreshMode::QualityFull
        : queuedClearRegion.width > 0 ? DisplayRefreshMode::RecoveryRegion
        : preserveQuality && mode == DisplayRefreshMode::FastUi ? queuedMode : mode;
    memcpy(queuedFramebuffer, fullFramebuffer, kPainterFramebufferBytes / 2);
    queuedSequence = result.sequence;
    queuedAt = millis();
    framePending = true;
    xSemaphoreGive(frameMutex);
    hardClearBeforeNextRefresh_ = false;
    xTaskNotifyGive(frameTask);
    result.ok = true;
    result.queued = true;
    if (hardClear) result.applied = DisplayRefreshMode::RecoveryFull;
    else if (regionClear) result.area = region;
    result.durationMs =
        static_cast<uint32_t>((esp_timer_get_time() - startedAt) / 1000);
    if (result.ok) {
        if (hardClear) {
            regionalRefreshesSinceFull_ = 0;
        } else if (mode == DisplayRefreshMode::QualityRegion ||
            mode == DisplayRefreshMode::FastUi) {
            ++regionalRefreshesSinceFull_;
        } else {
            regionalRefreshesSinceFull_ = 0;
        }
    }
    return result;
    }
#endif

    // Partial refresh remains disabled. Any regional request is promoted to
    // the visually accepted full clear + grayscale path.  A queued cleanup
    // takes precedence and is consumed only by a valid drawing request.
    if (mode == DisplayRefreshMode::RecoveryFull ||
        hardClearBeforeNextRefresh_) {
        result.applied = DisplayRefreshMode::RecoveryFull;
        hardClearBeforeNextRefresh_ = false;
    } else if (mode == DisplayRefreshMode::QualityRegion || mode == DisplayRefreshMode::RecoveryRegion) {
        result.applied = DisplayRefreshMode::QualityFull;
    }

    result.area = epd_full_screen();
    if (result.area.width <= 0 || result.area.height <= 0) {
        return result;
    }

    const int64_t startedAt = esp_timer_get_time();
    epd_poweron();
    delay(kPowerOnSettleMs);

    switch (result.applied) {
    case DisplayRefreshMode::QualityRegion:
    case DisplayRefreshMode::RecoveryRegion:
        // Promoted above; retained only to keep the switch exhaustive.
        break;

    case DisplayRefreshMode::RecoveryFull:
    case DisplayRefreshMode::QualityFull:
        // Visually accepted on the physical V2.4 panel: the official standard
        // full clear followed by exactly one complete grayscale target.
        epd_clear();
        epd_draw_grayscale_image(result.area,
                                 const_cast<uint8_t *>(fullFramebuffer));
        regionalRefreshesSinceFull_ = 0;
        break;

    case DisplayRefreshMode::PanelRepairWhite:
        // Handled before framebuffer validation and display power-on above.
        break;

    case DisplayRefreshMode::CalibrationGrayscale:
        // This one-shot path has its own precondition guard and is never
        // reached through the normal refresh API.
        break;

    case DisplayRefreshMode::CalibrationTransitionFull:
        // This one-shot path has its own precondition guard and is never
        // reached through the normal refresh API.
        break;

    case DisplayRefreshMode::FastUi:
    case DisplayRefreshMode::ReaderText:
        // The legacy backend preserves its visually validated clear + draw
        // behavior for the new semantic modes.
        epd_clear();
        epd_draw_grayscale_image(result.area,
                                 const_cast<uint8_t *>(fullFramebuffer));
        regionalRefreshesSinceFull_ = 0;
        break;
    }

    delay(kPostDrawSettleMs);
    epd_poweroff();
    delay(kPowerOffSettleMs);
    result.durationMs =
        static_cast<uint32_t>((esp_timer_get_time() - startedAt) / 1000);
    result.ok = true;
    return result;
}

DisplayRefreshResult DisplayRefreshController::repairToWhite() {
    DisplayRefreshResult result{};
    result.requested = DisplayRefreshMode::PanelRepairWhite;
    result.applied = DisplayRefreshMode::PanelRepairWhite;
    result.area = epd_full_screen();
    result.sequence = ++sequence_;

    if (!initialized_) {
        return result;
    }

#ifdef USE_EPD_PAINTER
    {
    const int64_t startedAt = esp_timer_get_time();
    flush();
    if (busy() || !waitForFastPainter()) {
        return result;
    }
    fastPainter.clear(nullptr, 0, EPD_Painter::ClearMode::HARD);
    hardClearBeforeNextRefresh_ = false;
    regionalRefreshesSinceFull_ = 0;
    calibrationDrawnSinceRepair_ = false;
    fullTransitionDrawnSinceBoot_ = false;
    result.durationMs =
        static_cast<uint32_t>((esp_timer_get_time() - startedAt) / 1000);
    result.ok = true;
    frameFailed = false;
    return result;
    }
#endif

    const int64_t startedAt = esp_timer_get_time();
    epd_poweron();
    delay(10);

    // This is deliberately identical to LilyGo-EPD47/examples/screen_repair:
    // clear, dwell through 20 full dark pushes, clear, dwell through 40 full
    // white pushes, and clear once more. The 500 ms dwell is essential; the
    // earlier short recovery cycles were not equivalent to this procedure.
    epd_clear();
    for (int32_t pass = 0; pass < kPanelRepairDarkPasses; ++pass) {
        epd_push_pixels(result.area, kPanelRepairPulseTime, 0);
        delay(kPanelRepairDwellMs);
    }
    epd_clear();
    for (int32_t pass = 0; pass < kPanelRepairWhitePasses; ++pass) {
        epd_push_pixels(result.area, kPanelRepairPulseTime, 1);
        delay(kPanelRepairDwellMs);
    }
    epd_clear();
    epd_poweroff_all();
    delay(kPowerOffSettleMs);

    regionalRefreshesSinceFull_ = 0;
    calibrationDrawnSinceRepair_ = false;
    fullTransitionDrawnSinceBoot_ = false;
    result.durationMs =
        static_cast<uint32_t>((esp_timer_get_time() - startedAt) / 1000);
    result.ok = true;
    return result;
}

DisplayRefreshResult DisplayRefreshController::drawCalibrationOnWhite(
    const uint8_t *fullFramebuffer) {
    DisplayRefreshResult result{};
    result.requested = DisplayRefreshMode::CalibrationGrayscale;
    result.applied = DisplayRefreshMode::CalibrationGrayscale;
    result.area = epd_full_screen();
    result.sequence = ++sequence_;

    if (!initialized_ || fullFramebuffer == nullptr ||
        calibrationDrawnSinceRepair_) {
        return result;
    }

#ifdef USE_EPD_PAINTER
    result = refresh(fullFramebuffer, DisplayRefreshMode::ReaderText);
    result.ok = result.ok && flush();
    result.queued = false;
    result.requested = DisplayRefreshMode::CalibrationGrayscale;
    result.applied = DisplayRefreshMode::CalibrationGrayscale;
    calibrationDrawnSinceRepair_ = result.ok;
    fullTransitionDrawnSinceBoot_ = false;
    return result;
#endif

    const int64_t startedAt = esp_timer_get_time();
    epd_poweron();
    delay(kPowerOnSettleMs);

    // The panel was independently verified white before this call. Drawing
    // once without another clear isolates black density and edge quality from
    // the still-unproven frame-replacement waveform.
    epd_draw_grayscale_image(result.area,
                             const_cast<uint8_t *>(fullFramebuffer));

    delay(kPostDrawSettleMs);
    epd_poweroff();
    delay(kPowerOffSettleMs);

    calibrationDrawnSinceRepair_ = true;
    fullTransitionDrawnSinceBoot_ = false;
    result.durationMs =
        static_cast<uint32_t>((esp_timer_get_time() - startedAt) / 1000);
    result.ok = true;
    return result;
}

DisplayRefreshResult DisplayRefreshController::drawFullTransition(
    const uint8_t *fullFramebuffer) {
    DisplayRefreshResult result{};
    result.requested = DisplayRefreshMode::CalibrationTransitionFull;
    result.applied = DisplayRefreshMode::CalibrationTransitionFull;
    result.area = epd_full_screen();
    result.sequence = ++sequence_;

    if (!initialized_ || fullFramebuffer == nullptr ||
        fullTransitionDrawnSinceBoot_) {
        return result;
    }

#ifdef USE_EPD_PAINTER
    result = refresh(fullFramebuffer, DisplayRefreshMode::RecoveryFull);
    result.ok = result.ok && flush();
    result.queued = false;
    result.requested = DisplayRefreshMode::CalibrationTransitionFull;
    result.applied = DisplayRefreshMode::CalibrationTransitionFull;
    fullTransitionDrawnSinceBoot_ = result.ok;
    return result;
#endif

    const int64_t startedAt = esp_timer_get_time();
    epd_poweron();
    delay(kPowerOnSettleMs);

    // Candidate normal page-turn waveform. Unlike the repair sequence this
    // uses the official bounded clear, then draws exactly one complete target.
    epd_clear();
    epd_draw_grayscale_image(result.area,
                             const_cast<uint8_t *>(fullFramebuffer));

    delay(kPostDrawSettleMs);
    epd_poweroff();
    delay(kPowerOffSettleMs);

    regionalRefreshesSinceFull_ = 0;
    fullTransitionDrawnSinceBoot_ = true;
    result.durationMs =
        static_cast<uint32_t>((esp_timer_get_time() - startedAt) / 1000);
    result.ok = true;
    return result;
}

uint32_t DisplayRefreshController::regionalRefreshesSinceFull() const {
    return regionalRefreshesSinceFull_;
}

void DisplayRefreshController::setIdleTimeout(int seconds) {
#ifdef USE_EPD_PAINTER
    fastPainter.setIdleTimeout(seconds);
#else
    (void)seconds;
#endif
}

void DisplayRefreshController::shutdown() {
    if (!initialized_) {
        return;
    }
#ifdef USE_EPD_PAINTER
    flush();
    if (frameTask) { vTaskDelete(frameTask); frameTask = nullptr; }
    if (frameMutex) { vSemaphoreDelete(frameMutex); frameMutex = nullptr; }
    heap_caps_free(queuedFramebuffer);
    heap_caps_free(painterFramebuffer);
    queuedFramebuffer = nullptr;
    painterFramebuffer = nullptr;
    // The official H716 power-off sequence targets the same shift-register
    // outputs. It is safe after the painter is idle and guarantees that the
    // panel rails are down before ESP deep sleep.
    epd_poweroff_all();
    fastPainter.end();
#else
    epd_poweroff_all();
#endif
    initialized_ = false;
    hardClearBeforeNextRefresh_ = false;
}

bool DisplayRefreshController::busy() const {
#ifdef USE_EPD_PAINTER
    return framePending.load() || frameActive.load();
#else
    return false;
#endif
}

bool DisplayRefreshController::flush(uint32_t timeoutMs) {
#ifdef USE_EPD_PAINTER
    const uint32_t started = millis();
    while (busy() && millis() - started < timeoutMs) delay(1);
    return !busy() && waitForFastPainter() && !frameFailed.load();
#else
    (void)timeoutMs;
    return true;
#endif
}

const char *DisplayRefreshController::modeName(DisplayRefreshMode mode) {
    switch (mode) {
    case DisplayRefreshMode::QualityRegion:
        return "quality-region";
    case DisplayRefreshMode::QualityFull:
        return "quality-full";
    case DisplayRefreshMode::RecoveryFull:
        return "recovery-full";
    case DisplayRefreshMode::PanelRepairWhite:
        return "panel-repair-white";
    case DisplayRefreshMode::CalibrationGrayscale:
        return "calibration-grayscale";
    case DisplayRefreshMode::CalibrationTransitionFull:
        return "calibration-transition-full";
    case DisplayRefreshMode::FastUi:
        return "fast-ui";
    case DisplayRefreshMode::ReaderText:
        return "reader-text";
    case DisplayRefreshMode::RecoveryRegion:
        return "recovery-region";
    }
    return "unknown";
}
