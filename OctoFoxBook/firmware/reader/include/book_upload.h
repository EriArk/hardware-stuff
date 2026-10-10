#pragma once

#include <Arduino.h>
#include <SD.h>

#include <mbedtls/sha256.h>

class BookUploadReceiver {
public:
    static constexpr uint32_t kMinimumBookBytes = 256;
    static constexpr uint32_t kMaximumBookBytes = 64U * 1024U * 1024U;

    BookUploadReceiver();
    ~BookUploadReceiver();

    bool start(const char *bookId, uint32_t expectedBytes,
               const char *expectedSha256, Print &output);
    void poll(Stream &input, Print &output);
    bool active() const;
    void cancel(Print &output) { if(active_) fail("cancelled-low-battery",output); }

    static bool validBookId(const char *bookId);
    static bool bookPath(const char *bookId, char *target, size_t capacity);
    static bool partialBookPath(const char *bookId, char *target,
                                size_t capacity);
    static bool ensureBookDirectory(const char *bookId);
    static bool invalidateDerivedFiles(const char *bookId);
    static bool publishPartial(const char *bookId);
    // Recoverable removal: move the whole book out of /books, never overwrite.
    static bool archiveBook(const char *bookId);

private:
    void fail(const char *reason, Print &output);
    void finish(Print &output);
    void reset();

    bool active_ = false;
    bool shaInitialized_ = false;
    File output_;
    mbedtls_sha256_context sha_{};
    uint32_t expectedBytes_ = 0;
    uint32_t receivedBytes_ = 0;
    uint32_t nextProgressBytes_ = 0;
    uint32_t lastDataAt_ = 0;
    char bookId_[33]{};
    char expectedSha256_[65]{};
    char directory_[64]{};
    char partialPath_[96]{};
    char finalPath_[96]{};
};
