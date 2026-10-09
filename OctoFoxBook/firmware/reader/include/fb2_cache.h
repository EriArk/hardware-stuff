#pragma once

#include <Arduino.h>

struct Fb2CacheInfo {
    bool ok = false;
    bool detailsReady = false;
    uint32_t sourceBytes = 0;
    uint32_t records = 0;
    char title[160]{};
    char author[128]{};
    char annotation[1536]{};
    char series[160]{};
    char seriesNumber[32]{};
    char genre[128]{};
    char error[64]{};
};

class Fb2Cache {
public:
    static bool build(const char *bookId, Fb2CacheInfo &info);
    static bool load(const char *bookId, Fb2CacheInfo &info);
    static bool contentPath(const char *bookId, char *target,
                            size_t capacity);
};
