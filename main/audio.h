#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_SAMPLE_RATE 16000

// Set up the Echo Base: IO expander, ES8311 (mic in, DAC muted), I2S RX.
esp_err_t audio_init(void);
// Read up to n mono samples (left slot, 32-bit, audio in the top bits). Blocks.
size_t audio_read(int32_t *samples, size_t n);
