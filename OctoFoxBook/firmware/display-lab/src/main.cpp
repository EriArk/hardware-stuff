#include <Arduino.h>
#include <esp_heap_caps.h>
#include <cstring>

#ifdef LAB_PAINTER
#include "EPD_Painter.h"
#include "EPD_Painter_presets.h"
EPD_Painter panel(EPD_LILYGO_EPD47_H716_PRESET, false);
constexpr const char* kBackend = "painter";
#else
#include "epd_driver.h"
constexpr const char* kBackend = "lilygo";
#endif
#include "frames.h"

namespace {
constexpr size_t kPixels = 960 * 540;
uint8_t* frame = nullptr;
bool ready = false;
bool active = false;
bool fault = false;
unsigned step = 0;
unsigned cadence = 0;
unsigned quality = 0;
unsigned cleanup = 0;
unsigned sceneId = 0;
char line[100];
size_t lineSize = 0;
bool overflow = false;
const char* const kQualities[] = {"HIGH", "NORMAL", "FAST"};
const char* const kCleanups[] = {"NONE", "HARD", "SOFT", "LOCAL"};
const char* const kScenes[] = {"TEXT", "MENU", "COVER", "LIBRARY", "MIXED"};

unsigned targetForStep() {
    if (sceneId == 1) return 4 + step % 2;
    if (sceneId == 2) return 6 + step % 2;
    if (sceneId == 3) return 8 + step % 2;
    if (sceneId == 4) {
        constexpr unsigned sequence[] = {8, 9, 6, 0, 7, 3};
        return sequence[step % 6];
    }
    return step % 4;
}

bool waitPanel() {
#ifdef LAB_PAINTER
    const uint32_t start = millis();
    while (!panel.paintIdle() && millis() - start < 5000) delay(1);
    if (!panel.paintIdle()) {
        active = false;
        fault = true;
        Serial.println("LAB ERROR panel-timeout reset-required=true");
        return false;
    }
#endif
    return true;
}

bool decode(unsigned index) {
    if (index >= sizeof(kFrames) / sizeof(kFrames[0]) || !frame) return false;
    size_t offset = 0;
    for (size_t i = 0; i < kFrameRuns[index]; ++i) {
        const LabRun run = kFrames[index][i];
        if (run.level > 3 || run.count > kPixels - offset) return false;
#ifdef LAB_PAINTER
        memset(frame + offset, run.level, run.count);
#else
        const uint8_t shade = 15 - 5 * run.level;
        for (size_t p = offset; p < offset + run.count; ++p) {
            if (p & 1) frame[p >> 1] = (frame[p >> 1] & 0x0f) | (shade << 4);
            else frame[p >> 1] = (frame[p >> 1] & 0xf0) | shade;
        }
#endif
        offset += run.count;
    }
    return offset == kPixels;
}

void showFrame(bool initial) {
    if (!waitPanel()) return;
    const unsigned target = targetForStep();
    const uint32_t start = micros();
    if (!decode(target)) {
        active = false;
        Serial.println("LAB ERROR invalid-target");
        return;
    }
    const uint32_t decoded = micros();
    const char* cleared = "NONE";
#ifdef LAB_PAINTER
    if (initial || (cadence && step % cadence == 0 && cleanup)) {
        if (initial || cleanup == 1) {
            panel.clear(nullptr, 0, EPD_Painter::ClearMode::HARD);
            cleared = "HARD";
        } else if (cleanup == 2) {
            panel.clear(nullptr, 0, EPD_Painter::ClearMode::SOFT);
            cleared = "SOFT";
        } else {
            panel.clearDirtyAreas(frame, 0, EPD_Painter::ClearMode::SOFT);
            cleared = "LOCAL";
        }
        if (!waitPanel()) return;
    }
    const uint32_t cleaned = micros();
    panel.paint(frame);
    if (!waitPanel()) return;
#else
    epd_poweron();
    // Exact existing LilyGo calls: no custom voltages or waveforms.
    if (initial || quality == 0) epd_clear();
    else epd_clear_area_cycles(epd_full_screen(), quality == 1 ? 2 : 1,
                               quality == 1 ? 45 : 30);
    cleared = initial || quality == 0 ? "FULL4" : quality == 1 ? "FULL2" : "FULL1";
    const uint32_t cleaned = micros();
    epd_draw_grayscale_image(epd_full_screen(), frame);
    epd_poweroff_all();
#endif
    const uint32_t done = micros();
    Serial.printf("LAB FRAME backend=%s quality=%s policy=%s cadence=%u scene=%s "
                  "step=%u target=%u initial=%u cleanup=%s decode_us=%lu clean_us=%lu "
                  "paint_us=%lu total_us=%lu ok=1\n", kBackend, kQualities[quality],
                  kCleanups[cleanup], cadence, kScenes[sceneId], step, target,
                  initial ? 1 : 0, cleared, (unsigned long)(decoded-start),
                  (unsigned long)(cleaned-decoded), (unsigned long)(done-cleaned),
                  (unsigned long)(done-start));
}

void command(const char* input) {
    if (!strcmp(input, "PING") || !strcmp(input, "LAB STATUS")) {
        Serial.printf("LAB STATUS version=2 fixtures=2 backend=%s ready=%u active=%u fault=%u step=%u "
                      "storage=untouched wifi=off\n", kBackend, ready, active, fault, step);
        return;
    }
    if (!ready || fault) { Serial.println("LAB ERROR not-ready"); return; }
    if (!strcmp(input, "LAB STOP")) {
        active = false;
        waitPanel();
        Serial.println("LAB STOPPED");
        return;
    }
    if (!strcmp(input, "LAB STEP")) {
        if (!active || step >= 48) { Serial.println("LAB ERROR start-required"); return; }
        ++step;
        showFrame(false);
        return;
    }
    char q[12], c[12], scene[12], extra;
    unsigned interval;
    if (sscanf(input, "LAB START %11s %11s %u %11s %c", q, c, &interval, scene, &extra) != 4) {
        Serial.println("LAB ERROR syntax"); return;
    }
    int qi = -1, ci = -1, si = -1;
    for (int i = 0; i < 3; ++i) if (!strcmp(q, kQualities[i])) qi = i;
    for (int i = 0; i < 4; ++i) if (!strcmp(c, kCleanups[i])) ci = i;
    for (int i = 0; i < 5; ++i) if (!strcmp(scene, kScenes[i])) si = i;
    if (qi < 0 || ci < 0 || si < 0 ||
        interval > 24 || (ci == 0 ? interval != 0 : interval == 0)) {
        Serial.println("LAB ERROR invalid-profile"); return;
    }
#ifndef LAB_PAINTER
    if (ci != 0 || interval != 0) { Serial.println("LAB ERROR lilygo-clears-every-frame"); return; }
#endif
    if (!waitPanel()) return;
    quality = qi; cleanup = ci; cadence = interval; sceneId = si; step = 0;
#ifdef LAB_PAINTER
    panel.setQuality(quality == 0 ? EPD_Painter::Quality::QUALITY_HIGH :
                     quality == 1 ? EPD_Painter::Quality::QUALITY_NORMAL :
                                    EPD_Painter::Quality::QUALITY_FAST);
#endif
    active = true;
    showFrame(true);
}
} // namespace

void setup() {
    Serial.begin(115200);
#ifdef LAB_PAINTER
    panel.setAutoShutdown(false);
    ready = panel.begin();
    if (ready) {
        panel.setIdleTimeout(5);
        panel.setGreyLevels(4);
        panel.setDirectTransitions(false);
    }
    frame = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, kPixels, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    epd_init();
    epd_poweroff_all();
    ready = true;
    frame = static_cast<uint8_t*>(heap_caps_malloc(kPixels / 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
    ready = ready && frame;
    // Wait for an explicit trial. Never mount SD or open/write NVS settings.
    command("LAB STATUS");
}

void loop() {
    while (Serial.available()) {
        const char ch = Serial.read();
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[lineSize] = '\0';
            if (overflow) Serial.println("LAB ERROR command-too-long");
            else command(line);
            lineSize = 0; overflow = false;
        } else if (lineSize + 1 < sizeof(line)) line[lineSize++] = ch;
        else overflow = true;
    }
    delay(1);
}
