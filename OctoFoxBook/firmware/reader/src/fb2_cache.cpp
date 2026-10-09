#include "fb2_cache.h"
#include "work_progress.h"

#include <ArduinoJson.h>
#include <SD.h>

#include <esp_heap_caps.h>

#include <ctype.h>
#include <string.h>

#include "book_upload.h"

namespace {

constexpr char kCacheSignature[] = "ABYSS_FB2_TEXT\t1";
constexpr size_t kReadBufferBytes = 4096;
constexpr size_t kWriteBufferBytes = 32768;
constexpr size_t kMaximumTagBytes = 384;
constexpr size_t kMaximumEntityBytes = 16;
constexpr size_t kMaximumRecordBytes = 16384;
constexpr uint32_t kDecoderVersion = 4;

void setError(Fb2CacheInfo &info, const char *message) {
    snprintf(info.error, sizeof(info.error), "%s", message);
}

bool ensureDirectory(const char *path) {
    return SD.exists(path) || SD.mkdir(path);
}

void appendUtf8(String &target, uint32_t codePoint) {
    if (codePoint <= 0x7F) {
        target += static_cast<char>(codePoint);
    } else if (codePoint <= 0x7FF) {
        target += static_cast<char>(0xC0 | (codePoint >> 6));
        target += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint <= 0xFFFF) {
        target += static_cast<char>(0xE0 | (codePoint >> 12));
        target += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        target += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint <= 0x10FFFF) {
        target += static_cast<char>(0xF0 | (codePoint >> 18));
        target += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        target += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        target += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

uint32_t windows1251CodePoint(uint8_t value) {
    if (value < 0x80) {
        return value;
    }
    if (value >= 0xC0) {
        return value < 0xE0 ? 0x0410U + (value - 0xC0U)
                            : 0x0430U + (value - 0xE0U);
    }
    constexpr uint16_t kWindows1251Table[64] = {
        0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
        0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
        0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
        0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
        0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
        0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
        0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    };
    return kWindows1251Table[value - 0x80U];
}

bool sourceUsesWindows1251(File &source) {
    char declaration[193]{};
    source.seek(0);
    const size_t count = source.read(
        reinterpret_cast<uint8_t *>(declaration), sizeof(declaration) - 1);
    declaration[count] = '\0';
    source.seek(0);
    for (size_t index = 0; index < count; ++index) {
        if (declaration[index] >= 'A' && declaration[index] <= 'Z') {
            declaration[index] = static_cast<char>(
                declaration[index] - ('A' - 'a'));
        }
    }
    return strstr(declaration, "windows-1251") != nullptr ||
           strstr(declaration, "windows1251") != nullptr ||
           strstr(declaration, "cp1251") != nullptr;
}

bool validUtf8(const char *value) {
    if (value == nullptr) {
        return false;
    }
    const auto *cursor = reinterpret_cast<const uint8_t *>(value);
    while (*cursor != 0) {
        if (*cursor <= 0x7F) {
            ++cursor;
            continue;
        }
        uint8_t continuation = 0;
        uint32_t codePoint = 0;
        if ((*cursor & 0xE0) == 0xC0) {
            continuation = 1;
            codePoint = *cursor & 0x1F;
        } else if ((*cursor & 0xF0) == 0xE0) {
            continuation = 2;
            codePoint = *cursor & 0x0F;
        } else if ((*cursor & 0xF8) == 0xF0) {
            continuation = 3;
            codePoint = *cursor & 0x07;
        } else {
            return false;
        }
        ++cursor;
        for (uint8_t index = 0; index < continuation; ++index, ++cursor) {
            if ((*cursor & 0xC0) != 0x80) {
                return false;
            }
            codePoint = (codePoint << 6) | (*cursor & 0x3F);
        }
        const uint32_t minimum = continuation == 1 ? 0x80U
                                  : continuation == 2 ? 0x800U
                                                      : 0x10000U;
        if (codePoint < minimum || codePoint > 0x10FFFFU ||
            (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
            return false;
        }
    }
    return true;
}

void invalidateWindows1251Pagination(const char *bookId) {
    constexpr const char *kArtifacts[] = {
        "cache/pages-v1.txt",       "cache/pages-v1.txt.part",
        "cache/pages-v1.txt.old",   "cache/pages-v1.idx",
        "cache/pages-v1.idx.part",  "cache/pages-v1.idx.old",
        "cache/chapters-v1.txt",    "cache/chapters-v1.txt.part",
        "cache/chapters-v1.txt.old", "cache/chapters-v2.txt",
        "cache/chapters-v2.txt.part", "cache/chapters-v2.txt.old",
    };
    char path[112]{};
    for (const char *artifact : kArtifacts) {
        snprintf(path, sizeof(path), "/books/%s/%s", bookId, artifact);
        if (SD.exists(path)) {
            SD.remove(path);
        }
    }
}

class Fb2StreamParser {
public:
    Fb2StreamParser(File &output, Fb2CacheInfo &info, bool windows1251)
        : output_(output), info_(info), windows1251_(windows1251) {
        tag_.reserve(kMaximumTagBytes);
        entity_.reserve(kMaximumEntityBytes);
        record_.reserve(2048);
        metaValue_.reserve(256);
        annotationValue_.reserve(1536);
    }

    bool feed(uint8_t value) {
        if (windows1251_ && value >= 0x80) {
            String encoded;
            appendUtf8(encoded, windows1251CodePoint(value));
            for (size_t index = 0; index < encoded.length(); ++index) {
                if (!feedDecoded(static_cast<uint8_t>(encoded[index]))) {
                    return false;
                }
            }
            return true;
        }
        return feedDecoded(value);
    }

    bool complete() const {
        return complete_;
    }

private:
    bool feedDecoded(uint8_t value) {
        if (complete_) {
            return true;
        }
        if (inTag_) {
            if (value == '>') {
                inTag_ = false;
                if (!processTag()) {
                    return false;
                }
                tag_ = "";
            } else if (tag_.length() < kMaximumTagBytes) {
                tag_ += static_cast<char>(value);
            } else {
                setError(info_, "xml-tag-too-large");
                return false;
            }
            return true;
        }

        if (inEntity_) {
            if (value == ';') {
                inEntity_ = false;
                decodeEntity();
                entity_ = "";
            } else if (entity_.length() < kMaximumEntityBytes) {
                entity_ += static_cast<char>(value);
            } else {
                appendText('&');
                for (size_t index = 0; index < entity_.length(); ++index) {
                    appendText(static_cast<uint8_t>(entity_[index]));
                }
                appendText(value);
                inEntity_ = false;
                entity_ = "";
            }
            return true;
        }

        if (value == '<') {
            inTag_ = true;
            tag_ = "";
        } else if (value == '&') {
            inEntity_ = true;
            entity_ = "";
        } else {
            appendText(value);
        }
        return info_.error[0] == '\0';
    }
    enum class MetaField : uint8_t {
        None,
        BookTitle,
        Genre,
        FirstName,
        MiddleName,
        LastName,
    };

    bool processTag() {
        String raw = tag_;
        raw.trim();
        if (raw.isEmpty() || raw[0] == '?' || raw[0] == '!') {
            return true;
        }

        bool closing = raw[0] == '/';
        size_t start = closing ? 1 : 0;
        while (start < raw.length() && isspace(raw[start])) {
            ++start;
        }
        size_t end = start;
        while (end < raw.length() && !isspace(raw[end]) && raw[end] != '/') {
            ++end;
        }
        String name = raw.substring(start, end);
        const int colon = name.lastIndexOf(':');
        if (colon >= 0) {
            name = name.substring(colon + 1);
        }
        name.toLowerCase();
        const bool selfClosing = !closing && raw.endsWith("/");

        if (closing) {
            closeTag(name);
        } else {
            if (inTitleInfo_ && name == "sequence" &&
                info_.series[0] == '\0') {
                captureSequence(raw);
            }
            openTag(name);
            if (selfClosing) {
                closeTag(name);
            }
        }
        return info_.error[0] == '\0';
    }

    void openTag(const String &name) {
        if (name == "title-info") {
            inTitleInfo_ = true;
            return;
        }
        if (inTitleInfo_ && name == "author" && !authorCaptured_) {
            inAuthor_ = true;
            firstName_ = "";
            middleName_ = "";
            lastName_ = "";
            return;
        }
        if (inTitleInfo_ && name == "book-title" && info_.title[0] == '\0') {
            beginMeta(MetaField::BookTitle);
            return;
        }
        if (inTitleInfo_ && name == "genre" && info_.genre[0] == '\0') {
            beginMeta(MetaField::Genre);
            return;
        }
        if (inTitleInfo_ && name == "annotation" &&
            info_.annotation[0] == '\0') {
            inAnnotation_ = true;
            annotationValue_ = "";
            annotationSpacePending_ = false;
            return;
        }
        if (inAuthor_ && name == "first-name") {
            beginMeta(MetaField::FirstName);
            return;
        }
        if (inAuthor_ && name == "middle-name") {
            beginMeta(MetaField::MiddleName);
            return;
        }
        if (inAuthor_ && name == "last-name") {
            beginMeta(MetaField::LastName);
            return;
        }

        if (name == "body" && !bodySeen_) {
            bodySeen_ = true;
            inBody_ = true;
            writeHeader();
            return;
        }
        if (!inBody_) {
            return;
        }
        if (name == "section") {
            ++sectionDepth_;
            writeRecord('S', String(sectionDepth_));
        } else if (name == "title") {
            ++titleDepth_;
        } else if (name == "p") {
            beginRecord(titleDepth_ > 0 ? 'H' : 'P');
        } else if (name == "subtitle") {
            beginRecord('H');
        } else if (name == "text-author") {
            beginRecord('P');
        } else if (name == "empty-line") {
            writeRecord('E', "");
        } else if (name == "image") {
            writeRecord('I', "[Изображение]");
        }
    }

    void closeTag(const String &name) {
        if (metaField_ != MetaField::None) {
            const bool matching =
                (metaField_ == MetaField::BookTitle && name == "book-title") ||
                (metaField_ == MetaField::Genre && name == "genre") ||
                (metaField_ == MetaField::FirstName && name == "first-name") ||
                (metaField_ == MetaField::MiddleName && name == "middle-name") ||
                (metaField_ == MetaField::LastName && name == "last-name");
            if (matching) {
                finishMeta();
            }
        }
        if (name == "annotation" && inAnnotation_) {
            annotationValue_.trim();
            snprintf(info_.annotation, sizeof(info_.annotation), "%s",
                     annotationValue_.c_str());
            inAnnotation_ = false;
        } else if (inAnnotation_ &&
                   (name == "p" || name == "subtitle" ||
                    name == "empty-line")) {
            annotationSpacePending_ = !annotationValue_.isEmpty();
        }
        if (name == "author" && inAuthor_) {
            String author = firstName_;
            if (!middleName_.isEmpty()) {
                if (!author.isEmpty()) {
                    author += ' ';
                }
                author += middleName_;
            }
            if (!lastName_.isEmpty()) {
                if (!author.isEmpty()) {
                    author += ' ';
                }
                author += lastName_;
            }
            snprintf(info_.author, sizeof(info_.author), "%s", author.c_str());
            authorCaptured_ = true;
            inAuthor_ = false;
        } else if (name == "title-info") {
            inTitleInfo_ = false;
            inAuthor_ = false;
        }

        if (!inBody_) {
            return;
        }
        if ((name == "p" || name == "subtitle" || name == "text-author") &&
            recordType_ != '\0') {
            finishRecord();
        } else if (name == "title" && titleDepth_ > 0) {
            --titleDepth_;
        } else if (name == "section" && sectionDepth_ > 0) {
            --sectionDepth_;
        } else if (name == "body") {
            if (recordType_ != '\0') {
                finishRecord();
            }
            inBody_ = false;
            complete_ = true;
        }
    }

    void beginMeta(MetaField field) {
        metaField_ = field;
        metaValue_ = "";
        metaSpacePending_ = false;
    }

    void finishMeta() {
        switch (metaField_) {
        case MetaField::BookTitle:
            snprintf(info_.title, sizeof(info_.title), "%s", metaValue_.c_str());
            break;
        case MetaField::Genre:
            snprintf(info_.genre, sizeof(info_.genre), "%s",
                     metaValue_.c_str());
            break;
        case MetaField::FirstName:
            firstName_ = metaValue_;
            break;
        case MetaField::MiddleName:
            middleName_ = metaValue_;
            break;
        case MetaField::LastName:
            lastName_ = metaValue_;
            break;
        case MetaField::None:
            break;
        }
        metaField_ = MetaField::None;
        metaValue_ = "";
        metaSpacePending_ = false;
    }

    void beginRecord(char type) {
        if (recordType_ != '\0') {
            finishRecord();
        }
        recordType_ = type;
        record_ = "";
        recordSpacePending_ = false;
    }

    void finishRecord() {
        record_.trim();
        if (!record_.isEmpty()) {
            writeRecord(recordType_, record_);
        }
        recordType_ = '\0';
        record_ = "";
        recordSpacePending_ = false;
    }

    void writeHeader() {
        output_.println(kCacheSignature);
        output_.print("TITLE\t");
        output_.println(info_.title);
        output_.print("AUTHOR\t");
        output_.println(info_.author);
    }

    void writeRecord(char type, const String &value) {
        output_.write(static_cast<uint8_t>(type));
        output_.write(static_cast<uint8_t>('\t'));
        output_.println(value);
        ++info_.records;
    }

    void appendNormalized(String &target, bool &spacePending, uint8_t value) {
        if (value <= 0x20 || value == 0x7F) {
            if (!target.isEmpty()) {
                spacePending = true;
            }
            return;
        }
        if (spacePending && !target.isEmpty()) {
            target += ' ';
        }
        spacePending = false;
        target += static_cast<char>(value);
    }

    void appendText(uint8_t value) {
        if (metaField_ != MetaField::None) {
            appendNormalized(metaValue_, metaSpacePending_, value);
        }
        if (recordType_ != '\0') {
            if (record_.length() >= kMaximumRecordBytes) {
                setError(info_, "paragraph-too-large");
                return;
            }
            appendNormalized(record_, recordSpacePending_, value);
        }
        if (inAnnotation_ && annotationValue_.length() <
                                 sizeof(info_.annotation) - 1) {
            appendNormalized(annotationValue_, annotationSpacePending_, value);
        }
    }

    bool attributeValue(const String &raw, const char *wanted,
                        String &target) {
        size_t cursor = 0;
        while (cursor < raw.length()) {
            while (cursor < raw.length() && isspace(raw[cursor])) {
                ++cursor;
            }
            const size_t nameStart = cursor;
            while (cursor < raw.length() && raw[cursor] != '=' &&
                   !isspace(raw[cursor]) && raw[cursor] != '/') {
                ++cursor;
            }
            const String name = raw.substring(nameStart, cursor);
            while (cursor < raw.length() && isspace(raw[cursor])) {
                ++cursor;
            }
            if (cursor >= raw.length() || raw[cursor] != '=') {
                while (cursor < raw.length() && !isspace(raw[cursor])) {
                    ++cursor;
                }
                continue;
            }
            ++cursor;
            while (cursor < raw.length() && isspace(raw[cursor])) {
                ++cursor;
            }
            if (cursor >= raw.length() ||
                (raw[cursor] != '\'' && raw[cursor] != '"')) {
                continue;
            }
            const char quote = raw[cursor++];
            const size_t valueStart = cursor;
            while (cursor < raw.length() && raw[cursor] != quote) {
                ++cursor;
            }
            if (name.equalsIgnoreCase(wanted)) {
                target = raw.substring(valueStart, cursor);
                return true;
            }
            if (cursor < raw.length()) {
                ++cursor;
            }
        }
        return false;
    }

    void captureSequence(const String &raw) {
        String name;
        String number;
        if (!attributeValue(raw, "name", name) || name.isEmpty()) {
            return;
        }
        attributeValue(raw, "number", number);
        snprintf(info_.series, sizeof(info_.series), "%s", name.c_str());
        snprintf(info_.seriesNumber, sizeof(info_.seriesNumber), "%s",
                 number.c_str());
    }

    void appendCodePoint(uint32_t codePoint) {
        if (codePoint == 0xA0) {
            appendText(' ');
            return;
        }
        String encoded;
        appendUtf8(encoded, codePoint);
        for (size_t index = 0; index < encoded.length(); ++index) {
            appendText(static_cast<uint8_t>(encoded[index]));
        }
    }

    void decodeEntity() {
        if (entity_ == "amp") {
            appendText('&');
        } else if (entity_ == "lt") {
            appendText('<');
        } else if (entity_ == "gt") {
            appendText('>');
        } else if (entity_ == "quot") {
            appendText('"');
        } else if (entity_ == "apos") {
            appendText('\'');
        } else if (entity_.startsWith("#x") || entity_.startsWith("#X")) {
            appendCodePoint(strtoul(entity_.c_str() + 2, nullptr, 16));
        } else if (entity_.startsWith("#")) {
            appendCodePoint(strtoul(entity_.c_str() + 1, nullptr, 10));
        } else {
            appendText('&');
            for (size_t index = 0; index < entity_.length(); ++index) {
                appendText(static_cast<uint8_t>(entity_[index]));
            }
            appendText(';');
        }
    }

    File &output_;
    Fb2CacheInfo &info_;
    String tag_;
    String entity_;
    String record_;
    String metaValue_;
    String annotationValue_;
    String firstName_;
    String middleName_;
    String lastName_;
    bool inTag_ = false;
    bool inEntity_ = false;
    bool inTitleInfo_ = false;
    bool inAuthor_ = false;
    bool inAnnotation_ = false;
    bool authorCaptured_ = false;
    bool bodySeen_ = false;
    bool inBody_ = false;
    bool complete_ = false;
    bool recordSpacePending_ = false;
    bool metaSpacePending_ = false;
    bool annotationSpacePending_ = false;
    bool windows1251_ = false;
    uint16_t titleDepth_ = 0;
    uint16_t sectionDepth_ = 0;
    char recordType_ = '\0';
    MetaField metaField_ = MetaField::None;
};

bool publishFile(const char *partialPath, const char *finalPath,
                 const char *oldPath) {
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
        SD.remove(oldPath);
    }
    return true;
}

bool writeMetadata(const char *bookId, const Fb2CacheInfo &info,
                   const char *bookDirectory, bool windows1251) {
    char partialPath[96]{};
    char finalPath[96]{};
    char oldPath[96]{};
    snprintf(partialPath, sizeof(partialPath), "%s/metadata.json.part",
             bookDirectory);
    snprintf(finalPath, sizeof(finalPath), "%s/metadata.json", bookDirectory);
    snprintf(oldPath, sizeof(oldPath), "%s/metadata.json.old", bookDirectory);
    SD.remove(partialPath);

    File output = SD.open(partialPath, FILE_WRITE);
    if (!output) {
        return false;
    }
    JsonDocument document;
    document["schema"] = "abyss-reader-book";
    document["version"] = 1;
    document["id"] = bookId;
    document["format"] = "fb2";
    document["title"] = info.title;
    JsonArray authors = document["authors"].to<JsonArray>();
    authors.add(info.author);
    document["annotation"] = info.annotation;
    JsonObject series = document["series"].to<JsonObject>();
    series["name"] = info.series;
    series["number"] = info.seriesNumber;
    JsonArray genres = document["genres"].to<JsonArray>();
    if (info.genre[0] != '\0') {
        genres.add(info.genre);
    }
    document["source_bytes"] = info.sourceBytes;
    document["cache_version"] = 1;
    document["decoder_version"] = kDecoderVersion;
    document["source_encoding"] =
        windows1251 ? "windows-1251" : "utf-8";
    document["cache_records"] = info.records;
    const bool written = serializeJsonPretty(document, output) > 0;
    output.flush();
    output.close();
    if (!written) {
        SD.remove(partialPath);
        return false;
    }
    return publishFile(partialPath, finalPath, oldPath);
}

}  // namespace

bool Fb2Cache::contentPath(const char *bookId, char *target, size_t capacity) {
    if (!BookUploadReceiver::validBookId(bookId) || target == nullptr ||
        capacity == 0) {
        return false;
    }
    const int written = snprintf(target, capacity,
                                 "/books/%s/cache/content-v1.txt", bookId);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

bool Fb2Cache::load(const char *bookId, Fb2CacheInfo &info) {
    // Fb2CacheInfo contains a full annotation. Reset it in place instead of
    // materializing a multi-kilobyte aggregate on the Arduino loop stack.
    memset(&info, 0, sizeof(info));
    if (!BookUploadReceiver::validBookId(bookId)) {
        setError(info, "invalid-book-id");
        return false;
    }

    char sourcePath[96]{};
    char cachedPath[96]{};
    char metadataPath[96]{};
    if (!BookUploadReceiver::bookPath(bookId, sourcePath,
                                      sizeof(sourcePath)) ||
        !contentPath(bookId, cachedPath, sizeof(cachedPath)) ||
        snprintf(metadataPath, sizeof(metadataPath),
                 "/books/%s/metadata.json", bookId) <= 0) {
        setError(info, "path-failed");
        return false;
    }

    File source = SD.open(sourcePath, FILE_READ);
    File cached = SD.open(cachedPath, FILE_READ);
    File metadata = SD.open(metadataPath, FILE_READ);
    if (!source || !cached || !metadata) {
        source.close();
        cached.close();
        metadata.close();
        setError(info, "cache-not-found");
        return false;
    }
    const uint32_t sourceBytes = static_cast<uint32_t>(source.size());
    const bool windows1251 = sourceUsesWindows1251(source);
    source.close();

    String signature = cached.readStringUntil('\n');
    signature.trim();
    String cachedTitle = cached.readStringUntil('\n');
    String cachedAuthor = cached.readStringUntil('\n');
    cachedTitle.trim();
    cachedAuthor.trim();
    cached.close();
    if (signature != kCacheSignature || !cachedTitle.startsWith("TITLE\t") ||
        !cachedAuthor.startsWith("AUTHOR\t")) {
        metadata.close();
        setError(info, "cache-invalid");
        return false;
    }

    JsonDocument document;
    const DeserializationError jsonError =
        deserializeJson(document, metadata);
    metadata.close();
    const char *schema = document["schema"].as<const char *>();
    const char *metadataBookId = document["id"].as<const char *>();
    const char *format = document["format"].as<const char *>();
    const char *title = document["title"].as<const char *>();
    const char *author = document["authors"][0].as<const char *>();
    const char *annotation = document["annotation"] | "";
    const char *series = document["series"]["name"] | "";
    const char *seriesNumber = document["series"]["number"] | "";
    const char *genre = document["genres"][0] | "";
    const uint32_t version = document["version"] | 0U;
    const uint32_t cacheVersion = document["cache_version"] | 0U;
    const uint32_t decoderVersion = document["decoder_version"] | 0U;
    const char *sourceEncoding =
        document["source_encoding"].as<const char *>();
    const uint32_t metadataSourceBytes = document["source_bytes"] | 0U;
    if (jsonError || schema == nullptr || metadataBookId == nullptr ||
        format == nullptr || title == nullptr || author == nullptr ||
        strcmp(schema, "abyss-reader-book") != 0 || version != 1 ||
        strcmp(metadataBookId, bookId) != 0 || strcmp(format, "fb2") != 0 ||
        cacheVersion != 1 || metadataSourceBytes != sourceBytes ||
        !validUtf8(title) || !validUtf8(author) ||
        (windows1251 &&
         (decoderVersion < kDecoderVersion || sourceEncoding == nullptr ||
          strcmp(sourceEncoding, "windows-1251") != 0))) {
        setError(info, "metadata-invalid");
        return false;
    }

    cachedTitle.remove(0, strlen("TITLE\t"));
    cachedAuthor.remove(0, strlen("AUTHOR\t"));
    if (cachedTitle != title || cachedAuthor != author) {
        setError(info, "cache-metadata-mismatch");
        return false;
    }

    info.ok = true;
    info.detailsReady = decoderVersion >= kDecoderVersion;
    info.sourceBytes = sourceBytes;
    info.records = document["cache_records"] | 0U;
    snprintf(info.title, sizeof(info.title), "%s", title);
    snprintf(info.author, sizeof(info.author), "%s", author);
    snprintf(info.annotation, sizeof(info.annotation), "%s", annotation);
    snprintf(info.series, sizeof(info.series), "%s", series);
    snprintf(info.seriesNumber, sizeof(info.seriesNumber), "%s",
             seriesNumber);
    snprintf(info.genre, sizeof(info.genre), "%s", genre);
    return true;
}

bool Fb2Cache::build(const char *bookId, Fb2CacheInfo &info) {
    memset(&info, 0, sizeof(info));
    if (!BookUploadReceiver::validBookId(bookId)) {
        setError(info, "invalid-book-id");
        return false;
    }

    char sourcePath[96]{};
    if (!BookUploadReceiver::bookPath(bookId, sourcePath, sizeof(sourcePath))) {
        setError(info, "source-path-failed");
        return false;
    }
    File source = SD.open(sourcePath, FILE_READ);
    if (!source) {
        setError(info, "book-not-found");
        return false;
    }
    info.sourceBytes = static_cast<uint32_t>(source.size());
    const bool windows1251 = sourceUsesWindows1251(source);

    char bookDirectory[64]{};
    char cacheDirectory[80]{};
    char finalPath[96]{};
    char partialPath[104]{};
    char oldPath[104]{};
    snprintf(bookDirectory, sizeof(bookDirectory), "/books/%s", bookId);
    snprintf(cacheDirectory, sizeof(cacheDirectory), "%s/cache", bookDirectory);
    contentPath(bookId, finalPath, sizeof(finalPath));
    snprintf(partialPath, sizeof(partialPath), "%s.part", finalPath);
    snprintf(oldPath, sizeof(oldPath), "%s.old", finalPath);
    if (!ensureDirectory(cacheDirectory)) {
        source.close();
        setError(info, "cache-mkdir-failed");
        return false;
    }
    SD.remove(partialPath);
    File output = SD.open(partialPath, FILE_WRITE);
    if (!output) {
        source.close();
        setError(info, "cache-open-failed");
        return false;
    }
    // The normalized cache is emitted as thousands of small XML-derived
    // records. stdio buffering turns those writes into FAT32-sized batches;
    // publication remains atomic because flush/close still precede rename.
    output.setBufferSize(kWriteBufferBytes);

    Fb2StreamParser parser(output, info, windows1251);
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(
        kReadBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        output.close();
        source.close();
        SD.remove(partialPath);
        setError(info, "read-buffer-allocation-failed");
        return false;
    }
    bool passed = true;
    while (source.available() && !parser.complete()) {
        reportWorkProgress();
        if (workCancelled()) { passed = false; setError(info, "cancelled"); break; }
        const size_t count = source.read(buffer, kReadBufferBytes);
        if (count == 0) {
            break;
        }
        for (size_t index = 0; index < count; ++index) {
            if (!parser.feed(buffer[index])) {
                passed = false;
                break;
            }
        }
        if (!passed) {
            break;
        }
    }
    heap_caps_free(buffer);
    source.close();
    output.flush();
    output.close();

    if (!passed || !parser.complete() || info.title[0] == '\0' ||
        info.records == 0) {
        SD.remove(partialPath);
        if (info.error[0] == '\0') {
            setError(info, "incomplete-main-body");
        }
        return false;
    }
    if (!publishFile(partialPath, finalPath, oldPath)) {
        SD.remove(partialPath);
        setError(info, "cache-publish-failed");
        return false;
    }
    if (!writeMetadata(bookId, info, bookDirectory, windows1251)) {
        setError(info, "metadata-publish-failed");
        return false;
    }
    if (windows1251) {
        invalidateWindows1251Pagination(bookId);
    }
    info.ok = true;
    info.detailsReady = true;
    return true;
}
