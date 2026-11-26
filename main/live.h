#pragma once
#include <stdint.h>

// Whole-panel pictures as live (unsaved) raw RGB frames; see live.c. The panel
// must be in DIY mode (draw_enter) first.

// Prepare for a panel of this size (re-call when the size changes). Returns the
// RGB888 buffer to draw into (width * height * 3 bytes, row by row from the top),
// or NULL if out of memory.
uint8_t *live_begin(int width, int height);
// Send the buffer as one frame, waiting for the panel's answer to each block.
// Returns 0, or a NimBLE error (-3 = no answer from the panel).
int live_send(void);
// Panel answers since the last call: invalid, next, no space (end of frame),
// complete, then timeouts.
void live_acks(uint32_t out[5]);
