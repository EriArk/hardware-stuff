#pragma once
// Recovery tests exercise reader-global files, not book-ID validation.
class BookUploadReceiver {
public:
    static bool validBookId(const char *) { return true; }
};
