#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ipixel.h"

typedef struct {
    uint8_t style;       // rhythm style 0..4
    uint8_t brightness;  // 0..100
    bool have_panel;
    ipixel_addr_t panel;
} settings_t;

// Load from NVS (defaults where nothing is saved).
void settings_load(settings_t *s);
void settings_save_style(uint8_t style);
void settings_save_brightness(uint8_t brightness);
void settings_save_panel(const ipixel_addr_t *addr);
