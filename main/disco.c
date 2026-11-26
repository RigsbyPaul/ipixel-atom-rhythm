// Live disco frames. Frames come only ~2.4 times a second, so the picture is
// kept calm: big jumps every frame looked jittery.
//   - background: a colour that steps 60 degrees round the wheel on a beat (at
//     most every 1.5 s), brightening on a beat and dimming between beats;
//   - 11 frequency beams rising from the bottom, bass left to treble right,
//     each band's peak since the last frame, in colours opposite the background;
//   - a few white sparkles on strong treble.
#include "disco.h"

#include <math.h>
#include <string.h>
#include "esp_random.h"
#include "fx.h"

#define BANDS 11

static float s_bg_hue = 200, s_bg_level, s_since_hue, s_level[BANDS];

static void hsv(float h, float s, float v, float out[3])
{
    h = fmodf(h, 360);
    if (h < 0) h += 360;
    h /= 60;
    float c = v * s, x = c * (1 - fabsf(fmodf(h, 2) - 1)), m = v - c;
    float r, g, b;
    if (h < 1) { r = c; g = x; b = 0; }
    else if (h < 2) { r = x; g = c; b = 0; }
    else if (h < 3) { r = 0; g = c; b = x; }
    else if (h < 4) { r = 0; g = x; b = c; }
    else if (h < 5) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }
    out[0] = r + m;
    out[1] = g + m;
    out[2] = b + m;
}

static inline uint8_t u8(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255); }

void disco_render(uint8_t *rgb, int w, int h, float dt, uint8_t avg[3])
{
    uint32_t beats = fx_take_beats();
    float peak[BANDS];
    fx_take_peaks(peak);
    float loud = fx_loud(), treble = fx_treble();

    // Colour steps on the beat, at most every HUE_HOLD_S: stepping on every
    // beat looked jittery at this frame rate.
    const float HUE_HOLD_S = 1.5f;
    s_since_hue += dt;
    if (beats) {
        s_bg_level = 1;
        if (s_since_hue >= HUE_HOLD_S) {
            s_bg_hue += 60;
            s_since_hue = 0;
        }
    } else {
        s_bg_level *= expf(-dt * 3);
    }
    // Beams: this frame's peaks, unsmoothed (smoothing made them lag seconds
    // behind the music at this frame rate).
    for (int b = 0; b < BANDS; b++) s_level[b] = peak[b];

    // Background: the beat colour, brighter just after a beat, never quite
    // black while there is music.
    float bg[3];
    hsv(s_bg_hue, 1, 0.05f + 0.3f * s_bg_level + 0.1f * fminf(1, loud * 2), bg);

    // Beams: band b fills columns [x0, x1) up to its level, with a 1-pixel gap.
    float bw = (float)w / BANDS;
    for (int b = 0; b < BANDS; b++) {
        int x0 = (int)(b * bw), x1 = (int)((b + 1) * bw) - 1;
        int top = h - (int)(fminf(1, s_level[b] * 1.15f) * h + 0.5f);  // first lit row
        float col[3];
        hsv(s_bg_hue + 150 + b * 6, 1, 1, col);  // opposite the background, a gentle spread
        for (int y = 0; y < h; y++) {
            for (int x = x0; x < (int)((b + 1) * bw); x++) {
                uint8_t *p = rgb + (y * w + x) * 3;
                if (x < x1 && y >= top) {
                    // Brighter towards the top of the beam.
                    float k = 0.55f + 0.45f * (float)(h - y) / (h - top + 1);
                    p[0] = u8(col[0] * k);
                    p[1] = u8(col[1] * k);
                    p[2] = u8(col[2] * k);
                } else {
                    p[0] = u8(bg[0]);
                    p[1] = u8(bg[1]);
                    p[2] = u8(bg[2]);
                }
            }
        }
    }

    // Sparkles: only on strong treble, and few.
    int n = treble > 0.4f ? (int)((treble - 0.4f) * 25 * w * h / (192 * 32)) : 0;
    for (int i = 0; i < n; i++) {
        uint32_t r = esp_random();
        int x = r % w, y = (r >> 16) % h;
        memset(rgb + (y * w + x) * 3, 255, 3);
    }

    uint32_t sum[3] = {0};
    for (int i = 0; i < w * h; i++) {
        sum[0] += rgb[i * 3];
        sum[1] += rgb[i * 3 + 1];
        sum[2] += rgb[i * 3 + 2];
    }
    for (int k = 0; k < 3; k++) avg[k] = (uint8_t)(sum[k] / (w * h));
}
