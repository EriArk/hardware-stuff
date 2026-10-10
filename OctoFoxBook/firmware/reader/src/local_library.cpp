#include "i18n.h"
#include "local_library.h"
#include "work_progress.h"

#include <SD.h>

#include <esp_heap_caps.h>

#include <string.h>

#include "book_upload.h"
#include "fb2_cache.h"
#include "reader_pagination.h"

namespace {

void setError(LocalLibraryInfo &info, const char *message) {
    snprintf(info.error, sizeof(info.error), "%s", message);
}

const char *leafName(const char *path) {
    if (path == nullptr) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash == nullptr ? path : slash + 1;
}

void sortEntries(LocalBookEntry *entries, size_t count,
                 LocalBookEntry &scratch) {
    for (size_t index = 1; index < count; ++index) {
        scratch = entries[index];
        size_t target = index;
        while (target > 0 &&
               strcmp(entries[target - 1].title, scratch.title) > 0) {
            entries[target] = entries[target - 1];
            --target;
        }
        entries[target] = scratch;
    }
}

bool loadEntry(const char *bookId, LocalBookEntry &entry,
               Fb2CacheInfo &cacheInfo) {
    memset(&entry, 0, sizeof(entry));
    memset(&cacheInfo, 0, sizeof(cacheInfo));
    snprintf(entry.id, sizeof(entry.id), "%s", bookId);

    char marker[96]{};
    snprintf(marker, sizeof(marker), "/books/%s/sync.pending", bookId);
    if (SD.exists(marker)) return false;
    snprintf(marker, sizeof(marker), "/books/%s/added.txt", bookId);
    File added = SD.open(marker, FILE_READ);
    if (added) {
        char value[24]{};
        added.readBytesUntil('\n', value, sizeof(value) - 1);
        entry.addedAt = strtoul(value, nullptr, 10);
        added.close();
    }

    char sourcePath[96]{};
    if (!BookUploadReceiver::bookPath(bookId, sourcePath,
                                      sizeof(sourcePath))) {
        return false;
    }
    File source = SD.open(sourcePath, FILE_READ);
    if (!source) {
        return false;
    }
    entry.sourceBytes = static_cast<uint32_t>(source.size());
    if (entry.addedAt == 0) entry.addedAt = static_cast<uint32_t>(source.getLastWrite());
    source.close();

    entry.metadataReady = Fb2Cache::load(bookId, cacheInfo);
    if (entry.metadataReady) {
        snprintf(entry.title, sizeof(entry.title), "%s", cacheInfo.title);
        snprintf(entry.author, sizeof(entry.author), "%s", cacheInfo.author);
        snprintf(entry.annotation, sizeof(entry.annotation), "%s",
                 cacheInfo.annotation);
        snprintf(entry.series, sizeof(entry.series), "%s", cacheInfo.series);
        snprintf(entry.seriesNumber, sizeof(entry.seriesNumber), "%s",
                 cacheInfo.seriesNumber);
        snprintf(entry.genre, sizeof(entry.genre), "%s", cacheInfo.genre);
        entry.detailsReady = cacheInfo.detailsReady;
    } else {
        snprintf(entry.title, sizeof(entry.title), "%s", bookId);
        snprintf(entry.author, sizeof(entry.author),
                 I18n::tr("Подготовится при открытии"));
    }

    ReaderPaginationInfo paginationInfo{};
    entry.paginationReady =
        entry.metadataReady && ReaderPagination::load(bookId, paginationInfo);
    if (entry.paginationReady) {
        entry.pageCount = paginationInfo.pageCount;
        ReaderProgress progress{};
        entry.hasProgress = ReaderPagination::loadProgress(
            bookId, paginationInfo.pageCount, progress);
        if (entry.hasProgress) {
            entry.currentPage = progress.currentPage;
            entry.finished = progress.finished;
        }
    }
    return true;
}

}  // namespace

bool LocalLibrary::scan(LocalBookEntry *entries, size_t capacity,
                        LocalLibraryInfo &info) {
    info = LocalLibraryInfo{};
    if (entries == nullptr || capacity == 0) {
        setError(info, "invalid-buffer");
        return false;
    }
    if (!SD.exists("/books")) {
        info.ok = true;
        return true;
    }

    File root = SD.open("/books", FILE_READ);
    if (!root || !root.isDirectory()) {
        root.close();
        setError(info, "books-directory-open-failed");
        return false;
    }

    auto *scratchEntry = static_cast<LocalBookEntry *>(heap_caps_calloc(
        1, sizeof(LocalBookEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *cacheInfo = static_cast<Fb2CacheInfo *>(heap_caps_calloc(
        1, sizeof(Fb2CacheInfo), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratchEntry == nullptr || cacheInfo == nullptr) {
        heap_caps_free(scratchEntry);
        heap_caps_free(cacheInfo);
        root.close();
        setError(info, "scan-workspace-allocation-failed");
        return false;
    }

    while (true) {
        reportWorkProgress();
        File item = root.openNextFile(FILE_READ);
        if (!item) {
            break;
        }
        const bool directory = item.isDirectory();
        char bookId[33]{};
        if (directory) {
            snprintf(bookId, sizeof(bookId), "%s", leafName(item.name()));
        }
        item.close();
        if (!directory || !BookUploadReceiver::validBookId(bookId)) {
            continue;
        }

        if (!loadEntry(bookId, *scratchEntry, *cacheInfo)) {
            continue;
        }
        ++info.totalCount;
        if (info.loadedCount >= capacity) {
            ++info.omittedCount;
            continue;
        }
        entries[info.loadedCount++] = *scratchEntry;
        if (scratchEntry->metadataReady && scratchEntry->paginationReady) {
            ++info.preparedCount;
        }
    }
    root.close();
    sortEntries(entries, info.loadedCount, *scratchEntry);
    heap_caps_free(cacheInfo);
    heap_caps_free(scratchEntry);
    info.ok = true;
    return true;
}
