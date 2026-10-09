#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace HomeLayout {
constexpr size_t kReadingCapacity = 3; // Continue + two older books.
constexpr size_t kAddedCapacity = 2;
constexpr size_t kSections = 0;
constexpr size_t kFirstBook = 1;

enum class Action { Sections, Sync, Settings, Continue, ReadingCard, AddedCard, Invalid };
inline size_t syncSelection(size_t reading, size_t added) { return kFirstBook + reading + added; }
inline size_t settingsSelection(size_t reading, size_t added) { return syncSelection(reading, added) + 1; }
inline size_t actionCount(size_t reading, size_t added) { return settingsSelection(reading, added) + 1; }
inline Action action(size_t selected, size_t reading, size_t added) {
    if (selected >= actionCount(reading, added)) return Action::Invalid;
    if (selected == kSections) return Action::Sections;
    if (selected == syncSelection(reading, added)) return Action::Sync;
    if (selected == settingsSelection(reading, added)) return Action::Settings;
    if (selected < kFirstBook + reading)
        return selected == kFirstBook ? Action::Continue : Action::ReadingCard;
    return Action::AddedCard;
}
inline size_t initialSelection(size_t reading, size_t added) {
    return reading + added ? kFirstBook : kSections;
}
inline const char *actionName(Action value) {
    switch (value) {
        case Action::Sections: return "sections";
        case Action::Sync: return "sync";
        case Action::Settings: return "settings";
        case Action::Continue: return "continue";
        case Action::ReadingCard: return "recent-card";
        case Action::AddedCard: return "added-card";
        default: return "invalid";
    }
}

// Used by both Home and the local Recently Added section. Stable ties, including
// legacy books without timestamps; reading progress never changes this order.
template<class Entry> bool newer(const Entry &a, const Entry &b) {
    return a.addedAt != b.addedAt ? a.addedAt > b.addedAt : strcmp(a.id, b.id) < 0;
}
template<class Entry>
size_t newest(const Entry *entries, size_t count, uint16_t *out, size_t capacity) {
    size_t found = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t at = 0;
        while (at < found && !newer(entries[i], entries[out[at]])) ++at;
        if (at >= capacity) continue;
        if (found < capacity) ++found;
        for (size_t j = found - 1; j > at; --j) out[j] = out[j - 1];
        out[at] = static_cast<uint16_t>(i);
    }
    return found;
}
} // namespace HomeLayout
