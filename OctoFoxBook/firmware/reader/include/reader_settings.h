#pragma once

#include <Arduino.h>

#include "reader_text.h"
#include "i18n.h"

struct ReaderSettings {
    ReaderTextSize textSize = ReaderTextSize::Medium;
    ReaderLineSpacing lineSpacing = ReaderLineSpacing::Normal;
    I18n::Language language = I18n::Language::English;
    uint8_t sleepMinutes = 30;
    uint8_t readingClearEvery = 24;
    uint8_t uiClearEvery = 4;
    char error[64]{};
};

class ReaderSettingsStore {
public:
    static bool load(ReaderSettings &settings);
    static bool save(const ReaderSettings &settings);
    static void apply(const ReaderSettings &settings);
};
