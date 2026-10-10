#pragma once
#include <stdint.h>
#include <string.h>
#include "i18n_catalog.h"

namespace I18n {
enum class Language : uint8_t { English = 0, Russian = 1 };
inline Language language = Language::English;
inline const char *text(const char *english, const char *russian) {
    return language == Language::Russian ? russian : english;
}
// Only explicitly marked application labels pass through this function.
// Book text, titles, SSIDs and user collection names are never translated.
inline const char *tr(const char *russian) {
    if (language == Language::Russian || !russian) return russian;
    for (const auto &entry : catalog) if (!strcmp(entry.ru, russian)) return entry.en;
    return russian;
}
}
