#include "storage_recovery.h"

#include <SD.h>

#include <string.h>

#include "book_upload.h"

namespace {

constexpr char kTestBookDirectory[] = "/books/recovery-test";
constexpr char kTestMarker[] = "/reader-diagnostics/recovery-test.armed";
constexpr char kRecoveredBookPayload[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\">"
    "<description><title-info><book-title>Recovery fixture</book-title>"
    "<author><first-name>Abyss</first-name><last-name>Test</last-name>"
    "</author></title-info></description><body><section><title><p>Recovery"
    "</p></title><p>The previous canonical source must survive an interrupted "
    "atomic publication. This reserved fixture is removed after acceptance."
    "</p></section></body></FictionBook>";
constexpr char kRecoveredStatePayload[] = "RECOVERY_STATE_OLD_V1\n";
constexpr char kPublishedMetadataPayload[] = "RECOVERY_METADATA_FINAL_V1\n";
constexpr char kStalePayload[] = "STALE_OR_PARTIAL\n";

constexpr const char *kAtomicBookArtifacts[] = {
    "book.fb2",
    "metadata.json",
    "reader-state.json",
    "cache/content-v1.txt",
    "cache/pages-v1.txt",
    "cache/pages-v1.idx",
    "cache/chapters-v1.txt",
    "cache/chapters-v2.txt",
};

const char *leafName(const char *path) {
    if (path == nullptr) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash == nullptr ? path : slash + 1;
}

bool removeTracked(const char *path, uint16_t &counter,
                   StorageRecoveryReport &report) {
    if (!SD.exists(path)) {
        return true;
    }
    if (!SD.remove(path)) {
        ++report.errors;
        return false;
    }
    ++counter;
    return true;
}

bool reconcileAtomicFile(const char *finalPath, const char *partialPath,
                         const char *oldPath,
                         StorageRecoveryReport &report) {
    bool passed = true;
    bool finalExists = SD.exists(finalPath);
    const bool oldExists = SD.exists(oldPath);
    if (!finalExists && oldExists) {
        if (SD.rename(oldPath, finalPath)) {
            ++report.filesRestored;
            finalExists = true;
        } else {
            ++report.errors;
            passed = false;
        }
    } else if (finalExists && oldExists) {
        passed = removeTracked(oldPath, report.rollbackCopiesRemoved, report) &&
                 passed;
    }

    // A .part file is never authoritative. The writer has already closed or
    // disappeared when boot recovery runs; only a published final or the
    // rollback copy may survive.
    passed = removeTracked(partialPath, report.partialsRemoved, report) &&
             passed;
    return passed;
}

bool reconcileBook(const char *bookId, StorageRecoveryReport &report) {
    bool passed = true;
    char finalPath[128]{};
    char partialPath[136]{};
    char oldPath[136]{};
    for (const char *artifact : kAtomicBookArtifacts) {
        const int finalWritten = snprintf(finalPath, sizeof(finalPath),
                                          "/books/%s/%s", bookId, artifact);
        const int partialWritten = snprintf(partialPath, sizeof(partialPath),
                                            "%s.part", finalPath);
        const int oldWritten = snprintf(oldPath, sizeof(oldPath), "%s.old",
                                        finalPath);
        if (finalWritten <= 0 || partialWritten <= 0 || oldWritten <= 0 ||
            static_cast<size_t>(finalWritten) >= sizeof(finalPath) ||
            static_cast<size_t>(partialWritten) >= sizeof(partialPath) ||
            static_cast<size_t>(oldWritten) >= sizeof(oldPath)) {
            ++report.errors;
            passed = false;
            continue;
        }
        passed = reconcileAtomicFile(finalPath, partialPath, oldPath, report) &&
                 passed;
    }
    return passed;
}

bool writeText(const char *path, const char *text) {
    SD.remove(path);
    File output = SD.open(path, FILE_WRITE);
    if (!output) {
        return false;
    }
    const size_t length = strlen(text);
    const bool written = output.write(
                             reinterpret_cast<const uint8_t *>(text), length) ==
                         length;
    output.flush();
    output.close();
    return written;
}

bool fileMatches(const char *path, const char *expected) {
    File input = SD.open(path, FILE_READ);
    if (!input) {
        return false;
    }
    const size_t expectedLength = strlen(expected);
    bool matches = input.size() == expectedLength;
    for (size_t index = 0; matches && index < expectedLength; ++index) {
        matches = input.read() == static_cast<uint8_t>(expected[index]);
    }
    input.close();
    return matches;
}

bool removeIfPresent(const char *path) {
    return !SD.exists(path) || SD.remove(path);
}

}  // namespace

bool StorageRecovery::recoverFile(const char *finalPath) {
    if (finalPath == nullptr || finalPath[0] == '\0') {
        return false;
    }
    char partialPath[136]{};
    char oldPath[136]{};
    const int partialSize = snprintf(partialPath, sizeof(partialPath), "%s.part", finalPath);
    const int oldSize = snprintf(oldPath, sizeof(oldPath), "%s.old", finalPath);
    if (partialSize <= 0 || oldSize <= 0 ||
        static_cast<size_t>(partialSize) >= sizeof(partialPath) ||
        static_cast<size_t>(oldSize) >= sizeof(oldPath)) {
        return false;
    }
    StorageRecoveryReport report{};
    return reconcileAtomicFile(finalPath, partialPath, oldPath, report);
}

bool StorageRecovery::run(StorageRecoveryReport &report) {
    report = StorageRecoveryReport{};
    bool passed = true;
    passed = reconcileAtomicFile(
                 "/reader/download-queue-v1.json",
                 "/reader/download-queue-v1.json.part",
                 "/reader/download-queue-v1.json.old", report) &&
             passed;
    passed = reconcileAtomicFile(
                 "/reader/recent-v1.json", "/reader/recent-v1.json.part",
                 "/reader/recent-v1.json.old", report) &&
             passed;
    passed = reconcileAtomicFile(
                 "/reader/settings-v1.json", "/reader/settings-v1.json.part",
                 "/reader/settings-v1.json.old", report) &&
             passed;
    passed = reconcileAtomicFile(
                 "/reader/favorites-v1.json", "/reader/favorites-v1.json.part",
                 "/reader/favorites-v1.json.old", report) &&
             passed;

    if (SD.exists("/books")) {
        File root = SD.open("/books", FILE_READ);
        if (!root || !root.isDirectory()) {
            if (root) {
                root.close();
            }
            ++report.errors;
            report.ok = false;
            return false;
        }
        while (true) {
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
            ++report.booksScanned;
            passed = reconcileBook(bookId, report) && passed;
        }
        root.close();
    }
    report.ok = passed && report.errors == 0;
    return report.ok;
}

bool StorageRecovery::cleanupSelfTest() {
    constexpr const char *paths[] = {
        "/books/recovery-test/book.fb2",
        "/books/recovery-test/book.fb2.part",
        "/books/recovery-test/book.fb2.old",
        "/books/recovery-test/reader-state.json",
        "/books/recovery-test/reader-state.json.part",
        "/books/recovery-test/reader-state.json.old",
        "/books/recovery-test/metadata.json",
        "/books/recovery-test/metadata.json.part",
        "/books/recovery-test/metadata.json.old",
        kTestMarker,
    };
    bool passed = true;
    for (const char *path : paths) {
        passed = removeIfPresent(path) && passed;
    }
    if (SD.exists("/books/recovery-test/cache")) {
        passed = SD.rmdir("/books/recovery-test/cache") && passed;
    }
    if (SD.exists(kTestBookDirectory)) {
        passed = SD.rmdir(kTestBookDirectory) && passed;
    }
    return passed;
}

bool StorageRecovery::armSelfTest() {
    if (!cleanupSelfTest() ||
        (!SD.exists("/reader-diagnostics") &&
         !SD.mkdir("/reader-diagnostics")) ||
        (!SD.exists("/books") && !SD.mkdir("/books")) ||
        !SD.mkdir(kTestBookDirectory)) {
        cleanupSelfTest();
        return false;
    }

    const bool written =
        writeText("/books/recovery-test/book.fb2.old",
                  kRecoveredBookPayload) &&
        writeText("/books/recovery-test/book.fb2.part", kStalePayload) &&
        writeText("/books/recovery-test/reader-state.json.old",
                  kRecoveredStatePayload) &&
        writeText("/books/recovery-test/reader-state.json.part",
                  kStalePayload) &&
        writeText("/books/recovery-test/metadata.json",
                  kPublishedMetadataPayload) &&
        writeText("/books/recovery-test/metadata.json.old", kStalePayload) &&
        writeText("/books/recovery-test/metadata.json.part", kStalePayload) &&
        writeText(kTestMarker, "ARMED\n");
    if (!written) {
        cleanupSelfTest();
    }
    return written;
}

bool StorageRecovery::verifySelfTest(StorageRecoverySelfTestResult &result) {
    result = StorageRecoverySelfTestResult{};
    if (!SD.exists(kTestMarker)) {
        return false;
    }
    result.recoveredBook = fileMatches(
        "/books/recovery-test/book.fb2", kRecoveredBookPayload);
    result.recoveredState = fileMatches(
        "/books/recovery-test/reader-state.json", kRecoveredStatePayload);
    result.preservedPublishedMetadata = fileMatches(
        "/books/recovery-test/metadata.json", kPublishedMetadataPayload);
    constexpr const char *leftovers[] = {
        "/books/recovery-test/book.fb2.part",
        "/books/recovery-test/book.fb2.old",
        "/books/recovery-test/reader-state.json.part",
        "/books/recovery-test/reader-state.json.old",
        "/books/recovery-test/metadata.json.part",
        "/books/recovery-test/metadata.json.old",
    };
    result.leftoversAbsent = true;
    for (const char *path : leftovers) {
        result.leftoversAbsent = result.leftoversAbsent && !SD.exists(path);
    }
    result.cleanupPassed = cleanupSelfTest();
    return result.recoveredBook && result.recoveredState &&
           result.preservedPublishedMetadata && result.leftoversAbsent &&
           result.cleanupPassed;
}
