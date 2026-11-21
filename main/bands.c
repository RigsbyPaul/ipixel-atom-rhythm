// FFT -> N log-spaced bands -> 0..max levels.
#include <stdbool.h>
#include "bands.h"

#include <math.h>
#include <string.h>
#include "audio.h"

#define N BANDS_FFT_SIZE
#define BIN_HZ ((float)AUDIO_SAMPLE_RATE / N)  // 31.25 Hz
#define LOW_HZ 60.0f
#define HIGH_HZ 8000.0f

// AGC tuning.
#define PEAK_FALL_DB 0.04f    // per block (~1.2 dB/s): how fast the top comes down after loud parts
#define FLOOR_RISE_DB 0.1f    // per block (~3 dB/s): the noise floor creeps up, drops at once
#define SILENCE_DB -100.0f    // below this the codec is still starting up (digital zero)
#define MIN_SPAN_DB 18.0f     // quiet rooms don't get stretched to full bars
#define MAX_SPAN_DB 40.0f
#define TOTAL_TILT_DB 16.5f   // treble lift across all bars (1.5 dB/bar with 11 bars)
#define FALLS_PER_FULL 15     // blocks for a full bar to drop to zero (~0.5 s)

static float s_win[N];
static float s_re[N], s_im[N];
static float s_pow[N / 2 + 1];
static bool s_setup;

static void setup(void)
{
    for (int i = 0; i < N; i++) {
        s_win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (N - 1));
    }
    s_setup = true;
}

void bands_init(bands_t *b, int count, int max_level)
{
    if (count > BANDS_MAX) count = BANDS_MAX;
    if (count < 1) count = 1;
    memset(b, 0, sizeof *b);
    b->count = count;
    b->max_level = max_level;
    b->tilt = count > 1 ? TOTAL_TILT_DB / (count - 1) : 0;
    b->peak = -60;
    b->floor = -150;
    // Log-spaced edges; every bar gets at least one bin of its own where possible,
    // so with many bars the lowest ones spread a little higher than LOW_HZ.
    int prev_hi = 0;
    for (int i = 0; i < count; i++) {
        float e0 = LOW_HZ * powf(HIGH_HZ / LOW_HZ, (float)i / count);
        float e1 = LOW_HZ * powf(HIGH_HZ / LOW_HZ, (float)(i + 1) / count);
        int lo = (int)lroundf(e0 / BIN_HZ);
        int hi = (int)lroundf(e1 / BIN_HZ);
        if (lo < prev_hi) lo = prev_hi;
        if (lo > N / 2 - 1) lo = N / 2 - 1;
        if (hi <= lo) hi = lo + 1;
        if (hi > N / 2) hi = N / 2;
        b->lo[i] = lo;
        b->hi[i] = hi;
        prev_hi = hi;
    }
}

// In-place iterative radix-2 FFT.
static void fft(float *re, float *im)
{
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        float ang = -2 * (float)M_PI / len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < N; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = a + len / 2;
                float xr = re[b] * cr - im[b] * ci;
                float xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
                float t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

bool bands_fft(const int32_t *samples)
{
    if (!s_setup) setup();
    // Window; remove DC so the lowest band isn't biased.
    float mean = 0;
    for (int i = 0; i < N; i++) mean += (float)(samples[i] >> 8);
    mean /= N;
    float total = 0;
    for (int i = 0; i < N; i++) {
        s_re[i] = ((float)(samples[i] >> 8) - mean) / 8388608.0f * s_win[i];
        s_im[i] = 0;
    }
    fft(s_re, s_im);
    for (int k = 0; k <= N / 2; k++) {
        s_pow[k] = s_re[k] * s_re[k] + s_im[k] * s_im[k];
        total += s_pow[k];
    }
    return 10 * log10f(total + 1e-15f) < SILENCE_DB;
}

void bands_levels(bands_t *b, bool silent, uint8_t *out)
{
    if (silent) {
        memset(b->levels, 0, b->count);
        memcpy(out, b->levels, b->count);
        return;
    }
    float db[BANDS_MAX];
    float loudest = -150, quietest = 0;
    for (int i = 0; i < b->count; i++) {
        float p = 1e-15f;
        for (int k = b->lo[i]; k < b->hi[i]; k++) p += s_pow[k];
        db[i] = 10 * log10f(p) + i * b->tilt;
        if (db[i] > loudest) loudest = db[i];
        if (i == 0 || db[i] < quietest) quietest = db[i];
    }
    if (b->floor < SILENCE_DB) b->floor = quietest;  // first real block

    // AGC: track the top (fast up, slow down) and the noise floor (slow up, fast down).
    b->peak = loudest > b->peak ? loudest : b->peak - PEAK_FALL_DB;
    b->floor = quietest < b->floor ? quietest : b->floor + FLOOR_RISE_DB;
    if (b->peak < b->floor + MIN_SPAN_DB) b->peak = b->floor + MIN_SPAN_DB;
    float lo = b->peak - MAX_SPAN_DB;
    if (lo < b->floor + 3) lo = b->floor + 3;
    float span = b->peak - lo;

    int fall = (b->max_level + FALLS_PER_FULL - 1) / FALLS_PER_FULL;
    for (int i = 0; i < b->count; i++) {
        int target = (int)lroundf((db[i] - lo) / span * b->max_level);
        if (target < 0) target = 0;
        if (target > b->max_level) target = b->max_level;
        int cur = b->levels[i];
        if (target >= cur) cur = target;
        else cur = cur - fall > target ? cur - fall : target;
        b->levels[i] = (uint8_t)cur;
    }
    memcpy(out, b->levels, b->count);
}
