#include "reader_settings.h"
#include "display_preferences.h"
#include "SD.h"
#include <cassert>
#include <cstring>
FakeSD SD;
void setReaderLayout(ReaderTextSize,ReaderLineSpacing) {}
int main() {
    const char *path="/reader/settings-v1.json";
    ReaderSettings settings;
    assert(ReaderSettingsStore::load(settings));
    assert(I18n::language==I18n::Language::English && !strcmp(I18n::tr("Настройки"),"Settings"));
    settings.language=I18n::Language::Russian;settings.sleepMinutes=15;settings.readingClearEvery=12;settings.uiClearEvery=2;
    settings.sleepLatching=true;settings.keyboardLayouts=21;
    assert(ReaderSettingsStore::save(settings));
    ReaderSettings restored;assert(ReaderSettingsStore::load(restored));
    assert(restored.sleepLatching && restored.keyboardLayouts==21);
    assert(restored.language==I18n::Language::Russian && restored.sleepMinutes==15 && restored.readingClearEvery==12 && restored.uiClearEvery==2);
    assert(I18n::language==I18n::Language::Russian && !strcmp(I18n::tr("Настройки"),"Настройки"));
    const auto before=SD.files.at(path);SD.failingRename=std::string(path)+".part";
    settings.language=I18n::Language::English;assert(!ReaderSettingsStore::save(settings));assert(SD.files.at(path)==before);
    SD.failingRename.clear();
    // Existing settings without language/preferences migrate to English defaults.
    SD.files[path]=R"({"schema":"abyss-reader-settings","version":1,"text_size":2,"line_spacing":0})";
    assert(ReaderSettingsStore::load(restored));assert(restored.textSize==ReaderTextSize::Large && restored.language==I18n::Language::English && restored.sleepMinutes==30);
    assert(!restored.sleepLatching && restored.keyboardLayouts==3);
    for(const char *bad:{R"("keyboard_layouts":0)",R"("keyboard_layouts":128)",R"("sleep_switch":2)"}) {
        SD.files[path]=std::string(R"({"schema":"abyss-reader-settings","version":1,)")+bad+"}";
        assert(!ReaderSettingsStore::load(restored));
    }
    SD.files[path]=R"({"schema":"abyss-reader-settings","version":1,"language":8})";
    assert(!ReaderSettingsStore::load(restored));
}
