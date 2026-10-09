#pragma once
#include <stddef.h>
#include <string.h>

// Three-button editor. No Arduino, display or storage dependency.
class TextKeyboard {
public:
    enum class Result { Changed, Back, Submit, Full };
    unsigned row = 0, column = 0, mode = 0;
    bool inside = false, visible = false;
    void reset(const char *initial = "", size_t limit = 63) {
        wipe(); limit_ = limit > 64 ? 64 : limit;
        const size_t n = strnlen(initial, limit_);
        memcpy(value_, initial, n); value_[n] = 0;
    }
    void wipe() {
        volatile char *p = value_;
        for (size_t i = 0; i < sizeof(value_); ++i) p[i] = 0;
        row = column = mode = 0; inside = visible = false;
    }
    const char *value() const { return value_; }
    size_t length() const { return strlen(value_); }
    size_t limit() const { return limit_; }
    const char *characters(unsigned r) const {
        static const char *lower[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
        static const char *upper[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
        static const char *digits[] = {"1234567890", "-+.,:/()", "%#*=!?"};
        static const char *symbols[] = {"!@#$%^&*()", "-_+=[]{};:", "'\"\\|/?,.<>`~"};
        if (r > 2) return "";
        return (mode == 0 ? lower : mode == 1 ? upper : mode == 2 ? digits : symbols)[r];
    }
    unsigned columns(unsigned r) const { return r < 3 ? strlen(characters(r)) : r == 3 ? 4 : r == 4 ? 3 : 1; }
    void move(int delta) {
        unsigned &position = inside ? column : row;
        const int count = inside ? columns(row) : 6;
        position = (static_cast<int>(position) + delta % count + count) % count;
        if (!inside) column = 0;
    }
    Result back() { if (inside) { inside = false; return Result::Changed; } return Result::Back; }
    Result confirm() {
        if (row == 5) return Result::Submit;
        if (!inside) { inside = true; return Result::Changed; }
        if (row < 3) return append(characters(row)[column]);
        if (row == 3) { mode = column; row = column = 0; inside = false; }
        else if (column == 0) return append(' ');
        else if (column == 1) { const size_t n = length(); if (n) value_[n-1] = 0; }
        else visible = !visible;
        return Result::Changed;
    }
private:
    char value_[65]{};
    size_t limit_ = 63;
    Result append(char c) {
        const size_t n = length();
        if (n >= limit_) return Result::Full;
        value_[n] = c; value_[n+1] = 0;
        return Result::Changed;
    }
};
