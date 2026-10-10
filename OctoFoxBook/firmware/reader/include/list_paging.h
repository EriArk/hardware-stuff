#pragma once
#include <stddef.h>

// Keep rows stationary while selecting within a page. The final page may be
// shorter: never pull earlier rows forward just to fill the screen.
constexpr size_t listPageStart(size_t selected, size_t rowsPerPage) {
    return rowsPerPage ? (selected / rowsPerPage) * rowsPerPage : 0;
}
