#include "recent_books.h"

#include <ArduinoJson.h>
#include <SD.h>

#include <string.h>

#include "book_upload.h"

namespace {

constexpr char kDirectory[] = "/reader";
constexpr char kPath[] = "/reader/recent-v1.json";
constexpr char kPartialPath[] = "/reader/recent-v1.json.part";
constexpr char kOldPath[] = "/reader/recent-v1.json.old";

void setError(RecentBookList &list, const char *message) {
    snprintf(list.error, sizeof(list.error), "%s", message);
}

bool publishFile() {
    SD.remove(kOldPath);
    const bool hadPublished = SD.exists(kPath);
    if (hadPublished && !SD.rename(kPath, kOldPath)) {
        return false;
    }
    if (!SD.rename(kPartialPath, kPath)) {
        if (hadPublished) {
            SD.rename(kOldPath, kPath);
        }
        return false;
    }
    SD.remove(kOldPath);
    return true;
}

bool writeList(RecentBookList &list) {
    if (!SD.exists(kDirectory) && !SD.mkdir(kDirectory)) {
        setError(list, "directory-create-failed");
        return false;
    }
    SD.remove(kPartialPath);
    File output = SD.open(kPartialPath, FILE_WRITE);
    if (!output) {
        setError(list, "state-open-failed");
        return false;
    }
    JsonDocument document;
    document["schema"] = "abyss-reader-recent";
    document["version"] = 1;
    JsonArray books = document["books"].to<JsonArray>();
    for (size_t index = 0; index < list.count; ++index) {
        books.add(list.ids[index]);
    }
    const bool written = serializeJsonPretty(document, output) > 0;
    output.flush();
    output.close();
    if (!written || !publishFile()) {
        SD.remove(kPartialPath);
        setError(list, "state-publish-failed");
        return false;
    }
    list.ok = true;
    return true;
}

}  // namespace

bool RecentBooks::load(RecentBookList &list) {
    list = RecentBookList{};
    File input = SD.open(kPath, FILE_READ);
    if (!input) {
        list.ok = true;
        return true;
    }

    JsonDocument document;
    const DeserializationError jsonError = deserializeJson(document, input);
    input.close();
    const char *schema = document["schema"].as<const char *>();
    const uint32_t version = document["version"] | 0U;
    JsonArray books = document["books"].as<JsonArray>();
    if (jsonError || schema == nullptr ||
        strcmp(schema, "abyss-reader-recent") != 0 || version != 1 ||
        books.isNull()) {
        setError(list, "invalid-state");
        return false;
    }

    for (JsonVariant value : books) {
        const char *bookId = value.as<const char *>();
        if (bookId == nullptr || !BookUploadReceiver::validBookId(bookId)) {
            continue;
        }
        bool duplicate = false;
        for (size_t index = 0; index < list.count; ++index) {
            duplicate = duplicate || strcmp(list.ids[index], bookId) == 0;
        }
        if (!duplicate && list.count < kRecentBookCapacity) {
            snprintf(list.ids[list.count++], 33, "%s", bookId);
        }
    }
    list.ok = true;
    return true;
}

bool RecentBooks::touch(const char *bookId, RecentBookList &list) {
    if (!BookUploadReceiver::validBookId(bookId)) {
        list = RecentBookList{};
        setError(list, "invalid-book-id");
        return false;
    }

    RecentBookList previous{};
    if (!load(previous)) {
        previous = RecentBookList{};
    }
    list = RecentBookList{};
    snprintf(list.ids[list.count++], 33, "%s", bookId);
    for (size_t index = 0;
         index < previous.count && list.count < kRecentBookCapacity; ++index) {
        if (strcmp(previous.ids[index], bookId) != 0) {
            snprintf(list.ids[list.count++], 33, "%s", previous.ids[index]);
        }
    }
    return writeList(list);
}
