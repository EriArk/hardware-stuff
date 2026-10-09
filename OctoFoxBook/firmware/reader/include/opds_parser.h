#pragma once

#include <Arduino.h>

// The personal server's random feed intentionally contains 25 books. Keep a
// little headroom for compatible feeds without making the fixed PSRAM object
// unbounded.
constexpr size_t kOpdsMaxEntries = 32;
constexpr size_t kOpdsUrlCapacity = 512;

enum class OpdsEntryKind : uint8_t {
    Unknown = 0,
    Navigation = 1,
    Book = 2,
};

struct OpdsEntry {
    OpdsEntryKind kind = OpdsEntryKind::Unknown;
    char title[192]{};
    char author[128]{};
    char summary[1536]{};
    char series[160]{};
    char seriesNumber[32]{};
    char id[128]{};
    char href[kOpdsUrlCapacity]{};
    char acquisitionHref[kOpdsUrlCapacity]{};
    char coverHref[kOpdsUrlCapacity]{};
    char thumbnailHref[kOpdsUrlCapacity]{};
    char authorHref[kOpdsUrlCapacity]{};
    char seriesHref[kOpdsUrlCapacity]{};
};

struct OpdsFeed {
    char title[192]{};
    OpdsEntry entries[kOpdsMaxEntries]{};
    size_t entryCount = 0;
    bool truncated = false;
    char selfHref[kOpdsUrlCapacity]{};
    char startHref[kOpdsUrlCapacity]{};
    char previousHref[kOpdsUrlCapacity]{};
    char nextHref[kOpdsUrlCapacity]{};
    char searchHref[kOpdsUrlCapacity]{};
    char error[64]{};
};

class OpdsParser {
public:
    // Parses the Atom/OPDS subset used by the reader. Namespace prefixes are
    // accepted, XML entities are decoded, and unknown elements are ignored.
    static bool parse(const String &xml, OpdsFeed &feed);
};
