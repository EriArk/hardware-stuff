#include <Arduino.h>
#include <esp_heap_caps.h>
#include <cstring>
#include "EPD_Painter.h"
#include "EPD_Painter_presets.h"
#include "ui_frames.h"

namespace {
EPD_Painter panel(EPD_LILYGO_EPD47_H716_PRESET, false);
constexpr size_t kPixels = 960 * 540;
constexpr uint8_t kPins[] = {39, 48, 45}; // UP, DOWN, OK on this physical reader.
const char* const kNames[] = {"UP", "DOWN", "OK"};
uint8_t* frame = nullptr;
QueueHandle_t presses = nullptr;
bool ready = false, fault = false;
unsigned style = 0, changes = 0;
char commandBuffer[64];
size_t commandSize = 0;
bool overflow = false;

bool idle() {
    const uint32_t start = millis();
    while (!panel.paintIdle() && millis() - start < 5000) delay(1);
    if (panel.paintIdle()) return true;
    fault = true;
    Serial.println("UI ERROR panel-timeout");
    return false;
}

bool show(unsigned target, const char* source) {
    if (!ready || fault || target > 1 || !idle()) return false;
    const uint32_t start = micros();
    size_t offset = 0;
    for (size_t i=0; i<kUiCounts[target]; ++i) {
        const UiRun r=kUiFrames[target][i];
        if (r.level>3 || r.count>kPixels-offset) {
            fault=true; Serial.println("UI ERROR invalid-frame"); return false;
        }
        memset(frame+offset,r.level,r.count); offset+=r.count;
    }
    if (offset!=kPixels) { fault=true; return false; }
    uint32_t hash=2166136261u;
    for (size_t i=0;i<kPixels;++i) hash=(hash^frame[i])*16777619u;
    if (hash!=kUiHashes[target]) {
        fault=true; Serial.println("UI ERROR frame-hash-mismatch"); return false;
    }
    // Clear each comparison so the previous visual style cannot bias the next.
    panel.clear(nullptr,0,EPD_Painter::ClearMode::HARD);
    if (!idle()) return false;
    panel.paint(frame);
    if (!idle()) return false;
    style=target;
    Serial.printf("UI FRAME style=%u name=%s source=%s fnv1a=%lu total_us=%lu ok=1\n",
                  style,style?"fantasy":"bookish",source,(unsigned long)hash,(unsigned long)(micros()-start));
    return true;
}

void inputTask(void*) {
    bool raw[3], stable[3];
    uint32_t changed[3];
    for (unsigned i=0;i<3;++i) {
        raw[i]=stable[i]=digitalRead(kPins[i]); changed[i]=millis();
    }
    for (;;) {
        const uint32_t now=millis();
        for (uint8_t i=0;i<3;++i) {
            const bool level=digitalRead(kPins[i]);
            if (level!=raw[i]) { raw[i]=level; changed[i]=now; }
            if (raw[i]!=stable[i] && now-changed[i]>=30) {
                stable[i]=raw[i];
                if (!stable[i]) xQueueSend(presses,&i,0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void command(const char* input) {
    if (!strcmp(input,"PING") || !strcmp(input,"UI STATUS")) {
        Serial.printf("UI STATUS version=2 ready=%u fault=%u style=%u changes=%u "
                      "up=%d down=%d ok=%d storage=untouched wifi=off\n",
                      ready,fault,style,changes,digitalRead(39),digitalRead(48),digitalRead(45));
    } else if (!strcmp(input,"UI NEXT")) {
        if (show(1-style,"usb")) ++changes;
    } else if (!strcmp(input,"UI SHOW 0") || !strcmp(input,"UI SHOW 1")) {
        if (show(input[8]-'0',"usb")) ++changes;
    } else if (*input) Serial.println("UI ERROR syntax");
}
}

void setup() {
    Serial.begin(115200);
    for (uint8_t pin:kPins) pinMode(pin,INPUT_PULLUP);
    panel.setAutoShutdown(false);
    ready=panel.begin();
    if (ready) {
        panel.setIdleTimeout(5);
        panel.setGreyLevels(4);
        panel.setDirectTransitions(false);
        panel.setQuality(EPD_Painter::Quality::QUALITY_HIGH);
    }
    frame=static_cast<uint8_t*>(heap_caps_aligned_alloc(16,kPixels,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    presses=xQueueCreate(6,sizeof(uint8_t));
    ready=ready && frame && presses;
    if (ready && xTaskCreate(inputTask,"preview-input",2048,nullptr,1,nullptr)!=pdPASS) ready=false;
    command("UI STATUS");
    if (ready) show(0,"boot");
}

void loop() {
    while (Serial.available()) {
        const char c=Serial.read();
        if (c=='\r') continue;
        if (c=='\n') {
            commandBuffer[commandSize]='\0';
            if (!overflow) command(commandBuffer);
            else Serial.println("UI ERROR command-too-long");
            commandSize=0;overflow=false;
        } else if (commandSize+1<sizeof(commandBuffer)) commandBuffer[commandSize++]=c;
        else overflow=true;
    }
    uint8_t button;
    if (presses && xQueueReceive(presses,&button,0)==pdTRUE) {
        Serial.printf("UI INPUT button=%s pin=%u\n",kNames[button],kPins[button]);
        if (show(1-style,kNames[button])) ++changes;
    }
    delay(1);
}
