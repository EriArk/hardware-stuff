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
    bool sectionsSelected = false;
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
}
