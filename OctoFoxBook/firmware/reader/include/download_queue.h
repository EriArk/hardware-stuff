#pragma once

#include <Arduino.h>

#include "opds_parser.h"

constexpr size_t kBookDownloadQueueCapacity = 4;

struct BookDownloadJob {
    char bookId[33]{};
    char title[192]{};
    char author[128]{};
    char acquisitionUrl[kOpdsUrlCapacity]{};
    char coverUrl[kOpdsUrlCapacity]{};
    uint8_t attempts = 0;
    char lastError[64]{};
};

struct BookDownloadQueueState {
    BookDownloadJob jobs[kBookDownloadQueueCapacity]{};
    size_t count = 0;
    char error[64]{};
};

class DownloadQueue {
public:
    static bool load(BookDownloadQueueState &state);
    static bool enqueue(const BookDownloadJob &job,
                        BookDownloadQueueState &state);
    static bool markFailure(const char *bookId, const char *error,
                            BookDownloadQueueState &state);
    static bool remove(const char *bookId, BookDownloadQueueState &state);
    static bool save(const BookDownloadQueueState &state);
};
