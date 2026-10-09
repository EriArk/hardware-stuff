#pragma once

#include <Arduino.h>

constexpr size_t kFavoriteCapacity = 48;
constexpr size_t kFavoriteUrlCapacity = 512;

enum class FavoriteFolder : uint8_t {
    WantToRead = 0,
    Favorite = 1,
    Later = 2,
};

struct FavoriteEntry {
    char bookId[33]{};
    char title[192]{};
    char author[160]{};
    char acquisitionHref[kFavoriteUrlCapacity]{};
    char coverHref[kFavoriteUrlCapacity]{};
    FavoriteFolder folder = FavoriteFolder::WantToRead;
    bool local = false;
};

struct FavoriteCollection {
    bool ok = false;
    size_t count = 0;
    FavoriteEntry entries[kFavoriteCapacity]{};
    char error[64]{};
};

class FavoritesStore {
public:
    static bool load(FavoriteCollection &collection);
    static bool put(const FavoriteEntry &entry,
                    FavoriteCollection &collection);
    static bool remove(const char *bookId, FavoriteCollection &collection);
    static bool find(const FavoriteCollection &collection, const char *bookId,
                     FavoriteEntry &entry);
    static const char *folderLabel(FavoriteFolder folder);
};
