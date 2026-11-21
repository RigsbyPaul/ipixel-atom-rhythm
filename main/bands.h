#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BANDS_FFT_SIZE 512
#define BANDS_MAX 128

// One way of splitting the spectrum into bars: its own band edges, AGC and fall.
typedef struct {
    int count;      // number of bars
    int max_level;  // top level (15 for the native rhythm screen, panel height for self-draw)
    int lo[BANDS_MAX], hi[BANDS_MAX];  // FFT bin range per bar
    float tilt;     // dB added per bar (lifts treble)
    float peak, floor;
    uint8_t levels[BANDS_MAX];
} bands_t;

// Set up count log-spaced bars (~60 Hz .. 8 kHz) with levels 0..max_level.
void bands_init(bands_t *b, int count, int max_level);
// Run the FFT on one block of BANDS_FFT_SIZE mic samples. Call once per block.
// Returns false for digital silence (codec start-up).
bool bands_fft(const int32_t *samples);
// Turn the last FFT into levels for b (instant rise, slow fall, AGC). out gets b->count values.
void bands_levels(bands_t *b, bool silent, uint8_t *out);
