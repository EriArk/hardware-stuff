#include "favorites_store.h"
#include "storage_recovery.h"

#include <ArduinoJson.h>
#include <SD.h>

#include <string.h>

#include "book_upload.h"

namespace {

constexpr char kDirectory[] = "/reader";
constexpr char kPath[] = "/reader/favorites-v1.json";
constexpr char kPartialPath[] = "/reader/favorites-v1.json.part";
constexpr char kOldPath[] = "/reader/favorites-v1.json.old";

void setError(FavoriteCollection &collection, const char *message) {
    snprintf(collection.error, sizeof(collection.error), "%s", message);
}

bool validFolder(uint8_t value) {
    return value <= static_cast<uint8_t>(FavoriteFolder::Later);
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

bool writeCollection(FavoriteCollection &collection) {
    if (!SD.exists(kDirectory) && !SD.mkdir(kDirectory)) {
        setError(collection, "directory-create-failed");
        return false;
    }
    SD.remove(kPartialPath);
    File output = SD.open(kPartialPath, FILE_WRITE);
    if (!output) {
        setError(collection, "state-open-failed");
        return false;
    }
    JsonDocument document;
    document["schema"] = "abyss-reader-favorites";
    document["version"] = 1;
    JsonArray books = document["books"].to<JsonArray>();
    for (size_t index = 0; index < collection.count; ++index) {
        const FavoriteEntry &entry = collection.entries[index];
        JsonObject book = books.add<JsonObject>();
        book["id"] = entry.bookId;
        book["title"] = entry.title;
        book["author"] = entry.author;
        book["acquisition"] = entry.acquisitionHref;
        if (entry.coverHref[0] != '\0') {
            book["cover"] = entry.coverHref;
        }
        book["folder"] = static_cast<uint8_t>(entry.folder);
        book["local"] = entry.local;
    }
    const bool written = serializeJson(document, output) > 0;
    output.flush();
    output.close();
    if (!written || !publishFile()) {
        SD.remove(kPartialPath);
        setError(collection, "state-publish-failed");
        return false;
    }
    collection.ok = true;
    collection.error[0] = '\0';
    return true;
}

}  // namespace

bool FavoritesStore::load(FavoriteCollection &collection) {
    collection = FavoriteCollection{};
    if (!StorageRecovery::recoverFile(kPath)) {
        setError(collection, "state-recovery-failed");
        return false;
    }
    File input = SD.open(kPath, FILE_READ);
    if (!input) {
        if (SD.exists(kPath)) {
            setError(collection, "state-open-failed");
            return false;
        }
        collection.ok = true;
        return true;
    }
    JsonDocument document;
    const DeserializationError jsonError = deserializeJson(document, input);
    input.close();
    const char *schema = document["schema"].as<const char *>();
    const uint32_t version = document["version"] | 0U;
    JsonArray books = document["books"].as<JsonArray>();
    if (jsonError || schema == nullptr ||
        strcmp(schema, "abyss-reader-favorites") != 0 || version != 1 ||
        books.isNull()) {
        setError(collection, "invalid-state");
        return false;
    }
    for (JsonObject book : books) {
        if (collection.count >= kFavoriteCapacity) {
            break;
        }
        const char *bookId = book["id"].as<const char *>();
        const uint8_t folder = book["folder"] | 0U;
        if (bookId == nullptr || !BookUploadReceiver::validBookId(bookId) ||
            !validFolder(folder)) {
            continue;
        }
        FavoriteEntry &entry = collection.entries[collection.count++];
        snprintf(entry.bookId, sizeof(entry.bookId), "%s", bookId);
        snprintf(entry.title, sizeof(entry.title), "%s",
                 book["title"] | bookId);
        snprintf(entry.author, sizeof(entry.author), "%s",
                 book["author"] | "");
        snprintf(entry.acquisitionHref, sizeof(entry.acquisitionHref), "%s",
                 book["acquisition"] | "");
        snprintf(entry.coverHref, sizeof(entry.coverHref), "%s",
                 book["cover"] | "");
        entry.folder = static_cast<FavoriteFolder>(folder);
        entry.local = book["local"] | false;
    }
    collection.ok = true;
    return true;
}

bool FavoritesStore::put(const FavoriteEntry &entry,
                         FavoriteCollection &collection) {
    if (!BookUploadReceiver::validBookId(entry.bookId)) {
        collection = FavoriteCollection{};
        setError(collection, "invalid-book-id");
        return false;
    }
    FavoriteCollection current{};
    if (!load(current)) {
        collection = current;
        return false;
    }
    size_t target = current.count;
    for (size_t index = 0; index < current.count; ++index) {
        if (strcmp(current.entries[index].bookId, entry.bookId) == 0) {
            target = index;
            break;
        }
    }
    if (target == current.count) {
        if (current.count >= kFavoriteCapacity) {
            collection = current;
            setError(collection, "capacity-reached");
            return false;
        }
        ++current.count;
    }
    current.entries[target] = entry;
    collection = current;
    return writeCollection(collection);
}

bool FavoritesStore::remove(const char *bookId,
                            FavoriteCollection &collection) {
    FavoriteCollection current{};
    if (!load(current)) {
        collection = current;
        return false;
    }
    for (size_t index = 0; index < current.count; ++index) {
        if (strcmp(current.entries[index].bookId, bookId) != 0) {
            continue;
        }
        if (index + 1 < current.count) {
            memmove(&current.entries[index], &current.entries[index + 1],
                    (current.count - index - 1) * sizeof(FavoriteEntry));
        }
        --current.count;
        current.entries[current.count] = FavoriteEntry{};
        collection = current;
        return writeCollection(collection);
    }
    collection = current;
    collection.ok = true;
    return true;
}

bool FavoritesStore::find(const FavoriteCollection &collection,
                          const char *bookId, FavoriteEntry &entry) {
    if (bookId == nullptr) {
        return false;
    }
    for (size_t index = 0; index < collection.count; ++index) {
        if (strcmp(collection.entries[index].bookId, bookId) == 0) {
            entry = collection.entries[index];
            return true;
        }
    }
    return false;
}

const char *FavoritesStore::folderLabel(FavoriteFolder folder) {
    switch (folder) {
        case FavoriteFolder::WantToRead:
            return "ХОЧУ ПРОЧИТАТЬ";
        case FavoriteFolder::Favorite:
            return "ЛЮБИМЫЕ";
        case FavoriteFolder::Later:
            return "НА ПОТОМ";
    }
    return "ИЗБРАННОЕ";
}
