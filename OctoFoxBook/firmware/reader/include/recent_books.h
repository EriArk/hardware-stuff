#pragma once

#include <Arduino.h>

constexpr size_t kRecentBookCapacity = 4;

struct RecentBookList {
    bool ok = false;
    size_t count = 0;
    char ids[kRecentBookCapacity][33]{};
    char error[64]{};
};

class RecentBooks {
public:
    static bool load(RecentBookList &list);
    static bool touch(const char *bookId, RecentBookList &list);
};
