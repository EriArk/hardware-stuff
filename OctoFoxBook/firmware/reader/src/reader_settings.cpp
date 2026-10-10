#include "reader_settings.h"
#include "text_keyboard.h"
#include "storage_recovery.h"
#include "display_preferences.h"

#include <ArduinoJson.h>
#include <SD.h>

namespace {

constexpr char kSettingsPath[] = "/reader/settings-v1.json";
constexpr char kPartialPath[] = "/reader/settings-v1.json.part";
constexpr char kOldPath[] = "/reader/settings-v1.json.old";

void setError(ReaderSettings &settings, const char *message) {
    snprintf(settings.error, sizeof(settings.error), "%s", message);
}

bool validTextSize(uint8_t value) {
    return value <= static_cast<uint8_t>(ReaderTextSize::Large);
}

bool validLineSpacing(uint8_t value) {
    return value <= static_cast<uint8_t>(ReaderLineSpacing::Airy);
}

bool publish() {
    SD.remove(kOldPath);
    const bool hadPrevious = SD.exists(kSettingsPath);
    if (hadPrevious && !SD.rename(kSettingsPath, kOldPath)) {
        return false;
    }
    if (!SD.rename(kPartialPath, kSettingsPath)) {
        if (hadPrevious) {
            SD.rename(kOldPath, kSettingsPath);
        }
        return false;
    }
    if (hadPrevious) {
        SD.remove(kOldPath);
    }
    return true;
}

}  // namespace

bool ReaderSettingsStore::load(ReaderSettings &settings) {
    settings = ReaderSettings{};
    if (!StorageRecovery::recoverFile(kSettingsPath)) {
        setError(settings, "settings-recovery-failed");
        return false;
    }
    if (!SD.exists(kSettingsPath)) {
        apply(settings);
        return true;
    }
    File input = SD.open(kSettingsPath, FILE_READ);
    if (!input) {
        setError(settings, "settings-open-failed");
        return false;
    }
    JsonDocument document;
    const DeserializationError jsonError = deserializeJson(document, input);
    input.close();
    const char *schema = document["schema"].as<const char *>();
    const uint8_t textSize = document["text_size"] | 1U;
    const uint8_t lineSpacing = document["line_spacing"] | 1U;
    if (jsonError || schema == nullptr ||
        strcmp(schema, "abyss-reader-settings") != 0 ||
        (document["version"] | 0U) != 1 || !validTextSize(textSize) ||
        !validLineSpacing(lineSpacing)) {
        setError(settings, "settings-invalid");
        return false;
    }
    settings.textSize = static_cast<ReaderTextSize>(textSize);
    settings.lineSpacing = static_cast<ReaderLineSpacing>(lineSpacing);
    const unsigned language = document["language"] | 0U;
    const unsigned sleep = document["sleep_minutes"] | 30U;
    const unsigned readingClear = document["reading_clear_every"] | 24U;
    const unsigned uiClear = document["ui_clear_every"] | 4U;
    const unsigned layouts = document["keyboard_layouts"] | 3U;
    const unsigned switchType = document["sleep_switch"] | 0U;
    if (language > 1 || switchType > 1 || !(layouts & 1U) || (layouts & ~TextKeyboard::AllLayouts) ||
        (sleep != 5 && sleep != 15 && sleep != 30 && sleep != 60) ||
        (readingClear != 8 && readingClear != 12 && readingClear != 24) ||
        (uiClear != 1 && uiClear != 2 && uiClear != 4)) {
        setError(settings, "settings-invalid");
        return false;
    }
    settings.language = static_cast<I18n::Language>(language);
    settings.sleepMinutes = sleep;
    settings.readingClearEvery = readingClear;
    settings.uiClearEvery = uiClear;
    settings.sleepLatching = switchType == 1;
    settings.keyboardLayouts = layouts;
    apply(settings);
    return true;
}

bool ReaderSettingsStore::save(const ReaderSettings &settings) {
    if (!StorageRecovery::recoverFile(kSettingsPath)) {
        return false;
    }
    SD.mkdir("/reader");
    SD.remove(kPartialPath);
    File output = SD.open(kPartialPath, FILE_WRITE);
    if (!output) {
        return false;
    }
    JsonDocument document;
    document["schema"] = "abyss-reader-settings";
    document["version"] = 1;
    document["text_size"] = static_cast<uint8_t>(settings.textSize);
    document["language"] = static_cast<uint8_t>(settings.language);
    document["sleep_minutes"] = settings.sleepMinutes;
    document["reading_clear_every"] = settings.readingClearEvery;
    document["ui_clear_every"] = settings.uiClearEvery;
    document["sleep_switch"] = settings.sleepLatching ? 1U : 0U;
    document["keyboard_layouts"] = settings.keyboardLayouts;
    document["line_spacing"] =
        static_cast<uint8_t>(settings.lineSpacing);
    const bool written = serializeJsonPretty(document, output) > 0;
    output.flush();
    output.close();
    if (!written || !publish()) {
        SD.remove(kPartialPath);
        return false;
    }
    return true;
}

void ReaderSettingsStore::apply(const ReaderSettings &settings) {
    I18n::language = settings.language;
    DisplayPreferences::uiClearEvery = settings.uiClearEvery;
    setReaderLayout(settings.textSize, settings.lineSpacing);
}
