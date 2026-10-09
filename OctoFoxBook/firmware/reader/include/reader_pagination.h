#pragma once

#include <Arduino.h>

struct ReaderPaginationInfo {
    bool ok = false;
    uint32_t pageCount = 0;
    uint32_t chapterCount = 0;
    uint32_t pagesBytes = 0;
    uint32_t sourceRecords = 0;
    char error[64]{};
};

struct ReaderPageIndexEntry {
    uint32_t fileOffset = 0;
    uint32_t sourceRecord = 0;
    uint32_t sourceByte = 0;
};

struct ReaderChapterEntry {
    uint32_t ordinal = 0;
    uint32_t page = 0;
    uint32_t sourceRecord = 0;
    char title[160]{};
};

struct ReaderProgress {
    bool found = false;
    bool finished = false;
    uint32_t currentPage = 1;
    uint32_t pageCount = 0;
    ReaderPageIndexEntry logical{};
    char error[64]{};
};

constexpr size_t kReaderBookmarkCapacity = 16;

struct ReaderBookmark {
    uint32_t page = 0;
    ReaderPageIndexEntry logical{};
    char title[160]{};
};

struct ReaderUserState {
    bool found = false;
    bool finished = false;
    size_t bookmarkCount = 0;
    ReaderBookmark bookmarks[kReaderBookmarkCapacity]{};
    char error[64]{};
};

class ReaderPagination {
public:
    static const char *layoutId();
    static bool build(const char *bookId, ReaderPaginationInfo &info);
    static bool load(const char *bookId, ReaderPaginationInfo &info);
    static bool pageEntry(const char *bookId, uint32_t page,
                          ReaderPageIndexEntry &entry);
    static bool pageForLogical(const char *bookId,
                               const ReaderPageIndexEntry &logical,
                               uint32_t &page);
    static bool currentChapter(const char *bookId, uint32_t page,
                               ReaderChapterEntry &entry);
    static bool chapterEntry(const char *bookId, uint32_t ordinal,
                             ReaderChapterEntry &entry);
    static bool adjacentChapter(const char *bookId, uint32_t page,
                                int8_t direction,
                                ReaderChapterEntry &entry);
    static bool pagesPath(const char *bookId, char *target, size_t capacity);
    static bool loadProgress(const char *bookId, uint32_t pageCount,
                             ReaderProgress &progress);
    static bool loadPortableState(const char *bookId,
                                  ReaderProgress &progress,
                                  ReaderUserState &state);
    static bool saveProgress(const char *bookId, uint32_t page,
                             uint32_t pageCount,
                             const ReaderPageIndexEntry &logical,
                             ReaderProgress &progress);
    static bool loadUserState(const char *bookId, uint32_t pageCount,
                              ReaderUserState &state);
    static bool toggleBookmark(const char *bookId, uint32_t page,
                               uint32_t pageCount,
                               const ReaderPageIndexEntry &logical,
                               const char *title, bool &added,
                               ReaderUserState &state);
    static bool setFinished(const char *bookId, uint32_t pageCount,
                            bool finished, ReaderUserState &state);
};
