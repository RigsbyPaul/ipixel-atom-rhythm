#pragma once
#include <stdbool.h>
#include <stdint.h>

// Music analysis for the disco style.

// Feed the latest 11 music levels (0..15) and the seconds since the last call.
// Works out loudness, bass, treble and beats.
void fx_listen(const uint8_t levels[11], float dt);
// True if a beat was heard in the last fx_listen.
bool fx_beat(void);
// 0..1: average level, low three bands, top three bands.
float fx_loud(void);
float fx_bass(void);
float fx_treble(void);
// 1 on a beat, decaying quickly.
float fx_kick(void);
// Beats since the last call (frames are slower than beats).
uint32_t fx_take_beats(void);
// Each band's peak (0..1) since the last call.
void fx_take_peaks(float out[11]);
