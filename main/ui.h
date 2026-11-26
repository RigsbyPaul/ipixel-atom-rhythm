#pragma once
#include <stdint.h>

typedef enum { UI_SHORT_PRESS = 1, UI_LONG_PRESS = 2 } ui_press_t;
typedef void (*ui_press_cb)(ui_press_t press);

// Start the button (G41) and status LED (G35) task. on_press runs in that task.
void ui_start(ui_press_cb on_press);
// Briefly flash the LED white (acknowledge a press).
void ui_flash(void);
// Set the style whose colour the LED shows while connected (0..4).
void ui_set_style(uint8_t style);
// For disco (style 6) the connected LED shows this colour: the panel's average.
void ui_set_live_colour(uint8_t r, uint8_t g, uint8_t b);
