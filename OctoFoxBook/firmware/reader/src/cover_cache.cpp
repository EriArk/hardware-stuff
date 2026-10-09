#include "cover_cache.h"

#include <SD.h>
#include <esp_heap_caps.h>

#if ESP_IDF_VERSION_MAJOR >= 5
#include "esp32s3/rom/tjpgd.h"
#elif ESP_IDF_VERSION_MAJOR >= 4
#include "esp32/rom/tjpgd.h"
#else
#include "rom/tjpgd.h"
#endif

#include "book_upload.h"

namespace {

constexpr char kMagic[8] = {'A', 'B', 'Y', 'S', 'S', 'G', '4', '1'};
constexpr uint8_t kCacheVersion = 1;

struct __attribute__((packed)) CoverHeader {
    char magic[8];
    uint8_t version;
    uint8_t reserved;
    uint16_t width;
    uint16_t height;
    uint32_t payloadBytes;
};

struct DecodeContext {
    const uint8_t *source = nullptr;
    size_t sourceBytes = 0;
    size_t sourceOffset = 0;
    uint8_t *gray = nullptr;
    uint16_t width = 0;
    uint16_t height = 0;
};

void setError(CoverCacheInfo &info, const char *error) {
    snprintf(info.error, sizeof(info.error), "%s",
             error == nullptr ? "" : error);
}

bool cacheDirectory(const char *bookId, char *target, size_t capacity) {
    if (!BookUploadReceiver::validBookId(bookId) || target == nullptr ||
        capacity == 0) {
        return false;
    }
    const int written =
        snprintf(target, capacity, "/books/%s/cache", bookId);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

bool cachePartPath(const char *bookId, char *target, size_t capacity) {
    if (!BookUploadReceiver::validBookId(bookId) || target == nullptr ||
        capacity == 0) {
        return false;
    }
    const int written = snprintf(target, capacity,
                                 "/books/%s/cache/cover-g4-v1.bin.part",
                                 bookId);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

bool cacheOldPath(const char *bookId, char *target, size_t capacity) {
    if (!BookUploadReceiver::validBookId(bookId) || target == nullptr ||
        capacity == 0) {
        return false;
    }
    const int written = snprintf(target, capacity,
                                 "/books/%s/cache/cover-g4-v1.bin.old",
                                 bookId);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

#if ESP_IDF_VERSION_MAJOR >= 5
UINT feedJpeg(JDEC *decoder, BYTE *buffer, UINT bytes) {
#else
uint32_t feedJpeg(JDEC *decoder, uint8_t *buffer, uint32_t bytes) {
#endif
    auto *context = static_cast<DecodeContext *>(decoder->device);
    if (context == nullptr || context->sourceOffset >= context->sourceBytes) {
        return 0;
    }
    const size_t available = context->sourceBytes - context->sourceOffset;
    const size_t count = min(static_cast<size_t>(bytes), available);
    if (buffer != nullptr && count > 0) {
        memcpy(buffer, context->source + context->sourceOffset, count);
    }
    context->sourceOffset += count;
    return static_cast<decltype(bytes)>(count);
}

#if ESP_IDF_VERSION_MAJOR >= 5
UINT outputJpeg(JDEC *decoder, void *bitmap, JRECT *rectangle) {
#else
uint32_t outputJpeg(JDEC *decoder, void *bitmap, JRECT *rectangle) {
#endif
    auto *context = static_cast<DecodeContext *>(decoder->device);
    if (context == nullptr || context->gray == nullptr || bitmap == nullptr ||
        rectangle == nullptr) {
        return 0;
    }
    const uint16_t blockWidth =
        static_cast<uint16_t>(rectangle->right - rectangle->left + 1);
    const uint16_t blockHeight =
        static_cast<uint16_t>(rectangle->bottom - rectangle->top + 1);
    auto *rgb = static_cast<uint8_t *>(bitmap);
    for (uint16_t row = 0; row < blockHeight; ++row) {
        const uint16_t y = static_cast<uint16_t>(rectangle->top + row);
        for (uint16_t column = 0; column < blockWidth; ++column) {
            const uint16_t x =
                static_cast<uint16_t>(rectangle->left + column);
            const uint8_t red = *rgb++;
            const uint8_t green = *rgb++;
            const uint8_t blue = *rgb++;
            if (x >= context->width || y >= context->height) {
                continue;
            }
            context->gray[static_cast<size_t>(y) * context->width + x] =
                static_cast<uint8_t>((red * 38U + green * 75U + blue * 15U) >>
                                     7U);
        }
    }
    return 1;
}

bool decodeJpeg(const uint8_t *source, size_t sourceBytes, uint8_t *&gray,
                uint16_t &width, uint16_t &height) {
    gray = nullptr;
    width = 0;
    height = 0;
    auto *work = static_cast<uint8_t *>(heap_caps_malloc(
        4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (work == nullptr) {
        return false;
    }
    DecodeContext context{};
    context.source = source;
    context.sourceBytes = sourceBytes;
    JDEC decoder{};
    const JRESULT prepared =
        jd_prepare(&decoder, feedJpeg, work, 4096, &context);
    if (prepared != JDR_OK || decoder.width == 0 || decoder.height == 0) {
        heap_caps_free(work);
        return false;
    }

    uint8_t scale = 0;
    while (scale < 3 &&
           ((decoder.width >> scale) > 720 ||
            (decoder.height >> scale) > 960)) {
        ++scale;
    }
    context.width =
        static_cast<uint16_t>(max(1U, decoder.width >> scale));
    context.height =
        static_cast<uint16_t>(max(1U, decoder.height >> scale));
    const size_t decodedBytes =
        static_cast<size_t>(context.width) * context.height;
    if (decodedBytes > 1024U * 1024U) {
        heap_caps_free(work);
        return false;
    }
    context.gray = static_cast<uint8_t *>(heap_caps_malloc(
        decodedBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (context.gray == nullptr) {
        heap_caps_free(work);
        return false;
    }
    memset(context.gray, 0xFF, decodedBytes);
    const JRESULT decoded = jd_decomp(&decoder, outputJpeg, scale);
    heap_caps_free(work);
    if (decoded != JDR_OK) {
        heap_caps_free(context.gray);
        return false;
    }
    gray = context.gray;
    width = context.width;
    height = context.height;
    return true;
}

uint8_t bilinearGray(const uint8_t *source, uint16_t sourceWidth,
                     uint16_t sourceHeight, uint16_t targetX,
                     uint16_t targetY, uint16_t targetWidth,
                     uint16_t targetHeight) {
    if (targetWidth <= 1 || targetHeight <= 1 || sourceWidth <= 1 ||
        sourceHeight <= 1) {
        return source[0];
    }
    const uint32_t sourceX = static_cast<uint32_t>(targetX) *
                             (sourceWidth - 1U) * 256U /
                             (targetWidth - 1U);
    const uint32_t sourceY = static_cast<uint32_t>(targetY) *
                             (sourceHeight - 1U) * 256U /
                             (targetHeight - 1U);
    const uint16_t x0 = static_cast<uint16_t>(sourceX >> 8U);
    const uint16_t y0 = static_cast<uint16_t>(sourceY >> 8U);
    const uint16_t x1 = static_cast<uint16_t>(
        x0 + 1U < sourceWidth ? x0 + 1U : sourceWidth - 1U);
    const uint16_t y1 = static_cast<uint16_t>(
        y0 + 1U < sourceHeight ? y0 + 1U : sourceHeight - 1U);
    const uint16_t fx = static_cast<uint16_t>(sourceX & 0xFFU);
    const uint16_t fy = static_cast<uint16_t>(sourceY & 0xFFU);
    const uint32_t top =
        source[static_cast<size_t>(y0) * sourceWidth + x0] * (256U - fx) +
        source[static_cast<size_t>(y0) * sourceWidth + x1] * fx;
    const uint32_t bottom =
        source[static_cast<size_t>(y1) * sourceWidth + x0] * (256U - fx) +
        source[static_cast<size_t>(y1) * sourceWidth + x1] * fx;
    return static_cast<uint8_t>(
        ((top * (256U - fy) + bottom * fy) + 32768U) >> 16U);
}

bool publish(const char *partPath, const char *finalPath,
             const char *oldPath) {
    SD.remove(oldPath);
    const bool hadPrevious = SD.exists(finalPath);
    if (hadPrevious && !SD.rename(finalPath, oldPath)) {
        return false;
    }
    if (!SD.rename(partPath, finalPath)) {
        if (hadPrevious) {
            SD.rename(oldPath, finalPath);
        }
        return false;
    }
    if (hadPrevious) {
        SD.remove(oldPath);
    }
    return true;
}

}  // namespace

bool CoverCache::sourcePath(const char *bookId, char *target,
                            size_t capacity) {
    if (!BookUploadReceiver::validBookId(bookId) || target == nullptr ||
        capacity == 0) {
        return false;
    }
    const int written =
        snprintf(target, capacity, "/books/%s/cover.jpg", bookId);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

bool CoverCache::partialSourcePath(const char *bookId, char *target,
                                   size_t capacity) {
    if (!BookUploadReceiver::validBookId(bookId) || target == nullptr ||
        capacity == 0) {
        return false;
    }
    const int written =
        snprintf(target, capacity, "/books/%s/cover.jpg.part", bookId);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

bool CoverCache::derivedPath(const char *bookId, char *target,
                             size_t capacity) {
    if (!BookUploadReceiver::validBookId(bookId) || target == nullptr ||
        capacity == 0) {
        return false;
    }
    const int written = snprintf(target, capacity,
                                 "/books/%s/cache/cover-g4-v1.bin", bookId);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

bool CoverCache::publishSourcePartial(const char *bookId) {
    char partPath[96]{};
    char finalPath[96]{};
    char oldPath[96]{};
    char cachePath[112]{};
    char cachePart[112]{};
    char cacheOld[112]{};
    if (!partialSourcePath(bookId, partPath, sizeof(partPath)) ||
        !sourcePath(bookId, finalPath, sizeof(finalPath)) ||
        snprintf(oldPath, sizeof(oldPath), "/books/%s/cover.jpg.old",
                 bookId) <= 0 ||
        !derivedPath(bookId, cachePath, sizeof(cachePath)) ||
        !cachePartPath(bookId, cachePart, sizeof(cachePart)) ||
        !cacheOldPath(bookId, cacheOld, sizeof(cacheOld)) ||
        !SD.exists(partPath)) {
        return false;
    }
    if (!publish(partPath, finalPath, oldPath)) {
        return false;
    }
    SD.remove(cachePath);
    SD.remove(cachePart);
    SD.remove(cacheOld);
    return true;
}

bool CoverCache::sourcePresent(const char *bookId) {
    char path[96]{};
    if (!sourcePath(bookId, path, sizeof(path))) {
        return false;
    }
    File source = SD.open(path, FILE_READ);
    if (!source) {
        return false;
    }
    const size_t bytes = source.size();
    uint8_t signature[2]{};
    const bool read = source.read(signature, sizeof(signature)) ==
                      sizeof(signature);
    source.close();
    return read && signature[0] == 0xFF && signature[1] == 0xD8 &&
           bytes >= 32 && bytes <= kMaximumSourceBytes;
}

bool CoverCache::build(const char *bookId, CoverCacheInfo &info) {
    info = CoverCacheInfo{};
    char sourceFile[96]{};
    char cacheDirectoryPath[96]{};
    char finalPath[112]{};
    char partPath[112]{};
    char oldPath[112]{};
    if (!sourcePath(bookId, sourceFile, sizeof(sourceFile)) ||
        !cacheDirectory(bookId, cacheDirectoryPath,
                        sizeof(cacheDirectoryPath)) ||
        !derivedPath(bookId, finalPath, sizeof(finalPath)) ||
        !cachePartPath(bookId, partPath, sizeof(partPath)) ||
        !cacheOldPath(bookId, oldPath, sizeof(oldPath))) {
        setError(info, "invalid-path");
        return false;
    }
    CoverBitmap existing{};
    if (load(bookId, existing, info)) {
        release(existing);
        info.reused = true;
        return true;
    }
    info = CoverCacheInfo{};
    File input = SD.open(sourceFile, FILE_READ);
    if (!input) {
        setError(info, "cover-source-missing");
        return false;
    }
    const size_t sourceBytes = input.size();
    if (sourceBytes < 32 || sourceBytes > kMaximumSourceBytes) {
        input.close();
        setError(info, "cover-source-size-invalid");
        return false;
    }
    auto *source = static_cast<uint8_t *>(heap_caps_malloc(
        sourceBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (source == nullptr || input.read(source, sourceBytes) != sourceBytes) {
        input.close();
        heap_caps_free(source);
        setError(info, "cover-source-read-failed");
        return false;
    }
    input.close();
    if (source[0] != 0xFF || source[1] != 0xD8) {
        heap_caps_free(source);
        setError(info, "cover-source-not-jpeg");
        return false;
    }

    uint8_t *decoded = nullptr;
    uint16_t decodedWidth = 0;
    uint16_t decodedHeight = 0;
    const bool decodedOk = decodeJpeg(source, sourceBytes, decoded,
                                      decodedWidth, decodedHeight);
    heap_caps_free(source);
    if (!decodedOk) {
        setError(info, "cover-decode-failed");
        return false;
    }
    const float scale = min(static_cast<float>(kMaximumWidth) / decodedWidth,
                            static_cast<float>(kMaximumHeight) /
                                decodedHeight);
    const uint16_t targetWidth = static_cast<uint16_t>(
        max(1.0F, floorf(decodedWidth * min(1.0F, scale))));
    const uint16_t targetHeight = static_cast<uint16_t>(
        max(1.0F, floorf(decodedHeight * min(1.0F, scale))));
    const uint32_t pixelCount =
        static_cast<uint32_t>(targetWidth) * targetHeight;
    const uint32_t payloadBytes = (pixelCount + 1U) / 2U;
    auto *packed = static_cast<uint8_t *>(heap_caps_malloc(
        payloadBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (packed == nullptr) {
        heap_caps_free(decoded);
        setError(info, "cover-cache-allocation-failed");
        return false;
    }
    memset(packed, 0xFF, payloadBytes);
    for (uint16_t y = 0; y < targetHeight; ++y) {
        for (uint16_t x = 0; x < targetWidth; ++x) {
            const uint8_t gray = bilinearGray(
                decoded, decodedWidth, decodedHeight, x, y, targetWidth,
                targetHeight);
            const uint8_t shade = static_cast<uint8_t>(gray >> 4U);
            const uint32_t ordinal =
                static_cast<uint32_t>(y) * targetWidth + x;
            if ((ordinal & 1U) == 0) {
                packed[ordinal / 2U] = static_cast<uint8_t>(
                    (packed[ordinal / 2U] & 0xF0U) | shade);
            } else {
                packed[ordinal / 2U] = static_cast<uint8_t>(
                    (packed[ordinal / 2U] & 0x0FU) | (shade << 4U));
            }
        }
    }
    heap_caps_free(decoded);

    if (!BookUploadReceiver::ensureBookDirectory(bookId) ||
        (!SD.exists(cacheDirectoryPath) && !SD.mkdir(cacheDirectoryPath))) {
        heap_caps_free(packed);
        setError(info, "cover-cache-mkdir-failed");
        return false;
    }
    SD.remove(partPath);
    File output = SD.open(partPath, FILE_WRITE);
    CoverHeader header{};
    memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kCacheVersion;
    header.width = targetWidth;
    header.height = targetHeight;
    header.payloadBytes = payloadBytes;
    const bool written = output &&
                         output.write(reinterpret_cast<const uint8_t *>(&header),
                                      sizeof(header)) == sizeof(header) &&
                         output.write(packed, payloadBytes) == payloadBytes;
    if (output) {
        output.flush();
        output.close();
    }
    heap_caps_free(packed);
    if (!written || !publish(partPath, finalPath, oldPath)) {
        SD.remove(partPath);
        setError(info, "cover-cache-publish-failed");
        return false;
    }
    info.ok = true;
    info.width = targetWidth;
    info.height = targetHeight;
    setError(info, "");
    return true;
}

bool CoverCache::load(const char *bookId, CoverBitmap &bitmap,
                      CoverCacheInfo &info) {
    release(bitmap);
    info = CoverCacheInfo{};
    char path[112]{};
    if (!derivedPath(bookId, path, sizeof(path))) {
        setError(info, "invalid-path");
        return false;
    }
    File input = SD.open(path, FILE_READ);
    if (!input) {
        setError(info, "cover-cache-missing");
        return false;
    }
    CoverHeader header{};
    if (input.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) !=
            sizeof(header) ||
        memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 ||
        header.version != kCacheVersion || header.width == 0 ||
        header.height == 0 || header.width > kMaximumWidth ||
        header.height > kMaximumHeight ||
        header.payloadBytes !=
            (static_cast<uint32_t>(header.width) * header.height + 1U) /
                2U ||
        input.size() != sizeof(header) + header.payloadBytes) {
        input.close();
        setError(info, "cover-cache-invalid");
        return false;
    }
    bitmap.pixels = static_cast<uint8_t *>(heap_caps_malloc(
        header.payloadBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (bitmap.pixels == nullptr ||
        input.read(bitmap.pixels, header.payloadBytes) !=
            header.payloadBytes) {
        input.close();
        release(bitmap);
        setError(info, "cover-cache-read-failed");
        return false;
    }
    input.close();
    bitmap.width = header.width;
    bitmap.height = header.height;
    info.ok = true;
    info.reused = true;
    info.width = header.width;
    info.height = header.height;
    setError(info, "");
    return true;
}

void CoverCache::release(CoverBitmap &bitmap) {
    heap_caps_free(bitmap.pixels);
    bitmap = CoverBitmap{};
}
