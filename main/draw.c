// Live drawing on the iPixel panel (ipixel-ble build_diy_mode / build_diy_pixels):
//   enter/leave: 05 00 04 01 <mode>   (1 = enter on cleared screen, 0 = leave, discard)
//   pixels:      <len lo> <len hi> 05 01 00 R G B x0 y0 x1 y1 ...   (we send <= 60 points)
// x from the left, y from the top.
#include "draw.h"

#include <string.h>
#include "bands.h"
#include "ipixel.h"

// Every self-draw command is written with response (the panel confirms each one),
// like ipixel-ble: a 192 x 32 panel dropped most fire-and-forget pixel writes.
#define MAX_POINTS 60          // per pixel command: 128-byte writes (ipixel-ble allows 200)
#define DRAW_FRAME_POINTS 240  // per frame, whatever the panel size
#define UNKNOWN 0xFF           // s_shown value: we don't know what this bar shows

enum { C_BLACK, C_GREEN, C_YELLOW, C_RED, C_COUNT };
static const uint8_t RGB[C_COUNT][3] = {
    {0, 0, 0}, {0, 200, 0}, {230, 160, 0}, {230, 0, 0},
};

static uint8_t s_shown[BANDS_MAX];  // bar heights currently on the panel

typedef struct {
    uint8_t buf[8 + 2 * MAX_POINTS];
    int n;
} batch_t;
static batch_t s_batch[C_COUNT];

static int mode(uint8_t m)
{
    uint8_t c[5] = {5, 0, 4, 1, m};
    return ipixel_write_confirmed(c, sizeof c);
}

int draw_enter(void)
{
    memset(s_shown, 0, sizeof s_shown);
    return mode(1);
}

int draw_leave(void) { return mode(0); }

static int flush(int colour)
{
    batch_t *b = &s_batch[colour];
    if (b->n == 0) return 0;
    int len = 8 + 2 * b->n;
    b->buf[0] = len & 0xFF;
    b->buf[1] = len >> 8;
    b->buf[2] = 5;
    b->buf[3] = 1;
    b->buf[4] = 0;
    memcpy(b->buf + 5, RGB[colour], 3);
    b->n = 0;
    return ipixel_write_confirmed(b->buf, len);
}

static int add(int colour, int x, int y)
{
    batch_t *b = &s_batch[colour];
    b->buf[8 + 2 * b->n] = (uint8_t)x;
    b->buf[9 + 2 * b->n] = (uint8_t)y;
    if (++b->n == MAX_POINTS) return flush(colour);
    return 0;
}

static int zone(int row, int height)  // row 0 = bottom
{
    if (row >= height * 85 / 100) return C_RED;
    if (row >= height * 60 / 100) return C_YELLOW;
    return C_GREEN;
}

// Queue the rows from..to-1 of bar i (2 pixels per row).
static int queue_rows(int i, int from, int to, int want, int height)
{
    int rc = 0;
    int x = i * 3;
    for (int r = from; r < to && rc == 0; r++) {
        int colour = r < want ? zone(r, height) : C_BLACK;
        int y = height - 1 - r;
        rc = add(colour, x, y);
        if (rc == 0) rc = add(colour, x + 1, y);
    }
    return rc;
}

int draw_frame(const uint8_t *heights, int count, int width, int height)
{
    static int s_next;  // bar to start from, so no bar is always left until last
    static int s_heal;  // bar whose whole column is rewritten when there is room
    static uint8_t touched[BANDS_MAX];
    int rc = 0, used = 0, ntouched = 0;
    if (count > draw_bar_count(width)) count = draw_bar_count(width);
    if (count <= 0) return 0;

    // Changed rows only (grow lights have..want-1, shrink clears want..have-1),
    // at most DRAW_FRAME_POINTS per frame. Bars that don't fit keep their old
    // s_shown value and are sent next frame, so a busy frame on a big panel
    // spreads over a few frames instead of flooding the link.
    int start = s_next % count;
    int k;
    for (k = 0; k < count && rc == 0; k++) {
        int i = (start + k) % count;
        int want = heights[i] > height ? height : heights[i];
        if (want < 1) want = 1;  // silence shows as a 1-pixel line along the bottom
        int have = s_shown[i];
        int from, to;
        if (have == UNKNOWN) {
            from = 0;
            to = height;
        } else {
            from = want > have ? have : want;
            to = want > have ? want : have;
        }
        int need = 2 * (to - from);
        if (need == 0) continue;
        if (used > 0 && used + need > DRAW_FRAME_POINTS) break;
        rc = queue_rows(i, from, to, want, height);
        used += need;
        s_shown[i] = (uint8_t)want;
        touched[ntouched++] = (uint8_t)i;
    }
    s_next = start + k;

    // Writes without response can be dropped silently: when there is room,
    // rewrite one more bar's whole column to wipe out any stale pixels.
    if (rc == 0 && used + 2 * height <= DRAW_FRAME_POINTS) {
        int i = s_heal++ % count;
        int want = heights[i] > height ? height : heights[i];
        if (want < 1) want = 1;  // silence shows as a 1-pixel line along the bottom
        rc = queue_rows(i, 0, height, want, height);
        s_shown[i] = (uint8_t)want;
        touched[ntouched++] = (uint8_t)i;
    }

    // Clear first, then paint, so a shrinking-then-growing bar never shows a gap.
    if (rc == 0) rc = flush(C_BLACK);
    for (int c = C_GREEN; c < C_COUNT && rc == 0; c++) rc = flush(c);
    if (rc != 0) {
        // Some of this frame's pixels never left: redraw those bars in full later.
        for (int c = 0; c < C_COUNT; c++) s_batch[c].n = 0;
        for (int t = 0; t < ntouched; t++) s_shown[touched[t]] = UNKNOWN;
    }
    return rc;
}
