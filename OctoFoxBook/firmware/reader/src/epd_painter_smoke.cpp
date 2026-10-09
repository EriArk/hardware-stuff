#include <Arduino.h>

#include "EPD_Painter_Adafruit.h"
#include "EPD_Painter_presets.h"

namespace {

constexpr uint8_t kPaper = 0;
constexpr uint8_t kLightGray = 1;
constexpr uint8_t kDarkGray = 2;
constexpr uint8_t kInk = 3;
constexpr uint32_t kModeSwitchTimeoutMs = 2500;

// Construct with the exact H716 profile. EPD_PAINTER_PRESET_AUTO remains a
// build flag only because the upstream 2.1.0 translation unit expects its
// probe table to exist; autoDetectBoard() immediately accepts this populated
// config and performs no I2C probing.
EPD_PainterAdafruit display(EPD_LILYGO_EPD47_H716_PRESET, true);

enum class Screen : uint8_t {
    Menu,
    Reader,
    Cover,
};

Screen activeScreen = Screen::Menu;
int selectedRow = 0;
uint32_t submittedFrames = 0;
uint32_t lastCompletedFrames = 0;
uint32_t lastSubmitAtMs = 0;
char serialLine[96] = {};
size_t serialLength = 0;

const char *const kMenuTitles[] = {
    "CONTINUE READING",
    "LOCAL LIBRARY",
    "ONLINE CATALOG",
    "SEARCH",
    "FAVORITES",
    "SETTINGS",
};

void waitForPanelIdle() {
    const uint32_t started = millis();
    while (!display.driver().paintIdle() &&
           millis() - started < kModeSwitchTimeoutMs) {
        delay(1);
    }
}

void selectFastUiMode() {
    waitForPanelIdle();
    display.driver().setGreyLevels(4);
    display.setQuality(EPD_Painter::Quality::QUALITY_FAST);
}

void selectReaderMode() {
    waitForPanelIdle();
    display.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
    display.driver().setGreyLevels(4);
}

void selectCoverMode() {
    waitForPanelIdle();
    display.setQuality(EPD_Painter::Quality::QUALITY_HIGH);
    display.driver().setGreyLevels(16);
}

void drawTopBar(const char *section, const char *mode) {
    display.fillRect(0, 0, display.width(), 78, kInk);
    display.setTextColor(kPaper);
    display.setTextSize(2);
    display.setCursor(22, 18);
    display.print("ABYSS // EPD PAINTER LAB");
    display.setTextSize(1);
    display.setCursor(22, 51);
    display.print(section);
    display.setCursor(395, 51);
    display.print(mode);
}

void drawMenu() {
    display.fillScreen(kPaper);
    drawTopBar("FAST MENU", "4 GREY");

    const int rowX = 20;
    const int rowW = display.width() - 40;
    const int rowH = 121;
    const int rowGap = 14;
    const int top = 101;

    for (int row = 0; row < 6; ++row) {
        const int y = top + row * (rowH + rowGap);
        const bool selected = row == selectedRow;
        const uint8_t fill = selected ? kInk : kPaper;
        const uint8_t ink = selected ? kPaper : kInk;

        display.fillRoundRect(rowX, y, rowW, rowH, 18, fill);
        display.drawRoundRect(rowX, y, rowW, rowH, 18,
                              selected ? kInk : kLightGray);
        display.fillRoundRect(rowX + 17, y + 23, 72, 72, 14,
                              selected ? kPaper : kDarkGray);
        display.setTextColor(selected ? kInk : kPaper);
        display.setTextSize(2);
        display.setCursor(rowX + 36, y + 51);
        if (row + 1 < 10) {
            display.print('0');
        }
        display.print(row + 1);

        display.setTextColor(ink);
        display.setTextSize(3);
        display.setCursor(rowX + 111, y + 30);
        display.print(kMenuTitles[row]);
        display.setTextSize(2);
        display.setCursor(rowX + 111, y + 75);
        display.print(selected ? "READY // ASYNC UPDATE" : "LOCAL ACTION");
    }

    display.setTextColor(kInk);
    display.setTextSize(1);
    display.setCursor(22, 929);
    display.print("UP/DOWN FAST   CENTER READER   BACK MENU");
}

void drawReader() {
    display.fillScreen(kPaper);
    drawTopBar("TEXT PAGE", "NORMAL 4 GREY");
    display.setTextColor(kInk);
    display.setTextSize(3);
    display.setCursor(34, 119);
    display.print("THE FAST PAPER EXPERIMENT");
    display.drawLine(34, 158, 506, 158, kLightGray);

    display.setTextSize(2);
    const char *const lines[] = {
        "A reader does not need eighty frames per second.",
        "It needs the controls to answer immediately and",
        "the page to arrive without making the hand wait.",
        "",
        "This screen uses the calmer four-grey waveform.",
        "Menus use the faster train; covers can use sixteen",
        "greys. Each job gets only the quality it requires.",
        "",
        "CENTER returns to the menu. UP and DOWN repaint",
        "this page so panel response can be measured.",
    };
    int y = 207;
    for (const char *line : lines) {
        display.setCursor(34, y);
        display.print(line);
        y += 58;
    }

    display.drawLine(34, 882, 506, 882, kLightGray);
    display.setTextSize(1);
    display.setCursor(34, 910);
    display.print("NORMAL TEXT // ONE SUBMISSION PER ACTION");
    display.setCursor(474, 910);
    display.print("1");
}

void drawCover() {
    display.fillScreen(kPaper);
    drawTopBar("COVER / LOADING", "HIGH 16 GREY");
    for (int band = 0; band < 16; ++band) {
        const int x = 35 + (band % 4) * 119;
        const int y = 124 + (band / 4) * 160;
        const uint8_t level = static_cast<uint8_t>(band * 17);
        display.fillRoundRect(x, y, 102, 139, 16, level);
    }
    display.setTextColor(kInk);
    display.setTextSize(3);
    display.setCursor(35, 800);
    display.print("SIXTEEN GREY COVER MODE");
    display.setTextSize(1);
    display.setCursor(35, 852);
    display.print("QUALITY IS RESERVED FOR STATIC ARTWORK");
}

void submitFrame(const char *reason, bool blocking = false) {
    const uint32_t startedUs = micros();
    if (blocking) {
        display.paint();
    } else {
        display.paintLater();
    }
    const uint32_t submitUs = micros() - startedUs;
    ++submittedFrames;
    lastSubmitAtMs = millis();
    Serial.printf(
        "EPD SUBMIT reason=%s screen=%u blocking=%s submit_us=%lu "
        "submitted=%lu completed=%lu idle=%s\n",
        reason, static_cast<unsigned>(activeScreen), blocking ? "true" : "false",
        static_cast<unsigned long>(submitUs),
        static_cast<unsigned long>(submittedFrames),
        static_cast<unsigned long>(display.driver().paintsCompleted()),
        display.driver().paintIdle() ? "true" : "false");
}

void renderActiveScreen(const char *reason, bool blocking = false) {
    switch (activeScreen) {
        case Screen::Menu:
            drawMenu();
            break;
        case Screen::Reader:
            drawReader();
            break;
        case Screen::Cover:
            drawCover();
            break;
    }
    submitFrame(reason, blocking);
}

void openMenu(const char *reason) {
    selectFastUiMode();
    activeScreen = Screen::Menu;
    renderActiveScreen(reason);
}

void openReader(const char *reason) {
    selectReaderMode();
    activeScreen = Screen::Reader;
    renderActiveScreen(reason);
}

void openCover(const char *reason) {
    selectCoverMode();
    activeScreen = Screen::Cover;
    renderActiveScreen(reason);
}

void handleCommand(char *line) {
    Serial.printf("EPD COMMAND value=%s received_ms=%lu\n", line,
                  static_cast<unsigned long>(millis()));

    if (strcmp(line, "PING") == 0) {
        Serial.println("PONG epd-painter-smoke 0.1.0");
        return;
    }
    if (strcmp(line, "INPUT UP SHORT") == 0 ||
        strcmp(line, "INPUT UP LONG") == 0) {
        if (activeScreen == Screen::Menu) {
            selectedRow = (selectedRow + 5) % 6;
        }
        renderActiveScreen("input-up");
        return;
    }
    if (strcmp(line, "INPUT DOWN SHORT") == 0 ||
        strcmp(line, "INPUT DOWN LONG") == 0) {
        if (activeScreen == Screen::Menu) {
            selectedRow = (selectedRow + 1) % 6;
        }
        renderActiveScreen("input-down");
        return;
    }
    if (strcmp(line, "INPUT CENTER SHORT") == 0) {
        if (activeScreen == Screen::Menu) {
            openReader("center-reader");
        } else {
            openMenu("center-menu");
        }
        return;
    }
    if (strcmp(line, "INPUT CENTER LONG") == 0 ||
        strcmp(line, "INPUT CENTER DOUBLE") == 0 ||
        strcmp(line, "MODE FAST") == 0) {
        openMenu("return-menu");
        return;
    }
    if (strcmp(line, "MODE NORMAL") == 0) {
        openReader("mode-normal");
        return;
    }
    if (strcmp(line, "MODE HIGH") == 0) {
        openCover("mode-high");
        return;
    }
    if (strcmp(line, "DISPLAY CLEAR") == 0) {
        waitForPanelIdle();
        const uint32_t started = millis();
        display.clear(nullptr, 0, EPD_Painter::ClearMode::HARD);
        Serial.printf("EPD CLEAR mode=hard elapsed_ms=%lu\n",
                      static_cast<unsigned long>(millis() - started));
        renderActiveScreen("after-clear", true);
        return;
    }
    Serial.println("ERROR UNKNOWN_COMMAND");
}

void pollSerial() {
    while (Serial.available() > 0) {
        const char value = static_cast<char>(Serial.read());
        if (value == '\r') {
            continue;
        }
        if (value == '\n') {
            if (serialLength > 0) {
                serialLine[serialLength] = '\0';
                handleCommand(serialLine);
                serialLength = 0;
            }
            continue;
        }
        if (serialLength + 1 < sizeof(serialLine)) {
            serialLine[serialLength++] = value;
        } else {
            serialLength = 0;
            Serial.println("ERROR COMMAND_TOO_LONG");
        }
    }
}

void reportCompletedPaints() {
    const uint32_t completed = display.driver().paintsCompleted();
    if (completed == lastCompletedFrames) {
        return;
    }
    lastCompletedFrames = completed;
    Serial.printf(
        "EPD COMPLETE completed=%lu submitted=%lu since_submit_ms=%lu idle=%s\n",
        static_cast<unsigned long>(completed),
        static_cast<unsigned long>(submittedFrames),
        static_cast<unsigned long>(millis() - lastSubmitAtMs),
        display.driver().paintIdle() ? "true" : "false");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    const uint32_t serialStart = millis();
    while (!Serial && millis() - serialStart < 1500) {
        delay(10);
    }
    Serial.println("BOOT epd-painter-smoke 0.1.0 board=LilyGo-EPD47-H716");

    // EPD_Painter's optional boot controller assumes that a board without a
    // detectable USB charger is battery-only and may immediately enter deep
    // sleep.  The no-touch H716 revision has no responding I2C charger, while
    // USB CDC is provided directly by the ESP32-S3, so that heuristic is not
    // valid here.  Keep the display driver awake for this explicit lab build.
    display.driver().setAutoShutdown(false);

    if (!display.begin()) {
        Serial.println("FATAL EPD_BEGIN_FAILED");
        while (true) {
            delay(1000);
        }
    }

    display.driver().setPaintProfile(true);
    display.driver().setIdleTimeout(0);
    const bool directTransitions =
        display.driver().setDirectTransitions(true);
    Serial.printf("EPD DIRECT_TRANSITIONS enabled=%s\n",
                  directTransitions ? "true" : "false");
    selectFastUiMode();
    const uint32_t clearStarted = millis();
    display.clear(nullptr, 0, EPD_Painter::ClearMode::HARD);
    Serial.printf("EPD CLEAR mode=hard elapsed_ms=%lu\n",
                  static_cast<unsigned long>(millis() - clearStarted));
    activeScreen = Screen::Menu;
    renderActiveScreen("boot-menu", true);
    lastCompletedFrames = display.driver().paintsCompleted();

    Serial.println(
        "READY commands=PING|INPUT UP/DOWN/CENTER SHORT/LONG/DOUBLE|"
        "MODE FAST/NORMAL/HIGH|DISPLAY CLEAR");
}

void loop() {
    pollSerial();
    reportCompletedPaints();
    delay(1);
}
