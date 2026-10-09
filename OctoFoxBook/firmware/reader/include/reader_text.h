#pragma once

#include <Arduino.h>

#include "epd_driver.h"

enum class ReaderTextSize : uint8_t {
    Small = 0,
    Medium = 1,
    Large = 2,
};

enum class ReaderLineSpacing : uint8_t {
    Compact = 0,
    Normal = 1,
    Airy = 2,
};

struct ReaderLayoutConfig {
    const GFXfont *font = nullptr;
    const char *id = nullptr;
    int32_t leftMargin = 34;
    int32_t rightMargin = 34;
    int32_t firstLineIndent = 28;
    int32_t firstBaseline = 68;
    int32_t headingBaseline = 68;
    int32_t lastBaseline = 865;
    int32_t lineAdvance = 42;
    int32_t headingToBody = 56;
    int32_t paragraphGap = 8;
};

void setReaderLayout(ReaderTextSize textSize,
                     ReaderLineSpacing lineSpacing);
ReaderTextSize readerTextSize();
ReaderLineSpacing readerLineSpacing();
const ReaderLayoutConfig &readerLayout();
const GFXfont *readerBodyFont();
uint32_t decodeReaderUtf8(const char *text, size_t length, size_t &cursor);
int32_t measureReaderUtf8(const String &text);
bool isReaderLayoutSpace(uint32_t codePoint);
