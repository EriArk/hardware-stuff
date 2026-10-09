#pragma once

#include <Arduino.h>

#include "reader_text.h"

struct ReaderSettings {
    ReaderTextSize textSize = ReaderTextSize::Medium;
    ReaderLineSpacing lineSpacing = ReaderLineSpacing::Normal;
    char error[64]{};
};

class ReaderSettingsStore {
public:
    static bool load(ReaderSettings &settings);
    static bool save(const ReaderSettings &settings);
    static void apply(const ReaderSettings &settings);
};
