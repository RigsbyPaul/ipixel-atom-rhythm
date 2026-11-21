#pragma once
#include <stdbool.h>
#include <stdint.h>

// Self-drawn bars using the panel's live drawing (DIY) mode.
// Bars are 2 pixels wide with a 1-pixel gap, so a panel w wide shows w/3 bars,
// each 0..h pixels tall, green / yellow / red from the bottom up.

static inline int draw_bar_count(int width) { return width / 3; }

// Enter drawing mode on a cleared screen and forget what was drawn.
int draw_enter(void);
// Leave drawing mode; the panel goes back to its saved program.
int draw_leave(void);
// Send only the pixels that changed since the last frame. Returns 0, or a
// NimBLE error (then call draw_enter() again to redraw from scratch).
int draw_frame(const uint8_t *heights, int count, int width, int height);
