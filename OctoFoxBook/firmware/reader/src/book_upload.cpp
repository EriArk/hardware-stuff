#include "book_upload.h"

#include <ctype.h>
#include <string.h>

namespace {

constexpr uint32_t kUploadIdleTimeoutMs = 30000;
// Native USB CDC on this board has no hardware receive flow control. A small
// stop-and-wait window prevents the Windows transmit queue from overrunning
// the device-side ring while SD writes are in progress.
constexpr uint32_t kUploadProgressWindowBytes = 256;
// The host uses the same 256-byte stop-and-wait window. Keeping the receive
// buffer to one window avoids consuming half of Arduino's loop-task stack.
constexpr size_t kTransferBufferBytes = kUploadProgressWindowBytes;

bool validSha256(const char *value) {
    if (value == nullptr || strlen(value) != 64) {
        return false;
    }
    for (size_t index = 0; index < 64; ++index) {
        if (!isxdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

void lowercaseCopy(char *target, const char *source, size_t capacity) {
    size_t index = 0;
    for (; source[index] != '\0' && index + 1 < capacity; ++index) {
        target[index] = static_cast<char>(
            tolower(static_cast<unsigned char>(source[index])));
    }
    target[index] = '\0';
}

bool removeIfPresent(const char *path) {
    return !SD.exists(path) || SD.remove(path);
}

bool invalidateDerivedBookFiles(const char *bookId) {
    constexpr const char *kArtifacts[] = {
        "cache/content-v1.txt",     "cache/content-v1.txt.part",
        "cache/content-v1.txt.old", "cache/pages-v1.txt",
        "cache/pages-v1.txt.part",  "cache/pages-v1.txt.old",
        "cache/pages-v1.idx",       "cache/pages-v1.idx.part",
        "cache/pages-v1.idx.old",   "cache/chapters-v1.txt",
        "cache/chapters-v1.txt.part",
        "cache/chapters-v1.txt.old", "cache/chapters-v2.txt",
        "cache/chapters-v2.txt.part",
        "cache/chapters-v2.txt.old", "metadata.json",
        "metadata.json.part",       "metadata.json.old",
        "reader-state.json",        "reader-state.json.part",
        "reader-state.json.old",
    };
    char path[112]{};
    for (const char *artifact : kArtifacts) {
        const int written = snprintf(path, sizeof(path), "/books/%s/%s",
                                     bookId, artifact);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(path) ||
            !removeIfPresent(path)) {
            return false;
        }
    }
    return true;
}

}  // namespace

BookUploadReceiver::BookUploadReceiver() {
    mbedtls_sha256_init(&sha_);
    shaInitialized_ = true;
}

BookUploadReceiver::~BookUploadReceiver() {
    if (output_) {
        output_.close();
    }
    if (shaInitialized_) {
        mbedtls_sha256_free(&sha_);
    }
}

bool BookUploadReceiver::validBookId(const char *bookId) {
    if (bookId == nullptr) {
        return false;
    }
    const size_t length = strlen(bookId);
    if (length == 0 || length > 32) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        const unsigned char value =
            static_cast<unsigned char>(bookId[index]);
        if (!isalnum(value) && value != '-' && value != '_') {
            return false;
        }
    }
    return true;
}

bool BookUploadReceiver::bookPath(const char *bookId, char *target,
                                  size_t capacity) {
    if (!validBookId(bookId) || target == nullptr || capacity == 0) {
        return false;
    }
    const int written =
        snprintf(target, capacity, "/books/%s/book.fb2", bookId);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

bool BookUploadReceiver::partialBookPath(const char *bookId, char *target,
                                         size_t capacity) {
    if (!validBookId(bookId) || target == nullptr || capacity == 0) {
        return false;
    }
    const int written =
        snprintf(target, capacity, "/books/%s/book.fb2.part", bookId);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

bool BookUploadReceiver::ensureBookDirectory(const char *bookId) {
    if (!validBookId(bookId)) {
        return false;
    }
    char directory[64]{};
    const int written =
        snprintf(directory, sizeof(directory), "/books/%s", bookId);
    return written > 0 && static_cast<size_t>(written) < sizeof(directory) &&
           (SD.exists("/books") || SD.mkdir("/books")) &&
           (SD.exists(directory) || SD.mkdir(directory));
}

bool BookUploadReceiver::invalidateDerivedFiles(const char *bookId) {
    return validBookId(bookId) && invalidateDerivedBookFiles(bookId);
}

bool BookUploadReceiver::archiveBook(const char *bookId) {
    if (!validBookId(bookId)) return false;
    char source[64]{}, target[96]{};
    snprintf(source, sizeof(source), "/books/%s", bookId);
    if (!SD.exists(source)) return true; // Replayed removal after lost ACK.
    if (!SD.exists("/trash") && !SD.mkdir("/trash")) return false;
    for (unsigned slot = 0; slot < 10000; ++slot) {
        snprintf(target, sizeof(target), "/trash/%s-%u", bookId, slot);
        if (!SD.exists(target)) return SD.rename(source, target);
    }
    return false;
}

bool BookUploadReceiver::publishPartial(const char *bookId) {
    char directory[64]{};
    char partialPath[96]{};
    char finalPath[96]{};
    char oldPath[96]{};
    if (!validBookId(bookId) ||
        snprintf(directory, sizeof(directory), "/books/%s", bookId) <= 0 ||
        !partialBookPath(bookId, partialPath, sizeof(partialPath)) ||
        !bookPath(bookId, finalPath, sizeof(finalPath)) ||
        snprintf(oldPath, sizeof(oldPath), "%s/book.fb2.old", directory) <=
            0 ||
        !SD.exists(partialPath)) {
        return false;
    }

    SD.remove(oldPath);
    const bool hadPrevious = SD.exists(finalPath);
    if (hadPrevious && !SD.rename(finalPath, oldPath)) {
        return false;
    }
    if (!SD.rename(partialPath, finalPath)) {
        if (hadPrevious) {
            SD.rename(oldPath, finalPath);
        }
        return false;
    }
    if (hadPrevious) {
        // Publication is already complete. Cleanup is best effort so callers
        // never retry a successful download merely because removal of the
        // disposable rollback copy failed.
        SD.remove(oldPath);
    }
    return true;
}

bool BookUploadReceiver::start(const char *bookId, uint32_t expectedBytes,
                               const char *expectedSha256, Print &output) {
    if (active_) {
        output.println("ERROR BOOK_PUT busy=true");
        return false;
    }
    if (!validBookId(bookId)) {
        output.println("ERROR BOOK_PUT reason=invalid-book-id");
        return false;
    }
    if (expectedBytes < kMinimumBookBytes ||
        expectedBytes > kMaximumBookBytes) {
        output.println("ERROR BOOK_PUT reason=invalid-size");
        return false;
    }
    if (!validSha256(expectedSha256)) {
        output.println("ERROR BOOK_PUT reason=invalid-sha256");
        return false;
    }

    snprintf(bookId_, sizeof(bookId_), "%s", bookId);
    lowercaseCopy(expectedSha256_, expectedSha256,
                  sizeof(expectedSha256_));
    snprintf(directory_, sizeof(directory_), "/books/%s", bookId_);
    snprintf(partialPath_, sizeof(partialPath_), "%s/book.fb2.part",
             directory_);
    snprintf(finalPath_, sizeof(finalPath_), "%s/book.fb2", directory_);

    if ((!SD.exists("/books") && !SD.mkdir("/books")) ||
        (!SD.exists(directory_) && !SD.mkdir(directory_))) {
        output.println("ERROR BOOK_PUT reason=mkdir-failed");
        reset();
        return false;
    }

    SD.remove(partialPath_);
    output_ = SD.open(partialPath_, FILE_WRITE);
    if (!output_) {
        output.println("ERROR BOOK_PUT reason=open-part-failed");
        reset();
        return false;
    }

    mbedtls_sha256_starts_ret(&sha_, 0);
    expectedBytes_ = expectedBytes;
    receivedBytes_ = 0;
    nextProgressBytes_ = kUploadProgressWindowBytes;
    lastDataAt_ = millis();
    active_ = true;
    output.printf("BOOK PUT READY id=%s bytes=%lu\n", bookId_,
                  static_cast<unsigned long>(expectedBytes_));
    return true;
}

void BookUploadReceiver::poll(Stream &input, Print &output) {
    if (!active_) {
        return;
    }

    if (millis() - lastDataAt_ > kUploadIdleTimeoutMs) {
        fail("timeout", output);
        return;
    }

    const int available = input.available();
    if (available <= 0) {
        return;
    }

    uint8_t buffer[kTransferBufferBytes];
    const uint32_t remaining = expectedBytes_ - receivedBytes_;
    const size_t requested = min(
        static_cast<size_t>(available),
        min(static_cast<size_t>(remaining), kTransferBufferBytes));
    size_t count = 0;
    while (count < requested) {
        const int value = input.read();
        if (value < 0) {
            break;
        }
        buffer[count++] = static_cast<uint8_t>(value);
    }
    if (count == 0) {
        return;
    }

    if (output_.write(buffer, count) != count) {
        fail("sd-write-failed", output);
        return;
    }
    mbedtls_sha256_update_ret(&sha_, buffer, count);
    receivedBytes_ += static_cast<uint32_t>(count);
    lastDataAt_ = millis();

    if (receivedBytes_ == expectedBytes_) {
        finish(output);
    } else if (receivedBytes_ >= nextProgressBytes_) {
        output.printf("BOOK PUT PROGRESS id=%s received=%lu\n", bookId_,
                      static_cast<unsigned long>(receivedBytes_));
        nextProgressBytes_ += kUploadProgressWindowBytes;
    }
}

bool BookUploadReceiver::active() const {
    return active_;
}

void BookUploadReceiver::fail(const char *reason, Print &output) {
    if (output_) {
        output_.close();
    }
    SD.remove(partialPath_);
    output.printf("ERROR BOOK_PUT reason=%s id=%s received=%lu expected=%lu\n",
                  reason, bookId_, static_cast<unsigned long>(receivedBytes_),
                  static_cast<unsigned long>(expectedBytes_));
    reset();
}

void BookUploadReceiver::finish(Print &output) {
    output_.flush();
    output_.close();

    uint8_t digest[32]{};
    mbedtls_sha256_finish_ret(&sha_, digest);
    char actualSha256[65]{};
    for (size_t index = 0; index < sizeof(digest); ++index) {
        snprintf(actualSha256 + index * 2, 3, "%02x", digest[index]);
    }
    if (strcmp(actualSha256, expectedSha256_) != 0) {
        fail("sha256-mismatch", output);
        return;
    }

    // Invalidate derived data before publication. If publication fails, the
    // previous original remains valid and its disposable caches are rebuilt
    // on the next open; stale pagination can never be paired with new bytes.
    if (!invalidateDerivedFiles(bookId_)) {
        output.printf("ERROR BOOK_PUT reason=cache-invalidate-failed id=%s\n",
                      bookId_);
        SD.remove(partialPath_);
        reset();
        return;
    }
    if (!publishPartial(bookId_)) {
        fail("publish-failed", output);
        return;
    }

    output.printf("BOOK PUT COMPLETE id=%s bytes=%lu sha256=%s\n", bookId_,
                  static_cast<unsigned long>(receivedBytes_), actualSha256);
    reset();
}

void BookUploadReceiver::reset() {
    active_ = false;
    expectedBytes_ = 0;
    receivedBytes_ = 0;
    nextProgressBytes_ = 0;
    lastDataAt_ = 0;
    bookId_[0] = '\0';
    expectedSha256_[0] = '\0';
    directory_[0] = '\0';
    partialPath_[0] = '\0';
    finalPath_[0] = '\0';
}
