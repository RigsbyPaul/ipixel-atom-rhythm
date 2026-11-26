// Music analysis for the disco style: loudness, bass, treble and beats from
// the 11 music levels. Fed by the audio task every block (~32 ms) and read by
// the send task between frames (plain floats: a torn read only costs a frame).
#include "fx.h"

#include <math.h>
#include <string.h>

#define BANDS 11

static float s_band[BANDS];  // 0..1, above each band's noise floor
static float s_floor[BANDS] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};  // per-band noise floor, 0..1
static float s_loud, s_bass, s_treble;
static float s_kick;         // 1 on a beat, decays
static bool s_beat;
static float s_prev[BANDS], s_flux_avg, s_since_beat = 1;
static volatile uint32_t s_beats;   // beats counted, for fx_take_beats
static float s_peak[BANDS];          // band peaks since fx_take_peaks

void fx_listen(const uint8_t levels[11], float dt)
{
    float sum = 0;
    for (int i = 0; i < BANDS; i++) {
        // Level above this band's own noise floor: room noise and mic hiss
        // keep the treble bands near full even in silence. The floor drops at
        // once and creeps up slowly (a full step takes ~30 s), so sustained
        // music still shows.
        float raw = levels[i] / 15.0f;
        if (raw < s_floor[i]) s_floor[i] = raw;
        else s_floor[i] += dt * 0.03f;
        // Rise above the floor on a fixed scale (7 levels = full), less a dead
        // zone of 2 levels for noise wobbling above its quietest moment. Not
        // stretched to the room left above the floor: hissy treble bands sit
        // near the top, and stretching made their small wobbles fill the beam.
        const float GATE = 2 / 15.0f, FULL = 7 / 15.0f;
        s_band[i] = (raw - s_floor[i] - GATE) / FULL;
        if (s_band[i] > 1) s_band[i] = 1;
        if (s_band[i] < 0) s_band[i] = 0;
        sum += s_band[i];
        if (s_band[i] > s_peak[i]) s_peak[i] = s_band[i];
    }
    s_loud = sum / BANDS;
    s_bass = (s_band[0] + s_band[1] + s_band[2]) / 3;
    s_treble = (s_band[8] + s_band[9] + s_band[10]) / 3;

    // Beat: onset in the low bands. Flux = how much bands 0..3 rose since the
    // last block; a beat is flux well above its recent average, at most ~3 a
    // second. Relative, so it works with a quiet mic or loud music alike.
    float flux = 0;
    for (int i = 0; i < 4; i++) {
        float d = s_band[i] - s_prev[i];
        if (d > 0) flux += d;
    }
    memcpy(s_prev, s_band, sizeof s_prev);
    s_since_beat += dt;
    s_beat = flux > s_flux_avg * 1.8f + 0.06f && s_since_beat > 0.3f;
    if (s_beat) {
        s_since_beat = 0;
        s_kick = 1;
        s_beats++;
    }
    s_flux_avg += (flux - s_flux_avg) * fminf(1, dt * 3);
    s_kick *= expf(-dt * 5);
}

bool fx_beat(void) { return s_beat; }

float fx_loud(void) { return s_loud; }
float fx_bass(void) { return s_bass; }
float fx_treble(void) { return s_treble; }
float fx_kick(void) { return s_kick; }

uint32_t fx_take_beats(void)
{
    static uint32_t seen;
    uint32_t now = s_beats, n = now - seen;
    seen = now;
    return n;
}

void fx_take_peaks(float out[11])
{
    for (int i = 0; i < BANDS; i++) {
        out[i] = s_peak[i];
        s_peak[i] = s_band[i];
    }
}
