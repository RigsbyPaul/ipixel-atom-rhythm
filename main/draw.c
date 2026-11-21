// Live drawing on the iPixel panel (ipixel-ble build_diy_mode / build_diy_pixels):
//   enter/leave: 05 00 04 01 <mode>   (1 = enter on cleared screen, 0 = leave, discard)
//   pixels:      <len lo> <len hi> 05 01 00 R G B x0 y0 x1 y1 ...   (<= 200 points)
// x from the left, y from the top.
#include "draw.h"

#include <string.h>
#include "bands.h"
#include "ipixel.h"

#define MAX_POINTS 200

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
    return ipixel_write(c, sizeof c);
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
    return ipixel_write(b->buf, len);
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

int draw_frame(const uint8_t *heights, int count, int width, int height)
{
    static int s_heal;  // bar whose whole column is rewritten this frame
    int rc = 0;
    if (count > draw_bar_count(width)) count = draw_bar_count(width);
    if (count <= 0) return 0;
    int heal = s_heal++ % count;
    for (int i = 0; i < count && rc == 0; i++) {
        int want = heights[i] > height ? height : heights[i];
        int have = s_shown[i];
        int x = i * 3;
        // Normally only the change: grow lights rows have..want-1, shrink clears
        // rows want..have-1. Writes without response can be dropped silently, so
        // one bar per frame is rewritten top to bottom to wipe out stale pixels.
        int from, to;
        if (i == heal) {
            from = 0;
            to = height;
        } else {
            from = want > have ? have : want;
            to = want > have ? want : have;
        }
        for (int r = from; r < to && rc == 0; r++) {
            int colour = r < want ? zone(r, height) : C_BLACK;
            int y = height - 1 - r;
            rc = add(colour, x, y);
            if (rc == 0) rc = add(colour, x + 1, y);
        }
        s_shown[i] = (uint8_t)want;
    }
    // Clear first, then paint, so a shrinking-then-growing bar never shows a gap.
    if (rc == 0) rc = flush(C_BLACK);
    for (int c = C_GREEN; c < C_COUNT && rc == 0; c++) rc = flush(c);
    if (rc != 0) {
        for (int c = 0; c < C_COUNT; c++) s_batch[c].n = 0;
    }
    return rc;
}
