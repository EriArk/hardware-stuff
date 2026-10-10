#pragma once
#include <stddef.h>
#include <string.h>
#include <stdint.h>

// Three-button editor. No Arduino, display or storage dependency.
class TextKeyboard {
public:
    enum class Result { Changed, Back, Submit, Full };
    unsigned row = 0, column = 0, mode = 0;
    bool inside = false, visible = false;
    // Layout selection is independent of the interface language. English is
    // always available for network passwords; all limits count UTF-8 bytes.
    enum Layout { English, Russian, German, French, Spanish, Portuguese, Italian, LayoutCount };
    static constexpr uint16_t AllLayouts = (1U << LayoutCount) - 1;
    uint16_t enabledLayouts = 3;
    unsigned layout = English;
    void configure(uint16_t enabled) {
        enabledLayouts = (enabled & AllLayouts) | 1;
        if (!(enabledLayouts & (1U << layout))) layout = English;
    }
    static const char *layoutName(unsigned id) {
        static const char *names[] = {"English", "Русский", "Deutsch", "Français", "Español", "Português", "Italiano"};
        return names[id < LayoutCount ? id : 0];
    }
    const char *modeLabel(unsigned col) const {
        static const char *labels[] = {"abc", "ABC", "123", "#+="};
        static const char *codes[] = {"EN", "RU", "DE", "FR", "ES", "PT", "IT"};
        return col < 4 ? labels[col] : codes[layout];
    }
    void reset(const char *initial = "", size_t limit = 63) {
        wipe(); limit_ = limit > 64 ? 64 : limit;
        size_t n = strnlen(initial, limit_);
        // Do not split a multibyte code point when loading a bounded value.
        if (initial[n]) while (n && (static_cast<unsigned char>(initial[n]) & 0xc0) == 0x80) --n;
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
        static const char *russian[] = {"йцукенгшщзхъ", "фывапролджэ", "ячсмитьбюё"};
        static const char *russianUpper[] = {"ЙЦУКЕНГШЩЗХЪ", "ФЫВАПРОЛДЖЭ", "ЯЧСМИТЬБЮЁ"};
        static const char *other[5][2][3] = {
            {{"qwertzuiopü", "asdfghjklöä", "yxcvbnmß"}, {"QWERTZUIOPÜ", "ASDFGHJKLÖÄ", "YXCVBNM"}},
            {{"azertyuiopéè", "qsdfghjklmàç", "wxcvbnùâêîôûëï"}, {"AZERTYUIOPÉÈ", "QSDFGHJKLMÀÇ", "WXCVBNÙÂÊÎÔÛËÏ"}},
            {{"qwertyuiopáé", "asdfghjklñí", "zxcvbnmóúü"}, {"QWERTYUIOPÁÉ", "ASDFGHJKLÑÍ", "ZXCVBNMÓÚÜ"}},
            {{"qwertyuiopáà", "asdfghjklçéê", "zxcvbnmãõíóôúâ"}, {"QWERTYUIOPÁÀ", "ASDFGHJKLÇÉÊ", "ZXCVBNMÃÕÍÓÔÚÂ"}},
            {{"qwertyuiopèé", "asdfghjklàò", "zxcvbnmìù"}, {"QWERTYUIOPÈÉ", "ASDFGHJKLÀÒ", "ZXCVBNMÌÙ"}}
        };
        if (r > 2) return "";
        if (mode < 2 && layout == Russian) return (mode == 0 ? russian : russianUpper)[r];
        if (mode < 2 && layout >= German && layout < LayoutCount) return other[layout-German][mode][r];
        return (mode == 0 ? lower : mode == 1 ? upper : mode == 2 ? digits : symbols)[r];
    }
    unsigned columns(unsigned r) const {
        if (r >= 3) return r == 3 ? 5 : r == 4 ? 3 : 1;
        unsigned n=0; for (const unsigned char *p=(const unsigned char*)characters(r); *p; ++p) if ((*p&0xc0)!=0x80) ++n;
        return n;
    }
    void symbol(unsigned r, unsigned col, char out[5]) const {
        const unsigned char *p=(const unsigned char*)characters(r);
        while (*p && col) { ++p; while ((*p&0xc0)==0x80) ++p; --col; }
        unsigned n=0; if (*p) {out[n++]=*p++; while ((*p&0xc0)==0x80 && n<4) out[n++]=*p++;} out[n]=0;
    }
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
        if (row < 3) { char s[5]; symbol(row,column,s); return append(s); }
        if (row == 3) {
            if (column == 4) {
                do { layout = (layout + 1) % LayoutCount; } while (!(enabledLayouts & (1U << layout)));
                mode = mode < 2 ? mode : 0;
            } else { mode = column; row = column = 0; inside = false; }
        }
        else if (column == 0) return append(' ');
        else if (column == 1) { size_t n = length(); if (n) { --n; while (n && (static_cast<unsigned char>(value_[n])&0xc0)==0x80) --n; value_[n]=0; } }
        else visible = !visible;
        return Result::Changed;
    }
private:
    char value_[65]{};
    size_t limit_ = 63;
    Result append(const char *s) {
        const size_t n=length(), added=strlen(s);
        if (n+added>limit_) return Result::Full;
        memcpy(value_+n,s,added+1); return Result::Changed;
    }
    Result append(char c) {
        const size_t n = length();
        if (n >= limit_) return Result::Full;
        value_[n] = c; value_[n+1] = 0;
        return Result::Changed;
    }
};
