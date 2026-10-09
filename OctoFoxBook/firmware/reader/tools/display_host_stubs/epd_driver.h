#pragma once
#include <cstdint>
constexpr int EPD_WIDTH = 8, EPD_HEIGHT = 4;
struct Rect_t { int32_t x, y, width, height; };
inline Rect_t epd_full_screen() { return {0, 0, EPD_WIDTH, EPD_HEIGHT}; }
inline void epd_init() {}
inline void epd_poweron() {}
inline void epd_poweroff() {}
inline void epd_poweroff_all() {}
inline void epd_clear() {}
inline void epd_draw_grayscale_image(Rect_t, uint8_t *) {}
inline void epd_push_pixels(Rect_t, int16_t, int) {}
