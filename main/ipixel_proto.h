// iPixel panel command builders, ported from ipixel-ble protocol.py.
// Every command starts with its 2-byte little-endian total length.
#pragma once
#include <stdint.h>
#include <string.h>

#define IPIXEL_RHYTHM_BARS 11
#define IPIXEL_RHYTHM_LEVEL_MAX 15
#define IPIXEL_RHYTHM_STYLES 5
#define IPIXEL_RHYTHM_LEN 16

// Music-bar screen: 10 00 01 02 <style> <11 levels 0..15>. No reply.
static inline void ipixel_build_rhythm(uint8_t out[IPIXEL_RHYTHM_LEN], uint8_t style,
                                       const uint8_t levels[IPIXEL_RHYTHM_BARS])
{
    out[0] = 16; out[1] = 0; out[2] = 1; out[3] = 2;
    out[4] = style < IPIXEL_RHYTHM_STYLES ? style : 0;
    for (int i = 0; i < IPIXEL_RHYTHM_BARS; i++) {
        out[5 + i] = levels[i] > IPIXEL_RHYTHM_LEVEL_MAX ? IPIXEL_RHYTHM_LEVEL_MAX : levels[i];
    }
}

// Brightness 0..100: 05 00 04 80 <level>
static inline void ipixel_build_brightness(uint8_t out[5], uint8_t level)
{
    out[0] = 5; out[1] = 0; out[2] = 4; out[3] = 0x80; out[4] = level > 100 ? 100 : level;
}

// Power: 05 00 07 01 <on>
static inline void ipixel_build_power(uint8_t out[5], int on)
{
    out[0] = 5; out[1] = 0; out[2] = 7; out[3] = 1; out[4] = on ? 1 : 0;
}
