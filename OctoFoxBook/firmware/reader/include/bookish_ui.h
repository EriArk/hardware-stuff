#include "i18n.h"
#pragma once
#include <stddef.h>
#include <stdint.h>

// Layout is independent of SD, networking, input and the display driver.
namespace BookishUI {
enum class Font { Small, Body, Control, Heading, Title, Hero };
struct Book {
    const char *id = "";
    const char *title = "";
    const char *author = "";
    const char *status = "";
    uint8_t progress = 0;
    bool selected = false;
};
struct Home {
    const Book *hero = nullptr;
    Book rows[2];
    size_t rowCount = 0;
    bool added = false;
    bool empty = false;
    bool syncSelected = false;
    bool settingsSelected = false;
    bool syncing = false;
    const char *notice = "";
    int tabFocus = -1;
    unsigned activeTab = 0;
};
class Canvas {
public:
    virtual ~Canvas() = default;
    virtual void box(int x, int y, int w, int h, int radius,
                     uint8_t border, uint8_t fill, int thickness = 1) = 0;
    virtual void text(Font font, const char *value, int x, int baseline,
                      int width, uint8_t ink = 0, uint8_t paper = 15) = 0;
    virtual void title(const char *value, int x, int baseline, int width) = 0;
    virtual void cover(const char *id, int x, int y, int w, int h) = 0;
    virtual void logo(int x, int y) = 0;
};
void header(Canvas &c, unsigned activeTab, int focus = -1);
void home(Canvas &c, const Home &view);
constexpr size_t kListRows = 4;
struct ListRow {
    const char *title = "";
    const char *subtitle = "";
    const char *detail = "";
    const char *coverId = "";
    bool book = false;
    bool selected = false;
    uint8_t progress = 0;
};
struct List {
    unsigned activeTab = 1;
    int tabFocus = -1;
    const char *title = "";
    const char *subtitle = "";
    const char *back = "";
    bool backFocused = false;
    const char *navigationHint = nullptr;
    const char *emptyTitle = I18n::tr("Здесь пока нет книг");
    const char *emptyHint = I18n::tr("Выберите другую подборку.");
    const char *emptyHint2 = "";
    ListRow rows[kListRows];
    size_t rowCount = 0;
    size_t total = 0;
    size_t selected = 0;
};
void list(Canvas &c, const List &view);
struct Card {
    Book book;
    unsigned activeTab = 1, focus = 0;
    const char *primary = I18n::tr("Читать");
    const char *series = I18n::tr("Без серии");
    bool favorite = false;
};
void card(Canvas &c, const Card &view);
void batteryNotice(Canvas &c, unsigned percent, bool critical);
struct Sync {
    const char *stage = "";
    const char *detail = "";
    int percent = -1;
    unsigned downloaded = 0, elapsedSeconds = 0, activeTab = 1;
    bool cancelling = false;
};
void syncProgress(Canvas &c, const Sync &view);
}
