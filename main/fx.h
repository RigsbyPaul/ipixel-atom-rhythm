#pragma once
#include <stdbool.h>
#include <stdint.h>

// Whole-panel disco effects, drawn into an RGB888 buffer (row by row from the top).
typedef enum {
    FX_WASH,      // whole panel one colour: hue follows pitch, brightness follows loudness
    FX_BEAT,      // new bold colour on every beat, fading until the next
    FX_BASS,      // coloured ripples from the centre on each bass hit
    FX_SPECTRUM,  // soft blocks of colour, bass red (left) to treble violet (right)
    FX_SPARKLE,   // twinkling pixels on the treble, a burst on each beat
    FX_CHASE,     // rainbow scrolling along, faster when loud, kicked on the beat
    FX_COUNT,
} fx_t;

// Feed the latest 11 music levels (0..15) and the seconds since the last call.
// Works out loudness, bass, treble and beats.
void fx_listen(const uint8_t levels[11], float dt);
// True if a beat was heard in the last fx_listen.
bool fx_beat(void);
// Draw one frame of effect e. Returns the panel's average colour in avg[3].
void fx_render(fx_t e, uint8_t *rgb, int width, int height, float dt, uint8_t avg[3]);
