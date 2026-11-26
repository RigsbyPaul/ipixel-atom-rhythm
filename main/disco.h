#pragma once
#include <stdint.h>

// Style 6, disco: whole-panel pictures drawn from the music and sent as live
// frames (live.c) in the panel's DIY mode, about 2.4 a second on a 192 x 32.

// Draw one frame into rgb (width * height * 3, row by row from the top), from
// the music since the last frame (fx_take_beats / fx_take_peaks). dt = seconds
// since the last frame. Returns the frame's average colour in avg[3].
void disco_render(uint8_t *rgb, int width, int height, float dt, uint8_t avg[3]);
