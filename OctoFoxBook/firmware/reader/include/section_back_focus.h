#pragma once
#include <stddef.h>

// Back is a separate control above the list, so book indices and saved positions
// keep their meaning. An empty list still has an operable way out.
namespace SectionBackFocus {
enum class Action { None, Redraw, Back };
inline Action input(bool &focused, size_t selected, size_t count, int direction, bool open) {
    if (open && (focused || count == 0)) { focused = false; return Action::Back; }
    if (direction < 0 && (focused || selected == 0 || count == 0)) {
        focused = true; return Action::Redraw;
    }
    if (focused && direction > 0) {
        if (count) focused = false;
        return Action::Redraw;
    }
    return Action::None;
}
}
