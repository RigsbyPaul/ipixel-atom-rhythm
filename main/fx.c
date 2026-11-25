// Disco effects. Ported from the preview mockup; all sizes are relative to a
// 192 x 32 panel and scale to other panels.
#include "fx.h"

#include <math.h>
#include <string.h>
#include "esp_random.h"

#define BANDS 11

static float s_band[BANDS];  // 0..1
static float s_loud, s_bass, s_treble;
static float s_kick;         // 1 on a beat, decays
static bool s_beat;
static float s_bass_avg = 0.3f, s_since_beat = 1;

static float frand(void) { return (esp_random() & 0xFFFF) / 65535.0f; }

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
    out[0] = r + m; out[1] = g + m; out[2] = b + m;
}

void fx_listen(const uint8_t levels[11], float dt)
{
    float sum = 0;
    for (int i = 0; i < BANDS; i++) {
        s_band[i] = levels[i] / 15.0f;
        sum += s_band[i];
    }
    s_loud = sum / BANDS;
    s_bass = (s_band[0] + s_band[1] + s_band[2]) / 3;
    s_treble = (s_band[8] + s_band[9] + s_band[10]) / 3;

    // Beat: bass well above its recent average, at most ~4 per second.
    s_since_beat += dt;
    s_beat = s_bass > 0.35f && s_bass > s_bass_avg * 1.35f + 0.08f && s_since_beat > 0.25f;
    if (s_beat) {
        s_since_beat = 0;
        s_kick = 1;
    }
    s_bass_avg += (s_bass - s_bass_avg) * fminf(1, dt * 2.5f);
    s_kick *= expf(-dt * 6);
}

bool fx_beat(void) { return s_beat; }

static inline void put(uint8_t *p, const float c[3])
{
    for (int k = 0; k < 3; k++) {
        float v = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
        p[k] = (uint8_t)(v * 255);
    }
}

static void fill(uint8_t *rgb, int w, int h, const float c[3])
{
    uint8_t px[3];
    put(px, c);
    for (int i = 0; i < w * h; i++) memcpy(rgb + i * 3, px, 3);
}

// --- effects ---

static float s_wash_hue = 200, s_time;

static void wash(uint8_t *rgb, int w, int h, float dt)
{
    float c = 0, s = 0.01f;
    for (int i = 0; i < BANDS; i++) { c += s_band[i] * i; s += s_band[i]; }
    float target = c / s * 28;  // bass-heavy = red, treble-heavy = blue/violet
    s_wash_hue += (target - s_wash_hue) * fminf(1, dt * 1.2f);
    float col[3];
    hsv(s_wash_hue + s_time * 8, 0.85f, 0.25f + 0.75f * fminf(1, s_loud * 1.6f) * (0.6f + 0.4f * s_kick), col);
    fill(rgb, w, h, col);
}

static float s_beat_hue;

static void beat(uint8_t *rgb, int w, int h, float dt)
{
    if (s_beat) s_beat_hue += 137;  // golden-angle steps: every colour differs from the last
    float col[3];
    hsv(s_beat_hue, 1, powf(s_kick, 1.3f), col);
    fill(rgb, w, h, col);
}

#define MAX_RIPPLES 6
static struct { float r, hue; } s_rip[MAX_RIPPLES];
static int s_nrip;

static void bass(uint8_t *rgb, int w, int h, float dt)
{
    if (s_beat) {
        if (s_nrip == MAX_RIPPLES) { memmove(s_rip, s_rip + 1, sizeof s_rip[0] * --s_nrip); }
        s_rip[s_nrip].r = 0;
        s_rip[s_nrip].hue = fmodf(s_time * 40, 360);
        s_nrip++;
    }
    for (int i = 0; i < s_nrip; i++) s_rip[i].r += 48 * dt;
    while (s_nrip && s_rip[0].r > 110) { memmove(s_rip, s_rip + 1, sizeof s_rip[0] * --s_nrip); }

    float sx = 192.0f / w, sy = 32.0f / h;
    float base_hue = s_time * 10;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float d = hypotf((x - w / 2.0f) * sx * 0.55f, (y - h / 2.0f) * sy * 1.6f);
            float acc[3];
            hsv(base_hue, 0.8f, 0.08f + 0.25f * s_kick * fmaxf(0, 1 - d / 40), acc);
            for (int i = 0; i < s_nrip; i++) {
                float k = fmaxf(0, 1 - fabsf(d - s_rip[i].r) / 9) * (1 - s_rip[i].r / 110);
                if (k <= 0) continue;
                float c[3];
                hsv(s_rip[i].hue, 1, k, c);
                acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2];
            }
            put(rgb + (y * w + x) * 3, acc);
        }
    }
}

static void spectrum(uint8_t *rgb, int w, int h, float dt)
{
    float bw = (float)w / BANDS;
    for (int b = 0; b < BANDS; b++) {
        float col[3];
        hsv(b * 27, 0.9f, fminf(1, s_band[b] * 1.2f), col);
        int x0 = (int)(b * bw), x1 = (int)((b + 1) * bw);
        for (int x = x0; x < x1; x++) {
            float edge = fminf(1, fminf(x - b * bw, (b + 1) * bw - x) / (bw / 5));  // soft edges
            float c[3] = {col[0] * edge, col[1] * edge, col[2] * edge};
            for (int y = 0; y < h; y++) put(rgb + (y * w + x) * 3, c);
        }
    }
}

#define MAX_SPARKS 400
static struct { uint8_t x, y; float l, c[3]; } s_spk[MAX_SPARKS];
static int s_nspk;

static void sparkle(uint8_t *rgb, int w, int h, float dt)
{
    float area = (float)(w * h) / (192 * 32);
    int n = (int)((s_treble * 30 * dt * 20 + (s_beat ? 160 : 0)) * area);
    for (int i = 0; i < n && s_nspk < MAX_SPARKS; i++) {
        s_spk[s_nspk].x = (uint8_t)(frand() * w);
        s_spk[s_nspk].y = (uint8_t)(frand() * h);
        s_spk[s_nspk].l = 1;
        hsv(frand() * 360, 0.6f, 1, s_spk[s_nspk].c);
        s_nspk++;
    }
    memset(rgb, 0, w * h * 3);
    float fade = powf(0.86f, dt * 20);
    int keep = 0;
    for (int i = 0; i < s_nspk; i++) {
        s_spk[i].l *= fade;
        if (s_spk[i].l < 0.05f) continue;
        float c[3] = {s_spk[i].c[0] * s_spk[i].l, s_spk[i].c[1] * s_spk[i].l, s_spk[i].c[2] * s_spk[i].l};
        put(rgb + (s_spk[i].y * w + s_spk[i].x) * 3, c);
        s_spk[keep++] = s_spk[i];
    }
    s_nspk = keep;
}

static float s_chase;

static void chase(uint8_t *rgb, int w, int h, float dt)
{
    s_chase += (0.6f + s_loud * 2) * dt * 20 + (s_beat ? 14 : 0);
    float sx = 192.0f / w;
    for (int x = 0; x < w; x++) {
        float col[3];
        hsv((x * sx + s_chase) * 2.2f, 1, 0.45f + 0.55f * fminf(1, s_loud * 1.6f), col);
        for (int y = 0; y < h; y++) put(rgb + (y * w + x) * 3, col);
    }
}

void fx_render(fx_t e, uint8_t *rgb, int w, int h, float dt, uint8_t avg[3])
{
    s_time += dt;
    switch (e) {
    case FX_WASH: wash(rgb, w, h, dt); break;
    case FX_BEAT: beat(rgb, w, h, dt); break;
    case FX_BASS: bass(rgb, w, h, dt); break;
    case FX_SPECTRUM: spectrum(rgb, w, h, dt); break;
    case FX_SPARKLE: sparkle(rgb, w, h, dt); break;
    case FX_CHASE: chase(rgb, w, h, dt); break;
    default: memset(rgb, 0, w * h * 3); break;
    }
    uint32_t sum[3] = {0};
    for (int i = 0; i < w * h; i++) {
        sum[0] += rgb[i * 3];
        sum[1] += rgb[i * 3 + 1];
        sum[2] += rgb[i * 3 + 2];
    }
    for (int k = 0; k < 3; k++) avg[k] = (uint8_t)(sum[k] / (w * h));
}
