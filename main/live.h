#pragma once
#include <stdint.h>

// Whole-panel pictures as live (unsaved) frames: the picture is JPEG-encoded
// and sent as ipixel-ble's encode_live_frame does.

// Prepare for a panel of this size (re-call when the size changes). Returns the
// RGB888 buffer to draw into (width * height * 3 bytes, row by row from the top),
// or NULL if out of memory.
uint8_t *live_begin(int width, int height);
// Encode the buffer and send it. Returns 0, or a NimBLE / encoder error.
int live_send(void);
// Size of the last JPEG sent, in bytes (for the status line).
int live_last_size(void);
