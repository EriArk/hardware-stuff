#pragma once

#include <Arduino.h>

struct LocalBookEntry {
    char id[33]{};
    char title[160]{};
    char author[128]{};
    char annotation[1536]{};
    char series[160]{};
    char seriesNumber[32]{};
    char genre[128]{};
    uint32_t sourceBytes = 0;
    uint32_t addedAt = 0;
    uint32_t currentPage = 0;
    uint32_t pageCount = 0;
    bool metadataReady = false;
    bool detailsReady = false;
    bool paginationReady = false;
    bool hasProgress = false;
    bool finished = false;
};

struct LocalLibraryInfo {
    bool ok = false;
    uint32_t totalCount = 0;
    uint32_t loadedCount = 0;
    uint32_t preparedCount = 0;
    uint32_t omittedCount = 0;
    char error[64]{};
};

class LocalLibrary {
public:
    static bool scan(LocalBookEntry *entries, size_t capacity,
                     LocalLibraryInfo &info);
};
