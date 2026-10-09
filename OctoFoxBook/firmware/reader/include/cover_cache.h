#pragma once

#include <Arduino.h>

struct CoverBitmap {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t *pixels = nullptr;
};

struct CoverCacheInfo {
    bool ok = false;
    bool reused = false;
    uint16_t width = 0;
    uint16_t height = 0;
    char error[64]{};
};

class CoverCache {
public:
    static constexpr uint16_t kMaximumWidth = 156;
    static constexpr uint16_t kMaximumHeight = 208;
    static constexpr uint32_t kMaximumSourceBytes = 4U * 1024U * 1024U;

    static bool sourcePath(const char *bookId, char *target,
                           size_t capacity);
    static bool partialSourcePath(const char *bookId, char *target,
                                  size_t capacity);
    static bool derivedPath(const char *bookId, char *target,
                            size_t capacity);
    static bool publishSourcePartial(const char *bookId);
    static bool sourcePresent(const char *bookId);
    static bool build(const char *bookId, CoverCacheInfo &info);
    static bool load(const char *bookId, CoverBitmap &bitmap,
                     CoverCacheInfo &info);
    static void release(CoverBitmap &bitmap);
};
